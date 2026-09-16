/*
 * The instrument: three oscillators, noise, a mixer, a four-pole ladder and
 * two contours. No I/O, no globals, no clock.
 *
 * This is the Model D's signal path and it is arranged in the order the panel
 * is, because that is the order the sound goes through it - which is most of
 * why the panel is laid out that way in the first place:
 *
 *   CONTROLLERS      tune, glide, and what the modulation source is made of
 *   OSCILLATOR BANK  three oscillators, each a range, a detune and a waveform
 *   MIXER            their three levels, plus noise and the feedback return
 *   MODIFIERS        the ladder, and the two contours that move it and the gain
 *
 * Everything crossing into here is single precision and the whole app builds
 * -Werror=double-promotion: apps link -nostdlib against a table carrying only
 * some of the double helpers, so a stray `1.0` is an app that will not load
 * rather than one that runs slowly.
 *
 * ---------------------------------------------------------------- the wheels
 *
 * The Model D's two wheels are to the left of the keyboard and are not on the
 * panel in the photograph this was built from, so they are not here. What that
 * costs is the modulation *depth*: MODULATION MIX sets what the source is made
 * of - oscillator 3 at one end, noise at the other - and on the real
 * instrument the wheel then decides how much of it reaches the oscillators or
 * the filter. With no wheel, the two routing switches carry a fixed depth
 * chosen to be musical rather than a parameter, and a wheel is the obvious
 * next control to add. See MOD_OSC_SEMIS and MOD_FILT_OCT.
 */
#ifndef MG_DSP_H
#define MG_DSP_H

#include <stdbool.h>
#include <stdint.h>

/** The six oscillator footages, in the order the RANGE knob steps them. */
typedef enum { RANGE_LO, RANGE_32, RANGE_16, RANGE_8, RANGE_4, RANGE_2, RANGE_N } mg_range_t;

/** The six waveforms, in the order the WAVEFORM knob steps them - the Model
    D's own order, triangle round to narrowest pulse. */
typedef enum {
    WAVE_TRI, WAVE_TRISAW, WAVE_SAW, WAVE_SQUARE, WAVE_WIDE, WAVE_NARROW, WAVE_N
} mg_wave_t;

const char *mg_range_name(int r);
const char *mg_wave_name(int w);

/** How far the keyboard tracks the filter: none, a third, two thirds, all.
    On the Model D that is the two KEYBOARD CONTROL switches, and both of them
    together is the full amount rather than one and a third. */
typedef enum { KBD_NONE, KBD_THIRD, KBD_TWOTHIRD, KBD_FULL } mg_kbdtrack_t;

/**
 * Everything the panel has to say, in the units the panel is calibrated in.
 *
 * Read once per block and smoothed from there, so the UI may rewrite it
 * whenever it likes and never has to think about where a block boundary is.
 */
typedef struct {
    /* --- controllers --- */
    float   tune;            /**< semitones, -2..+2                          */
    float   glide;           /**< seconds to cross an octave, 0 = off        */
    float   mod_mix;         /**< 0 = all oscillator 3, 1 = all noise        */
    bool    osc_mod;         /**< route the modulation to oscillator pitch   */
    bool    osc3_kbd;        /**< oscillator 3 follows the keyboard          */

    /* --- oscillator bank --- */
    uint8_t range[3];
    uint8_t wave[3];
    float   detune[3];       /**< semitones; [0] is unused - osc 1 is the reference */

    /* --- mixer --- */
    float   vol[3];
    bool    on[3];
    float   noise_vol;
    bool    noise_on;
    bool    noise_pink;
    float   fb_vol;          /**< the external input's level                 */
    bool    fb_on;
    bool    fb_mode;         /**< true = feed the output back into the mixer */

    /* --- modifiers --- */
    bool    filt_mod;        /**< route the modulation to the cutoff         */
    uint8_t kbd_track;       /**< mg_kbdtrack_t                              */
    float   cutoff;          /**< the panel's own -4..+4                     */
    float   emphasis;        /**< 0..1, self-oscillating at the top          */
    float   contour;         /**< 0..1, how much of the filter contour       */
    float   fa, fd, fs;      /**< filter contour: attack s, decay s, sustain */
    float   la, ld, ls;      /**< loudness contour, the same                 */

    float   master;          /**< 0..1                                       */
} mg_patch_t;

/* What one key press is, as far as the voice is concerned. */
typedef struct {
    float note;              /**< MIDI-ish, 60 = middle C. Fractional is fine */
    bool  gate;
} mg_perf_t;

typedef struct {
    float    sr, inv_sr;

    float    phase[3];
    uint32_t rng;
    float    pink0, pink1, pink2;

    /* Glide, in semitones, chased toward the note being held. */
    float    pitch, pitch_target;
    bool     primed;
    bool     was_gated;

    /* The ladder's four poles and the memories between them. */
    float    y1, y2, y3, y4, ox, oy1, oy2, oy3;

    /* The two contours. */
    float    fenv, lenv;
    uint8_t  fstage, lstage;

    float    fb;             /**< the feedback return, already AC-coupled */
    float    fb_x1;          /**< and the coupling capacitor's memory       */

    /* The smoothed copies of everything a finger can move mid-note. */
    float    s_cut, s_res, s_master, s_vol[3], s_noise, s_fbv, s_mix;

    /* What the panel wants back. */
    float    mod_out;        /**< the modulation source, -1..1              */
    float    peak;           /**< the mixer's level, for the overload lamp  */
    bool     overload;
} mg_voice_t;

void mg_voice_init(mg_voice_t *v, float sample_rate);

/**
 * Render @p n mono frames.
 *
 * Writes int16 because that is what the codec takes, and clamps rather than
 * wrapping: an overflow that wraps is a full-scale square wave in the middle
 * of a note, which is the loudest possible way to report a gain mistake.
 */
void mg_voice_render(mg_voice_t *v, const mg_patch_t *p, const mg_perf_t *perf,
                     int16_t *out, int n);

#endif /* MG_DSP_H */
