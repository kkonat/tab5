/*
 * The buttons, which on the real thing are the case.
 *
 * Four rubber pads at the corners for the wolf, three switches down the side
 * for the mode, and a reset pad you needed a pen for. This is all of that on
 * glass, drawn two ways.
 *
 *   NeOS         phosphor green on black, the same chips every other app on
 *                this tablet uses. The emulator is a program among programs.
 *   Elektronika  dull cream plastic, moulded seats and dark rubber, with the
 *                display sunk into the case. The emulator is the object it is
 *                emulating.
 *
 * Neither is the "real" one. The first is honest about what this is and the
 * second is honest about what it was, and which of those you want depends
 * entirely on whether you are showing somebody the tablet or the game - so
 * STYLE switches between them and the choice survives a reboot.
 *
 * The layout is the same either way. Only the paint differs, which is what
 * keeps a second style from being a second app: everything below the drawing
 * functions knows about rectangles and nothing else.
 *
 * Everything here is held, not tapped. neos_touch_tap() is the wrong shape
 * for a joystick - it reports a press and release that stayed put, which is
 * exactly the event this game does not care about - so the buttons are read
 * from the raw contact list and the ROM does its own edge detection, the same
 * as it did off the real pads.
 */
#include <stdio.h>
#include <string.h>

#include "nupogodi.h"
#include "ngl_theme.h"

#define RADIUS   10
#define GAP      10

/* ------------------------------------------------------------------ */
/* The Elektronika palette                                             */
/* ------------------------------------------------------------------ */

/*
 * The case of one of these is not gold and never was.
 *
 * It is a pale warm plastic - off-white when it left the factory in 1989 and
 * a dirty cream by the time anyone reading this has held one - and the point
 * of getting that right is that plastic is dull. Metal has a highlight; this
 * has a slight sheen at the top and a slight shadow at the bottom and nothing
 * else, which is what the gradient is for. It is cheap: only ever repainted
 * when something already forced a full repaint.
 */
#define EL_CASE_TOP    NGL_RGB(212, 206, 192)
#define EL_CASE_BOT    NGL_RGB(186, 179, 163)

/*
 * A button sits in the case, not on it.
 *
 * There is no ring: what goes round a button is a hole in the same plastic,
 * which is a shadow with a lit edge at the bottom of it and nothing else.
 * These are percentages of the case colour of that row rather than colours of
 * their own, so the seat never reads as a separate part painted on top.
 */
#define EL_SEAT_DARK   70          /* the wall of the hole */
#define EL_SEAT_LIT    120         /* the far side of it, catching the light */

/* The rubber pads, the plastic switches, and the shine off both. */
#define EL_RUBBER      NGL_RGB(58,  56,  53)
#define EL_RUBBER_HOT  NGL_RGB(98,  94,  88)
#define EL_PILL        NGL_RGB(163, 158, 148)
#define EL_PILL_HOT    NGL_RGB(202, 197, 186)
#define EL_SHEEN       NGL_RGB(102,  99,  94)
#define EL_SHEEN_HOT   NGL_RGB(140, 136, 130)

/* Screen printing on the case. */
#define EL_INK         NGL_RGB(38,  32,  26)

/*
 * How wide the recess around the glass would like to be.
 *
 * It lives in the space the asset's black border gave up, and how much that
 * is depends on the artwork - so this is what to draw when there is room, and
 * npg_panel_layout() works out how much there actually is. An asset generated
 * to the size tools/genlcd aims at leaves enough; one built for an older
 * layout gets a narrower recess rather than one drawn over the system bar.
 */
#define BEZEL 14

/* ------------------------------------------------------------------ */

/* The mode switches, in the order they are laid out left to right. */
enum { CH_GAME_A = 0, CH_GAME_B, CH_TIME, CH_ALARM, CH_ACL, CH_LIVES,
       CH_STYLE, CH_N };

static const char *const CH_TXT[CH_N] = {
    "GAME A", "GAME B", "TIME", "ALARM", "ACL", "LIVES", "STYLE"
};

/* Which key each chip presses. The last three are not keys at all. */
static const int CH_KEY[CH_N] = {
    NPG_KEY_GAME_A, NPG_KEY_GAME_B, NPG_KEY_TIME, NPG_KEY_ALARM, -1, -1, -1
};

