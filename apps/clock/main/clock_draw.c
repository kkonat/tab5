/*
 * Making a digit big.
 *
 * ngl's largest font is 24x48. A clock on a 720x1280 panel wants digits around
 * 200 pixels tall, and shipping a font at that size would be a 40 KB glyph
 * table per weight for the ten characters that actually change. So none of the
 * three renderers here is a font: each takes a shape that already exists - a
 * bitmap glyph, seven segments, a grid of dots - and computes it at whatever
 * size it is asked for.
 *
 * They are all built out of ngl_fill_rect() rather than per-pixel writes. The
 * cost of a frame on this machine is set by the pixels that get touched and by
 * how many panel rows the flush has to write back, and a filled rectangle is
 * the one shape ngl walks in scanline order.
 */
#include <string.h>

#include "ngl.h"

#include "clock.h"

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

ngl_color_t clk_mix(ngl_color_t a, ngl_color_t b, uint8_t t)
{
    const int u = 255 - t;
    const int r = (((a >> 11) & 0x1F) * u + ((b >> 11) & 0x1F) * t) / 255;
    const int g = (((a >>  5) & 0x3F) * u + ((b >>  5) & 0x3F) * t) / 255;
    const int c = (( a        & 0x1F) * u + ( b        & 0x1F) * t) / 255;
    return (ngl_color_t)((r << 11) | (g << 5) | c);
}

/* ------------------------------------------------------------------ */
/* Blocky: a bitmap font, magnified                                    */
/* ------------------------------------------------------------------ */

int16_t clk_blocky_w(const ngl_font_t *f, const char *str, int16_t scale)
{
    if (!f || !str) {
        return 0;
    }
    return (int16_t)((int)strlen(str) * f->width * scale);
}

