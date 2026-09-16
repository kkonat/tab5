/*
 * The panel: where everything is, what a touch landed on, and what to repaint.
 *
 * Laid out from the canvas's size rather than from literals, so the same file
 * is right on a successor panel and right in either landscape orientation -
 * the turn is turn.c's business and nothing here knows which way up the
 * tablet is being held.
 */
#ifndef SYN_UI_H
#define SYN_UI_H

#include "synth1.h"

/** Work out the layout for the canvas's current size. Call after a rotation. */
void syn_ui_layout(const syn_app_t *a);

/** The rectangle a control owns, for hit testing and for repainting. */
ngl_rect_t syn_ui_ctl_rect(int ctl);

/** Which control is under (x, y), or one of the HIT_* constants. */
int syn_ui_hit(int16_t x, int16_t y);

/**
 * How far a knob has been turned by a drag of @p dy pixels.
 *
 * Vertical and relative: the knob picks up from wherever it was rather than
 * jumping to where the finger landed, which is the only behaviour that lets a
 * 70-pixel knob be set to a value finer than a seventieth.
 */
float syn_ui_drag(float norm0, int16_t y0, int16_t y);

/** The knob positions the app comes up on. The ranges live in syn_ui.c, so
    the patch it starts on is chosen there too rather than as four magic
    fractions in the loop. */
void syn_ui_defaults(syn_app_t *a);

/** The y every knob and the switch are centred on - which half of the RANGE
    switch a tap landed in is the only thing outside this file that needs it. */
int16_t syn_ui_knob_cy(void);

/** Recompute a->patch from the knob positions. */
void syn_ui_patch(syn_app_t *a);

/** Format the value a control is showing, into @p buf. */
void syn_ui_value(const syn_app_t *a, int ctl, char *buf, int size);

/** Paint the whole panel and put all of it on the glass. */
void syn_ui_repaint(syn_app_t *a);

/**
 * Paint whatever is marked dirty and present only that.
 *
 * Returns the microseconds spent handing rectangles to the panel, which the
 * footer reports separately from the drawing that filled them - the two have
 * different cures and a single number would hide which one is the problem.
 */
uint32_t syn_ui_paint(syn_app_t *a);

#endif /* SYN_UI_H */
