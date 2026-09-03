/*
 * The clock page.
 *
 * What time it is, what day it is, which month that day is in, and the two
 * numbers that decide all of it - the zone offset and whether the daylight
 * hour is being added. Those two are settings rather than a rule table on
 * purpose; neos_time.h says why.
 *
 * Written as one page with the pieces stacked, because that is what fits and
 * because the eventual tabbed version wants the same pieces. Each block below
 * paints into a rectangle it is handed, so moving one behind a tab later is a
 * change to the layout function and to nothing else.
 *
 * Only the second hand and the sync line are repainted on the tick. The
 * calendar changes once a day and the controls change when they are touched,
 * and repainting a month grid twice a second on a software blitter is how a
 * page like this ends up flickering.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_net.h"
#include "neos_panel.h"
#include "neos_sys.h"
#include "neos_time.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "clock";

#define POLL_MS   40
#define TICK_MS   200

#define MARGIN    16
#define TIME_H    72
#define DATE_H    40
#define CAL_HEAD  32
#define ROW_H     56
#define BTN_W     64
#define GAP       10

static const char *const WDAY_SHORT[7] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
static const char *const WDAY_LONG[7]  = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};
static const char *const MONTH[13] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};

/* ------------------------------------------------------------------ */
/* Calendar arithmetic                                                 */
/* ------------------------------------------------------------------ */

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m)
{
    static const int8_t D[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) {
        return 30;
    }
    return (m == 2 && leap(y)) ? 29 : D[m];
}

/**
 * Weekday of a date, 0 = Sunday. Sakamoto's method.
 *
 * Done here rather than asked of neos_time, because what this page needs is
 * the weekday of the *first* of the displayed month, which is not now and is
 * not a time the system clock has any opinion about.
 */