static ngl_rect_t s_area;
static ngl_rect_t s_art;           /* the glass, which the bezel goes round */
static int16_t    s_bezel;         /* how much of BEZEL fits around it */
static ngl_rect_t s_dir[4];        /* indexed by npg_key_t, 0..3 */
static ngl_rect_t s_chip[CH_N];

static bool s_down[NPG_KEY_N];
static bool s_acl;
static bool s_lives;               /* latched, not a key: see npg_panel_cheat */
static uint8_t s_style;

/* What is currently on the screen, so a repaint only happens on a change. */
static bool s_shown_dir[4];
static bool s_shown_chip[CH_N];
static bool s_laid_out;

#define STYLE_FILE "style"

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

void npg_panel_layout(ngl_rect_t area, ngl_rect_t art)
{
    s_area = area;
    s_art  = art;

    /*
     * The pads take the margin the artwork's own shape left over, right out
     * to the edge of the glass on both sides - flush, with no border on the
     * outside, because that edge is a real one and a thumb can find it
     * without looking. The gap is on the inside only, against the artwork.
     */
    /*
     * How much recess there is room for, before anything is placed against
     * it. Vertically it is what the artwork left inside its box, which on an
     * asset built for a taller layout can be very little; horizontally there
     * is always plenty, and the pads are moved out of the way below.
     */
    const int16_t above = (int16_t)(art.y - area.y);
    const int16_t below = (int16_t)(area.y + area.h - NPG_STRIP_H
                                    - (art.y + art.h));
    s_bezel = BEZEL;
    if (s_bezel > above) {
        s_bezel = above;
    }
    if (s_bezel > below) {
        s_bezel = below;
    }
    if (s_bezel < 0) {
        s_bezel = 0;
    }

    int16_t pw = (int16_t)(art.x - area.x - GAP - s_bezel);
    const int16_t rmargin = (int16_t)(area.x + area.w - (art.x + art.w)
                                      - GAP - s_bezel);
    if (rmargin < pw) {
        pw = rmargin;
    }
    if (pw > NPG_PAD_W) {
        pw = NPG_PAD_W;
    }

    int16_t ph = (int16_t)((art.h - GAP) / 2);
    if (ph > NPG_PAD_H) {
        ph = NPG_PAD_H;
    }

    const int16_t lx  = area.x;
    const int16_t rx  = (int16_t)(area.x + area.w - pw);
    const int16_t top = (int16_t)(art.y + (art.h - (2 * ph + GAP)) / 2);

    s_dir[NPG_KEY_LEFT_UP]    = ngl_rect(lx, top, pw, ph);
    s_dir[NPG_KEY_LEFT_DOWN]  = ngl_rect(lx, (int16_t)(top + ph + GAP), pw, ph);
    s_dir[NPG_KEY_RIGHT_UP]   = ngl_rect(rx, top, pw, ph);
    s_dir[NPG_KEY_RIGHT_DOWN] = ngl_rect(rx, (int16_t)(top + ph + GAP), pw, ph);

    /*
     * The mode switches share the strip evenly. Even rather than sized to
     * their text: they are pressed by feel while looking at the screen, and a
     * row of equal targets is easier to hit than a row of ragged ones.
     */
    const int16_t sy = (int16_t)(area.y + area.h - NPG_STRIP_H
                                 + (NPG_STRIP_H - NPG_CHIP_H) / 2);
    const int16_t sw = (int16_t)((area.w - (CH_N + 1) * GAP) / CH_N);

    for (int i = 0; i < CH_N; i++) {
        s_chip[i] = ngl_rect((int16_t)(area.x + GAP + i * (sw + GAP)), sy,
                             sw, NPG_CHIP_H);
    }

    memset(s_shown_dir, 0, sizeof(s_shown_dir));
    memset(s_shown_chip, 0, sizeof(s_shown_chip));
    s_laid_out = true;
}

