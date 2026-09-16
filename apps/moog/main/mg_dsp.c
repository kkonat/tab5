/*
 * The instrument. See mg_dsp.h for the shape of it.
 */
#include <math.h>

#include "mg_dsp.h"
#include "fastmath.h"

/* ------------------------------------------------------------------ */
/* Constants of the instrument                                         */
/* ------------------------------------------------------------------ */

/*
 * What each RANGE position is, in octaves from 8'.
 *
 * 8' is the reference - a key plays the note it is named after - and each step
 * either side is an octave. LO is the odd one: on the Model D it takes the
 * oscillator below 32' and out of the audio band altogether, which is what
 * makes oscillator 3 usable as the modulation source. Four octaves below 8'
 * puts a middle C at about eight hertz, which is a vibrato rather than a note.
 */
static const int8_t RANGE_OCT[RANGE_N] = { -4, -2, -1, 0, 1, 2 };

static const char *const RANGE_NAME[RANGE_N] = { "LO", "32'", "16'", "8'", "4'", "2'" };
static const char *const WAVE_NAME[WAVE_N]   = { "TRI", "T/S", "SAW", "SQR", "WIDE", "NARR" };

const char *mg_range_name(int r) { return (r >= 0 && r < RANGE_N) ? RANGE_NAME[r] : "?"; }
const char *mg_wave_name(int w)  { return (w >= 0 && w < WAVE_N)  ? WAVE_NAME[w]  : "?"; }

/* The duty cycles of the three rectangular waves. The Model D's are not
   documented as exact figures; these are the usual reconstruction and are what
   make WIDE and NARROW sound like two different waves rather than two squares. */
#define DUTY_SQUARE 0.50f
#define DUTY_WIDE   0.26f
#define DUTY_NARROW 0.13f

/*
 * The fixed modulation depths, which on the real instrument are the wheel.
 *
 * A semitone and a half of pitch is a strong vibrato at oscillator-3 rates and
 * an obvious burble at noise; two octaves of cutoff is a filter sweep you
 * cannot miss. Both are deliberately on the generous side, because a routing
 * switch that you cannot hear being switched is a routing switch nobody will
 * believe works. See the note in mg_dsp.h about the missing wheel.
 */
#define MOD_OSC_SEMIS 1.5f
#define MOD_FILT_OCT  2.0f

/*
 * How hard the feedback return drives the mixer at INPUT VOLUME 10.
 *
 * It has to be able to take the loop gain past one, or the control cannot do
 * the thing it exists for. Patching a Minimoog's output back into its external
 * input is not a subtle effect - it is the howl - and a return that can only
 * ever tint the sound is a return that reads as broken.
 *
 * Nothing downstream needs protecting from it. The ladder's own soft clip
 * bounds the loop wherever the gain lands, which is exactly how the hardware
 * stays musical when it is screaming rather than merely loud.
 */
#define FB_GAIN 3.0f

/*
 * The coupling capacitor on the external input, as a one-pole at about 4 Hz.
 *
 * This is the part that is not optional, and the reason is worth stating
 * because the symptom looks nothing like the cause. A ladder passes DC. Feed
 * its output back into its own input with a loop gain over one and the DC
 * component grows until the saturator pins - and then stays pinned, because
 * the thing holding it there is its own output. What that sounds like is not
 * a howl, it is silence, with the overload lamp lit and the filter jammed
 * against the rail until the key is released. The hardware cannot do this
 * because its input is AC-coupled, and neither can this.
 */
#define FB_HP_R 0.9995f

/* The cutoff knob's -4..+4 as hertz: nine and a bit octaves from 20 Hz, which
   lands the top of the travel just above where the ear stops. */
#define CUT_BASE 20.0f
#define CUT_OCT  1.2f      /* octaves per knob unit */

/* How far the filter contour can push the cutoff at full AMOUNT OF CONTOUR. */
#define CONTOUR_OCT 4.0f

/* An envelope's attack aims past its target so that the approach is the
   exponential curve the hardware has rather than a curve that never arrives. */
