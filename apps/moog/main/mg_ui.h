/*
 * Where every control is, what it is worth, and what to repaint.
 *
 * Laid out from the canvas's size rather than from literals, so the same file
 * is right in either landscape orientation - the turn is apps/common/turn's
 * business and nothing here knows which way up the tablet is being held.
 */
#ifndef MG_UI_H
#define MG_UI_H

#include "moog.h"

/** Work out the layout for the canvas's current size. Call after a rotation. */
void mg_ui_layout(const mg_app_t *a);

/** The rectangle a control owns, for hit testing and for repainting. */
ngl_rect_t mg_ui_rect(int ctl);

/** The keyboard's rectangle, which the app needs to route touches into. */
ngl_rect_t mg_ui_keys_rect(void);

/** Which control is under (x, y) on the page now showing, or a HIT_*. */
int mg_ui_hit(const mg_app_t *a, int16_t x, int16_t y);

/** The patch the instrument comes up on. */
void mg_ui_defaults(mg_app_t *a);

/** Recompute a->patch from the control positions. */
void mg_ui_patch(mg_app_t *a);

/** Format what a control is showing, into @p buf. */
void mg_ui_value(const mg_app_t *a, int ctl, char *buf, int size);

/**
 * Where a control ends up after a drag of (y0 - y) pixels from @p v0.
 *
 * Vertical and relative: a control picks up from wherever it was rather than
 * jumping to the finger, which is the only behaviour that lets a 60-pixel knob
 * be set to a value finer than a sixtieth. A selector lands on a detent.
 */
float mg_ui_drag(int ctl, float v0, int16_t y0, int16_t y);

/** Paint the whole panel and put all of it on the glass. */
void mg_ui_repaint(mg_app_t *a);

/**
 * Paint whatever is marked dirty and present only that.
 *
 * Returns the microseconds spent handing rectangles to the panel, which the
 * header reports apart from the drawing that filled them: the two have
 * different cures, and one number would hide which of them is the problem.
 */
uint32_t mg_ui_paint(mg_app_t *a);

#endif /* MG_UI_H */