void clk_blocky(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                const ngl_font_t *f, int16_t scale, ngl_color_t c)
{
    if (!s || !f || !str || scale < 1) {
        return;
    }

    const size_t cell = (size_t)f->height * f->bytes_per_row;

    for (const char *p = str; *p; p++, x = (int16_t)(x + f->width * scale)) {
        const uint8_t ch = (uint8_t)*p;
        if (ch < f->first || ch > f->last) {
            continue;                       /* space, and anything absent */
        }
        const uint8_t *g = f->bits + (size_t)(ch - f->first) * cell;

        for (int16_t row = 0; row < f->height; row++) {
            const uint8_t *line = g + (size_t)row * f->bytes_per_row;

            /*
             * One fill per run of set bits, not per bit. A digit's strokes are
             * horizontal runs several pixels long, so this is the difference
             * between a few dozen rectangles per glyph and a few thousand.
             */
            int16_t col = 0;
            while (col < f->width) {
                if (!((line[col >> 3] >> (7 - (col & 7))) & 1)) {
                    col++;
                    continue;
                }
                const int16_t run0 = col;
                while (col < f->width &&
                       ((line[col >> 3] >> (7 - (col & 7))) & 1)) {
                    col++;
                }
                ngl_fill_rect(s, ngl_rect((int16_t)(x + run0 * scale),
                                          (int16_t)(y + row * scale),
                                          (int16_t)((col - run0) * scale),
                                          scale), c);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Seven segments                                                      */
/* ------------------------------------------------------------------ */

/*
 *      aaaa
 *     f    b
 *     f    b
 *      gggg
 *     e    c
 *     e    c
 *      dddd
 *
 * Bit per segment, a in bit 0. The table is the one every seven-segment part
 * has had since the 1970s and is not worth deriving.
 */
static const uint8_t SEG[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

uint16_t clk_seg7_mask_of(int d)
{
    return (d >= 0 && d <= 9) ? SEG[d] : 0;
}

void clk_seg7_fit(clk_seg7_t *g, int16_t w, int16_t h, int16_t slant)
{
    g->w     = w;
    g->h     = h;
    /* An eighth of the width is about where a real display sits: thick enough
       to read at a distance, thin enough that an 8 is not a filled box. */
    g->thick = (int16_t)(w / 8);
    if (g->thick < 3) {
        g->thick = 3;
    }
    g->gap   = (int16_t)(g->thick / 3 + 1);
    g->slant = slant;
    g->bleed = 0;
}

/**
 * How far right a row is pushed by the slant.
 *
 * @p dy is measured from the top of the digit, not of the segment, so every
 * part of a digit leans by the same amount at the same height - which is what
 * keeps a slanted 8 a parallelogram instead of a zigzag.
 *
 * Applied per scanline rather than by shearing a finished bitmap, which is
 * what keeps the mitred ends mitred instead of stepped.
 */
static inline int16_t lean(const clk_seg7_t *g, int16_t dy)
{
    return g->slant ? (int16_t)((int32_t)g->slant * (g->h - dy) / g->h) : 0;
}

/**
 * A horizontal segment: a bar whose ends come to a point.
 *
 * Row i of the bar is inset from both ends by its distance from the bar's
 * centre line, so the ends are 45-degree mitres and two segments meeting at a
 * corner leave the notch a real display has.
 *
 * @p ytop is the digit's top, which only the slant needs.
 */
static void seg_h(ngl_surface_t *s, const clk_seg7_t *g, int16_t ytop,
                  int16_t x0, int16_t y0, int16_t len, ngl_color_t c)
{
    const int16_t t = g->thick;
    const int16_t half = (int16_t)(t / 2);
    const int16_t b = g->bleed;

    /*
     * The bleed extends the run of rows past both ends of the bar and widens
     * each one, using the nearest real row's inset for the rows outside it -
     * so the mitre is carried outwards rather than squared off, and a halo is
     * the same shape as the thing casting it.
     */
    for (int16_t i = (int16_t)(-b); i < (int16_t)(t + b); i++) {
        const int16_t ii = (int16_t)(i < 0 ? 0 : (i >= t ? t - 1 : i));
        const int16_t inset = (int16_t)(ii <= half ? half - ii : ii - half);
        const int16_t w = (int16_t)(len - 2 * inset + 2 * b);
        if (w <= 0) {
            continue;
        }
        const int16_t y = (int16_t)(y0 + i);
        ngl_fill_rect(s, ngl_rect((int16_t)(x0 + inset - b + lean(g, (int16_t)(y - ytop))),
                                  y, w, 1), c);
    }
}

/** The same, on its side: @p len tall and `thick` wide, tapered at both ends. */
static void seg_v(ngl_surface_t *s, const clk_seg7_t *g, int16_t ytop,
                  int16_t x0, int16_t y0, int16_t len, ngl_color_t c)
{
    const int16_t t = g->thick;
    const int16_t half = (int16_t)(t / 2);
    const int16_t b = g->bleed;

    for (int16_t i = (int16_t)(-b); i < (int16_t)(len + b); i++) {
        const int16_t ii = (int16_t)(i < 0 ? 0 : (i >= len ? len - 1 : i));
        int16_t inset = 0;
        if (ii < half) {
            inset = (int16_t)(half - ii);
        } else if (ii >= len - half) {
            inset = (int16_t)(ii - (len - half) + 1);
        }
        const int16_t w = (int16_t)(t - 2 * inset + 2 * b);
        if (w <= 0) {
            continue;
        }
        const int16_t y = (int16_t)(y0 + i);
        ngl_fill_rect(s, ngl_rect((int16_t)(x0 + inset - b + lean(g, (int16_t)(y - ytop))),
                                  y, w, 1), c);
    }
}

/**
 * Paint the segments named in @p mask.
 *
 * Everything is measured off `thick` and `gap`, so a digit scales without any
 * of it being retuned. The height works out exactly:
 *
 *     3 * thick  the three horizontals
 *   + 4 * gap    the notch above and below each vertical
 *   + 2 * vlen   the two rows of verticals
 *   = h
 *
 * which is what `vlen` below is solved for.
 */
void clk_seg7_paint(ngl_surface_t *s, int16_t x, int16_t y, uint16_t mask,
                    const clk_seg7_t *g, ngl_color_t c)
{
    const int16_t t    = g->thick;
    const int16_t gap  = g->gap;
    const int16_t hlen = (int16_t)(g->w - 2 * gap);
    const int16_t vlen = (int16_t)((g->h - 3 * t - 4 * gap) / 2);
    const int16_t midy = (int16_t)(y + (g->h - t) / 2);
    const int16_t vx_l = x;
    const int16_t vx_r = (int16_t)(x + g->w - t);

    if (hlen <= 0 || vlen <= 0) {
        return;
    }

    /* a, g, d - top, middle, bottom */
    if (mask & 0x01) { seg_h(s, g, y, (int16_t)(x + gap), y, hlen, c); }
    if (mask & 0x40) { seg_h(s, g, y, (int16_t)(x + gap), midy, hlen, c); }
    if (mask & 0x08) { seg_h(s, g, y, (int16_t)(x + gap), (int16_t)(y + g->h - t), hlen, c); }

    /* f, b - upper left and right */
    if (mask & 0x20) { seg_v(s, g, y, vx_l, (int16_t)(y + t + gap), vlen, c); }
    if (mask & 0x02) { seg_v(s, g, y, vx_r, (int16_t)(y + t + gap), vlen, c); }

    /* e, c - lower left and right */
    if (mask & 0x10) { seg_v(s, g, y, vx_l, (int16_t)(midy + t + gap), vlen, c); }
    if (mask & 0x04) { seg_v(s, g, y, vx_r, (int16_t)(midy + t + gap), vlen, c); }
}

void clk_seg7_digit(ngl_surface_t *s, int16_t x, int16_t y, int d,
                    const clk_seg7_t *g, ngl_color_t on, ngl_color_t off)
{
    const uint8_t lit = (d >= 0 && d <= 9) ? SEG[d] : 0;

    /* The dead segments first and the live ones over them, so that where the
       two overlap at a mitre the lit one wins - which is the way round a real
       display looks. */
    if (off != on) {
        clk_seg7_paint(s, x, y, (uint16_t)(CLK_SEG7_ALL & ~lit), g, off);
    }
    clk_seg7_paint(s, x, y, lit, g, on);
}

void clk_seg7_lit(ngl_surface_t *s, int16_t x, int16_t y, int d,
                  const clk_seg7_t *g, ngl_color_t c)
{
    /* A blank digit lights nothing, the same as it does in clk_seg7_digit().
       The two disagreeing about what a negative d means would show up as a
       clock with no time drawing a row of solid eights. */
    clk_seg7_paint(s, x, y, clk_seg7_mask_of(d), g, c);
}

int16_t clk_seg7_colon_w(const clk_seg7_t *g)
{
    return (int16_t)(g->thick * 2);
}

void clk_seg7_colon(ngl_surface_t *s, int16_t x, int16_t y,
                    const clk_seg7_t *g, ngl_color_t c)
{
    const int16_t t = g->thick;
    /* Level with the two horizontal joins, which is where the colon of a real
       display sits - a quarter of the way down and a quarter up. */
    const int16_t y1 = (int16_t)(y + g->h / 4 - t / 2);
    const int16_t y2 = (int16_t)(y + (3 * g->h) / 4 - t / 2);
    /* Centred in the cell clk_seg7_colon_w() reports, so a caller that
       advances by that width puts the dots midway between the digits. */
    const int16_t cx = (int16_t)(x + (clk_seg7_colon_w(g) - t) / 2);

    ngl_fill_rect(s, ngl_rect((int16_t)(cx + lean(g, (int16_t)(y1 - y))), y1, t, t), c);
    ngl_fill_rect(s, ngl_rect((int16_t)(cx + lean(g, (int16_t)(y2 - y))), y2, t, t), c);
}

/* ------------------------------------------------------------------ */
/* Fourteen segments                                                   */
/* ------------------------------------------------------------------ */

/*
 *       -- a --
 *      |\  |  /|
 *      f h i j b
 *      |  \|/  |
 *       g1-- g2
 *      |  /|\  |
 *      e k l m c
 *      |/  |  \|
 *       -- d --
 *
 * The seven above plus a split middle and a star of six through the centre,
 * which is the least a fixed display needs to spell a word. Bit order is the
 * one every published table uses - a in bit 0, then b c d e f, the two halves
 * of the middle, and the star from the upper left round to the lower right -
 * so the glyph table below can be read against a datasheet.
 *
 * The geometry is the same struct the seven-segment cell uses, because a
 * fourteen is that cell with more electrodes on it. Sharing the type is what
 * lets a row mix the two and have them sit on the same baseline with the same
 * lean.
 */
enum {
    S14_A = 0x0001, S14_B = 0x0002, S14_C  = 0x0004, S14_D = 0x0008,
    S14_E = 0x0010, S14_F = 0x0020, S14_G1 = 0x0040, S14_G2 = 0x0080,
    S14_H = 0x0100, S14_I = 0x0200, S14_J  = 0x0400, S14_K = 0x0800,
    S14_L = 0x1000, S14_M = 0x2000,
};

/*
 * Space through underscore, which is the whole of what a display like this
 * ever had: digits, capitals and the handful of marks that fit. Lowercase
 * folds to uppercase in the lookup rather than doubling the table - a real
 * fourteen-segment part had no lowercase, and inventing one would be the one
 * thing on these faces that could not have been built.
 */
#define S14_FIRST 0x20
#define S14_LAST  0x5F

static const uint16_t SEG14[S14_LAST - S14_FIRST + 1] = {
    0x0000, /*   */  0x0006, /* ! */  0x0220, /* " */  0x12CE, /* # */
    0x12ED, /* $ */  0x0C24, /* % */  0x235D, /* & */  0x0400, /* ' */
    0x2400, /* ( */  0x0900, /* ) */  0x3FC0, /* * */  0x12C0, /* + */
    0x0800, /* , */  0x00C0, /* - */  0x0000, /* . */  0x0C00, /* / */
    0x0C3F, /* 0 */  0x0006, /* 1 */  0x00DB, /* 2 */  0x008F, /* 3 */
    0x00E6, /* 4 */  0x00ED, /* 5 */  0x00FD, /* 6 */  0x0007, /* 7 */
    0x00FF, /* 8 */  0x00EF, /* 9 */  0x1200, /* : */  0x0A00, /* ; */
    0x2400, /* < */  0x00C8, /* = */  0x0900, /* > */  0x1083, /* ? */
    0x02BB, /* @ */  0x00F7, /* A */  0x128F, /* B */  0x0039, /* C */
    0x120F, /* D */  0x00F9, /* E */  0x0071, /* F */  0x00BD, /* G */
    0x00F6, /* H */  0x1200, /* I */  0x001E, /* J */  0x2470, /* K */
    0x0038, /* L */  0x0536, /* M */  0x2136, /* N */  0x003F, /* O */
    0x00F3, /* P */  0x203F, /* Q */  0x20F3, /* R */  0x00ED, /* S */
    0x1201, /* T */  0x003E, /* U */  0x0C30, /* V */  0x2836, /* W */
    0x2D00, /* X */  0x1500, /* Y */  0x0C09, /* Z */  0x0039, /* [ */
    0x2100, /* backslash */
                     0x000F, /* ] */  0x0C03, /* ^ */  0x0008, /* _ */
};

/* The top box lit and nothing else, which is the only degree sign a cell of
   fourteen electrodes can make. Reached as 0xB0 - the byte CLK_DEG puts in a
   string - the same way the dot matrix reaches its own. */
#define S14_DEGREE (S14_A | S14_B | S14_F | S14_G1 | S14_G2)

uint16_t clk_seg14_mask_of(char ch)
{
    uint8_t c = (uint8_t)ch;

    if (c == 0xB0) {
        return S14_DEGREE;
    }
    /* Fourteen electrodes and not one of them is an accent - there is nowhere
       in the cell to put a mark and no row outside it to use, so a marked
       letter is its base letter. Which is what every segment display in the
       country does, and why the ones in railway stations get shouted at. */
    c = (uint8_t)clk_pl_base((char)c);
    if (c >= 'a' && c <= 'z') {
        c = (uint8_t)(c - 'a' + 'A');
    }
    if (c < S14_FIRST || c > S14_LAST) {
        return 0;                    /* an unlit cell, not a box */
    }
    return SEG14[c - S14_FIRST];
}

void clk_seg14_fit(clk_seg14_t *g, int16_t w, int16_t h, int16_t slant)
{
    g->w     = w;
    g->h     = h;
    /* Thinner than a seven: twice as many electrodes cross the same cell, and
       at w/8 the star in the middle closes up into a blob. */
    g->thick = (int16_t)(w / 9);
    if (g->thick < 3) {
        g->thick = 3;
    }
    /* And a wider notch, for the same reason - six segments meet at the centre
       and the eye has to be able to see where one of them ends. */
    g->gap   = (int16_t)(g->thick / 2 + 1);
    g->slant = slant;
    g->bleed = 0;
}

/**
 * One of the four diagonals: a bar of the cell's thickness, at an angle.
 *
 * One horizontal run per scanline, like the upright and the flat, so it costs
 * the same kind of work and picks up the same lean. The run is wider than
 * `thick` by the secant of the angle - a sloped bar crossed horizontally is
 * longer than it is thick - which is what keeps a diagonal the same visual
 * weight as the segments it meets.
 */
static void seg_d(ngl_surface_t *s, const clk_seg7_t *g, int16_t ytop,
                  int16_t x0, int16_t y0, int16_t x1, int16_t y1, ngl_color_t c)
{
    if (y1 < y0) {
        const int16_t tx = x0, ty = y0;
        x0 = x1; y0 = y1; x1 = tx; y1 = ty;
    }
    const int16_t dy = (int16_t)(y1 - y0);
    if (dy <= 0) {
        return;
    }
    const int32_t dx  = (int32_t)x1 - x0;
    const int32_t adx = dx < 0 ? -dx : dx;

    /* hypot to about two percent, and without a float: this app is built
       -Werror=double-promotion for the reason in its CMakeLists. */
    const int32_t len = adx > dy ? adx + 3 * dy / 8 : dy + 3 * adx / 8;
    const int16_t b   = g->bleed;
    int16_t run = (int16_t)((int32_t)g->thick * len / dy + 2 * b);
    if (run < 1) {
        run = 1;
    }

    for (int16_t i = (int16_t)(-b); i < (int16_t)(dy + b); i++) {
        const int16_t ii = (int16_t)(i < 0 ? 0 : (i > dy ? dy : i));
        const int16_t y  = (int16_t)(y0 + i);
        const int16_t cx = (int16_t)(x0 + dx * ii / dy);
        ngl_fill_rect(s, ngl_rect((int16_t)(cx - run / 2 + lean(g, (int16_t)(y - ytop))),
                                  y, run, 1), c);
    }
}

void clk_seg14_paint(ngl_surface_t *s, int16_t x, int16_t y, uint16_t mask,
                     const clk_seg14_t *g, ngl_color_t c)
{
    const int16_t t     = g->thick;
    const int16_t gp    = g->gap;
    const int16_t hlen  = (int16_t)(g->w - 2 * gp);
    const int16_t vlen  = (int16_t)((g->h - 3 * t - 4 * gp) / 2);
    const int16_t midy  = (int16_t)(y + (g->h - t) / 2);
    const int16_t cx    = (int16_t)(x + (g->w - t) / 2);   /* the centre upright */
    const int16_t hhalf = (int16_t)((g->w - t) / 2 - 2 * gp);
    const int16_t vxr   = (int16_t)(x + g->w - t);

    if (hlen <= 0 || vlen <= 0 || !mask) {
        return;
    }

    /* a, d - the full-width top and bottom */
    if (mask & S14_A) { seg_h(s, g, y, (int16_t)(x + gp), y, hlen, c); }
    if (mask & S14_D) { seg_h(s, g, y, (int16_t)(x + gp), (int16_t)(y + g->h - t), hlen, c); }

    /* g1, g2 - the middle in halves, which is what makes a G and not an 8 */
    if (hhalf > 0) {
        if (mask & S14_G1) { seg_h(s, g, y, (int16_t)(x + gp), midy, hhalf, c); }
        if (mask & S14_G2) { seg_h(s, g, y, (int16_t)(cx + t + gp), midy, hhalf, c); }
    }

    /* the six uprights: the outer pairs, and the two down the centre */
    if (mask & S14_F) { seg_v(s, g, y, x,   (int16_t)(y + t + gp),    vlen, c); }
    if (mask & S14_B) { seg_v(s, g, y, vxr, (int16_t)(y + t + gp),    vlen, c); }
    if (mask & S14_E) { seg_v(s, g, y, x,   (int16_t)(midy + t + gp), vlen, c); }
    if (mask & S14_C) { seg_v(s, g, y, vxr, (int16_t)(midy + t + gp), vlen, c); }
    if (mask & S14_I) { seg_v(s, g, y, cx,  (int16_t)(y + t + gp),    vlen, c); }
    if (mask & S14_L) { seg_v(s, g, y, cx,  (int16_t)(midy + t + gp), vlen, c); }

    /*
     * And the four corners in to the centre.
     *
     * These are given as the path the middle of the bar takes, not as the box
     * it fits in, which is what lets them start where the two segments at that
     * corner cross rather than clear of both. A diagonal drawn between the
     * inside edges instead spans a third of the cell on a shape this tall and
     * comes out looking like a short upright - and an M, an N and a W are then
     * the same character.
     *
     * The two halves stop just short of each other at the centre so the notch
     * is there when only one of them is driven, which is what an N needs.
     */
    const int16_t lx  = (int16_t)(x + t / 2 + gp);
    const int16_t rx  = (int16_t)(x + g->w - t / 2 - gp);
    const int16_t ty  = (int16_t)(y + t / 2 + gp);
    const int16_t by  = (int16_t)(y + g->h - t / 2 - gp);
    const int16_t mu  = (int16_t)(midy - gp);
    const int16_t md  = (int16_t)(midy + t + gp);
    const int16_t cxl = (int16_t)(cx + t / 2 - gp / 2);
    const int16_t cxr = (int16_t)(cx + t / 2 + gp / 2);

    if (mask & S14_H) { seg_d(s, g, y, lx, ty, cxl, mu, c); }
    if (mask & S14_J) { seg_d(s, g, y, cxr, mu, rx, ty, c); }
    if (mask & S14_K) { seg_d(s, g, y, lx, by, cxl, md, c); }
    if (mask & S14_M) { seg_d(s, g, y, cxr, md, rx, by, c); }
}

void clk_seg14_char(ngl_surface_t *s, int16_t x, int16_t y, char ch,
                    const clk_seg14_t *g, ngl_color_t on, ngl_color_t off)
{
    const uint16_t lit = clk_seg14_mask_of(ch);

    /* The dead electrodes first and the live ones over them, so a mitre where
       the two meet is won by the lit one - which is the way round a real panel
       looks. */
    if (off != on) {
        clk_seg14_paint(s, x, y, (uint16_t)(CLK_SEG14_ALL & ~lit), g, off);
    }
    clk_seg14_paint(s, x, y, lit, g, on);
}

/* ------------------------------------------------------------------ */
/* Dot matrix                                                          */
/* ------------------------------------------------------------------ */

/* Defined in clock_dots.c: five columns per glyph, bit 0 at the top. */
extern const uint8_t clk_dot5x7[];
const uint8_t *clk_dot5x7_glyph(char ch);

#define DOT_COLS 5
#define DOT_ROWS 7

int16_t clk_dots_w(const char *str, int16_t pitch)
{
    if (!str) {
        return 0;
    }
    const int n = (int)strlen(str);
    /* One blank column of pitch between characters, none after the last. */
    return (int16_t)(n * (DOT_COLS + 1) * pitch - pitch);
}

int16_t clk_dots_h(int16_t pitch)
{
    return (int16_t)(DOT_ROWS * pitch);
}

/*
 * Square, not round.
 *
 * A dot matrix anode is a square patch of phosphor screen-printed on a plate,
 * and the gap between two of them is the unprinted plate between the patches -
 * so what the grid reads as is squares with a hair of dark between them. Round
 * dots are an LED sign, a different piece of hardware with a lens on every
 * pixel, and drawing them here cost the glyphs their corners: a 5x7 'M' or 'W'
 * is diagonals one dot wide, and a circle throws away exactly the corners that
 * make one diagonal join the next.
 */
static void one_dot(ngl_surface_t *s, int16_t cx, int16_t cy, int16_t d, ngl_color_t c)
{
    ngl_fill_rect(s, ngl_rect(cx, cy, d, d), c);
}

void clk_dots(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
              int16_t pitch, int16_t dot, ngl_color_t on, ngl_color_t off)
{
    if (!s || !str || pitch < 1) {
        return;
    }
    if (dot < 1) {
        dot = 1;
    }
    /* Centre the dot in its cell, so the grid stays square when dot < pitch. */
    const int16_t inset = (int16_t)((pitch - dot) / 2);

    for (const char *p = str; *p; p++, x = (int16_t)(x + (DOT_COLS + 1) * pitch)) {
        const uint8_t *g = clk_dot5x7_glyph(*p);

        for (int16_t col = 0; col < DOT_COLS; col++) {
            const uint8_t bits = g ? g[col] : 0;
            for (int16_t row = 0; row < DOT_ROWS; row++) {
                const bool lit = (bits >> row) & 1;
                if (!lit && off == on) {
                    continue;
                }
                one_dot(s, (int16_t)(x + col * pitch + inset),
                        (int16_t)(y + row * pitch + inset), dot,
                        lit ? on : off);
            }
        }

        /*
         * The mark, in the blank row the pitch leaves above or below the cell.
         * Only the dots that are on: the row is between two lines of cells and
         * is not part of either, so lighting the whole of it the way the cell
         * itself is lit would draw a grid where the module has none.
         */
        const uint8_t acc = clk_dot5x7_accent(*p);
        if (acc & (CLK_ACC_ABOVE | CLK_ACC_BELOW)) {
            const int16_t ry = (acc & CLK_ACC_ABOVE)
                               ? (int16_t)(y - pitch) : (int16_t)(y + DOT_ROWS * pitch);
            for (int16_t col = 0; col < DOT_COLS; col++) {
                if ((acc >> col) & 1) {
                    one_dot(s, (int16_t)(x + col * pitch + inset),
                            (int16_t)(ry + inset), dot, on);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Odds and ends                                                       */
/* ------------------------------------------------------------------ */

void clk_degree(ngl_surface_t *s, int16_t x, int16_t y, int16_t r, ngl_color_t c)
{
    if (r < 2) {
        r = 2;
    }
    /* A ring, drawn as a filled disc with a hole rather than by stepping a
       circle: two round rects, and the second one has to match whatever is
       behind it, so the caller gets a solid mark and draws it on flat ground. */
    ngl_draw_round_rect(s, ngl_rect((int16_t)(x - r), (int16_t)(y - r),
                                    (int16_t)(2 * r), (int16_t)(2 * r)),
                        r, c, (int16_t)(r > 4 ? 2 : 1));
}

const char *clk_wday(const neos_rtc_t *t)
{
    static const char *const W[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    static const int8_t T[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

    /*
     * Sakamoto, rather than the RTC's own wday field.
     *
     * The chip stores the weekday as a seventh number that nothing checks
     * against the other six, so a cell that has been set by hand - or by an
     * app that guessed - can say Tuesday about a Thursday. The date is the
     * thing that is right, so the weekday is derived from it.
     */
    int y = t->year, m = t->month;
    if (m < 1 || m > 12) {
        return "---";
    }
    if (m < 3) {
        y -= 1;
    }
    const int wd = (y + y / 4 - y / 100 + y / 400 + T[m - 1] + t->day) % 7;
    return W[wd < 0 ? 0 : wd];
}

const char *clk_month(const neos_rtc_t *t)
{
    static const char *const M[13] = {
        "---", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
    };
    return M[(t->month >= 1 && t->month <= 12) ? t->month : 0];
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

void clk_stack(ngl_rect_t area, int16_t h, int16_t strip_h,
               int16_t gap, ngl_rect_t *out_time, ngl_rect_t *out_strip)
{
    if (strip_h <= 0) {
        gap = 0;
        strip_h = 0;
    }
    const int16_t total = (int16_t)(h + gap + strip_h);
    const int16_t top = (int16_t)(area.y + (area.h - total) / 2);

    if (out_time) {
        /* Full width, not `w`: the caller centres its own content inside, and
           a face that repaints the block by filling it needs the whole strip
           of screen the digits could ever occupy, not just this minute's. */
        *out_time = ngl_rect(area.x, top, area.w, h);
    }
    if (out_strip) {
        *out_strip = ngl_rect(area.x, (int16_t)(top + h + gap), area.w, strip_h);
    }
}

/* ------------------------------------------------------------------ */
/* Gravity                                                             */
/* ------------------------------------------------------------------ */

bool clk_gravity(const clock_frame_t *f, int16_t *gx, int16_t *gy)
{
    int16_t ax = 0, ay = 0, az = 0;
    if (!f || !neos_imu_accel_mg(&ax, &ay, &az)) {
        return false;
    }

    /*
     * Sensor frame to screen frame.
     *
     * The BMI270's in-plane axes line up with the panel's own: NeOS's
     * orientation service calls +y "down" in NGL_ROT_0, where logical and
     * physical coordinates are the same thing, so sensor +y is panel +y and
     * sensor +x is panel +x.
     *
     * Rotating that into the frame the app draws in is then the direction half
     * of ngl_from_panel() - the same transform a touch goes through, with the
     * translations dropped because this is a vector and not a point. It agrees
     * with the service's own table at every rotation: down is +y at 0, -x at
     * 90, -y at 180 and +x at 270.
     */
    int16_t lx, ly;
    switch (f->rot) {
    case NGL_ROT_90:  lx = ay;             ly = (int16_t)(-ax); break;
    case NGL_ROT_180: lx = (int16_t)(-ax); ly = (int16_t)(-ay); break;
    case NGL_ROT_270: lx = (int16_t)(-ay); ly = ax;             break;
    default:          lx = ax;             ly = ay;             break;
    }

    if (gx) { *gx = lx; }
    if (gy) { *gy = ly; }
    return true;
}

/* ------------------------------------------------------------------ */
/* Icons, at the resolution the hardware actually has                  */
/* ------------------------------------------------------------------ */

void clk_icon_blocky(ngl_surface_t *s, int16_t x, int16_t y, const ngl_icon_t *ic,
                     int16_t step, uint8_t threshold, ngl_color_t c)
{
    if (!s || !ic || step < 1) {
        return;
    }

    for (int16_t sy = 0; sy + step <= (int16_t)ic->h; sy = (int16_t)(sy + step)) {
        int16_t run0 = -1;

        for (int16_t sx = 0; sx <= (int16_t)ic->w; sx = (int16_t)(sx + step)) {
            /*
             * The block's mean coverage, not its corner: sampling one pixel of
             * a thin stroke either keeps all of it or loses all of it, and an
             * icon full of thin strokes then breaks up differently every time
             * it is drawn at a different size.
             */
            bool on = false;
            if (sx + step <= (int16_t)ic->w) {
                uint32_t sum = 0;
                for (int16_t yy = sy; yy < sy + step; yy++) {
                    for (int16_t xx = sx; xx < sx + step; xx++) {
                        sum += ic->alpha[(size_t)yy * ic->w + xx];
                    }
                }
                on = (sum / ((uint32_t)step * step)) >= threshold;
            }

            /* Coalesce, as clk_blocky() does - a row of an icon is a few runs,
               not a few dozen independent squares. */
            if (on && run0 < 0) {
                run0 = sx;
            } else if (!on && run0 >= 0) {
                ngl_fill_rect(s, ngl_rect((int16_t)(x + run0), (int16_t)(y + sy),
                                          (int16_t)(sx - run0), step), c);
                run0 = -1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* A gradient RGB565 can actually hold                                 */
/* ------------------------------------------------------------------ */

void clk_gradient_dither(ngl_surface_t *s, ngl_color_t top, ngl_color_t bot)
{
    /*
     * Ordered dithering, 4x4 Bayer.
     *
     * The problem this solves is not subtlety, it is arithmetic: between the
     * two ends of a gentle grey-green ramp there are about four distinct
     * 5-bit reds and seven 6-bit greens, so however many rows the gradient is
     * painted in, it can only ever be that many flat bands - and on a panel
     * this size each one is a couple of hundred pixels tall and perfectly
     * visible as a stripe.
     *
     * So each channel is computed at 8 bits, and the fraction that will not
     * fit in the target's bit depth is turned into a per-pixel decision to
     * round up or down against the threshold matrix. The banding becomes
     * noise at the pixel level, which the eye integrates back into the ramp
     * that was asked for.
     */
    static const uint8_t BAYER[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 },
    };

    if (!s) {
        return;
    }
    const int16_t w = ngl_surface_w(s);
    const int16_t h = ngl_surface_h(s);
    if (w <= 0 || h <= 0) {
        return;
    }

    /* Both ends back out to 8 bits per channel, so the interpolation happens
       at a precision the destination does not have. */
    const int r0 = ((top >> 11) & 0x1F) * 255 / 31, r1 = ((bot >> 11) & 0x1F) * 255 / 31;
    const int g0 = ((top >>  5) & 0x3F) * 255 / 63, g1 = ((bot >>  5) & 0x3F) * 255 / 63;
    const int b0 = ( top        & 0x1F) * 255 / 31, b1 = ( bot        & 0x1F) * 255 / 31;

    for (int16_t y = 0; y < h; y++) {
        const int r = r0 + (r1 - r0) * y / (h > 1 ? h - 1 : 1);
        const int g = g0 + (g1 - g0) * y / (h > 1 ? h - 1 : 1);
        const int b = b0 + (b1 - b0) * y / (h > 1 ? h - 1 : 1);

        for (int16_t x = 0; x < w; x++) {
            /* 0-15 scaled to the size of one step of each channel: 8 levels
               for the 5-bit ones, 4 for green. */
            const int t = BAYER[y & 3][x & 3];
            const int rr = (r + t / 2) >> 3;
            const int gg = (g + t / 4) >> 2;
            const int bb = (b + t / 2) >> 3;

            ngl_pixel(s, x, y, (ngl_color_t)(((rr > 31 ? 31 : rr) << 11) |
                                             ((gg > 63 ? 63 : gg) <<  5) |
                                              (bb > 31 ? 31 : bb)));
        }
    }
}