/*
 * One channel of the gradient at `t` of `h`, dithered.
 *
 * A pale gradient down a whole screen is the one thing RGB565 is bad at:
 * five bits of red over six hundred rows is four flat bands with a visible
 * step between each, and the eye finds a step in a flat area of plastic
 * instantly. So the value is computed in quarters and the fraction is spent
 * on an ordered dither down the rows - a row lands on the next value up if
 * its quarter beats the threshold for its position in a four-row cycle.
 *
 * The result is a few rows of 1px interleave where a hard edge used to be,
 * which at this pitch reads as the smooth ramp it is standing in for. It
 * costs nothing: the row is still one colour and still one hline.
 */
static int ramp(int a, int b, int t, int h, int thr)
{
    const int q = a * 4 + (b - a) * 4 * t / h;
    return (q >> 2) + (((q & 3) > thr) ? 1 : 0);
}

/*
 * The case colour at an absolute screen row.
 *
 * A function of the row and not of whatever rectangle is being painted,
 * because a button that repaints itself has to put the case back first and
 * the gradient has to line up with the rest of it when it does - which is
 * also why the dither is keyed off the absolute row and not a local one.
 *
 * Interpolated in RGB565's own components, so there is no rounding back and
 * forth through 8 bits per channel.
 */
static ngl_color_t case_at(int16_t y)
{
    /* Bayer, four deep: the order that spreads four thresholds as evenly as
       four rows can be spread. In sequence they would be a visible stripe. */
    static const int BAYER[4] = { 0, 2, 1, 3 };

    const int r0 = (EL_CASE_TOP >> 11) & 0x1f, r1 = (EL_CASE_BOT >> 11) & 0x1f;
    const int g0 = (EL_CASE_TOP >> 5)  & 0x3f, g1 = (EL_CASE_BOT >> 5)  & 0x3f;
    const int b0 =  EL_CASE_TOP        & 0x1f, b1 =  EL_CASE_BOT        & 0x1f;
    const int h  = s_area.h > 0 ? s_area.h : 1;

    int t = y - s_area.y;
    if (t < 0) {
        t = 0;
    } else if (t > h) {
        t = h;
    }

    const int thr = BAYER[(unsigned)y & 3];
    return (ngl_color_t)((ramp(r0, r1, t, h, thr) << 11) |
                         (ramp(g0, g1, t, h, thr) << 5)  |
                          ramp(b0, b1, t, h, thr));
}

/*
 * A colour taken up or down, in percent.
 *
 * Under 100 is toward black and over 100 is toward white, which is the whole
 * of the lighting model here: every shadow and every highlight in this style
 * is the case colour of that row moved one way or the other, so nothing can
 * drift out of the palette and a change to the case colour carries into all
 * of them.
 */
static ngl_color_t shade(ngl_color_t c, int pct)
{
    int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;

    if (pct <= 100) {
        r = r * pct / 100;
        g = g * pct / 100;
        b = b * pct / 100;
    } else {
        r += (31 - r) * (pct - 100) / 100;
        g += (63 - g) * (pct - 100) / 100;
        b += (31 - b) * (pct - 100) / 100;
    }
    return (ngl_color_t)((r << 11) | (g << 5) | b);
}

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

/*
 * A solid triangle out of horizontal lines.
 *
 * The corner pads on the case are moulded arrows, and an arrow says which way
 * the wolf goes in a way that "UL" does not. ngl has no filled polygon and
 * does not need one for this: a triangle is a run of hlines whose width falls
 * off linearly, and at this size the stair-stepping on the diagonals reads as
 * the moulding it is imitating.
 */
static void triangle(ngl_surface_t *sc, ngl_rect_t r, bool up, ngl_color_t c)
{
    const int16_t cx = (int16_t)(r.x + r.w / 2);
    for (int i = 0; i < r.h; i++) {
        /* Full width at the base, a point at the apex. */
        const int t = up ? i : r.h - 1 - i;
        const int16_t half = (int16_t)((int32_t)r.w * t / (2 * r.h));
        ngl_hline(sc, (int16_t)(cx - half), (int16_t)(r.y + i),
                  (int16_t)(2 * half + 1), c);
    }
}

/** A circle, which ngl draws as a rounded rect whose corners have met. */
static void disc(ngl_surface_t *sc, int16_t cx, int16_t cy, int16_t d,
                 ngl_color_t c)
{
    const ngl_rect_t r = ngl_rect((int16_t)(cx - d / 2), (int16_t)(cy - d / 2), d, d);
    ngl_fill_round_rect(sc, r, (int16_t)(d / 2), c);
}

