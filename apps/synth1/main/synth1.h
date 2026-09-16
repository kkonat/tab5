/*
 * Synth1 - a monophonic oscillator, an LFO bending it, and a scope.
 *
 * This is a proof of concept for a Minimoog on the Tab5, and the thing it
 * exists to prove is not that a sawtooth can be generated. It is that the two
 * loops can share this machine: a codec that must be fed every five
 * milliseconds for as long as the app is open, and a 1280x720 panel whose
 * knobs have to move under a finger without the sound noticing. Everything
 * here is arranged around that one question, and the answer is on the screen -
 * see the footer, and docs/README.md for what each number is.
 *
 * Three decisions carry it, and each is written up where it is made:
 *
 *   syn_audio.h  the voice runs on a thread of its own at a priority above the
 *                drawing loop, so being late is not something the UI can cause
 *   turn.h       the canvas is the panel's own shape and the landscape turn is
 *                in the coordinates, so no frame is ever rotated by the PPA -
 *                the expensive half of what lab/defender measured
 *   syn_ui.c     a widget repaints when its own value changed and the scope
 *                repaints a column when that column moved, so a frame is
 *                proportional to what altered rather than to the screen
 *
 * What comes next - two oscillators, noise, the mixer, the ladder filter, the
 * two envelopes, three pages behind < > and a 1.5-octave keyboard - changes
 * syn_dsp.c and syn_ui.c and touches neither of the other two.
 */
#ifndef SYNTH1_H
#define SYNTH1_H

#include <stdbool.h>
#include <stdint.h>

#include "syn_audio.h"
#include "syn_dsp.h"
#include "turn.h"

/* The controls, in the order they sit across the panel. */
typedef enum {
    C_FREQ = 0,   /* oscillator pitch          */
    C_LEVEL,      /* output level              */
    C_RATE,       /* LFO rate                  */
    C_AMT,        /* LFO depth                 */
    C_SHAPE,      /* LFO waveform, four ways   */
    C_HILO,       /* which rate range          */
    C_COUNT
} syn_ctl_t;

/* What a touch landed on, when it did not land on a control. */
#define HIT_NONE   (-1)
#define HIT_CLOSE  (-2)
#define HIT_FOOTER (-3)

typedef struct {
    turn_t gfx;

    /*
     * A knob's position is the truth and its value is derived from it, rather
     * than the other way about.
     *
     * Storing the value instead would mean inverting the mapping on every
     * touch, and the log mappings are not exactly invertible in single
     * precision - so a knob that was not moved would creep by a fraction of a
     * pixel every time it was asked where it was.
     */
    float norm[C_COUNT];       /* 0..1, and only the continuous four are used */
    int   shape;
    bool  hi;                  /* the LFO's rate range */

    syn_patch_t patch;         /* what the above comes to, in Hz and fractions */

    /* --- what a finger is doing --- */
    int     grab;              /* the control being dragged, or HIT_NONE */
    float   grab_norm;         /* where it was when the finger landed    */
    int16_t grab_x, grab_y;
    bool    grab_moved;        /* far enough that this is a drag, not a tap */

    /* --- instruments --- */
    uint32_t fps;
    uint32_t work_us;          /* the frame, minus the instruments themselves */
    uint32_t paint_us;         /* what went to the glass, of that            */
    bool     capped;           /* paced to 60 Hz, or let run                 */
    bool     audio;            /* the codec came up                          */

    /* --- what needs drawing --- */
    bool     repaint_all;
    uint32_t ctl_dirty;        /* one bit per syn_ctl_t */
    bool     scope_dirty;
    bool     hud_dirty;
} syn_app_t;

#endif /* SYNTH1_H */
