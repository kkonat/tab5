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
/* System panels                                                       */
/* ------------------------------------------------------------------ */

/*
 * The pages behind the system bar's icons, which an app may also raise.
 *
 * Everything on them is NeOS's - the radio, the clock, the pack - so there was
 * never an app-side version to write, and the bar is one tap away from every
 * screen anyway. What the bar cannot do is be somewhere else: a settings page
 * with a row of power readings on it is exactly where "and the rest of it"
 * belongs, and sending the user off to hunt for an icon in the corner instead
 * is how a row of numbers becomes a dead end.
 *
 * The numbers are ABI - an app names one - so append, never renumber.
 */
typedef enum {
    NEOS_SYSPANEL_WIFI    = 1,
    NEOS_SYSPANEL_CLOCK   = 2,
    NEOS_SYSPANEL_BATTERY = 3,
} neos_syspanel_t;

/**
 * Raise a system panel over this app.
 *
 * Returns at once: the panel runs on NeOS's own task and the app carries on
 * running underneath it, with its draws dropped and its taps withheld for the
 * duration, exactly as when the panel was opened from the bar. So an app that
 * calls this keeps polling its own loop and finds out the panel has gone by
 * neos_ui_busy() going false - there is nothing to wait for and nothing to
 * clean up.
 *
 * Ignored while a panel is already up. There is one screen.
 */
void neos_syspanel_open(neos_syspanel_t p);

/* ------------------------------------------------------------------ */
/* The app registry                                                    */
/* ------------------------------------------------------------------ */

/*
 * The most apps one card may hold.
 *
 * Generous rather than measured: the registry is 272 bytes an entry and is
 * allocated once, from PSRAM, at the first scan - see neos_apps.c - so a cap
 * nobody comes near costs 17 KB of the 32 MB there and nothing at all of the
 * internal pool NeOS and the radio share. Held as a static array this would
 * have been the other way round, and the figure would have had to be argued
 * over rather than simply set out of reach.
 *
 * Raising it is safe in both directions and needs no ABI bump. An app built
 * against a smaller value asks for fewer entries and sees fewer apps; one
 * built against a larger value asks past the end and gets NULL from
 * neos_apps_get(), which is what it gets for any index past the count anyway.
 * Only the shell notices, and only by rebuilding.
 */
#define NEOS_APPS_MAX 64

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

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */

/*
 * What the machine is actually running, which is a great deal more than the
 * app is.
 *
 * An app is one function on the boot task, and everything else on this tablet -
 * touch, the bar, the radio, the SDIO transport underneath it, a background
 * service some earlier app left behind - is a task it never sees. That is fine
 * until something is eating the CPU or the RAM and the question is what, at
 * which point a list of tasks is the only answer there is.
 *
 * Read-only apart from neos_task_kill(), and that one refuses the tasks NeOS
 * needs - see below.
 */

/** Longest task name FreeRTOS keeps, including the terminator. */
#define NEOS_TASK_NAME 16

/** How many tasks neos_tasks() will describe in one call. */
#define NEOS_TASKS_MAX 48

typedef enum {
    NEOS_TASK_RUNNING = 0,
    NEOS_TASK_READY,
    NEOS_TASK_BLOCKED,
    NEOS_TASK_SUSPENDED,
    NEOS_TASK_DELETED,
    NEOS_TASK_UNKNOWN,
} neos_task_state_t;

/** NeOS needs this one: neos_task_kill() will refuse it. */
#define NEOS_TASK_PROTECTED 0x01u
/** Started by an app through neos_service_start(), and outliving it. */
#define NEOS_TASK_SERVICE   0x02u

typedef struct {
    /*
     * NeOS's own handle for the task, not FreeRTOS's.
     *
     * A handle is an address and addresses come back: a task that has exited
     * frees its control block, the next task to start may be handed the same
     * memory, and a list drawn a second ago would then have a row pointing at
     * a task nobody asked about. Ids come from a counter and are never reused,
     * so the id in a row that has gone stale matches nothing and
     * neos_task_kill() refuses it instead of killing whatever took its place.
     */
    uint32_t id;
    char     name[NEOS_TASK_NAME];
    uint8_t  state;             /**< neos_task_state_t */
    uint8_t  prio;
    int8_t   core;              /**< -1 when the task is not pinned */
    uint8_t  flags;             /**< NEOS_TASK_* */
    uint32_t stack_free;        /**< low-water mark in bytes: the worst it ever got */
    uint16_t cpu_permille;      /**< share of one core, 0-1000, over the last second */
    uint16_t reserved;
} neos_task_t;

