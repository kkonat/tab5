/*
 * The LED face: seven segments, glowing, in green or red.
 *
 * One paint function and two rows in the table. Green and red LED clocks are
 * the same object with a different die in it, and making them two faces here
 * would have been two copies of this file that drifted apart.
 *
 * The only face with no date and no weather, which is the point of it rather
 * than an omission: a seven-segment display cannot spell Wednesday, and a
 * clock radio that could would not be one. Everything else this app draws
 * carries the calendar; this is the one you can read at four in the morning
 * without learning anything you did not ask for.
 *
 * The glow is three passes of the same digit at three bleeds - see the `bleed`
 * field in clock.h. Biggest and dimmest first, so each ring is drawn over by
 * the next and only its outer margin survives; the result is a halo whose
 * falloff comes from how much of each pass is left rather than from any
 * blending, which ngl has no cheap way to do.
 */
#include <stdio.h>

#include "ngl.h"

#include "clock.h"

#define MARGIN     40
#define SEC_GAP    26

/*
 * Two materials, which is the whole trick.
 *
 * The front of one of these is not one surface. There is a dark mask - opaque
 * plastic, near black, slightly warm - with segment-shaped windows cut in it,
 * and behind each window a piece of tinted transparent plastic over the die.
 * With the die off you still see that window: it is a different material from
 * the mask around it, so it reads as a dark shape of the display's own colour
 * rather than as a dim version of the lit segment.
 *
 * Getting that wrong is what makes a drawn seven-segment display look drawn.
 * A dead segment mixed from the lit colour is just a faint lit segment, and
 * the eye reads it as a digit that is on but broken; a dead segment in its own
 * dark tint reads as plastic.
 */
#define LED_MASK   NGL_RGB(11, 10, 10)    /* the opaque surround */

/* The two halo passes: how far each reaches as a fraction of segment
   thickness, and how much of the lit colour survives in it. */
#define HALO1_BLEED_NUM 5
#define HALO1_BLEED_DEN 4
#define HALO1_MIX  42
#define HALO2_BLEED_NUM 1
#define HALO2_BLEED_DEN 2
#define HALO2_MIX  110

/* A third, tight and hot, right against the segment: what makes it read as
   emitting rather than as painted. */
#define HALO3_BLEED_NUM 1
#define HALO3_BLEED_DEN 5
#define HALO3_MIX  190

typedef struct {
    ngl_color_t on;    /**< the die, lit */
    ngl_color_t off;   /**< the tinted window over it, dark */
} led_cfg_t;

/*
 * The dark tints are not the lit colours scaled down. A green display's dead
 * segments are a dark olive and a red one's a dark maroon - the filter is
 * doing the colouring, not the emitter, so both sit much closer to the mask
 * in brightness and much further from it in hue than a simple dim would.
 */
static const led_cfg_t LED_GREEN = {
    NGL_RGB(80, 255, 105),
    NGL_RGB(26, 42, 24),
};
static const led_cfg_t LED_RED = {
    NGL_RGB(255, 70, 45),
    NGL_RGB(46, 20, 18),
};

static struct {
    ngl_rect_t  band, hhmm, ss;
    clk_seg7_t  big, small;
    int16_t     dgap, sgap;    /* between digits, each size */
} L;

/* ------------------------------------------------------------------ */

/**
 * Digit size, from whichever of the two dimensions runs out first.
 *
 * Across, a "HH:MM" is four digits, one colon and five gaps; the colon is a
 * quarter of a digit and a gap a tenth, so the row is 4.75 digit widths. Down,
 * a seven-segment digit is about 1.9 times its width, and the seconds and
 * their gap come out of the same budget.
 */
