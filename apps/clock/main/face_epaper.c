/*
 * The e-paper face: black on white, and slow about it.
 *
 * The only face here that is defined by what its hardware is bad at. Electronic
 * ink holds a picture with no power at all and pays for it at every update: a
 * particle has to be dragged from one side of a capsule to the other, which
 * takes the better part of a second, and doing it cleanly means driving the
 * whole panel to black and back first. So an e-paper clock does not show
 * seconds - nobody builds one that spends a second of flashing per second of
 * time - and when the minute turns, the whole thing blinks.
 *
 * Both of those are drawn here rather than skipped, because they are the face:
 *
 *   refresh() is the inversion sequence, black, white, black, content, with
 *   real pauses in it. That costs about a third of a second once a minute and
 *   is deliberately not hidden. Nothing is lost by blocking in it - NeOS
 *   queues the tap that arrives during one and the shell collects it on the
 *   next frame, so the face stays responsive without having to be a state
 *   machine about it.
 *
 *   the ghost is what a partial update leaves behind. The previous digits are
 *   painted very faintly under the new ones, so a 3 that has just become a 4
 *   leaves the corner of the 3 showing - which is the artefact anybody who has
 *   owned one of these will recognise, and the reason the flash exists.
 *
 * There is no black here and no white either. Ink is a dark warm grey and the
 * substrate is bone; a real panel has nowhere near the contrast of a screen,
 * and drawing it at full range makes it look like a monochrome LCD instead.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"

#include "clock.h"

#define MARGIN     40
#define STRIP_H    76
#define STACK_GAP  34

/*
 * How coarse the strip is relative to the time.
 *
 * The panel has one resolution. Drawing 200-pixel digits out of blocks and
 * then a crisp caption underneath them in a 16-pixel typeface would be two
 * devices in one frame - so the strip is the small font magnified too, and
 * only the magnification differs. A third of the time's scale keeps the line
 * short enough to fit while leaving its pixels plainly visible.
 */
#define STRIP_SCALE_NUM 1
#define STRIP_SCALE_DEN 3

#define PAPER      NGL_RGB(238, 236, 230)
#define INK        NGL_RGB(32, 30, 34)

/* How much ink is left where the last minute's digits were. */
#define GHOST_MIX  22

/* The inversion, in milliseconds. Long enough to see, short enough that the
   clock is not visibly wrong while it happens. */
#define FLASH_MS   110
#define SETTLE_MS  70

static struct {
    ngl_rect_t band, strip;
    int16_t    scale, sscale;
    int16_t    tx, ty, tw, th;
    char       showing[8];    /* what is on the panel now, for the ghost */
} L;

static void layout(const clock_frame_t *f)
{
    const ngl_font_t *fnt = &ngl_font_large;

    const int16_t avail_w = (int16_t)(f->area.w - 2 * MARGIN);
    const int16_t avail_h = (int16_t)((f->area.h - STRIP_H - STACK_GAP) * 3 / 5);

    const int16_t by_w = (int16_t)(avail_w / (5 * fnt->width));
    const int16_t by_h = (int16_t)(avail_h / fnt->height);
    L.scale = by_w < by_h ? by_w : by_h;
    if (L.scale < 1) {
        L.scale = 1;
    }

    /* The strip's grid, from the time's, with a floor so it never becomes a
       1:1 font again and quietly stops matching the rest of the panel. */
    L.sscale = (int16_t)(L.scale * STRIP_SCALE_NUM / STRIP_SCALE_DEN);
    if (L.sscale < 2) {
        L.sscale = 2;
    }
    /* And a ceiling, so the date still fits across: the longest line this
       draws is about 26 cells of the small font. */
    while (L.sscale > 2 &&
           (int16_t)(26 * ngl_font_small.width * L.sscale) > avail_w) {
        L.sscale--;
    }

    L.tw = clk_blocky_w(fnt, "00:00", L.scale);
    L.th = (int16_t)(fnt->height * L.scale);

    clk_stack(f->area, L.th, STRIP_H, STACK_GAP, &L.band, &L.strip);
    L.tx = (int16_t)(L.band.x + (L.band.w - L.tw) / 2);
    L.ty = L.band.y;
}

static void time_string(const clock_frame_t *f, char *buf, size_t n)
{
    if (f->have_time) {
        snprintf(buf, n, "%02d:%02d", f->t.hour, f->t.min);
    } else {
        snprintf(buf, n, "--:--");
    }
}

/** The finished picture: ghost of the old time, then the new one, then the strip. */
static void content(ngl_surface_t *s, const clock_frame_t *f, const char *now,
                    const char *before)
{
    ngl_fill_rect(s, f->area, PAPER);

    if (before && before[0] && strcmp(before, now) != 0) {
        clk_blocky(s, L.tx, L.ty, before, &ngl_font_large, L.scale,
                   clk_mix(PAPER, INK, GHOST_MIX));
    }
    clk_blocky(s, L.tx, L.ty, now, &ngl_font_large, L.scale, INK);

    clk_wx_strip_blocky(s, L.strip, f, &ngl_font_small, L.sscale, INK, PAPER);
}

/**
 * Drive the panel through an update.
 *
 * Two full inversions and then the picture, which is what a real controller
 * does on a full refresh and is the only way it clears the ghost of everything
 * before this minute. Each phase is flushed and then waited on - the wait is
 * the point, so it is not something to tune away.
 */
static void refresh(ngl_surface_t *s, const clock_frame_t *f, const char *now,
                    const char *before)
{
    ngl_fill_rect(s, f->area, INK);
    ngl_flush();
    neos_sleep_ms(FLASH_MS);

    ngl_fill_rect(s, f->area, PAPER);
    ngl_flush();
    neos_sleep_ms(SETTLE_MS);

    ngl_fill_rect(s, f->area, INK);
    ngl_flush();
    neos_sleep_ms(SETTLE_MS);

    content(s, f, now, before);
    ngl_flush();
}

static void paint(const clock_frame_t *f, const void *cfg)
{
    (void)cfg;
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }

    char now[8];
    time_string(f, now, sizeof(now));

    if (f->full) {
        ngl_clear(s, PAPER);
        layout(f);
        /*
         * No flash on a full repaint. The face has just been switched to or
         * the screen has just rotated, and neither is a minute turning - a
         * panel that blinked every time it was looked at would be one nobody
         * would put on a shelf.
         */
        content(s, f, now, 0);
        ngl_flush();
        snprintf(L.showing, sizeof(L.showing), "%s", now);
        return;
    }

    /*
     * Driven off the string rather than off f->min, so that the panel and what
     * it is showing cannot disagree: if a refresh were ever missed - a rotation
     * landing on the same tick, say - this notices on the next frame instead of
     * waiting a minute for the next edge.
     */
    if (strcmp(now, L.showing) != 0) {
        char before[8];
        snprintf(before, sizeof(before), "%s", L.showing);
        snprintf(L.showing, sizeof(L.showing), "%s", now);
        refresh(s, f, now, before);
    }
}

const clock_face_t clock_face_epaper = {
    .key   = "epaper",
    .name  = "E-paper",
    .blurb = "black on white, and slow about it",
    .paint = paint,
    .leave = 0,
    .cfg   = 0,
    /*
     * Nothing moves between minutes, so this is only how quickly a tap is
     * noticed and how soon after the minute turns the refresh starts. A
     * quarter of a second is under what the flash itself takes.
     */
    .frame_ms = 250,
};
