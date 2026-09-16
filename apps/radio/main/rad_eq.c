/*
 * The equaliser: the same filters the synth ran, in the same order.
 *
 * On the Pi these were one SuperCollider synth, set live over OSC, and the
 * EQ page drew a curve by evaluating the same transfer functions in Python
 * so the picture matched the sound. Here there is no synth, so this file is
 * both halves at once - the cascade that the audio actually goes through, and
 * the magnitude response the page draws - which removes the one way those two
 * could ever have disagreed.
 *
 * The coefficients are the RBJ cookbook's, unchanged, and the cascade is:
 *
 *     high-pass (four-pole, as two Butterworth sections)
 *     low shelf
 *     three peaking bands
 *     high shelf
 *
 * Designed at NEOS_AUDIO_RATE and not at the station's rate, because this
 * runs after the resampler. That is also why the drawn curve is honest for
 * every station rather than for the one the synth's fs happened to match.
 *
 * Transposed direct form II, and in float. The chip has a single-precision
 * FPU; seven biquads on one channel at 48 kHz is under two million flops a
 * second, which is nothing next to the decoder - and float state cannot
 * develop the limit cycles a 16-bit fixed-point biquad does at low
 * frequencies, which is exactly where a 120 Hz high-pass lives.
 */

#include "radio.h"
#include "rad_math.h"

#include <string.h>

#define FS  ((float)NEOS_AUDIO_RATE)

/*
 * The synth's BHiPass4 square-roots the rq it is handed and gives both of its
 * sections the result, so an rq of 2 puts each section at 0.707 - a
 * Butterworth pair. The same number is used here, or the filter would not be
 * the filter the Pi was running.
 */
#define HPF_SECTION_Q  0.70710678f

/* ------------------------------------------------------------------ */
/* Design                                                              */
/* ------------------------------------------------------------------ */

static void set(rad_biquad_t *q, float b0, float b1, float b2,
                float a0, float a1, float a2)
{
    /* a0 is divided out here rather than at every sample. It is never zero
       for any of the forms below within the ranges the sliders allow. */
    const float inv = 1.0f / a0;
    q->b0 = b0 * inv;
    q->b1 = b1 * inv;
    q->b2 = b2 * inv;
    q->a1 = a1 * inv;
    q->a2 = a2 * inv;
}

static void design_highpass(rad_biquad_t *q, float hz, float qf)
{
    const float w0    = RAD_TAU * hz / FS;
    const float cw    = rad_cosf(w0);
    const float sw    = rad_sinf(w0);
    const float alpha = sw / (2.0f * (qf < 0.05f ? 0.05f : qf));

    set(q, (1.0f + cw) * 0.5f, -(1.0f + cw), (1.0f + cw) * 0.5f,
           1.0f + alpha, -2.0f * cw, 1.0f - alpha);
}

static void design_peaking(rad_biquad_t *q, float hz, float gain_db, float qf)
{
    const float a     = rad_pow10f(gain_db / 40.0f);
    const float w0    = RAD_TAU * hz / FS;
    const float cw    = rad_cosf(w0);
    const float sw    = rad_sinf(w0);
    const float alpha = sw / (2.0f * (qf < 0.05f ? 0.05f : qf));

    set(q, 1.0f + alpha * a, -2.0f * cw, 1.0f - alpha * a,
           1.0f + alpha / a, -2.0f * cw, 1.0f - alpha / a);
}

static void design_shelf(rad_biquad_t *q, float hz, float gain_db, float slope,
                         bool high)
{
    const float a  = rad_pow10f(gain_db / 40.0f);
    const float w0 = RAD_TAU * hz / FS;
    const float cw = rad_cosf(w0);
    const float sw = rad_sinf(w0);

    if (slope < 0.05f) {
        slope = 0.05f;
    }
    const float alpha = sw * 0.5f * rad_sqrtf((a + 1.0f / a) * (1.0f / slope - 1.0f) + 2.0f);
    const float two_root = 2.0f * rad_sqrtf(a) * alpha;

    if (high) {
        set(q, a * ((a + 1.0f) + (a - 1.0f) * cw + two_root),
               -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw),
               a * ((a + 1.0f) + (a - 1.0f) * cw - two_root),
               (a + 1.0f) - (a - 1.0f) * cw + two_root,
               2.0f * ((a - 1.0f) - (a + 1.0f) * cw),
               (a + 1.0f) - (a - 1.0f) * cw - two_root);
    } else {
        set(q, a * ((a + 1.0f) - (a - 1.0f) * cw + two_root),
               2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw),
               a * ((a + 1.0f) - (a - 1.0f) * cw - two_root),
               (a + 1.0f) + (a - 1.0f) * cw + two_root,
               -2.0f * ((a - 1.0f) + (a + 1.0f) * cw),
               (a + 1.0f) + (a - 1.0f) * cw - two_root);
    }
}

