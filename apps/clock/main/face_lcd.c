/*
 * The LCD face: a digital watch, from the front.
 *
 * The rule this face is built around is that nothing on it is drawn. A twisted
 * nematic panel is a sheet of glass with a fixed pattern of transparent
 * electrodes on it, decided when it was made; every digit, every word, every
 * symbol it will ever show is already there, and "displaying" something means
 * driving one electrode so that its patch of crystal stops passing light.
 *
 * Three consequences run through everything below:
 *
 *   Nothing is ever absent. An unlit segment is still a segment - the crystal
 *   is in the cell whether it is twisted or not - so the whole figure eight
 *   sits behind every digit, all fourteen electrodes sit behind every letter
 *   of the place name, the six weekdays nobody is on are still printed, and
 *   the nine weather symbols it is not doing are still etched on the glass.
 *   They are drawn faint, every frame, and the lit one goes over them.
 *
 *   Nothing is free-form. There is no font in a watch. The date and the
 *   temperature are seven-segment digits and the place is fourteen-segment
 *   cells, because those are the only characters the hardware owns; the words
 *   - MON, C - are fixed legends printed once at the factory, which is why
 *   those alone may use a typeface. A name too long for the cells scrolls
 *   through them one whole cell at a time, because there is nowhere on this
 *   hardware for half a letter to be.
 *
 *   Nothing switches instantly. A twisted nematic cell takes a third of a
 *   second or so to turn over and it does not do it cleanly - it arrives with
 *   energy left and rings about its new alignment, so under a bright light you
 *   can watch a segment darken, back off a shade and settle. So every
 *   electrode on this face runs through a ramp with a small ring in it, which
 *   is RAMP_ON and RAMP_OFF below and the reason a digit changing costs a few
 *   frames rather than one.
 *
 * The rest is the physical stack, in the order light meets it: the mica
 * reflector, dithered because RGB565 cannot hold a gentle gradient without
 * banding; the dead segments; the shadow the glass casts, softened and thrown
 * by however the tablet is being held; and the segments themselves, dark
 * rather than black, because a twisted crystal absorbs most of the light and
 * not all of it.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"

#include "clock.h"

#define MARGIN     30
#define GAP_MIN    16
#define GAP_MAX    54
#define WDAY_H     34
#define ICON_H     58

/* Between the place marquee and the temperature it sits to the left of. */
#define INFO_SEP   28

/* The mica, top and bottom of its gradient. */
#define MICA_TOP   NGL_RGB(163, 170, 150)
#define MICA_BOT   NGL_RGB(134, 141, 122)

/* Not black: a twisted crystal absorbs most of the light, not all of it. */
#define LCD_INK    NGL_RGB(28, 32, 34)

/* How much ink is left in a dead segment and in a printed-but-unlit legend.
   Low: an undriven cell on a real panel is a shape you can find when you look
   for it, not one you read - it has to disappear behind the driven ones at a
   glance and only come back when the display is nearly empty. */
#define GHOST_MIX  17
#define LEGEND_MIX 30

/*
 * The shadow, in three passes.
 *
 * A hard offset copy reads as a second set of segments; what a few millimetres
 * of glass actually casts is soft. So the widest pass is drawn first and
 * faintest, and each one over it is tighter and darker - the same trick the
 * LED face uses for its halo, run in the other direction.
 */
#define SHADOW_PASSES 3
static const struct { int16_t bleed; uint8_t mix; } SHADOW[SHADOW_PASSES] = {
    { 3, 14 },
    { 1, 30 },
    { 0, 52 },
};

/* How far it can travel at a full 1 g of tilt, as a fraction of segment
   thickness. Small: the glass is thin. */
#define SHADOW_NUM 3
#define SHADOW_DEN 4

/*
 * How a cell gets from off to on, and back.
 *
 * A ramp, but not a clean one. A nematic cell driven from rest does not slide
 * to its new alignment and stop there - it arrives with some energy left and
 * rings about the end point before the viscosity takes it, and on a watch
 * under a hard light you can catch that as the segment darkening, backing off
 * a shade, and settling. The ring is small on purpose: it has to be something
 * you notice out of the corner of your eye on a digit that just changed, not
 * a visible bounce.
 *
 * Two tables rather than one reversed, and two speeds, because the two
 * directions are not the same event. Driven, the cell rings on its way over
 * and takes six frames about it - a third of a second, which is roughly what
 * the slow panels in cheap watches take, and is the part worth watching.
 * Released, it clears in three: the phase counter runs back two steps at a
 * time, so the release samples the far end of its own table and is over almost
 * before it started.
 *
 * Both tables start at 0 and end at 255, which is what lets a settled cell sit
 * at either end of the same phase counter with nothing special said about it.
 */
#define RAMP_STEPS 6
#define RAMP_DOWN  2
static const uint8_t RAMP_ON[RAMP_STEPS + 1]  = { 0, 96, 186, 240, 226, 248, 255 };
static const uint8_t RAMP_OFF[RAMP_STEPS + 1] = { 0, 22,  10,  62, 140, 210, 255 };

