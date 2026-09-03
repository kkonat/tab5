#include <string.h>

#include "neos_status.h"
#include "ngl_theme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "status";

#define STATUS_MAX     128
#define FRAME_MS       33      /* ~30 fps */
#define SCROLL_PX      2       /* per frame -> ~60 px/s */
#define SCROLL_GAP     64      /* blank run between repeats of the text */
#define SCROLL_PAUSE_MS 800    /* hold at the start before scrolling off */

static ngl_rect_t (*s_region_fn)(void);

static SemaphoreHandle_t s_lock;
static char      s_msg[STATUS_MAX];
static bool      s_active;
static int64_t   s_expires_us;      /* 0 = no expiry */
static int64_t   s_shown_us;

static bool      s_scrolling;
static int16_t   s_offset;
static int16_t   s_text_w;
static bool      s_needs_paint;

/* -1 when there is no transfer; 0..100 turns the line into a bar. */
static int       s_progress = -1;

static const ngl_font_t *const FONT = &ngl_font_small;

/* ------------------------------------------------------------------ */

static void paint_locked(void)
{
    if (!s_region_fn) {
        return;
    }
    const ngl_rect_t r = s_region_fn();
    if (r.w <= 0 || r.h <= 0) {
        return;
    }

    ngl_surface_t *s = ngl_bar_begin(r);
    if (!s) {
        return;
    }

    ngl_fill_rect(s, r, TH_BAR_BG);

    if (s_progress >= 0) {
        /*
         * A bar in the same slot a toast would use. It is the only thing on
         * the tablet that can say an upload is moving, and putting it here
         * means it costs no screen real estate and no app has to know.
         */
        const int16_t h = 10;
        const ngl_rect_t track = ngl_rect(r.x, (int16_t)(r.y + (r.h - h) / 2), r.w, h);
        ngl_draw_rect(s, track, TH_EDGE, 1);

        const int16_t inner = (int16_t)(track.w - 4);
        const int16_t fill = (int16_t)((int32_t)inner * s_progress / 100);
        if (fill > 0) {
            ngl_fill_rect(s, ngl_rect((int16_t)(track.x + 2), (int16_t)(track.y + 2),
                                      fill, (int16_t)(h - 4)), TH_ACCENT);
        }
    } else if (s_active && s_msg[0]) {
        const int16_t ty = (int16_t)(r.y + (r.h - FONT->height) / 2);
        if (!s_scrolling) {
            ngl_text(s, (int16_t)(r.x + (r.w - s_text_w) / 2), ty, s_msg, FONT, TH_TEXT_DIM);
        } else {
            /* Two copies, one gap apart, so the wrap is seamless. Clipping to
               the region is what keeps this inside its placeholder. */
            const int16_t x0 = (int16_t)(r.x - s_offset);
            ngl_text(s, x0, ty, s_msg, FONT, TH_TEXT_DIM);
            ngl_text(s, (int16_t)(x0 + s_text_w + SCROLL_GAP), ty, s_msg, FONT, TH_TEXT_DIM);
        }
    }

    ngl_bar_end();
    ngl_flush();
}

/* Decide centred vs marquee for the region we have right now. */
static void relayout_locked(void)
{
    s_text_w = ngl_text_width(FONT, s_msg);
    const ngl_rect_t r = s_region_fn ? s_region_fn() : ngl_rect(0, 0, 0, 0);
    s_scrolling = s_active && (s_text_w > r.w);
    s_offset = 0;
    s_needs_paint = true;
}

static void set_message(const char *msg, uint32_t ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (msg && msg[0]) {
        strlcpy(s_msg, msg, sizeof(s_msg));
        s_active = true;
        s_shown_us = esp_timer_get_time();
        s_expires_us = ms ? s_shown_us + (int64_t)ms * 1000 : 0;
    } else {
        s_msg[0] = 0;
        s_active = false;
        s_expires_us = 0;
    }
    relayout_locked();

    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------ */

static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));

        xSemaphoreTake(s_lock, portMAX_DELAY);

        const int64_t now = esp_timer_get_time();
        if (s_active && s_expires_us && now >= s_expires_us) {
            s_msg[0] = 0;
            s_active = false;
            s_expires_us = 0;
            relayout_locked();
        }

        if (s_active && s_scrolling &&
            (now - s_shown_us) > (int64_t)SCROLL_PAUSE_MS * 1000) {
            s_offset = (int16_t)(s_offset + SCROLL_PX);
            if (s_offset >= s_text_w + SCROLL_GAP) {
                s_offset = 0;
            }
            s_needs_paint = true;
        }

        const bool paint = s_needs_paint;
        s_needs_paint = false;
        if (paint) {
            paint_locked();     /* static text costs one paint, not 30 a second */
        }

        xSemaphoreGive(s_lock);
    }
}

void neos_status_init(ngl_rect_t (*region_fn)(void))
{
    s_region_fn = region_fn;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    xTaskCreate(status_task, "status", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "status line running at %d fps", 1000 / FRAME_MS);
}

void neos_status(const char *msg)
{
    set_message(msg, 0);
}

void neos_status_for(const char *msg, uint32_t ms)
{
    set_message(msg, ms);
}

void neos_status_clear(void)
{
    set_message(NULL, 0);
}

void neos_status_paint(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    relayout_locked();          /* the region may have changed size */
    paint_locked();
    xSemaphoreGive(s_lock);
}

void neos_status_progress(int percent)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (percent < 0) {
        s_progress = -1;
    } else {
        s_progress = percent > 100 ? 100 : percent;
    }
    s_needs_paint = true;
    xSemaphoreGive(s_lock);
}
