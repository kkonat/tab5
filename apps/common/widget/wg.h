/*
 * The knobs, the selectors and the switches - one set, shared.
 *
 * These began in apps/synth1 and moved here when apps/moog needed the same
 * ones. That is the whole reason they are shared rather than restyled: two
 * synthesisers on the same machine drawn in two different hands would look
 * like two programs by two people, and a knob is a knob. What each app still
 * owns is its own *layout* - where the cells are and what goes in them - which
 * is the part that genuinely differs between a six-control test rig and a
 * thirty-six-control Minimoog.
 *
 * Everything takes landscape coordinates and draws through turn.h, so nothing
 * here knows which way up the tablet is.
 *
 * The palette is the system theme's and nothing else. An app that wanted a
 * red switch cap because the hardware it imitates has one would be the first
 * step towards every app on this machine having its own idea of what a control
 * looks like.
 */
#ifndef WG_H
#define WG_H

#include <stdbool.h>
#include <stdint.h>

#include "turn.h"

/**
 * One knob, centred on (cx, cy) with radius @p r, turned to @p norm of its
 * travel - seven-thirty round to four-thirty, the way a panel knob's end stops
 * are drawn.
 *
 * @p detents is 0 for a continuous control, or the number of positions for a
 * selector: that changes the tick count, and lifts the pointer's inner end
 * clear of the middle of the face so that wg_wave() or a legend can go there.
 */
void wg_knob(const turn_t *g, int16_t cx, int16_t cy, int16_t r, float norm,
             int detents);

/**
 * The waveforms a selector can be pointing at.
 *
 * One list across both apps, in no particular order, because an app maps its
 * own enumeration onto this one and the numbering here is nobody's ABI.
 */
typedef enum {
    WG_SAW = 0,
    WG_TRI,
    WG_SQUARE,
    WG_SH,          /**< sample and hold                       */
    WG_TRISAW,      /**< a ramp with its corner knocked off     */
    WG_WIDE,        /**< a rectangle, about a quarter           */
    WG_NARROW,      /**< a rectangle, about an eighth           */
    WG_WAVES
} wg_wave_t;

/**
 * A waveform, drawn small in the middle of a selector's face.
 *
 * @p hw and @p hh are the half-width and half-height. Keep them inside 0.46 of
 * the knob's radius at the corners, which is what leaves wg_knob()'s pointer
 * clear of the glyph.
 *
 * @p cycles is how many periods to fit in that width, and it is a parameter
 * rather than a constant because the right answer depends on how big the knob
 * is. Two is better where there is room: one square and one sawtooth are the
 * same picture until you see where the next one starts. But the width has to
 * be shared out between them, and on a knob small enough that one period is
 * twenty pixels, a narrow pulse drawn twice is two vertical lines and no
 * information at all - where drawn once it is still a pulse with a width you
 * can compare against the one beside it.
 */
void wg_wave(const turn_t *g, int16_t cx, int16_t cy, int16_t hw, int16_t hh,
             int wave, int cycles);

/**
 * A two-position switch: two stacked caps, the one in force lit.
 *
 * A switch and not a two-position knob, because a knob with two positions is a
 * knob you have to read. Both positions carry their legend, so which way it is
 * set can be seen without knowing which way up "on" is.
 */
void wg_switch(const turn_t *g, ngl_rect_t r, bool upper,
               const char *a, const char *b);

#endif /* WG_H */
