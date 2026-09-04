/*
 * The weather, and the several ways a display can admit to knowing it.
 *
 * NeOS does the fetching - there is one radio and one place worth knowing
 * about, so neos_weather() hands over a reading and nothing here opens a
 * socket. What is left is the display decisions the ABI deliberately does not
 * make, and there turn out to be more of them than "which picture".
 *
 * The one that shapes this file is that a real segment display cannot draw a
 * picture at all. Every symbol an LCD or a VFD will ever show is etched onto
 * the glass when it is made, all of them at once; lighting one means driving
 * one electrode, and the others do not disappear, they just stop being driven.
 * So there is a fixed, ordered set below, faces built on that kind of hardware
 * draw the whole row and light one of it, and only the two faces whose
 * hardware really can put an arbitrary glyph anywhere - the terminal and the
 * e-paper - get a free-form line.
 *
 * The other decision is what to do with no reading at all. No network is the
 * normal state of this tablet, not an error, so nothing draws a placeholder:
 * the free-form line gets shorter and the annunciator row simply lights
 * nothing, which is exactly what the real panel would do.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"

#include "clock.h"
#include "clock_icon_data.h"
#include "neos_weather.h"

/* ------------------------------------------------------------------ */
/* The fixed set                                                       */
/* ------------------------------------------------------------------ */

/*
 * Printed left to right roughly in order of how much weather there is, which
 * is the order these panels are always laid out in and reads as a scale rather
 * than as a list.
 */
static const ngl_icon_t *const WX_SET[CLK_WX_SET_N] = {
    &clock_icon_sun_48,
    &clock_icon_moon_48,
    &clock_icon_cloudsun_48,
    &clock_icon_cloudmoon_48,
    &clock_icon_cloud_48,
    &clock_icon_fog_48,
    &clock_icon_drizzle_48,
    &clock_icon_rain_48,
    &clock_icon_snow_48,
    &clock_icon_storm_48,
};

/* Positions in WX_SET, named so the mapping below reads as weather rather
   than as indices. */
enum {
    WX_SUN = 0, WX_MOON, WX_CLOUDSUN, WX_CLOUDMOON, WX_CLOUD,
    WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM,
};

const ngl_icon_t *clk_wx_set(int i)
{
    return (i >= 0 && i < CLK_WX_SET_N) ? WX_SET[i] : 0;
}

/**
 * WMO 4677 to one of the ten.
 *
 * Ranges rather than a hundred-entry table: the codes are grouped by kind
 * already - 5x drizzle, 6x rain, 7x snow, 8x showers, 9x storms - and this
 * panel has ten symbols, so most of the resolution in the code is detail no
 * display here could show.
 *
 * Day and night differ only up to code 2. Past that the sky is covered and
 * whether the sun is behind it is not something a symbol can say.
 */
static int wmo_to_index(uint16_t code, bool is_day)
{
    switch (code) {
    case 0:          return is_day ? WX_SUN : WX_MOON;
    case 1: case 2:  return is_day ? WX_CLOUDSUN : WX_CLOUDMOON;
    case 3:          return WX_CLOUD;
    case 45: case 48: return WX_FOG;
    default: break;
    }

    if (code >= 51 && code <= 57) { return WX_DRIZZLE; }
    if (code >= 61 && code <= 67) { return WX_RAIN; }
    if (code >= 71 && code <= 77) { return WX_SNOW; }
    if (code >= 80 && code <= 82) { return WX_RAIN; }
    if (code == 85 || code == 86) { return WX_SNOW; }
    if (code >= 95 && code <= 99) { return WX_STORM; }

    /* A code this was not taught is still weather, and the temperature beside
       it is still true - so the overcast symbol rather than nothing. */
    return WX_CLOUD;
}

int clk_wx_index(const clock_frame_t *f)
{
    neos_weather_t wx;
    if (!f || !neos_weather(&wx)) {
        return -1;
    }
    return wmo_to_index(wx.code, wx.is_day != 0);
}

