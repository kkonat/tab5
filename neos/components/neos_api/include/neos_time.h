/*
 * What time it is.
 *
 * Three clocks, and it matters which is which.
 *
 *   The RX8130 is the clock of last resort - battery backed, still right after
 *   a week in a drawer, and it holds *local* time. That is deliberate. It is
 *   what neos_rtc_read() has always returned and what the system readout has
 *   always shown, and a chip whose registers say something other than the time
 *   a person in the room would say is a chip that will eventually be read by
 *   something that does not know the difference.
 *
 *   The system clock holds UTC, because that is what a system clock is and
 *   what time() means everywhere else. It is seeded from the RTC at boot and
 *   corrected by SNTP if a network ever turns up.
 *
 *   Local time is the system clock plus an offset. Not a time zone database:
 *   the offset and the daylight-saving hour are two things the owner sets by
 *   hand on the clock page, because a tablet with no network for weeks at a
 *   time cannot be relied on to know that a rule changed, and a wrong rule is
 *   worse than a number somebody chose.
 *
 * Everything below trades in whole minutes and in neos_rtc_t. No time_t
 * crosses to an app: apps link -nostdlib against a syscall table with no
 * compiler runtime in it, and 64-bit arithmetic on a time_t is a link error
 * against __divdi3 rather than a slow instruction.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "neos_sys.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Restore the saved zone and seed the system clock from the RTC.
 *
 * Called once by NeOS during bring-up, after the RTC has been probed and the
 * settings store is open. Not exported to apps - by the time one runs, the
 * machine already knows what time it is or knows that it does not.
 */
void neos_time_init(void);

/* ------------------------------------------------------------------ */
/* Reading it                                                          */
/* ------------------------------------------------------------------ */

/**
 * Local wall time, from whichever source is better.
 *
 * The system clock once it has been set - from the RTC at boot, or from the
 * network since - because it counts in software and does not cost an I2C
 * transaction per read, which matters to a clock in the system bar that is
 * asked once a second forever.
 *
 * False means the machine genuinely does not know: no RTC, or an RTC that has
 * never been set. Show a dash, not a wrong time.
 */
bool neos_time_local(neos_rtc_t *out);

/** The same instant in UTC. */
bool neos_time_utc(neos_rtc_t *out);

/** True once the network has set the clock this boot. */
bool neos_time_synced(void);

/**
 * How long ago that was, in seconds, or 0 if it has not happened.
 *
 * The clock page shows it because "synced" on its own is not the useful fact -
 * an SNTP sync three days ago and one three seconds ago are both "synced", and
 * only one of them explains a clock that is drifting.
 */
uint32_t neos_time_since_sync_s(void);

/* ------------------------------------------------------------------ */
/* The zone                                                            */
/* ------------------------------------------------------------------ */

/**
 * Standard-time offset from UTC, in minutes. Negative west of Greenwich.
 *
 * Minutes rather than hours because a whole hour is not universal - India is
 * +330, Nepal +345, and a UI that could only express hours would be wrong in
 * places rather than merely inconvenient.
 */
int  neos_tz_offset_min(void);

/** Whether the daylight-saving hour is currently being added. */
bool neos_tz_dst(void);

/** Standard offset plus the daylight hour: what local time actually is. */
int  neos_tz_total_min(void);

/*
 * Setting either one moves the wall clock, so both rewrite the RTC.
 *
 * The instant does not change - it is the same moment, described differently -
 * so the system clock is left exactly where it is and only the chip that
 * stores local time is corrected. Both are saved and come back after a reboot.
 */
void neos_tz_offset_set(int minutes);
void neos_tz_dst_set(bool on);

/**
 * Set the clock from a UTC instant, in seconds since 1970.
 *
 * What SNTP calls when it lands. Sets the system clock, converts to local, and
 * writes the RTC so the correction survives a power cut.
 */
void neos_time_set_utc(int64_t epoch_s);

/**
 * Set the clock from a local wall time the user typed.
 *
 * The inverse of the above and the reason the clock page can set a time at all
 * on a tablet that has never seen a network.
 */
bool neos_time_set_local(const neos_rtc_t *t);

#ifdef __cplusplus
}
#endif
