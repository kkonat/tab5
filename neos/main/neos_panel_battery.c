/*
 * The battery detail page.
 *
 * The bar icon only has room for a colour; this is where a tap on it lands to
 * see the numbers behind that colour - the actual pack voltage, the current
 * flowing, and where that voltage sits against the 2S LiPo lines the icon
 * flashes over. Same source as the icon, same thresholds (NEOS_BATTERY_2S_*
 * in neos_bar.h), so the two can never disagree about whether the pack is in
 * danger.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_bar.h"
#include "neos_panel.h"
#include "neos_sys.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "battpanel";

#define POLL_MS 40
#define TICK_MS 500     /* matches the bar's own widget tick, for the same flash rate */

#define MARGIN  16
#define ICON_W  96
#define ROW_H   48
#define GAP     10

static ngl_rect_t s_panel, s_content;
static ngl_rect_t s_icon, s_volt, s_current_row, s_power_row, s_status_row;

static void layout(void)
{
    const ngl_rect_t app = ngl_app_area();
    s_panel = ngl_rect((int16_t)(app.x + 12), (int16_t)(app.y + 12),
                       (int16_t)(app.w - 24), (int16_t)(app.h - 24));

    s_content = ngl_rect((int16_t)(s_panel.x + 2),
                         (int16_t)(s_panel.y + NEOS_UI_TITLE_H + 1),
                         (int16_t)(s_panel.w - 4),
                         (int16_t)(s_panel.h - NEOS_UI_TITLE_H - 3));

    const int16_t x = (int16_t)(s_content.x + MARGIN);
    const int16_t w = (int16_t)(s_content.w - 2 * MARGIN);
    int16_t y = (int16_t)(s_content.y + 16);

    s_icon = ngl_rect(x, y, ICON_W, ICON_W);
    s_volt = ngl_rect((int16_t)(x + ICON_W + GAP), y,
                      (int16_t)(w - ICON_W - GAP), ICON_W);
    y = (int16_t)(y + ICON_W + GAP);

    s_current_row = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_power_row   = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_status_row  = ngl_rect(x, y, w, ROW_H);
}

/* ------------------------------------------------------------------ */

static ngl_color_t level_color(bool danger, bool warn)
{
    return danger ? TH_BAD : (warn ? TH_WARN : TH_OK);
}

static void paint_icon(ngl_surface_t *s, bool have, bool danger, bool warn, bool blink_on)
{
    ngl_fill_rect(s, s_icon, TH_MODAL_BG);
    if (!have || (danger && !blink_on)) {
        return;
    }
    const ngl_icon_t *ic = ngl_icon_find("battery", 32);
    if (!ic) {
        return;
    }
    ngl_icon(s, (int16_t)(s_icon.x + (s_icon.w - ic->w) / 2),
             (int16_t)(s_icon.y + (s_icon.h - ic->h) / 2), ic,
             level_color(danger, warn));
}

static void paint_volt(ngl_surface_t *s, bool have, int32_t bus_mv,
                       bool danger, bool warn, bool blink_on)
{
    ngl_fill_rect(s, s_volt, TH_MODAL_BG);

    char buf[16];
    if (!have) {
        snprintf(buf, sizeof(buf), "no reading");
        neos_ui_text_left(s, s_volt, buf, &ngl_font_large, TH_TEXT_FAINT);
        return;
    }
    if (danger && !blink_on) {
        return;   /* the "off" half of the flash, same as the bar icon */
    }
    snprintf(buf, sizeof(buf), "%d.%02d V", (int)(bus_mv / 1000), (int)((bus_mv % 1000) / 10));
    neos_ui_text_left(s, s_volt, buf, &ngl_font_large, level_color(danger, warn));
}

static void paint_current(ngl_surface_t *s, bool have, int32_t current_ma)
{
    ngl_fill_rect(s, s_current_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_current_row, "Current", &ngl_font_small, TH_TEXT_DIM);

    char buf[24];
    if (have) {
        snprintf(buf, sizeof(buf), "%d mA", (int)current_ma);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    neos_ui_text_right(s, s_current_row, buf, &ngl_font_small, TH_TEXT);
}

static void paint_power(ngl_surface_t *s, bool have, int32_t power_mw)
{
    ngl_fill_rect(s, s_power_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_power_row, "Power", &ngl_font_small, TH_TEXT_DIM);

    char buf[24];
    if (have) {
        snprintf(buf, sizeof(buf), "%d mW", (int)power_mw);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    neos_ui_text_right(s, s_power_row, buf, &ngl_font_small, TH_TEXT);
}

static void paint_status(ngl_surface_t *s, bool have, bool danger, bool warn)
{
    ngl_fill_rect(s, s_status_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_status_row, "Status", &ngl_font_small, TH_TEXT_DIM);

    const char *msg;
    ngl_color_t c;
    if (!have) {
        msg = "power monitor not found";
        c   = TH_TEXT_FAINT;
    } else if (danger) {
        msg = "DANGER - charge the pack now";
        c   = TH_BAD;
    } else if (warn) {
        msg = "getting low - charge soon";
        c   = TH_WARN;
    } else {
        msg = "healthy";
        c   = TH_OK;
    }
    neos_ui_text_right(s, s_status_row, msg, &ngl_font_small, c);
}

/* ------------------------------------------------------------------ */

void neos_panel_battery(void)
{
    if (!ngl_screen()) {
        return;
    }
    layout();

    neos_power_t p = {0};
    bool have = neos_power_read(&p);
    bool danger = have && p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;
    bool warn   = have && !danger && p.bus_mv < NEOS_BATTERY_2S_WARN_MV;
    bool blink_on = true;

    ngl_overlay_restore();
    ngl_surface_t *s = ngl_overlay_begin(ngl_app_area());
    if (s) {
        neos_ui_dim(s);
        neos_ui_frame(s, s_panel, "Battery");
        paint_icon(s, have, danger, warn, blink_on);
        paint_volt(s, have, p.bus_mv, danger, warn, blink_on);
        paint_current(s, have, p.current_ma);
        paint_power(s, have, p.power_mw);
        paint_status(s, have, danger, warn);
        ngl_overlay_end();
        ngl_flush();
    }

    uint32_t since = 0;
    ngl_rect_t area = ngl_app_area();

    while (!neos_ui_should_close()) {
        int16_t tx = 0, ty = 0;
        if (neos_touch_tap_os(&tx, &ty)) {
            const ngl_rect_t close = neos_ui_close_rect(s_panel);
            if (ngl_rect_contains(&close, tx, ty)) {
                break;
            }
        }

        bool all = false;
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

        have = neos_power_read(&p);
        danger = have && p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;
        warn   = have && !danger && p.bus_mv < NEOS_BATTERY_2S_WARN_MV;

        /* Flip every tick while in danger, same as the bar icon, so the two
           flash in step instead of at odds with each other. */
        if (danger) {
            blink_on = !blink_on;
        } else {
            blink_on = true;
        }

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
                neos_ui_frame(sc, s_panel, "Battery");
            }
            paint_icon(sc, have, danger, warn, blink_on);
            paint_volt(sc, have, p.bus_mv, danger, warn, blink_on);
            paint_current(sc, have, p.current_ma);
            paint_power(sc, have, p.power_mw);
            paint_status(sc, have, danger, warn);
            ngl_overlay_end();
            ngl_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    ESP_LOGI(TAG, "battery page closed");
}
