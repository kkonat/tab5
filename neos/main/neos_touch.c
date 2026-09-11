/*
 * Touch input.
 *
 * One task polls the controller and turns a stream of coordinates into the
 * three things the rest of the system wants: where the fingers are now, "a tap
 * happened here", and the click the speaker makes when one does. Edge
 * detection and the movement tolerance live here rather than in every app,
 * because getting them subtly different per app is how a UI starts feeling
 * inconsistent.
 *
 * Coordinates leave here in screen space. The controller reports in the panel
 * native frame and knows nothing about rotation, so every point goes through
 * ngl_from_panel() first - otherwise taps land in the wrong place the moment
 * the tablet is turned.
 *
 * More than one finger is read because the on-screen keyboard needs it: shift
 * and ctrl are held with one hand while the other types, which is not a
 * gesture that can be expressed as a sequence of single taps. Everything else
 * in the system looks at the first point and is unaffected.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"

#include "ngl.h"
#include "neos_api.h"
#include "neos_audio.h"
#include "neos_bar.h"
#include "neos_boot.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "touch";

#define POLL_MS      20     /* 50 Hz: a modifier key has to feel held, not sampled */
#define TAP_SLOP_PX  24     /* a finger rolls this much on a deliberate tap */

static esp_lcd_touch_handle_t s_tp;

/*
 * The live point set.
 *
 * Written by the poll task and read from every other task without a lock. The
 * count is published after the coordinates and cleared before them, so a
 * reader can see a stale position but never a position belonging to a finger
 * that was not there - which is the only ordering that matters when the worst
 * case is one frame of a key highlight.
 */
static volatile int     s_n;
static volatile int16_t s_px[NEOS_TOUCH_MAX], s_py[NEOS_TOUCH_MAX];

static volatile bool    s_down;
static volatile int16_t s_x, s_y;

/* A completed tap, waiting to be collected. */
static volatile bool    s_tap_pending;
static volatile int16_t s_tap_x, s_tap_y;

/* Where the current press started, to tell a tap from a drag. */
static int16_t s_press_x, s_press_y;

/*
 * Nested rather than boolean, for the same reason overlays are: the Wi-Fi
 * panel captures the finger, then raises the keyboard which captures it too.
 * A boolean would hand input back to the app when the keyboard closed, with
 * the list still up.
 */
static volatile int s_captured;

/* ------------------------------------------------------------------ */

/** Read every finger on the glass. Returns how many, in screen coordinates. */
static int read_points(int16_t *xs, int16_t *ys)
{
    uint16_t px[NEOS_TOUCH_MAX], py[NEOS_TOUCH_MAX];
    uint8_t  count = 0;

    esp_lcd_touch_read_data(s_tp);
    if (!esp_lcd_touch_get_coordinates(s_tp, px, py, NULL, &count, NEOS_TOUCH_MAX)) {
        return 0;
    }
    if (count > NEOS_TOUCH_MAX) {
        count = NEOS_TOUCH_MAX;
    }
    for (int i = 0; i < count; i++) {
        ngl_from_panel((int16_t)px[i], (int16_t)py[i], &xs[i], &ys[i]);
    }
    return count;
}

/*
 * What a tap on the system bar does.
 *
 * Consumed here and never delivered to the app: these are the system's own
 * controls, and an app should not be able to see, let alone swallow, the
 * gesture that closes it or the one that opens the Wi-Fi list.
 *
 * Nothing on the bar answers while a panel is up. The panel is modal - that is
 * what modal means - and the close button in particular would otherwise close
 * the app underneath a dialog the app itself is waiting on.
 */
static bool bar_took_it(int16_t x, int16_t y)
{
    if (s_captured > 0) {
        return false;
    }
    switch (neos_bar_hit(x, y)) {
    case NEOS_BAR_CLOSE:
        ESP_LOGI(TAG, "close button");
        neos_app_request_close();
        return true;
    case NEOS_BAR_WIFI:
        neos_ui_open(NEOS_PANEL_WIFI);
        return true;
    case NEOS_BAR_CLOCK:
        neos_ui_open(NEOS_PANEL_CLOCK);
        return true;
    case NEOS_BAR_BATTERY:
        neos_ui_open(NEOS_PANEL_BATTERY);
        return true;
    default:
        return false;
    }
}

/*
 * The way out of a fullscreen app, and it costs no screen at all.
 *
 * An app that has taken the panel has taken the close button with it, so the
 * one control that is in the same place in every app is gone and the only one
 * left is one the app drew - which is fine until it drew it wrong, or under a
 * finger, or not at all. This is the replacement, and it is seen here in the
 * touch task before the app is told about any of it, so an app cannot swallow
 * it the way it could swallow a tap in a corner.
 *
 * Four fingers, because two are already on the glass in anything played with
 * thumbs and three is a plausible fumble; four flat on the panel is not
 * something that happens by accident. Held rather than tapped for the same
 * reason.
 *
 * It does both halves: the bar comes back, so the system's own close button is
 * there to be pressed even if the app carries on regardless, and the app is
 * asked to close, which is what a well-behaved one acts on. Neither can help
 * against an app that has stopped polling - it is running on the boot task as
 * ordinary code and there is nothing to preempt - and the bar's own button
 * could not either.
 */
