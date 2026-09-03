/*
 * Touch bring-up, core side. The app-facing half is in neos_api.h.
 */
#pragma once

#include "esp_err.h"

/** Start the touch controller and its polling task. */
esp_err_t neos_touch_init(void);

/** Discard any uncollected tap. Called when the screen changes hands. */
void neos_touch_drop(void);
