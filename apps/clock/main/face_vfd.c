/*
 * The VFD face: a vacuum fluorescent display, hardware and all.
 *
 * Three things make one of these recognisable, and the third is the one
 * everybody forgets:
 *
 *   the phosphor is that particular blue-green, and it glows - a lit dot bleeds
 *   into the glass around it, so every dot is drawn twice, once wide and dim
 *   and once at size;
 *
 *   the unlit dots are visible. The phosphor is painted on the anodes whether
 *   they are driven or not, so the whole grid shows faintly in a dark room -
 *   which is why clk_dots() takes an off colour and this face passes one;
 *
 *   and you can see the works. Behind the glass there is a mesh of horizontal
 *   filament wires - the cathode, a few hair-fine tungsten lines stretched the
 *   whole width - and the grid electrodes between the character cells. On a
 *   real display these are in front of the phosphor and slightly out of focus,
 *   so they are drawn first, dim, and the digits go over the top of them.
 *
 * The filaments do not sit on the pixel grid on purpose. A real one is a wire
 * under tension, drawn at whatever angle it was strung at, and a line exactly
 * on a scanline reads as a rendering artefact rather than as a wire - so they
 * are drawn anti-aliased with a slight sag and land between rows.
 *
 * The layout is five bands down one tube, which is how a panel of this kind is
 * built: a row of weekday annunciators, the date, the time, the place and the
 * temperature, and the weather symbols. Everything but the time is on the same
 * small matrix, because a tube has one cell size per band and the band is
 * decided when it is made.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"

#include "clock.h"

#define MARGIN     36
#define SEC_GAP    20
#define INFO_SEP   28
#define ICON_H     58
#define GAP_MIN    18
#define GAP_MAX    88

/* Not black: the inside of the tube is a very dark blue-green, never neutral. */
#define VFD_BG     NGL_RGB(5, 11, 13)
#define VFD_ON     NGL_RGB(130, 245, 255)

/* Undriven phosphor, and the halo around a driven dot. Low: an anode that is
   not being driven should be something you notice about the tube rather than
   something you read, or every cell competes with the one that is lit. */
#define VFD_OFF_MIX  10
#define VFD_HALO_MIX 34

/* The works, all barely there. Tungsten is warm, the grid is not. */
#define VFD_WIRE   NGL_RGB(190, 160, 100)
#define WIRE_MIX   52
#define MESH_MIX   16

#define FILAMENTS  4

/* One whole cell of the place marquee, per step. */
#define MARQUEE_MS 420

/* Monday first: the week starts where the calendar it is printed under says
   it does, which on everything but an American panel is here. */
static const char *const WDAY[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };

/* As many cells as the place band can ever have. A tube has a fixed number
   and this is it. */
#define MAX_MARQ 24

static struct {
    ngl_rect_t band, wday, date, hhmm, ss, info, icons;
    int16_t    pitch, dot;       /* the time */
    int16_t    spitch, sdot;     /* the seconds */
    int16_t    tpitch, tdot;     /* the date and the place */
    int16_t    wpitch, wdot;     /* the annunciators */

    int16_t    marq_x, marq_n;   /* the place */
    ngl_rect_t marq_box;         /* and where to touch it */
    int16_t    temp_x;

    uint32_t   marq_step;
} L;

/* ------------------------------------------------------------------ */

/** The temperature as this tube would print it, or "" with no reading. */
static void temp_str(char *buf, size_t n)
{
    int16_t c10 = 0;
    buf[0] = 0;
    if (!clk_wx_temp_c10(&c10)) {
        return;
    }
    const int whole = c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10);
    snprintf(buf, n, "%d" CLK_DEG "C", whole);
}

/** The date band's fixed field: eleven cells, and always eleven. */
static void date_str(const clock_frame_t *f, char *buf, size_t n)
{
    if (f->have_time) {
        snprintf(buf, n, "%02d %s %04d", f->t.day, clk_month(&f->t), f->t.year);
    } else {
        snprintf(buf, n, "-- --- ----");
    }
}

