/*
 * system - what the machine is doing, and the few things you can change.
 *
 * The stock M5Tab5 demo puts fifteen hardware panels behind a scrolling
 * launcher. This is the same job with the parts that are readings and
 * switches, and without the parts that are demonstrations - no camera
 * viewfinder, no RS485 loopback, no music player. Four tabs, one screen each,
 * nothing to scroll.
 *
 * Every row is a line in one of the tables below rather than a hand-placed
 * widget, because the interesting thing about a page of readings is which
 * readings are on it, and that should be legible as a list. Adding a row is
 * one entry and a function that fills a string.
 *
 * Redrawing is per cell. A value cell is repainted only on the tick where its
 * text actually changed, which on a 720x1280 panel with a software blit is the
 * difference between a page that updates and a page that flickers. The frame
 * around it - labels, rules, the tab strip - is painted once per tab switch
 * and then left alone.
 *
 * Nothing here is linked against ngl or NeOS: every ngl_* and neos_* symbol is
 * resolved from the syscall table when the image is loaded.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_net.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

#define MARGIN      24
#define TAB_ROW_H   44         /* one row of the tab strip */
#define TAB_PAD     24         /* breathing room either side of a label */
#define ROW_H       56

#define SW_W        88          /* toggle track */
#define SW_H        40
#define SW_KNOB     30

#define SLIDER_X    260         /* track starts this far into the row */
#define SLIDER_TAIL 116         /* and stops this far short of its end */
#define SLIDER_H    10

#define TICK_MS     150         /* how often a reading is re-read */
#define POLL_MS     20          /* how often the finger is looked at */

/*
 * Room for the widest tab, which is Power since it grew a slider and two
 * buttons. The per-cell caches are indexed by row, so this is what sizes them -
 * and the assertions under the tab table are what notice when a tab outgrows it.
 */
#define MAX_ROWS    14

/* The shortest a row may be squeezed to before it stops being tappable. */
#define ROW_H_MIN   40

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    R_VALUE,    /**< label, and a string that is recomputed every tick */
    R_TOGGLE,   /**< label, and a switch */
    R_SLIDER,   /**< label, a track, and the value */
    R_ACTION,   /**< label, a value, and a tap that does something */
    /*
     * A labelled button in the value cell.
     *
     * R_ACTION already makes a row tappable, and it is the right shape when the
     * row has a value that happens also to be the invitation - "tap to scan"
     * sitting where the scan's result will be. It is the wrong shape when there
     * is no value at all: a row whose only right-hand side is the word "off"
     * reads as a reading of something, and the one thing it must read as is a
     * thing you can press.
     */
    R_BUTTON,
} rkind_t;

typedef struct {
    rkind_t     kind;
    const char *label;

    void (*text)(char *buf, int n);     /* R_VALUE, R_ACTION */
    bool (*get)(void);                  /* R_TOGGLE */
    void (*set)(bool on);
    int  (*iget)(void);                 /* R_SLIDER */
    void (*iset)(int v);
    void (*act)(void);                  /* R_ACTION, R_BUTTON */
    const char *btn;                    /* R_BUTTON: what is written on it */
    /*
     * R_SLIDER, when it is not a percentage.
     *
     * imax of zero means 0-100 and "%d%%", which is what a backlight is. A sleep
     * timeout is neither: its range is minutes and its bottom end is a word, not
     * a number, so the row says what its own value means instead of leaving the
     * slider to guess.
     */
    int  imax;
    void (*itext)(char *buf, int n, int v);
} row_t;

typedef struct {
    const char  *name;
    const row_t *rows;
    int          nrows;

    /*
     * A tab that draws itself instead of being a table of rows.
     *
     * One tab needs it: the task list is however many tasks are running, which
     * is not a fixed set of readings and cannot be a fixed set of rows. Rather
     * than bend the row machinery into something that can also be a list, a tab
     * may bring its own three functions - the frame, the tick, and the tap - and
     * everything below treats it the same way otherwise.
     */
    void (*paint)(ngl_surface_t *sc);
    void (*tick)(ngl_surface_t *sc);
    bool (*tap)(int16_t x, int16_t y);
} tab_t;

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

/*
 * Fixed-point printing by hand.
 *
 * Everything below prints through unsigned long and %lu rather than uint32_t
 * and %u: on this target uint32_t is a long, and the two spell the same width
 * differently. The casts are what stop that being a warning here and a wrong
 * number on some future target. The apps on this card have no floating point
 * anywhere and the ABI hands over scaled integers precisely so they do not
 * need any - a %d.%03d costs nothing and cannot round a reading into a
 * different number.
 */
static void fmt_milli(char *buf, int n, int32_t milli, const char *unit)
{
    const char *sign = milli < 0 ? "-" : "";
    const unsigned long m = (unsigned long)(milli < 0 ? -milli : milli);

    /*
     * Under a whole unit, stay in millis.
     *
     * A tablet sitting on USB with a full battery draws single-digit
     * milliamps through the shunt, and "0.001 A" throws away every digit
     * that was actually measured - it reads as a stuck display rather than
     * as a small number. Above a unit the decimal form is the readable one.
     */
    if (m < 1000) {
        snprintf(buf, n, "%s%lu m%s", sign, m, unit);
    } else {
        snprintf(buf, n, "%s%lu.%03lu %s", sign, m / 1000, m % 1000, unit);
    }
}

/** Tenths, e.g. 372 -> "37.2". */
static void fmt_tenths(char *buf, int n, int v, const char *unit)
{
    const char *sign = v < 0 ? "-" : "";
    const int a = v < 0 ? -v : v;
    snprintf(buf, n, "%s%d.%d %s", sign, a / 10, a % 10, unit);
}

/*
 * Binary units, one decimal, unit chosen so the number stays under four
 * digits. Everything this prints is a memory or a card size, and those are
 * quoted in powers of two by every other tool that will be read alongside
 * this screen.
 *
 * Shifts and remainders, never a division. The input is 64-bit and apps link
 * -nostdlib against a syscall table with no compiler runtime in it, so a
 * 64-bit divide here is not slow, it fails to link against __udivdi3. Every
 * step below either shifts or has already been narrowed to 32 bits.
 */
static void fmt_bytes(char *buf, int n, uint64_t b)
{
    static const char *const UNIT[] = { "B", "KB", "MB", "GB", "TB" };

    int      u = 0;
    uint32_t rem = 0;                  /* what the last shift dropped, 0-1023 */
    while (b >= 1024 && u < 4) {
        rem = (uint32_t)(b & 1023);
        b >>= 10;
        u++;
    }

    const unsigned long whole = (unsigned long)b;
    if (u == 0) {
        snprintf(buf, n, "%lu B", whole);
        return;
    }
    /* rem is under 1024, so this stays a 32-bit multiply. */
    const unsigned long frac = (unsigned long)((rem * 10u) >> 10);
    snprintf(buf, n, "%lu.%lu %s", whole, frac, UNIT[u]);
}

