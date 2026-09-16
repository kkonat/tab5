/*
 * The system bar, drawn by NeOS. See neos_bar.c for why it is not the shell's.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ngl.h"
#include "neos_sys.h"   /* neos_power_t, for the battery helpers below */

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

/**
 * What is under (x, y) on the bar, if anything.
 *
 * The bar's controls belong to NeOS: the touch driver asks this on every tap
 * and acts on the answer itself, so an app never sees the gesture that closes
 * it or the one that opens the Wi-Fi list. The numbers carry no meaning beyond
 * this file and the switch in neos_touch.c.
 */
typedef enum {
    NEOS_BAR_NONE = 0,
    NEOS_BAR_CLOSE,
    NEOS_BAR_WIFI,
    NEOS_BAR_CLOCK,
    NEOS_BAR_BATTERY,
} neos_bar_hit_t;

neos_bar_hit_t neos_bar_hit(int16_t x, int16_t y);

/*
 * 2S LiPo pack thresholds, read off neos_power_read()'s bus_mv - the INA226
 * sits across the main rail, which on this board is the battery.
 *
 * WARN is 3.30 V/cell: still safe to keep going, but worth a colour change.
 * DANGER is 3.00 V/cell: the point past which further discharge risks
 * permanently damaging the cells, which is what the flashing is for.
 *
 * Shared with neos_panel_battery.c so the detail panel and the bar icon never
 * disagree about where the lines are.
 */
#define NEOS_BATTERY_2S_WARN_MV   6600
#define NEOS_BATTERY_2S_DANGER_MV 6000

/*
 * Below this there is no pack on the terminals.
 *
 * Pull the battery and the monitor does not stop answering - it reports about
 * 1.88 V off whatever is left holding the rail up. That is not a flat battery
 * and must not be treated as one: it is under the danger line, so the icon
 * would sit there flashing red about a pack that is not there, and it would go
 * into the history as a cliff that never happened.
 *
 * 4.5 V is 2.25 V/cell. A 2S pack is destroyed well before it gets there and
 * its protection circuit opens long before that, so nothing this side of the
 * line is a battery worth plotting, and everything above it is.
 */
#define NEOS_BATTERY_MIN_MV 4500

/*
 * The recorded pack voltage, for the detail panel's plot.
 *
 * Kept here and not in the panel because the bar is what already reads the
 * monitor every tick, and a second sampler would mean two readings a second of
 * the same register and two answers that could disagree. It also has to outlive
 * the panel: a plot that only started recording when you opened it would be
 * empty at the exact moment you went looking for it.
 *
 * Two days at ten seconds a point. The period is a multiple of the bar's own
 * 500 ms tick, so a point is the mean of the twenty readings inside it rather
 * than whichever one the clock happened to land on - the pack sags under load
 * and an instantaneous sample plots that as a spike in the trend.
 *
 * Both numbers are chosen against what gets looked at rather than against what
 * is cheap. Ten seconds is fine enough that zooming in shows the load steps a
 * backlight makes, which is the resolution at which "is it charging" stops being
 * a guess; two days is long enough to hold a whole discharge and the charge
 * after it. That is 17280 points and 34 KB, which is why the ring is allocated
 * out of PSRAM rather than declared - 34 KB of internal RAM is a tenth of what
 * this board has left by the time the scheduler starts.
 */
#define NEOS_BATTERY_HIST_HOURS     48
#define NEOS_BATTERY_HIST_PERIOD_MS 10000
#define NEOS_BATTERY_HIST_N \
    ((NEOS_BATTERY_HIST_HOURS) * 3600 * 1000 / (NEOS_BATTERY_HIST_PERIOD_MS))

/*
 * Which way the current flows, measured on this board rather than assumed.
 *
 * The shunt is in series with the pack and not with the system load - a
 * running tablet draws hundreds of mA and the monitor reads about one while a
 * charger is plugged in, which is only possible if what it is watching is the
 * battery itself. So the current is the pack's own, and positive is out of it:
 * unplugged and running, this board reads about +200 mA.
 *
 * The idle band is the third state and not a rounding error. A charger that is
 * carrying the system while the pack sits untouched reads near zero, which is
 * neither charging nor running on battery, and saying "charging" there would
 * be a claim the hardware has not made.
 */
