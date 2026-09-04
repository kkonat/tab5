/*
 * The boot chain.
 *
 * NeOS itself launches nothing by name. The card says what to run, in
 * autorun.cfg, and everything after that is one app at a time: an app runs,
 * returns, and NeOS runs whatever it asked for next - or the card's autorun
 * app again if it asked for nothing. Put a different card in and a different
 * shell comes up; that is the whole point of doing it this way.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Never returns. Waits for a card, reads autorun.cfg, and runs the chain. */
void neos_boot(void);

/**
 * Ask NeOS to run @p dir after the current app returns.
 *
 * Exported to apps. It only records the request - the app stays in control
 * until it returns from main(), and only one app is ever resident, so a
 * launcher hands over by calling this and then returning.
 */
void neos_exec(const char *dir);

/**
 * Ask the running app to close.
 *
 * For whatever ends up owning input - the bar close button, a gesture, a
 * timeout. The app decides when to act on it.
 */
void neos_app_request_close(void);

/**
 * Launch @p dir on somebody else's behalf: neos_exec() plus a close request.
 *
 * This is the console's way in (@NEOSRUN, see neos_upload.c), and it is what
 * an app's own neos_exec() is not: the caller is not the running app, so the
 * running app has to be asked to go before the request can be honoured. An
 * empty or NULL @p dir means "back to the card's autorun app" - just the
 * close, with nothing queued behind it.
 *
 * Checks what it can up front - the name, the card, the manifest - so the
 * caller gets an answer rather than the tablet flashing a message screen at
 * nobody. False with a reason in @p err if any of that fails; true means the
 * request is queued, not that the app has started, because it cannot start
 * until whatever is running now returns.
 */
bool neos_launch_request(const char *dir, char *err, size_t err_sz);