static void fmt_uptime(char *buf, int n, uint32_t sec)
{
    const unsigned long s = sec;
    snprintf(buf, n, "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

/* ------------------------------------------------------------------ */
/* IO                                                                  */
/* ------------------------------------------------------------------ */

static int  bl_get(void)      { return neos_backlight(); }
static void bl_set(int v)     { neos_backlight_set(v); }

static void v_resolution(char *b, int n)
{
    ngl_surface_t *sc = ngl_screen();
    snprintf(b, n, "%dx%d", sc ? ngl_surface_w(sc) : 0, sc ? ngl_surface_h(sc) : 0);
}

static void v_rotation(char *b, int n)
{
    snprintf(b, n, "%s", neos_orient_name(ngl_rotation()));
}

/* The lock is inverted on screen: what a user wants to switch is whether the
   screen follows the tablet, not whether it has been pinned. */
static bool autorot_get(void)      { return !neos_orient_is_locked(); }
static void autorot_set(bool on)
{
    if (on) {
        neos_orient_unlock();
    } else {
        neos_orient_lock(ngl_rotation());
    }
    neos_status_for(on ? "auto-rotate on" : "rotation pinned", 1500);
}

static void v_touch(char *b, int n)
{
    neos_touch_t t = {0};
    if (!neos_touch(&t)) {
        snprintf(b, n, "no panel");
    } else if (t.down) {
        snprintf(b, n, "%d, %d", t.x, t.y);
    } else {
        snprintf(b, n, "up");
    }
}

static int s_taps;
static void v_taps(char *b, int n) { snprintf(b, n, "%d", s_taps); }

/*
 * The way out of the settings store.
 *
 * Backlight and the rails are remembered across reboots, which is what makes
 * them settings rather than app state - and anything the system remembers on
 * a user's behalf needs somewhere the user can forget it again.
 */
static void v_settings(char *b, int n) { snprintf(b, n, "tap to clear"); }
static void settings_tap(void)
{
    neos_settings_reset();
    neos_status_for("stored settings cleared - board defaults on next boot", 3000);
}

/*
 * The click is played by NeOS on every touch, everywhere, so this switch is
 * about the whole machine rather than about this app - which is why it sits on
 * the same page as the backlight and not behind a preference of its own.
 * Switching it on is also what brings the audio codec up, and it can refuse.
 */
static bool taps_get(void) { return neos_tap_sound(); }
static void taps_set(bool on)
{
    if (neos_tap_sound_set(on)) {
        neos_status_for(on ? "tap sounds on" : "tap sounds off", 1500);
    } else {
        neos_status_for("the audio codec would not start", 2500);
    }
}

/*
 * The keyboard, from the app side.
 *
 * One call that blocks until the user is done, which is the whole of the text
 * input ABI - the panel, the modifiers and the screen it is drawn over all
 * belong to NeOS. It is here because the system app is where you come to find
 * out whether a part of this machine works, and a keyboard nothing on the card
 * ever raised would be a keyboard nobody could check.
 */
static char s_typed[64];

static void v_typed(char *b, int n)
{
    snprintf(b, n, "%s", s_typed[0] ? s_typed : "tap to type");
}

static void typed_tap(void)
{
    if (neos_input_text("Anything you like", s_typed, sizeof(s_typed), 0)) {
        neos_status_for("got it", 1500);
    } else {
        neos_status_for("cancelled", 1500);
    }
}

/* Down with the task page, which is where the thing it starts is stopped. */
static void tone_tap(void);

static const row_t ROWS_IO[] = {
    { .kind = R_SLIDER, .label = "Backlight",   .iget = bl_get, .iset = bl_set },
    { .kind = R_VALUE,  .label = "Resolution",  .text = v_resolution },
    { .kind = R_VALUE,  .label = "Orientation", .text = v_rotation },
    { .kind = R_TOGGLE, .label = "Auto-rotate", .get = autorot_get, .set = autorot_set },
    { .kind = R_VALUE,  .label = "Touch",       .text = v_touch },
    { .kind = R_VALUE,  .label = "Taps seen",   .text = v_taps },
    { .kind = R_TOGGLE, .label = "Tap sounds",  .get = taps_get, .set = taps_set },
    { .kind = R_ACTION, .label = "Text input",  .text = v_typed, .act = typed_tap },
    { .kind = R_ACTION, .label = "Saved settings", .text = v_settings, .act = settings_tap },
    /*
     * Here rather than on the task page, because starting a thing and stopping
     * it are not the same kind of act: this is a switch on a page of switches,
     * and stopping it is one row in a list of everything that is running.
     */
    { .kind = R_BUTTON, .label = "Background tone", .btn = "play", .act = tone_tap },
};

/* ------------------------------------------------------------------ */
/* Sensors                                                             */
/* ------------------------------------------------------------------ */

/*
 * All three axes come from one read rather than three, so that what is on
 * screen is one instant of the sensor and not three. A tablet being waved
 * about would otherwise show a triple that never existed.
 */
static void axes(char *b, int n, bool accel)
{
    int16_t x = 0, y = 0, z = 0;
    const bool ok = accel ? neos_imu_accel_mg(&x, &y, &z)
                          : neos_imu_gyro_dps(&x, &y, &z);
    if (!ok) {
        snprintf(b, n, "-");
        return;
    }
    snprintf(b, n, "%+d %+d %+d", x, y, z);
}

static void v_accel(char *b, int n) { axes(b, n, true); }
static void v_gyro(char *b, int n)  { axes(b, n, false); }

static void v_tilt(char *b, int n)
{
    snprintf(b, n, "%s%s", neos_orient_name(neos_orient_get()),
             neos_orient_is_locked() ? " (pinned)" : "");
}

static void v_temp(char *b, int n)
{
    const int16_t t = neos_die_temp_c10();
    if (t == INT16_MIN) {
        snprintf(b, n, "-");
    } else {
        fmt_tenths(b, n, t, "C");
    }
}

static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

static void v_rtc_date(char *b, int n)
{
    neos_rtc_t t;
    if (!neos_rtc_read(&t)) {
        snprintf(b, n, "not set");
        return;
    }
    snprintf(b, n, "%s %04d-%02u-%02u", WDAY[t.wday % 7], t.year, t.month, t.day);
}

static void v_rtc_time(char *b, int n)
{
    neos_rtc_t t;
    if (!neos_rtc_read(&t)) {
        snprintf(b, n, "-");
        return;
    }
    snprintf(b, n, "%02u:%02u:%02u", t.hour, t.min, t.sec);
}

/*
 * The RTC rows are the chip; these two are the time.
 *
 * Usually the same reading, and deliberately shown separately: the chip holds
 * local time and the system clock holds UTC, so when they disagree - a zone
 * just changed, an SNTP sync just landed - the difference is the thing worth
 * seeing.
 */
static void v_local(char *b, int n)
{
    neos_rtc_t t;
    if (!neos_time_local(&t)) {
        snprintf(b, n, "not set");
        return;
    }
    snprintf(b, n, "%02u:%02u:%02u  %s", t.hour, t.min, t.sec,
             neos_time_synced() ? "synced" : "from RTC");
}

static void v_zone(char *b, int n)
{
    const int m = neos_tz_total_min();
    const char sign = m < 0 ? '-' : '+';
    const int a = m < 0 ? -m : m;
    snprintf(b, n, "UTC%c%02d:%02d%s", sign, a / 60, a % 60,
             neos_tz_dst() ? "  DST" : "");
}

static const row_t ROWS_SENSORS[] = {
    { .kind = R_VALUE, .label = "Accel  mg",  .text = v_accel },
    { .kind = R_VALUE, .label = "Gyro  dps",  .text = v_gyro },
    { .kind = R_VALUE, .label = "Facing",     .text = v_tilt },
    { .kind = R_VALUE, .label = "Die temp",   .text = v_temp },
    { .kind = R_VALUE, .label = "RTC date",   .text = v_rtc_date },
    { .kind = R_VALUE, .label = "RTC time",   .text = v_rtc_time },
    { .kind = R_VALUE, .label = "Local time",  .text = v_local },
    { .kind = R_VALUE, .label = "Time zone",   .text = v_zone },
};

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */

/*
 * One I2C transaction serves the whole tab.
 *
 * Four rows all want the same sample, and asking the part four times a tick
 * would both cost four times the bus traffic and put four different instants
 * on one screen. The tick reads once into here and every row formats out of
 * it, so the volts, the amps and the watts on screen always multiply out.
 */
static neos_power_t s_pwr;
static bool         s_pwr_ok;

static void v_bus(char *b, int n)
{
    if (!s_pwr_ok) { snprintf(b, n, "-"); return; }
    fmt_milli(b, n, s_pwr.bus_mv, "V");
}

static void v_current(char *b, int n)
{
    if (!s_pwr_ok) { snprintf(b, n, "-"); return; }
    fmt_milli(b, n, s_pwr.current_ma, "A");
}

static void v_watts(char *b, int n)
{
    if (!s_pwr_ok) { snprintf(b, n, "-"); return; }
    fmt_milli(b, n, s_pwr.power_mw, "W");
}

static void v_shunt(char *b, int n)
{
    if (!s_pwr_ok) { snprintf(b, n, "-"); return; }
    snprintf(b, n, "%d uV", (int)s_pwr.shunt_uv);
}

/*
 * The monitor sits across the battery, not across the input: on this board it
 * reads about 8 V, which is a 2S pack and nothing else on the tablet. So a
 * current of nearly zero is the normal reading while it runs on USB with the
 * pack neither charging nor discharging - the sign is what says which way it
 * is going once it does.
 */
static void v_monitor(char *b, int n)
{
    const int a = neos_power_monitor_addr();
    if (a < 0) {
        snprintf(b, n, "absent");
    } else {
        snprintf(b, n, "INA226 @ 0x%02X", a);
    }
}

static void v_reset(char *b, int n)  { snprintf(b, n, "%s", neos_reset_reason()); }

/*
 * Every rail goes through one pair of thunks.
 *
 * A rail is a number, but a row wants a nullary function, so each one needs
 * its own two-line getter and setter. They are all identical apart from the
 * constant, which is exactly the shape a macro is for - and it keeps the
 * failure message and the toast in one place instead of eight.
 */
static void rail_toggle(neos_feature_t f, bool on)
{
    char msg[64];
    if (neos_feature_set(f, on)) {
        snprintf(msg, sizeof(msg), "%s %s", neos_feature_name(f), on ? "on" : "off");
    } else {
        snprintf(msg, sizeof(msg), "%s: the io expander did not take it",
                 neos_feature_name(f));
    }
    neos_status_for(msg, 1500);
}

#define RAIL(fn, feat)                                              \
    static bool fn##_get(void)    { return neos_feature(feat); }    \
    static void fn##_set(bool on) { rail_toggle(feat, on); }

RAIL(usb5v,  NEOS_FEAT_USB_5V)
RAIL(charge, NEOS_FEAT_CHARGE)
RAIL(qc,     NEOS_FEAT_CHARGE_QC)
RAIL(ext5v,  NEOS_FEAT_EXT_5V)
RAIL(cam,    NEOS_FEAT_CAMERA)
RAIL(spk,    NEOS_FEAT_SPEAKER)
RAIL(wifi,   NEOS_FEAT_WIFI)
RAIL(ant,    NEOS_FEAT_ANTENNA)

/*
 * The rest of the pack, which is a page of its own and already exists.
 *
 * Five rows of power readings and then nothing about the battery itself - no
 * percentage, no history, no estimate of how long it has left - while all of
 * that sits behind an icon in the corner of the bar. The icon is one tap from
 * everywhere and is still the wrong place to send somebody who is already
 * looking at a page called Power, so the page says where the rest of it is.
 */
static void battery_tap(void)
{
    neos_syspanel_open(NEOS_SYSPANEL_BATTERY);
}

/*
 * Sleeping, and the two halves of it.
 *
 * The slider is how long the tablet waits; the button is not waiting. They are
 * on the power page and not among the display settings on purpose - what turning
 * the screen off is for is the battery, and the reason to look for it is the
 * reading three rows up.
 */
static int  sleep_get(void)      { return neos_idle_timeout_min(); }
static void sleep_set(int v)     { neos_idle_timeout_set(v); }

static void sleep_text(char *b, int n, int v)
{
    if (v <= 0) {
        snprintf(b, n, "never");
    } else {
        snprintf(b, n, "%d min", v);
    }
}

/*
 * Off now, and everything else keeps running - which is the whole feature and
 * the reason this is not called "sleep". A player started from the IO tab plays
 * through it, the clock stays right, and a touch or picking the tablet up brings
 * the screen back.
 */
static void screen_tap(void)
{
    /*
     * The message, a moment to read it, and then dark.
     *
     * The pause is the whole of it. The status line is painted by NeOS on its
     * own clock, so setting a message and switching the panel off in the next
     * instruction shows nobody anything - and what it would have said is the one
     * thing a user meeting this button for the first time needs to know, which is
     * how to undo it. Blocking here is free: this is a tap on a settings page
     * and there is nothing else the app was going to do with the next half
     * second.
     */
    neos_status_for("screen off - touch it or pick it up to wake", 2500);
    neos_sleep_ms(700);
    neos_screen_off();
}

static const row_t ROWS_POWER[] = {
    { .kind = R_VALUE,  .label = "Bus voltage",  .text = v_bus },
    { .kind = R_VALUE,  .label = "Current",      .text = v_current },
    { .kind = R_VALUE,  .label = "Power",        .text = v_watts },
    { .kind = R_VALUE,  .label = "Shunt",        .text = v_shunt },
    { .kind = R_VALUE,  .label = "Monitor",      .text = v_monitor },
    { .kind = R_TOGGLE, .label = "Charging",     .get = charge_get, .set = charge_set },
    { .kind = R_TOGGLE, .label = "Fast charge",  .get = qc_get,     .set = qc_set },
    { .kind = R_TOGGLE, .label = "USB-A 5V out", .get = usb5v_get,  .set = usb5v_set },
    { .kind = R_TOGGLE, .label = "Ext 5V out",   .get = ext5v_get,  .set = ext5v_set },
    { .kind = R_BUTTON, .label = "Battery detail", .btn = "more...", .act = battery_tap },
    { .kind = R_SLIDER, .label = "Sleep after",  .iget = sleep_get, .iset = sleep_set,
      .imax = NEOS_IDLE_MAX_MIN, .itext = sleep_text },
    { .kind = R_BUTTON, .label = "Screen",       .btn = "off", .act = screen_tap },
};

/* ------------------------------------------------------------------ */
/* Peripherals                                                         */
/* ------------------------------------------------------------------ */

static void v_card(char *b, int n)
{
    if (!neos_sd_mounted()) {
        snprintf(b, n, "not mounted");
        return;
    }
    char size[24];
    fmt_bytes(size, sizeof(size), neos_sd_bytes());
    snprintf(b, n, "%s  %s", neos_sd_name(), size);
}

/*
 * The bus map, filled by a scan and then left alone.
 *
 * A scan probes every address on the bus, which is the one call in the system
 * ABI that takes real time, so it happens once at startup and afterwards only
 * when the row is tapped - never on the tick.
 */
#define MAX_I2C 16
static uint8_t s_i2c[MAX_I2C];
static int     s_i2c_n;
static int     s_i2c_sel;      /* which address the row is naming */

static void i2c_rescan(void)
{
    s_i2c_n = neos_i2c_scan(s_i2c, MAX_I2C);
    if (s_i2c_n > MAX_I2C) {
        s_i2c_n = MAX_I2C;
    }
    s_i2c_sel = 0;
    char msg[48];
    snprintf(msg, sizeof(msg), "%d device%s on the bus",
             s_i2c_n, s_i2c_n == 1 ? "" : "s");
    neos_status_for(msg, 2000);
}

static void v_i2c_map(char *b, int n)
{
    if (s_i2c_n == 0) {
        snprintf(b, n, "tap to scan");
        return;
    }
    /* Hex, in bus order. Short enough to read at a glance and the only form
       that can be compared against a datasheet without arithmetic. */
    int at = 0;
    for (int i = 0; i < s_i2c_n && at < n - 4; i++) {
        at += snprintf(b + at, n - at, i ? " %02X" : "%02X", s_i2c[i]);
    }
}

/*
 * The named part, one at a time.
 *
 * The whole list of names does not fit on a row and a screen of them would be
 * a second page. Cycling one per tick turns the row into something you watch
 * for a moment instead of something you navigate.
 */
static void v_i2c_who(char *b, int n)
{
    if (s_i2c_n == 0) {
        snprintf(b, n, "-");
        return;
    }
    const uint8_t a = s_i2c[s_i2c_sel % s_i2c_n];
    const char *name = neos_i2c_name(a);
    snprintf(b, n, "0x%02X  %s", a, name[0] ? name : "unknown");
}

static void i2c_tap(void) { i2c_rescan(); }

static void v_chip(char *b, int n)
{
    snprintf(b, n, "%s  x%u @ %u MHz", neos_chip(), neos_cores(), neos_cpu_mhz());
}

static const row_t ROWS_PERIPH[] = {
    { .kind = R_VALUE,  .label = "SD card",     .text = v_card },
    { .kind = R_ACTION, .label = "I2C bus",     .text = v_i2c_map, .act = i2c_tap },
    { .kind = R_VALUE,  .label = "  at",        .text = v_i2c_who },
    { .kind = R_TOGGLE, .label = "Camera",      .get = cam_get,  .set = cam_set },
    { .kind = R_TOGGLE, .label = "Speaker",     .get = spk_get,  .set = spk_set },
    { .kind = R_TOGGLE, .label = "Ext antenna", .get = ant_get,  .set = ant_set },
};

/* ------------------------------------------------------------------ */
/* Network                                                             */
/* ------------------------------------------------------------------ */

/*
 * Readings, and the rail the radio runs on. Nothing here joins or leaves a
 * network: there is one radio and NeOS owns it, so choosing a network is the
 * Wi-Fi icon in the system bar - which is one tap away from every screen,
 * including this one.
 */
static void v_net_state(char *b, int n)
{
    switch (neos_net_state()) {
    case NEOS_NET_ABSENT:     snprintf(b, n, "no radio"); break;
    case NEOS_NET_OFF:        snprintf(b, n, "off");      break;
    case NEOS_NET_CONNECTING: snprintf(b, n, "joining");  break;
    case NEOS_NET_ONLINE:     snprintf(b, n, "online");   break;
    default:                  snprintf(b, n, "idle");     break;
    }
}

static void v_net_ssid(char *b, int n)
{
    const char *v = neos_net_ssid();
    snprintf(b, n, "%s", v[0] ? v : "-");
}

static void v_net_ip(char *b, int n)
{
    const char *v = neos_net_ip();
    snprintf(b, n, "%s", v[0] ? v : "-");
}

static void v_net_rssi(char *b, int n)
{
    const int r = neos_net_rssi();
    if (r == 0) {
        snprintf(b, n, "-");
    } else {
        snprintf(b, n, "%d dBm", r);
    }
}

static void v_net_saved(char *b, int n) { snprintf(b, n, "%d", neos_net_known_count()); }

static void v_net_seen(char *b, int n)
{
    if (neos_net_scanning()) {
        snprintf(b, n, "scanning");
        return;
    }
    /* NULL and 0: the count is the return value, so asking for none of the
       rows is the cheap way to ask how many there are. */
    snprintf(b, n, "%d", neos_net_scan_results(NULL, 0));
}

static void v_mac(char *b, int n) { snprintf(b, n, "%s", neos_mac()); }

static void v_clock_src(char *b, int n)
{
    if (!neos_time_synced()) {
        snprintf(b, n, "RTC");
        return;
    }
    const unsigned long ago = (unsigned long)neos_time_since_sync_s();
    if (ago < 90) {
        snprintf(b, n, "network, %lus ago", ago);
    } else if (ago < 5400) {
        snprintf(b, n, "network, %lum ago", ago / 60);
    } else {
        snprintf(b, n, "network, %luh ago", ago / 3600);
    }
}

static const row_t ROWS_NET[] = {
    { .kind = R_VALUE,  .label = "Wi-Fi",       .text = v_net_state },
    { .kind = R_VALUE,  .label = "Network",     .text = v_net_ssid },
    { .kind = R_VALUE,  .label = "Address",     .text = v_net_ip },
    { .kind = R_VALUE,  .label = "Signal",      .text = v_net_rssi },
    { .kind = R_VALUE,  .label = "In range",    .text = v_net_seen },
    { .kind = R_VALUE,  .label = "Remembered",  .text = v_net_saved },
    { .kind = R_VALUE,  .label = "MAC",         .text = v_mac },
    { .kind = R_VALUE,  .label = "Clock from",  .text = v_clock_src },
    { .kind = R_TOGGLE, .label = "Radio power", .get = wifi_get, .set = wifi_set },
};

/* ------------------------------------------------------------------ */
/* The machine, and the card                                           */
/* ------------------------------------------------------------------ */

static void v_build(char *b, int n)  { snprintf(b, n, "%s", neos_build()); }
static void v_built(char *b, int n)  { snprintf(b, n, "%s", neos_build_date()); }
static void v_idf(char *b, int n)    { snprintf(b, n, "%s", neos_idf_version()); }

/*
 * Both ABI versions, because the interesting thing is whether they match.
 *
 * The left one is the firmware; the right is what this app was compiled
 * against, which the loader resolved through the guard chain in neos_abi.h.
 * An app older than the firmware is the normal case and is exactly what that
 * chain is for - it is only a difference in the major that would have stopped
 * the app loading at all.
 */
static void v_abi(char *b, int n)
{
    const uint32_t fw = neos_abi();
    snprintf(b, n, "%u.%u  (app %d.%d)",
             (unsigned)(fw >> 16), (unsigned)(fw & 0xFFFF),
             NEOS_ABI_MAJOR, NEOS_ABI_MINOR);
}

static void v_ram(char *b, int n)
{
    char free_[24], total[24];
    fmt_bytes(free_, sizeof(free_), neos_heap_free());
    fmt_bytes(total, sizeof(total), neos_heap_total());
    snprintf(b, n, "%s free of %s", free_, total);
}

static void v_psram(char *b, int n)
{
    char free_[24], total[24];
    fmt_bytes(free_, sizeof(free_), neos_psram_free());
    fmt_bytes(total, sizeof(total), neos_psram_total());
    snprintf(b, n, "%s free of %s", free_, total);
}

static void v_uptime(char *b, int n) { fmt_uptime(b, n, neos_uptime_s()); }

static const row_t ROWS_SYSTEM[] = {
    { .kind = R_VALUE, .label = "NeOS build", .text = v_build },
    { .kind = R_VALUE, .label = "Built",      .text = v_built },
    { .kind = R_VALUE, .label = "ABI",        .text = v_abi },
    { .kind = R_VALUE, .label = "ESP-IDF",    .text = v_idf },
    { .kind = R_VALUE, .label = "SoC",        .text = v_chip },
    { .kind = R_VALUE, .label = "RAM",        .text = v_ram },
    { .kind = R_VALUE, .label = "PSRAM",      .text = v_psram },
    { .kind = R_VALUE, .label = "Uptime",     .text = v_uptime },
    { .kind = R_VALUE, .label = "Last reset", .text = v_reset },
};

/* ------------------------------------------------------------------ */

static void v_sd_name(char *b, int n)
{
    const char *v = neos_sd_name();
    snprintf(b, n, "%s", v[0] ? v : "no card");
}

static void v_sd_type(char *b, int n)
{
    const char *v = neos_sd_type();
    snprintf(b, n, "%s", v[0] ? v : "-");
}

static void v_sd_size(char *b, int n)
{
    if (!neos_sd_mounted()) {
        snprintf(b, n, "-");
        return;
    }
    fmt_bytes(b, n, neos_sd_bytes());
}

/*
 * Free space is counted by walking the allocation table, which is slow enough
 * that it must not happen on a tick. It is read once when the tab is opened
 * and whenever the row is tapped, and cached in between.
 */
static uint64_t s_sd_free;
static bool     s_sd_free_known;

static void sd_measure(void)
{
    s_sd_free = neos_sd_mounted() ? neos_sd_free_bytes() : 0;
    s_sd_free_known = true;
}

static void v_sd_free(char *b, int n)
{
    if (!neos_sd_mounted()) {
        snprintf(b, n, "-");
        return;
    }
    if (!s_sd_free_known) {
        snprintf(b, n, "tap to measure");
        return;
    }
    fmt_bytes(b, n, s_sd_free);
}

static void sd_free_tap(void) { sd_measure(); }

static void v_sd_bus(char *b, int n)
{
    if (!neos_sd_mounted()) {
        snprintf(b, n, "-");
        return;
    }
    const unsigned long khz = (unsigned long)neos_sd_speed_khz();
    snprintf(b, n, "%d-bit at %lu MHz", neos_sd_bus_width(), khz / 1000);
}

static void v_sd_mount(char *b, int n) { snprintf(b, n, "%s", neos_sd_mount()); }

static void v_sd_apps(char *b, int n)
{
    const int c = neos_apps_count();
    snprintf(b, n, "%d", c);
}

static const row_t ROWS_SD[] = {
    { .kind = R_VALUE,  .label = "Card",      .text = v_sd_name },
    { .kind = R_VALUE,  .label = "Type",      .text = v_sd_type },
    { .kind = R_VALUE,  .label = "Capacity",  .text = v_sd_size },
    { .kind = R_ACTION, .label = "Free",      .text = v_sd_free, .act = sd_free_tap },
    { .kind = R_VALUE,  .label = "Bus",       .text = v_sd_bus },
    { .kind = R_VALUE,  .label = "Mounted at", .text = v_sd_mount },
    { .kind = R_VALUE,  .label = "Apps",      .text = v_sd_apps },
};

/* ------------------------------------------------------------------ */

/* The task page brings its own three functions; they are at the bottom, with
   the painting helpers they are built out of. */
static void tasks_paint(ngl_surface_t *sc);
static void tasks_tick(ngl_surface_t *sc);
static bool tasks_tap(int16_t x, int16_t y);

#define NTABS 8

/* Named fields, so that a tab which does not bring its own painting says so by
   leaving those out rather than by three NULLs nobody can read. */
static const tab_t TABS[NTABS] = {
    { .name = "IO",      .rows = ROWS_IO,      .nrows = (int)(sizeof(ROWS_IO)      / sizeof(row_t)) },
    { .name = "SENSORS", .rows = ROWS_SENSORS, .nrows = (int)(sizeof(ROWS_SENSORS) / sizeof(row_t)) },
    { .name = "POWER",   .rows = ROWS_POWER,   .nrows = (int)(sizeof(ROWS_POWER)   / sizeof(row_t)) },
    { .name = "PERIPH",  .rows = ROWS_PERIPH,  .nrows = (int)(sizeof(ROWS_PERIPH)  / sizeof(row_t)) },
    { .name = "NET",     .rows = ROWS_NET,     .nrows = (int)(sizeof(ROWS_NET)     / sizeof(row_t)) },
    { .name = "SD",      .rows = ROWS_SD,      .nrows = (int)(sizeof(ROWS_SD)      / sizeof(row_t)) },
    { .name = "SYSTEM",  .rows = ROWS_SYSTEM,  .nrows = (int)(sizeof(ROWS_SYSTEM)  / sizeof(row_t)) },
    { .name = "TASKS",   .paint = tasks_paint, .tick = tasks_tick, .tap = tasks_tap },
};

/* The per-cell caches are indexed by row, so the widest tab sets their size. */
_Static_assert(sizeof(ROWS_IO)      / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_SENSORS) / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_POWER)   / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_PERIPH)  / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_NET)     / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_SD)      / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");
_Static_assert(sizeof(ROWS_SYSTEM)  / sizeof(row_t) <= MAX_ROWS, "MAX_ROWS too small");

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static ngl_rect_t s_area;       /* what the OS is currently giving us */
static int        s_tab;

