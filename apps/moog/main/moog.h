/*
 * A Minimoog Model D on the Tab5.
 *
 * Thirty-six controls over two pages, a master fader that is on both of them,
 * and an octave and a half of keyboard along the bottom. The signal path is
 * mg_dsp.c; this header is the panel's half - what the controls are, where
 * they are, and what a finger is doing to them.
 *
 * ------------------------------------------------------------- the two pages
 *
 * The Model D's panel is four sections wide and none of them will fit usefully
 * on a 1280-pixel screen next to the other three. So it is cut where the
 * instrument itself is cut:
 *
 *   page 1   CONTROLLERS and OSCILLATOR BANK - what the sound is made of
 *   page 2   MIXER and MODIFIERS - how much of each, and what happens to it
 *
 * That split is not arbitrary and it is not only about width. Tuning an
 * oscillator and setting a filter contour are different activities, minutes
 * apart, and the two halves of the panel are in constant use *within* an
 * activity and hardly at all across one. Cutting between the mixer and the
 * oscillator bank instead would separate "which oscillator is this" from "can
 * I hear it", which is a pair you work with a second apart.
 *
 * The master fader is on neither page because it belongs to neither: it is the
 * one control you reach for without looking, and a volume you have to change
 * pages to find is a volume you cannot turn down in a hurry.
 *
 * ---------------------------------------------------------- what it is built on
 *
 * Both of the decisions apps/synth1 was written to test are taken as settled
 * here and are not restated:
 *
 *   the voice is on a thread above the drawing loop      mg_audio.h
 *   the canvas is the panel's shape, so nothing rotates  apps/common/turn
 *
 * What this app adds to them is a third: with thirty-six controls on screen,
 * a repaint has to be able to cost one of them. See mg_ui.c.
 */
#ifndef MOOG_H
#define MOOG_H

#include <stdbool.h>
#include <stdint.h>

#include "turn.h"
#include "wg.h"
#include "mg_audio.h"
#include "mg_dsp.h"
#include "mg_panel.h"

/* ------------------------------------------------------------------ */
/* The controls                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    /* --- page 1: CONTROLLERS --- */
    MC_TUNE = 0,
    MC_OSC_MOD,
    MC_GLIDE,
    MC_MOD_MIX,
    MC_OSC3_CTL,

    /* --- page 1: OSCILLATOR BANK --- */
    MC_O1_RANGE, MC_O1_WAVE,
    MC_O2_RANGE, MC_O2_FREQ, MC_O2_WAVE,
    MC_O3_RANGE, MC_O3_FREQ, MC_O3_WAVE,

    /* --- page 2: MIXER --- */
    MC_V1, MC_ON1,
    MC_V2, MC_ON2,
    MC_V3, MC_ON3,
    MC_VN, MC_ONN, MC_NCOLOUR,
    MC_VI, MC_ONI, MC_IMODE,

    /* --- page 2: MODIFIERS --- */
    MC_FILT_MOD, MC_KBD1, MC_KBD2,
    MC_CUTOFF, MC_EMPHASIS, MC_CONTOUR,
    MC_FA, MC_FD, MC_FS,
    MC_LA, MC_LD, MC_LS,

    /* --- on every page --- */
    MC_VOLUME,

    MC_COUNT
} mg_ctl_t;

/** Which page a control is on. MG_BOTH is the fader. */
#define MG_PAGES 2
#define MG_BOTH  2

typedef enum {
    CK_KNOB = 0,   /**< continuous, 0..1                                     */
    CK_SELECT,     /**< stepped; a knob with detents and a named position    */
    CK_SWITCH,     /**< two positions, both legends showing, the one in force lit */
    CK_SLIDER      /**< the fader                                            */
} mg_kind_t;

/*
 * There is no separate rocker kind, and no red one either.
 *
 * The instrument distinguishes its routing switches from its level switches by
 * making the first red, which is real information - it is the only thing
 * separating OSCILLATOR MODULATION from the four ON switches it sits among.
 * It is dropped here on purpose: the widgets are apps/common/widget's and are
 * drawn exactly as apps/synth1 draws them, and one app inventing a second
 * palette is the first step towards every app on this machine having its own
 * idea of what a control looks like. What carries the distinction instead is
 * the grouping and the caption, which is what the sections are for.
 */
typedef struct {
    uint8_t     page;
    uint8_t     kind;
    uint8_t     steps;     /**< CK_SELECT: how many positions               */
    const char *cap;       /**< the caption over it, or NULL for none       */
    const char *lab_a;     /**< CK_SWITCH: the upper legend                 */
    const char *lab_b;     /**< CK_SWITCH: the lower legend                 */
} mg_def_t;

extern const mg_def_t mg_defs[MC_COUNT];

/* What a touch landed on, when it did not land on a control. */
#define HIT_NONE   (-1)
#define HIT_CLOSE  (-2)
#define HIT_PREV   (-3)
#define HIT_NEXT   (-4)
#define HIT_KEYS   (-5)
#define HIT_METERS (-6)

/* ------------------------------------------------------------------ */
/* The keyboard                                                        */
/* ------------------------------------------------------------------ */

/* The semitone the leftmost key plays: C3, so the octave and a half runs up
   to the F above middle C. MG_KEYS - how many there are - is mg_panel.h's,
   next to the code that draws them. */
#define MG_KEY_C  48

/* ------------------------------------------------------------------ */
/* The app                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    turn_t   gfx;

    /*
     * A control's position is the truth and its value is derived from it.
     *
     * Uniform across the five kinds - a knob is 0..1, a selector is its
     * position number, a switch is 0 or 1 - so that dragging, painting and
     * hit testing are each one function rather than five. What the number
     * *means* is mg_ui_patch()'s business and nothing else's.
     */
    float    v[MC_COUNT];

    uint8_t  page;
    mg_patch_t patch;

    /* --- the keyboard --- */
    uint32_t held;             /**< bit per semitone, 0 = the leftmost key */
    float    note;             /**< what is sounding: the lowest held      */
    bool     gate;

    /* --- what a finger is doing to a control --- */
    int      grab;             /**< the control, or a HIT_* constant */
    float    grab_v;
    int16_t  grab_x, grab_y;   /**< where it landed */
    int16_t  last_x, last_y;   /**< and where it was last frame, so that the
                                    next frame's points can be matched to it */
    bool     grab_moved;

    /* --- instruments --- */
    uint32_t fps, work_us, paint_us;
    bool     capped;
    bool     audio;

    /* --- what needs drawing --- */
    bool     repaint_all;
    uint32_t ctl_dirty[2];     /**< a bit per control, in two words */
    uint32_t keys_changed;     /**< a bit per key that went down or up       */
    bool     head_dirty;
} mg_app_t;

static inline void mg_mark(mg_app_t *a, int c)
{
    if (c >= 0 && c < MC_COUNT) {
        a->ctl_dirty[c >> 5] |= 1u << (c & 31);
    }
}

static inline bool mg_is_dirty(const mg_app_t *a, int c)
{
    return (a->ctl_dirty[c >> 5] & (1u << (c & 31))) != 0;
}

#endif /* MOOG_H */
