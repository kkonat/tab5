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

/**
 * Read a whole file back off the card.
 *
 * @param rel   path relative to the card root, as for neos_file_write().
 * @param buf   filled with the file
 * @param size  how much room @p buf has
 * @return how many bytes were placed in @p buf, or negative on failure.
 *         -3 means there is no such file, which is the ordinary answer the
 *         first time an app looks for its own settings and not an error worth
 *         a message.
 *
 * A file longer than @p size is refused rather than truncated: half a settings
 * file parses as well as a whole one and then means something else, so the
 * caller is told its buffer was too small instead of being handed a prefix.
 *
 * The mirror of neos_file_write(), and no handle crosses here either - the
 * file is opened, read and closed inside the call, for the reason in that
 * function's comment.
 */
int neos_file_read(const char *rel, void *buf, size_t size);

/**
 * How long a file on the card is, in bytes.
 *
 * @return the length, or negative on failure with the same codes
 *         neos_file_read() uses - -3 for no such file.
 *
 * Exists because the alternative is what apps were doing without it: offer
 * neos_file_read() a buffer, be told -6, offer twice as much, and repeat.
 * That costs an allocation and a stat per attempt and has to carry a ceiling
 * chosen by guesswork, which is a decision about how large a file may be made
 * in the wrong place. Ask first instead.
 *
 * The answer can be stale by the time it is used - the card is removable and
 * nothing here holds it - so neos_file_read() still refuses a buffer that
 * turned out too small rather than trusting this.
 */
int neos_file_size(const char *rel);

/**
 * Read @p len bytes from @p off in a file on the card.
 *
 * @param rel   path relative to the card root, as for neos_file_read().
 * @param buf   filled with what was read
 * @param len   how much to read
 * @param off   where to start
 * @return how many bytes were placed in @p buf, which is short of @p len only
 *         at end of file, or negative on failure with neos_file_read()'s codes.
 *
 * The whole-file call is the right one for a settings file and the wrong one
 * for a pack with several members in it: reading one member means holding the
 * entire file somewhere first, so the large copy and the small one it is
 * cut down to are both live at once, and the large one lands in PSRAM. With
 * an offset each member is read straight to where it will be used.
 *
 * Still no handle: the file is opened, sought, read and closed inside the
 * call, so a card pulled between two of these costs the second read and
 * nothing else. That is the same trade neos_file_read() makes and the reason
 * is in neos_file_write()'s comment - it just costs an open per member here,
 * which is worth it against keeping a FILE* alive across app code.
 */
int neos_file_read_at(const char *rel, void *buf, size_t len, uint32_t off);

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
/* Game mode                                                           */
/* ------------------------------------------------------------------ */

/**
 * Take the whole panel: no system bar, and no close button but the app's own.
 *
 * For something that is a picture rather than a screenful of controls - a
 * game, a viewer, an emulator - where 56 px along the top is both a tenth of
 * the height and a piece of another program's furniture sitting on top of
 * yours. ngl_app_area() becomes the whole panel, and nothing is clipped away
 * from the top any more.
 *
 * What the app takes on with it is the way out. The bar's close button is the
 * one control that is in the same place in every app, and this removes it, so
 * an app that turns this on and does not draw its own has made itself
 * unleavable. Draw the button first, then ask for the mode.
 *
 * Also switched off: the status line and the toasts, which have nowhere to go
 * without a bar. neos_status_for() keeps working and does nothing, so an app
 * does not have to guard every call.
 *
 * NeOS keeps a way back that costs no screen at all - four fingers held on the
 * glass for a moment restores the bar and asks the app to close. That is the
 * fullscreen equivalent of the close button and not a debugging aid: the touch
 * task sees it before the app does, so an app cannot swallow it. It cannot
 * rescue an app that has stopped polling neos_app_close_requested() - nothing
 * can, since an app runs as ordinary code on the boot task - but it does cover
 * the case this mode makes newly possible, which is a close button that was
 * never drawn or was drawn somewhere unreachable.
 *
 * Process-wide state held on the app's behalf, like the orientation lock, so
 * NeOS puts the bar back when the app returns however it returns. An app that
 * crashes fullscreen does not leave the next one without a close button.
 *
 * @return true if the mode is now what was asked for.
 */
bool neos_fullscreen(bool on);

/** Whether the panel currently belongs to the app. */
bool neos_is_fullscreen(void);

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
    /*
     * Which shelf the shell files this under - "Games", "Tools". Empty when
     * the manifest does not say, which is not an error: what an unclassified
     * app is called and where it goes is the shell's business, not the
     * registry's, so nothing is invented here.
     *
     * Appended after `ok` rather than sorted in next to `desc`, because every
     * offset above it is compiled into apps already on the card. See the
     * assertions below.
     */
    char     category[24];
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
 * Let @p dir run again after a crash quarantined it. True if it was.
 *
 * Quarantine is a safety net and not a verdict: NeOS counts a fault against
 * whichever app the previous boot died inside, and refuses to launch it again,
 * because an app that panics on its first frame would otherwise be picked up
 * by autorun and panic again forever. What it cannot know is whether the fault
 * has since been fixed - the usual case is exactly that, an app that crashed
 * once and has been rebuilt and uploaded over the top of itself.
 *
 * So there has to be a way back, and it belongs to whoever is holding the
 * tablet rather than to a rebuild: the same authority the close button has.
 * The shell puts it behind a deliberate gesture so that it cannot be the
 * accident that undoes the safety net.
 *
 * Clears the counter and updates the registry in place - no card walk, but
 * neos_apps_generation() moves, so a shell that watches it redraws the card
 * with its bomb replaced without being told anything else.
 *
 * A clean run clears the counter by itself, so an app let back out and then
 * behaving needs nothing further. One that crashes again is quarantined again.
 */
bool neos_apps_unquarantine(const char *dir);

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
/* Appended at 1.15. Everything above it kept its offset, which is what makes
   that a minor rather than a major. */
_Static_assert(offsetof(neos_app_t, category) == 245, "neos_app_t layout is frozen for ABI v1");