/**
 * Describe every task in the system, oldest first.
 *
 * Returns how many were written, at most @p max. The order is the order the
 * tasks were created in and does not change while they live, which is what
 * makes a list with a kill button on each row safe to tap: sorting by anything
 * that moves - CPU, state - is how a tap lands on a different task from the one
 * it was aimed at.
 *
 * cpu_permille is measured over about a second, recomputed by whichever call
 * first crosses that boundary. Calling this ten times a second is not ten times
 * the cost and does not make the figure ten times as jumpy - the nine
 * intervening calls report the same window.
 */
int neos_tasks(neos_task_t *out, int max);

/** "running", "ready", "blocked", "suspended", "deleted", "?". */
const char *neos_task_state_name(uint8_t state);

/**
 * End a task.
 *
 * @return true if the task is gone. False if @p id names nothing - a row that
 *         has gone stale - or names a task carrying NEOS_TASK_PROTECTED.
 *
 * Protected is not a policy, it is the difference between a tablet and a brick:
 * the idle tasks, the timer task, the TCP/IP stack, touch, the panel runner,
 * and the task the calling app is itself running on. Killing any of those ends
 * the session with nothing on screen to say why, and no list is worth that.
 *
 * A service (NEOS_TASK_SERVICE) is asked before it is killed: its stop flag is
 * set, it gets half a second to notice through neos_service_stopping() and
 * return, and only then is it deleted outright. Everything else is deleted
 * outright immediately, because nothing else agreed to be asked.
 *
 * Deleting a task outright leaks whatever it was holding - its heap, its files,
 * any mutex it was inside - and FreeRTOS cannot do otherwise. That is the
 * honest price of the button, and the reason the protected list exists.
 */
bool neos_task_kill(uint32_t id);

/* ------------------------------------------------------------------ */
/* Background services                                                 */
/* ------------------------------------------------------------------ */

/*
 * One app is resident, and some of what an app does should not stop when it
 * stops being the app you are looking at.
 *
 * A player is the case that forces it. Nothing about pulling a stream off the
 * network and feeding the codec needs the screen, and a player that falls
 * silent the moment you go and look at something else is not a player. So an
 * app can leave a task behind: a function of its own that keeps running after
 * main() returns, through the next app and the one after that, until it returns
 * or it is stopped.
 *
 * What makes that possible is that NeOS keeps the app's image loaded for as
 * long as one of its services is running. The service is ordinary app code at
 * the address it was relocated to; freeing the image under it would be a jump
 * into whatever the allocator did with those pages next. The image is freed
 * when the last service from it exits, so a player left running costs its stack
 * and its few hundred kilobytes of text, and a player that has finished costs
 * nothing.
 *
 * What a service must not do is draw. The screen belongs to whichever app is
 * resident, which is no longer this one, and a service that paints into it is
 * painting over somebody else's window. Neither the surface nor the tap queue
 * is fenced off - this is one address space and there is no way to fence them -
 * so that is a rule rather than an enforcement. Sound, the network and the card
 * are all fine; anything the user has to see belongs in the app.
 */

/** How many services can run at once, across all apps. */
#define NEOS_SERVICES_MAX 4

/** A service body. It ends by returning, and should return when asked. */
typedef void (*neos_service_fn)(void *arg);

/**
 * Start @p fn on a task of its own, and keep this app's image loaded for as
 * long as it runs.
 *
 * @param name   what the task is called in a list of tasks. Truncated to
 *               NEOS_TASK_NAME-1, and prefixed with nothing: the app's own name
 *               is worth putting in it.
 * @param fn     the body. Called once with @p arg; the service ends when it
 *               returns.
 * @param arg    passed through untouched. It has to outlive the app, so it must
 *               not point into main()'s frame - static storage, or something
 *               malloc'd, is what this is for.
 * @param stack  stack bytes, or 0 for a sensible default. A service that
 *               touches the network or the card wants several kilobytes.
 *
 * @return the task id neos_tasks() reports for it, or 0 if it could not be
 *         started - no free slot, no memory, or NeOS could not take on the
 *         image.
 *
 * Started at a priority above the app and below input, so an app that
 * busy-waits cannot starve a player and a player cannot make the glass feel
 * slow.
 */