#define ATTACK_OVERSHOOT 1.3f

/* Below this the codec is asked for silence outright rather than for a very
   quiet instrument, which is the same thing minus a residual the amplifier
   would still be powered for. */
#define GATE_FLOOR 0.0002f

/* How fast a knob's new value is reached, as a fraction of the error per
   sample. At 48 kHz, 1/240 is five milliseconds - short enough not to feel
   like lag, long enough that the step is inaudible. */
#define SMOOTH (1.0f / 240.0f)

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

/* ------------------------------------------------------------------ */
/* The pieces                                                          */
/* ------------------------------------------------------------------ */

/*
 * One oscillator's waveform at phase @p ph, with @p dt of phase per sample.
 *
 * The triangle is generated naively and the others are not. That is a
 * judgement rather than an oversight: a triangle's harmonics fall away as the
 * square of their number, so what folds back is already 40 dB down two octaves
 * up, where a sawtooth's fold-back is only 6 dB down and is the first thing
 * anybody notices. The three rectangles get a correction at each of their two
 * edges, and the sawtooth at its one.
 */
static inline float shape(int wave, float ph, float dt)
{
    switch (wave) {
    case WAVE_TRI:
        return (ph < 0.5f) ? (4.0f * ph - 1.0f) : (3.0f - 4.0f * ph);

    case WAVE_TRISAW: {
        /* The Model D's second position is between the two, and reads on a
           scope as a ramp with its corner knocked off. */
        const float tri = (ph < 0.5f) ? (4.0f * ph - 1.0f) : (3.0f - 4.0f * ph);
        const float saw = ph + ph - 1.0f - blep(ph, dt);
        return 0.45f * tri + 0.55f * saw;
    }
    case WAVE_SAW:
        return ph + ph - 1.0f - blep(ph, dt);

    default: {
        const float duty = (wave == WAVE_SQUARE) ? DUTY_SQUARE
                         : (wave == WAVE_WIDE)   ? DUTY_WIDE
                                                 : DUTY_NARROW;
        float p = (ph < duty) ? 1.0f : -1.0f;
        p += blep(ph, dt);
        /* The falling edge, shifted so that it too sits at phase zero, which
           is the only place blep() knows how to look. */
        p -= blep(wrap1(ph + 1.0f - duty), dt);

        /*
         * A rectangle that is not a square carries a DC offset of 2d-1, and DC
         * into a resonant ladder is a step in the cutoff every time the
         * waveform knob is turned. It is taken out here, and the result scaled
         * back to a peak of one so that WIDE and NARROW arrive at the mixer at
         * the level SQUARE does rather than 74 per cent louder.
         */
        p -= duty + duty - 1.0f;
        return p * (0.5f / (1.0f - duty));
    }
    }
}

/*
 * A bounded soft clip: two multiplies, a divide, and a hard corner at three.
 *
 * This is the Pade approximation to tanh, which it matches to better than a
 * per cent over the range it is used in and which - unlike tanh - is a few
 * instructions rather than a call through the syscall table.
 *
 * It replaced the cubic `y - y*y*y/6` that the textbook ladder carries, and
 * the reason is worth recording because the cubic looks perfectly reasonable.
 * It is a fine saturator for small signals and it is not a saturator at all
 * for large ones: its derivative goes negative past 1.41, it crosses zero at
 * 2.45, and beyond that it runs away. With one oscillator at moderate level
 * that never comes up. With three at full, noise on top, the output fed back
 * into the mixer and EMPHASIS at the stop - all of which this panel can ask
 * for at once - the ladder is driven to five and the cubic diverges instead
 * of clipping. That is a self-oscillating filter turning into full-scale
 * noise, which is the loudest possible way to be wrong.
 */
static inline float sat(float x)
{
    const float t = clampf(x, -3.0f, 3.0f);
    return t * (27.0f + t * t) / (27.0f + 9.0f * t * t);
}