static void layout(const clock_frame_t *f)
{
    const int16_t avail_w = (int16_t)(f->area.w - 2 * MARGIN);
    const int16_t avail_h = (int16_t)(f->area.h - 2 * MARGIN);

    /*
     * The time band sets the cell size, and everything else is a fraction of
     * it - one tube, one set of proportions. "00:00" is five cells of five
     * columns with a blank between each, so 29 pitches; the seconds beside it
     * are 17 more at 55%, and there is a gap between the two.
     */
    int16_t pitch = (int16_t)(((avail_w - SEC_GAP) * 100) / 3835);
    if (pitch < 2) {
        pitch = 2;
    }

    L.pitch  = pitch;
    L.dot    = (int16_t)(pitch * 4 / 5);
    L.spitch = (int16_t)(pitch * 55 / 100);
    L.sdot   = (int16_t)(L.spitch * 4 / 5);
    L.tpitch = (int16_t)(pitch * 40 / 100);
    L.tdot   = (int16_t)(L.tpitch * 4 / 5);
    L.wpitch = (int16_t)(pitch * 25 / 100);
    L.wdot   = (int16_t)(L.wpitch * 4 / 5);
    if (L.spitch < 2) { L.spitch = 2; }
    if (L.tpitch < 2) { L.tpitch = 2; }
    if (L.wpitch < 2) { L.wpitch = 2; }
    if (L.sdot < 1)   { L.sdot = 1; }
    if (L.tdot < 1)   { L.tdot = 1; }
    if (L.wdot < 1)   { L.wdot = 1; }

    /* Seven three-letter legends and the blanks between them have to fit
       across, and on a narrow screen they are what runs out first. */
    while (L.wpitch > 2 && clk_dots_w("MON MON MON MON MON MON MON", L.wpitch) > avail_w) {
        L.wpitch--;
        L.wdot = (int16_t)(L.wpitch * 4 / 5);
        if (L.wdot < 1) { L.wdot = 1; }
    }

    const int16_t wday_h = clk_dots_h(L.wpitch);
    const int16_t band_h = clk_dots_h(L.tpitch);
    const int16_t big_h  = clk_dots_h(L.pitch);
    const int16_t rows   = (int16_t)(wday_h + band_h + big_h + band_h + ICON_H);

    int16_t gap = (int16_t)((avail_h - rows) / 4);
    if (gap < GAP_MIN) { gap = GAP_MIN; }
    if (gap > GAP_MAX) { gap = GAP_MAX; }

    clk_stack(f->area, (int16_t)(rows + 4 * gap), 0, 0, &L.band, 0);

    int16_t y = L.band.y;

    L.wday = ngl_rect(L.band.x, y, L.band.w, wday_h);
    y = (int16_t)(y + wday_h + gap);

    L.date = ngl_rect(L.band.x, y, L.band.w, band_h);
    y = (int16_t)(y + band_h + gap);

    {
        const int16_t big_w = clk_dots_w("00:00", L.pitch);
        const int16_t sec_w = clk_dots_w(":00", L.spitch);
        const int16_t sec_h = clk_dots_h(L.spitch);
        const int16_t all_w = (int16_t)(big_w + SEC_GAP + sec_w);
        const int16_t x = (int16_t)(L.band.x + (L.band.w - all_w) / 2);

        L.hhmm = ngl_rect(x, y, big_w, big_h);
        /* Sitting on the bottom row of the big cells rather than centred on
           them, which is where the seconds window of a panel like this is. */
        L.ss = ngl_rect((int16_t)(x + big_w + SEC_GAP),
                        (int16_t)(y + big_h - sec_h), sec_w, sec_h);
    }
    y = (int16_t)(y + big_h + gap);

    L.info = ngl_rect(L.band.x, y, L.band.w, band_h);
    {
        /* Inset like the symbol row below: this band and that one are the only
           two that run to their own ends rather than being centred. */
        const int16_t x0 = (int16_t)(L.band.x + MARGIN);
        const int16_t w0 = (int16_t)(L.band.w - 2 * MARGIN);

        /* The temperature field is as wide as the widest reading the tube can
           ever show, not as wide as this one - the cells do not move. */
        const int16_t tw = clk_dots_w("-88" CLK_DEG "C", L.tpitch);
        const int16_t cell = (int16_t)(6 * L.tpitch);
        int16_t n = (int16_t)((w0 - tw - INFO_SEP + L.tpitch) / cell);
        if (n < 0)        { n = 0; }
        if (n > MAX_MARQ) { n = MAX_MARQ; }

        L.marq_x = x0;
        L.marq_n = n;
        L.marq_box = ngl_rect(x0, y, (int16_t)(n > 0 ? n * cell - L.tpitch : 0),
                              band_h);
        L.temp_x = (int16_t)(x0 + w0 - tw);
    }
    y = (int16_t)(y + band_h + gap);

    L.icons = ngl_rect((int16_t)(L.band.x + MARGIN), y,
                       (int16_t)(L.band.w - 2 * MARGIN), ICON_H);
}

/* ------------------------------------------------------------------ */

/**
 * The filament wires and the grid mesh, over the whole display.
 *
 * Drawn once per full repaint and then left alone. The bands are painted over
 * them and never erase them, because every repaint of a band fills its own
 * rectangle first - so a wire crossing it is gone until the next full paint.
 * That is exactly what a real one does: the works are only visible where the
 * phosphor is not.
 */
