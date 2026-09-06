/*
 * ngl drawing primitives.
 *
 * Portable: no ESP-IDF dependency beyond the PSRAM allocator in ngl_surface_new.
 * Everything clips against surface->clip, so callers never have to bounds-check.
 */
#include <stdlib.h>
#include <string.h>

#include "ngl.h"
#include "ngl_internal.h"
#include "esp_heap_caps.h"

/* ------------------------------------------------------------------ */
/* Rect helpers                                                        */
/* ------------------------------------------------------------------ */

bool ngl_rect_intersect(const ngl_rect_t *a, const ngl_rect_t *b, ngl_rect_t *out)
{
    const int16_t x0 = a->x > b->x ? a->x : b->x;
    const int16_t y0 = a->y > b->y ? a->y : b->y;
    const int16_t a1x = a->x + a->w, b1x = b->x + b->w;
    const int16_t a1y = a->y + a->h, b1y = b->y + b->h;
    const int16_t x1 = a1x < b1x ? a1x : b1x;
    const int16_t y1 = a1y < b1y ? a1y : b1y;

    out->x = x0;
    out->y = y0;
    out->w = (int16_t)(x1 - x0);
    out->h = (int16_t)(y1 - y0);
    return out->w > 0 && out->h > 0;
}

ngl_rect_t ngl_rect_union(const ngl_rect_t *a, const ngl_rect_t *b)
{
    if (ngl_rect_empty(a)) { return *b; }
    if (ngl_rect_empty(b)) { return *a; }

    const int16_t x0 = a->x < b->x ? a->x : b->x;
    const int16_t y0 = a->y < b->y ? a->y : b->y;
    const int16_t a1x = a->x + a->w, b1x = b->x + b->w;
    const int16_t a1y = a->y + a->h, b1y = b->y + b->h;
    const int16_t x1 = a1x > b1x ? a1x : b1x;
    const int16_t y1 = a1y > b1y ? a1y : b1y;

    return ngl_rect(x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0));
}