/** Where a cell at phase @p ph sits, on its way to on (@p rising) or off. */
static inline uint8_t ramp(uint8_t ph, bool rising)
{
    if (ph > RAMP_STEPS) {
        ph = RAMP_STEPS;
    }
    return rising ? RAMP_ON[ph] : RAMP_OFF[ph];
}

/* One whole cell of the place marquee, per step. */
#define MARQUEE_MS 420

/* ------------------------------------------------------------------ */
/* A cell                                                              */
/* ------------------------------------------------------------------ */

/*
 * One addressable position on the glass: where it is, which electrodes were
 * etched into it, which of them are driven now, and how far each one has got
 * along the curve above.
 *
 * `all` is what makes one struct do for the three kinds of cell this face has
 * - a seven-segment digit, a fourteen-segment letter, and the single bar that
 * is a minus sign or a date separator. It is the ghost layer as well as the
 * limit: a minus sign that ghosted a whole figure eight behind itself would be
 * a digit cell, and the panel does not have one there.
 */
#define MAX_SEGS  14
#define MAX_CELLS 48

/* Everything that is not the marquee: ten for DD-MM-YYYY, six for the time,
   three for the temperature and its sign. What is left is how long a place
   name the glass can hold, which is why the cap is written this way round. */
#define FIXED_CELLS 19

typedef struct {
    int16_t           x, y;
    const clk_seg7_t *g;
    bool              wide;             /* fourteen electrodes, not seven */
    uint16_t          all;              /* what was etched here */
    uint16_t          want;             /* what is driven now, so which ramp */
    uint8_t           ph[MAX_SEGS];     /* how far along it each one has got */
} cell_t;

/* A run of cells that is erased and repainted as a unit. */
typedef struct {
    int16_t           first, n;
    ngl_rect_t        box;              /* the strip of glass it occupies */
    const clk_seg7_t *g;                /* whose thickness sets the spill */
} row_t;

static struct {
    ngl_rect_t band, wday, date, time, info, icons;

    clk_seg7_t  big, small, tiny, dash;
    clk_seg14_t wide;
    int16_t     dgap, sgap, tgap, wgap;

    /* Where the two colons and the temperature legend land, which are the only
       things on the face that are not cells. */
    int16_t     colon_x, scolon_x, sec_y;
    int16_t     temp_x, ring_r;

    cell_t      cell[MAX_CELLS];
    int16_t     ncell;
    row_t       r_date, r_time, r_info;
    int16_t     n_marq;                 /* the marquee, first in r_info */
    ngl_rect_t  marq_box;               /* and where to touch it */
    int16_t     i_sign, i_temp;         /* and the temperature beside it */

    /* Which legend and which symbol are being driven, so the two that are not
       cells can pick the same pair of ramps the cells do. */
    uint8_t     wday_ph[7];
    uint8_t     icon_ph[CLK_WX_SET_N];
    int16_t     wday_on, icon_on;

    uint32_t    marq_step;

    int16_t     ox, oy;                 /* the shadow offset now on screen */

    /*
     * The dithered ground, rendered once and blitted from thereafter.
     *
     * clk_gradient_dither() is per-pixel and this face repaints a digit box
     * every second, every time a cell is part-way through the flicker curve
     * and every time the tablet moves, so doing it on demand would be the
     * whole core. Rendering it once into PSRAM and memcpy-ing rectangles out
     * of it turns the expensive thing into the cheap one.
     */
    ngl_surface_t *ground;
} L;

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void free_ground(void)
{
    if (L.ground) {
        ngl_surface_free(L.ground);
        L.ground = 0;
    }
}

static int16_t add_cell(int16_t x, int16_t y, const clk_seg7_t *g,
                        uint16_t all, bool wide)
{
    if (L.ncell >= MAX_CELLS) {
        return (int16_t)(MAX_CELLS - 1);   /* cannot happen; do not scribble */
    }
    cell_t *c = &L.cell[L.ncell];
    memset(c, 0, sizeof(*c));
    c->x = x;
    c->y = y;
    c->g = g;
    c->all = all;
    c->wide = wide;
    return L.ncell++;
}

/** The four cell geometries, all derived from one big-digit width. */
static void fit(int16_t w)
{
    if (w < 12) {
        w = 12;
    }
    /* A twelfth of the height is about seven degrees, which is where watch
       faces have sat since the LCD ones started copying the LED ones. */
    const int16_t h = (int16_t)((w * 19) / 10);
    clk_seg7_fit(&L.big, w, h, (int16_t)(h / 12));
    L.dgap = (int16_t)(w / 9);

    const int16_t sw = (int16_t)((w * 45) / 100);
    const int16_t sh = (int16_t)((sw * 19) / 10);
    clk_seg7_fit(&L.small, sw, sh, (int16_t)(sh / 12));
    L.sgap = (int16_t)(sw / 8);

    const int16_t tw = (int16_t)((w * 34) / 100);
    const int16_t th = (int16_t)((tw * 19) / 10);
    clk_seg7_fit(&L.tiny, tw, th, (int16_t)(th / 12));
    L.tgap = (int16_t)(tw / 6 + 1);

    /* The separator and the minus sign: a digit cell's middle bar, in a cell
       half as wide. Everything else about it - stroke, notch, lean - has to
       come from the digits it sits between or it reads as a hyphen. */
    L.dash = L.tiny;
    L.dash.w = (int16_t)(tw / 2);

    clk_seg14_fit(&L.wide, tw, th, (int16_t)(th / 12));
    L.wgap = (int16_t)(tw / 6 + 1);
}