/*
 * How tall a row is on this tab, in this orientation.
 *
 * It used to be the constant ROW_H, which was fine while the longest tab was
 * nine rows: nine times fifty-six fits under the tab strip in landscape with
 * room to spare. Twelve does not, and the row that does not fit is not clipped -
 * it is drawn off the bottom of the panel, where a slider you cannot see is
 * still a slider you can drag. So the height is what fits, down to a floor
 * below which a row stops being something a finger can hit.
 */
static int16_t s_row_h = ROW_H;

/* Last thing painted into each cell, so a tick can tell whether to repaint. */
static char s_shown[MAX_ROWS][48];
static bool s_shown_on[MAX_ROWS];
static int  s_shown_i[MAX_ROWS];
static bool s_have_shown;

/*
 * The tab strip wraps.
 *
 * Seven tabs across 720 px of portrait is 102 px each, and "SENSORS" is 112 -
 * so a single row stopped fitting the moment the strip grew, and it will stop
 * fitting again at the next tab. Rather than shortening names until they fit
 * the narrowest case, the strip works out how many fit and uses as many rows
 * as it needs. One row in landscape, two in portrait, and adding a tab is
 * still just a line in the table.
 */
static int s_tab_cols = 1, s_tab_rows = 1;

static void tabs_layout(void)
{
    int16_t widest = 0;
    for (int i = 0; i < NTABS; i++) {
        const int16_t w = ngl_text_width(&ngl_font_small, TABS[i].name);
        if (w > widest) {
            widest = w;
        }
    }
    const int16_t cell = (int16_t)(widest + TAB_PAD);
    int cols = cell > 0 ? s_area.w / cell : NTABS;
    if (cols < 1)     { cols = 1; }
    if (cols > NTABS) { cols = NTABS; }
    s_tab_cols = cols;
    s_tab_rows = (NTABS + cols - 1) / cols;
}

