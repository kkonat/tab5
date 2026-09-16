/*
 * The picture.
 *
 * A frequency axis across the bottom, a signal axis up the left, and one
 * translucent bell per network centred on its channel. The bells overlap
 * because the radios do - that overlap is the thing worth looking at and the
 * reason this is not a list - so they are drawn as filled shapes at low alpha
 * with a solid outline on top: the fill is what makes a pile-up on channel 6
 * visibly a pile-up, and the outline is what keeps each network's own shape
 * followable through it.
 *
 * Names run vertically up out of the axis, in the same colour as the bell they
 * belong to, which is the whole of the legend. Colour is doing real work here
 * and is the one place this app leaves the system palette - see ws_model.c.
 *
 * Repainting is all or nothing, and that is deliberate rather than lazy. The
 * plot changes when a scan lands, about every three seconds, and when it
 * changes nearly every pixel of it changes - every bell moves, because RSSI
 * moves. There is no cheap subset to find. So the expensive frame is drawn
 * rarely and the cheap frame, which is most of them, draws nothing at all: the
 * tick below leaves without touching the surface unless the model's revision
 * moved or somebody tapped something.
 */
#include <math.h>

#include "wifiscan.h"

/* ------------------------------------------------------------------ */
/* The shape of a carrier                                              */
/* ------------------------------------------------------------------ */

/*
 * A real 802.11b/g spectral mask is flat-topped with shoulders, not a
 * Gaussian. A Gaussian is drawn instead because the question this answers is
 * "how much do these two tread on each other", the answer to that is the
 * overlap area, and a bell gets that right to within the width of the outline
 * while costing one expf() per column instead of a piecewise mask.
 *
 * Sigma is picked so that +/-2 sigma is the 22 MHz a channel actually occupies
 * - the standard's number, not a look. REACH stops at 2.5 sigma, where the
 * curve is at 4% of its peak and another column would be a rounding error
 * costing a screen width of blends.
 */
#define WS_SIGMA   5.5f
#define WS_REACH   2.5f

/* Low enough that four overlapping networks are still four distinguishable
   colours rather than one pale smear, high enough that one on its own reads as
   a solid shape. */
#define WS_FILL_A  56

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define WS_CTRL_H   56     /* the span buttons live here */

/*
 * The channel numbers, and the room above them the leaders need.
 *
 * Taller than the numbers by half again, and that difference is the whole
 * reason for the constant. A crowded channel fans its names out to the right,
 * and each one is tied back to its own tick by a line - which over 200 px of
 * fan needs real vertical run to read as a line rather than as a smear along
 * the axis. Twenty pixels turns six ambiguous labels into six visible
 * guy-ropes converging on channel 6.
 */
#define WS_AXIS_H   60
#define WS_AXIS_NUM 26     /* the numbers start this far down it */
#define WS_GUTTER   64     /* room for "-100" up the left */
#define WS_MARGIN   14

/* One vertical name: the font's height becomes its width once it is turned. */
#define WS_LBL_W    (ngl_font_small.height + 3)

typedef struct {
    ngl_rect_t area;
    ngl_rect_t ctrl;
    ngl_rect_t plot;
    ngl_rect_t axis;
    ngl_rect_t status;
    ngl_rect_t seg[WS_SPAN_COUNT];
    int        lo, hi;     /* first and last channel on screen */
    int        f0, f1;     /* MHz at the left and right edge of the plot */
    int        db_top;     /* dBm at the top of the plot, and at the bottom */
    int        db_bot;
    int        db_step;    /* how far apart the horizontal rules are */
} ws_lay_t;

typedef struct {
    ws_lay_t   lay;
    ws_span_t  span;
    bool       laid;

    /* The signal axis, which follows the room - see autoscale(). Held here
       rather than in the layout because it has to survive being relaid. */
    int        ax_top, ax_bot;
    bool       ax_set;

    bool       repaint;
    uint32_t   seen_rev;
    bool       seen_scanning;
    int        seen_found;
    int        crowded;    /* names there was no room for */
    ngl_rect_t seen_area;  /* to notice a rotation */
} ws_ui_t;

