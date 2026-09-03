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