/** How wide the time row comes out, HH:MM and the small SS beside it. */
static int16_t time_row_w(void)
{
    return (int16_t)(4 * L.big.w + clk_seg7_colon_w(&L.big) + 5 * L.dgap
                     + clk_seg7_colon_w(&L.small) + 2 * L.sgap + 2 * L.small.w
                     + L.big.slant);
}

/** And the date row, DD-MM-YYYY. */
static int16_t date_row_w(void)
{
    return (int16_t)(8 * L.tiny.w + 2 * L.dash.w + 9 * L.tgap + L.tiny.slant);
}

/** The temperature and its legend, which the marquee gets what is left of. */
static int16_t temp_w(void)
{
    return (int16_t)(L.dash.w + L.tgap + 2 * L.tiny.w + 2 * L.tgap
                     + 2 * L.ring_r + 4 + ngl_text_width(&ngl_font_small, "C")
                     + L.tiny.slant);
}

static void layout(const clock_frame_t *f)
{
    const int16_t avail_w = (int16_t)(f->area.w - 2 * MARGIN);
    const int16_t avail_h = (int16_t)(f->area.h - 2 * MARGIN);

    /*
     * Across first, because the time row is the widest thing on the face and
     * every other size on it is a fraction of that row's digit. The estimate
     * is close enough that the loop below normally runs once - it is there for
     * the rounding, and for a screen turned on its side.
     */
    int16_t w = (int16_t)((avail_w * 100) / 610);
    for (int guard = 0; guard < 4; guard++) {
        fit(w);
        const int16_t need = time_row_w();
        if (need <= avail_w || need <= 0) {
            break;
        }
        w = (int16_t)((int32_t)w * avail_w / need);
    }

    /* Down: five rows, four gaps. Only the three middle ones scale, so the
       height check is against the digit and the two segment strips. */
    for (int guard = 0; guard < 4; guard++) {
        const int16_t rows = (int16_t)(WDAY_H + L.tiny.h + L.big.h + L.tiny.h + ICON_H);
        if (rows + 4 * GAP_MIN <= avail_h || L.big.w <= 12) {
            break;
        }
        w = (int16_t)((int32_t)L.big.w * (avail_h - 4 * GAP_MIN - WDAY_H - ICON_H)
                      / (L.big.h + 2 * L.tiny.h));
        fit(w);
    }

    /*
     * Whatever is left over goes into the gaps rather than round the outside,
     * up to a point. A watch is a panel with its rows spread across it, not a
     * tight block floating in the middle of the glass.
     */
    const int16_t rows = (int16_t)(WDAY_H + L.tiny.h + L.big.h + L.tiny.h + ICON_H);
    int16_t gap = (int16_t)((avail_h - rows) / 4);
    if (gap < GAP_MIN) { gap = GAP_MIN; }
    if (gap > GAP_MAX) { gap = GAP_MAX; }

    clk_stack(f->area, (int16_t)(rows + 4 * gap), 0, 0, &L.band, 0);

    L.ring_r = (int16_t)(L.tiny.h / 8 + 1);
    L.ncell = 0;

    int16_t y = L.band.y;

    /* ---- the weekday legends ------------------------------------- */
    L.wday = ngl_rect(L.band.x, y, L.band.w, WDAY_H);
    y = (int16_t)(y + WDAY_H + gap);

    /* ---- the date, DD-MM-YYYY ------------------------------------ */
    L.date = ngl_rect(L.band.x, y, L.band.w, L.tiny.h);
    {
        const int16_t dw = date_row_w();
        int16_t x = (int16_t)(L.band.x + (L.band.w - dw) / 2);
        if (x < L.band.x) { x = L.band.x; }

        L.r_date.first = L.ncell;
        L.r_date.box = L.date;
        L.r_date.g = &L.tiny;

        static const uint8_t FIELD[10] = { 0, 0, 1, 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 10; i++) {
            if (FIELD[i]) {
                add_cell(x, y, &L.dash, 0x40, false);
                x = (int16_t)(x + L.dash.w + L.tgap);
            } else {
                add_cell(x, y, &L.tiny, CLK_SEG7_ALL, false);
                x = (int16_t)(x + L.tiny.w + L.tgap);
            }
        }
        L.r_date.n = (int16_t)(L.ncell - L.r_date.first);
    }
    y = (int16_t)(y + L.tiny.h + gap);

    /* ---- the time, HH:MM with the seconds on the same line -------- */
    L.time = ngl_rect(L.band.x, y, L.band.w, L.big.h);
    L.sec_y = (int16_t)(y + L.big.h - L.small.h);
    {
        const int16_t tw = time_row_w();
        int16_t x = (int16_t)(L.band.x + (L.band.w - tw) / 2);
        if (x < L.band.x) { x = L.band.x; }

        L.r_time.first = L.ncell;
        L.r_time.box = L.time;
        L.r_time.g = &L.big;

        add_cell(x, y, &L.big, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.big.w + L.dgap);
        add_cell(x, y, &L.big, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.big.w + L.dgap);

        L.colon_x = x;
        x = (int16_t)(x + clk_seg7_colon_w(&L.big) + L.dgap);

        add_cell(x, y, &L.big, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.big.w + L.dgap);
        add_cell(x, y, &L.big, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.big.w + L.dgap);

        L.scolon_x = x;
        x = (int16_t)(x + clk_seg7_colon_w(&L.small) + L.sgap);

        /* Sitting on the big digits' baseline rather than centred on them,
           which is where the seconds window of a watch this shape is. */
        add_cell(x, L.sec_y, &L.small, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.small.w + L.sgap);
        add_cell(x, L.sec_y, &L.small, CLK_SEG7_ALL, false);

        L.r_time.n = (int16_t)(L.ncell - L.r_time.first);
    }
    y = (int16_t)(y + L.big.h + gap);

    /* ---- the place, and the temperature it scrolls beside --------- */
    L.info = ngl_rect(L.band.x, y, L.band.w, L.tiny.h);
    {
        /* Inset, like the symbol row below it: these two are the only bands
           that run to their own ends rather than being centred, and a cell
           hard against the edge of the glass is not a thing a panel has. */
        const int16_t x0 = (int16_t)(L.band.x + MARGIN);
        const int16_t w0 = (int16_t)(L.band.w - 2 * MARGIN);

        const int16_t tw = temp_w();
        int16_t room = (int16_t)(w0 - tw - INFO_SEP);
        int16_t mc = (int16_t)((room + L.wgap) / (L.wide.w + L.wgap));
        if (mc < 0) { mc = 0; }
        if (mc > MAX_CELLS - FIXED_CELLS) {
            mc = (int16_t)(MAX_CELLS - FIXED_CELLS);
        }

        L.r_info.first = L.ncell;
        L.r_info.box = L.info;
        L.r_info.g = &L.tiny;

        L.n_marq = mc;
        int16_t x = x0;
        for (int i = 0; i < mc; i++) {
            add_cell(x, y, &L.wide, CLK_SEG14_ALL, true);
            x = (int16_t)(x + L.wide.w + L.wgap);
        }

        L.marq_box = ngl_rect(x0, y, (int16_t)(mc > 0 ? x - L.wgap - x0 : 0),
                              L.wide.h);

        L.temp_x = (int16_t)(x0 + w0 - tw);
        if (L.temp_x < x + INFO_SEP) {
            L.temp_x = (int16_t)(x + INFO_SEP);
        }
        x = L.temp_x;

        L.i_sign = add_cell(x, y, &L.dash, 0x40, false);
        x = (int16_t)(x + L.dash.w + L.tgap);
        L.i_temp = add_cell(x, y, &L.tiny, CLK_SEG7_ALL, false);
        x = (int16_t)(x + L.tiny.w + L.tgap);
        add_cell(x, y, &L.tiny, CLK_SEG7_ALL, false);

        L.r_info.n = (int16_t)(L.ncell - L.r_info.first);
    }
    y = (int16_t)(y + L.tiny.h + gap);

    /* ---- and the symbols along the bottom ------------------------- */
    L.icons = ngl_rect((int16_t)(L.band.x + MARGIN), y,
                       (int16_t)(L.band.w - 2 * MARGIN), ICON_H);

    /* The ground follows the screen, so it is rebuilt here and nowhere else. */
    free_ground();
    L.ground = ngl_surface_new(f->area.w, f->area.h);
    if (L.ground) {
        clk_gradient_dither(L.ground, MICA_TOP, MICA_BOT);
    }
}

