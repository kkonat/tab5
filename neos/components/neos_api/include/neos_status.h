/*
 * NeOS status line - transient messages in the system bar.
 *
 * "Scanning /apps on sd", "Scanned", "Card ejected". One message at a time;
 * a new one replaces whatever is showing.
 *
 * The status area is not a fixed rectangle. It is whatever the bar has left
 * over after its widgets have taken their space, which the bar owner reports
 * through the region callback - so adding a clock or a battery gauge shrinks
 * the status line automatically instead of drawing over it. Text that no
 * longer fits switches from centred to marquee rather than being cut off.
 */
#pragma once

#include <stdint.h>
#include "ngl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the status service.
 *
 * @param region_fn  Returns the rectangle currently available for status text,
 *                   in screen coordinates. Called on every repaint, so it can
 *                   shrink as bar widgets come and go.
 */
void neos_status_init(ngl_rect_t (*region_fn)(void));

/** Show `msg` until something replaces or clears it. NULL clears. */
void neos_status(const char *msg);

/** Show `msg`, then clear it automatically after `ms`. */
void neos_status_for(const char *msg, uint32_t ms);

/** Clear immediately. */
void neos_status_clear(void);

/**
 * Show a progress bar in the status area, 0..100. Negative ends it and hands
 * the area back to whatever message was showing.
 *
 * It lives where toasts do because that is the one strip of screen NeOS owns
 * outright - a transfer can then be shown over the top of any app, including
 * no app at all, without anything having to cooperate.
 */
void neos_status_progress(int percent);

/** Repaint the status area. The bar painter calls this on a full bar repaint. */
void neos_status_paint(void);

#ifdef __cplusplus
}
#endif
