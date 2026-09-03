/*
 * Crash attribution across reboots.
 *
 * A breadcrumb written before an app runs and erased after it returns, plus a
 * per-app crash counter. Both live in NVS - see neos_crash.c for why not RTC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Note that @p app is about to be entered. */
void neos_crumb_enter(const char *app);

/** Note that the app returned. */
void neos_crumb_leave(void);

/** Read the breadcrumb and clear it. True if the previous run left one. */
bool neos_crumb_take(char *out, size_t out_sz);

/** How many times @p app has crashed. Non-zero means quarantined. */
uint32_t neos_quarantine_count(const char *app);

/** Forget @p app has ever crashed. A clean run earns this. */
void neos_quarantine_clear(const char *app);

/** Record a crash against @p app. */
void neos_quarantine_add(const char *app);

/**
 * Remember which app the previous boot died inside, without judging it yet.
 */
void neos_crash_note(const char *app, bool was_a_fault);

/**
 * Decide what that crash meant, now that the card has named its shell.
 *
 * Anything but the shell is quarantined. The shell is not: it is the app
 * that never returns, so its breadcrumb is still on flash after every normal
 * power-off - and the power button is the documented way out of a wedged
 * tablet. Quarantining on that would make one deliberate power-cycle enough
 * to stop the card ever booting again.
 */
void neos_crash_settle(const char *autorun_dir);