static int16_t tabs_h(void)
{
    return (int16_t)(s_tab_rows * TAB_ROW_H);
}

/** Re-measure the rows for whichever tab is open. */
static void rows_layout(void)
{
    const int n = TABS[s_tab].nrows;
    s_row_h = ROW_H;
    if (n <= 0) {
        return;
    }
    const int16_t avail = (int16_t)(s_area.h - tabs_h() - 16);
    if (avail > 0 && n * ROW_H > avail) {
        s_row_h = (int16_t)(avail / n);
        if (s_row_h < ROW_H_MIN) {
            s_row_h = ROW_H_MIN;
        }
    }
}

static ngl_rect_t tab_rect(int i)
{
    const int16_t w = (int16_t)(s_area.w / s_tab_cols);
    const int row = i / s_tab_cols;
    const int col = i % s_tab_cols;
    return ngl_rect((int16_t)(s_area.x + col * w),
                    (int16_t)(s_area.y + row * TAB_ROW_H), w, TAB_ROW_H);
}

static ngl_rect_t row_rect(int i)
{
    return ngl_rect((int16_t)(s_area.x + MARGIN),
                    (int16_t)(s_area.y + tabs_h() + 8 + i * s_row_h),
                    (int16_t)(s_area.w - 2 * MARGIN), s_row_h);
}