static void layout(const clock_frame_t *f)
{
    const int16_t avail_w = (int16_t)(f->area.w - 2 * MARGIN);
    const int16_t avail_h = (int16_t)(f->area.h - 2 * MARGIN);

    int16_t w = (int16_t)((avail_w * 100) / 475);
    int16_t h = (int16_t)((w * 19) / 10);

    /* The seconds are 45% of the big digits, so the column is
       h + SEC_GAP + 0.45h*1.0, and 1.45h + gap must fit. */
    const int16_t need = (int16_t)((h * 145) / 100 + SEC_GAP);
    if (need > avail_h) {
        h = (int16_t)(((avail_h - SEC_GAP) * 100) / 145);
        w = (int16_t)((h * 10) / 19);
    }
    if (w < 12) { w = 12; }
    if (h < 24) { h = 24; }

    clk_seg7_fit(&L.big, w, h, 0);
    L.dgap = (int16_t)(w / 10);

    const int16_t sw = (int16_t)((w * 45) / 100);
    clk_seg7_fit(&L.small, sw, (int16_t)((sw * 19) / 10), 0);
    L.sgap = (int16_t)(sw / 8);

    const int16_t big_w = (int16_t)(4 * w + clk_seg7_colon_w(&L.big) + 5 * L.dgap);
    const int16_t sec_w = (int16_t)(2 * sw + L.sgap);
    const int16_t inner = (int16_t)(h + SEC_GAP + L.small.h);

    clk_stack(f->area, inner, 0, 0, &L.band, 0);

    L.hhmm = ngl_rect((int16_t)(L.band.x + (L.band.w - big_w) / 2), L.band.y,
                      big_w, h);
    L.ss   = ngl_rect((int16_t)(L.band.x + (L.band.w - sec_w) / 2),
                      (int16_t)(L.band.y + h + SEC_GAP), sec_w, L.small.h);
}

/** How far the widest halo pass reaches past the segment it came from. */
static int16_t halo_reach(const clk_seg7_t *g)
{
    return (int16_t)(g->thick * HALO1_BLEED_NUM / HALO1_BLEED_DEN + 1);
}

/**
 * One digit and its halo. @p g is copied, because the bleed is written to it.
 *
 * Widest and dimmest first, so each pass is drawn over by the next and only
 * its outer margin survives. The falloff is therefore how much of each ring is
 * left rather than any blending, which ngl has no cheap way to do - and three
 * rings rather than two is what takes it from a fringe to a glow.
 *
 * Every pass is over the mask, not over the dead segments: the halo of a lit
 * segment lands on its neighbours' dark windows too, which is exactly what it
 * does on the real thing and is most of why one looks like it is glowing.
 */
static void glow_digit(ngl_surface_t *s, int16_t x, int16_t y, int d,
                       const clk_seg7_t *g, ngl_color_t on, ngl_color_t off)
{
    clk_seg7_t t = *g;

    t.bleed = (int16_t)(g->thick * HALO1_BLEED_NUM / HALO1_BLEED_DEN);
    clk_seg7_lit(s, x, y, d, &t, clk_mix(LED_MASK, on, HALO1_MIX));

    t.bleed = (int16_t)(g->thick * HALO2_BLEED_NUM / HALO2_BLEED_DEN);
    clk_seg7_lit(s, x, y, d, &t, clk_mix(LED_MASK, on, HALO2_MIX));

    t.bleed = (int16_t)(g->thick * HALO3_BLEED_NUM / HALO3_BLEED_DEN);
    clk_seg7_lit(s, x, y, d, &t, clk_mix(LED_MASK, on, HALO3_MIX));

    t.bleed = 0;
    clk_seg7_digit(s, x, y, d, &t, on, off);
}

/**
 * Two digits of a number, with the dead segments behind them.
 *
 * The dead ones are painted for the whole pair before any halo, so that a
 * neighbour's glow lands on top of them rather than under - which is the way
 * round it looks on a real display, where the bloom is in the diffuser above
 * everything and not in the glass beside it.
 */
