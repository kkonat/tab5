#include "ngl.h"
#include "ngl_theme.h"
#include "ngl_icon_data.h"

#include <string.h>

#include "neos_msg.h"

static void centred(ngl_surface_t *s, int16_t y, const char *text,
                    const ngl_font_t *font, ngl_color_t col)
{
    const ngl_rect_t a = ngl_app_area();
    const int16_t w = ngl_text_width(font, text);
    ngl_text(s, (int16_t)(a.x + (a.w - w) / 2), y, text, font, col);
}

/*
 * The message on screen right now.
 *
 * Kept so that a rotation can repaint it. A message screen is shown exactly
 * when no app is running - that is what it means - so there is nobody else to
 * ask for a redraw, and the OS has to be able to redraw its own screens.
 */
static neos_msg_kind_t s_kind;
static char            s_title[64];
static char            s_detail[96];
static bool            s_showing;

static void paint(neos_msg_kind_t kind, const char *title, const char *detail);

void neos_msg(neos_msg_kind_t kind, const char *title, const char *detail)
{
    s_kind = kind;
    strlcpy(s_title, title ? title : "", sizeof(s_title));
    strlcpy(s_detail, detail ? detail : "", sizeof(s_detail));
    s_showing = true;
    paint(kind, s_title, s_detail);
}

void neos_msg_repaint(void)
{
    if (s_showing) {
        paint(s_kind, s_title, s_detail);
    }
}

void neos_msg_none(void)
{
    s_showing = false;
}

static void paint(neos_msg_kind_t kind, const char *title, const char *detail)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;     /* no panel: the log is the only output there is */
    }

    const ngl_icon_t *icon;
    ngl_color_t tint;
    switch (kind) {
    case NEOS_MSG_WAIT:    icon = &ngl_icon_clock_32;    tint = TH_TEXT_DIM; break;
    case NEOS_MSG_MISSING: icon = &ngl_icon_folder_32;   tint = TH_WARN;     break;
    default:               icon = &ngl_icon_dizzy_32;    tint = TH_BAD;      break;
    }

    ngl_clear(sc, TH_BG);

    const ngl_rect_t a = ngl_app_area();
    const int16_t mid = (int16_t)(a.y + a.h / 2);

    ngl_icon(sc, (int16_t)(a.x + (a.w - icon->w) / 2), (int16_t)(mid - 120), icon, tint);
    centred(sc, (int16_t)(mid - 50), title, &ngl_font_large, TH_TEXT);
    if (detail && detail[0]) {
        centred(sc, (int16_t)(mid + 10), detail, &ngl_font_small, TH_TEXT_DIM);
    }
    ngl_flush();
}
