/*
 * The screen going off, and the idle timer that does it unasked.
 *
 * Sleep on this tablet is the panel going dark and nothing else stopping, and
 * neos_api.h says why that is the whole of it. This is the firmware side: the
 * watcher task, the wake sources, and the two pieces of hardware it drives that
 * nothing else has any business driving.
 */
#pragma once

#include <stdbool.h>

#include "neos_api.h"

/**
 * Load the saved timeout and start watching. Called once during bring-up, after
 * the settings store and the display.
 */
void neos_screen_init(void);
