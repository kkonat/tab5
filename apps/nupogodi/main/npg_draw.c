/*
 * 72 bits of segment state, turned into pixels.
 *
 * The naive way is what the emulators this is descended from do: copy the
 * background over the frame, draw every lit segment onto it, push the lot.
 * That is 1.4 MB of PSRAM traffic per frame at 720p, a hundred and twenty-odd
 * times a second, to show a display that changes about four times a second.
 *
 * So instead the previous segment state is kept, and a frame repaints only
 * the segments that actually moved. Each one is a small rectangle: the
 * background is put back inside it, every lit segment that overlaps it is
 * drawn again, and that rectangle alone is blitted to the screen. A tick
 * where nothing moved - which is nearly all of them - does no work and does
 * not flush.
 *
 * Redrawing *all* the overlapping segments rather than just the changed one
 * is what makes the order not matter. Two segments that share pixels are
 * common in this artwork (the wolf's two arm positions overlap at the
 * shoulder), and a scheme that erased one and drew the other would leave a
 * notch where they met, depending on which moved first.
 *
 * Composition happens in an app-owned buffer rather than in the screen
 * surface, because a segment multiplies the background rather than replacing
 * it and ngl has no multiply blend - and could not have one that read back
 * from the panel's buffer anyway. So the buffer is ours, ngl_surface_wrap()
 * makes it something ngl_blit() will read from, and the screen only ever
 * receives finished pixels.
 */
#include <stdlib.h>
#include <string.h>

#include "nupogodi.h"

static const npg_asset_t *s_a;
static ngl_color_t       *s_px;      /* the compose buffer, artwork sized */
static ngl_surface_t     *s_surf;    /* the same pixels, as something ngl reads */
static ngl_rect_t         s_at;      /* where the artwork's origin is on screen */
static ngl_rect_t         s_vis;     /* the part of it that reaches the screen */
static bool               s_lit[SM5A_SEGMENTS];
static bool               s_all;

/* ------------------------------------------------------------------ */

/**
 * An LCD segment darkens what is printed behind it; it does not cover it.
 *
 * `c` is 255 where the segment is not, and the caller skips those rather than
 * multiplying by 255/256 and losing a bit of the background to rounding on
 * every pixel of every frame.
 */
static inline ngl_color_t mul565(ngl_color_t bg, uint8_t c)
{
    uint32_t r = ((bg >> 11) & 0x1f) * c >> 8;
    uint32_t g = ((bg >> 5)  & 0x3f) * c >> 8;
    uint32_t b = ( bg        & 0x1f) * c >> 8;
    return (ngl_color_t)((r << 11) | (g << 5) | b);
}

static inline bool overlaps(const npg_seg_t *s, const ngl_rect_t *r)
{
    return !(s->x >= r->x + r->w || s->x + s->w <= r->x ||
             s->y >= r->y + r->h || s->y + s->h <= r->y);
}

/* Put the artwork back inside `r`, undoing whatever was drawn there. */
static void restore(const ngl_rect_t *r)
{
    for (int y = 0; y < r->h; y++) {
        const size_t off = (size_t)(r->y + y) * s_a->w + r->x;
        memcpy(s_px + off, s_a->bg + off, (size_t)r->w * sizeof(ngl_color_t));
    }
}

/* Draw segment `i` onto the compose buffer, clipped to `r`. */
static void blend(int i, const ngl_rect_t *r)
{
    const npg_seg_t *sg = &s_a->seg[i];
    if (!sg->w || !sg->h) {
        return;
    }

    /* The overlap of the segment with the rectangle being repainted. */
    int x0 = sg->x > r->x ? sg->x : r->x;
    int y0 = sg->y > r->y ? sg->y : r->y;
    int x1 = sg->x + sg->w < r->x + r->w ? sg->x + sg->w : r->x + r->w;
    int y1 = sg->y + sg->h < r->y + r->h ? sg->y + sg->h : r->y + r->h;

    for (int y = y0; y < y1; y++) {
        const uint8_t *cov = s_a->cov + sg->px + (size_t)(y - sg->y) * sg->w;
        ngl_color_t   *dst = s_px + (size_t)y * s_a->w;

        for (int x = x0; x < x1; x++) {
            const uint8_t c = cov[x - sg->x];
            if (c != 0xff) {
                dst[x] = mul565(dst[x], c);
            }
        }
    }
}