/*
 * The four-pole ladder, in Stilson and Smith's form.
 *
 * Four one-pole sections with the output fed back to the input through the
 * resonance control, which is the topology Moog patented and is why this
 * filter sounds like this filter: the feedback both peaks the corner and,
 * because every section is in the loop, takes 24 dB an octave off everything
 * above it.
 *
 * The cubic on the way out is the part that is not linear theory, and it is
 * doing two jobs. It is the transistor ladder's own saturation, which is what
 * keeps a real Minimoog musical when it is driven hard instead of merely loud.
 * And it is what makes self-oscillation stable: with EMPHASIS at the top the
 * loop gain is over one and a linear filter would grow without bound, where
 * this settles into the sine the hardware settles into.
 */
static inline float ladder(mg_voice_t *v, float in, float fc, float res)
{
    float f = 2.0f * fc * v->inv_sr;
    if (f > 0.98f) { f = 0.98f; }
    if (f < 0.0005f) { f = 0.0005f; }

    const float p = f * (1.8f - 0.8f * f);
    const float k = p + p - 1.0f;

    const float t  = (1.0f - p) * 1.386249f;
    const float t2 = 12.0f + t * t;
    const float r  = res * (t2 + 6.0f * t) / (t2 - 6.0f * t);

    const float x = in - r * v->y4;

    v->y1  = x     * p + v->ox  * p - k * v->y1;  v->ox  = x;
    v->y2  = v->y1 * p + v->oy1 * p - k * v->y2;  v->oy1 = v->y1;
    v->y3  = v->y2 * p + v->oy2 * p - k * v->y3;  v->oy2 = v->y2;
    v->y4  = v->y3 * p + v->oy3 * p - k * v->y4;  v->oy3 = v->y3;

    /*
     * Scaled either side of the soft clip so the knee sits at about two rather
     * than at one: below unity this is within a few per cent of a straight
     * wire, which is where the instrument spends its life, and it still cannot
     * leave +/-2 however hard the mixer drives it.
     */
    v->y4 = 2.0f * sat(v->y4 * 0.5f);

    /*
     * A bound the arithmetic above is no longer supposed to need, and which is
     * here anyway: this is a feedback loop with a knob on its gain, and if a
     * coefficient ever goes somewhere it should not, the failure mode without
     * this is a NaN that propagates through every sample after it. Four is
     * twice what sat() allows, so this firing at all means something else has
     * gone wrong.
     */
    v->y4 = clampf(v->y4, -4.0f, 4.0f);
    return v->y4;
}

/* The per-sample coefficient for a one-pole that covers its span in `secs`.
   Worked out per block rather than per sample: it is one expf() against a knob
   that cannot move more than once every five milliseconds anyway. */
static float rate_of(float secs, float sr)
{
    if (secs < 0.0005f) {
        return 1.0f;                    /* instant, and no division by nearly nothing */
    }
    const float k = 1.0f - expf(-1.0f / (secs * sr));
    return clampf(k, 0.0f, 1.0f);
}

static inline float env_step(float level, uint8_t *stage,
                             float ka, float kd, float kr, float sus)
{
    switch (*stage) {
    case ENV_ATTACK:
        level += (ATTACK_OVERSHOOT - level) * ka;
        if (level >= 1.0f) { level = 1.0f; *stage = ENV_DECAY; }
        break;
    case ENV_DECAY:
        level += (sus - level) * kd;
        if (level - sus < 0.001f && sus - level < 0.001f) {
            level = sus;
            *stage = ENV_SUSTAIN;
        }
        break;
    case ENV_SUSTAIN:
        /* Tracks the knob, so turning SUSTAIN while a key is held does what
           the hardware does rather than waiting for the next note. */
        level += (sus - level) * kd;
        break;
    case ENV_RELEASE:
        level -= level * kr;
        if (level < GATE_FLOOR) { level = 0.0f; *stage = ENV_IDLE; }
        break;
    default:
        level = 0.0f;
        break;
    }
    return level;
}

/* ------------------------------------------------------------------ */