static void works(ngl_surface_t *s, ngl_rect_t r)
{
    const ngl_color_t wire = clk_mix(VFD_BG, VFD_WIRE, WIRE_MIX);
    const ngl_color_t mesh = clk_mix(VFD_BG, VFD_ON, MESH_MIX);

    /* The grid electrodes: one per character cell of the time band, so they
       line up with the digits above them rather than with the screen. */
    const int16_t cell = (int16_t)(6 * L.pitch);
    for (int16_t x = L.hhmm.x; x < L.hhmm.x + L.hhmm.w; x = (int16_t)(x + cell)) {
        ngl_vline(s, (int16_t)(x - L.pitch / 2), r.y, r.h, mesh);
    }
    ngl_vline(s, (int16_t)(L.hhmm.x + L.hhmm.w - L.pitch / 2), r.y, r.h, mesh);

    /*
     * The filaments, with a sag. Anti-aliased and a couple of pixels off
     * horizontal, so they read as wire under tension rather than as a row of
     * the framebuffer somebody forgot to clear.
     */
    for (int i = 0; i < FILAMENTS; i++) {
        const int16_t y = (int16_t)(r.y + (int32_t)r.h * (2 * i + 1) / (2 * FILAMENTS));
        const int16_t sag = (int16_t)(r.h / 90 + 1);
        ngl_line_aa(s, r.x, y, (int16_t)(r.x + r.w / 2), (int16_t)(y + sag), wire);
        ngl_line_aa(s, (int16_t)(r.x + r.w / 2), (int16_t)(y + sag),
                    (int16_t)(r.x + r.w), y, wire);
    }
}

/** A dot-matrix string with its bloom: wide and dim, then at size. */
static void glow_dots(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                      int16_t pitch, int16_t dot)
{
    const ngl_color_t halo = clk_mix(VFD_BG, VFD_ON, VFD_HALO_MIX);
    const ngl_color_t off  = clk_mix(VFD_BG, VFD_ON, VFD_OFF_MIX);

    /* The halo is drawn with no off colour, so only the lit dots bloom - an
       undriven anode has nothing to glow with. */
    const int16_t wide = (int16_t)(dot + pitch / 2);
    clk_dots(s, (int16_t)(x - (wide - dot) / 2), (int16_t)(y - (wide - dot) / 2),
             str, pitch, wide, halo, halo);
    clk_dots(s, x, y, str, pitch, dot, VFD_ON, off);
}

/**
 * The weekday annunciators: all seven printed, one of them driven.
 *
 * Their own anodes rather than part of a matrix, which is why the six that are
 * not on show as the shape of a word and not as a cell full of dark dots - and
 * why none of them moves when a different day comes round.
 */
static void paint_wday(ngl_surface_t *s, const clock_frame_t *f)
{
    ngl_fill_rect(s, L.wday, VFD_BG);

    const char *now = f->have_time ? clk_wday(&f->t) : "---";
    const ngl_color_t dim = clk_mix(VFD_BG, VFD_ON, VFD_OFF_MIX + 5);

    const int16_t slot = (int16_t)(L.wday.w / 7);
    const int16_t word = clk_dots_w("MON", L.wpitch);

    for (int i = 0; i < 7; i++) {
        const int16_t x = (int16_t)(L.wday.x + i * slot + (slot - word) / 2);
        const bool on = (now[0] == WDAY[i][0] && now[1] == WDAY[i][1] &&
                         now[2] == WDAY[i][2]);
        if (on) {
            glow_dots(s, x, L.wday.y, WDAY[i], L.wpitch, L.wdot);
        } else {
            clk_dots(s, x, L.wday.y, WDAY[i], L.wpitch, L.wdot, dim, dim);
        }
    }
}

static void paint_date(ngl_surface_t *s, const clock_frame_t *f)
{
    char buf[16];
    date_str(f, buf, sizeof(buf));

    ngl_fill_rect(s, clk_inflate(L.date, L.tpitch), VFD_BG);
    const int16_t w = clk_dots_w(buf, L.tpitch);
    glow_dots(s, (int16_t)(L.date.x + (L.date.w - w) / 2), L.date.y,
              buf, L.tpitch, L.tdot);
}

