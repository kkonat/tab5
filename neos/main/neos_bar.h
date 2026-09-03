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

/** The close button, in screen coordinates. */
ngl_rect_t neos_bar_close_rect(void);

/** True if (x, y) is on the close button and the button is showing. */
bool neos_bar_hit_close(int16_t x, int16_t y);