void mg_voice_init(mg_voice_t *v, float sample_rate)
{
    for (int i = 0; i < 3; i++) {
        v->phase[i] = 0.0f;
        v->s_vol[i] = 0.0f;
    }
    v->sr      = sample_rate;
    v->inv_sr  = 1.0f / sample_rate;
    v->rng     = 0x2468ace1u;
    v->pink0 = v->pink1 = v->pink2 = 0.0f;
    v->pitch = v->pitch_target = 60.0f;
    v->primed = false;
    v->was_gated = false;
    v->y1 = v->y2 = v->y3 = v->y4 = 0.0f;
    v->ox = v->oy1 = v->oy2 = v->oy3 = 0.0f;
    v->fenv = v->lenv = 0.0f;
    v->fstage = v->lstage = ENV_IDLE;
    v->fb = v->fb_x1 = 0.0f;
    v->s_cut = v->s_res = v->s_master = 0.0f;
    v->s_noise = v->s_fbv = v->s_mix = 0.0f;
    v->mod_out = 0.0f;
    v->peak = 0.0f;
    v->overload = false;
}

void mg_voice_render(mg_voice_t *v, const mg_patch_t *p, const mg_perf_t *perf,
                     int16_t *out, int n)
{
    /* --- once per block ------------------------------------------- */

    if (!v->primed) {
        v->s_cut    = p->cutoff;
        v->s_res    = p->emphasis;
        v->s_master = p->master;
        v->s_noise  = p->noise_vol;
        v->s_fbv    = p->fb_vol;
        v->s_mix    = p->mod_mix;
        for (int i = 0; i < 3; i++) { v->s_vol[i] = p->vol[i]; }
        v->pitch    = perf->note;
        v->primed   = true;
    }

    /*
     * The gate's rising edge, and only its rising edge.
     *
     * This is the Model D's single trigger and it is the whole feel of the
     * instrument: play legato - press the next key before releasing the last -
     * and the contours are not restarted, so the note slides under a sound
     * that is already sustaining. Retriggering on every key would be a
     * different instrument, and a more ordinary one.
     */
    if (perf->gate && !v->was_gated) {
        v->fstage = ENV_ATTACK;
        v->lstage = ENV_ATTACK;
    } else if (!perf->gate && v->was_gated) {
        v->fstage = ENV_RELEASE;
        v->lstage = ENV_RELEASE;
    }
    v->was_gated = perf->gate;
    v->pitch_target = perf->note;

    const float fka = rate_of(p->fa, v->sr);
    const float fkd = rate_of(p->fd, v->sr);
    const float lka = rate_of(p->la, v->sr);
    const float lkd = rate_of(p->ld, v->sr);

    /*
     * Release is the decay time, which is the Model D with its decay switch
     * on - there is no separate release control on the panel and never was.
     */
    const float fkr = fkd;
    const float lkr = lkd;

    /*
     * Glide is a time to cross an octave, so the coefficient is that time
     * spread over twelve semitones. Expressed that way rather than as a fixed
     * rate because a portamento that takes as long to move a semitone as an
     * octave is not what the knob is labelled.
     */
    const float glide_k = (p->glide < 0.001f) ? 1.0f
                        : rate_of(p->glide * (1.0f / 12.0f), v->sr);

    /*
     * Each oscillator's frequency at the reference note, so the per-sample work
     * is one exponential for the bend and three multiplies rather than three
     * exponentials.
     *
     * The reference note is 60 - middle C - because that is what the per-sample
     * bend below is measured from, and the two have to agree or every note is
     * out by the difference. The 9 is the interval from middle C up to A440:
     * note 69 minus note 60. Getting that wrong by writing 69 here, as though
     * `semis` were a note number rather than an offset from one, tunes the
     * whole instrument five and three quarter octaves flat - which sounds
     * exactly like a broken oscillator and is nothing of the sort.
     */
    float base[3];
    for (int i = 0; i < 3; i++) {
        const float semis = p->tune + p->detune[i] +
                            12.0f * (float)RANGE_OCT[p->range[i] % RANGE_N];
        base[i] = 440.0f * exp2_fast((semis - 9.0f) * (1.0f / 12.0f));
    }

    const float kbd_amt = (p->kbd_track == KBD_THIRD)    ? (1.0f / 3.0f)
                        : (p->kbd_track == KBD_TWOTHIRD) ? (2.0f / 3.0f)
                        : (p->kbd_track == KBD_FULL)     ? 1.0f : 0.0f;

    const float t_cut    = p->cutoff;
    const float t_res    = p->emphasis;
    const float t_master = p->master * p->master;   /* a fader wants a square */
    const float t_noise  = p->noise_on ? p->noise_vol : 0.0f;
    const float t_fbv    = (p->fb_on && p->fb_mode) ? p->fb_vol * FB_GAIN : 0.0f;
    const float t_mix    = p->mod_mix;
    float t_vol[3];
    for (int i = 0; i < 3; i++) { t_vol[i] = p->on[i] ? p->vol[i] : 0.0f; }

    const int w0 = p->wave[0] % WAVE_N, w1 = p->wave[1] % WAVE_N, w2 = p->wave[2] % WAVE_N;
    const float nyq = v->sr * 0.5f;

    /* --- locals, so the loop is not walking a struct ---------------- */

    float ph0 = v->phase[0], ph1 = v->phase[1], ph2 = v->phase[2];
    float pitch = v->pitch;
    uint32_t rng = v->rng;
    float pk0 = v->pink0, pk1 = v->pink1, pk2 = v->pink2;
    float fenv = v->fenv, lenv = v->lenv;
    float fb = v->fb, fb_x1 = v->fb_x1;
    float peak = 0.0f;
    float modv = 0.0f;

    for (int i = 0; i < n; i++) {
        /* --- the knobs, chased ---------------------------------- */
        v->s_cut    += (t_cut    - v->s_cut)    * SMOOTH;
        v->s_res    += (t_res    - v->s_res)    * SMOOTH;
        v->s_master += (t_master - v->s_master) * SMOOTH;
        v->s_noise  += (t_noise  - v->s_noise)  * SMOOTH;
        v->s_fbv    += (t_fbv    - v->s_fbv)    * SMOOTH;
        v->s_mix    += (t_mix    - v->s_mix)    * SMOOTH;
        v->s_vol[0] += (t_vol[0] - v->s_vol[0]) * SMOOTH;
        v->s_vol[1] += (t_vol[1] - v->s_vol[1]) * SMOOTH;
        v->s_vol[2] += (t_vol[2] - v->s_vol[2]) * SMOOTH;

        pitch += (v->pitch_target - pitch) * glide_k;

        /* --- the contours --------------------------------------- */
        fenv = env_step(fenv, &v->fstage, fka, fkd, fkr, p->fs);
        lenv = env_step(lenv, &v->lstage, lka, lkd, lkr, p->ls);

        /* --- noise ---------------------------------------------- */
        rng = xorshift(rng);
        const float white = rand_bipolar(rng);
        float noise;
        if (p->noise_pink) {
            /* Paul Kellet's three-pole economy filter: -3 dB an octave to
               within a tenth of a decibel across the band, for three
               multiply-accumulates. */
            pk0 = 0.99765f * pk0 + white * 0.0990460f;
            pk1 = 0.96300f * pk1 + white * 0.2965164f;
            pk2 = 0.57000f * pk2 + white * 1.0526913f;
            noise = (pk0 + pk1 + pk2 + white * 0.1848f) * 0.22f;
        } else {
            noise = white;
        }

        /* --- oscillator 3, which is also the modulation source --- */

        /*
         * Off the keyboard it is a free-running oscillator at whatever its
         * range and frequency knobs say, which is what OSC. 3 CONTROL is for:
         * a modulation source that does not change pitch when you play.
         */
        const float bend3 = p->osc3_kbd
                          ? exp2_fast((pitch - 60.0f) * (1.0f / 12.0f))
                          : 1.0f;
        float f2 = base[2] * bend3;
        if (f2 > nyq) { f2 = nyq; }
        const float dt2 = f2 * v->inv_sr;
        ph2 = wrap1(ph2 + dt2);
        const float o3 = shape(w2, ph2, dt2);

        /*
         * The modulation source: oscillator 3 at one end of MODULATION MIX,
         * noise at the other. It is taken before the mixer, so it is heard as
         * modulation whether or not oscillator 3's own fader is up - which is
         * the arrangement that lets the third oscillator be a modulator and a
         * voice independently.
         */
        const float mod = o3 + (noise - o3) * v->s_mix;
        modv = mod;

        /* --- oscillators 1 and 2 -------------------------------- */
        const float bend_semis = (pitch - 60.0f)
                               + (p->osc_mod ? mod * MOD_OSC_SEMIS : 0.0f);
        const float bend = exp2_fast(bend_semis * (1.0f / 12.0f));

        float f0 = base[0] * bend; if (f0 > nyq) { f0 = nyq; }
        float f1 = base[1] * bend; if (f1 > nyq) { f1 = nyq; }

        const float dt0 = f0 * v->inv_sr;
        const float dt1 = f1 * v->inv_sr;
        ph0 = wrap1(ph0 + dt0);
        ph1 = wrap1(ph1 + dt1);

        const float o1 = shape(w0, ph0, dt0);
        const float o2 = shape(w1, ph1, dt1);

        /* --- the mixer ------------------------------------------ */
        float sum = o1 * v->s_vol[0] + o2 * v->s_vol[1] + o3 * v->s_vol[2]
                  + noise * v->s_noise + fb * v->s_fbv;

        const float mag = sum < 0.0f ? -sum : sum;
        if (mag > peak) { peak = mag; }

        /* --- the ladder ----------------------------------------- */
        float cut_oct = (v->s_cut + 4.0f) * CUT_OCT
                      + kbd_amt * (pitch - 60.0f) * (1.0f / 12.0f)
                      + p->contour * fenv * CONTOUR_OCT;
        if (p->filt_mod) {
            cut_oct += mod * MOD_FILT_OCT;
        }
        float fc = CUT_BASE * exp2_fast(cut_oct);
        if (fc > 18000.0f) { fc = 18000.0f; }
        if (fc < 12.0f)    { fc = 12.0f; }

        float y = ladder(v, sum, fc, v->s_res);

        /* --- the loudness contour ------------------------------- */

        /*
         * The amplifier, and then the feedback tapped off it - before the
         * master fader and not after.
         *
         * Before, because the fader is how loud you are listening and the
         * return is part of the sound: taken after it, turning the volume down
         * would quietly change the timbre as well, and at the default setting
         * would hold the loop gain under one so the howl could never start at
         * all.
         *
         * After the contour, though, because that is where the hardware's
         * output jack is: close the loudness contour and the loop closes with
         * it, so a Minimoog left screaming stops when the key comes up rather
         * than howling until somebody turns the input down.
         */
        const float amp = y * lenv;

        /* The coupling capacitor: y = x - x1 + r*y1. See FB_HP_R. */
        fb    = amp - fb_x1 + FB_HP_R * fb;
        fb_x1 = amp;
        fb    = clampf(fb, -2.0f, 2.0f);


        int32_t q = (int32_t)(amp * v->s_master * 30000.0f);
        if (q >  32767) { q =  32767; }
        if (q < -32768) { q = -32768; }
        out[i] = (int16_t)q;
    }

    v->phase[0] = ph0;
    v->phase[1] = ph1;
    v->phase[2] = ph2;
    v->pitch    = pitch;
    v->rng      = rng;
    v->pink0 = pk0; v->pink1 = pk1; v->pink2 = pk2;
    v->fenv = fenv; v->lenv = lenv;
    v->fb    = fb;
    v->fb_x1 = fb_x1;
    v->peak = peak;

    /*
     * The overload lamp, which on the hardware watches the mixer and not the
     * output. That is the useful place for it: the ladder is what distorts,
     * and by the time the distortion is at the speaker the lamp would only be
     * telling you what you can already hear.
     */
    v->overload = (peak > 1.0f);

    /* The modulation source as it was left, for the panel's indicator. */
    v->mod_out = clampf(modv, -1.0f, 1.0f);
}