bool ngl_rect_contains(const ngl_rect_t *r, int16_t x, int16_t y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                            */
/* ------------------------------------------------------------------ */

void ngl_surface_init(ngl_surface_t *s, ngl_color_t *px, int16_t w, int16_t h, int16_t stride)
{
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride ? stride : w;
    s->clip = ngl_rect(0, 0, w, h);
    s->owns_px = false;
}

ngl_surface_t *ngl_surface_wrap(ngl_color_t *px, int16_t w, int16_t h, int16_t stride)
{
    if (!px || w <= 0 || h <= 0) {
        return NULL;
    }
    ngl_surface_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    ngl_surface_init(s, px, w, h, stride);
    return s;
}

ngl_surface_t *ngl_surface_new(int16_t w, int16_t h)
{
    if (w <= 0 || h <= 0) {
        return NULL;
    }
    ngl_surface_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    /*
     * DMA-capable so surfaces can be handed straight to esp_lcd if needed, and
     * aligned - base and row both - so that ngl_blit_scale() can hand one to
     * the PPA rather than falling back to a loop. A row rounded up to a whole
     * number of 64-byte cache lines is 32 pixels of slack at worst, which is
     * cheap against the alternative of every surface being ineligible for the
     * one operation that most wants the hardware.
     */
    const int16_t stride = (int16_t)((w + 31) & ~31);
    ngl_color_t *px = heap_caps_aligned_calloc(64, (size_t)stride * h, sizeof(ngl_color_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!px) {
        free(s);
        return NULL;
    }
    ngl_surface_init(s, px, w, h, stride);
    s->owns_px = true;
    return s;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

/*
 * These exist because ngl_surface_t is opaque past this component. They are
 * three instructions each and cost a syscall-table entry; that is the price of
 * being able to add a field to a surface without invalidating the card.
 */

int16_t ngl_surface_w(const ngl_surface_t *s)
{
    return s ? s->w : 0;
}

int16_t ngl_surface_h(const ngl_surface_t *s)
{
    return s ? s->h : 0;
}

ngl_rect_t ngl_surface_bounds(const ngl_surface_t *s)
{
    return s ? ngl_rect(0, 0, s->w, s->h) : ngl_rect(0, 0, 0, 0);
}

ngl_rect_t ngl_surface_clip(const ngl_surface_t *s)
{
    return s ? s->clip : ngl_rect(0, 0, 0, 0);
}

const ngl_color_t *ngl_surface_row(const ngl_surface_t *s, int16_t y)
{
    if (!s || !s->px || y < 0 || y >= s->h) {
        return NULL;
    }
    return &s->px[(size_t)y * s->stride];
}

void ngl_surface_free(ngl_surface_t *s)
{
    if (!s) {
        return;
    }
    if (s->owns_px) {
        heap_caps_free(s->px);
    }
    free(s);
}

void ngl_clip_set(ngl_surface_t *s, const ngl_rect_t *r)
{
    /* A panel is up and this is not its task: leave the clip alone rather than
       letting the app hand itself one. Its draws are refused below anyway; the
       point of not writing here is that the panel's own clip is in this field
       and an app must not be able to move it mid-frame. */
    if (ngl_screen_blocked(s)) {
        return;
    }

    /* On the screen the allowed area excludes the system bar, and no caller
       can widen past it - that is what makes the bar off limits to apps. */
    const ngl_rect_t allowed = (s == ngl_screen())
                              ? ngl_app_area()
                              : ngl_rect(0, 0, s->w, s->h);
    if (!r) {
        s->clip = allowed;
        return;
    }
    if (!ngl_rect_intersect(r, &allowed, &s->clip)) {
        s->clip = ngl_rect(0, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Pixels                                                              */
/* ------------------------------------------------------------------ */

void ngl_pixel(ngl_surface_t *s, int16_t x, int16_t y, ngl_color_t c)
{
    if (ngl_screen_blocked(s)) {
        return;
    }
    if (ngl_rect_contains(&s->clip, x, y)) {
        s->px[(size_t)y * s->stride + x] = c;
    }
}

void ngl_pixel_blend(ngl_surface_t *s, int16_t x, int16_t y, ngl_color_t c, uint8_t a)
{
    if (a == 0 || ngl_screen_blocked(s) || !ngl_rect_contains(&s->clip, x, y)) {
        return;
    }
    if (a == 255) {
        s->px[(size_t)y * s->stride + x] = c;
        return;
    }

    ngl_color_t *p = &s->px[(size_t)y * s->stride + x];
    const uint16_t d = *p;

    /* Blend in the 565 domain: cheaper than a full unpack to 888 and back,
       and the error is below one LSB of each channel. */
    const uint16_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    const uint16_t sr = (c >> 11) & 0x1F, sg = (c >> 5) & 0x3F, sb = c & 0x1F;
    const uint16_t ia = 255 - a;

    const uint16_t r = (uint16_t)((sr * a + dr * ia) / 255);
    const uint16_t g = (uint16_t)((sg * a + dg * ia) / 255);
    const uint16_t b = (uint16_t)((sb * a + db * ia) / 255);

    *p = (ngl_color_t)((r << 11) | (g << 5) | b);
}

/* ------------------------------------------------------------------ */
/* Rects                                                               */
/* ------------------------------------------------------------------ */

void ngl_fill_rect(ngl_surface_t *s, ngl_rect_t r, ngl_color_t c)
{
    ngl_rect_t d;
    if (ngl_screen_blocked(s) || !ngl_rect_intersect(&r, &s->clip, &d)) {
        return;
    }
    for (int16_t y = d.y; y < d.y + d.h; y++) {
        ngl_color_t *row = &s->px[(size_t)y * s->stride + d.x];
        for (int16_t x = 0; x < d.w; x++) {
            row[x] = c;
        }
    }
    if (s == ngl_screen()) {
        ngl_dirty(&d);
    }
}

void ngl_clear(ngl_surface_t *s, ngl_color_t c)
{
    /*
     * The one draw call that ignores the clip - it has to, or an app could
     * never repaint the bar. So it is also the one that has to be refused
     * explicitly while a panel is up, since clipping the app to nothing would
     * not stop this.
     */
    if (ngl_screen_blocked(s)) {
        return;
    }

    const ngl_rect_t whole = ngl_rect(0, 0, s->w, s->h);

    if (s != ngl_screen()) {
        ngl_fill_rect(s, whole, c);
        return;
    }

    /* Clearing the screen means the whole screen, bar included - then the bar
       is redrawn, so an app that clears can never leave it blank. */
    const ngl_rect_t saved = s->clip;
    s->clip = whole;
    ngl_fill_rect(s, whole, c);
    s->clip = saved;

    ngl_bar_paint();
}

void ngl_hline(ngl_surface_t *s, int16_t x, int16_t y, int16_t w, ngl_color_t c)
{
    ngl_fill_rect(s, ngl_rect(x, y, w, 1), c);
}

void ngl_vline(ngl_surface_t *s, int16_t x, int16_t y, int16_t h, ngl_color_t c)
{
    ngl_fill_rect(s, ngl_rect(x, y, 1, h), c);
}

void ngl_draw_rect(ngl_surface_t *s, ngl_rect_t r, ngl_color_t c, int16_t t)
{
    if (t <= 0 || r.w <= 0 || r.h <= 0) {
        return;
    }
    if (t * 2 >= r.w || t * 2 >= r.h) {
        ngl_fill_rect(s, r, c);
        return;
    }
    ngl_fill_rect(s, ngl_rect(r.x, r.y, r.w, t), c);                       /* top    */
    ngl_fill_rect(s, ngl_rect(r.x, (int16_t)(r.y + r.h - t), r.w, t), c);  /* bottom */
    ngl_fill_rect(s, ngl_rect(r.x, (int16_t)(r.y + t), t,
                            (int16_t)(r.h - 2 * t)), c);                 /* left   */
    ngl_fill_rect(s, ngl_rect((int16_t)(r.x + r.w - t), (int16_t)(r.y + t), t,
                            (int16_t)(r.h - 2 * t)), c);                 /* right  */
}

/* ------------------------------------------------------------------ */
/* Rounded rects                                                       */
/* ------------------------------------------------------------------ */

/* Clamp the radius to what actually fits in the rect. */
static int16_t clamp_radius(const ngl_rect_t *r, int16_t rad)
{
    const int16_t max = (r->w < r->h ? r->w : r->h) / 2;
    if (rad < 0) { rad = 0; }
    return rad > max ? max : rad;
}

void ngl_fill_round_rect(ngl_surface_t *s, ngl_rect_t r, int16_t radius, ngl_color_t c)
{
    radius = clamp_radius(&r, radius);
    if (radius == 0) {
        ngl_fill_rect(s, r, c);
        return;
    }

    /* Middle slab, full width. */
    ngl_fill_rect(s, ngl_rect(r.x, (int16_t)(r.y + radius), r.w,
                            (int16_t)(r.h - 2 * radius)), c);

    /* Cap rows: one span per scanline, width from the circle equation. */
    const int32_t r2 = (int32_t)radius * radius;
    for (int16_t dy = 0; dy < radius; dy++) {
        const int32_t yy = radius - dy;
        int16_t dx = 0;
        while ((int32_t)(dx + 1) * (dx + 1) + yy * yy <= r2 + radius) {
            dx++;
        }
        const int16_t x0 = (int16_t)(r.x + radius - dx);
        const int16_t w  = (int16_t)(r.w - 2 * (radius - dx));
        ngl_fill_rect(s, ngl_rect(x0, (int16_t)(r.y + dy), w, 1), c);
        ngl_fill_rect(s, ngl_rect(x0, (int16_t)(r.y + r.h - 1 - dy), w, 1), c);
    }
}

void ngl_draw_round_rect(ngl_surface_t *s, ngl_rect_t r, int16_t radius, ngl_color_t c,
                        int16_t t)
{
    if (t <= 0) {
        return;
    }
    radius = clamp_radius(&r, radius);
    if (radius == 0) {
        ngl_draw_rect(s, r, c, t);
        return;
    }
    /* Outline = filled outer shape minus filled inner shape. Simple, and the
       corner geometry stays consistent with ngl_fill_round_rect by construction. */
    ngl_rect_t inner = ngl_rect((int16_t)(r.x + t), (int16_t)(r.y + t),
                              (int16_t)(r.w - 2 * t), (int16_t)(r.h - 2 * t));
    if (inner.w <= 0 || inner.h <= 0) {
        ngl_fill_round_rect(s, r, radius, c);
        return;
    }

    /* Draw the ring by filling the outer shape only where the inner is not.
       Done per scanline to avoid a temp surface. */
    const int16_t ri = clamp_radius(&inner, (int16_t)(radius - t));
    const int32_t ro2 = (int32_t)radius * radius;
    const int32_t ri2 = (int32_t)ri * ri;

    for (int16_t y = 0; y < r.h; y++) {
        /* Outer span for this row */
        int16_t ox0, ox1;
        if (y < radius) {
            const int32_t yy = radius - y;
            int16_t dx = 0;
            while ((int32_t)(dx + 1) * (dx + 1) + yy * yy <= ro2 + radius) { dx++; }
            ox0 = (int16_t)(r.x + radius - dx);
            ox1 = (int16_t)(r.x + r.w - (radius - dx));
        } else if (y >= r.h - radius) {
            const int32_t yy = radius - (r.h - 1 - y);
            int16_t dx = 0;
            while ((int32_t)(dx + 1) * (dx + 1) + yy * yy <= ro2 + radius) { dx++; }
            ox0 = (int16_t)(r.x + radius - dx);
            ox1 = (int16_t)(r.x + r.w - (radius - dx));
        } else {
            ox0 = r.x;
            ox1 = (int16_t)(r.x + r.w);
        }

        const int16_t iy = (int16_t)(y - t);
        int16_t ix0 = 0, ix1 = -1;   /* empty inner span by default */
        if (iy >= 0 && iy < inner.h) {
            if (iy < ri) {
                const int32_t yy = ri - iy;
                int16_t dx = 0;
                while ((int32_t)(dx + 1) * (dx + 1) + yy * yy <= ri2 + ri) { dx++; }
                ix0 = (int16_t)(inner.x + ri - dx);
                ix1 = (int16_t)(inner.x + inner.w - (ri - dx));
            } else if (iy >= inner.h - ri) {
                const int32_t yy = ri - (inner.h - 1 - iy);
                int16_t dx = 0;
                while ((int32_t)(dx + 1) * (dx + 1) + yy * yy <= ri2 + ri) { dx++; }
                ix0 = (int16_t)(inner.x + ri - dx);
                ix1 = (int16_t)(inner.x + inner.w - (ri - dx));
            } else {
                ix0 = inner.x;
                ix1 = (int16_t)(inner.x + inner.w);
            }
        }

        const int16_t sy = (int16_t)(r.y + y);
        if (ix1 <= ix0) {
            ngl_fill_rect(s, ngl_rect(ox0, sy, (int16_t)(ox1 - ox0), 1), c);
        } else {
            ngl_fill_rect(s, ngl_rect(ox0, sy, (int16_t)(ix0 - ox0), 1), c);
            ngl_fill_rect(s, ngl_rect(ix1, sy, (int16_t)(ox1 - ix1), 1), c);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lines                                                               */
/* ------------------------------------------------------------------ */

void ngl_line(ngl_surface_t *s, int16_t x0, int16_t y0, int16_t x1, int16_t y1, ngl_color_t c)
{
    if (y0 == y1) { ngl_hline(s, (int16_t)(x0 < x1 ? x0 : x1), y0, (int16_t)(abs(x1 - x0) + 1), c); return; }
    if (x0 == x1) { ngl_vline(s, x0, (int16_t)(y0 < y1 ? y0 : y1), (int16_t)(abs(y1 - y0) + 1), c); return; }

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    ngl_rect_t bounds = ngl_rect((int16_t)(x0 < x1 ? x0 : x1), (int16_t)(y0 < y1 ? y0 : y1),
                               (int16_t)(abs(x1 - x0) + 1), (int16_t)(abs(y1 - y0) + 1));
    for (;;) {
        ngl_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) { break; }
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 = (int16_t)(x0 + sx); }
        if (e2 <= dx) { err += dx; y0 = (int16_t)(y0 + sy); }
    }
    if (s == ngl_screen()) { ngl_dirty(&bounds); }
}

/* Xiaolin Wu, integer-fraction variant. */
void ngl_line_aa(ngl_surface_t *s, int16_t x0, int16_t y0, int16_t x1, int16_t y1, ngl_color_t c)
{
    ngl_rect_t bounds = ngl_rect((int16_t)((x0 < x1 ? x0 : x1) - 1),
                               (int16_t)((y0 < y1 ? y0 : y1) - 1),
                               (int16_t)(abs(x1 - x0) + 3),
                               (int16_t)(abs(y1 - y0) + 3));

    bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        int16_t t;
        t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        int16_t t;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    const int dx = x1 - x0;
    const int dy = y1 - y0;
    /* 16.16 fixed point gradient */
    const int32_t grad = dx == 0 ? (1 << 16) : (int32_t)((int64_t)dy * 65536 / dx);

    int32_t inter = (int32_t)y0 * 65536;
    for (int16_t x = x0; x <= x1; x++) {
        const int16_t yi = (int16_t)(inter >> 16);
        const uint8_t f = (uint8_t)((inter >> 8) & 0xFF);   /* fractional part */
        if (steep) {
            ngl_pixel_blend(s, yi, x, c, (uint8_t)(255 - f));
            ngl_pixel_blend(s, (int16_t)(yi + 1), x, c, f);
        } else {
            ngl_pixel_blend(s, x, yi, c, (uint8_t)(255 - f));
            ngl_pixel_blend(s, x, (int16_t)(yi + 1), c, f);
        }
        inter += grad;
    }

    if (s == ngl_screen()) { ngl_dirty(&bounds); }
}

/* ------------------------------------------------------------------ */
/* Blitting                                                            */
/* ------------------------------------------------------------------ */

static bool blit_setup(ngl_surface_t *dst, int16_t x, int16_t y,
                       const ngl_surface_t *src, const ngl_rect_t *src_rect,
                       ngl_rect_t *sr, ngl_rect_t *dr)
{
    ngl_rect_t whole = ngl_rect(0, 0, src->w, src->h);
    ngl_rect_t want = src_rect ? *src_rect : whole;
    if (!ngl_rect_intersect(&want, &whole, sr)) {
        return false;
    }
    ngl_rect_t placed = ngl_rect(x, y, sr->w, sr->h);
    if (!ngl_rect_intersect(&placed, &dst->clip, dr)) {
        return false;
    }
    /* Shift the source origin by however much the destination was clipped. */
    sr->x = (int16_t)(sr->x + (dr->x - x));
    sr->y = (int16_t)(sr->y + (dr->y - y));
    sr->w = dr->w;
    sr->h = dr->h;
    return true;
}

void ngl_blit(ngl_surface_t *dst, int16_t x, int16_t y,
             const ngl_surface_t *src, const ngl_rect_t *src_rect)
{
    if (ngl_screen_blocked(dst)) {
        return;
    }

    ngl_rect_t sr, dr;
    if (!blit_setup(dst, x, y, src, src_rect, &sr, &dr)) {
        return;
    }
    for (int16_t row = 0; row < dr.h; row++) {
        const ngl_color_t *sp = &src->px[(size_t)(sr.y + row) * src->stride + sr.x];
        ngl_color_t *dp = &dst->px[(size_t)(dr.y + row) * dst->stride + dr.x];
        memcpy(dp, sp, (size_t)dr.w * sizeof(ngl_color_t));
    }
    if (dst == ngl_screen()) { ngl_dirty(&dr); }
}

void ngl_blit_key(ngl_surface_t *dst, int16_t x, int16_t y,
                 const ngl_surface_t *src, const ngl_rect_t *src_rect, ngl_color_t key)
{
    if (ngl_screen_blocked(dst)) {
        return;
    }

    ngl_rect_t sr, dr;
    if (!blit_setup(dst, x, y, src, src_rect, &sr, &dr)) {
        return;
    }
    for (int16_t row = 0; row < dr.h; row++) {
        const ngl_color_t *sp = &src->px[(size_t)(sr.y + row) * src->stride + sr.x];
        ngl_color_t *dp = &dst->px[(size_t)(dr.y + row) * dst->stride + dr.x];
        for (int16_t col = 0; col < dr.w; col++) {
            if (sp[col] != key) {
                dp[col] = sp[col];
            }
        }
    }
    if (dst == ngl_screen()) { ngl_dirty(&dr); }
}

bool ngl_blit_scale(ngl_surface_t *dst, ngl_rect_t dst_rect,
                   const ngl_surface_t *src, const ngl_rect_t *src_rect)
{
    if (!dst || !src || !dst->px || !src->px || ngl_screen_blocked(dst)) {
        return false;
    }
    if (dst_rect.w <= 0 || dst_rect.h <= 0) {
        return false;
    }

    const ngl_rect_t whole = ngl_rect(0, 0, src->w, src->h);
    ngl_rect_t sr;
    if (!ngl_rect_intersect(src_rect ? src_rect : &whole, &whole, &sr)) {
        return false;
    }

    ngl_rect_t dr;
    if (!ngl_rect_intersect(&dst_rect, &dst->clip, &dr)) {
        return false;
    }

    /*
     * The engine scales a block, not a block with a piece taken out of it, and
     * working out which fraction of the source a clipped destination wants is
     * exact only at integer factors. So the hardware gets the whole rectangle
     * or none of it, and anything the clip touched goes round the loop below -
     * which is the uninteresting case anyway: a frame placed inside the app
     * area is not clipped, and one that is has just been dragged half off the
     * screen.
     */
    if (dr.w == dst_rect.w && dr.h == dst_rect.h &&
        ngl_ppa_scale(dst, dr, src, sr)) {
        if (dst == ngl_screen()) { ngl_dirty(&dr); }
        return true;
    }

    /*
     * Nearest neighbour in 16.16. The source step is fixed by the *unclipped*
     * destination, so a clipped blit shows the same part of the picture at the
     * same size as an unclipped one would have - it is a window onto the same
     * result, not a squeezed version of it.
     */
    const uint32_t stepx = ((uint32_t)sr.w << 16) / (uint32_t)dst_rect.w;
    const uint32_t stepy = ((uint32_t)sr.h << 16) / (uint32_t)dst_rect.h;
    const uint32_t offx  = (uint32_t)(dr.x - dst_rect.x) * stepx;
    const uint32_t offy  = (uint32_t)(dr.y - dst_rect.y) * stepy;

    for (int16_t row = 0; row < dr.h; row++) {
        uint32_t syf = offy + (uint32_t)row * stepy;
        int16_t sy = (int16_t)(sr.y + (syf >> 16));
        if (sy >= sr.y + sr.h) { sy = (int16_t)(sr.y + sr.h - 1); }

        const ngl_color_t *sp = &src->px[(size_t)sy * src->stride + sr.x];
        ngl_color_t *dp = &dst->px[(size_t)(dr.y + row) * dst->stride + dr.x];

        uint32_t sxf = offx;
        for (int16_t col = 0; col < dr.w; col++) {
            uint32_t sx = sxf >> 16;
            if (sx >= (uint32_t)sr.w) { sx = (uint32_t)sr.w - 1; }
            dp[col] = sp[sx];
            sxf += stepx;
        }
    }
    if (dst == ngl_screen()) { ngl_dirty(&dr); }
    return true;
}

void ngl_blit_p8(ngl_surface_t *dst, int16_t x, int16_t y,
                const uint8_t *src, int16_t w, int16_t h, int16_t stride,
                const ngl_color_t *pal, int key)
{
    if (!dst || !dst->px || !src || !pal || w <= 0 || h <= 0 ||
        ngl_screen_blocked(dst)) {
        return;
    }
    if (stride <= 0) {
        stride = w;
    }

    ngl_rect_t placed = ngl_rect(x, y, w, h), dr;
    if (!ngl_rect_intersect(&placed, &dst->clip, &dr)) {
        return;
    }
    /* However much the clip took off the left and top is where the source
       starts - the same shift blit_setup() makes, done by hand because the
       source here is not a surface. */
    const int16_t sx0 = (int16_t)(dr.x - x);
    const int16_t sy0 = (int16_t)(dr.y - y);

    for (int16_t row = 0; row < dr.h; row++) {
        const uint8_t *sp = &src[(size_t)(sy0 + row) * stride + sx0];
        ngl_color_t *dp = &dst->px[(size_t)(dr.y + row) * dst->stride + dr.x];

        if (key < 0) {
            for (int16_t col = 0; col < dr.w; col++) {
                dp[col] = pal[sp[col]];
            }
        } else {
            const uint8_t k = (uint8_t)key;
            for (int16_t col = 0; col < dr.w; col++) {
                const uint8_t i = sp[col];
                if (i != k) {
                    dp[col] = pal[i];
                }
            }
        }
    }
    if (dst == ngl_screen()) { ngl_dirty(&dr); }
}

/*
 * Darken in place.
 *
 * Straight into the pixels rather than through ngl_pixel_blend(): a scrim
 * covers most of the screen, and per-pixel clip tests over 900,000 pixels is
 * the difference between a panel that appears and one that wipes on.
 */
void ngl_dim_rect(ngl_surface_t *s, ngl_rect_t r, uint8_t amount)
{
    ngl_rect_t d;
    if (ngl_screen_blocked(s) || !ngl_rect_intersect(&r, &s->clip, &d)) {
        return;
    }
    const uint32_t keep = 255u - amount;

    for (int16_t y = d.y; y < d.y + d.h; y++) {
        ngl_color_t *row = &s->px[(size_t)y * s->stride + d.x];
        for (int16_t x = 0; x < d.w; x++) {
            const ngl_color_t p = row[x];
            const uint32_t rr = ((p >> 11) & 0x1F) * keep / 255u;
            const uint32_t gg = ((p >> 5)  & 0x3F) * keep / 255u;
            const uint32_t bb = ( p        & 0x1F) * keep / 255u;
            row[x] = (ngl_color_t)((rr << 11) | (gg << 5) | bb);
        }
    }
    if (s == ngl_screen()) {
        ngl_dirty(&d);
    }
}
