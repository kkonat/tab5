/*
 * Boot-time message screens.
 *
 * These exist because of an ordering problem: the launcher lives on the card,
 * so every reason NeOS cannot reach the launcher is a reason the launcher
 * cannot report it. Core therefore owns a small full-screen message of its
 * own - no system bar, no status line, nothing that belongs to an app.
 */
#pragma once

typedef enum {
    NEOS_MSG_WAIT,     /**< waiting on something the user can fix */
    NEOS_MSG_MISSING,  /**< something that should be on the card is not */
    NEOS_MSG_BAD,      /**< found it, could not use it */
} neos_msg_kind_t;

/** Take over the screen with one message. Safe before any app has loaded. */
void neos_msg(neos_msg_kind_t kind, const char *title, const char *detail);

/** Redraw whatever message is showing. Called when the screen rotates. */
void neos_msg_repaint(void);

/** Note that an app has the screen now, so rotations are its problem. */
void neos_msg_none(void);