/*
 * One rubber button in the hole it sits in.
 *
 * Four discs, each one leaving a crescent of the one before it.
 *
 * The lit disc is first and sits low, so what survives of it is the arc along
 * the bottom of the hole where the light reaches the far wall. The shadowed
 * wall goes over that. Then the sheen, up and to the left, where the light
 * has been coming from everywhere else in this app - and then the rubber over
 * *that*, offset the other way, so the sheen ends up as a rim on the lit side
 * of the button rather than a dot in the middle of it. Rubber that has been
 * thumbed for thirty years has no highlight; it has an edge that catches.
 */
static void rubber(ngl_surface_t *sc, int16_t cx, int16_t cy, int16_t d, bool hot)
{
    const ngl_color_t c = case_at(cy);
    const int16_t seat = (int16_t)(d + d / 8);
    const int16_t o = (int16_t)(d / 32 > 2 ? d / 32 : 2);

    disc(sc, cx, (int16_t)(cy + d / 16), seat, shade(c, EL_SEAT_LIT));
    disc(sc, cx, cy, seat, shade(c, EL_SEAT_DARK));
    disc(sc, cx, cy, d, hot ? EL_SHEEN_HOT : EL_SHEEN);
    disc(sc, (int16_t)(cx + o), (int16_t)(cy + o), (int16_t)(d - o),
         hot ? EL_RUBBER_HOT : EL_RUBBER);
}

/*
 * One moulded switch: a lozenge of the same plastic, standing proud.
 *
 * The opposite of the bezel below - light along the top, shadow underneath -
 * because this one sticks out of the case rather than being sunk into it, and
 * those two facts are the same fact seen from either side.
 */
