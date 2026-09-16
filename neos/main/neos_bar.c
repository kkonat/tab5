/*
 * The system bar.
 *
 * Owned by NeOS, not by the shell. It used to belong to whichever app was the
 * shell, which meant it vanished the moment that app handed over - and a
 * launched app was then on a bare screen with no way back, because the only
 * close button was drawn by an app that was no longer resident.
 *
 * Keeping it here also means there is exactly one close button, one clock and
 * one Wi-Fi icon, at one place, that every app gets for free and no app can
 * draw over: ngl reserves the strip and refuses to clip anything else into it.
 *
 * Everything below is in screen coordinates, which is the rotated frame - the
 * bar is the top of the screen, not the top of the panel, so it and its
 * widgets follow the tablet around without anything special happening.
 */
#include <stdio.h>
#include <string.h>

#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "neos_build.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_bar.h"
#include "neos_boot.h"
#include "neos_net.h"
#include "neos_screen.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"

static const char *TAG = "bar";

#define BAR_H   56
#define BAR_BTN 40
#define BAR_PAD 8

/* How often the widgets are looked at, and the period the history averages
   over. One constant, because the timer below and the accumulator in
   battery_sample() have to agree about how long a tick is. */
#define TICK_MS 500

/* The version badge: a rounded outline around the build string. */
#define VER_H   38
#define VER_PAD 10
#define VER_GAP 10

/* The widgets on the right, between the status line and the close button. */
#define WIFI_W    40
#define CLOCK_W   92
#define BATTERY_W 40
#define WID_GAP   6

/*
 * Which build this is, as it goes in the badge.
 *
 * A counter bumped once per build (neos/cmake/bump_build.cmake), not a git
 * hash: the tablet spends most of its life running something that has not
 * been committed, so a hash names the wrong image exactly when you are trying
 * to work out whether the thing on the glass is the thing you just built. A
 * number that always moves forward answers that at a glance.
 */
static const char *build_version(void)
{
    static char buf[16];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "v0.%d", NEOS_BUILD);
    }
    return buf;
}

static ngl_rect_t version_rect(void)
{
    const int16_t w = (int16_t)(ngl_text_width(&ngl_font_small, build_version())
                                + 2 * VER_PAD);
    const int16_t x = (int16_t)(14 + ngl_text_width(&ngl_font_small, "NeOS") + VER_GAP);
    return ngl_rect(x, (int16_t)((BAR_H - VER_H) / 2), w, VER_H);
}

/*
 * The close button only exists while there is something worth closing. The
 * shell is relaunched the instant it returns, so an X on the shell would look
 * like a button that redraws the screen and does nothing else.
 */
static bool s_closable;

int16_t neos_bar_height(void)
{
    return BAR_H;
}

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

/*
 * Widgets claim fixed space at the edges and the status line takes whatever is
 * left, so adding a clock or a battery gauge shrinks the status area instead
 * of drawing on top of it. That is why the geometry is computed rather than
 * written down as constants.
 *
 * Right to left: the close button, the clock, the Wi-Fi icon. The close button
 * stays hard against the corner whether or not the other two are there,
 * because it is the one a user reaches for without looking.
 */
static ngl_rect_t close_rect(void)
{
    const ngl_rect_t bar = ngl_bar_rect();
    return ngl_rect((int16_t)(bar.w - BAR_BTN - BAR_PAD),
                    (int16_t)((bar.h - BAR_BTN) / 2), BAR_BTN, BAR_BTN);
}

static ngl_rect_t clock_rect(void)
{
    const ngl_rect_t bar = ngl_bar_rect();
    const int16_t right = (int16_t)(bar.w - BAR_PAD - (s_closable ? BAR_BTN + WID_GAP : 0));
    return ngl_rect((int16_t)(right - CLOCK_W), 0, CLOCK_W, bar.h);
}

static ngl_rect_t wifi_rect(void)
{
    const ngl_rect_t c = clock_rect();
    return ngl_rect((int16_t)(c.x - WID_GAP - WIFI_W), 0, WIFI_W, c.h);
}

static ngl_rect_t battery_rect(void)
{
    const ngl_rect_t w = wifi_rect();
    return ngl_rect((int16_t)(w.x - WID_GAP - BATTERY_W), 0, BATTERY_W, w.h);
}

static ngl_rect_t widgets_rect(void)
{
    const ngl_rect_t b = battery_rect();
    const ngl_rect_t w = wifi_rect();
    const ngl_rect_t c = clock_rect();
    const ngl_rect_t bw = ngl_rect_union(&b, &w);
    return ngl_rect_union(&bw, &c);
}

