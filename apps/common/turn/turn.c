/*
 * The turned canvas. See turn.h for why it is turned.
 */
#include <math.h>
#include <stdlib.h>

#include "turn.h"

#include "neos_sys.h"

/*
 * The scratch a run of glyphs is composed in before it is blitted.
 *
 * 48 is the tallest font on this machine laid on its side, and 640 holds forty
 * cells of the small one or twenty-six of the large - longer runs are chunked.
 * It is one allocation for the life of the app because a text scratch that is
 * malloc'd per string is a fragmentation source in an app that draws numbers
 * sixty times a second.
 */
#define SCRATCH_W  48
#define SCRATCH_H 640

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool turn_init(turn_t *g, ngl_rotation_t rot)
{
    int16_t pw = 0, ph = 0;
    ngl_panel_size(&pw, &ph);
    if (pw <= 0 || ph <= 0) {
        return false;
    }

    g->pw   = pw;
    g->ph   = ph;
    g->w    = ph;        /* landscape is the panel on its side */
    g->h    = pw;
    g->rot  = rot;
    g->fast = false;

    /*
     * PSRAM, and deliberately: this is 1.8 MB and the internal pool is 768 KB
     * with NeOS living in it. lab/defender can ask for internal RAM because
     * its picture is 240x180; a full-panel canvas that took the last of the
     * fast pool would not buy a faster frame, it would buy a tablet that
     * cannot open a socket. ngl_surface_new() aligns the base and rounds the
     * row to a cache line, which is what makes it eligible for the PPA at all.
     */
    g->s = ngl_surface_new(pw, ph);
    if (!g->s) {
        return false;
    }

    g->scratch = (ngl_color_t *)malloc((size_t)SCRATCH_W * SCRATCH_H *
                                       sizeof(ngl_color_t));
    if (g->scratch) {
        g->scratch_s = ngl_surface_wrap(g->scratch, SCRATCH_W, SCRATCH_H, SCRATCH_W);
    }
    if (!g->scratch_s) {
        turn_free(g);
        return false;
    }
    return true;
}

void turn_free(turn_t *g)
{
    if (g->scratch_s) { ngl_surface_free(g->scratch_s); g->scratch_s = NULL; }
    if (g->scratch)   { free(g->scratch);               g->scratch   = NULL; }
    if (g->s)         { ngl_surface_free(g->s);         g->s         = NULL; }
}

