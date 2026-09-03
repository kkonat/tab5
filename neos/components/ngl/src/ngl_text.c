/*
 * ngl text rendering: 1-bit bitmap fonts, no anti-aliasing.
 *
 * Glyph bits are packed MSB-first, `bytes_per_row` bytes per scanline, cells
 * stored consecutively from font->first. See tools/genfont.py.
 */
#include "ngl.h"
#include "ngl_internal.h"

static const uint8_t *glyph_bits(const ngl_font_t *f, char ch)
{
    uint8_t c = (uint8_t)ch;
    if (c < f->first || c > f->last) {
        c = '?';
        if (c < f->first || c > f->last) {
            return NULL;
        }
    }
    const size_t cell = (size_t)f->height * f->bytes_per_row;
    return f->bits + (size_t)(c - f->first) * cell;
}

static void draw_glyph(ngl_surface_t *s, int16_t x, int16_t y, const uint8_t *g,
                       const ngl_font_t *f, ngl_color_t c, bool use_bg, ngl_color_t bg)
{
    for (int16_t row = 0; row < f->height; row++) {
        const uint8_t *line = g + (size_t)row * f->bytes_per_row;
        const int16_t py = (int16_t)(y + row);
        for (int16_t col = 0; col < f->width; col++) {
            const bool on = (line[col >> 3] >> (7 - (col & 7))) & 1;
            if (on) {
                ngl_pixel(s, (int16_t)(x + col), py, c);
            } else if (use_bg) {
                ngl_pixel(s, (int16_t)(x + col), py, bg);
            }
        }
    }
}

static int16_t text_common(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                           const ngl_font_t *f, ngl_color_t c, bool use_bg, ngl_color_t bg)
{
    if (!str || !f) {
        return x;
    }
    const int16_t x0 = x;
    for (const char *p = str; *p; p++) {
        const uint8_t *g = glyph_bits(f, *p);
        if (g) {
            draw_glyph(s, x, y, g, f, c, use_bg, bg);
        }
        x = (int16_t)(x + f->width);
    }

    if (s == ngl_screen()) {
        ngl_rect_t r = ngl_rect(x0, y, (int16_t)(x - x0), f->height);
        ngl_dirty(&r);
    }
    return x;
}

int16_t ngl_text(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                const ngl_font_t *f, ngl_color_t c)
{
    return text_common(s, x, y, str, f, c, false, 0);
}

int16_t ngl_text_bg(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                   const ngl_font_t *f, ngl_color_t c, ngl_color_t bg)
{
    return text_common(s, x, y, str, f, c, true, bg);
}

int16_t ngl_text_width(const ngl_font_t *f, const char *str)
{
    if (!f || !str) {
        return 0;
    }
    int16_t n = 0;
    while (str[n]) {
        n++;
    }
    return (int16_t)(n * f->width);
}