static void pill(ngl_surface_t *sc, ngl_rect_t r, bool hot)
{
    const int16_t rad = (int16_t)(r.h / 2);
    const ngl_color_t body = hot ? EL_PILL_HOT : EL_PILL;

    ngl_fill_round_rect(sc, ngl_rect(r.x, (int16_t)(r.y + 3), r.w, r.h), rad,
                        shade(case_at((int16_t)(r.y + r.h)), 72));
    ngl_fill_round_rect(sc, r, rad, body);
    ngl_draw_round_rect(sc, r, rad, shade(body, 76), 1);

    /* The light on the top face, inset so it does not touch the edge - the
       moulding is round there and a highlight running into it looks flat. */
    ngl_fill_round_rect(sc, ngl_rect((int16_t)(r.x + rad), (int16_t)(r.y + 4),
                                     (int16_t)(r.w - 2 * rad),
                                     (int16_t)(r.h / 5 > 3 ? r.h / 5 : 3)),
                        2, shade(body, 118));
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/*
 * The case, behind everything else.
 *
 * Taken as a rectangle rather than as "the whole area" because a button that
 * repaints itself has to put the case back first, and the gradient has to
 * line up with the rest of it when it does - so the colour is a function of
 * the absolute row, not of where this particular rectangle starts.
 */
static void ground_rect(ngl_surface_t *sc, ngl_rect_t r)
{
    if (s_style == NPG_STYLE_NEOS) {
        ngl_fill_rect(sc, r, TH_BG);
        return;
    }

    for (int y = r.y; y < r.y + r.h; y++) {
        ngl_hline(sc, r.x, (int16_t)y, r.w, case_at((int16_t)y));
    }
}

void npg_panel_ground(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (sc && s_laid_out) {
        ground_rect(sc, s_area);
    }
}

/** `r` grown by `n` on every side. */
static ngl_rect_t grow(ngl_rect_t r, int16_t n)
{
    return ngl_rect((int16_t)(r.x - n), (int16_t)(r.y - n),
                    (int16_t)(r.w + 2 * n), (int16_t)(r.h + 2 * n));
}

/*
 * One rectangle of the recess: dark along the top and left, light along the
 * bottom and right.
 *
 * The sides are drawn a row at a time rather than as two vlines because the
 * case is a gradient - a side wall in one flat colour would be a seam down
 * each edge of the display, which is exactly the join this is here to hide.
 * The corners fall to the horizontal runs, so the top corners are shadow and
 * the bottom ones are light, which is what a square hole does.
 */
static void recess_ring(ngl_surface_t *sc, ngl_rect_t r, int dark, int lite)
{
    const int16_t y0 = r.y, y1 = (int16_t)(r.y + r.h - 1);
    const int16_t x1 = (int16_t)(r.x + r.w - 1);

    ngl_hline(sc, r.x, y0, r.w, shade(case_at(y0), dark));
    ngl_hline(sc, r.x, y1, r.w, shade(case_at(y1), lite));

    for (int16_t y = (int16_t)(y0 + 1); y < y1; y++) {
        const ngl_color_t c = case_at(y);
        ngl_pixel(sc, r.x, y, shade(c, dark));
        ngl_pixel(sc, x1, y, shade(c, lite));
    }
}

/*
 * The display, sunk into the case.
 *
 * Two things in one: the wall of the recess, which is the outer three fifths
 * and is where the light and shadow are, and then its floor - a flat step of
 * plastic - and finally a dark lip at the bottom of it for the glass to sit
 * behind. Drawn outwards in, so each ring lands inside the one before it.
 *
 * The whole thing lives in the space the artwork's black border gave up, so
 * nothing here costs the picture a pixel. See npg_asset_t::margin.
 */
void npg_panel_frame(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc || !s_laid_out) {
        return;
    }

    if (s_style == NPG_STYLE_NEOS) {
        /* The same hairline every other app on this tablet puts round a
           panel. The case is already black; anything more would be a frame
           for its own sake. */
        ngl_draw_rect(sc, grow(s_art, 2), TH_EDGE, 2);
        return;
    }

    const int16_t n = s_bezel;
    if (n < 3) {
        return;                             /* not enough room to say anything */
    }

    /* The lip is two rows of the wall, not two rows on top of it, so a narrow
       recess loses depth rather than losing the edge the glass sits behind. */
    const int wall = n * 3 / 5 > 1 ? n * 3 / 5 : 1;

    for (int d = 1; d <= n; d++) {
        int dark, lite;

        if (d > n - 2) {
            dark = lite = 34;               /* the lip against the glass */
        } else if (d <= wall) {
            dark = 100 - 46 * d / wall;     /* down the wall into shadow */
            lite = 100 + 40 * d / wall;     /* and up the other one into light */
        } else {
            dark = lite = 74;               /* the floor it all sits on */
        }
        recess_ring(sc, grow(s_art, (int16_t)(n + 1 - d)), dark, lite);
    }
}

static void paint_dir(int k)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t r = s_dir[k];
    const bool hot = s_down[k];
    const bool up  = (k == NPG_KEY_LEFT_UP || k == NPG_KEY_RIGHT_UP);

    if (s_style == NPG_STYLE_NEOS) {
        ngl_fill_round_rect(sc, r, RADIUS, hot ? TH_CLOSE_FILL : TH_PANEL);
        ngl_draw_round_rect(sc, r, RADIUS, hot ? TH_ACCENT : TH_EDGE, 2);

        const int16_t tw = (int16_t)(r.w * 6 / 10);
        const int16_t th = (int16_t)(tw * 3 / 4);
        const ngl_rect_t t = ngl_rect((int16_t)(r.x + (r.w - tw) / 2),
                                      (int16_t)(r.y + (r.h - th) / 2), tw, th);
        triangle(sc, t, up, hot ? TH_GLOW : TH_TEXT_DIM);
        return;
    }

    /*
     * A round pad with the arrow printed on the case beside it rather than on
     * the button, which is where it is on the real machine - the rubber is
     * blank and the moulding around it carries the marking.
     *
     * The button takes everything the arrow does not, so it is as big as a
     * pad this wide can be. The two on a side face each other across the
     * middle with their arrows at the outer ends, which is the arrangement on
     * the case and is also the one a thumb resting between them can work.
     */
    ground_rect(sc, r);

    const int16_t room = (int16_t)(r.h - NPG_ARROW_H);
    int16_t d = (int16_t)((r.w < room ? r.w : room) * 88 / 100);
    if (d < 24) {
        d = 24;
    }

    const int16_t cx = (int16_t)(r.x + r.w / 2);
    const int16_t cy = (int16_t)(up ? r.y + r.h - d / 2 : r.y + d / 2);

    /* A fixed size, because a moulded arrow is a fixed size: it says which
       way, and saying it twice as loud on a wider tablet says nothing more. */
    const int16_t th = (int16_t)(NPG_ARROW_H - 16);
    const int16_t tw = (int16_t)(th * 4 / 3);
    const int16_t ty = (int16_t)((up ? r.y : r.y + r.h - NPG_ARROW_H) + 8);

    triangle(sc, ngl_rect((int16_t)(cx - tw / 2), ty, tw, th), up, EL_INK);
    rubber(sc, cx, cy, d, hot);
}