/* ------------------------------------------------------------------ */
/* The ground                                                          */
/* ------------------------------------------------------------------ */

/** Put the mica back over @p r, in screen coordinates. */
static void ground(ngl_surface_t *s, ngl_rect_t r, const clock_frame_t *f)
{
    if (!L.ground) {
        ngl_fill_rect(s, r, MICA_TOP);       /* out of PSRAM; flat, but right */
        return;
    }
    const ngl_rect_t src = ngl_rect((int16_t)(r.x - f->area.x),
                                    (int16_t)(r.y - f->area.y), r.w, r.h);
    ngl_blit(s, r.x, r.y, L.ground, &src);
}

/** The ground's colour at one height, for tinting something drawn over it. */
static ngl_color_t ground_at(int16_t y, const clock_frame_t *f)
{
    const int16_t dy = (int16_t)(y - f->area.y);
    const int16_t h = f->area.h > 1 ? (int16_t)(f->area.h - 1) : 1;
    const int t = dy <= 0 ? 0 : (dy >= h ? 255 : (255 * dy) / h);
    return clk_mix(MICA_TOP, MICA_BOT, (uint8_t)t);
}

/* ------------------------------------------------------------------ */
/* Tilt                                                                */
/* ------------------------------------------------------------------ */

static void shadow_offset(const clock_frame_t *f, int16_t *ox, int16_t *oy)
{
    const int16_t reach = (int16_t)(L.big.thick * SHADOW_NUM / SHADOW_DEN);
    int16_t gx = 0, gy = 0;

    if (!clk_gravity(f, &gx, &gy)) {
        /* No accelerometer: light from above, which is what a photograph of a
           watch assumes and what the face has to look right as. */
        *ox = 0;
        *oy = reach;
        return;
    }

    int32_t x = (int32_t)gx * reach / 1000;
    int32_t y = (int32_t)gy * reach / 1000;
    if (x >  reach) { x =  reach; }
    if (x < -reach) { x = -reach; }
    if (y >  reach) { y =  reach; }
    if (y < -reach) { y = -reach; }

    *ox = (int16_t)x;
    *oy = (int16_t)y;
}

