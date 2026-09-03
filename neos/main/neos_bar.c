/*
 * The system bar.
 *
 * Owned by NeOS, not by the shell. It used to belong to whichever app was the
 * shell, which meant it vanished the moment that app handed over - and a
 * launched app was then on a bare screen with no way back, because the only
 * close button was drawn by an app that was no longer resident.
 *
 * Keeping it here also means there is exactly one close button, at one place,
 * that every app gets for free and no app can draw over: ngl reserves the
 * strip and refuses to clip anything else into it.
 *
 * Everything below is in screen coordinates, which is the rotated frame - the
 * bar is the top of the screen, not the top of the panel, so it and its button
 * follow the tablet around without anything special happening.
 */
#include <string.h>

#include "esp_log.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_bar.h"
#include "neos_boot.h"
#include "neos_status.h"

static const char *TAG = "bar";

#define BAR_H   56
#define BAR_BTN 40
#define BAR_PAD 8

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

ngl_rect_t neos_bar_close_rect(void)
{
    const ngl_rect_t bar = ngl_bar_rect();
    return ngl_rect((int16_t)(bar.w - BAR_BTN - BAR_PAD),
                    (int16_t)((bar.h - BAR_BTN) / 2), BAR_BTN, BAR_BTN);
}

bool neos_bar_hit_close(int16_t x, int16_t y)
{
    if (!s_closable) {
        return false;
    }
    const ngl_rect_t r = neos_bar_close_rect();
    return ngl_rect_contains(&r, x, y);
}

/*
 * Widgets claim fixed space at the edges and the status line takes whatever is
 * left, so adding a clock or a battery gauge shrinks the status area instead
 * of drawing on top of it. That is why the geometry is computed rather than
 * written down as constants.
 */
static int16_t right_reserved(void)
{
    return (int16_t)(BAR_PAD + (s_closable ? BAR_BTN : 0));
}

static int16_t left_reserved(void)
{
    return (int16_t)(14 + ngl_text_width(&ngl_font_small, "NeOS"));
}

static ngl_rect_t status_rect(void)
{
    const ngl_rect_t bar = ngl_bar_rect();
    const int16_t l = (int16_t)(left_reserved() + 16);
    const int16_t r = (int16_t)(right_reserved() + 16);
    int16_t w = (int16_t)(bar.w - l - r);
    if (w < 0) {
        w = 0;
    }
    return ngl_rect(l, (int16_t)((bar.h - ngl_font_small.height) / 2),
                    w, ngl_font_small.height);
}

/* ngl calls this from ngl_clear() with the reservation lifted, so this is the
   only code that can draw here. */
static void bar_painter(ngl_surface_t *s, ngl_rect_t bar)
{
    ngl_fill_rect(s, bar, TH_BAR_BG);
    ngl_hline(s, 0, (int16_t)(bar.h - 1), bar.w, TH_BAR_EDGE);
    ngl_text(s, 14, (int16_t)((bar.h - 32) / 2), "NeOS", &ngl_font_small, TH_TEXT_DIM);

    if (s_closable) {
        const ngl_rect_t b = neos_bar_close_rect();
        ngl_fill_round_rect(s, b, 8, TH_CLOSE_FILL);
        ngl_draw_round_rect(s, b, 8, TH_CLOSE_LINE, 2);

        const ngl_icon_t *ic = ngl_icon_find("close", 24);
        if (ic) {
            ngl_icon(s, (int16_t)(b.x + (b.w - ic->w) / 2),
                     (int16_t)(b.y + (b.h - ic->h) / 2), ic, TH_CLOSE_X);
        }
    }

    /* Last, because the fill above wiped whatever was showing. */
    neos_status_paint();
}

void neos_bar_init(void)
{
    ngl_reserve_top(BAR_H, bar_painter);
    neos_status_init(status_rect);
    ESP_LOGI(TAG, "system bar reserved, %d px", BAR_H);
}

void neos_bar_set_closable(bool closable)
{
    if (s_closable == closable) {
        return;
    }
    s_closable = closable;
    ngl_bar_paint();     /* the status area grows into the freed space, or yields it */
    ngl_flush();
}