static void paint_chip(int i)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t r = s_chip[i];
    const bool hot = (i == CH_LIVES) ? s_lives
                   : (i == CH_ACL)   ? s_acl
                   : (i == CH_STYLE) ? false
                                     : s_down[CH_KEY[i]];

    if (s_style == NPG_STYLE_NEOS) {
        ngl_fill_round_rect(sc, r, RADIUS, hot ? TH_CLOSE_FILL : TH_PANEL);
        ngl_draw_round_rect(sc, r, RADIUS, hot ? TH_ACCENT : TH_EDGE, 2);

        const int16_t tw = ngl_text_width(&ngl_font_small, CH_TXT[i]);
        ngl_text(sc, (int16_t)(r.x + (r.w - tw) / 2),
                 (int16_t)(r.y + (r.h - ngl_font_small.height) / 2),
                 CH_TXT[i], &ngl_font_small, hot ? TH_GLOW : TH_TEXT);
        return;
    }

    /*
     * A moulded switch with its legend printed under it, which is how they
     * are on the case: the word belongs to the button above it and reads that
     * way round the moment they are stacked rather than side by side.
     */
    ground_rect(sc, r);

    int16_t pw = (int16_t)(r.w - 2 * GAP);
    if (pw > NPG_PILL_W) {
        pw = NPG_PILL_W;
    }
    pill(sc, ngl_rect((int16_t)(r.x + (r.w - pw) / 2), r.y, pw, NPG_PILL_H),
         hot);

    const int16_t tw = ngl_text_width(&ngl_font_small, CH_TXT[i]);
    ngl_text(sc, (int16_t)(r.x + (r.w - tw) / 2),
             (int16_t)(r.y + r.h - ngl_font_small.height),
             CH_TXT[i], &ngl_font_small, EL_INK);
}

void npg_panel_paint(void)
{
    if (!s_laid_out) {
        return;
    }
    npg_panel_frame();

    for (int k = 0; k < 4; k++) {
        paint_dir(k);
        s_shown_dir[k] = s_down[k];
    }
    for (int i = 0; i < CH_N; i++) {
        paint_chip(i);
        s_shown_chip[i] = (i == CH_LIVES) ? s_lives
                        : (i == CH_ACL)   ? s_acl
                        : (i == CH_STYLE) ? false
                                          : s_down[CH_KEY[i]];
    }
}

/* ------------------------------------------------------------------ */
/* Style                                                               */
/* ------------------------------------------------------------------ */

uint8_t npg_panel_style(void)
{
    return s_style;
}

static void style_path(char *buf, size_t n)
{
    snprintf(buf, n, "apps/%s/%s", neos_app_self(), STYLE_FILE);
}

void npg_panel_style_load(void)
{
    char path[128];
    uint8_t v = NPG_STYLE_NEOS;

    style_path(path, sizeof(path));
    if (neos_file_read(path, &v, sizeof(v)) == (int)sizeof(v)
        && v < NPG_STYLE_N) {
        s_style = v;
    }
}

void npg_panel_style_set(uint8_t style)
{
    if (style >= NPG_STYLE_N || style == s_style) {
        return;
    }
    s_style = style;

    char path[128];
    style_path(path, sizeof(path));
    neos_file_write(path, &s_style, sizeof(s_style));
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

static bool hit(ngl_rect_t r, const neos_touch_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        if (ngl_rect_contains(&r, p[i].x, p[i].y)) {
            return true;
        }
    }
    return false;
}