void turn_rotate(turn_t *g, ngl_rotation_t rot)
{
    g->rot = rot;
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

void turn_clear(const turn_t *g, ngl_color_t c)
{
    ngl_clear(g->s, c);
}

void turn_fill(const turn_t *g, ngl_rect_t r, ngl_color_t c)
{
    ngl_fill_rect(g->s, turn_prect(g, r), c);
}

void turn_frame(const turn_t *g, ngl_rect_t r, ngl_color_t c, int16_t thickness)
{
    ngl_draw_rect(g->s, turn_prect(g, r), c, thickness);
}

void turn_round(const turn_t *g, ngl_rect_t r, int16_t radius, ngl_color_t c)
{
    ngl_fill_round_rect(g->s, turn_prect(g, r), radius, c);
}

void turn_round_frame(const turn_t *g, ngl_rect_t r, int16_t radius,
                     ngl_color_t c, int16_t thickness)
{
    ngl_draw_round_rect(g->s, turn_prect(g, r), radius, c, thickness);
}

/*
 * A landscape row is a panel column and a landscape column is a panel row.
 * Both go through ngl_fill_rect() rather than ngl_hline() / ngl_vline()
 * because the map has already decided which of the two this is, and one path
 * that is right whichever way up the tablet is beats two that have to agree.
 */
void turn_hline(const turn_t *g, int16_t x, int16_t y, int16_t w, ngl_color_t c)
{
    ngl_fill_rect(g->s, turn_prect(g, ngl_rect(x, y, w, 1)), c);
}

void turn_vline(const turn_t *g, int16_t x, int16_t y, int16_t h, ngl_color_t c)
{
    ngl_fill_rect(g->s, turn_prect(g, ngl_rect(x, y, 1, h)), c);
}

/* ------------------------------------------------------------------ */
/* Lines and circles                                                   */
/* ------------------------------------------------------------------ */

void turn_line(const turn_t *g, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
              ngl_color_t c)
{
    int16_t a0, b0, a1, b1;
    turn_point(g, x0, y0, &a0, &b0);
    turn_point(g, x1, y1, &a1, &b1);
    ngl_line(g->s, a0, b0, a1, b1, c);
}

void turn_line_aa(const turn_t *g, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                 ngl_color_t c)
{
    int16_t a0, b0, a1, b1;
    turn_point(g, x0, y0, &a0, &b0);
    turn_point(g, x1, y1, &a1, &b1);
    ngl_line_aa(g->s, a0, b0, a1, b1, c);
}

/*
 * A circle is the one shape the map cannot change, so the centre is mapped and
 * the spans are laid down in panel rows - in order, which is the whole reason
 * this file exists. The half-width per row comes off sqrtf() rather than a
 * midpoint iteration: it is one call per row on a radius of seventy-odd, the
 * FPU is single precision and idle, and the closed form cannot drift.
 */
void turn_disc(const turn_t *g, int16_t cx, int16_t cy, int16_t r, ngl_color_t c)
{
    int16_t px, py;
    turn_point(g, cx, cy, &px, &py);

    const float rr = (float)r * (float)r;
    for (int dy = -r; dy <= r; dy++) {
        const float t = rr - (float)dy * (float)dy;
        const int16_t dx = (int16_t)sqrtf(t < 0.0f ? 0.0f : t);
        ngl_fill_rect(g->s,
                      ngl_rect((int16_t)(px - dx), (int16_t)(py + dy),
                               (int16_t)(2 * dx + 1), 1), c);
    }
}

void turn_ring(const turn_t *g, int16_t cx, int16_t cy, int16_t r,
              int16_t thickness, ngl_color_t c)
{
    int16_t px, py;
    turn_point(g, cx, cy, &px, &py);

    const int16_t ri  = (int16_t)(r - thickness);
    const float   ro2 = (float)r * (float)r;
    const float   ri2 = (float)ri * (float)ri;

    for (int dy = -r; dy <= r; dy++) {
        const float d2 = (float)dy * (float)dy;
        const float to = ro2 - d2;
        if (to < 0.0f) {
            continue;
        }
        const int16_t xo = (int16_t)sqrtf(to);
        const float   ti = ri2 - d2;
        if (ti <= 0.0f) {
            /* Below the inner circle's extent: the annulus is solid here. */
            ngl_fill_rect(g->s, ngl_rect((int16_t)(px - xo), (int16_t)(py + dy),
                                         (int16_t)(2 * xo + 1), 1), c);
            continue;
        }
        const int16_t xi = (int16_t)sqrtf(ti);
        ngl_fill_rect(g->s, ngl_rect((int16_t)(px - xo), (int16_t)(py + dy),
                                     (int16_t)(xo - xi + 1), 1), c);
        ngl_fill_rect(g->s, ngl_rect((int16_t)(px + xi), (int16_t)(py + dy),
                                     (int16_t)(xo - xi + 1), 1), c);
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/*
 * One run of glyphs, turned, into the scratch and out again in a single blit.
 *
 * ngl_text() cannot be used on this canvas: it draws a glyph upright in
 * surface space, and upright in a panel-shaped surface is sideways on the
 * glass. So the bits are read here - MSB-first, bytes_per_row per scanline,
 * cells consecutive from font->first, exactly as ngl_text.c has them - and
 * written transposed.
 *
 * Both axes of that transpose flip between the two landscape orientations,
 * which is the same both-axes flip turn_point() carries: at NGL_ROT_90 a glyph
 * row counts backwards along the scratch row and the run reads forwards down
 * it, and at NGL_ROT_270 it is the other way about in both. Getting one of the
 * two and not the other is upside-down text or backwards text, neither of
 * which looks like a coordinate bug until it is written down like this.
 *
 * The scratch is filled in its own row order, which is the turned one: the
 * outer loop walks the glyph's columns because a glyph column is a scratch
 * row. So both halves of this - the composition and the blit that follows -
 * are row-major writes over contiguous memory, which is the property the whole
 * canvas is arranged around.
 */
static void run(const turn_t *g, int16_t x, int16_t y, const char *str, int n,
                const ngl_font_t *f, ngl_color_t fg, ngl_color_t bg)
{
    const int16_t fw = (int16_t)f->width;
    const int16_t fh = (int16_t)f->height;

    /* Turned, the run is fh across and n*fw down. */
    const int16_t sw = fh;
    const int16_t sh = (int16_t)(n * fw);
    if (n <= 0 || sw > SCRATCH_W || sh > SCRATCH_H) {
        return;
    }

    const bool   fwd  = (g->rot == NGL_ROT_90);
    const size_t cell = (size_t)fh * f->bytes_per_row;

    for (int i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)str[i];
        if (ch < f->first || ch > f->last) {
            ch = '?';
            if (ch < f->first || ch > f->last) {
                ch = f->first;
            }
        }
        const uint8_t *glyph = f->bits + (size_t)(ch - f->first) * cell;

        for (int16_t col = 0; col < fw; col++) {
            const int idx = i * fw + col;          /* along the run */
            const int sy  = fwd ? idx : (sh - 1 - idx);
            ngl_color_t  *dst  = g->scratch + (size_t)sy * SCRATCH_W;
            const uint8_t mask = (uint8_t)(0x80u >> (col & 7));
            const int     byte = col >> 3;

            for (int16_t r = 0; r < fh; r++) {
                const uint8_t *line = glyph + (size_t)r * f->bytes_per_row;
                dst[fwd ? (fh - 1 - r) : r] = (line[byte] & mask) ? fg : bg;
            }
        }
    }

    const ngl_rect_t pr = turn_prect(g, ngl_rect(x, y, sh, fh));
    const ngl_rect_t sr = ngl_rect(0, 0, sw, sh);
    ngl_blit(g->s, pr.x, pr.y, g->scratch_s, &sr);
}

int16_t turn_text(const turn_t *g, int16_t x, int16_t y, const char *str,
                 const ngl_font_t *f, ngl_color_t fg, ngl_color_t bg)
{
    if (!str || !f || f->width == 0) {
        return x;
    }
    const int16_t fw    = (int16_t)f->width;
    const int     chunk = SCRATCH_H / fw;

    int i = 0;
    while (str[i]) {
        int n = 0;
        while (str[i + n] && n < chunk) {
            n++;
        }
        run(g, (int16_t)(x + i * fw), y, str + i, n, f, fg, bg);
        i += n;
    }
    return (int16_t)(x + i * fw);
}

void turn_text_mid(const turn_t *g, int16_t x, int16_t y, int16_t w,
                  const char *str, const ngl_font_t *f, ngl_color_t fg,
                  ngl_color_t bg)
{
    const int16_t tw = ngl_text_width(f, str);
    turn_text(g, (int16_t)(x + (w - tw) / 2), y, str, f, fg, bg);
}

void turn_text_right(const turn_t *g, int16_t x, int16_t y, int16_t w,
                    const char *str, const ngl_font_t *f, ngl_color_t fg,
                    ngl_color_t bg)
{
    const int16_t tw = ngl_text_width(f, str);
    turn_text(g, (int16_t)(x + w - tw), y, str, f, fg, bg);
}

/* ------------------------------------------------------------------ */
/* Onto the glass                                                      */
/* ------------------------------------------------------------------ */

uint32_t turn_present(const turn_t *g, ngl_rect_t r)
{
    ngl_rect_t clipped;
    const ngl_rect_t all = ngl_rect(0, 0, g->w, g->h);
    if (!ngl_rect_intersect(&r, &all, &clipped)) {
        return 0;
    }
    const ngl_rect_t pr = turn_prect(g, clipped);

    const uint32_t t0 = (uint32_t)neos_uptime_us();
    (void)ngl_panel_scale(g->s, pr, pr, false, false);
    return (uint32_t)neos_uptime_us() - t0;
}