static int16_t left_reserved(void)
{
    const ngl_rect_t v = version_rect();
    return (int16_t)(v.x + v.w);
}

static ngl_rect_t status_rect(void)
{
    const ngl_rect_t bar = ngl_bar_rect();
    const int16_t l = (int16_t)(left_reserved() + 16);
    int16_t w = (int16_t)(battery_rect().x - WID_GAP - l);
    if (w < 0) {
        w = 0;
    }
    return ngl_rect(l, (int16_t)((bar.h - ngl_font_small.height) / 2),
                    w, ngl_font_small.height);
}

neos_bar_hit_t neos_bar_hit(int16_t x, int16_t y)
{
    /*
     * No bar, no buttons - and this has to be said here rather than left to
     * the rectangles, because they do not all collapse when the bar does.
     * close_rect() takes its height from BAR_BTN and only its position from
     * the bar, so at zero height it is still a real rectangle straddling the
     * top-right corner: an invisible close button sitting inside a fullscreen
     * app, in the exact place a game is most likely to put its own.
     */
    if (ngl_bar_height() <= 0) {
        return NEOS_BAR_NONE;
    }
    ngl_rect_t r = close_rect();
    if (s_closable && ngl_rect_contains(&r, x, y)) {
        return NEOS_BAR_CLOSE;
    }
    r = wifi_rect();
    if (ngl_rect_contains(&r, x, y)) {
        return NEOS_BAR_WIFI;
    }
    r = clock_rect();
    if (ngl_rect_contains(&r, x, y)) {
        return NEOS_BAR_CLOCK;
    }
    r = battery_rect();
    if (ngl_rect_contains(&r, x, y)) {
        return NEOS_BAR_BATTERY;
    }
    return NEOS_BAR_NONE;
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

/*
 * The Wi-Fi glyph is one icon in three colours rather than three icons.
 *
 * There is only one Wi-Fi symbol in the icon set and it is the right one; what
 * differs between "no network", "trying" and "online" is not the shape but how
 * present it looks, which is exactly what a tint is for. Grey is not a mood
 * here - it is the documented way this bar says a thing is not available, the
 * same as a disabled row anywhere else.
 */
static ngl_color_t wifi_tint(neos_net_state_t st)
{
    switch (st) {
    case NEOS_NET_ONLINE:     return TH_OK;
    case NEOS_NET_CONNECTING: return TH_WARN;
    default:                  return TH_TEXT_FAINT;
    }
}

static void paint_wifi(ngl_surface_t *s)
{
    const ngl_rect_t r = wifi_rect();
    ngl_fill_rect(s, r, TH_BAR_BG);

    const ngl_icon_t *ic = ngl_icon_find("wifi", 24);
    if (!ic) {
        return;
    }
    ngl_icon(s, (int16_t)(r.x + (r.w - ic->w) / 2),
             (int16_t)(r.y + (r.h - ic->h) / 2), ic, wifi_tint(neos_net_state()));
}

/* ------------------------------------------------------------------ */
/* The pack, read once and remembered                                  */
/* ------------------------------------------------------------------ */

/*
 * One reading a tick, kept for everyone who wants it.
 *
 * The icon and the widget refresh used to call neos_power_read() separately,
 * which was two I2C round trips for the same register twice a second and, more
 * to the point, two answers: the refresh could decide the pack was in danger
 * and then the paint that followed could read a different millivolt on the
 * other side of the threshold and draw it healthy. Reading once and sharing
 * the result is what makes the icon and the panel agree by construction.
 */
static neos_power_t s_last;
static bool         s_last_ok;      /* ... and there is a pack on the terminals */
static bool         s_monitor_ok;   /* the INA226 answered, pack or no pack */

static bool battery_last(neos_power_t *out)
{
    if (!s_last_ok) {
        return false;
    }
    *out = s_last;
    return true;
}

bool neos_battery_monitor_ok(void)
{
    return s_monitor_ok;
}

/*
 * The history behind the panel's plot. See neos_bar.h for the shape of it.
 *
 * s_hist is a ring: oldest at s_hist_head once it has wrapped, at 0 before
 * that. Readings are accumulated across the period and committed as a mean,
 * and a period the monitor never answered in commits a zero, which the plot
 * draws as a break in the trace rather than a dive to the floor.
 */
static uint16_t *s_hist;        /* NEOS_BATTERY_HIST_N entries, in PSRAM */
static int      s_hist_n;
static int      s_hist_head;
static uint32_t s_hist_seq;

/** The ring, once. Without it nothing is recorded and the plot says so. */
static void hist_init(void)
{
    if (s_hist) {
        return;
    }
    s_hist = heap_caps_calloc(NEOS_BATTERY_HIST_N, sizeof(uint16_t),
                              MALLOC_CAP_SPIRAM);
    if (!s_hist) {
        ESP_LOGE(TAG, "no PSRAM for %d history points - not recording",
                 NEOS_BATTERY_HIST_N);
    }
}

/** Logical index, 0 being the oldest reading held, to the slot it lives in. */
static uint16_t hist_at(int j)
{
    const int start = (s_hist_n == NEOS_BATTERY_HIST_N) ? s_hist_head : 0;
    return s_hist[(start + j) % NEOS_BATTERY_HIST_N];
}

static int64_t  s_acc_mv;
static int      s_acc_n;
static uint32_t s_acc_ms;

/*
 * How often the record is committed to flash, and how much of it goes.
 *
 * The ring is 34 KB and the whole NVS partition is 64 KB, so what is kept across
 * a reboot cannot be the ring - a blob that size needs room for two copies while
 * it is rewritten and there is not room for one. What is kept is the same two
 * days at one point a minute, which is six kilobytes, and the ten-second detail
 * is lost at the reboot rather than the trace.
 *
 * Ten minutes between commits rather than five, because the blob is eight times
 * bigger than it used to be and the thing being protected is a two-day record:
 * losing the last ten minutes of it to a power cut is not a loss worth a
 * megabyte a day of flash writes.
 */
#define SAVE_EVERY_MS 600000

/* One stored point per minute, so six ring slots to each. */
#define STORE_DECIM (60000 / NEOS_BATTERY_HIST_PERIOD_MS)
#define STORE_N     (NEOS_BATTERY_HIST_N / STORE_DECIM)
static uint32_t s_save_ms;

/* Defined below, with the charge curve they depend on. */
static void capacity_track(void);
static void battery_store_save(void);

static void hist_push(uint16_t mv)
{
    if (!s_hist) {
        return;
    }
    s_hist[s_hist_head] = mv;
    s_hist_head = (s_hist_head + 1) % NEOS_BATTERY_HIST_N;
    if (s_hist_n < NEOS_BATTERY_HIST_N) {
        s_hist_n++;
    }
    s_hist_seq++;
}

/*
 * Called from the timer and not from neos_bar_widgets_refresh(), which returns
 * early while a panel is up: a history that stopped recording whenever the
 * panel was open would be missing exactly the stretch somebody opened the
 * panel to look at.
 */
static void battery_sample(void)
{
    neos_power_t p = {0};
    s_monitor_ok = neos_power_read(&p);
    /*
     * A reading below the floor is the pack being absent, not the pack being
     * empty - see NEOS_BATTERY_MIN_MV. Dropping it here rather than at each
     * use is what keeps the icon, the status line, the percentage and the plot
     * from each having their own opinion about whether there is a battery.
     */
    s_last_ok = s_monitor_ok && p.bus_mv >= NEOS_BATTERY_MIN_MV;
    s_last    = p;

    capacity_track();

    if (s_last_ok) {
        s_acc_mv += p.bus_mv;
        s_acc_n++;
    }

    s_save_ms += TICK_MS;
    if (s_save_ms >= SAVE_EVERY_MS) {
        s_save_ms = 0;
        battery_store_save();
    }

    s_acc_ms += TICK_MS;
    if (s_acc_ms < NEOS_BATTERY_HIST_PERIOD_MS) {
        return;
    }
    s_acc_ms = 0;

    uint16_t mv = 0;
    if (s_acc_n > 0) {
        const int64_t mean = s_acc_mv / s_acc_n;
        mv = (mean > UINT16_MAX) ? UINT16_MAX : (uint16_t)mean;
    }
    s_acc_mv = 0;
    s_acc_n  = 0;
    hist_push(mv);
}

int neos_battery_history_count(void)
{
    return s_hist ? s_hist_n : 0;
}

int neos_battery_history_avg(uint16_t *out, int cols, int first, int count)
{
    if (!out || !s_hist || cols <= 0 || count <= 0) {
        return 0;
    }

    for (int i = 0; i < cols; i++) {
        /*
         * Bucket edges from the window rather than from the data, so a window
         * that reaches back before the record starts leaves those buckets empty
         * instead of stretching what exists to fill them. 64-bit because two
         * days of samples times a thousand columns overflows an int.
         */
        int a = first + (int)(((int64_t)i * count) / cols);
        int b = first + (int)(((int64_t)(i + 1) * count) / cols);
        if (b <= a) {
            b = a + 1;          /* zoomed in past one sample a column */
        }

        uint32_t sum = 0;
        int      m   = 0;
        for (int j = a; j < b; j++) {
            if (j < 0 || j >= s_hist_n) {
                continue;
            }
            const uint16_t v = hist_at(j);
            if (!v) {
                continue;       /* a period the monitor did not answer in */
            }
            sum += v;
            m++;
        }
        out[i] = m ? (uint16_t)(sum / (uint32_t)m) : 0;
    }
    return cols;
}

uint32_t neos_battery_history_seq(void)
{
    return s_hist_seq;
}

neos_power_src_t neos_battery_source(void)
{
    if (!s_last_ok) {
        return NEOS_PWR_UNKNOWN;
    }
    if (s_last.current_ma > NEOS_BATTERY_IDLE_MA) {
        return NEOS_PWR_BATTERY;
    }
    if (s_last.current_ma < -NEOS_BATTERY_IDLE_MA) {
        return NEOS_PWR_CHARGING;
    }
    return NEOS_PWR_EXTERNAL;
}

/*
 * The discharge curve of a 2S LiPo, as pack millivolts against percent.
 *
 * A lithium cell's voltage is not proportional to what is left in it: it drops
 * quickly off a full charge, sits on a long plateau for most of the middle,
 * and then falls off a cliff at the end. Interpolating a straight line from
 * 8.4 V to 6.0 V would read about 60% for most of the discharge and then lose
 * forty points in the last few minutes, so the curve has to be a table.
 *
 * Pack volts, so twice the per-cell figures: 4.20 V/cell full, and zero at the
 * 3.00 V/cell danger line for the reason given in neos_bar.h. Ordered high to
 * low; neos_battery_soc_mv() walks it and interpolates between neighbours.
 */
static const struct { int16_t mv; int8_t pct; } OCV[] = {
    { 8400, 100 }, { 8300,  95 }, { 8220,  90 }, { 8040,  80 },
    { 7900,  70 }, { 7740,  60 }, { 7640,  50 }, { 7580,  40 },
    { 7500,  30 }, { 7400,  20 }, { 7340,  15 }, { 7260,  10 },
    { 7060,   5 }, { 6600,   2 }, { 6000,   0 },
};

int neos_battery_soc_mv(int32_t open_mv)
{
    const int n = (int)(sizeof(OCV) / sizeof(OCV[0]));
    if (open_mv >= OCV[0].mv) {
        return 100;
    }
    if (open_mv <= OCV[n - 1].mv) {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if (open_mv >= OCV[i].mv) {
            const int32_t span = OCV[i - 1].mv - OCV[i].mv;
            const int32_t up   = open_mv - OCV[i].mv;
            const int32_t pct  = OCV[i].pct +
                                 (OCV[i - 1].pct - OCV[i].pct) * up / span;
            return (int)pct;
        }
    }
    return 0;
}

int neos_battery_soc(const neos_power_t *p)
{
    if (!p) {
        return 0;
    }
    /* Positive current is out of the pack and depresses the terminals, so
       adding I*R back is the same expression in both directions. */
    return neos_battery_soc_mv(p->bus_mv + p->current_ma * NEOS_BATTERY_R_MOHM / 1000);
}

/* ------------------------------------------------------------------ */
/* Capacity, learned from a discharge                                  */
/* ------------------------------------------------------------------ */

/*
 * How big a fall to measure over. Ten points of charge is enough that the
 * percentage either end is worth more than its own rounding, and small enough
 * that an ordinary session produces one.
 */
#define CAP_MIN_DROP_PCT 10

/* A pack this board could plausibly carry. Anything outside it is a bad
   measurement - a load that changed under us, or a percentage that moved for
   some reason other than charge leaving - and is dropped rather than averaged
   into a figure that is then wrong and confident. */
#define CAP_MIN_MAH 100
#define CAP_MAX_MAH 50000

static int32_t s_capacity_mah;      /* 0 until a discharge has been watched */
static double  s_mah_out;           /* charge out since the window opened */
static int     s_soc_ref = -1;      /* the percentage it opened at */

int neos_battery_capacity_mah(void)
{
    return s_capacity_mah;
}

/*
 * Capacity is what charge-out and percentage-fall divide into.
 *
 * Voltage sag on its own cannot give this. Sag against current is internal
 * resistance - it says something about the pack's age and nothing about how
 * much is in it. What does give capacity is watching the charge actually leave
 * while the percentage comes down: integrate the current over the fall, and
 * mAh divided by the fraction of the pack it emptied is the whole pack.
 *
 * Which means the figure is only ever as good as the charge curve behind the
 * percentage. That is the honest limit of it, and the reason this is an
 * estimate that improves rather than a measurement that settles.
 */
static void capacity_track(void)
{
    if (!s_last_ok || neos_battery_source() != NEOS_PWR_BATTERY) {
        /* Charging, external, or no pack: the window is meaningless now and
           resuming it later would count a fall that charge did not cause. */
        s_soc_ref = -1;
        s_mah_out = 0;
        return;
    }

    const int soc = neos_battery_soc(&s_last);
    if (s_soc_ref < 0) {
        s_soc_ref = soc;
        s_mah_out = 0;
        return;
    }

    s_mah_out += (double)s_last.current_ma * TICK_MS / 3600000.0;

    const int fall = s_soc_ref - soc;
    if (fall < CAP_MIN_DROP_PCT) {
        return;
    }

    const int32_t cap = (int32_t)(s_mah_out * 100.0 / fall);
    if (cap >= CAP_MIN_MAH && cap <= CAP_MAX_MAH) {
        /* Weighted toward what is already known, so one odd session moves the
           figure without replacing it. */
        s_capacity_mah = s_capacity_mah ? (s_capacity_mah * 3 + cap) / 4 : cap;
        ESP_LOGI(TAG, "capacity: this run %ld mAh over %d%%, holding %ld mAh",
                 (long)cap, fall, (long)s_capacity_mah);
    }
    s_soc_ref = soc;
    s_mah_out = 0;
}

/* ------------------------------------------------------------------ */
/* The record, across reboots                                          */
/* ------------------------------------------------------------------ */

/*
 * The history and the learned capacity, in NVS.
 *
 * NVS and not the card, because the record has to survive a tablet with no
 * card in it, and because a few kilobytes rewritten now and then would be an odd
 * thing to keep a filesystem open for.
 *
 * What is stored is the record decimated to one point a minute - see
 * SAVE_EVERY_MS for why it cannot be the ring itself. So a restored trace is
 * two days long and flat inside each minute, and zooming into a stretch that
 * came back from flash shows exactly the resolution that was kept rather than
 * pretending to the ten seconds it was recorded at.
 *
 * The stamp is what makes it a record rather than a snapshot. Samples are
 * spaced by a fixed period, so restoring them at the wrong offset would draw
 * an hour of history that did not happen; with the time the newest one was
 * taken, the gap while the tablet was off can be counted out in periods and
 * left as the break in the trace that it was.
 */
#define STORE_NS    "batt"
#define STORE_KEY   "rec2"
#define STORE_MAGIC 0x4E424832u   /* "NBH2" - one point a minute, two days */

typedef struct {
    uint32_t magic;
    int64_t  stamp;             /* epoch seconds of the newest slot */
    int32_t  capacity_mah;
    uint16_t n;                 /* slots written, always STORE_N */
    uint16_t slots[STORE_N];    /* oldest first, one per minute */
} batt_store_t;

static nvs_handle_t s_store;

/*
 * The record being written or read, in PSRAM.
 *
 * Six kilobytes, and it used to be a static of a quarter the size. Neither
 * belongs on a stack - this is called from the widget timer - and neither
 * belongs in internal RAM when it is touched twice an hour.
 */
static batt_store_t *s_rec;

static batt_store_t *store_rec(void)
{
    if (!s_rec) {
        s_rec = heap_caps_malloc(sizeof(batt_store_t), MALLOC_CAP_SPIRAM);
    }
    return s_rec;
}

/* The clock starts at the epoch on a cold boot, so a stamp from before the
   tablet could possibly have been built is a clock that has not been set yet,
   not a very old record. */
#define STAMP_SANE_AFTER 1700000000LL

static int64_t epoch_now(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

static void battery_store_save(void)
{
    if (!s_store) {
        return;
    }
    const int64_t now = epoch_now();
    if (now < STAMP_SANE_AFTER) {
        return;    /* unstamped is worse than not saved: it restores wrongly */
    }

    batt_store_t *rec = store_rec();
    if (!rec) {
        return;
    }
    rec->magic        = STORE_MAGIC;
    rec->stamp        = now;
    rec->capacity_mah = s_capacity_mah;

    /*
     * The whole ring, averaged a minute at a time. The window deliberately
     * starts before the record does when the tablet has been up for less than
     * two days - those buckets come back empty and are stored as the gaps they
     * are, which is what makes the stamp enough to place the rest.
     */
    rec->n = (uint16_t)neos_battery_history_avg(rec->slots, STORE_N,
                                                s_hist_n - NEOS_BATTERY_HIST_N,
                                                NEOS_BATTERY_HIST_N);

    if (nvs_set_blob(s_store, STORE_KEY, rec, sizeof(*rec)) == ESP_OK) {
        nvs_commit(s_store);
    }
}

static void battery_store_load(void)
{
    if (nvs_open(STORE_NS, NVS_READWRITE, &s_store) != ESP_OK) {
        ESP_LOGW(TAG, "no NVS for the battery record - history starts empty");
        s_store = 0;
        return;
    }

    batt_store_t *rec = store_rec();
    if (!rec) {
        return;
    }
    size_t len = sizeof(*rec);
    if (nvs_get_blob(s_store, STORE_KEY, rec, &len) != ESP_OK ||
        len != sizeof(*rec) || rec->magic != STORE_MAGIC) {
        return;
    }

    /* The capacity is worth keeping whatever the clock says - it is not a
       time series and does not care when it was learned. */
    if (rec->capacity_mah >= CAP_MIN_MAH && rec->capacity_mah <= CAP_MAX_MAH) {
        s_capacity_mah = rec->capacity_mah;
    }

    const int64_t now = epoch_now();
    if (rec->stamp < STAMP_SANE_AFTER || now < STAMP_SANE_AFTER || now < rec->stamp) {
        return;                 /* no usable clock at one end or the other */
    }
    if (!s_hist || rec->n == 0) {
        return;
    }

    /* How many minutes the tablet was off for, which is how far from the right
       edge the newest stored point belongs. */
    const int64_t gap_min = (now - rec->stamp) / 60;
    if (gap_min >= STORE_N) {
        return;                 /* off for longer than the window is wide */
    }

    /*
     * Laid back into the ring at the offset the clock says, so what is left of
     * the old trace keeps its distance from the right-hand edge and the time the
     * tablet was off is a gap rather than a splice.
     *
     * Each stored minute fills the six slots it covers. That is the resolution
     * that was kept, said in the units the ring is indexed in - not a claim that
     * six readings were taken, which is why the comment at the top of the store
     * says so out loud.
     */
    memset(s_hist, 0, (size_t)NEOS_BATTERY_HIST_N * sizeof(uint16_t));
    for (int j = 0; j < (int)rec->n && j < STORE_N; j++) {
        if (!rec->slots[j]) {
            continue;
        }
        const int age_min = (STORE_N - 1 - j) + (int)gap_min;
        for (int k = 0; k < STORE_DECIM; k++) {
            const int idx = NEOS_BATTERY_HIST_N - 1 - (age_min * STORE_DECIM + k);
            if (idx >= 0 && idx < NEOS_BATTERY_HIST_N) {
                s_hist[idx] = rec->slots[j];
            }
        }
    }
    s_hist_n    = NEOS_BATTERY_HIST_N;
    s_hist_head = 0;
    s_hist_seq++;

    ESP_LOGI(TAG, "battery record restored: %u min, %lld min gap, capacity %ld mAh",
             (unsigned)rec->n, (long long)gap_min, (long)s_capacity_mah);
}

/*
 * The battery icon is one glyph in three colours, the same trick as the Wi-Fi
 * one: dim while there is plenty left, TH_WARN once the pack passes 3.3 V/cell,
 * and flashing TH_BAD once it passes the 3.0 V/cell line a 2S LiPo should not
 * be taken past. s_batt_blink_on is toggled by the widget tick and read here
 * rather than recomputed, so the on/off halves of the flash actually alternate
 * instead of both landing on whichever phase a given repaint happens to hit.
 *
 * Nothing is drawn at all if the power monitor never answered - a dash or a
 * question mark would claim to know something about a battery that, on this
 * board, might not be wired up.
 */
static bool s_batt_blink_on = true;

static void paint_battery(ngl_surface_t *s)
{
    const ngl_rect_t r = battery_rect();
    ngl_fill_rect(s, r, TH_BAR_BG);

    neos_power_t p;
    if (!battery_last(&p)) {
        return;
    }

    const bool danger = p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;
    if (danger && !s_batt_blink_on) {
        return;   /* the "off" half of the flash */
    }

    const ngl_icon_t *ic = ngl_icon_find("battery", 24);
    if (!ic) {
        return;
    }
    const ngl_color_t c = danger ? TH_BAD
                         : (p.bus_mv < NEOS_BATTERY_2S_WARN_MV ? TH_WARN : TH_TEXT_DIM);
    ngl_icon(s, (int16_t)(r.x + (r.w - ic->w) / 2),
             (int16_t)(r.y + (r.h - ic->h) / 2), ic, c);
}

/** "14:32", or "--:--" on a tablet that has never been told the time. */
static void clock_text(char *buf, size_t n)
{
    neos_rtc_t t;
    if (!neos_time_local(&t)) {
        snprintf(buf, n, "--:--");
        return;
    }
    snprintf(buf, n, "%02u:%02u", t.hour, t.min);
}

static void paint_clock(ngl_surface_t *s)
{
    const ngl_rect_t r = clock_rect();
    ngl_fill_rect(s, r, TH_BAR_BG);

    char buf[8];
    clock_text(buf, sizeof(buf));

    /* Brighter once it has been set from the network, because the difference
       between a clock that is right and one that is merely running is the
       only thing about it worth a second colour. */
    const ngl_color_t c = neos_time_synced() ? TH_TEXT : TH_TEXT_DIM;
    const int16_t w = ngl_text_width(&ngl_font_small, buf);
    ngl_text(s, (int16_t)(r.x + (r.w - w) / 2),
             (int16_t)((r.h - ngl_font_small.height) / 2), buf, &ngl_font_small, c);
}

/*
 * Repainting the widgets is its own path, not a whole-bar repaint.
 *
 * A full repaint wipes the status line, which is often mid-marquee, and costs
 * a flush of the entire strip once a second forever. Painting only the two
 * widget cells - and only when what they say has actually changed - is the
 * difference between a bar with a clock in it and a bar that flickers.
 */
static char             s_shown_clock[8];
static neos_net_state_t s_shown_net = (neos_net_state_t)-1;
static int32_t           s_shown_batt_mv = INT32_MIN;   /* unread yet */

void neos_bar_widgets_refresh(void)
{
    /*
     * Nothing while a panel is up.
     *
     * Not because the bar is covered - panels stay inside the app area - but
     * because ngl_overlay_leave() puts the whole screen back as it found it,
     * so anything painted here in the meantime is thrown away. Returning
     * before the cache is touched is the part that matters: recording a state
     * as shown when it was not is how an icon gets stuck until the next time
     * it happens to change again.
     */
    if (ngl_overlay_active()) {
        return;
    }

    /*
     * Nor while the screen is off, and for the second half of the same reason:
     * the cache is left alone, so nothing is recorded as shown that was not.
     * What the panel gets when it comes back is a full repaint - see
     * neos_screen_on() - which is why the clock does not have to be kept up to
     * date in the dark.
     */
    if (neos_screen_is_off()) {
        return;
    }

    char now[8];
    clock_text(now, sizeof(now));
    const neos_net_state_t st = neos_net_state();

    neos_power_t p = {0};
    const bool    batt_ok = battery_last(&p);
    const int32_t batt_mv = batt_ok ? p.bus_mv : INT32_MIN;
    const bool    danger  = batt_ok && p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;

    /*
     * The blink has to force a repaint every tick it is active, since neither
     * the clock nor the net state changes while the icon alternates on and
     * off. Once danger clears, s_batt_blink_on is left true rather than
     * whatever phase it stopped on, so the icon comes back solid instead of
     * possibly stuck invisible.
     */
    bool batt_changed = (batt_mv != s_shown_batt_mv);
    if (danger) {
        s_batt_blink_on = !s_batt_blink_on;
        batt_changed    = true;
    } else if (!s_batt_blink_on) {
        s_batt_blink_on = true;
        batt_changed    = true;
    }

    if (st == s_shown_net && strcmp(now, s_shown_clock) == 0 && !batt_changed) {
        return;
    }
    s_shown_net    = st;
    s_shown_batt_mv = batt_mv;
    strlcpy(s_shown_clock, now, sizeof(s_shown_clock));

    ngl_surface_t *s = ngl_bar_begin(widgets_rect());
    if (!s) {
        return;
    }
    paint_battery(s);
    paint_wifi(s);
    paint_clock(s);
    /* ngl_bar_end() flushes the widget strip; see neos_status.c for why this
       must not be a full flush. */
    ngl_bar_end();
}

static void widget_tick(void *arg)
{
    (void)arg;
    /* Before the refresh, and outside its early return: the pack is recorded
       on every tick whether or not there is a bar to repaint. */
    battery_sample();
    neos_bar_widgets_refresh();
}

/* ------------------------------------------------------------------ */

/* ngl calls this from ngl_clear() with the reservation lifted, so this is the
   only code that can draw here. */
static void bar_painter(ngl_surface_t *s, ngl_rect_t bar)
{
    ngl_fill_rect(s, bar, TH_BAR_BG);
    ngl_hline(s, 0, (int16_t)(bar.h - 1), bar.w, TH_BAR_EDGE);
    ngl_text(s, 14, (int16_t)((bar.h - 32) / 2), "NeOS", &ngl_font_small, TH_TEXT_DIM);

    /* Outline, not a fill: at this size a filled badge is a bright block in
       the corner of every screen, and the version is reference material, not
       something to look at. */
    const ngl_rect_t v = version_rect();
    ngl_draw_round_rect(s, v, (int16_t)(VER_H / 2), TH_BAR_EDGE, 2);
    ngl_text(s, (int16_t)(v.x + VER_PAD), (int16_t)((bar.h - 32) / 2),
             build_version(), &ngl_font_small, TH_TEXT_FAINT);

    if (s_closable) {
        const ngl_rect_t b = close_rect();
        ngl_fill_round_rect(s, b, 8, TH_CLOSE_FILL);
        ngl_draw_round_rect(s, b, 8, TH_CLOSE_LINE, 2);

        const ngl_icon_t *ic = ngl_icon_find("close", 24);
        if (ic) {
            ngl_icon(s, (int16_t)(b.x + (b.w - ic->w) / 2),
                     (int16_t)(b.y + (b.h - ic->h) / 2), ic, TH_CLOSE_X);
        }
    }

    paint_battery(s);
    paint_wifi(s);
    paint_clock(s);

    /* Last, because the fill above wiped whatever was showing. */
    neos_status_paint();
}

void neos_bar_init(void)
{
    ngl_reserve_top(BAR_H, bar_painter);
    neos_status_init(status_rect);

    /*
     * One reading before the first paint, so the icon is not blank for the
     * half second until the timer first fires. Guarded, because this function
     * runs again every time the bar comes back from fullscreen and a sample
     * taken outside the tick would put the history's period accounting half a
     * tick out each time.
     */
    if (!s_last_ok) {
        hist_init();
        battery_store_load();
        battery_sample();
    }

    /*
     * Twice a second, not once. A clock that only shows minutes still has to
     * change on the minute rather than up to a second after it, and half the
     * period is the cheapest way to be within half a second of the edge. The
     * repaint itself is skipped unless the text moved, so this costs one
     * snprintf and a compare 119 times out of 120.
     */
    static esp_timer_handle_t s_tick;
    if (!s_tick) {
        const esp_timer_create_args_t args = {
            .callback = widget_tick,
            .name     = "barwidget",
        };
        if (esp_timer_create(&args, &s_tick) == ESP_OK) {
            esp_timer_start_periodic(s_tick, TICK_MS * 1000);
        }
    }

    ESP_LOGI(TAG, "system bar reserved, %d px, build %s", BAR_H, build_version());
}

/* ------------------------------------------------------------------ */
/* Game mode                                                           */
/* ------------------------------------------------------------------ */

/*
 * Handing the top 56 px back to the app, and taking them again afterwards.
 *
 * The whole mechanism is the reservation: ngl_reserve_top(0, NULL) widens the
 * screen clip to the full panel and leaves nothing for ngl_clear() to repaint
 * a bar into, and neos_bar_hit() then tests taps against rectangles of zero
 * height, which nothing is inside - so the bar's buttons stop answering
 * without any separate switch for that. Everything else here is bookkeeping so
 * that the way back is not the app's responsibility.
 *
 * s_fullscreen is what the touch task reads to arm the four-finger escape.
 * Kept here rather than there because this is the thing that knows, and a
 * second copy of it in the touch layer is a second thing to get wrong when an
 * app exits without turning the mode off.
 */
static bool s_fullscreen;

bool neos_fullscreen(bool on)
{
    if (on == s_fullscreen) {
        return true;
    }
    s_fullscreen = on;

    if (on) {
        ngl_reserve_top(0, NULL);
        ESP_LOGI(TAG, "system bar released - the app owns the panel");
    } else {
        neos_bar_init();
        ngl_bar_paint();
        ESP_LOGI(TAG, "system bar back");
    }
    ngl_dirty_all();      /* the app area changed shape; everything repaints */
    return true;
}

bool neos_is_fullscreen(void)
{
    return s_fullscreen;
}

void neos_bar_set_closable(bool closable)
{
    if (s_closable == closable) {
        return;
    }
    s_closable = closable;
    /* Every widget on the right moves when the button comes and goes, and the
       status area grows into the freed space or yields it. */
    s_shown_clock[0] = 0;
    s_shown_net = (neos_net_state_t)-1;
    ngl_bar_paint();
    ngl_flush();
}
