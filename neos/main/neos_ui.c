/*
 * The panel runner and the widgets panels are built out of.
 *
 * One task, one panel at a time, and a request flag between it and the touch
 * poller that raised it. That is the whole scheduler: there is one screen, so
 * a second panel is not a thing to queue, it is a thing to ignore.
 */
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_panel.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "ui";

static volatile neos_panel_t s_request;
static volatile bool         s_active;
static volatile bool         s_close_requested;

/* ------------------------------------------------------------------ */
/* The runner                                                          */
/* ------------------------------------------------------------------ */

/*
 * Everything that has to be true while a panel is up, in one place.
 *
 * The order matters on the way in and on the way out. The screen is taken
 * before the finger, so there is no window where taps are being swallowed but
 * the app is still drawing over the panel that has not appeared yet; and given
 * back after, so the app's own frame is on the glass before it can act on a
 * tap again.
 */
static void run_panel(neos_panel_t which)
{
    if (!ngl_overlay_enter()) {
        ESP_LOGW(TAG, "cannot take the screen for a panel");
        return;
    }
    neos_touch_capture(true);
    s_close_requested = false;
    s_active = true;

    switch (which) {
    case NEOS_PANEL_WIFI:    neos_panel_wifi();    break;
    case NEOS_PANEL_CLOCK:   neos_panel_clock();   break;
    case NEOS_PANEL_BATTERY: neos_panel_battery(); break;
    default: break;
    }

    s_active = false;
    neos_touch_capture(false);
    ngl_overlay_leave();
    neos_touch_drop();          /* the tap that closed it is not the app's */
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        const neos_panel_t want = s_request;
        if (want == NEOS_PANEL_NONE) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        run_panel(want);
        s_request = NEOS_PANEL_NONE;
    }
}

void neos_ui_init(void)
{
    /*
     * Deep, because a panel runs the keyboard, the keyboard runs the layout,
     * and Wi-Fi scanning underneath it all calls into lwIP. This is the one
     * NeOS task with a UI and a network stack on the same stack frame.
     */
    xTaskCreate(ui_task, "ui", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "panel runner up");
}

void neos_ui_open(neos_panel_t p)
{
    if (s_active || s_request != NEOS_PANEL_NONE) {
        return;
    }
    s_request = p;
}

void neos_ui_close(void)
{
    /*
     * The request as well as the panel. There is a window between the tap that
     * asked for a panel and the ui task picking it up, and a panel that opened
     * inside that window would be sitting over an app that had already gone -
     * so it would restore the wrong screen when it closed.
     */
    s_request = NEOS_PANEL_NONE;
    s_close_requested = true;
}

bool neos_ui_active(void)
{
    return s_active;
}

bool neos_ui_should_close(void)
{
    return s_close_requested;
}

/* The app-facing name for the same question. */
bool neos_ui_busy(void)
{
    return ngl_overlay_active();
}

/* ------------------------------------------------------------------ */
/* Chrome                                                              */
/* ------------------------------------------------------------------ */

#define CLOSE_BTN 40
#define CLOSE_PAD 8

void neos_ui_dim(ngl_surface_t *s)
{
    const ngl_rect_t app = ngl_app_area();
    ngl_dim_rect(s, app, TH_SCRIM);
}

ngl_rect_t neos_ui_close_rect(ngl_rect_t r)
{
    return ngl_rect((int16_t)(r.x + r.w - CLOSE_BTN - CLOSE_PAD),
                    (int16_t)(r.y + (NEOS_UI_TITLE_H - CLOSE_BTN) / 2),
                    CLOSE_BTN, CLOSE_BTN);
}

ngl_rect_t neos_ui_frame(ngl_surface_t *s, ngl_rect_t r, const char *title)
{
    ngl_fill_round_rect(s, r, 10, TH_MODAL_BG);
    ngl_draw_round_rect(s, r, 10, TH_MODAL_EDGE, 2);

    if (title && title[0]) {
        ngl_text(s, (int16_t)(r.x + 18),
                 (int16_t)(r.y + (NEOS_UI_TITLE_H - ngl_font_small.height) / 2),
                 title, &ngl_font_small, TH_TEXT);
    }
    ngl_hline(s, (int16_t)(r.x + 1), (int16_t)(r.y + NEOS_UI_TITLE_H),
              (int16_t)(r.w - 2), TH_RULE);

    const ngl_rect_t b = neos_ui_close_rect(r);
    ngl_fill_round_rect(s, b, 8, TH_CLOSE_FILL);
    ngl_draw_round_rect(s, b, 8, TH_CLOSE_LINE, 2);
    const ngl_icon_t *ic = ngl_icon_find("close", 24);
    if (ic) {
        ngl_icon(s, (int16_t)(b.x + (b.w - ic->w) / 2),
                 (int16_t)(b.y + (b.h - ic->h) / 2), ic, TH_CLOSE_X);
    }

    return ngl_rect((int16_t)(r.x + 2), (int16_t)(r.y + NEOS_UI_TITLE_H + 1),
                    (int16_t)(r.w - 4), (int16_t)(r.h - NEOS_UI_TITLE_H - 3));
}

void neos_ui_button(ngl_surface_t *s, ngl_rect_t r, const char *label, bool on)
{
    const int16_t rad = 8;
    ngl_fill_round_rect(s, r, rad, on ? TH_KEY_LATCH : TH_KEY_FILL);
    ngl_draw_round_rect(s, r, rad, on ? TH_ACCENT : TH_KEY_EDGE, 2);
    neos_ui_text_centred(s, r, label, &ngl_font_small, on ? TH_GLOW : TH_KEY_TEXT);
}

/*
 * All three of these clip to the box they are given.
 *
 * Not decoration: every string a panel draws is either a network name or a
 * date the user chose, and neither has a length this layout knew about. Text
 * that outgrows its cell has to stop at the edge rather than run into the
 * label beside it.
 */
static void text_at(ngl_surface_t *s, ngl_rect_t box, int16_t x, const char *str,
                    const ngl_font_t *f, ngl_color_t c)
{
    const ngl_rect_t saved = ngl_surface_clip(s);
    ngl_rect_t inner;
    if (!ngl_rect_intersect(&box, &saved, &inner)) {
        return;
    }
    ngl_clip_set(s, &inner);
    ngl_text(s, x, (int16_t)(box.y + (box.h - f->height) / 2), str, f, c);
    ngl_clip_set(s, &saved);
}

void neos_ui_text_centred(ngl_surface_t *s, ngl_rect_t r, const char *str,
                          const ngl_font_t *f, ngl_color_t c)
{
    const int16_t w = ngl_text_width(f, str);
    text_at(s, r, (int16_t)(r.x + (r.w - w) / 2), str, f, c);
}

void neos_ui_text_left(ngl_surface_t *s, ngl_rect_t r, const char *str,
                       const ngl_font_t *f, ngl_color_t c)
{
    text_at(s, r, r.x, str, f, c);
}

void neos_ui_text_right(ngl_surface_t *s, ngl_rect_t r, const char *str,
                        const ngl_font_t *f, ngl_color_t c)
{
    const int16_t w = ngl_text_width(f, str);
    int16_t x = (int16_t)(r.x + r.w - w);
    if (x < r.x) {
        x = r.x;
    }
    text_at(s, r, x, str, f, c);
}
