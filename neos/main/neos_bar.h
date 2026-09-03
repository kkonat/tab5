/*
 * The system bar, drawn by NeOS. See neos_bar.c for why it is not the shell's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ngl.h"

/** Reserve the bar and point the status line at it. Call once, after ngl. */
void neos_bar_init(void);

/** Height of the reserved strip. */
int16_t neos_bar_height(void);

/**
 * Show or hide the close button.
 *
 * True only while an app the user can actually leave is running. The card's
 * autorun app is relaunched the moment it returns, so closing it would just
 * repaint the same screen - and with no card there is no app at all.
 */
void neos_bar_set_closable(bool closable);

/**
 * What is under (x, y) on the bar, if anything.
 *
 * The bar's controls belong to NeOS: the touch driver asks this on every tap
 * and acts on the answer itself, so an app never sees the gesture that closes
 * it or the one that opens the Wi-Fi list. The numbers carry no meaning beyond
 * this file and the switch in neos_touch.c.
 */
typedef enum {
    NEOS_BAR_NONE = 0,
    NEOS_BAR_CLOSE,
    NEOS_BAR_WIFI,
    NEOS_BAR_CLOCK,
} neos_bar_hit_t;

neos_bar_hit_t neos_bar_hit(int16_t x, int16_t y);

/**
 * Repaint the Wi-Fi icon and the clock if either has moved.
 *
 * Called on a timer from inside the bar, and by anything that has just changed
 * the network on purpose and does not want to wait a second to see it.
 */
void neos_bar_widgets_refresh(void);