bool npg_panel_poll(bool *restyled)
{
    *restyled = false;

    if (!s_laid_out) {
        return false;
    }

    neos_touch_t pts[NEOS_TOUCH_MAX];
    int n = neos_touch_points(pts, NEOS_TOUCH_MAX);
    if (n > NEOS_TOUCH_MAX) {
        n = NEOS_TOUCH_MAX;      /* the count is the truth; the array is not */
    }

    bool any = false;

    for (int k = 0; k < 4; k++) {
        s_down[k] = hit(s_dir[k], pts, n);
        any |= s_down[k];
    }

    s_acl = hit(s_chip[CH_ACL], pts, n);

    for (int i = 0; i < CH_N; i++) {
        if (CH_KEY[i] < 0) {
            continue;
        }
        s_down[CH_KEY[i]] = hit(s_chip[i], pts, n);
        any |= s_down[CH_KEY[i]];
    }

    /*
     * LIVES latches on the press edge, because it is not a key. On the board
     * it is an unpopulated pad that is either bridged or not, and the ROM
     * only looks at it during power-on - so it is a switch you set and then
     * press ACL, not something you can hold down mid-game.
     */
    static bool lives_was;
    const bool lives_now = hit(s_chip[CH_LIVES], pts, n);
    if (lives_now && !lives_was) {
        s_lives = !s_lives;
    }
    lives_was = lives_now;

    /* STYLE, likewise on the edge - and it repaints everything, so the caller
       is told rather than this doing it from inside a poll. */
    static bool style_was;
    const bool style_now = hit(s_chip[CH_STYLE], pts, n);
    if (style_now && !style_was) {
        npg_panel_style_set((uint8_t)((s_style + 1) % NPG_STYLE_N));
        *restyled = true;
    }
    style_was = style_now;

    if (*restyled) {
        return any;      /* the caller is about to repaint the lot anyway */
    }

    /* Repaint only what moved. Chips and pads are large, and repainting all
       eleven of them on every 7.8 ms tick would be most of the app's frame. */
    bool painted = false;

    for (int k = 0; k < 4; k++) {
        if (s_down[k] != s_shown_dir[k]) {
            paint_dir(k);
            s_shown_dir[k] = s_down[k];
            painted = true;
        }
    }
    for (int i = 0; i < CH_N; i++) {
        const bool now = (i == CH_LIVES) ? s_lives
                       : (i == CH_ACL)   ? s_acl
                       : (i == CH_STYLE) ? false
                                         : s_down[CH_KEY[i]];
        if (now != s_shown_chip[i]) {
            paint_chip(i);
            s_shown_chip[i] = now;
            painted = true;
        }
    }

    /*
     * Flushed here rather than left for the frame's own flush, because the
     * frame only flushes when a segment moved - and a button lighting up is
     * exactly the case where nothing on the LCD has changed yet. A press is a
     * few times a second and the rectangles are small, so the extra flush
     * costs nothing worth saving.
     */
    if (painted) {
        ngl_flush();
    }
    return any;
}

uint8_t npg_panel_k(uint8_t r_out)
{
    uint8_t k = 0;

    /* R3 selects the corner pads. The bit order is MAME's IN.1, which is the
       order the ROM expects and not the order they are laid out in. */
    if (r_out & 0x04) {
        if (s_down[NPG_KEY_RIGHT_DOWN]) k |= 0x01;
        if (s_down[NPG_KEY_RIGHT_UP])   k |= 0x02;
        if (s_down[NPG_KEY_LEFT_DOWN])  k |= 0x04;
        if (s_down[NPG_KEY_LEFT_UP])    k |= 0x08;
    }

    /* R4 selects the mode switches - MAME's IN.2. */
    if (r_out & 0x08) {
        if (s_down[NPG_KEY_TIME])   k |= 0x01;
        if (s_down[NPG_KEY_GAME_B]) k |= 0x02;
        if (s_down[NPG_KEY_GAME_A]) k |= 0x04;
        if (s_down[NPG_KEY_ALARM])  k |= 0x08;
    }

    return k;
}

bool npg_panel_acl(void)   { return s_acl; }
bool npg_panel_cheat(void) { return s_lives; }
