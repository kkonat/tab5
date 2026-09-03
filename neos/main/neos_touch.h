/*
 * Touch bring-up, core side. The app-facing half is in neos_api.h.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "neos_api.h"

/** Start the touch controller and its polling task. */
esp_err_t neos_touch_init(void);

/** Discard any uncollected tap. Called when the screen changes hands. */
void neos_touch_drop(void);

/**
 * Whether there is a working touch panel at all.
 *
 * Asked before anything modal goes up. A keyboard on a tablet that cannot be
 * touched is a screen with no way out of it, and the honest answer to "take
 * some text from the user" on such a machine is no, not a hang.
 */
bool neos_touch_present(void);

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

/*
 * While a modal panel is up the finger belongs to the panel, and neither the
 * app nor the system bar may see it.
 *
 * The app half matters because an app cannot be paused - it keeps polling
 * through the whole life of the panel - and an app that acted on a tap meant
 * for the keyboard would be responding to something the user was not doing to
 * it. The bar half matters because the close button would otherwise close the
 * app out from under its own modal dialog.
 *
 * Captured input does not vanish: it is read through the _os() readers below,
 * which is how the panel gets it.
 */
void neos_touch_capture(bool on);
bool neos_touch_captured(void);

/** As neos_touch_tap(), for the OS. Sees taps even while captured. */
bool neos_touch_tap_os(int16_t *x, int16_t *y);

/** As neos_touch_points(), for the OS. Sees fingers even while captured. */
int neos_touch_points_os(neos_touch_t *out, int max);