#define ESCAPE_FINGERS 4
#define ESCAPE_MS    800

static void escape_watch(int n)
{
    static uint32_t held_ms;

    if (n < ESCAPE_FINGERS || !neos_is_fullscreen()) {
        held_ms = 0;
        return;
    }
    held_ms += POLL_MS;
    if (held_ms < ESCAPE_MS) {
        return;
    }
    held_ms = 0;

    ESP_LOGI(TAG, "%d fingers held - taking the panel back", n);
    neos_fullscreen(false);
    neos_app_request_close();
}

static void touch_task(void *arg)
{
    (void)arg;
    bool was_down = false;

    for (;;) {
        int16_t xs[NEOS_TOUCH_MAX], ys[NEOS_TOUCH_MAX];
        const int n = read_points(xs, ys);
        const bool down = n > 0;

        escape_watch(n);

        if (down) {
            for (int i = 0; i < n; i++) {
                s_px[i] = xs[i];
                s_py[i] = ys[i];
            }
            s_n = n;
            s_x = xs[0];
            s_y = ys[0];
            if (!was_down) {
                s_press_x = xs[0];
                s_press_y = ys[0];
                /*
                 * The click is on the press, not on the tap. A key that
                 * clicked when the finger came off would feel like it was
                 * confirming something rather than being pressed, and the
                 * whole point of the sound is to say the glass registered
                 * the touch at the moment it did.
                 */
                neos_audio_click();
            }
        } else {
            s_n = 0;
            if (was_down) {
                /* Release: a tap only if the finger stayed put. */
                if (abs(s_x - s_press_x) <= TAP_SLOP_PX &&
                    abs(s_y - s_press_y) <= TAP_SLOP_PX &&
                    !bar_took_it(s_x, s_y)) {
                    s_tap_x = s_x;
                    s_tap_y = s_y;
                    s_tap_pending = true;
                }
            }
        }

        s_down = down;
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t neos_touch_init(void)
{
    const bsp_touch_config_t cfg = {0};
    esp_err_t err = bsp_touch_new(&cfg, &s_tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no touch panel: %s", esp_err_to_name(err));
        s_tp = NULL;
        return err;
    }

    xTaskCreate(touch_task, "touch", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "touch up, %d points, polling every %d ms", NEOS_TOUCH_MAX, POLL_MS);
    return ESP_OK;
}

void neos_touch_drop(void)
{
    /* A tap nobody collected is stale the moment the screen changes hands. */
    s_tap_pending = false;
}

bool neos_touch_present(void)
{
    return s_tp != NULL;
}

void neos_touch_capture(bool on)
{
    if (on) {
        s_captured++;
    } else if (s_captured > 0) {
        s_captured--;
    }
    s_tap_pending = false;    /* whatever is queued was aimed at the other side */
}

bool neos_touch_captured(void)
{
    return s_captured > 0;
}

/* ------------------------------------------------------------------ */
/* Readers                                                             */
/* ------------------------------------------------------------------ */

static int points_into(neos_touch_t *out, int max)
{
    const int n = s_n;
    for (int i = 0; i < n && i < max; i++) {
        out[i].x    = s_px[i];
        out[i].y    = s_py[i];
        out[i].down = true;
    }
    return n;
}

bool neos_touch_tap_os(int16_t *x, int16_t *y)
{
    if (!s_tap_pending) {
        return false;
    }
    if (x) { *x = s_tap_x; }
    if (y) { *y = s_tap_y; }
    s_tap_pending = false;      /* collected: one tap is delivered once */
    return true;
}

int neos_touch_points_os(neos_touch_t *out, int max)
{
    if (!s_tp || !out || max <= 0) {
        return 0;
    }
    return points_into(out, max);
}

bool neos_touch(neos_touch_t *out)
{
    if (!s_tp || !out) {
        return false;
    }
    /*
     * A captured finger reads as no finger rather than as a stale position.
     * An app polling this through a modal panel is not entitled to know where
     * the user is typing, and "up" is the one answer that cannot make it do
     * something wrong.
     */
    if (s_captured > 0) {
        out->x    = 0;
        out->y    = 0;
        out->down = false;
        return true;
    }
    out->x    = s_x;
    out->y    = s_y;
    out->down = s_down;
    return true;
}

bool neos_touch_tap(int16_t *x, int16_t *y)
{
    if (s_captured > 0) {
        return false;
    }
    return neos_touch_tap_os(x, y);
}

int neos_touch_points(neos_touch_t *out, int max)
{
    if (s_captured > 0) {
        return 0;
    }
    return neos_touch_points_os(out, max);
}
