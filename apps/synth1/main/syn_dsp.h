/*
 * One oscillator and the LFO bending it. No I/O, no globals, no clock.
 *
 * This is the part that will grow into the Minimoog - a second oscillator, the
 * noise source, the mixer, the ladder and its two envelopes all arrive here -
 * so it is kept as a function of a state block and a patch and nothing else.
 * What it must never acquire is a reason to know about the screen, the codec
 * or the passage of time: syn_audio.c owns all three, and the reason the audio
 * loop cannot starve is that this routine's cost is a known number of
 * instructions per sample rather than something that depends on what the glass
 * is doing.
 *
 * Single precision throughout, and the whole app is built
 * -Werror=double-promotion to keep it that way: apps link -nostdlib and the
 * loader's table carries only some of the double helpers, so a stray `1.0`
 * is an app that will not load rather than one that runs slowly.
 *
 * ------------------------------------------------------------- the aliasing
 *
 * The oscillator is a sawtooth, which is the Minimoog's own richest waveform
 * and also the one that aliases worst if it is generated naively: a ramp reset
 * is a step, a step has every harmonic, and every harmonic above 24 kHz folds
 * back down as an inharmonic whistle that moves the wrong way when you turn
 * the pitch knob. PolyBLEP subtracts a two-sample-wide correction across the
 * discontinuity, which is about eight instructions and takes the worst of that
 * away. It matters more here than in most synths because the LFO is allowed to
 * run at audio rate: at 2 kHz modulating a 2 kHz carrier the instantaneous
 * pitch sweeps most of the spectrum every millisecond, and without the
 * correction what you hear is the folding rather than the FM.
 */
#ifndef SYN_DSP_H
#define SYN_DSP_H

#include <stdbool.h>
#include <stdint.h>

/** LFO shapes, in the order the selector steps through them. */
typedef enum {
    SYN_SAW = 0,
    SYN_TRI,
    SYN_SQR,
    SYN_SH,
    SYN_SHAPES
} syn_shape_t;

/** The four-character legends the selector draws. Indexed by syn_shape_t. */
const char *syn_shape_name(int shape);

/**
 * What the knobs are asking for, in the units the knobs are calibrated in.
 *
 * Read once per block by syn_voice_render() and smoothed from there, so a
 * caller may update it whenever it likes and does not have to think about
 * where the block boundary is.
 */
typedef struct {
    float freq;      /**< oscillator pitch, Hz                          */
    float amp;       /**< output level, 0..1                            */
    float lfo_rate;  /**< Hz - sub-audio or audio, the knob decides     */
    float lfo_amt;   /**< 0..1, which is 0..+/-1 octave of deviation    */
    int   lfo_shape; /**< syn_shape_t                                   */
} syn_patch_t;

typedef struct {
    float    sr, inv_sr;

    float    phase;      /* oscillator, 0..1 */
    float    lphase;     /* LFO, 0..1        */
    float    sh;         /* the held sample  */
    uint32_t rng;

    /*
     * The smoothed copies, which are what is actually played.
     *
     * A knob is a finger on glass reporting in jumps of several pixels, and a
     * frequency that jumps is a click - the waveform's slope changes between
     * one sample and the next with nothing to make the change continuous. So
     * every continuous parameter is chased rather than assigned, at a time
     * constant short enough not to feel like lag and long enough that the step
     * is inaudible. The shape is not among them: it is a discontinuity by
     * nature and smoothing between a square and a triangle would be a fifth
     * waveform nobody asked for.
     */
    float    freq, amp, lrate, lamt;
    bool     primed;     /* the chase starts *at* the first patch, not at zero */

    float    lfo_out;    /* the last LFO value, for the indicator on screen */
} syn_voice_t;

void syn_voice_init(syn_voice_t *v, float sample_rate);

/**
 * Render @p n mono frames.
 *
 * Writes int16 because that is what the codec takes, and clamps rather than
 * wrapping: an overflow that wraps is a full-scale square wave in the middle
 * of a note, which is the loudest possible way to report a gain mistake.
 */
void syn_voice_render(syn_voice_t *v, const syn_patch_t *p, int16_t *out, int n);

#endif /* SYN_DSP_H */