static ws_ui_t s_ui;

static const char *const SPAN_LABEL[WS_SPAN_COUNT] = { "1-7", "7-13", "ALL" };

/*
 * Which channels a span covers.
 *
 * The halves overlap on 7 rather than meeting between 6 and 7, because the
 * thing somebody is looking for when they zoom is a gap, and a gap that fell
 * exactly on the seam would be invisible in both halves.
 *
 * ALL stops at 13 unless something is actually up on 14 - which is Japan only,
 * and rare enough that carrying its 12 MHz of empty axis the rest of the time
 * would cost every other channel a pixel or two of width for nothing.
 */
static void span_chans(ws_span_t s, const ws_model_t *m, int *lo, int *hi)
{
    switch (s) {
    case WS_SPAN_LOW:  *lo = 1; *hi = 7;  return;
    case WS_SPAN_HIGH: *lo = 7; *hi = 13; return;
    default: break;
    }
    *lo = 1;
    *hi = 13;
    for (int i = 0; i < m->n; i++) {
        if (m->ap[i].chan == 14) {
            *hi = 14;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The signal axis                                                     */
/* ------------------------------------------------------------------ */

/* Nearest multiple of five, up and down. Written through an offset because C
   truncates towards zero and every number here is negative, so the plain
   division rounds the wrong way on exactly the values that matter. */
static int up5(int v)   { const int u = v + 200; return ((u + 4) / 5) * 5 - 200; }
static int down5(int v) { const int u = v + 200; return (u / 5) * 5 - 200; }

/* Never show stronger than this or weaker than this, whatever the scan says:
   above -30 is a router on the same desk and below -100 is not a network you
   could use, and neither deserves screen. */
#define WS_DB_CEIL   (-30)
#define WS_DB_FLOOR  (-100)

/* The least the axis may span. Below this the grid is measuring noise: two
   networks 3 dB apart would be drawn half a screen apart and look like a
   difference that matters. */
#define WS_DB_MIN_SPAN 20

/*
 * How much slack the axis keeps before it moves.
 *
 * The whole difficulty with an axis that follows its data is that RSSI is not
 * a measurement so much as a running argument - it moves a couple of dB every
 * scan in a room where nothing has happened - and an axis recomputed from it
 * every three seconds would rescale every three seconds, which makes a plot
 * that is harder to read than the fixed one it replaced. Every bell would
 * change height for reasons that have nothing to do with the bells.
 *
 * So the axis is not recomputed, it is *checked*: it stays exactly where it is
 * until the data leaves a generous band, and only then is it refitted. GRIP is
 * the top of that band - a peak has to come within 2 dB of the ceiling before
 * the axis makes room - and SLACK is the bottom of it, so an axis is only
 * pulled in once there are 22 dB of empty sky above the strongest network.
 * Between those the picture simply holds still, which is what it is for.
 */
#define WS_DB_GRIP   2
#define WS_DB_SLACK  22

static bool autoscale(ws_ui_t *ui, const ws_model_t *m)
{
    int top, bot;

    /*
     * Only what is on screen counts. Zooming to 1-7 while the strongest thing
     * in the flat is on 11 would otherwise keep an axis sized for a network
     * nobody can see, and the four that are visible would stay squashed along
     * the bottom - which is the very complaint this function exists to answer.
     */
    int lo_ch, hi_ch;
    span_chans(ui->span, m, &lo_ch, &hi_ch);

    int shown = 0;
    for (int i = 0; i < m->n; i++) {
        if (m->ap[i].chan >= lo_ch && m->ap[i].chan <= hi_ch) {
            shown++;
        }
    }

    if (shown <= 0) {
        /* Nothing to fit. Keep whatever is up so that a scan which comes back
           empty for one pass does not throw the axis away and then rebuild it
           when the networks reappear a moment later. */
        if (ui->ax_set) {
            return false;
        }
        top = WS_DB_CEIL;
        bot = WS_DB_FLOOR;
    } else {
        int hi = -128;
        int lo = 0;
        for (int i = 0; i < m->n; i++) {
            if (m->ap[i].chan < lo_ch || m->ap[i].chan > hi_ch) {
                continue;
            }
            if (m->ap[i].rssi > hi) { hi = m->ap[i].rssi; }
            if (m->ap[i].rssi < lo) { lo = m->ap[i].rssi; }
        }

        /* Already comfortable? Then nothing moves, which is the common case
           and the point of the whole exercise. */
        if (ui->ax_set &&
            hi <= ui->ax_top - WS_DB_GRIP && hi >= ui->ax_top - WS_DB_SLACK &&
            lo >= ui->ax_bot + WS_DB_GRIP) {
            return false;
        }

        /* A peak sits four dB below the top and the weakest four above the
           bottom, so a bell is never drawn touching either frame. */
        top = up5(hi + 4);
        bot = down5(lo - 4);

        if (top > WS_DB_CEIL)  { top = WS_DB_CEIL;  }
        if (bot < WS_DB_FLOOR) { bot = WS_DB_FLOOR; }

        /* Open it downwards first: the strong end is where the eye goes, so
           the headroom above the peak is the part worth keeping exact. */
        while (top - bot < WS_DB_MIN_SPAN && bot > WS_DB_FLOOR) {
            bot -= 5;
        }
        while (top - bot < WS_DB_MIN_SPAN && top < WS_DB_CEIL) {
            top += 5;
        }
    }

    if (ui->ax_set && top == ui->ax_top && bot == ui->ax_bot) {
        return false;
    }
    ui->ax_top = top;
    ui->ax_bot = bot;
    ui->ax_set = true;
    return true;
}

static void relayout(ws_ui_t *ui, const ws_model_t *m)
{
    ws_lay_t *L = &ui->lay;

    L->area = ngl_app_area();
    L->ctrl = ngl_rect(L->area.x, L->area.y, L->area.w, WS_CTRL_H);

    int16_t x = (int16_t)(L->ctrl.x + WS_MARGIN);
    for (int i = 0; i < WS_SPAN_COUNT; i++) {
        L->seg[i] = ngl_rect(x, (int16_t)(L->ctrl.y + 8), 116, 40);
        x = (int16_t)(x + 116 + 10);
    }
    L->status = ngl_rect(x, L->ctrl.y,
                         (int16_t)(L->ctrl.x + L->ctrl.w - x), WS_CTRL_H);

    /*
     * The plot is everything between the buttons and the channel numbers.
     *
     * There was a band under it for the names once, and giving that up is what
     * pays for both halves of this: the bells get the whole height, so a 10 dB
     * difference is 80 px rather than 40, and a name gets 35 characters of run
     * instead of 17, which is more than an SSID can legally be - so nothing is
     * ever truncated. The names stand over the plot instead, which is also
     * where they belong: a name touching its own bell needs no legend.
     */
    const int16_t top  = (int16_t)(L->area.y + WS_CTRL_H + 1);
    const int16_t bot  = (int16_t)(L->area.y + L->area.h);

    L->plot = ngl_rect((int16_t)(L->area.x + WS_GUTTER), top,
                       (int16_t)(L->area.w - WS_GUTTER - WS_MARGIN),
                       (int16_t)(bot - top - WS_AXIS_H));
    L->axis = ngl_rect(L->plot.x, (int16_t)(L->plot.y + L->plot.h),
                       L->plot.w, WS_AXIS_H);

    span_chans(ui->span, m, &L->lo, &L->hi);

    /*
     * Air at each end, so the outermost bell is not clipped against the frame
     * - it is the shape of the edge of the band that says whether channel 1 is
     * really as empty as it looks.
     *
     * Fifteen and not half a channel: the margin has to cover a bell's whole
     * reach, which is WS_SIGMA * WS_REACH, and anything less cuts the tail of
     * the first and last networks off square against the edge.
     */
    L->f0 = ws_chan_mhz(L->lo) - 15;
    L->f1 = ws_chan_mhz(L->hi) + 15;

    L->db_top = ui->ax_top;
    L->db_bot = ui->ax_bot;

    /* Ten dB rules over a wide axis, five over a narrow one. A fixed ten
       would leave a 20 dB axis with a single rule across the middle, which is
       a line rather than a grid; a fixed five would put nine of them on a
       70 dB axis and turn the plot into graph paper. */
    L->db_step = (L->db_top - L->db_bot) >= 40 ? 10 : 5;

    ui->laid      = true;
    ui->seen_area = L->area;
}

/* ------------------------------------------------------------------ */
/* Axes                                                                */
/* ------------------------------------------------------------------ */

static int16_t x_of(const ws_lay_t *L, float mhz)
{
    const float t = (mhz - (float)L->f0) / (float)(L->f1 - L->f0);
    return (int16_t)((float)L->plot.x + t * (float)(L->plot.w - 1) + 0.5f);
}

static float mhz_of(const ws_lay_t *L, int16_t x)
{
    const float t = (float)(x - L->plot.x) / (float)(L->plot.w - 1);
    return (float)L->f0 + t * (float)(L->f1 - L->f0);
}

/* Integer, because dBm is an integer and this is called for grid lines and
   peaks rather than once per column. */
static int16_t y_of(const ws_lay_t *L, int dbm)
{
    if (dbm > L->db_top) {
        dbm = L->db_top;
    }
    if (dbm < L->db_bot) {
        dbm = L->db_bot;
    }
    const int span = L->db_top - L->db_bot;
    return (int16_t)(L->plot.y + L->plot.h - 1
                     - (dbm - L->db_bot) * (L->plot.h - 1) / span);
}

static void draw_grid(ngl_surface_t *s, const ws_lay_t *L)
{
    const int16_t base = y_of(L, L->db_bot);

    /* Strictly between the two ends. The top is the frame and has no room for
       a label above the plot; the bottom is the axis and is drawn brighter,
       below. Both still get their number in the gutter, because on an axis
       that moves, the two ends are the two figures worth reading. */
    for (int db = L->db_bot + L->db_step; db < L->db_top; db += L->db_step) {
        ngl_hline(s, L->plot.x, y_of(L, db), L->plot.w, TH_RULE);
    }

    ngl_hline(s, L->plot.x, base, L->plot.w, TH_EDGE);

    for (int db = L->db_bot; db <= L->db_top; db += L->db_step) {
        char t[8];
        snprintf(t, sizeof t, "%d", db);
        const int16_t w = ngl_text_width(&ngl_font_small, t);
        int16_t       y = (int16_t)(y_of(L, db) - ngl_font_small.height / 2);

        /* The topmost number would sit half over the rule above the plot, so
           it is dropped under its own line instead of centred on it. */
        if (db == L->db_top) {
            y = (int16_t)(y + ngl_font_small.height / 2 + 2);
        }
        ngl_text(s, (int16_t)(L->plot.x - 10 - w), y,
                 t, &ngl_font_small, TH_TEXT_FAINT);
    }

    for (int ch = L->lo; ch <= L->hi; ch++) {
        const int16_t x = x_of(L, (float)ws_chan_mhz(ch));

        /* Dotted rather than drawn, so a bell's outline crossing it still
           reads as the outline and not as a junction. */
        for (int16_t y = (int16_t)(L->plot.y + 2); y < base; y = (int16_t)(y + 7)) {
            ngl_pixel(s, x, y, TH_RULE);
        }
        ngl_vline(s, x, base, 7, TH_EDGE);

        char t[4];
        snprintf(t, sizeof t, "%d", ch);
        const int16_t w = ngl_text_width(&ngl_font_small, t);
        ngl_text(s, (int16_t)(x - w / 2), (int16_t)(L->axis.y + WS_AXIS_NUM),
                 t, &ngl_font_small, TH_TEXT_DIM);
    }
}

/* ------------------------------------------------------------------ */
/* The bells                                                           */
/* ------------------------------------------------------------------ */

static void draw_bell(ngl_surface_t *s, const ws_lay_t *L, const ws_ap_t *a)
{
    if (a->chan < 1 || a->chan > 14) {
        return;                     /* not on this band; nowhere to put it */
    }

    const float   fc   = (float)ws_chan_mhz(a->chan);
    const int16_t base = y_of(L, L->db_bot);
    const float   amp  = (float)(base - y_of(L, a->rssi));
    if (amp <= 0.0f) {
        return;
    }

    const int16_t left  = L->plot.x;
    const int16_t right = (int16_t)(L->plot.x + L->plot.w - 1);

    int16_t x0 = x_of(L, fc - WS_SIGMA * WS_REACH);
    int16_t x1 = x_of(L, fc + WS_SIGMA * WS_REACH);
    if (x0 < left) {
        x0 = left;
    }
    if (x1 > right) {
        x1 = right;
    }

    int16_t prev = base;

    for (int16_t x = x0; x <= x1; x++) {
        const float d = (mhz_of(L, x) - fc) / WS_SIGMA;
        const float h = amp * expf(-0.5f * d * d);

        int16_t y = (int16_t)((float)base - h + 0.5f);
        if (y < L->plot.y) {
            y = L->plot.y;
        }

        /* The fill, blended rather than drawn: two networks on top of each
           other have to come out looking like two. */
        for (int16_t py = y; py < base; py++) {
            ngl_pixel_blend(s, x, py, a->colour, WS_FILL_A);
        }

        /* The outline, solid and anti-aliased, so one network stays
           followable across a crowd of them. */
        if (x > x0) {
            ngl_line_aa(s, (int16_t)(x - 1), prev, x, y, a->colour);
        }
        prev = y;
    }
}

/* ------------------------------------------------------------------ */
/* Names, turned                                                       */
/* ------------------------------------------------------------------ */

/*
 * One string, a quarter turn anticlockwise: first character at the bottom,
 * reading upward.
 *
 * ngl draws text one way up, which is the right call for a library - but a
 * name has to sit above the channel it is on, and a horizontal name is four
 * channels wide. So the glyph bits are walked here instead. The format is
 * ngl's own and is documented in ngl_text.c: cells consecutive from
 * font->first, rows of bytes_per_row bytes, bits MSB-first. Turning it is
 * nothing more than swapping which of row and column moves x and which moves
 * y, and taking the column the other way so the text climbs.
 *
 * ngl_pixel() marks nothing dirty, which is what makes this cheap enough to do
 * a pixel at a time - the caller has already dirtied the whole app area with
 * the fill it cleared to.
 */
static void vglyphs(ngl_surface_t *s, int16_t x, int16_t bottom, const char *str,
                    const ngl_font_t *f, ngl_color_t c)
{
    const size_t cell = (size_t)f->height * f->bytes_per_row;
    int16_t      pen  = bottom;

    for (const char *p = str; *p; p++) {
        uint8_t ch = (uint8_t)*p;
        if (ch < f->first || ch > f->last) {
            ch = '?';
        }
        if (ch >= f->first && ch <= f->last) {
            const uint8_t *g = f->bits + (size_t)(ch - f->first) * cell;
            for (int16_t row = 0; row < f->height; row++) {
                const uint8_t *line = g + (size_t)row * f->bytes_per_row;
                for (int16_t col = 0; col < f->width; col++) {
                    if ((line[col >> 3] >> (7 - (col & 7))) & 1) {
                        ngl_pixel(s, (int16_t)(x + row), (int16_t)(pen - col), c);
                    }
                }
            }
        }
        pen = (int16_t)(pen - f->width);
    }
}

/*
 * The same, cut out of whatever it is standing on.
 *
 * The names live over the plot now rather than under it, which means coloured
 * text over coloured fills, over the grid, over other networks' outlines - and
 * a bright green name on a bright green bell is not a name. So the string is
 * stamped four times in the ground colour, one pixel out in each direction,
 * before it is drawn: a one-pixel moat that separates the letters from the
 * picture without hiding any more of the picture than the letters already do.
 *
 * Four offsets and not eight. The diagonals close the corners of the moat and
 * are what make it look like an outline rather than a shadow, which is heavier
 * than this wants - the point is legibility, and at 16 px wide the letters get
 * there with the four.
 */
static void vtext(ngl_surface_t *s, int16_t x, int16_t bottom, const char *str,
                  const ngl_font_t *f, ngl_color_t c)
{
    vglyphs(s, (int16_t)(x - 1), bottom, str, f, TH_BG);
    vglyphs(s, (int16_t)(x + 1), bottom, str, f, TH_BG);
    vglyphs(s, x, (int16_t)(bottom - 1), str, f, TH_BG);
    vglyphs(s, x, (int16_t)(bottom + 1), str, f, TH_BG);
    vglyphs(s, x, bottom, str, f, c);
}

/*
 * Where the names go.
 *
 * Each one wants to sit centred on its channel, and on a crowded channel they
 * cannot all have that. Rather than stacking them - which would put the third
 * network on channel 6 somewhere nobody would look for it - they are laid out
 * left to right in channel order, each taking the leftmost place at or after
 * where it wanted to be. So a name is always at or to the right of its own
 * channel and never before it, and a crowd on 6 spills into the empty air over
 * 7 and 8 instead of vanishing. Any name that had to move gets a leader line
 * down to its tick, so "which channel is this one actually on" is never a
 * guess.
 *
 * Returns how many did not fit at all, which the status line reports: a plot
 * that silently dropped four networks would be worse than a list.
 */
static int draw_names(ngl_surface_t *s, const ws_lay_t *L, const ws_model_t *m)
{
    int order[WS_MAX];
    int n = 0;

    for (int i = 0; i < m->n; i++) {
        if (m->ap[i].chan >= L->lo && m->ap[i].chan <= L->hi) {
            order[n++] = i;
        }
    }

    /* Channel ascending. The model is already strongest-first and this sort is
       stable, so within a channel the strongest network keeps the place
       closest to its own tick - which is the one worth having. */
    for (int i = 1; i < n; i++) {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0 && m->ap[order[j]].chan > m->ap[key].chan) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    const int16_t right  = (int16_t)(L->plot.x + L->plot.w);
    const int16_t bottom = (int16_t)(L->plot.y + L->plot.h - 2);
    const int     room   = L->plot.h / ngl_font_small.width;

    int16_t pen  = L->plot.x;
    int     over = 0;

    for (int k = 0; k < n; k++) {
        const ws_ap_t *a  = &m->ap[order[k]];
        const int16_t  cx = x_of(L, (float)ws_chan_mhz(a->chan));

        int16_t lx = (int16_t)(cx - WS_LBL_W / 2);
        if (lx < pen) {
            lx = pen;
        }
        if (lx + WS_LBL_W > right) {
            over = n - k;       /* the rest are further right still */
            break;
        }

        /* Room is 35 characters against an SSID's legal 32, so this never
           fires at this rotation - it is here for the day the plot is shorter
           than that, and it marks the cut rather than hiding it: two networks
           called Home-Guest-2G and Home-Guest-5G must not both come out as
           Home-Guest-. */
        char t[34];
        snprintf(t, sizeof t, "%s", a->ssid);
        if (room > 1 && (int)strlen(t) > room) {
            t[room - 1] = '>';
            t[room]     = 0;
        }

        vtext(s, (int16_t)(lx + 1), bottom, t, &ngl_font_small, a->colour);

        /* A name that had to move gets a leader down to the tick it belongs
           to, drawn in the network's own colour: on a crowded channel the
           labels fan out to the right, and without this the third one reads as
           being on a channel two along from the one it is really on. */
        const int16_t mid = (int16_t)(lx + WS_LBL_W / 2);
        if (mid - cx > 3 || cx - mid > 3) {
            ngl_line_aa(s, mid, (int16_t)(L->axis.y + 2),
                        cx, (int16_t)(L->axis.y + WS_AXIS_NUM - 4), a->colour);
        }

        pen = (int16_t)(lx + WS_LBL_W);
    }

    return over;
}

/* ------------------------------------------------------------------ */
/* Furniture                                                           */
/* ------------------------------------------------------------------ */

static void draw_segs(ngl_surface_t *s, const ws_ui_t *ui)
{
    for (int i = 0; i < WS_SPAN_COUNT; i++) {
        const ngl_rect_t r  = ui->lay.seg[i];
        const bool       on = (i == (int)ui->span);

        ngl_fill_round_rect(s, r, 8, on ? TH_KEY_LATCH : TH_KEY_FILL);
        ngl_draw_round_rect(s, r, 8, on ? TH_ACCENT : TH_KEY_EDGE, 1);

        const int16_t w = ngl_text_width(&ngl_font_small, SPAN_LABEL[i]);
        ngl_text(s, (int16_t)(r.x + (r.w - w) / 2),
                 (int16_t)(r.y + (r.h - ngl_font_small.height) / 2),
                 SPAN_LABEL[i], &ngl_font_small, on ? TH_GLOW : TH_KEY_TEXT);
    }
}

static const char *nothing_to_show(const ws_model_t *m);

static void draw_status(ngl_surface_t *s, const ws_ui_t *ui, const ws_model_t *m)
{
    const ngl_rect_t r = ui->lay.status;
    ngl_fill_rect(s, r, TH_BG);

    /* No count when the plot is not showing one. A line reading "12 networks"
       beside a plot saying the radio is off would be describing a scan from
       before somebody switched it off, which is true and useless. */
    char line[72];
    if (nothing_to_show(m)) {
        line[0] = 0;
    } else if (ui->crowded > 0) {
        snprintf(line, sizeof line, "%d networks   %d off the edge",
                 m->found, ui->crowded);
    } else {
        snprintf(line, sizeof line, "%d network%s", m->found,
                 m->found == 1 ? "" : "s");
    }

    const int16_t w = ngl_text_width(&ngl_font_small, line);
    const int16_t y = (int16_t)(r.y + (r.h - ngl_font_small.height) / 2);
    const int16_t x = (int16_t)(r.x + r.w - WS_MARGIN - w);

    ngl_text(s, x, y, line, &ngl_font_small, TH_TEXT_DIM);

    /* The live pip. A scan is a couple of seconds of the radio being somewhere
       else, and this is what says the picture is about to move rather than
       that it has stopped. */
    ngl_fill_round_rect(s, ngl_rect((int16_t)(x - 26),
                                    (int16_t)(y + ngl_font_small.height / 2 - 6),
                                    12, 12), 6,
                        m->scanning ? TH_ACCENT : TH_TEXT_FAINT);
}

/* Something to say when there is nothing to draw. None of these is an error: a
   tablet that has just woken up is in the last one for a second or two every
   time, and the radio being off is a thing the bar does and this reports. */
static const char *nothing_to_show(const ws_model_t *m)
{
    switch (neos_net_state()) {
    case NEOS_NET_ABSENT:
        return "no radio - this tablet cannot see the band";
    case NEOS_NET_OFF:
        return "the radio is off - turn it on from the bar";
    default:
        break;
    }
    if (!m->ever) {
        return "listening";
    }
    return m->n == 0 ? "nothing on the air" : NULL;
}

static void draw_all(ws_ui_t *ui, const ws_model_t *m)
{
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return;
    }
    const ws_lay_t *L = &ui->lay;

    /* One fill for the whole app area, which is also what marks it dirty -
       every pixel below lands inside it, including the ones ngl_pixel() puts
       down without telling anybody. */
    ngl_fill_rect(s, L->area, TH_BG);
    ngl_hline(s, L->area.x, (int16_t)(L->area.y + WS_CTRL_H), L->area.w, TH_RULE);

    draw_grid(s, L);

    const char *why = nothing_to_show(m);
    if (why) {
        const int16_t w = ngl_text_width(&ngl_font_small, why);
        ngl_text(s, (int16_t)(L->plot.x + (L->plot.w - w) / 2),
                 (int16_t)(L->plot.y + L->plot.h / 2 - ngl_font_small.height / 2),
                 why, &ngl_font_small, TH_TEXT_DIM);
        ui->crowded = 0;
    } else {
        /* Weakest first, so where two cross it is the stronger network's
           outline that survives - which is the one being looked for. */
        for (int i = m->n - 1; i >= 0; i--) {
            draw_bell(s, L, &m->ap[i]);
        }
        ui->crowded = draw_names(s, L, m);
    }

    draw_segs(s, ui);
    draw_status(s, ui, m);
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

static void handle_touch(ws_ui_t *ui)
{
    int16_t tx, ty;
    if (!neos_touch_tap(&tx, &ty)) {
        return;
    }
    for (int i = 0; i < WS_SPAN_COUNT; i++) {
        if (ngl_rect_contains(&ui->lay.seg[i], tx, ty)) {
            if (ui->span != (ws_span_t)i) {
                ui->span    = (ws_span_t)i;
                ui->repaint = true;
            }
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The loop's half of it                                               */
/* ------------------------------------------------------------------ */

void ws_ui_init(void)
{
    memset(&s_ui, 0, sizeof s_ui);
    s_ui.span    = WS_SPAN_ALL;
    s_ui.repaint = true;
}

void ws_ui_repaint(void)
{
    s_ui.repaint = true;
}

void ws_ui_tick(const ws_model_t *m)
{
    ws_ui_t *ui = &s_ui;

    if (ui->laid) {
        handle_touch(ui);
    }

    /* A rotation changes the app area under us and nothing else says so. */
    const ngl_rect_t now = ngl_app_area();
    if (!ui->laid || now.x != ui->seen_area.x || now.y != ui->seen_area.y ||
        now.w != ui->seen_area.w || now.h != ui->seen_area.h) {
        ui->repaint = true;
    }

    if (m->rev != ui->seen_rev || m->found != ui->seen_found) {
        ui->repaint = true;
    }

    /* Checked every tick rather than only when the model moves, because the
       span buttons change which networks are in view and therefore what the
       axis has to hold. Almost every call returns false. */
    if (autoscale(ui, m)) {
        ui->repaint = true;
    }

    if (ui->repaint) {
        relayout(ui, m);
        draw_all(ui, m);
        ui->repaint       = false;
        ui->seen_rev      = m->rev;
        ui->seen_found    = m->found;
        ui->seen_scanning = m->scanning;
        ngl_flush();
        return;
    }

    /* Nothing moved but the pip. Repaint the corner it lives in and nothing
       else, which is what keeps an idle plot at zero pixels of panel traffic
       between scans. */
    if (m->scanning != ui->seen_scanning) {
        ui->seen_scanning = m->scanning;
        ngl_surface_t *s = ngl_screen();
        if (s) {
            draw_status(s, ui, m);
            ngl_flush();
        }
    }
}
