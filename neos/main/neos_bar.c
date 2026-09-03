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

#include "esp_log.h"
#include "esp_timer.h"

#include "neos_build.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_bar.h"
#include "neos_boot.h"
#include "neos_net.h"
#include "neos_status.h"
#include "neos_time.h"

static const char *TAG = "bar";

#define BAR_H   56
#define BAR_BTN 40
#define BAR_PAD 8

/* The version badge: a rounded outline around the build string. */
#define VER_H   38
#define VER_PAD 10
#define VER_GAP 10

/* The two widgets on the right, between the status line and the close button. */
#define WIFI_W  40
#define CLOCK_W 92
#define WID_GAP 6

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

static ngl_rect_t widgets_rect(void)
{
    const ngl_rect_t w = wifi_rect();
    const ngl_rect_t c = clock_rect();
    return ngl_rect_union(&w, &c);
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
    int16_t w = (int16_t)(wifi_rect().x - WID_GAP - l);
    if (w < 0) {
        w = 0;
    }
    return ngl_rect(l, (int16_t)((bar.h - ngl_font_small.height) / 2),
                    w, ngl_font_small.height);
}

neos_bar_hit_t neos_bar_hit(int16_t x, int16_t y)
{
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

    char now[8];
    clock_text(now, sizeof(now));
    const neos_net_state_t st = neos_net_state();

    if (st == s_shown_net && strcmp(now, s_shown_clock) == 0) {
        return;
    }
    s_shown_net = st;
    strlcpy(s_shown_clock, now, sizeof(s_shown_clock));

    ngl_surface_t *s = ngl_bar_begin(widgets_rect());
    if (!s) {
        return;
    }
    paint_wifi(s);
    paint_clock(s);
    ngl_bar_end();
    ngl_flush();
}

static void widget_tick(void *arg)
{
    (void)arg;
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
            esp_timer_start_periodic(s_tick, 500 * 1000);
        }
    }

    ESP_LOGI(TAG, "system bar reserved, %d px, build %s", BAR_H, build_version());
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
