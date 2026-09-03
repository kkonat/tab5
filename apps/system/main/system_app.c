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

#define MAX_ROWS    10

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    R_VALUE,    /**< label, and a string that is recomputed every tick */
    R_TOGGLE,   /**< label, and a switch */
    R_SLIDER,   /**< label, a 0-100 track, and the number */
    R_ACTION,   /**< label, a value, and a tap that does something */
} rkind_t;

typedef struct {
    rkind_t     kind;
    const char *label;

    void (*text)(char *buf, int n);     /* R_VALUE, R_ACTION */
    bool (*get)(void);                  /* R_TOGGLE */
    void (*set)(bool on);
    int  (*iget)(void);                 /* R_SLIDER */
    void (*iset)(int v);
    void (*act)(void);                  /* R_ACTION */
} row_t;

typedef struct {
    const char  *name;
    const row_t *rows;
    int          nrows;
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

#define NTABS 7

static const tab_t TABS[NTABS] = {
    { "IO",      ROWS_IO,      (int)(sizeof(ROWS_IO)      / sizeof(row_t)) },
    { "SENSORS", ROWS_SENSORS, (int)(sizeof(ROWS_SENSORS) / sizeof(row_t)) },
    { "POWER",   ROWS_POWER,   (int)(sizeof(ROWS_POWER)   / sizeof(row_t)) },
    { "PERIPH",  ROWS_PERIPH,  (int)(sizeof(ROWS_PERIPH)  / sizeof(row_t)) },
    { "NET",     ROWS_NET,     (int)(sizeof(ROWS_NET)     / sizeof(row_t)) },
    { "SD",      ROWS_SD,      (int)(sizeof(ROWS_SD)      / sizeof(row_t)) },
    { "SYSTEM",  ROWS_SYSTEM,  (int)(sizeof(ROWS_SYSTEM)  / sizeof(row_t)) },
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
                    (int16_t)(s_area.y + tabs_h() + 8 + i * ROW_H),
                    (int16_t)(s_area.w - 2 * MARGIN), ROW_H);
}

/** Where a value string is drawn, right-aligned. Also the repaint unit. */
static ngl_rect_t val_rect(const ngl_rect_t *row)
{
    const int16_t w = (int16_t)(row->w - SLIDER_X);
    return ngl_rect((int16_t)(row->x + SLIDER_X), (int16_t)(row->y + 8),
                    w, (int16_t)(ROW_H - 16));
}

static ngl_rect_t switch_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + row->w - SW_W),
                    (int16_t)(row->y + (ROW_H - SW_H) / 2), SW_W, SW_H);
}

static ngl_rect_t track_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + SLIDER_X),
                    (int16_t)(row->y + (ROW_H - SLIDER_H) / 2),
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

static void paint_slider(ngl_surface_t *sc, const ngl_rect_t *row, int v)
{
    if (v < 0)   { v = 0; }
    if (v > 100) { v = 100; }

    const ngl_rect_t tr = track_rect(row);
    const int16_t r = (int16_t)(SLIDER_H / 2);

    /* The knob overhangs the track at both ends and the percentage sits past
       it, so the cleared area is the row from the track leftwards, not the
       track. */
    ngl_fill_rect(sc, ngl_rect((int16_t)(tr.x - 12), (int16_t)(row->y + 4),
                               (int16_t)(row->w - SLIDER_X + 12),
                               (int16_t)(ROW_H - 8)), TH_BG);
    ngl_fill_round_rect(sc, tr, r, TH_RULE);

    const int16_t fw = (int16_t)((int32_t)tr.w * v / 100);
    if (fw > 0) {
        ngl_fill_round_rect(sc, ngl_rect(tr.x, tr.y, fw, tr.h), r, TH_ACCENT);
    }

    const ngl_rect_t knob = ngl_rect((int16_t)(tr.x + fw - 11),
                                     (int16_t)(tr.y - 6), 22, 22);
    ngl_fill_round_rect(sc, knob, 11, TH_GLOW);

    char n[8];
    snprintf(n, sizeof(n), "%d%%", v);
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
    for (int i = 0; i < t->nrows; i++) {
        const ngl_rect_t row = row_rect(i);
        ngl_text(sc, (int16_t)(row.x + 4),
                 (int16_t)(row.y + (ROW_H - ngl_font_small.height) / 2),
                 t->rows[i].label, &ngl_font_small, TH_TEXT_DIM);
        ngl_hline(sc, row.x, (int16_t)(row.y + ROW_H - 1), row.w, TH_RULE);
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
            paint_slider(sc, &row, v);
            break;
        }
        }
    }

    s_have_shown = true;
    ngl_flush();
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
    int v = tr.w > 0 ? (int)(((int32_t)t.x - tr.x) * 100 / tr.w) : 0;
    if (v < 0)   { v = 0; }
    if (v > 100) { v = 100; }

    const row_t *r = &tab->rows[s_drag];
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
                paint_page();
                tick();
            }
            return;
        }
    }

    const tab_t *tab = &TABS[s_tab];
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
        } else if (r->kind == R_ACTION && r->act) {
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
    paint_page();
    tick();

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
               re-measured before anything is placed against it. */
            tabs_layout();
            printf("[system] screen changed to %dx%d, relaying out\n",
                   s_area.w, s_area.h);
            paint_page();
            tick();
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
