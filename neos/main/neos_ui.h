/*
 * System panels - the screens NeOS owns, over whatever app is running.
 *
 * There are two ways one opens. The keyboard is raised by the app itself, from
 * inside neos_input_text(), and blocks it. The Wi-Fi list and the clock page
 * are raised by a tap on the system bar, which arrives on the touch task while
 * the app is happily running - so they run on a task of their own and the app
 * is fenced off the screen instead of being stopped. Both go through the same
 * two mechanisms: ngl_overlay_enter() takes the pixels, neos_touch_capture()
 * takes the finger.
 *
 * Panels are not apps. Launching an app to change a Wi-Fi setting would evict
 * whatever the user was doing, which is exactly what a modal panel exists to
 * avoid - you come back to the screen you left, mid-edit, unaware anything
 * happened.
 *
 * The widget helpers at the bottom are shared by every panel and by the
 * keyboard. They are here rather than in each panel so that the frames, the
 * close boxes and the buttons are the same shape everywhere without three
 * files having to agree by hand.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ngl.h"

typedef enum {
    NEOS_PANEL_NONE = 0,
    NEOS_PANEL_WIFI,
    NEOS_PANEL_CLOCK,
    NEOS_PANEL_BATTERY,
} neos_panel_t;

/** Start the task that runs bar-launched panels. Call once, after the bar. */
void neos_ui_init(void);

/**
 * Ask for a panel.
 *
 * Returns at once - the panel runs on the ui task, not the caller's, because
 * the caller is the touch poller and a panel that ran there would stop input
 * being read, including its own.
 *
 * Ignored while a panel is already up. There is one screen.
 */
void neos_ui_open(neos_panel_t p);

/** Ask the open panel to close. For when the app underneath is going away. */
void neos_ui_close(void);

/**
 * True while a panel owns the screen.
 *
 * Also the answer to the app-facing neos_ui_busy(); see neos_api.h for what an
 * app is expected to do about it, which is nothing.
 */
bool neos_ui_active(void);

/**
 * True when the open panel should wind up, because the app underneath it is
 * going away or the card was pulled. Panels poll it the way an app polls
 * neos_app_close_requested().
 *
 * The close box is not routed through here: a panel owns its own frame, sees
 * that tap itself and simply returns.
 */
bool neos_ui_should_close(void);

/* ------------------------------------------------------------------ */
/* Panel chrome                                                        */
/* ------------------------------------------------------------------ */

/** Height of a panel's title bar, and so where its content starts. */
#define NEOS_UI_TITLE_H  56

/** Darken everything behind a panel, so it reads as in front rather than instead. */
void neos_ui_dim(ngl_surface_t *s);

/**
 * Paint a panel's frame and title into @p r, and return the rectangle left
 * over for its content.
 *
 * The close box goes in the upper right of the frame - the same corner as the
 * system bar's, so that "the way out is up there" is one thing to learn rather
 * than one per panel.
 */
ngl_rect_t neos_ui_frame(ngl_surface_t *s, ngl_rect_t r, const char *title);

/** The close box of a frame at @p r, in screen coordinates. */
ngl_rect_t neos_ui_close_rect(ngl_rect_t r);

/**
 * A labelled button.
 *
 * @p on fills it instead of outlining it - the same weight difference the
 * switches in the system app use, and for the same reason: in a one-hue
 * palette, weight reads as a state and brightness reads as an artefact.
 */
void neos_ui_button(ngl_surface_t *s, ngl_rect_t r, const char *label, bool on);

/** Centre a string in @p r. */
void neos_ui_text_centred(ngl_surface_t *s, ngl_rect_t r, const char *str,
                          const ngl_font_t *f, ngl_color_t c);

/** Draw @p str at the left of @p r, clipped to it, vertically centred. */
void neos_ui_text_left(ngl_surface_t *s, ngl_rect_t r, const char *str,
                       const ngl_font_t *f, ngl_color_t c);

/** As neos_ui_text_left(), right-aligned. */
void neos_ui_text_right(ngl_surface_t *s, ngl_rect_t r, const char *str,
                        const ngl_font_t *f, ngl_color_t c);