/** Where a value string is drawn, right-aligned. Also the repaint unit. */
static ngl_rect_t val_rect(const ngl_rect_t *row)
{
    const int16_t w = (int16_t)(row->w - SLIDER_X);
    return ngl_rect((int16_t)(row->x + SLIDER_X), (int16_t)(row->y + 8),
                    w, (int16_t)(s_row_h - 16));
}

static ngl_rect_t switch_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + row->w - SW_W),
                    (int16_t)(row->y + (s_row_h - SW_H) / 2), SW_W, SW_H);
}

static ngl_rect_t track_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + SLIDER_X),
                    (int16_t)(row->y + (s_row_h - SLIDER_H) / 2),
                    (int16_t)(row->w - SLIDER_X - SLIDER_TAIL), SLIDER_H);
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/*
 * Right-aligned inside box, and confined to it.
 *
 * The clip is not decoration. Values here are strings whose length is not
 * known when the layout is chosen - a bus with twelve devices on it, a card
 * with a long product name - and right-aligned text that outgrows its cell
 * grows leftwards, straight through the label it is the value of.
 */
static void text_right(ngl_surface_t *sc, const ngl_rect_t *box, const char *s,
                       ngl_color_t c)
{
    const int16_t w = ngl_text_width(&ngl_font_small, s);
    int16_t x = (int16_t)(box->x + box->w - w);
    if (x < box->x) {
        x = box->x;
    }
    const int16_t y = (int16_t)(box->y + (box->h - ngl_font_small.height) / 2);

    ngl_clip_set(sc, box);
    ngl_text(sc, x, y, s, &ngl_font_small, c);
    ngl_clip_set(sc, NULL);
}

/** As text_right(), centred. For what is written on a button. */
static void text_centred(ngl_surface_t *sc, const ngl_rect_t *box, const char *s,
                         ngl_color_t c)
{
    const int16_t w = ngl_text_width(&ngl_font_small, s);
    int16_t x = (int16_t)(box->x + (box->w - w) / 2);
    if (x < box->x) {
        x = box->x;
    }
    const int16_t y = (int16_t)(box->y + (box->h - ngl_font_small.height) / 2);

    ngl_clip_set(sc, box);
    ngl_text(sc, x, y, s, &ngl_font_small, c);
    ngl_clip_set(sc, NULL);
}

/*
 * A button: outlined, filled behind the label, and the same shape everywhere.
 *
 * Outline rather than fill, like the switches and for the same reason given at
 * paint_switch() - in a one-hue palette a filled rectangle is a brightness
 * difference, and a brightness difference reads as a rendering artefact rather
 * than as a control.
 */
static void button_box(ngl_surface_t *sc, ngl_rect_t r, const char *label)
{
    ngl_fill_round_rect(sc, r, 8, TH_KEY_FILL);
    ngl_draw_round_rect(sc, r, 8, TH_KEY_EDGE, 2);
    text_centred(sc, &r, label, TH_KEY_TEXT);
}

/** Where a row's button goes: the right-hand end of the value cell. */
static ngl_rect_t button_rect(const ngl_rect_t *row, const char *label)
{
    int16_t w = (int16_t)(ngl_text_width(&ngl_font_small, label) + 36);
    if (w < 88) {
        w = 88;
    }
    const int16_t h = (int16_t)(s_row_h - 16 > 44 ? 44 : s_row_h - 16);
    return ngl_rect((int16_t)(row->x + row->w - w),
                    (int16_t)(row->y + (s_row_h - h) / 2), w, h);
}

static void paint_tabs(ngl_surface_t *sc)
{
    for (int i = 0; i < NTABS; i++) {
        const ngl_rect_t t = tab_rect(i);
        const bool on = (i == s_tab);

        ngl_fill_rect(sc, t, on ? TH_PANEL : TH_BG);

        const int16_t tw = ngl_text_width(&ngl_font_small, TABS[i].name);
        ngl_text(sc, (int16_t)(t.x + (t.w - tw) / 2),
                 (int16_t)(t.y + (t.h - ngl_font_small.height) / 2 - 2),
                 TABS[i].name, &ngl_font_small, on ? TH_GLOW : TH_TEXT_DIM);

        /* The selected tab is marked by a bar under it rather than by a
           brighter fill. In a one-hue palette a fill difference reads as a
           rendering artefact; an edge reads as a choice. */
        if (on) {
            ngl_fill_rect(sc, ngl_rect(t.x, (int16_t)(t.y + t.h - 4), t.w, 4),
                          TH_ACCENT);
        }
    }
    ngl_hline(sc, s_area.x, (int16_t)(s_area.y + tabs_h() - 1), s_area.w, TH_RULE);
}

/**
 * The switch.
 *
 * Off is an outline, on is filled: the two states differ in weight rather
 * than only in where the knob sits, so which one is which survives being
 * glanced at from across a desk.
 */
static void paint_switch(ngl_surface_t *sc, ngl_rect_t sw, bool on)
{
    const int16_t r = (int16_t)(SW_H / 2);
    ngl_fill_rect(sc, sw, TH_BG);

    if (on) {
        ngl_fill_round_rect(sc, sw, r, TH_CLOSE_FILL);
        ngl_draw_round_rect(sc, sw, r, TH_ACCENT, 2);
    } else {
        ngl_draw_round_rect(sc, sw, r, TH_EDGE, 2);
    }

    const int16_t pad = (int16_t)((SW_H - SW_KNOB) / 2);
    const ngl_rect_t knob = ngl_rect(
        (int16_t)(on ? sw.x + sw.w - SW_KNOB - pad : sw.x + pad),
        (int16_t)(sw.y + pad), SW_KNOB, SW_KNOB);
    ngl_fill_round_rect(sc, knob, (int16_t)(SW_KNOB / 2),
                        on ? TH_GLOW : TH_TEXT_FAINT);
}

/** The top of a slider's range: 0-100 unless the row says otherwise. */
static int slider_max(const row_t *r)
{
    return r->imax > 0 ? r->imax : 100;
}

static void paint_slider(ngl_surface_t *sc, const ngl_rect_t *row, const row_t *r,
                         int v)
{
    const int max = slider_max(r);
    if (v < 0)   { v = 0; }
    if (v > max) { v = max; }

    const ngl_rect_t tr = track_rect(row);
    const int16_t rad = (int16_t)(SLIDER_H / 2);

    /* The knob overhangs the track at both ends and the value sits past it, so
       the cleared area is the row from the track leftwards, not the track. */
    ngl_fill_rect(sc, ngl_rect((int16_t)(tr.x - 12), (int16_t)(row->y + 4),
                               (int16_t)(row->w - SLIDER_X + 12),
                               (int16_t)(s_row_h - 8)), TH_BG);
    ngl_fill_round_rect(sc, tr, rad, TH_RULE);

    const int16_t fw = (int16_t)((int32_t)tr.w * v / max);
    if (fw > 0) {
        ngl_fill_round_rect(sc, ngl_rect(tr.x, tr.y, fw, tr.h), rad, TH_ACCENT);
    }

    const ngl_rect_t knob = ngl_rect((int16_t)(tr.x + fw - 11),
                                     (int16_t)(tr.y - 6), 22, 22);
    ngl_fill_round_rect(sc, knob, 11, TH_GLOW);

    char n[16];
    if (r->itext) {
        r->itext(n, sizeof(n), v);
    } else {
        snprintf(n, sizeof(n), "%d%%", v);
    }
    ngl_rect_t box = *row;
    text_right(sc, &box, n, TH_TEXT);
}