bool clk_wx_temp_c10(int16_t *out)
{
    neos_weather_t wx;
    if (!neos_weather(&wx)) {
        return false;
    }
    if (out) {
        *out = wx.temp_c10;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Where it is doing it                                                */
/* ------------------------------------------------------------------ */

bool clk_wx_place(char *out, size_t n)
{
    neos_weather_t wx;

    if (!out || n == 0) {
        return false;
    }
    out[0] = 0;

    /*
     * The map wins over the network. An IP database is right about where a
     * connection comes out, which is not always where the tablet is, and the
     * whole point of being able to point at the country is that the answer
     * sticks - so a chosen city is not a hint that gets overwritten by the
     * next fetch.
     */
    const char *picked = clk_city_chosen();
    if (picked) {
        return clk_pl_from_utf8(picked, out, n) > 0;
    }

    if (!neos_weather(&wx) || wx.place[0] == 0) {
        return false;
    }

    /*
     * Through the same conversion the city table goes through, and for the
     * reason in clock_text.c: this arrives as UTF-8, and a letter that is two
     * bytes of it is two blank cells on a display with one cell per letter.
     */
    return clk_pl_from_utf8(wx.place, out, n) > 0;
}

/* How many blank cells wind past between the end of the text and its own
   start again. Three reads as a pause rather than as part of the word. */
#define MARQUEE_PAD 3

void clk_marquee(char *out, size_t n, const char *src, int cells, uint32_t step)
{
    if (!out || n == 0) {
        return;
    }
    if (cells < 0) {
        cells = 0;
    }
    if ((size_t)cells + 1 > n) {
        cells = (int)n - 1;
    }
    out[0] = 0;
    if (!src) {
        src = "";
    }

    const int len = (int)strlen(src);
    int i = 0;

    if (len <= cells) {
        /* It fits, so it does not move: a real one only scrolls what it has to,
           and a name sliding for no reason is the tell. Centred in the cells,
           which is where a fixed field puts a short string. */
        const int pad = (cells - len) / 2;
        for (; i < pad; i++) {
            out[i] = ' ';
        }
        for (int j = 0; j < len; j++, i++) {
            out[i] = src[j];
        }
        for (; i < cells; i++) {
            out[i] = ' ';
        }
        out[i] = 0;
        return;
    }

    /* Otherwise the window is over a ring of the text plus its blank tail, and
       stepping is one whole cell - there is nowhere on this hardware for half
       a character to be. */
    const int ring = len + MARQUEE_PAD;
    const int at = (int)(step % (uint32_t)ring);
    for (; i < cells; i++) {
        const int k = (at + i) % ring;
        out[i] = k < len ? src[k] : ' ';
    }
    out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* The annunciator row                                                 */
/* ------------------------------------------------------------------ */

void clk_wx_icon_row(ngl_surface_t *s, ngl_rect_t r, int active,
                     ngl_color_t on, ngl_color_t off)
{
    if (!s || r.w <= 0 || r.h <= 0) {
        return;
    }
    const ngl_icon_t *first = WX_SET[0];
    if (!first) {
        return;
    }

    /*
     * Evenly across the whole width, computed from the row rather than from
     * the icons. That is what makes the positions fixed: a symbol sits where
     * it sits whether it is lit or not, and nothing shifts when the weather
     * changes - on the real panel it could not, and a row that reflowed would
     * give the whole thing away.
     */
    (void)first;
    for (int i = 0; i < CLK_WX_SET_N; i++) {
        const ngl_rect_t at = clk_wx_icon_slot(r, i);
        ngl_icon(s, at.x, at.y, WX_SET[i], i == active ? on : off);
    }
}

ngl_rect_t clk_wx_icon_slot(ngl_rect_t r, int i)
{
    const ngl_icon_t *ic = clk_wx_set(i);
    if (!ic) {
        return ngl_rect(r.x, r.y, 0, 0);
    }
    /* The same arithmetic clk_wx_icon_row() lays the row out with, and the
       only copy of it: a face that draws one symbol differently - lighting it
       part way, or throwing a shadow under it - has to put it exactly where
       the faint one it is covering already is. */
    const int16_t slot = (int16_t)(r.w / CLK_WX_SET_N);
    return ngl_rect((int16_t)(r.x + i * slot + (slot - (int16_t)ic->w) / 2),
                    (int16_t)(r.y + (r.h - (int16_t)ic->h) / 2),
                    (int16_t)ic->w, (int16_t)ic->h);
}

/* ------------------------------------------------------------------ */
/* The free-form line                                                  */
/* ------------------------------------------------------------------ */

/**
 * Round tenths of a degree to whole ones, away from zero on the half.
 *
 * Integer, because this app is built -Werror=double-promotion for the reason
 * in its CMakeLists: apps link -nostdlib and a stray double is a link error
 * against a helper the loader does not carry.
 */
static int whole_degrees(int16_t c10)
{
    return c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10);
}

/** The pieces, for whichever of the three renderers below is about to draw. */
static void wx_pieces(const clock_frame_t *f, char *date, size_t dn,
                      char *temp, size_t tn, const ngl_icon_t **ic)
{
    snprintf(date, dn, "%s %d %s %d",
             clk_wday(&f->t), f->t.day, clk_month(&f->t), f->t.year);
    temp[0] = 0;
    *ic = 0;

    const int idx = clk_wx_index(f);
    int16_t c10 = 0;
    if (idx < 0 || !clk_wx_temp_c10(&c10)) {
        return;
    }
    *ic = WX_SET[idx];
    snprintf(temp, tn, "%d" CLK_DEG "C", whole_degrees(c10));
}

/**
 * Split a temperature at the degree marker.
 *
 * The system font stops at 126 and has no such glyph, so the renderers that
 * use one draw a ring beside the number instead - which means they have to
 * know where the number ends. Splitting the formatted string is cheaper than
 * formatting it twice and cannot disagree with itself.
 */
static void split_degree(const char *temp, char *num, size_t n)
{
    size_t i = 0;
    for (; temp[i] && (uint8_t)temp[i] != 0xB0 && i + 1 < n; i++) {
        num[i] = temp[i];
    }
    num[i] = 0;
}

void clk_wx_strip(ngl_surface_t *s, ngl_rect_t r, const clock_frame_t *f,
                  const ngl_font_t *font, ngl_color_t fg, ngl_color_t bg)
{
    if (!s || !f || r.w <= 0 || r.h <= 0) {
        return;
    }
    if (!font) {
        font = &ngl_font_small;
    }

    ngl_fill_rect(s, r, bg);
    if (!f->have_time) {
        return;
    }

    char date[32], temp[16], num[16];
    const ngl_icon_t *ic;
    wx_pieces(f, date, sizeof(date), temp, sizeof(temp), &ic);
    split_degree(temp, num, sizeof(num));

    const int16_t gap    = (int16_t)font->width;
    const int16_t ring_r = (int16_t)(font->height / 7 + 1);
    const int16_t ring_w = (int16_t)(2 * ring_r + 3);

    /* Measure the whole line before drawing any of it, so it is centred as one
       object rather than as three that happen to be adjacent. */
    int16_t w = ngl_text_width(font, date);
    if (ic) {
        w = (int16_t)(w + gap + ic->w + gap / 2
                      + ngl_text_width(font, num) + ring_w
                      + ngl_text_width(font, "C"));
    }

    int16_t x = (int16_t)(r.x + (r.w - w) / 2);
    if (x < r.x) {
        x = r.x;
    }
    const int16_t ty = (int16_t)(r.y + (r.h - font->height) / 2);

    x = ngl_text(s, x, ty, date, font, fg);
    if (!ic) {
        return;
    }

    x = (int16_t)(x + gap);
    ngl_icon(s, x, (int16_t)(r.y + (r.h - ic->h) / 2), ic, fg);
    x = (int16_t)(x + ic->w + gap / 2);

    x = ngl_text(s, x, ty, num, font, fg);
    clk_degree(s, (int16_t)(x + ring_r), (int16_t)(ty + ring_r + 2), ring_r, fg);
    x = (int16_t)(x + ring_w);
    ngl_text(s, x, ty, "C", font, fg);
}

/* ------------------------------------------------------------------ */
/* The same line at one resolution                                     */
/* ------------------------------------------------------------------ */

void clk_wx_strip_blocky(ngl_surface_t *s, ngl_rect_t r, const clock_frame_t *f,
                         const ngl_font_t *font, int16_t scale,
                         ngl_color_t fg, ngl_color_t bg)
{
    if (!s || !f || r.w <= 0 || r.h <= 0) {
        return;
    }
    if (!font) {
        font = &ngl_font_small;
    }
    if (scale < 1) {
        scale = 1;
    }

    ngl_fill_rect(s, r, bg);
    if (!f->have_time) {
        return;
    }

    char date[32], temp[16], num[16];
    const ngl_icon_t *ic;
    wx_pieces(f, date, sizeof(date), temp, sizeof(temp), &ic);
    split_degree(temp, num, sizeof(num));

    /* The icon is sampled on the same grid the glyphs are drawn on, so one
       block is one block everywhere on the panel. */
    const int16_t istep = scale;
    const int16_t gap   = (int16_t)(font->width * scale / 2);
    const int16_t gh    = (int16_t)(font->height * scale);
    const int16_t ring_r = (int16_t)(gh / 7 + 1);
    const int16_t ring_w = (int16_t)(2 * ring_r + 3);

    int16_t w = clk_blocky_w(font, date, scale);
    if (ic) {
        w = (int16_t)(w + gap + ic->w + gap / 2
                      + clk_blocky_w(font, num, scale) + ring_w
                      + clk_blocky_w(font, "C", scale));
    }

    int16_t x = (int16_t)(r.x + (r.w - w) / 2);
    if (x < r.x) {
        x = r.x;
    }
    const int16_t ty = (int16_t)(r.y + (r.h - gh) / 2);

    clk_blocky(s, x, ty, date, font, scale, fg);
    x = (int16_t)(x + clk_blocky_w(font, date, scale));
    if (!ic) {
        return;
    }

    x = (int16_t)(x + gap);
    /* 128 rather than something cleverer: electronic ink has one bit and no
       opinion about coverage, so half is where the particle flips. */
    clk_icon_blocky(s, x, (int16_t)(r.y + (r.h - ic->h) / 2), ic, istep, 128, fg);
    x = (int16_t)(x + ic->w + gap / 2);

    clk_blocky(s, x, ty, num, font, scale, fg);
    x = (int16_t)(x + clk_blocky_w(font, num, scale));
    clk_degree(s, (int16_t)(x + ring_r), (int16_t)(ty + ring_r + 2), ring_r, fg);
    x = (int16_t)(x + ring_w);
    clk_blocky(s, x, ty, "C", font, scale, fg);
}