static void pair(ngl_surface_t *s, int16_t x, int16_t y, int v, bool blank,
                 const clk_seg7_t *g, int16_t gap, ngl_color_t on, ngl_color_t off)
{
    const int16_t adv = (int16_t)(g->w + gap);

    for (int i = 0; i < 2; i++) {
        clk_seg7_digit(s, (int16_t)(x + i * adv), y, -1, g, off, off);
    }
    for (int i = 0; i < 2; i++) {
        const int d = blank ? -1 : (i == 0 ? v / 10 : v % 10);
        glow_digit(s, (int16_t)(x + i * adv), y, d, g, on, off);
    }
}

/* What has to be erased before a repaint: the digit box plus however far the
   widest halo pass reaches, or last second's bloom is still standing around
   this second's digits. */
static ngl_rect_t halo_box(ngl_rect_t r, const clk_seg7_t *g)
{
    return clk_inflate(r, halo_reach(g));
}

static void paint_time(ngl_surface_t *s, const clock_frame_t *f,
                       const led_cfg_t *led)
{
    const ngl_color_t on = led->on, off = led->off;

    ngl_fill_rect(s, halo_box(L.hhmm, &L.big), LED_MASK);

    const int16_t adv = (int16_t)(L.big.w + L.dgap);
    int16_t x = L.hhmm.x;

    pair(s, x, L.hhmm.y, f->t.hour, !f->have_time, &L.big, L.dgap, on, off);
    x = (int16_t)(x + 2 * adv);

    /*
     * The colon blinks on the half second, which is the one thing about a
     * clock radio everybody remembers. It is driven off the seconds field
     * rather than a timer so that it cannot drift out of step with the digits
     * beside it.
     */
    clk_seg7_colon(s, x, L.hhmm.y, &L.big, off);
    if (!f->have_time || (f->t.sec & 1) == 0) {
        clk_seg7_t t = L.big;
        t.bleed = (int16_t)(L.big.thick * HALO2_BLEED_NUM / HALO2_BLEED_DEN);
        clk_seg7_colon(s, x, L.hhmm.y, &t, clk_mix(LED_MASK, on, HALO2_MIX));
        clk_seg7_colon(s, x, L.hhmm.y, &L.big, on);
    }
    x = (int16_t)(x + clk_seg7_colon_w(&L.big) + L.dgap);

    pair(s, x, L.hhmm.y, f->t.min, !f->have_time, &L.big, L.dgap, on, off);
}

static void paint_secs(ngl_surface_t *s, const clock_frame_t *f,
                       const led_cfg_t *led)
{
    ngl_fill_rect(s, halo_box(L.ss, &L.small), LED_MASK);
    /* A shade down from the hours: on the real thing the seconds are a smaller
       part with the same die behind it, so it is the area that differs and not
       the colour - but a little less of it reads as further away. */
    pair(s, L.ss.x, L.ss.y, f->t.sec, !f->have_time, &L.small, L.sgap,
         clk_mix(LED_MASK, led->on, 205), led->off);
}

static void paint(const clock_frame_t *f, const void *cfg)
{
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }
    const led_cfg_t *led = (const led_cfg_t *)cfg;

    if (f->full) {
        ngl_clear(s, LED_MASK);
        layout(f);
    }
    if (f->full || f->sec) {
        /* Both, every second: the colon lives in the HH:MM block and blinks
           with the seconds, so there is no cheaper answer than this. */
        paint_time(s, f, led);
        paint_secs(s, f, led);
        ngl_flush();
    }
}

const clock_face_t clock_face_led_green = {
    .key      = "led-green",
    .name     = "LED green",
    .blurb    = "seven segment, glowing",
    .paint    = paint,
    .leave    = 0,
    .cfg      = &LED_GREEN,
    /* Nothing moves between seconds, and the shell wakes on the tick anyway. */
    .frame_ms = 120,
};

const clock_face_t clock_face_led_red = {
    .key      = "led-red",
    .name     = "LED red",
    .blurb    = "seven segment, glowing",
    .paint    = paint,
    .leave    = 0,
    .cfg      = &LED_RED,
    .frame_ms = 120,
};
