/*
 * The NeOS ABI.
 *
 * Every symbol declared here is exported to loaded apps through the syscall
 * table in neos_syscalls.c. Both the firmware and the apps compile against
 * this one header: an app that links against a struct laid out differently
 * from the firmware's would not fail to load, it would read garbage.
 *
 * Additions are cheap: a new symbol here is a NEOS_ABI_MINOR bump and older
 * apps carry on running. Changing the meaning or the layout of anything below
 * is not - see neos_abi.h for where that line falls and how it is enforced.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ngl.h"
#include "neos_abi.h"

/* ------------------------------------------------------------------ */
/* Misc                                                                */
/* ------------------------------------------------------------------ */

int  neos_log(const char *msg);
int  neos_add(int a, int b);

/** Yield for @p ms. The only way an app can idle without burning the core. */
void neos_sleep_ms(uint32_t ms);

/* ------------------------------------------------------------------ */
/* The boot chain                                                      */
/* ------------------------------------------------------------------ */

/**
 * Ask NeOS to run @p dir once this app returns from main().
 *
 * One app is resident at a time, so this does not start anything: it records
 * the request, and the handover happens when you return. Returning without
 * calling it brings the card's autorun app back instead.
 */
void neos_exec(const char *dir);

/**
 * True once something has asked the running app to close.
 *
 * An app that stays up - a shell, or anything with its own event loop - polls
 * this and returns from main() when it goes true. There is no way to kill an
 * app from outside: it runs on the caller stack as ordinary code, so exiting
 * is something the app does, not something done to it.
 *
 * NeOS clears the flag before each app starts, so a close never carries over.
 */
bool neos_app_close_requested(void);

/**
 * The directory this app was loaded from, which is also its id in the
 * registry - "launcher", "hello".
 *
 * argv[0] is the display name out of the manifest and two apps may well
 * share one, so this is the only thing an app can match against a
 * neos_app_t. A shell needs it to leave itself out of its own list.
 */
const char *neos_app_self(void);

/* ------------------------------------------------------------------ */
/* The card                                                            */
/* ------------------------------------------------------------------ */

/**
 * Write a whole file to the card.
 *
 * @param rel   path relative to the card root, e.g. "album/mandel.png".
 *              Missing directories along it are created. It may not begin
 *              with '/' or contain "..", so an app cannot write outside the
 *              card however it was asked to.
 * @param data  the finished file
 * @param len   its length
 * @return 0 on success, negative on failure. Nothing is left behind on
 *         failure - a partial file is deleted rather than kept.
 *
 * One call per file, and no handle crosses the boundary. There is no
 * neos_file_open on purpose: the card can be pulled at any moment on this
 * machine, and an app holding an open FILE* across that is a half-written
 * file plus a handle into a dead filesystem. NeOS keeps the card.
 */
int neos_file_write(const char *rel, const void *data, size_t len);

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t x, y;   /**< screen coordinates, rotation already applied */
    bool    down;   /**< a finger is on the glass right now */
} neos_touch_t;

/**
 * Where the finger is now. False if the tablet has no working touch panel,
 * in which case @p out is untouched - an app that only ever polls this must
 * still be usable, or it is unusable on a broken panel.
 */
bool neos_touch(neos_touch_t *out);

/**
 * Collect one completed tap, if there is one waiting.
 *
 * A tap is a press and release that did not travel far. NeOS does the edge
 * detection so that every app agrees on what a tap is, and delivers each one
 * exactly once - the second caller gets false.
 *
 * A tap on the system bar never arrives here: those belong to NeOS - the close
 * button, the Wi-Fi icon, the clock - and are consumed before an app can see
 * them, let alone swallow them.
 */
bool neos_touch_tap(int16_t *x, int16_t *y);

/** How many fingers NeOS tracks at once. */
#define NEOS_TOUCH_MAX 5