static ngl_rect_t shadow_box(ngl_rect_t r, const clk_seg7_t *g)
{
    return clk_inflate(r, (int16_t)(g->thick * SHADOW_NUM / SHADOW_DEN
                                    + SHADOW[0].bleed + 1));
}

/* ------------------------------------------------------------------ */
/* Driving the cells                                                   */
/* ------------------------------------------------------------------ */

/**
 * Move every electrode of @p c one step towards @p want. True if any moved.
 *
 * One step per frame and no arithmetic on time: the curve is the panel's, not
 * an easing, and a cell that is behind because the app was busy should carry
 * on from where it is rather than jump to where a clock says it should be.
 */
static bool cell_step(cell_t *c, uint16_t want)
{
    bool moving = false;

    c->want = (uint16_t)(want & c->all);
    for (int i = 0; i < MAX_SEGS; i++) {
        if (!((c->all >> i) & 1)) {
            c->ph[i] = 0;
            continue;
        }
        const uint8_t tgt = ((want >> i) & 1) ? RAMP_STEPS : 0;
        if (c->ph[i] < tgt) {
            c->ph[i]++;
            moving = true;
        } else if (c->ph[i] > tgt) {
            c->ph[i] = (uint8_t)(c->ph[i] > RAMP_DOWN ? c->ph[i] - RAMP_DOWN : 0);
            moving = true;
        }
    }
    return moving;
}

/** The same, but there instantly - for a repaint that is not a transition. */
static void cell_snap(cell_t *c, uint16_t want)
{
    c->want = (uint16_t)(want & c->all);
    for (int i = 0; i < MAX_SEGS; i++) {
        c->ph[i] = (uint8_t)(((c->want >> i) & 1) ? RAMP_STEPS : 0);
    }
}

static void cell_paint_mask(ngl_surface_t *s, const cell_t *c, int16_t dx, int16_t dy,
                            uint16_t mask, const clk_seg7_t *g, ngl_color_t col)
{
    if (!mask) {
        return;
    }
    if (c->wide) {
        clk_seg14_paint(s, (int16_t)(c->x + dx), (int16_t)(c->y + dy), mask, g, col);
    } else {
        clk_seg7_paint(s, (int16_t)(c->x + dx), (int16_t)(c->y + dy), mask, g, col);
    }
}

/**
 * One cell: the dead electrodes behind it, its soft shadow, then the crystal.
 *
 * Everything the cell can show, in the order the light meets it. Segments part
 * way along the flicker curve are grouped by how far they have got and drawn a
 * group at a time, so a cell mid-transition costs a handful of extra fills and
 * a settled one costs exactly what it did before any of this existed.
 */