/** The value cell, and only it. Called on every tick where the text moved. */
static void paint_value(ngl_surface_t *sc, int i, const char *s)
{
    const ngl_rect_t row = row_rect(i);
    const ngl_rect_t box = val_rect(&row);
    ngl_fill_rect(sc, box, TH_BG);
    text_right(sc, &box, s, TH_TEXT);
}

/*
 * The frame: everything that does not change until the tab does.
 *
 * Values are deliberately left out - they are painted by the first tick,
 * which is also what fills in the "what is currently shown" cache. Painting
 * them here as well would put the same string on the panel twice.
 */
static void paint_page(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    ngl_clear(sc, TH_BG);            /* also repaints the system bar */
    paint_tabs(sc);

    const tab_t *t = &TABS[s_tab];
    if (t->paint) {
        t->paint(sc);
    }
    for (int i = 0; i < t->nrows; i++) {
        const ngl_rect_t row = row_rect(i);
        ngl_text(sc, (int16_t)(row.x + 4),
                 (int16_t)(row.y + (s_row_h - ngl_font_small.height) / 2),
                 t->rows[i].label, &ngl_font_small, TH_TEXT_DIM);
        ngl_hline(sc, row.x, (int16_t)(row.y + s_row_h - 1), row.w, TH_RULE);

        /* A button is part of the frame, not part of the tick: what is written
           on it does not change, so painting it once per tab is once too often
           already. */
        if (t->rows[i].kind == R_BUTTON && t->rows[i].btn) {
            button_box(sc, button_rect(&row, t->rows[i].btn), t->rows[i].btn);
        }
    }

    s_have_shown = false;            /* nothing on this page is cached yet */
    ngl_flush();
}


/* ------------------------------------------------------------------ */
/* The tick                                                            */
/* ------------------------------------------------------------------ */

/*
 * Recompute every cell on the page and repaint the ones that moved.
 *
 * Cheap because the comparison is on the formatted string, not on the
 * underlying reading: an accelerometer that wobbles in the noise below the
 * displayed precision produces no repaint at all, and a page of static
 * readings costs one flush of nothing.
 */
static void tick(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }

    const tab_t *t = &TABS[s_tab];

    if (t->tick) {
        t->tick(sc);
        ngl_flush();
        return;
    }

    if (t->rows == ROWS_POWER) {
        s_pwr_ok = neos_power_read(&s_pwr);
    }
    /* One name per second or so. At tick rate the row would be a blur, and
       the point of it is that you can read one part off it and stop. */
    static int hold;
    if (t->rows == ROWS_PERIPH && s_i2c_n > 0 && ++hold >= 1000 / TICK_MS) {
        hold = 0;
        s_i2c_sel = (s_i2c_sel + 1) % s_i2c_n;
    }

    for (int i = 0; i < t->nrows; i++) {
        const row_t *r = &t->rows[i];
        const ngl_rect_t row = row_rect(i);

        switch (r->kind) {
        case R_VALUE:
        case R_ACTION: {
            char buf[48];
            buf[0] = 0;
            if (r->text) {
                r->text(buf, sizeof(buf));
            }
            if (s_have_shown && strcmp(s_shown[i], buf) == 0) {
                break;
            }
            snprintf(s_shown[i], sizeof(s_shown[i]), "%s", buf);
            paint_value(sc, i, buf);
            break;
        }
        case R_TOGGLE: {
            const bool on = r->get ? r->get() : false;
            if (s_have_shown && s_shown_on[i] == on) {
                break;
            }
            s_shown_on[i] = on;
            const ngl_rect_t sw = switch_rect(&row);
            paint_switch(sc, sw, on);
            break;
        }
        case R_SLIDER: {
            const int v = r->iget ? r->iget() : 0;
            if (s_have_shown && s_shown_i[i] == v) {
                break;
            }
            s_shown_i[i] = v;
            paint_slider(sc, &row, r, v);
            break;
        }
        case R_BUTTON:
            break;          /* painted with the frame, and never changes */
        }
    }

    s_have_shown = true;
    ngl_flush();
}

/** The frame and then the contents, which is what a page change costs. */
static void repaint_tab(void)
{
    rows_layout();
    paint_page();
    tick();
}

/* ------------------------------------------------------------------ */
/* Tasks                                                              */
/* ------------------------------------------------------------------ */

/*
 * Every task in the machine, and a way to end one.
 *
 * Not a table of rows like the other tabs, because it is not a fixed list of
 * readings - it is however many tasks happen to be running, which is thirty-odd
 * and changes while you are looking at it. So this tab paints itself: one line
 * per task, a page at a time, with its own cache so that a page of thirty lines
 * costs the two or three cells that actually moved.
 *
 * The order comes from NeOS and is creation order, which never changes while a
 * task lives. That is not a detail: every row here has a kill button on it, and
 * a list that re-sorted itself by CPU twice a second would be a list where the
 * row under your finger is not the row you aimed at.
 */

#define TASK_ROW_H     44
#define MAX_TASK_ROWS  26
#define TASK_HEAD_H    34
#define TASK_FOOT_H    48

/* Columns, measured in from the right edge of the row. The name gets whatever
   is left, which in portrait is still twice what a task name needs. */
#define TASK_BTN_W     64
#define TASK_STACK_W   116
#define TASK_CPU_W     84
#define TASK_STATE_W   112

/*
 * A kill is two taps, and the second one has to be soon.
 *
 * Deleting a task is not undoable and cannot be made so - FreeRTOS abandons
 * whatever the task was holding - so the button arms first and kills second. The
 * timeout is what keeps an armed row from lying in wait: come back to this page
 * a minute later and nothing is armed, because nothing should be.
 */
#define TASK_ARM_MS    3000

/* How often the list is actually re-read. The page ticks four times as often as
   this, and NeOS only recomputes the CPU share once a second anyway - walking
   every TCB at tick rate would be paying for a figure that cannot have changed. */
#define TASK_REREAD_MS 450

static neos_task_t s_tasks[NEOS_TASKS_MAX];
static int         s_ntasks;
static int         s_task_page;
static int         s_task_fit = 1;      /* rows that fit on the page right now */

static uint32_t s_armed_id;
static uint64_t s_armed_at;

/* What is currently on each line, so a tick can tell whether to repaint it. */
static char s_task_shown[MAX_TASK_ROWS][80];
static int  s_task_visible;             /* lines with something on them */
static int  s_shown_ntasks = -1;
static int  s_shown_page    = -1;

static ngl_rect_t tasks_area(void)
{
    const int16_t y = (int16_t)(s_area.y + tabs_h() + 8);
    return ngl_rect((int16_t)(s_area.x + MARGIN), y,
                    (int16_t)(s_area.w - 2 * MARGIN),
                    (int16_t)(s_area.h - (y - s_area.y) - 8));
}

static ngl_rect_t task_row_rect(int i)
{
    const ngl_rect_t a = tasks_area();
    return ngl_rect(a.x, (int16_t)(a.y + TASK_HEAD_H + i * TASK_ROW_H),
                    a.w, TASK_ROW_H);
}

static ngl_rect_t task_foot_rect(void)
{
    const ngl_rect_t a = tasks_area();
    return ngl_rect(a.x, (int16_t)(a.y + a.h - TASK_FOOT_H), a.w, TASK_FOOT_H);
}

/** The [x] button on a row, whether or not the row is allowed one. */
static ngl_rect_t task_btn_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + row->w - TASK_BTN_W),
                    (int16_t)(row->y + (TASK_ROW_H - 32) / 2), TASK_BTN_W, 32);
}

/** The page button, or an empty rectangle when everything fits on one page. */
static ngl_rect_t task_page_rect(void)
{
    const ngl_rect_t f = task_foot_rect();
    if (s_ntasks <= s_task_fit) {
        return ngl_rect(0, 0, 0, 0);
    }
    return ngl_rect((int16_t)(f.x + f.w - 160), (int16_t)(f.y + 6), 160, 36);
}

static int task_pages(void)
{
    if (s_task_fit <= 0) {
        return 1;
    }
    const int p = (s_ntasks + s_task_fit - 1) / s_task_fit;
    return p < 1 ? 1 : p;
}

static bool task_armed(const neos_task_t *t)
{
    return s_armed_id == t->id && t->id != 0 &&
           (neos_uptime_ms() - s_armed_at) < TASK_ARM_MS;
}

