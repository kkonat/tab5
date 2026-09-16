/*
 * The voice. See syn_dsp.h for what it is and what it must not become.
 */
#include "syn_dsp.h"

/* exp2_fast(), blep(), xorshift(), wrap1() - shared with apps/moog so that
   there is one hand-rolled exponential in this repository and not two. */
#include "fastmath.h"

/*
 * How fast a knob's new value is reached, as a fraction of the error per
 * sample.
 *
 * Expressed as the reciprocal of a time constant in samples so that the
 * numbers read as milliseconds: at 48 kHz, 1/720 is 15 ms and 1/240 is 5 ms.
 * Pitch gets the longer one because a fast pitch chase is a portamento nobody
 * asked for at one end and a click at the other; level gets the shorter one
 * because a slow gain chase feels like the knob is not connected.
 */
#define GLIDE_FREQ  (1.0f / 720.0f)
#define GLIDE_AMP   (1.0f / 240.0f)
#define GLIDE_LFO   (1.0f / 480.0f)

/* Full deflection of the amount knob is an octave either way. An octave is
   plenty of vibrato at 6 Hz and plenty of index at 600. */
#define MOD_OCTAVES 1.0f

/* Below this the codec is asked for silence outright rather than for a very
   quiet oscillator, which is the same thing minus a residual the amplifier
   would still be powered for. */
#define AMP_FLOOR 0.0005f

static const char *const SHAPE_NAME[SYN_SHAPES] = { "SAW", "TRI", "SQR", "S&H" };

const char *syn_shape_name(int shape)
{
    return (shape >= 0 && shape < SYN_SHAPES) ? SHAPE_NAME[shape] : "?";
}

/* ------------------------------------------------------------------ */

void syn_voice_init(syn_voice_t *v, float sample_rate)
{
    v->sr      = sample_rate;
    v->inv_sr  = 1.0f / sample_rate;
    v->phase   = 0.0f;
    v->lphase  = 0.0f;
    v->sh      = 0.0f;
    v->rng     = 0x1234567u;
    v->freq    = 0.0f;
    v->amp     = 0.0f;
    v->lrate   = 0.0f;
    v->lamt    = 0.0f;
    v->primed  = false;
    v->lfo_out = 0.0f;
}

void syn_voice_render(syn_voice_t *v, const syn_patch_t *p, int16_t *out, int n)
{
    /*
     * The patch is read once, here, and the block is rendered against that
     * copy. A knob moved halfway through a block therefore takes effect at the
     * next one, five milliseconds later, which is below anything a finger can
     * notice and removes every question about what happens if a field changes
     * between two samples of the same loop.
     */
    if (!v->primed) {
        v->freq   = p->freq;
        v->amp    = p->amp;
        v->lrate  = p->lfo_rate;
        v->lamt   = p->lfo_amt;
        v->primed = true;
    }

    const float t_freq  = p->freq;
    const float t_amp   = p->amp;
    const float t_lrate = p->lfo_rate;
    const float t_lamt  = p->lfo_amt;
    const int   shape   = p->lfo_shape;

    float phase   = v->phase;
    float lphase  = v->lphase;
    float sh      = v->sh;
    uint32_t rng  = v->rng;
    float freq    = v->freq;
    float amp     = v->amp;
    float lrate   = v->lrate;
    float lamt    = v->lamt;
    float lfo     = v->lfo_out;

    const float inv_sr = v->inv_sr;
    const float nyq    = v->sr * 0.5f;

    for (int i = 0; i < n; i++) {
        freq  += (t_freq  - freq)  * GLIDE_FREQ;
        amp   += (t_amp   - amp)   * GLIDE_AMP;
        lrate += (t_lrate - lrate) * GLIDE_LFO;
        lamt  += (t_lamt  - lamt)  * GLIDE_LFO;

        /* --- the LFO ------------------------------------------------ */
        const float ldt = lrate * inv_sr;
        lphase += ldt;
        bool wrapped = false;
        while (lphase >= 1.0f) {
            lphase -= 1.0f;
            wrapped = true;
        }

        switch (shape) {
        case SYN_TRI: {
            const float t = lphase + lphase - 1.0f;      /* -1..1 */
            lfo = 1.0f - 2.0f * (t < 0.0f ? -t : t);     /* triangle, -1..1 */
            break;
        }
        case SYN_SQR:
            lfo = (lphase < 0.5f) ? 1.0f : -1.0f;
            /*
             * Corrected at both edges, because at the top of the hi range this
             * is a 2 kHz square being summed into pitch: uncorrected, its own
             * aliases modulate the carrier and the result is a rattle that
             * sounds like a bug in the oscillator rather than in the LFO.
             */
            lfo += blep(lphase, ldt);
            lfo -= blep(wrap1(lphase + 0.5f), ldt);
            break;
        case SYN_SH:
            if (wrapped) {
                rng = xorshift(rng);
                sh  = rand_bipolar(rng);
            }
            lfo = sh;
            break;
        case SYN_SAW:
        default:
            lfo = lphase + lphase - 1.0f;
            lfo -= blep(lphase, ldt);
            break;
        }

        /* --- the oscillator ----------------------------------------- */

        /*
         * Exponential, so the knob and the modulation both move in octaves and
         * the vibrato is the same musical interval wherever the pitch is set.
         * Linear FM would detune rather than vibrate, and would take the pitch
         * through zero at the bottom of the amount knob's travel.
         */
        float f = freq * exp2_fast(lfo * lamt * MOD_OCTAVES);
        if (f > nyq)   { f = nyq; }
        if (f < 0.01f) { f = 0.01f; }

        const float dt = f * inv_sr;
        phase += dt;
        while (phase >= 1.0f) {
            phase -= 1.0f;
        }

        float s = phase + phase - 1.0f;
        s -= blep(phase, dt);

        float y = (amp < AMP_FLOOR) ? 0.0f : s * amp;

        /* 32000 and not 32767: the BLEP correction overshoots a little either
           side of the step, and clipping that would put a flat spot on the one
           part of the waveform it was added to smooth. */
        int32_t q = (int32_t)(y * 32000.0f);
        if (q >  32767) { q =  32767; }
        if (q < -32768) { q = -32768; }
        out[i] = (int16_t)q;
    }

    v->phase   = phase;
    v->lphase  = lphase;
    v->sh      = sh;
    v->rng     = rng;
    v->freq    = freq;
    v->amp     = amp;
    v->lrate   = lrate;
    v->lamt    = lamt;
    v->lfo_out = lfo;
}