static void paint_time(ngl_surface_t *s, const clock_frame_t *f)
{
    char buf[8];
    if (f->have_time) {
        snprintf(buf, sizeof(buf), "%02d:%02d", f->t.hour, f->t.min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }

    /* The bloom's dots are wider than their cells and drawn back half the
       difference, so they overhang the measured box on every side. */
    ngl_fill_rect(s, clk_inflate(L.hhmm, L.pitch), VFD_BG);
    glow_dots(s, L.hhmm.x, L.hhmm.y, buf, L.pitch, L.dot);
}

static void paint_secs(ngl_surface_t *s, const clock_frame_t *f)
{
    char buf[8];
    if (f->have_time) {
        snprintf(buf, sizeof(buf), ":%02d", f->t.sec);
    } else {
        snprintf(buf, sizeof(buf), ":--");
    }

    ngl_fill_rect(s, clk_inflate(L.ss, L.spitch), VFD_BG);
    glow_dots(s, L.ss.x, L.ss.y, buf, L.spitch, L.sdot);
}

/**
 * The place and the temperature, on one band.
 *
 * The temperature sits in a field as wide as the longest reading the tube can
 * show, hard against the right, and the place scrolls through whatever cells
 * are left of it. Scrolling is one whole cell at a time because a matrix has
 * nowhere to put half a character, and the field is padded rather than centred
 * so that a shorter name does not slide the temperature along.
 */
static void paint_info(ngl_surface_t *s, const clock_frame_t *f)
{
    char place[40], win[MAX_MARQ + 1], temp[16];

    (void)f;
    ngl_fill_rect(s, clk_inflate(L.info, L.tpitch), VFD_BG);

    if (!clk_wx_place(place, sizeof(place))) {
        place[0] = 0;
    }
    clk_marquee(win, sizeof(win), place, L.marq_n, L.marq_step);
    glow_dots(s, L.marq_x, L.info.y, win, L.tpitch, L.tdot);

    temp_str(temp, sizeof(temp));
    if (temp[0]) {
        /* Right-aligned inside its own fixed field, so the degree sign lands
           in the same cell whether it is nine degrees out or nineteen. */
        const int16_t tw = clk_dots_w("-88" CLK_DEG "C", L.tpitch);
        const int16_t w = clk_dots_w(temp, L.tpitch);
        glow_dots(s, (int16_t)(L.temp_x + tw - w), L.info.y, temp, L.tpitch, L.tdot);
    }
}

/**
 * The weather symbols: all ten, etched on the glass, one of them driven.
 *
 * A vacuum fluorescent display has one anode per symbol, laid down when the
 * tube was built. The nine it is not showing are still phosphor and still
 * faintly visible, and none of them moves when a different one lights - so
 * this draws the whole row in the unlit tint every time and lights one, which
 * is the only thing the hardware could do.
 */
static void paint_icons(ngl_surface_t *s, const clock_frame_t *f)
{
    ngl_fill_rect(s, L.icons, VFD_BG);
    clk_wx_icon_row(s, L.icons, clk_wx_index(f), VFD_ON,
                    clk_mix(VFD_BG, VFD_ON, VFD_OFF_MIX + 5));
}

/**
 * The place band, which is the one band on this tube worth pointing at.
 *
 * The map goes up in the tube's own colours - phosphor on dark glass, the
 * whole matrix faintly there - so it reads as another window in the same
 * display rather than as a dialog from somewhere else.
 */
static bool tap(const clock_frame_t *f, int16_t x, int16_t y)
{
    if (L.marq_n <= 0 || L.marq_box.w <= 0) {
        return false;
    }
    const ngl_rect_t hit = clk_inflate(L.marq_box, 16);
    if (!ngl_rect_contains(&hit, x, y)) {
        return false;
    }

    clk_city_pick(f, VFD_BG, VFD_ON, clk_mix(VFD_BG, VFD_ON, VFD_OFF_MIX + 5));
    return true;
}

static void paint(const clock_frame_t *f, const void *cfg)
{
    (void)cfg;
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }

    const uint32_t step = f->ms / MARQUEE_MS;
    const bool wound = (step != L.marq_step);
    L.marq_step = step;

    if (f->full) {
        ngl_clear(s, VFD_BG);
        layout(f);
        works(s, f->area);
        paint_wday(s, f);
        paint_date(s, f);
        paint_time(s, f);
        paint_secs(s, f);
        paint_info(s, f);
        paint_icons(s, f);
        ngl_flush();
        return;
    }

    if (f->min) {
        paint_wday(s, f);
        paint_date(s, f);
        paint_time(s, f);
        paint_icons(s, f);
    }
    if (f->sec) {
        paint_secs(s, f);
    }
    /* The place band winds on its own clock, which is not the wall clock. */
    if (wound || f->min) {
        paint_info(s, f);
    }
    if (f->sec || f->min || wound) {
        ngl_flush();
    }
}

const clock_face_t clock_face_vfd = {
    .key      = "vfd",
    .name     = "VFD",
    .blurb    = "dot matrix, filaments and all",
    .paint    = paint,
    .leave    = 0,
    .tap      = tap,
    .cfg      = 0,
    /* Fast enough that the place band steps on time; the repaint only happens
       when a band really changed. */
    .frame_ms = 120,
};