static void paint_cell(ngl_surface_t *s, const cell_t *c, const clock_frame_t *f)
{
    const ngl_color_t base  = ground_at(c->y, f);
    const ngl_color_t ghost = clk_mix(base, LCD_INK, GHOST_MIX);

    cell_paint_mask(s, c, 0, 0, c->all, c->g, ghost);

    /*
     * Grouped by how far along the ramp each electrode is, and by which ramp
     * it is on, so a cell mid-transition is a handful of extra fills and a
     * settled one is exactly the one fill it was before any of this existed.
     */
    uint16_t at[2][RAMP_STEPS + 1];
    memset(at, 0, sizeof(at));
    bool any = false;
    for (int i = 0; i < MAX_SEGS; i++) {
        if (c->ph[i]) {
            at[(c->want >> i) & 1][c->ph[i]] |= (uint16_t)(1u << i);
            any = true;
        }
    }
    if (!any) {
        return;
    }

    if (L.ox || L.oy) {
        for (int p = 0; p < SHADOW_PASSES; p++) {
            clk_seg7_t t = *c->g;
            t.bleed = SHADOW[p].bleed;
            for (int d = 0; d < 2; d++) {
                for (int k = 1; k <= RAMP_STEPS; k++) {
                    if (!at[d][k]) {
                        continue;
                    }
                    /* The shadow is only ever as dark as the thing casting it,
                       so a segment half-way up throws half a shadow - and rings
                       with it, which is what stops the ring reading as the
                       backlight rather than as the crystal. */
                    const uint8_t mix = (uint8_t)(SHADOW[p].mix * ramp(k, d != 0) / 255);
                    cell_paint_mask(s, c, L.ox, L.oy, at[d][k], &t,
                                    clk_mix(base, LCD_INK, mix));
                }
            }
        }
    }

    for (int d = 0; d < 2; d++) {
        for (int k = 1; k <= RAMP_STEPS; k++) {
            if (at[d][k]) {
                cell_paint_mask(s, c, 0, 0, at[d][k], c->g,
                                clk_mix(ghost, LCD_INK, ramp(k, d != 0)));
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* What each row is driving                                            */
/* ------------------------------------------------------------------ */

/** Two digits of @p n into cells @p i and @p i+1; negative blanks both. */
static void want_two(uint16_t *want, int i, int n)
{
    want[i]     = (uint16_t)(n < 0 ? 0 : clk_seg7_mask_of((n / 10) % 10));
    want[i + 1] = (uint16_t)(n < 0 ? 0 : clk_seg7_mask_of(n % 10));
}

static void want_date(const clock_frame_t *f, uint16_t *want)
{
    const int y = f->have_time ? f->t.year : -1;

    want_two(want, 0, f->have_time ? f->t.day : -1);
    want[2] = 0x40;                                   /* always driven */
    want_two(want, 3, f->have_time ? f->t.month : -1);
    want[5] = 0x40;
    want_two(want, 6, y < 0 ? -1 : (y / 100) % 100);
    want_two(want, 8, y < 0 ? -1 : y % 100);
}

static void want_time(const clock_frame_t *f, uint16_t *want)
{
    want_two(want, 0, f->have_time ? f->t.hour : -1);
    want_two(want, 2, f->have_time ? f->t.min : -1);
    want_two(want, 4, f->have_time ? f->t.sec : -1);
}

static void want_info(const clock_frame_t *f, uint16_t *want, bool *have_temp)
{
    char place[40], win[MAX_CELLS + 1];

    (void)f;
    if (!clk_wx_place(place, sizeof(place))) {
        place[0] = 0;
    }
    clk_marquee(win, sizeof(win), place, L.n_marq, L.marq_step);
    for (int i = 0; i < L.n_marq; i++) {
        want[i] = clk_seg14_mask_of(win[i]);
    }

    int16_t c10 = 0;
    const bool got = clk_wx_temp_c10(&c10);
    const int temp = got ? (c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10)) : 0;
    const int v = temp < 0 ? -temp : temp;
    const int base = (int)(L.i_sign - L.r_info.first);

    want[base] = (uint16_t)((got && temp < 0) ? 0x40 : 0);
    want_two(want, base + 1, got ? v : -1);

    if (have_temp) {
        *have_temp = got;
    }
}

/**
 * Step a row and say whether it needs repainting.
 *
 * @p snap drives everything home without a transition, which is what a full
 * repaint wants - a face that has just been switched to has not changed, it
 * has arrived, and watching it flicker on would be an animation the hardware
 * has no reason to do.
 */
static bool row_step(const row_t *r, const uint16_t *want, bool snap)
{
    bool moved = false;

    for (int i = 0; i < r->n; i++) {
        cell_t *c = &L.cell[r->first + i];
        if (snap) {
            cell_snap(c, want[i]);
        } else if (cell_step(c, want[i])) {
            moved = true;
        }
    }
    return snap || moved;
}

static void row_paint(ngl_surface_t *s, const row_t *r, const clock_frame_t *f)
{
    ground(s, shadow_box(r->box, r->g), f);
    for (int i = 0; i < r->n; i++) {
        paint_cell(s, &L.cell[r->first + i], f);
    }
}

/* ------------------------------------------------------------------ */
/* Rows                                                                */
/* ------------------------------------------------------------------ */

/* Monday first: the week starts where the calendar it is printed under says
   it does, and a watch sold anywhere but the United States prints it here. */
static const char *const WDAY[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };

/**
 * The weekday legends: all seven, printed, one of them on.
 *
 * A typeface is allowed here and nowhere else on this face. These words are
 * screen-printed onto the glass when the panel is made - they are not being
 * rendered by anything - so they may be any shape the factory liked, and it is
 * the crystal behind one of them that is the display. Which is also why they
 * cast no shadow and the cells below them do: the ink is on the surface, the
 * crystal is a few millimetres under it.
 */
static bool wday_step(const clock_frame_t *f, bool snap)
{
    const char *now = f->have_time ? clk_wday(&f->t) : "---";
    bool moved = false;

    L.wday_on = -1;
    for (int i = 0; i < 7; i++) {
        const bool on = (now[0] == WDAY[i][0] && now[1] == WDAY[i][1] &&
                         now[2] == WDAY[i][2]);
        const uint8_t tgt = on ? RAMP_STEPS : 0;
        if (on) {
            L.wday_on = (int16_t)i;
        }
        if (snap) {
            L.wday_ph[i] = tgt;
        } else if (L.wday_ph[i] < tgt) {
            L.wday_ph[i]++;
            moved = true;
        } else if (L.wday_ph[i] > tgt) {
            L.wday_ph[i] = (uint8_t)(L.wday_ph[i] > RAMP_DOWN
                                     ? L.wday_ph[i] - RAMP_DOWN : 0);
            moved = true;
        }
    }
    return snap || moved;
}

static void paint_wday(ngl_surface_t *s, const clock_frame_t *f)
{
    ground(s, L.wday, f);

    const ngl_color_t base = ground_at(L.wday.y, f);
    const ngl_color_t dim  = clk_mix(base, LCD_INK, LEGEND_MIX);

    const int16_t slot = (int16_t)(L.wday.w / 7);
    const int16_t ty = (int16_t)(L.wday.y + (L.wday.h - ngl_font_small.height) / 2);

    for (int i = 0; i < 7; i++) {
        const int16_t tw = ngl_text_width(&ngl_font_small, WDAY[i]);
        const int16_t tx = (int16_t)(L.wday.x + i * slot + (slot - tw) / 2);
        ngl_text(s, tx, ty, WDAY[i], &ngl_font_small,
                 clk_mix(dim, LCD_INK, ramp(L.wday_ph[i], i == L.wday_on)));
    }
}

/**
 * The two colons of the time row.
 *
 * Not cells: a colon on a watch is a pair of electrodes that are driven the
 * whole time the display is on, so they never transition and there is nothing
 * for the flicker curve to do to them. They still cast a shadow, because they
 * are the same crystal as the digits either side.
 */
static void paint_colons(ngl_surface_t *s, const clock_frame_t *f)
{
    const ngl_color_t base = ground_at(L.time.y, f);

    if (L.ox || L.oy) {
        const ngl_color_t sh = clk_mix(base, LCD_INK, SHADOW[2].mix);
        clk_seg7_colon(s, (int16_t)(L.colon_x + L.ox), (int16_t)(L.time.y + L.oy),
                       &L.big, sh);
        clk_seg7_colon(s, (int16_t)(L.scolon_x + L.ox), (int16_t)(L.sec_y + L.oy),
                       &L.small, sh);
    }
    clk_seg7_colon(s, L.colon_x, L.time.y, &L.big, LCD_INK);
    clk_seg7_colon(s, L.scolon_x, L.sec_y, &L.small, LCD_INK);
}

/**
 * The degree ring and the C, which are printed and not driven.
 *
 * Full darkness when there is a reading and the legend tint when there is not,
 * the same way a watch does not hide its own punctuation but does stop
 * claiming a unit for a number it has not got.
 */
static void paint_temp_legend(ngl_surface_t *s, const clock_frame_t *f, bool have_temp)
{
    const ngl_color_t base = ground_at(L.info.y, f);
    const ngl_color_t dim  = clk_mix(base, LCD_INK, LEGEND_MIX);
    const ngl_color_t c    = have_temp ? LCD_INK : dim;

    const int16_t x = (int16_t)(L.cell[L.i_temp].x + 2 * L.tiny.w + 2 * L.tgap);
    const int16_t ty = (int16_t)(L.info.y + (L.tiny.h - ngl_font_small.height) / 2);

    clk_degree(s, (int16_t)(x + L.ring_r),
               (int16_t)(L.info.y + L.ring_r + L.tiny.thick), L.ring_r, c);
    ngl_text(s, (int16_t)(x + 2 * L.ring_r + 4), ty, "C", &ngl_font_small, c);
}

/**
 * The weather symbols: all ten, etched, one of them on.
 *
 * These are liquid crystal like the digits - the same glass, the same few
 * millimetres of it - so the lit one throws the same shadow, thrown the same
 * way by the same tilt. The nine that are not lit are still there.
 */
static bool icon_step(const clock_frame_t *f, bool snap)
{
    const int active = clk_wx_index(f);
    bool moved = false;

    L.icon_on = (int16_t)active;
    for (int i = 0; i < CLK_WX_SET_N; i++) {
        const uint8_t tgt = (i == active) ? RAMP_STEPS : 0;
        if (snap) {
            L.icon_ph[i] = tgt;
        } else if (L.icon_ph[i] < tgt) {
            L.icon_ph[i]++;
            moved = true;
        } else if (L.icon_ph[i] > tgt) {
            L.icon_ph[i] = (uint8_t)(L.icon_ph[i] > RAMP_DOWN
                                     ? L.icon_ph[i] - RAMP_DOWN : 0);
            moved = true;
        }
    }
    return snap || moved;
}

/* An icon has an alpha edge and no bleed to grow, so its shadow is softened by
   drawing it about its own offset rather than by widening it. */
static const struct { int16_t dx, dy; uint8_t mix; } ICON_SHADOW[] = {
    { -1, -1, 16 }, {  1, -1, 16 }, { -1,  1, 16 }, {  1,  1, 16 }, { 0, 0, 52 },
};
#define ICON_SHADOW_N ((int)(sizeof(ICON_SHADOW) / sizeof(ICON_SHADOW[0])))

static void paint_icons(ngl_surface_t *s, const clock_frame_t *f)
{
    ground(s, shadow_box(L.icons, &L.tiny), f);

    const ngl_color_t base  = ground_at(L.icons.y, f);
    const ngl_color_t ghost = clk_mix(base, LCD_INK, GHOST_MIX);

    /* Every symbol first, faint and in its fixed place, then whichever one is
       being driven over the top of its own ghost. */
    clk_wx_icon_row(s, L.icons, -1, ghost, ghost);

    for (int i = 0; i < CLK_WX_SET_N; i++) {
        if (!L.icon_ph[i]) {
            continue;
        }
        const ngl_icon_t *ic = clk_wx_set(i);
        if (!ic) {
            continue;
        }
        const ngl_rect_t slot = clk_wx_icon_slot(L.icons, i);
        const uint8_t lvl = ramp(L.icon_ph[i], i == L.icon_on);

        if (L.ox || L.oy) {
            for (int p = 0; p < ICON_SHADOW_N; p++) {
                const uint8_t mix = (uint8_t)(ICON_SHADOW[p].mix * lvl / 255);
                if (!mix) {
                    continue;
                }
                ngl_icon(s, (int16_t)(slot.x + L.ox + ICON_SHADOW[p].dx),
                         (int16_t)(slot.y + L.oy + ICON_SHADOW[p].dy), ic,
                         clk_mix(base, LCD_INK, mix));
            }
        }
        ngl_icon(s, slot.x, slot.y, ic, clk_mix(ghost, LCD_INK, lvl));
    }
}

/* ------------------------------------------------------------------ */

static void paint(const clock_frame_t *f, const void *cfg)
{
    (void)cfg;
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }

    if (f->full) {
        /* Cleared once so the system bar - which only ngl_clear() repaints -
           is dealt with, then the real ground goes over the app area. */
        ngl_clear(s, MICA_TOP);
        layout(f);
        shadow_offset(f, &L.ox, &L.oy);
        ground(s, f->area, f);
    }

    /*
     * A tilt is worth a repaint only when the shadow would land on a different
     * pixel. Without this the face would redraw from sensor noise alone, which
     * on a software blitter is the whole core spent on a shadow moving between
     * the same two places.
     */
    int16_t ox = L.ox, oy = L.oy;
    shadow_offset(f, &ox, &oy);
    const bool tilted = (ox != L.ox || oy != L.oy);
    L.ox = ox;
    L.oy = oy;

    L.marq_step = f->ms / MARQUEE_MS;

    uint16_t want[MAX_CELLS];
    bool drew = false;
    bool have_temp = false;

    memset(want, 0, sizeof(want));
    want_date(f, want);
    if (row_step(&L.r_date, want, f->full) || tilted) {
        row_paint(s, &L.r_date, f);
        drew = true;
    }

    memset(want, 0, sizeof(want));
    want_time(f, want);
    if (row_step(&L.r_time, want, f->full) || tilted) {
        row_paint(s, &L.r_time, f);
        paint_colons(s, f);
        drew = true;
    }

    memset(want, 0, sizeof(want));
    want_info(f, want, &have_temp);
    if (row_step(&L.r_info, want, f->full) || tilted) {
        row_paint(s, &L.r_info, f);
        paint_temp_legend(s, f, have_temp);
        drew = true;
    }

    /* Printed ink on the surface of the glass: no shadow, so no repaint when
       the tablet moves. */
    if (wday_step(f, f->full)) {
        paint_wday(s, f);
        drew = true;
    }
    if (icon_step(f, f->full) || tilted) {
        paint_icons(s, f);
        drew = true;
    }

    if (drew) {
        ngl_flush();
    }
}

/**
 * The place band is the one thing on this face worth pointing at.
 *
 * A generous target: the cells are a third of a digit tall and the thing being
 * aimed at is a word, not a letter. Whether or not a city comes back the map
 * was drawn over the face, so this always reports the tap consumed and takes
 * the full repaint that comes with it.
 */
static bool tap(const clock_frame_t *f, int16_t x, int16_t y)
{
    if (L.n_marq <= 0 || L.marq_box.w <= 0) {
        return false;
    }
    const ngl_rect_t hit = clk_inflate(L.marq_box, 16);
    if (!ngl_rect_contains(&hit, x, y)) {
        return false;
    }

    const ngl_color_t base = ground_at(L.info.y, f);
    clk_city_pick(f, MICA_TOP, LCD_INK, clk_mix(base, LCD_INK, GHOST_MIX));
    return true;
}

static void leave(void)
{
    free_ground();
}

const clock_face_t clock_face_lcd = {
    .key      = "lcd",
    .name     = "LCD",
    .blurb    = "watch glass, shadow follows the tilt",
    .paint    = paint,
    .leave    = leave,
    .tap      = tap,
    .cfg      = 0,
    /* Fast enough that the shadow tracks the wrist rather than catching up
       with it, and that the ramp above is six frames rather than one; the
       repaint only happens when something on the glass really moved. */
    .frame_ms = 60,
};