uint32_t neos_service_start(const char *name, neos_service_fn fn, void *arg,
                            uint32_t stack);

/**
 * True once something has asked the calling service to stop.
 *
 * A service polls it the way an app polls neos_app_close_requested(), and for
 * the same reason: a task running ordinary code cannot be stopped from outside
 * without abandoning whatever it was holding. Returning promptly is the whole
 * difference between a service that is stopped and one that is killed.
 *
 * False anywhere but inside a service, so shared code can call it safely.
 */
bool neos_service_stopping(void);

/** How many services are running - this app's, and any other app's. */
int neos_service_count(void);

/* ------------------------------------------------------------------ */
/* The screen, and the idle timer that turns it off                     */
/* ------------------------------------------------------------------ */

/*
 * Sleep on this machine is the screen going off, and nothing else stopping.
 *
 * The panel and its backlight are most of what the tablet spends, and they are
 * also the only part that can be switched off and on again with nothing
 * depending on the timing - so that is what sleeping is here. Tasks keep
 * running, which is the point: a player left behind by neos_service_start()
 * plays through it, the clock stays right, the pack keeps being recorded, and
 * the network stays joined.
 *
 * What it is not is a lower clock. The PSRAM runs at 200 MHz and the display
 * scans out of it over MIPI-DSI; dropping the CPU frequency under that moves
 * both those timings, and a tablet that wakes up to a corrupt panel is worse
 * than one that idles a little warm. Where polling can be slowed it is - touch
 * drops to 8 Hz and the bar stops repainting - and that is the whole of the CPU
 * saving.
 *
 * Any touch wakes it, and the touch that wakes it is swallowed rather than
 * delivered: the first thing you do to a sleeping tablet is not a button press.
 * So does picking it up, which is the accelerometer rather than the glass.
 */

/** The longest wait the setting allows, in minutes. */
#define NEOS_IDLE_MAX_MIN 10

/** Turn the screen off now. Everything else keeps running. */
void neos_screen_off(void);

/** Turn it back on, as a touch would. */
void neos_screen_on(void);

/** Whether the screen is currently off. */
bool neos_screen_is_off(void);

/**
 * Tell the idle timer the user is still there.
 *
 * Touch does this by itself, so most apps never need it. What needs it is an
 * app that is watched rather than used - a slideshow, a plot, a game played by
 * tilting the tablet rather than tapping it - which would otherwise be switched
 * off mid-frame by a timer measuring the one thing it is not doing.
 *
 * Wakes the screen if it is off, which also makes this how an app says "look at
 * this now": an alarm, a timer that has finished, a message that has arrived.
 */
void neos_idle_poke(void);

/**
 * How long the tablet waits before turning the screen off, in minutes. Zero
 * means never, which is what a tablet on a desk with a dashboard on it wants.
 */
int neos_idle_timeout_min(void);

/**
 * Set it, 0 to NEOS_IDLE_MAX_MIN. Saved, like the backlight: it is the
 * machine's setting and not the app's, and a tablet that forgot it over a
 * reboot would switch its screen off at a time nobody chose. False if the value
 * is out of range.
 */
bool neos_idle_timeout_set(int minutes);

/*
 * neos_task_t is filled by the firmware into storage the app owns, so its
 * layout is compiled into every app that reads one. Same rule as neos_app_t:
 * appending a field is a minor, moving one is a major, and this is where you
 * find out rather than in a wrong number on a list.
 */
_Static_assert(sizeof(neos_task_t) == 32, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, id)           ==  0, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, name)         ==  4, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, state)        == 20, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, prio)         == 21, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, core)         == 22, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, flags)        == 23, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, stack_free)   == 24, "neos_task_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_task_t, cpu_permille) == 28, "neos_task_t layout is frozen for ABI v1");
