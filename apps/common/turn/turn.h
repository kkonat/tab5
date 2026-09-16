/*
 * The canvas, and the turn that never reaches the PPA.
 *
 * The Tab5's panel is 720x1280 portrait underneath. A landscape app drawn the
 * ordinary way - into ngl_screen(), out through ngl_flush() - is written twice
 * and the second write is a 90 degree rotate, which is the expensive half by a
 * long way: measured on this machine the PPA scales in order at 85 Mpixels/s
 * and rotates at 19, because a rotate writes a column where it read a row and
 * PSRAM never gets to burst. lab/defender pays that once per frame for a whole
 * screen and will not, and neither will this.
 *
 * So the canvas here is already the panel's shape - 720 wide by 1280 tall -
 * and the turn happens in the coordinates rather than in the pixels. Every
 * function below takes landscape coordinates (0..1279 across, 0..719 down, the
 * way the tablet is being held) and maps them before it draws. What comes out
 * of the map is still a rectangle, still a circle and still a row-major span,
 * so underneath every one of these is ngl's own primitive writing panel rows
 * in order. Nothing walks a stride that it did not have to.
 *
 * Text is the one thing that does not survive the map, because a glyph is not
 * symmetric: a landscape line of type is a column on the glass. turn_text()
 * composes a run of glyphs turned, into a scratch buffer, and blits that -
 * which is row-major on both sides and costs one ngl_blit() per run.
 *
 * turn_present() hands a rectangle to the glass through ngl_panel_scale(), 1:1,
 * no rotation and no mirror: the flip between the two landscape orientations
 * is in the map as well, so the picture that reaches the panel is already the
 * right way round and the engine has nothing left to do but copy.
 */
#ifndef TURN_H
#define TURN_H

#include <stdbool.h>
#include <stdint.h>

#include "ngl.h"

typedef struct {
    ngl_surface_t *s;          /* the canvas, panel-shaped: pw x ph      */
    ngl_color_t   *scratch;    /* turned glyph runs, on their way to it  */
    ngl_surface_t *scratch_s;
    int16_t        pw, ph;     /* the panel, always 720x1280             */
    int16_t        w, h;       /* landscape, which is that on its side   */
    ngl_rotation_t rot;        /* NGL_ROT_90 or NGL_ROT_270, nothing else */
    bool           fast;       /* the canvas came out of internal RAM    */
} turn_t;

/** Allocate the canvas for @p rot. False if there is no memory for one. */
bool turn_init(turn_t *g, ngl_rotation_t rot);
void turn_free(turn_t *g);

/**
 * Turn the picture over.
 *
 * NGL_ROT_270 is NGL_ROT_90 flipped in both axes, and both flips are in the
 * map - so this changes where the next draw lands and nothing else. The caller
 * owns the repaint, because every pixel already on the canvas is now upside
 * down.
 */
void turn_rotate(turn_t *g, ngl_rotation_t rot);

/* ------------------------------------------------------------------ */
/* The map                                                             */
/* ------------------------------------------------------------------ */

/*
 * Landscape (x, y) -> panel (px, py), and it is ngl_screen.c's phys_index()
 * with the framebuffer taken out:
 *
 *   NGL_ROT_90    px = pw - 1 - y      py = x
 *   NGL_ROT_270   px = y               py = ph - 1 - x
 *
 * Inline and in the header because the widgets call it per vertex and it is
 * three instructions.
 */
static inline void turn_point(const turn_t *g, int16_t x, int16_t y,
                             int16_t *px, int16_t *py)
{
    if (g->rot == NGL_ROT_90) {
        *px = (int16_t)(g->pw - 1 - y);
        *py = x;
    } else {
        *px = y;
        *py = (int16_t)(g->ph - 1 - x);
    }
}

/** The same for a rectangle, which stays a rectangle - only narrower and
    taller, since the two axes have swapped. */
static inline ngl_rect_t turn_prect(const turn_t *g, ngl_rect_t r)
{
    if (g->rot == NGL_ROT_90) {
        return ngl_rect((int16_t)(g->pw - (r.y + r.h)), r.x, r.h, r.w);
    }
    return ngl_rect(r.y, (int16_t)(g->ph - (r.x + r.w)), r.h, r.w);
}

/* ------------------------------------------------------------------ */
/* Drawing, in landscape coordinates                                   */
/* ------------------------------------------------------------------ */

void turn_clear(const turn_t *g, ngl_color_t c);
void turn_fill(const turn_t *g, ngl_rect_t r, ngl_color_t c);
void turn_frame(const turn_t *g, ngl_rect_t r, ngl_color_t c, int16_t thickness);
void turn_round(const turn_t *g, ngl_rect_t r, int16_t radius, ngl_color_t c);
void turn_round_frame(const turn_t *g, ngl_rect_t r, int16_t radius,
                     ngl_color_t c, int16_t thickness);

/** A landscape row. It is a panel column, so this is one strided fill. */
void turn_hline(const turn_t *g, int16_t x, int16_t y, int16_t w, ngl_color_t c);

/** A landscape column. It is a panel row, so this is the cheap one - which is
    why the scope draws itself out of these. */
void turn_vline(const turn_t *g, int16_t x, int16_t y, int16_t h, ngl_color_t c);

void turn_line(const turn_t *g, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
              ngl_color_t c);
void turn_line_aa(const turn_t *g, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                 ngl_color_t c);

/** A filled circle. Rotation does not change what a circle looks like, so this
    maps the centre and then draws panel rows in order. */
void turn_disc(const turn_t *g, int16_t cx, int16_t cy, int16_t r, ngl_color_t c);

/** An annulus of @p thickness, outer radius @p r. */
void turn_ring(const turn_t *g, int16_t cx, int16_t cy, int16_t r,
              int16_t thickness, ngl_color_t c);

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/** Draw @p str with its top-left at (x, y). Returns the x after the last
    glyph, as ngl_text() does. Cells are filled with @p bg first. */
int16_t turn_text(const turn_t *g, int16_t x, int16_t y, const char *str,
                 const ngl_font_t *f, ngl_color_t fg, ngl_color_t bg);

/** Centred in [x, x+w). */
void turn_text_mid(const turn_t *g, int16_t x, int16_t y, int16_t w,
                  const char *str, const ngl_font_t *f, ngl_color_t fg,
                  ngl_color_t bg);

/** Right-aligned to x+w. */
void turn_text_right(const turn_t *g, int16_t x, int16_t y, int16_t w,
                    const char *str, const ngl_font_t *f, ngl_color_t fg,
                    ngl_color_t bg);

/* ------------------------------------------------------------------ */
/* Onto the glass                                                      */
/* ------------------------------------------------------------------ */

/**
 * Copy one landscape rectangle of the canvas to the panel.
 *
 * 1:1 and unmirrored, so the PPA has nothing to do but move bytes in order.
 * Nothing is marked dirty and no flush follows - this *is* the panel, which is
 * also why ngl's back buffer no longer matches it over @p r: whatever restores
 * pixels from that buffer, which is any system panel closing, will put back
 * what was underneath. neos_ui_busy() going false is the edge to repaint on.
 *
 * Returns what it cost in microseconds, because that is one of the numbers
 * this app was written to find out.
 */
uint32_t turn_present(const turn_t *g, ngl_rect_t r);

#endif /* TURN_H */
