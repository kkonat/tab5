/*
 * The NeOS face: the system's own palette, over falling glyphs.
 *
 * The only face that draws in ngl_theme.h, because it is the only one that is
 * meant to look like the rest of the machine rather than like a particular
 * piece of hardware.
 *
 * Two things are moving at once and they share a screen without compositing.
 * The rain runs underneath at its own pace and repaints only the cells that
 * changed; the clock is repainted once a second, and only the part of it that
 * changed. What keeps them out of each other's way is a reservation - the rain
 * is handed the rectangles the text occupies and steps around them - rather
 * than a z-order, because ngl has no alpha and a drop that walked through a
 * digit would leave a black cell in it until the next tick.
 *
 * The glow is four offset copies of the text under the real one. That is not a
 * blur and does not pretend to be: a bitmap font magnified eight times has
 * edges a pixel-accurate blur would only make muddy, and four dim copies at
 * one radius reads as phosphor bloom for the cost of four more draws a minute.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "clock.h"

/* The strip is one line of the large font plus room for the icon. */
#define STRIP_H   56
#define STACK_GAP 30
#define SEC_GAP   14
#define MARGIN    24

/* How far the glow copies sit from the text, as a fraction of the block size,
   and how much of the text colour survives in them. */
#define GLOW_MIX  60

static struct {
    ngl_rect_t band;      /* the full-width row the time sits in */
    ngl_rect_t strip;
    int16_t    scale, sscale;
    int16_t    tx, ty;    /* top left of "HH:MM" */
    int16_t    sx, sy;    /* top left of the seconds, on their own line */
    int16_t    big_w, big_h, sec_w, sec_h;
} L;

static void layout(const clock_frame_t *f)
{
    const ngl_font_t *fnt = &ngl_font_large;

    /*
     * The biggest scale that fits both ways round.
     *
     * Width binds in portrait and height in landscape, so both are asked and
     * the smaller answer wins - which is this face's whole response to being
     * turned over. The seconds go on their own line rather than trailing the
     * time: sharing a line costs about a third of the width, and on a 720-wide
     * panel that is the difference between digits 240 pixels tall and 190.
     */
    const int16_t avail_w = (int16_t)(f->area.w - 2 * MARGIN);
    const int16_t avail_h = (int16_t)((f->area.h - STRIP_H - STACK_GAP) / 2);

    const int16_t by_w = (int16_t)(avail_w / (5 * fnt->width));
    const int16_t by_h = (int16_t)(avail_h / fnt->height);
    L.scale = by_w < by_h ? by_w : by_h;
    if (L.scale < 1) {
        L.scale = 1;
    }
    L.sscale = (int16_t)(L.scale / 2);
    if (L.sscale < 1) {
        L.sscale = 1;
    }

    L.big_w = clk_blocky_w(fnt, "00:00", L.scale);
    L.big_h = (int16_t)(fnt->height * L.scale);
    L.sec_w = clk_blocky_w(fnt, "00", L.sscale);
    L.sec_h = (int16_t)(fnt->height * L.sscale);

    const int16_t inner = (int16_t)(L.big_h + SEC_GAP + L.sec_h);
    clk_stack(f->area, inner, STRIP_H, STACK_GAP, &L.band, &L.strip);

    L.tx = (int16_t)(L.band.x + (L.band.w - L.big_w) / 2);
    L.ty = L.band.y;
    L.sx = (int16_t)(L.band.x + (L.band.w - L.sec_w) / 2);
    L.sy = (int16_t)(L.ty + L.big_h + SEC_GAP);
}

/*
 * The rectangles that have to be erased, which are not the ones the text was
 * measured into: glow_text() draws four copies a whole `scale` outside the
 * glyph box, so an erase of the box alone would leave last minute's halo
 * standing around this minute's digits.
 */
static ngl_rect_t big_box(void)
{
    return clk_inflate(ngl_rect(L.tx, L.ty, L.big_w, L.big_h), L.scale);
}

static ngl_rect_t sec_box(void)
{
    return clk_inflate(ngl_rect(L.sx, L.sy, L.sec_w, L.sec_h), L.sscale);
}

/** The text plus its bloom, in one place so the two are never out of step. */
static void glow_text(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                      int16_t scale, ngl_color_t c)
{
    const ngl_font_t *fnt = &ngl_font_large;
    const int16_t d = (int16_t)(scale > 1 ? scale : 1);
    const ngl_color_t halo = clk_mix(TH_BG, c, GLOW_MIX);

    clk_blocky(s, (int16_t)(x - d), y, str, fnt, scale, halo);
    clk_blocky(s, (int16_t)(x + d), y, str, fnt, scale, halo);
    clk_blocky(s, x, (int16_t)(y - d), str, fnt, scale, halo);
    clk_blocky(s, x, (int16_t)(y + d), str, fnt, scale, halo);
    clk_blocky(s, x, y, str, fnt, scale, c);
}

static void paint_time(ngl_surface_t *s, const clock_frame_t *f)
{
    char buf[8];
    if (f->have_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", f->t.hour, f->t.min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }

    ngl_fill_rect(s, big_box(), TH_BG);
    glow_text(s, L.tx, L.ty, buf, L.scale, TH_TEXT);
}

static void paint_secs(ngl_surface_t *s, const clock_frame_t *f)
{
    char buf[8];
    if (f->have_time) {
        snprintf(buf, sizeof(buf), "%02d", f->t.sec);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }

    ngl_fill_rect(s, sec_box(), TH_BG);
    glow_text(s, L.sx, L.sy, buf, L.sscale, TH_TEXT_DIM);
}

static void paint(const clock_frame_t *f, const void *cfg)
{
    (void)cfg;
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }

    if (f->full) {
        ngl_clear(s, TH_BG);
        layout(f);

        clk_rain_layout(f->area);
        /*
         * Reserved before the first drop falls, and re-reserved nowhere else:
         * the time is monospaced, so the rectangle it occupies is the same at
         * 09:05 as at 23:59 and only a rotation can change it.
         */
        clk_rain_reserve(big_box());
        clk_rain_reserve(sec_box());
        clk_rain_reserve(L.strip);

        paint_time(s, f);
        paint_secs(s, f);
        clk_wx_strip(s, L.strip, f, &ngl_font_large, TH_TEXT_DIM, TH_BG);
        ngl_flush();
        return;
    }

    /* The rain flushes its own bands; see clock_rain.c for why that is the
       cheap shape and one big flush is not. */
    clk_rain_step();

    if (f->min) {
        paint_time(s, f);
        clk_wx_strip(s, L.strip, f, &ngl_font_large, TH_TEXT_DIM, TH_BG);
    }
    if (f->sec) {
        paint_secs(s, f);
    }
    if (f->sec || f->min) {
        ngl_flush();
    }
}

const clock_face_t clock_face_neos = {
    .key      = "neos",
    .name     = "NeOS",
    .blurb    = "phosphor, over glyph rain",
    .paint    = paint,
    .leave    = 0,
    .cfg      = 0,
    /* The rain's pace, not the clock's. Fast enough to fall convincingly,
       slow enough to leave the core to everything else. */
    .frame_ms = 50,
};
