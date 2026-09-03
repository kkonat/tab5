/*
 * Touch input.
 *
 * One task polls the controller and turns a stream of coordinates into the two
 * things apps actually want: where the finger is now, and "a tap happened
 * here". Edge detection and the movement tolerance live here rather than in
 * every app, because getting them subtly different per app is how a UI starts
 * feeling inconsistent.
 *
 * Coordinates leave here in screen space. The controller reports in the panel
 * native frame and knows nothing about rotation, so every point goes through
 * ngl_from_panel() first - otherwise taps land in the wrong place the moment
 * the tablet is turned.
 */
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"
#include "bsp/touch.h"

#include "ngl.h"
#include "neos_api.h"
#include "neos_bar.h"
#include "neos_boot.h"
#include "neos_touch.h"

static const char *TAG = "touch";

#define POLL_MS      30     /* ~33 Hz: smooth enough to track, cheap enough to ignore */
#define TAP_SLOP_PX  24     /* a finger rolls this much on a deliberate tap */

static esp_lcd_touch_handle_t s_tp;

static volatile bool    s_down;
static volatile int16_t s_x, s_y;

/* A completed tap, waiting to be collected. */
static volatile bool    s_tap_pending;
static volatile int16_t s_tap_x, s_tap_y;

/* Where the current press started, to tell a tap from a drag. */
static int16_t s_press_x, s_press_y;

static bool read_point(int16_t *lx, int16_t *ly)
{
    uint16_t px[1], py[1];
    uint8_t  count = 0;

    esp_lcd_touch_read_data(s_tp);
    if (!esp_lcd_touch_get_coordinates(s_tp, px, py, NULL, &count, 1) || count == 0) {
        return false;
    }
    ngl_from_panel((int16_t)px[0], (int16_t)py[0], lx, ly);
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;
    bool was_down = false;

    for (;;) {
        int16_t x = 0, y = 0;
        const bool down = read_point(&x, &y);

        if (down) {
            s_x = x;
            s_y = y;
            if (!was_down) {
                s_press_x = x;
                s_press_y = y;
            }
        } else if (was_down) {
            /* Release: a tap only if the finger stayed put. */
            if (abs(s_x - s_press_x) <= TAP_SLOP_PX &&
                abs(s_y - s_press_y) <= TAP_SLOP_PX) {

                /*
                 * The close button belongs to NeOS, so the tap that hits it
                 * is consumed here and never reaches the app - an app should
                 * not be able to see, let alone swallow, the gesture that
                 * closes it.
                 */
                if (neos_bar_hit_close(s_x, s_y)) {
                    ESP_LOGI(TAG, "close button");
                    neos_app_request_close();
                } else {
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
    ESP_LOGI(TAG, "touch up, polling every %d ms", POLL_MS);
    return ESP_OK;
}

void neos_touch_drop(void)
{
    /* A tap nobody collected is stale the moment the screen changes hands. */
    s_tap_pending = false;
}

bool neos_touch(neos_touch_t *out)
{
    if (!s_tp || !out) {
        return false;
    }
    out->x    = s_x;
    out->y    = s_y;
    out->down = s_down;
    return true;
}

bool neos_touch_tap(int16_t *x, int16_t *y)
{
    if (!s_tap_pending) {
        return false;
    }
    if (x) {
        *x = s_tap_x;
    }
    if (y) {
        *y = s_tap_y;
    }
    s_tap_pending = false;      /* collected: one tap is delivered once */
    return true;
}
