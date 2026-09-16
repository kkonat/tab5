/*
 * The two things this panel needs that apps/common/widget does not have: a
 * fader and a keyboard.
 *
 * Everything else - knobs, selectors, switches - is wg.h's and is drawn
 * exactly as apps/synth1 draws it. Two synthesisers on one machine drawn in
 * two hands would look like two programs by two people, so nothing here
 * restyles anything: these are the pieces that had nowhere to be shared from,
 * and they are drawn out of the same theme palette as the rest.
 *
 * Landscape coordinates throughout, drawn through turn.h, so nothing here
 * knows which way up the tablet is.
 */
#ifndef MG_PANEL_H
#define MG_PANEL_H

#include <stdbool.h>
#include <stdint.h>

#include "turn.h"

/*
 * An octave and a half: C up to the F eighteen semitones above, which is
 * eleven white keys and seven black. Not two octaves, because at the width
 * that is left after the fader, two octaves is a 45-pixel white key - narrower
 * than the thumb that has to land on it.
 */
#define MG_KEYS 18

/** The master fader. Vertical, with a cap, and zero at the bottom. */
void mg_fader(const turn_t *g, ngl_rect_t r, float norm);

/** The overload lamp: the mixer running past full scale, which is the Model
    D's own place to watch for it - the ladder is what distorts, and by the
    output the lamp would only be repeating what you can already hear. */
void mg_lamp(const turn_t *g, ngl_rect_t r, bool on);

/** Paint the keyboard. @p held is one bit per semitone from the leftmost. */
void mg_keys(const turn_t *g, ngl_rect_t r, uint32_t held);

/**
 * Repaint one key, and whatever black keys overlap it.
 *
 * Takes the whole @p held mask rather than one key's state, because a white
 * key's rectangle runs under the two black keys either side of it: repainting
 * it rubs them out, and putting them back needs to know whether they are
 * themselves down. Returns the rectangle actually touched - wider than the key
 * where a black one overhangs - which is what has to be handed to the glass.
 */
ngl_rect_t mg_key_paint(const turn_t *g, ngl_rect_t r, int key, uint32_t held);

/** Which key is under (x, y), or -1. */
int mg_key_at(ngl_rect_t r, int16_t x, int16_t y);

/** The rectangle one key occupies. */
ngl_rect_t mg_key_rect(ngl_rect_t r, int key);

bool mg_key_black(int key);

#endif /* MG_PANEL_H */