/**
 * Every finger on the glass, in screen coordinates.
 *
 * Returns the number touching, which may exceed @p max - the array fills to
 * max and the count is still the truth, the same convention as
 * neos_i2c_scan(). Each point that is written has `down` set; a finger that is
 * not there does not appear, so there are no gaps to skip.
 *
 * Order is whatever the controller reports and is not a finger identity: a
 * point does not stay at the same index across calls, and lifting one finger
 * can renumber the rest. An app that wants to know a particular contact has
 * moved has to match by position itself.
 */
int neos_touch_points(neos_touch_t *out, int max);

/* ------------------------------------------------------------------ */
/* Text input                                                          */
/* ------------------------------------------------------------------ */

/** Mask the field and do not echo what is typed. For passwords. */
#define NEOS_INPUT_SECRET  0x01u

/**
 * Put the system keyboard up and wait for a line of text.
 *
 * Blocking, and blocking is the feature. The app stops inside this call, which
 * is what makes the keyboard modal without NeOS having to suspend anything:
 * one app is resident and it is the one that stopped, so nothing else is
 * drawing. What was on screen is saved before the keyboard appears and put
 * back before this returns, so an app gets its own frame back and never has to
 * know a panel was over it.
 *
 * @param title  what the field is for, shown above it. May be NULL.
 * @param buf    seeded with the current value and overwritten with the new
 *               one. Left untouched if the user cancels.
 * @param size   sizeof(buf), including the terminator.
 * @param flags  NEOS_INPUT_* or zero.
 *
 * @return true if the user confirmed, false if they cancelled or the keyboard
 *         could not open. False means @p buf still holds what it did.
 */
bool neos_input_text(const char *title, char *buf, size_t size, uint32_t flags);

/**
 * True while a system panel - the keyboard, the Wi-Fi list, the clock page -
 * is over the app.
 *
 * An app does not need this to be correct: its draws are dropped and its taps
 * withheld for the duration either way. It is here so that an app which is
 * doing something expensive per frame can stop doing it at something nobody
 * can see, and so that one which measures its own frame rate does not report a
 * stall it did not cause.
 */
bool neos_ui_busy(void);

/* ------------------------------------------------------------------ */
/* The app registry                                                    */
/* ------------------------------------------------------------------ */

#define NEOS_APPS_MAX 12

typedef struct {
    char     dir[32];      /**< directory under /apps, and the app's id */
    char     name[48];     /**< display name from the manifest */
    char     desc[96];     /**< one-line description, may be empty */
    char     entry[64];    /**< the .elf to load */
    uint32_t crashes;      /**< non-zero: quarantined, will not be run */
    bool     ok;           /**< manifest was readable and complete */
} neos_app_t;

/** Re-walk the card. Returns how many apps are on it. */
int neos_apps_scan(void);

/** How many apps the last scan found. */
int neos_apps_count(void);

/** App by index, or NULL past the end. */
const neos_app_t *neos_apps_get(int idx);

/** App by directory name, or NULL if the card has no such app. */
const neos_app_t *neos_apps_find(const char *dir);

/**
 * Bumped every time the registry is rebuilt.
 *
 * An app that draws a list polls this and redraws when it moves. That is how
 * a freshly uploaded app appears without anything having to be restarted -
 * NeOS rescans after writing it, and the shell notices on its next tick.
 */
uint32_t neos_apps_generation(void);

/*
 * neos_app_t crosses the boundary as a pointer, but apps read its fields, so
 * the offsets are compiled into them. Widening one of these arrays moves
 * everything after it: that is a NEOS_ABI_MAJOR bump, and this is where you
 * find out. Appending a field is fine - nothing shifts - and is a minor.
 */
_Static_assert(offsetof(neos_app_t, dir)     ==   0, "neos_app_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_app_t, name)    ==  32, "neos_app_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_app_t, desc)    ==  80, "neos_app_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_app_t, entry)   == 176, "neos_app_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_app_t, crashes) == 240, "neos_app_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_app_t, ok)      == 244, "neos_app_t layout is frozen for ABI v1");