void rad_eq_design(rad_eq_t *eq, const int *p)
{
    /* Scaled integers in, real numbers out - the one place in the app where
       that conversion happens, so the units below are the table's units. */
    const float hpf_hz = (float)p[P_HPF_FREQ];

    design_highpass(&eq->section[0], hpf_hz, HPF_SECTION_Q);
    design_highpass(&eq->section[1], hpf_hz, HPF_SECTION_Q);

    design_shelf(&eq->section[2], (float)p[P_LOW_FREQ],
                 (float)p[P_LOW_GAIN] * 0.1f, (float)p[P_LOW_RS] * 0.01f, false);

    design_peaking(&eq->section[3], (float)p[P_LOMID_FREQ],
                   (float)p[P_LOMID_GAIN] * 0.1f, (float)p[P_LOMID_RQ] * 0.01f);
    design_peaking(&eq->section[4], (float)p[P_MID_FREQ],
                   (float)p[P_MID_GAIN] * 0.1f, (float)p[P_MID_RQ] * 0.01f);
    design_peaking(&eq->section[5], (float)p[P_HIMID_FREQ],
                   (float)p[P_HIMID_GAIN] * 0.1f, (float)p[P_HIMID_RQ] * 0.01f);

    design_shelf(&eq->section[6], (float)p[P_HIGH_FREQ],
                 (float)p[P_HIGH_GAIN] * 0.1f, (float)p[P_HIGH_RS] * 0.01f, true);

    eq->ready = true;
}

void rad_eq_reset(rad_eq_t *eq)
{
    /* The coefficients are left alone: only the history is cleared, which is
       what wants clearing when a new station starts. Redesigning on every
       slider move deliberately does *not* come through here, because dropping
       the state mid-note is a click. */
    for (int i = 0; i < RAD_SECTIONS; i++) {
        eq->section[i].z1 = eq->section[i].z2 = 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

void rad_eq_run(rad_eq_t *eq, int16_t *pcm, int n)
{
    if (!eq->ready) {
        return;
    }

    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i];

        for (int k = 0; k < RAD_SECTIONS; k++) {
            rad_biquad_t *q = &eq->section[k];
            const float   y = q->b0 * x + q->z1;
            q->z1 = q->b1 * x - q->a1 * y + q->z2;
            q->z2 = q->b2 * x - q->a2 * y;
            x = y;
        }

        /*
         * Clipped rather than wrapped. A cascade with 15 dB of boost in it
         * will overshoot full scale on a stream that was already mastered
         * loud, and the difference between the two behaviours is the
         * difference between a bit of distortion and a burst of noise that
         * sounds like the hardware has broken.
         */
        const int32_t out = (int32_t)(x + (x < 0.0f ? -0.5f : 0.5f));
        pcm[i] = (int16_t)(out > 32767 ? 32767 : (out < -32768 ? -32768 : out));
    }
}

/* ------------------------------------------------------------------ */
/* Drawing it                                                          */
/* ------------------------------------------------------------------ */

/*
 * Cascaded filters multiply, and decibels are logarithms, so the combined
 * response is one logarithm of the product rather than a sum of seven. That
 * is not only tidier: it is six fewer logarithms per column of the curve, and
 * the curve is over a thousand columns wide.
 *
 * The squared magnitude is evaluated in sin^2(w/2) rather than by summing
 * b0 + b1.cos(w) + b2.cos(2w) and taking the modulus, and that is not a
 * micro-optimisation - it is the difference between a curve that is right and
 * one that is nearly right where it matters most. A high-pass at 120 Hz has
 * coefficients near (0.5, -1, 0.5) over (1, -2, 1), so the obvious sum is
 * three numbers near one cancelling to something near zero, and in single
 * precision the low end of the axis comes out a fifth of a decibel wrong.
 * In this form the cancellation happens once, in (b0 + b1 + b2), which for a
 * high-pass is exactly zero - the filter has no output at DC, and the formula
 * says so exactly rather than nearly.
 *
 *   |H|^2 = [ (b0+b1+b2)^2 - 4(b0.b1 + 4.b0.b2 + b1.b2).p + 16.b0.b2.p^2 ]
 *           / [ the same in a ],      p = sin^2(w/2)
 *
 * It also costs one sine per column instead of two sines and two cosines.
 */
float rad_eq_response(const rad_eq_t *eq, float hz)
{
    if (!eq->ready) {
        return 0.0f;
    }

    const float half = rad_sinf(RAD_PI * hz / FS);
    const float p    = half * half;          /* sin^2(w/2) */

    float ratio = 1.0f;

    for (int k = 0; k < RAD_SECTIONS; k++) {
        const rad_biquad_t *q = &eq->section[k];

        const float bs = q->b0 + q->b1 + q->b2;
        const float as = 1.0f  + q->a1 + q->a2;

        float num = bs * bs
                  - 4.0f * (q->b0 * q->b1 + 4.0f * q->b0 * q->b2 + q->b1 * q->b2) * p
                  + 16.0f * q->b0 * q->b2 * p * p;
        float den = as * as
                  - 4.0f * (q->a1 + 4.0f * q->a2 + q->a1 * q->a2) * p
                  + 16.0f * q->a2 * p * p;

        /* Both are squared magnitudes and so cannot really be negative; a
           rounding error at an exact zero can make one look it. */
        if (num < 1e-20f) { num = 1e-20f; }
        if (den < 1e-20f) { den = 1e-20f; }

        ratio *= num / den;
        if (ratio < 1e-20f) {
            ratio = 1e-20f;     /* deep in a high-pass stopband; the page
                                   clamps the curve long before here anyway */
        }
    }

    /* Ten, not twenty: `ratio` is a ratio of squares. */
    return 10.0f * rad_log10f(ratio);
}