/** One line, as text, for the "has this changed" comparison. */
static void task_key(char *buf, int n, const neos_task_t *t)
{
    snprintf(buf, n, "%s|%u|%d|%u|%u|%u|%u|%d",
             t->name, (unsigned)t->prio, (int)t->core, (unsigned)t->state,
             (unsigned)t->cpu_permille, (unsigned)(t->stack_free >> 6),
             (unsigned)t->flags, task_armed(t) ? 1 : 0);
}

static void paint_task_row(ngl_surface_t *sc, int i, const neos_task_t *t)
{
    const ngl_rect_t row = task_row_rect(i);
    ngl_fill_rect(sc, row, TH_BG);

    const int16_t ty = (int16_t)(row.y + (TASK_ROW_H - ngl_font_small.height) / 2);
    const int16_t right = (int16_t)(row.x + row.w);

    const int16_t btn_x   = (int16_t)(right - TASK_BTN_W);
    const int16_t stack_x = (int16_t)(btn_x - TASK_STACK_W);
    const int16_t cpu_x   = (int16_t)(stack_x - TASK_CPU_W);
    const int16_t state_x = (int16_t)(cpu_x - TASK_STATE_W);

    /*
     * A service is one of ours and is named in the accent colour; a protected
     * task is one nobody may touch and is dimmed. The colour is the same
     * information the missing button carries, said in a way that reads down a
     * column of thirty rows without hunting for gaps.
     */
    ngl_color_t nc = TH_TEXT;
    if (t->flags & NEOS_TASK_SERVICE) {
        nc = TH_ACCENT;
    } else if (t->flags & NEOS_TASK_PROTECTED) {
        nc = TH_TEXT_DIM;
    }

    ngl_rect_t namebox = ngl_rect(row.x, row.y, (int16_t)(state_x - row.x - 8),
                                  TASK_ROW_H);
    ngl_clip_set(sc, &namebox);
    int16_t nx = ngl_text(sc, row.x, ty, t->name, &ngl_font_small, nc);

    /* Priority and core, right after the name rather than in columns of their
       own: they are what you look at once, when a row has already caught your
       eye for some other reason. */
    char pc[16];
    if (t->core < 0) {
        snprintf(pc, sizeof(pc), "  p%u", (unsigned)t->prio);
    } else {
        snprintf(pc, sizeof(pc), "  p%u c%d", (unsigned)t->prio, (int)t->core);
    }
    ngl_text(sc, nx, ty, pc, &ngl_font_small, TH_TEXT_FAINT);
    ngl_clip_set(sc, NULL);

    ngl_rect_t box = ngl_rect(state_x, row.y, TASK_STATE_W, TASK_ROW_H);
    text_right(sc, &box, neos_task_state_name(t->state), TH_TEXT_DIM);

    char buf[24];
    /*
     * Tenths of a percent, because most of this list is at zero and the
     * interesting rows are at a tenth or two - a whole-percent column would show
     * thirty noughts and one number, which says less than it looks like it does.
     */
    snprintf(buf, sizeof(buf), "%u.%u%%",
             (unsigned)(t->cpu_permille / 10), (unsigned)(t->cpu_permille % 10));
    box = ngl_rect(cpu_x, row.y, TASK_CPU_W, TASK_ROW_H);
    text_right(sc, &box, buf, t->cpu_permille >= 100 ? TH_WARN : TH_TEXT);

    fmt_bytes(buf, sizeof(buf), t->stack_free);
    box = ngl_rect(stack_x, row.y, TASK_STACK_W, TASK_ROW_H);
    text_right(sc, &box, buf, t->stack_free < 512 ? TH_BAD : TH_TEXT_DIM);

    const ngl_rect_t btn = task_btn_rect(&row);
    if (!(t->flags & NEOS_TASK_PROTECTED)) {
        const bool armed = task_armed(t);
        ngl_fill_round_rect(sc, btn, 6, armed ? TH_BAD : TH_CLOSE_FILL);
        ngl_draw_round_rect(sc, btn, 6, armed ? TH_BAD : TH_CLOSE_LINE, 2);
        text_centred(sc, &btn, armed ? "sure?" : "x", armed ? TH_BG : TH_CLOSE_X);
    }

    ngl_hline(sc, row.x, (int16_t)(row.y + TASK_ROW_H - 1), row.w, TH_RULE);
}

static void paint_task_foot(ngl_surface_t *sc)
{
    const ngl_rect_t f = task_foot_rect();
    ngl_fill_rect(sc, f, TH_BG);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d task%s, %d service%s", s_ntasks,
             s_ntasks == 1 ? "" : "s",
             neos_service_count(), neos_service_count() == 1 ? "" : "s");
    ngl_text(sc, f.x, (int16_t)(f.y + (TASK_FOOT_H - ngl_font_small.height) / 2 + 4),
             buf, &ngl_font_small, TH_TEXT_DIM);

    const ngl_rect_t pg = task_page_rect();
    if (pg.w > 0) {
        snprintf(buf, sizeof(buf), "page %d/%d", s_task_page + 1, task_pages());
        button_box(sc, pg, buf);
    }
}

static void tasks_read(void)
{
    s_ntasks = neos_tasks(s_tasks, NEOS_TASKS_MAX);
    if (s_task_page >= task_pages()) {
        s_task_page = task_pages() - 1;
    }
    if (s_task_page < 0) {
        s_task_page = 0;
    }
}

/** The frame: the column headings, and the room the rows will go in. */
static void tasks_paint(ngl_surface_t *sc)
{
    const ngl_rect_t a = tasks_area();

    s_task_fit = (a.h - TASK_HEAD_H - TASK_FOOT_H) / TASK_ROW_H;
    if (s_task_fit < 1)             { s_task_fit = 1; }
    if (s_task_fit > MAX_TASK_ROWS) { s_task_fit = MAX_TASK_ROWS; }

    const int16_t right = (int16_t)(a.x + a.w);
    const int16_t btn_x   = (int16_t)(right - TASK_BTN_W);
    const int16_t stack_x = (int16_t)(btn_x - TASK_STACK_W);
    const int16_t cpu_x   = (int16_t)(stack_x - TASK_CPU_W);
    const int16_t state_x = (int16_t)(cpu_x - TASK_STATE_W);
    const int16_t hy = (int16_t)(a.y + (TASK_HEAD_H - ngl_font_small.height) / 2);

    ngl_text(sc, a.x, hy, "task", &ngl_font_small, TH_TEXT_FAINT);

    ngl_rect_t box = ngl_rect(state_x, a.y, TASK_STATE_W, TASK_HEAD_H);
    text_right(sc, &box, "state", TH_TEXT_FAINT);
    box = ngl_rect(cpu_x, a.y, TASK_CPU_W, TASK_HEAD_H);
    text_right(sc, &box, "cpu", TH_TEXT_FAINT);
    box = ngl_rect(stack_x, a.y, TASK_STACK_W, TASK_HEAD_H);
    text_right(sc, &box, "stack left", TH_TEXT_FAINT);

    ngl_hline(sc, a.x, (int16_t)(a.y + TASK_HEAD_H - 1), a.w, TH_RULE);

    /* Nothing is cached for a page that has just been drawn empty. */
    s_task_visible  = 0;
    s_shown_ntasks  = -1;
    s_shown_page    = -1;
}

static void tasks_tick(ngl_surface_t *sc)
{
    static uint32_t since = TASK_REREAD_MS;      /* read on the first tick */

    since += TICK_MS;
    if (since >= TASK_REREAD_MS) {
        since = 0;
        tasks_read();
    }

    const int from = s_task_page * s_task_fit;
    int shown = s_ntasks - from;
    if (shown < 0)            { shown = 0; }
    if (shown > s_task_fit)   { shown = s_task_fit; }

    for (int i = 0; i < shown; i++) {
        char key[80];
        task_key(key, sizeof(key), &s_tasks[from + i]);
        if (i < s_task_visible && strcmp(s_task_shown[i], key) == 0) {
            continue;
        }
        snprintf(s_task_shown[i], sizeof(s_task_shown[i]), "%s", key);
        paint_task_row(sc, i, &s_tasks[from + i]);
    }

    /* A task that has gone leaves a line behind it. Clearing is the only way
       back: nothing else on this page ever paints over that row. */
    for (int i = shown; i < s_task_visible; i++) {
        ngl_fill_rect(sc, task_row_rect(i), TH_BG);
    }
    s_task_visible = shown;

    if (s_ntasks != s_shown_ntasks || s_task_page != s_shown_page) {
        s_shown_ntasks = s_ntasks;
        s_shown_page   = s_task_page;
        paint_task_foot(sc);
    }
}