#define NEOS_BATTERY_IDLE_MA 20

/*
 * Pack internal resistance, milliohms, for load compensation.
 *
 * A pack under load reads lower than it is, so a percentage taken straight off
 * the terminals drops every time the screen brightens and climbs back when it
 * dims. Adding I*R back gets the open-circuit voltage the charge curve is
 * defined against. At this board's currents the correction is tens of
 * millivolts - small, but it is the difference between a figure that drifts
 * with the backlight and one that does not.
 *
 * Not measured off this board either. If the percentage jumps when a heavy
 * load starts, this is the number to correct.
 */
#define NEOS_BATTERY_R_MOHM 150

typedef enum {
    NEOS_PWR_UNKNOWN = 0,
    NEOS_PWR_BATTERY,      /* the pack is running the tablet */
    NEOS_PWR_EXTERNAL,     /* something else is, and the pack is idle */
    NEOS_PWR_CHARGING,     /* ... and the pack is taking current */
} neos_power_src_t;

/** Where the tablet is getting its power, from the last reading. */
neos_power_src_t neos_battery_source(void);

/**
 * Percent charge for an open-circuit pack voltage.
 *
 * Zero is NEOS_BATTERY_2S_DANGER_MV, not a flat pack: the thresholds this file
 * already defines are the ones the icon and the status line use, and a
 * percentage that hit zero somewhere else would be a second opinion about when
 * the battery is empty. It follows that the warning line lands around ten
 * percent and that what is below zero is reserve, which is what it is for.
 */
int neos_battery_soc_mv(int32_t open_mv);

/** The same, load-corrected, for a live reading. */
int neos_battery_soc(const neos_power_t *p);

/**
 * Whether the power monitor itself answered.
 *
 * Separate from whether there is a battery, so the panel can tell "this board
 * has no INA226" apart from "the pack has been taken out" - they are different
 * problems and the same blank reading.
 */
bool neos_battery_monitor_ok(void);

/**
 * The pack's measured capacity in mAh, or 0 if it has not been learned.
 *
 * Learned by watching a discharge, not configured: charge out of the pack is
 * integrated while the percentage falls, and capacity is what those two divide
 * into. It needs a stretch of real discharging to produce a figure and it gets
 * steadier the more of them it sees. Persisted, so the learning is not thrown
 * away at every reboot.
 */
int neos_battery_capacity_mah(void);

/** How many points have been recorded so far, at most NEOS_BATTERY_HIST_N. */
int neos_battery_history_count(void);

/**
 * Read a window of the record, averaged down to @p cols buckets.
 *
 * @param out    filled with @p cols means, in mV, oldest first.
 * @param cols   how many buckets to divide the window into.
 * @param first  index of the oldest sample in the window, 0 being the oldest
 *               recorded. May be negative: the window is allowed to reach back
 *               past the start of the record, and those buckets come out empty.
 * @param count  how many samples the window spans.
 * @return how many buckets were written, which is @p cols or zero.
 *
 * A zero in the output is a bucket with no reading in it - a period the monitor
 * did not answer in, or a stretch before the record began - and not a zero-volt
 * pack. The plot draws those as a break rather than a line to the floor.
 *
 * Averaging happens here rather than in the panel because this is the side that
 * has the ring, and handing out 34 KB so that the caller can reduce it to six
 * hundred pixels would be 34 KB copied several times a second while a finger is
 * dragging the plot. It also puts the decision in one place: two days across six
 * hundred columns is thirty samples a column, and the mean of those thirty is a
 * truer line than whichever one of them a subsample would have landed on.
 */
int neos_battery_history_avg(uint16_t *out, int cols, int first, int count);

/**
 * Bumped every time a reading is committed to the history.
 *
 * The panel repaints its plot when this moves rather than on its own 500 ms
 * poll, so the trace redraws once every ten seconds instead of twenty times
 * for the same pixels.
 */
uint32_t neos_battery_history_seq(void);

/**
 * Repaint the Wi-Fi icon and the clock if either has moved.
 *
 * Called on a timer from inside the bar, and by anything that has just changed
 * the network on purpose and does not want to wait a second to see it.
 */
void neos_bar_widgets_refresh(void);