/* Everything lit that shows inside `r`, in index order. */
static void blend_all_in(const ngl_rect_t *r)
{
    for (int i = 0; i < s_a->nseg; i++) {
        if (s_lit[i] && overlaps(&s_a->seg[i], r)) {
            blend(i, r);
        }
    }
}

/*
 * A rectangle of the compose buffer, onto the screen.
 *
 * Clipped to the visible picture on the way, which is the one thing that
 * keeps the asset's black border off the glass: a segment's rectangle
 * includes the room its shadow needs, so a segment near the edge has a
 * rectangle that reaches into the border - and a push of that rectangle would
 * paint a notch of black through the bezel every time that segment moved.
 */
static void push(const ngl_rect_t *r)
{
    ngl_rect_t v;
    if (!ngl_rect_intersect(r, &s_vis, &v)) {
        return;
    }
    ngl_blit(ngl_screen(), (int16_t)(s_at.x + v.x), (int16_t)(s_at.y + v.y),
             s_surf, &v);
}

/* ------------------------------------------------------------------ */

bool npg_draw_begin(const npg_asset_t *a, ngl_rect_t at)
{
    s_a = a;

    /*
     * `at` is where the picture goes; s_at is where the buffer's own origin
     * would go, which is up and to the left of it by the border that is never
     * drawn. Everything below works in buffer coordinates - the segment table
     * does - and this is the only place the difference exists.
     */
    s_at  = ngl_rect((int16_t)(at.x - a->vis.x), (int16_t)(at.y - a->vis.y),
                     (int16_t)a->w, (int16_t)a->h);
    s_vis = a->vis;

    s_px = malloc((size_t)a->w * a->h * sizeof(ngl_color_t));
    if (!s_px) {
        return false;
    }
    s_surf = ngl_surface_wrap(s_px, (int16_t)a->w, (int16_t)a->h, (int16_t)a->w);
    if (!s_surf) {
        free(s_px);
        s_px = NULL;
        return false;
    }

    memset(s_lit, 0, sizeof(s_lit));
    s_all = true;
    npg_draw_update();
    return true;
}

void npg_draw_invalidate(void)
{
    s_all = true;
}

void npg_draw_update(void)
{
    if (!s_px) {
        return;
    }

    const ngl_rect_t whole = { 0, 0, (int16_t)s_a->w, (int16_t)s_a->h };

    if (s_all) {
        for (int i = 0; i < s_a->nseg; i++) {
            s_lit[i] = sm5a_segment(i);
        }
        memcpy(s_px, s_a->bg, (size_t)s_a->w * s_a->h * sizeof(ngl_color_t));
        blend_all_in(&whole);
        push(&whole);
        ngl_flush();
        s_all = false;
        return;
    }

    /*
     * Collect first, then repaint. The lit array has to be fully up to date
     * before the first rectangle is composed, because composing one draws
     * every segment that overlaps it - including ones later in this same
     * frame's change list.
     */
    int changed[SM5A_SEGMENTS];
    int n = 0;

    for (int i = 0; i < s_a->nseg; i++) {
        const bool now = sm5a_segment(i);
        if (now != s_lit[i]) {
            s_lit[i] = now;
            if (s_a->seg[i].w && s_a->seg[i].h) {
                changed[n++] = i;
            }
        }
    }
    if (n == 0) {
        return;      /* the usual answer, and the reason this is cheap */
    }

    for (int k = 0; k < n; k++) {
        const npg_seg_t *sg = &s_a->seg[changed[k]];
        const ngl_rect_t r = { (int16_t)sg->x, (int16_t)sg->y,
                               (int16_t)sg->w, (int16_t)sg->h };
        restore(&r);
        blend_all_in(&r);
        push(&r);
    }
    ngl_flush();
}

void npg_draw_end(void)
{
    if (s_surf) {
        ngl_surface_free(s_surf);
        s_surf = NULL;
    }
    free(s_px);
    s_px = NULL;
    s_a = NULL;
}