static bool tasks_tap(int16_t x, int16_t y)
{
    const ngl_rect_t pg = task_page_rect();
    if (pg.w > 0 && ngl_rect_contains(&pg, x, y)) {
        s_task_page = (s_task_page + 1) % task_pages();
        s_armed_id  = 0;
        repaint_tab();
        return true;
    }

    const int from = s_task_page * s_task_fit;
    for (int i = 0; i < s_task_visible; i++) {
        const ngl_rect_t row = task_row_rect(i);
        if (!ngl_rect_contains(&row, x, y)) {
            continue;
        }
        neos_task_t *t = &s_tasks[from + i];
        const ngl_rect_t btn = task_btn_rect(&row);

        if ((t->flags & NEOS_TASK_PROTECTED) || !ngl_rect_contains(&btn, x, y)) {
            /* A tap on the row but not on its button is how an armed row is
               put back - the safe gesture has to be the easy one. */
            s_armed_id = 0;
            repaint_tab();
            return true;
        }

        if (task_armed(t)) {
            char msg[64];
            const bool svc = (t->flags & NEOS_TASK_SERVICE) != 0;
            if (neos_task_kill(t->id)) {
                snprintf(msg, sizeof(msg), "%s %s", t->name,
                         svc ? "asked to stop" : "deleted");
            } else {
                snprintf(msg, sizeof(msg), "%s would not go", t->name);
            }
            neos_status_for(msg, 2500);
            s_armed_id = 0;
            tasks_read();
            repaint_tab();
            return true;
        }

        s_armed_id = t->id;
        s_armed_at = neos_uptime_ms();
        paint_task_row(ngl_screen(), i, t);
        ngl_flush();
        return true;
    }

    s_armed_id = 0;
    return true;        /* the tab owns every tap inside its own area */
}

/* ------------------------------------------------------------------ */
/* A service, so that the thing this page can stop can also be started */
/* ------------------------------------------------------------------ */

/*
 * A tone that keeps playing after this app has gone.
 *
 * It is here because the alternative was an ABI nothing on the card exercises.
 * Leaving a task behind is the one part of NeOS that cannot be checked by
 * looking at a screen - the whole claim is about what happens after the screen
 * belongs to somebody else - so the check is: start this, leave the app, and
 * listen. Stop it from the list above, or wait for it to finish on its own.
 *
 * Sixteen samples of a sine and a phase accumulator, because apps have no libm
 * and a test tone does not need one. Q12 on the increment gets 440 Hz to within
 * a fraction of a hertz at the one rate the codec runs at.
 */
#define TONE_HZ        440
#define TONE_SECONDS   30
#define TONE_BLOCK     480      /* 10 ms, so a stop is noticed inside one */

static const int16_t TONE_WAVE[16] = {
        0,  3062,  5657,  7391,  8000,  7391,  5657,  3062,
        0, -3062, -5657, -7391, -8000, -7391, -5657, -3062,
};

static void tone_service(void *arg)
{
    (void)arg;

    if (!neos_audio_open()) {
        printf("[system] tone service: the codec would not start\n");
        return;
    }
    neos_audio_gain(60);

    static int16_t block[TONE_BLOCK];
    const uint32_t inc = (uint32_t)TONE_HZ * 16u * 4096u / NEOS_AUDIO_RATE;
    uint32_t phase = 0;

    const int blocks = TONE_SECONDS * NEOS_AUDIO_RATE / TONE_BLOCK;
    for (int b = 0; b < blocks && !neos_service_stopping(); b++) {
        for (int i = 0; i < TONE_BLOCK; i++) {
            block[i] = TONE_WAVE[(phase >> 12) & 15];
            phase += inc;
        }
        if (neos_audio_write(block, TONE_BLOCK) < 0) {
            break;
        }
    }

    neos_audio_close();
    printf("[system] tone service done\n");
}

/*
 * No "playing" state on the row, deliberately.
 *
 * The app cannot know when the tone has finished without asking NeOS for the
 * task list on every tick of a tab that has nothing to do with tasks, and a row
 * that said "playing" for twenty seconds after the sound stopped would be worse
 * than a row that says nothing. What is running is a question the TASKS tab
 * answers, and it is one tab away.
 */
static void tone_tap(void)
{
    if (neos_service_start("systone", tone_service, NULL, 8192)) {
        neos_status_for("playing in the background - leave the app and listen", 3000);
    } else {
        neos_status_for("no free service slot - stop one in TASKS", 2500);
    }
}


/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/**
 * A press inside a slider track, dragged.
 *
 * Handled from the live touch rather than from taps, because a slider that
 * only moves on release is a slider you have to aim at. Returns true while it
 * owns the finger, which is what keeps the tap it eventually produces from
 * being read a second time as a tap on the row.
 */
static bool drag_slider(void)
{
    static int s_drag = -1;         /* row being dragged, -1 for none */

    neos_touch_t t = {0};
    if (!neos_touch(&t) || !t.down) {
        const bool was = s_drag >= 0;
        s_drag = -1;
        return was;
    }

    const tab_t *tab = &TABS[s_tab];
    if (s_drag < 0) {
        for (int i = 0; i < tab->nrows; i++) {
            if (tab->rows[i].kind != R_SLIDER) {
                continue;
            }
            /* Generous vertically: the track is 10 px tall and a fingertip is
               not, so the whole row grabs it. */
            const ngl_rect_t row = row_rect(i);
            if (ngl_rect_contains(&row, t.x, t.y)) {
                s_drag = i;
                break;
            }
        }
        if (s_drag < 0) {
            return false;
        }
    }

    /*
     * The tab can change under a held finger - a drag that started on the
     * backlight row, then a second finger on a tab - and the row index means
     * something different on every page. Dropping the drag is the only answer
     * that cannot end up writing to whatever control now sits at that index.
     */
    if (s_drag >= tab->nrows || tab->rows[s_drag].kind != R_SLIDER) {
        s_drag = -1;
        return false;
    }

    const ngl_rect_t row = row_rect(s_drag);
    const ngl_rect_t tr = track_rect(&row);
    const row_t *r = &tab->rows[s_drag];
    const int max = slider_max(r);
    /*
     * Rounded to the nearest step, not truncated. On a 0-100 track that is a
     * pixel either way and nobody could tell; on an eleven-position one it is
     * the difference between a knob that lands where the finger is and one that
     * always reads a minute short.
     */
    int v = tr.w > 0 ? (int)((((int32_t)t.x - tr.x) * max + tr.w / 2) / tr.w) : 0;
    if (v < 0)   { v = 0; }
    if (v > max) { v = max; }

    if (r->iset) {
        r->iset(v);
    }
    return true;
}

static void handle_tap(int16_t x, int16_t y)
{
    s_taps++;

    for (int i = 0; i < NTABS; i++) {
        const ngl_rect_t t = tab_rect(i);
        if (ngl_rect_contains(&t, x, y)) {
            if (i != s_tab) {
                s_tab = i;
                /* Opening the card tab is the moment to walk the FAT once,
                   rather than on every tick or never. */
                if (TABS[i].rows == ROWS_SD) {
                    sd_measure();
                }
                repaint_tab();
            }
            return;
        }
    }

    const tab_t *tab = &TABS[s_tab];
    if (tab->tap && tab->tap(x, y)) {
        return;
    }

    for (int i = 0; i < tab->nrows; i++) {
        const ngl_rect_t row = row_rect(i);
        if (!ngl_rect_contains(&row, x, y)) {
            continue;
        }
        const row_t *r = &tab->rows[i];
        if (r->kind == R_TOGGLE && r->get && r->set) {
            /* The whole row is the hit area, not just the 88 px switch. There
               is nothing else on the row to hit by mistake. */
            r->set(!r->get());
        } else if ((r->kind == R_ACTION || r->kind == R_BUTTON) && r->act) {
            r->act();
        }
        return;
    }
}

/* ------------------------------------------------------------------ */

static bool area_moved(void)
{
    const ngl_rect_t a = ngl_app_area();
    return a.x != s_area.x || a.y != s_area.y || a.w != s_area.w || a.h != s_area.h;
}

int main(int argc, char **argv)
{
    printf("[system] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[system] no screen, nothing to show\n");
        return 0;
    }

    s_area = ngl_app_area();
    tabs_layout();
    repaint_tab();

    /* Once, up front. Everything else on the page is a register read; this one
       walks the bus, so it happens where a pause does not look like a stall. */
    i2c_rescan();

    uint32_t since = 0;

    while (!neos_app_close_requested()) {
        /* NeOS can rotate the screen underneath a running app and nothing
           reports it, so the app area is watched instead. */
        if (area_moved()) {
            s_area = ngl_app_area();
            /* A rotation changes how many tabs fit on a row, so the strip is
               re-measured before anything is placed against it - and then the
               rows, which get whatever height is left under it. */
            tabs_layout();
            printf("[system] screen changed to %dx%d, relaying out\n",
                   s_area.w, s_area.h);
            repaint_tab();
        }

        const bool dragging = drag_slider();

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty) && !dragging) {
            handle_tap(tx, ty);
        }

        /*
         * Readings refresh on a clock; a drag does not wait for it. Making the
         * knob catch up 150 ms after the finger would read as a broken slider,
         * and repainting one track is cheap enough to do at input rate.
         */
        if (dragging || since >= TICK_MS) {
            tick();
            since = 0;
        }

        /* The yield is unconditional. Polling touch is the whole event loop,
           so without it this app is a busy wait on one of the two cores. */
        neos_sleep_ms(POLL_MS);
        since += POLL_MS;
    }

    printf("[system] closing\n");
    return 0;
}