static int weekday(int y, int m, int d)
{
    static const int T[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static ngl_rect_t s_panel, s_content;
static ngl_rect_t s_time, s_date, s_cal;
static ngl_rect_t s_tz_row, s_dst_row, s_sync_row;
static ngl_rect_t s_tz_minus, s_tz_plus, s_dst_btn, s_sync_btn;

static int16_t s_cell_w, s_cell_h;

static void layout(void)
{
    const ngl_rect_t app = ngl_app_area();
    s_panel = ngl_rect((int16_t)(app.x + 12), (int16_t)(app.y + 12),
                       (int16_t)(app.w - 24), (int16_t)(app.h - 24));

    /* The frame is painted separately; this is only the arithmetic, so it can
       be redone on a rotation without anything being drawn twice. */
    s_content = ngl_rect((int16_t)(s_panel.x + 2),
                         (int16_t)(s_panel.y + NEOS_UI_TITLE_H + 1),
                         (int16_t)(s_panel.w - 4),
                         (int16_t)(s_panel.h - NEOS_UI_TITLE_H - 3));

    const int16_t x = (int16_t)(s_content.x + MARGIN);
    const int16_t w = (int16_t)(s_content.w - 2 * MARGIN);
    int16_t y = (int16_t)(s_content.y + 8);

    s_time = ngl_rect(x, y, w, TIME_H);   y = (int16_t)(y + TIME_H);
    s_date = ngl_rect(x, y, w, DATE_H);   y = (int16_t)(y + DATE_H + 8);

    /*
     * The three control rows are placed from the bottom up and the calendar
     * gets what is left, so the controls stay reachable in portrait - where
     * there is enough height for a very tall month grid and no reason to have
     * one - and the grid simply gets roomier in landscape.
     */
    const int16_t bottom = (int16_t)(s_content.y + s_content.h - 8);
    s_sync_row = ngl_rect(x, (int16_t)(bottom - ROW_H), w, ROW_H);
    s_dst_row  = ngl_rect(x, (int16_t)(bottom - 2 * ROW_H), w, ROW_H);
    s_tz_row   = ngl_rect(x, (int16_t)(bottom - 3 * ROW_H), w, ROW_H);

    int16_t cal_h = (int16_t)(s_tz_row.y - 8 - y);
    if (cal_h < CAL_HEAD + 2 * 24) {
        cal_h = (int16_t)(CAL_HEAD + 2 * 24);
    }
    s_cal = ngl_rect(x, y, w, cal_h);

    s_cell_w = (int16_t)(s_cal.w / 7);
    s_cell_h = (int16_t)((s_cal.h - CAL_HEAD) / 6);

    const int16_t bh = (int16_t)(ROW_H - 12);
    s_tz_plus  = ngl_rect((int16_t)(s_tz_row.x + s_tz_row.w - BTN_W),
                          (int16_t)(s_tz_row.y + 6), BTN_W, bh);
    s_tz_minus = ngl_rect((int16_t)(s_tz_plus.x - GAP - BTN_W),
                          (int16_t)(s_tz_row.y + 6), BTN_W, bh);
    s_dst_btn  = ngl_rect((int16_t)(s_dst_row.x + s_dst_row.w - 2 * BTN_W - GAP),
                          (int16_t)(s_dst_row.y + 6),
                          (int16_t)(2 * BTN_W + GAP), bh);
    s_sync_btn = ngl_rect((int16_t)(s_sync_row.x + s_sync_row.w - 2 * BTN_W - GAP),
                          (int16_t)(s_sync_row.y + 6),
                          (int16_t)(2 * BTN_W + GAP), bh);
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static void fmt_offset(char *b, size_t n, int minutes)
{
    const char sign = minutes < 0 ? '-' : '+';
    const int a = minutes < 0 ? -minutes : minutes;
    snprintf(b, n, "UTC%c%02d:%02d", sign, a / 60, a % 60);
}

static void paint_time(ngl_surface_t *s, const neos_rtc_t *t, bool have)
{
    ngl_fill_rect(s, s_time, TH_MODAL_BG);

    char buf[16];
    if (have) {
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u", t->hour, t->min, t->sec);
    } else {
        snprintf(buf, sizeof(buf), "--:--:--");
    }
    neos_ui_text_centred(s, s_time, buf, &ngl_font_large,
                         have ? TH_GLOW : TH_TEXT_FAINT);
}

static void paint_date(ngl_surface_t *s, const neos_rtc_t *t, bool have)
{
    ngl_fill_rect(s, s_date, TH_MODAL_BG);

    char buf[48];
    if (have) {
        snprintf(buf, sizeof(buf), "%s %u %s %d",
                 WDAY_LONG[t->wday % 7], t->day, MONTH[t->month % 13], t->year);
    } else {
        snprintf(buf, sizeof(buf), "the clock has never been set");
    }
    neos_ui_text_centred(s, s_date, buf, &ngl_font_small,
                         have ? TH_TEXT : TH_TEXT_FAINT);
}

/*
 * The month the displayed day is in, with that day ringed.
 *
 * Six week rows always, whether the month needs them or not: a grid that
 * changed height between February and August would move everything under it,
 * and a page whose controls are somewhere different each month is a page you
 * have to look at before you can touch.
 */
static void paint_calendar(ngl_surface_t *s, const neos_rtc_t *t, bool have)
{
    ngl_fill_rect(s, s_cal, TH_MODAL_BG);
    if (!have) {
        return;
    }

    for (int c = 0; c < 7; c++) {
        const ngl_rect_t h = ngl_rect((int16_t)(s_cal.x + c * s_cell_w), s_cal.y,
                                      s_cell_w, CAL_HEAD);
        neos_ui_text_centred(s, h, WDAY_SHORT[c], &ngl_font_small, TH_TEXT_DIM);
    }
    ngl_hline(s, s_cal.x, (int16_t)(s_cal.y + CAL_HEAD - 1),
              (int16_t)(s_cell_w * 7), TH_RULE);

    const int first = weekday(t->year, t->month, 1);
    const int ndays = days_in_month(t->year, t->month);

    for (int d = 1; d <= ndays; d++) {
        const int idx = first + d - 1;
        const ngl_rect_t cell = ngl_rect(
            (int16_t)(s_cal.x + (idx % 7) * s_cell_w),
            (int16_t)(s_cal.y + CAL_HEAD + (idx / 7) * s_cell_h),
            s_cell_w, s_cell_h);

        char num[12];
        snprintf(num, sizeof(num), "%d", d);

        if (d == t->day) {
            const int16_t side = s_cell_w < s_cell_h ? s_cell_w : s_cell_h;
            const ngl_rect_t ring = ngl_rect(
                (int16_t)(cell.x + (cell.w - side) / 2),
                (int16_t)(cell.y + (cell.h - side) / 2), side, side);
            ngl_draw_round_rect(s, ring, (int16_t)(side / 2), TH_ACCENT, 2);
        }
        neos_ui_text_centred(s, cell, num, &ngl_font_small,
                             d == t->day ? TH_GLOW : TH_TEXT);
    }
}

static void paint_controls(ngl_surface_t *s)
{
    ngl_fill_rect(s, s_tz_row, TH_MODAL_BG);
    ngl_fill_rect(s, s_dst_row, TH_MODAL_BG);

    char off[16];
    fmt_offset(off, sizeof(off), neos_tz_offset_min());

    neos_ui_text_left(s, s_tz_row, "Time zone", &ngl_font_small, TH_TEXT_DIM);
    ngl_rect_t val = s_tz_row;
    val.w = (int16_t)(s_tz_minus.x - val.x - GAP);
    neos_ui_text_right(s, val, off, &ngl_font_small, TH_TEXT);
    neos_ui_button(s, s_tz_minus, "-", false);
    neos_ui_button(s, s_tz_plus, "+", false);

    neos_ui_text_left(s, s_dst_row, "Daylight saving", &ngl_font_small, TH_TEXT_DIM);
    neos_ui_button(s, s_dst_btn, neos_tz_dst() ? "+1h on" : "off", neos_tz_dst());
}

/*
 * The sync line is the honest answer to "is this clock right".
 *
 * There are four different things it can be, and they are not degrees of the
 * same thing: no radio at all, a radio with no network, a network that has not
 * answered yet, and a time that came from a server and when. Collapsing them
 * into "synced / not synced" would hide the one case a user can do something
 * about.
 */
static void paint_sync(ngl_surface_t *s)
{
    ngl_fill_rect(s, s_sync_row, TH_MODAL_BG);

    char line[64];
    const neos_net_state_t st = neos_net_state();

    if (neos_time_synced()) {
        const unsigned long ago = (unsigned long)neos_time_since_sync_s();
        if (ago < 90) {
            snprintf(line, sizeof(line), "from the network, %lus ago", ago);
        } else if (ago < 5400) {
            snprintf(line, sizeof(line), "from the network, %lum ago", ago / 60);
        } else {
            snprintf(line, sizeof(line), "from the network, %luh ago", ago / 3600);
        }
    } else if (st == NEOS_NET_ABSENT) {
        snprintf(line, sizeof(line), "no radio - set it by hand");
    } else if (st == NEOS_NET_ONLINE) {
        snprintf(line, sizeof(line), "online, waiting for a time server");
    } else {
        snprintf(line, sizeof(line), "running from the RTC");
    }

    neos_ui_text_left(s, s_sync_row, "Source", &ngl_font_small, TH_TEXT_DIM);
    ngl_rect_t val = s_sync_row;
    val.x = (int16_t)(val.x + 140);
    val.w = (int16_t)(s_sync_btn.x - val.x - GAP);
    neos_ui_text_right(s, val, line, &ngl_font_small, TH_TEXT_DIM);

    const bool can = st == NEOS_NET_ONLINE;
    neos_ui_button(s, s_sync_btn, can ? "Sync now" : "offline", false);
}

/* ------------------------------------------------------------------ */

void neos_panel_clock(void)
{
    if (!ngl_screen()) {
        return;
    }
    layout();

    neos_rtc_t now = {0};
    bool have = neos_time_local(&now);

    ngl_overlay_restore();
    ngl_surface_t *s = ngl_overlay_begin(ngl_app_area());
    if (s) {
        neos_ui_dim(s);
        neos_ui_frame(s, s_panel, "Date and time");
        paint_time(s, &now, have);
        paint_date(s, &now, have);
        paint_calendar(s, &now, have);
        paint_controls(s);
        paint_sync(s);
        ngl_overlay_end();
        ngl_flush();
    }

    neos_rtc_t shown = now;
    bool shown_have = have;
    uint32_t since = 0;
    ngl_rect_t area = ngl_app_area();

    while (!neos_ui_should_close()) {
        int16_t tx = 0, ty = 0;
        bool controls_moved = false;
        bool all = false;

        if (neos_touch_tap_os(&tx, &ty)) {
            const ngl_rect_t close = neos_ui_close_rect(s_panel);
            if (ngl_rect_contains(&close, tx, ty)) {
                break;
            }
            /*
             * Quarter hours, not whole ones. India is +5:30 and Nepal +5:45,
             * and a control that could not express them would be a control
             * that is simply wrong in those places.
             */
            if (ngl_rect_contains(&s_tz_minus, tx, ty)) {
                neos_tz_offset_set(neos_tz_offset_min() - 15);
                controls_moved = true;
            } else if (ngl_rect_contains(&s_tz_plus, tx, ty)) {
                neos_tz_offset_set(neos_tz_offset_min() + 15);
                controls_moved = true;
            } else if (ngl_rect_contains(&s_dst_btn, tx, ty)) {
                neos_tz_dst_set(!neos_tz_dst());
                controls_moved = true;
            } else if (ngl_rect_contains(&s_sync_btn, tx, ty)) {
                neos_net_sntp_restart();
                controls_moved = true;
            }
        }

        /* The zone moving moves the wall clock, so the day and the month can
           change under it - that is a whole-page repaint, not a tick. */
        if (controls_moved) {
            all = true;
        }

        const ngl_rect_t a = ngl_app_area();
        if (a.x != area.x || a.y != area.y || a.w != area.w || a.h != area.h) {
            area = a;
            layout();
            all = true;
        }

        since += POLL_MS;
        const bool tick = since >= TICK_MS;
        if (tick) {
            since = 0;
        }

        if (!tick && !all) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        have = neos_time_local(&now);

        const bool day_moved = all || have != shown_have ||
                               now.day != shown.day || now.month != shown.month ||
                               now.year != shown.year;

        ngl_surface_t *sc;
        if (all) {
            ngl_overlay_restore();
            sc = ngl_overlay_begin(ngl_app_area());
        } else {
            sc = ngl_overlay_begin(s_panel);
        }
        if (sc) {
            if (all) {
                neos_ui_dim(sc);
                neos_ui_frame(sc, s_panel, "Date and time");
            }
            paint_time(sc, &now, have);
            if (day_moved) {
                paint_date(sc, &now, have);
                paint_calendar(sc, &now, have);
            }
            if (all) {
                paint_controls(sc);
            }
            paint_sync(sc);
            ngl_overlay_end();
            ngl_flush();
        }

        shown = now;
        shown_have = have;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    ESP_LOGI(TAG, "clock page closed");
}
