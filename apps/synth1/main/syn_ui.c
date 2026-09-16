/*
 * The panel. See syn_ui.h for what it offers and synth1.h for why.
 *
 * ---------------------------------------------------------------- repainting
 *
 * A frame here costs what changed and not what is on screen, which is the
 * third of the three decisions the app rests on.
 *
 * The canvas is 720x1280 and a wholesale repaint of it - fill, widgets, text,
 * and then 921,600 pixels through the PPA - is tens of milliseconds. The
 * codec's cushion is thirty. Those two numbers are close enough that an app
 * which repaints everything per frame is an app whose sound depends on what
 * the drawing loop happens to be doing, and the whole point of syn_audio.c is
 * that it must not be. Even with the voice safely on its own thread, a full
 * repaint per frame is a 20 fps panel, which is not what a knob under a finger
 * should feel like.
 *
 * So there are three grains of dirt and each is the natural unit of its own
 * widget: a control repaints its cell when its own value changed, the footer
 * repaints its two rows when the instruments are due, and the scope repaints a
 * column when that column moved. The last is the one that matters, because the
 * scope is the widget that changes every single frame: erasing the box and
 * redrawing the trace is a third of a megapixel, where erasing only the part
 * of each column the trace has left is a few thousand.
 */
#include <math.h>
#include <stdio.h>

#include "syn_ui.h"

#include "wg.h"
#include "ngl_theme.h"
#include "neos_sys.h"

/* ------------------------------------------------------------------ */
/* Ranges                                                              */
/* ------------------------------------------------------------------ */

/* Seven octaves, A0 to A7 - the Minimoog's own range, near enough, and wide
   enough that the top of it is where anti-aliasing starts to show. */
#define FREQ_LO    27.5f
#define FREQ_HI  3520.0f

/*
 * The two rate ranges, which is what the LO/HI switch selects.
 *
 * LO is a modulation source: a slow sweep at the bottom, a fast vibrato at the
 * top. HI takes the LFO into the audio band, where modulating pitch stops
 * being vibrato and becomes FM - a different timbre rather than a moving one.
 * That is the interesting half for a load test, because it is the setting
 * where every sample of the block genuinely differs from the last.
 */
#define RATE_LO_MIN    0.05f
#define RATE_LO_MAX   20.0f
#define RATE_HI_MIN   20.0f
#define RATE_HI_MAX 2000.0f

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define MARGIN     24
#define GAP        16
#define HEADER_H   56
#define CLOSE_S    52

#define SCOPE_Y    64
#define SCOPE_H   272

#define CELL_Y    348
#define CELL_H    252
#define KNOB_R     70
#define KNOB_CY   116     /* from the top of the cell */
#define VALUE_Y   210     /* likewise */

/* The widest plot this file will index, which is the panel less its margins.
   The two span arrays below are sized from it and are the scope's memory of
   where the trace was last frame. */
#define PLOT_MAX 1232

static struct {
    int16_t    w, h;
    ngl_rect_t header, close, lfo_bar, scope, plot, foot1, foot2;
    ngl_rect_t cell[C_COUNT];
    int16_t    cx[C_COUNT];      /* knob centres, x */
    int16_t    cy;               /* knob centres, y - the same for all of them */
} L;

static const char *const CTL_NAME[C_COUNT] = {
    "PITCH", "LEVEL", "LFO RATE", "LFO AMT", "SHAPE", "RANGE"
};

void syn_ui_layout(const syn_app_t *a)
{
    const int16_t w = a->gfx.w;
    const int16_t h = a->gfx.h;

    L.w = w;
    L.h = h;

    L.header  = ngl_rect(0, 0, w, HEADER_H);
    L.close   = ngl_rect((int16_t)(w - MARGIN - CLOSE_S), 2, CLOSE_S, CLOSE_S);
    L.lfo_bar = ngl_rect((int16_t)(MARGIN + 260), 20, 240, 16);

    L.scope = ngl_rect(MARGIN, SCOPE_Y, (int16_t)(w - 2 * MARGIN), SCOPE_H);
    L.plot  = ngl_rect((int16_t)(L.scope.x + 2), (int16_t)(L.scope.y + 2),
                       (int16_t)(L.scope.w - 4), (int16_t)(L.scope.h - 4));
    if (L.plot.w > PLOT_MAX) {
        L.plot.w = PLOT_MAX;
    }

    const int16_t cw = (int16_t)((w - 2 * MARGIN - (C_COUNT - 1) * GAP) / C_COUNT);
    for (int i = 0; i < C_COUNT; i++) {
        L.cell[i] = ngl_rect((int16_t)(MARGIN + i * (cw + GAP)), CELL_Y, cw, CELL_H);
        L.cx[i]   = (int16_t)(L.cell[i].x + cw / 2);
    }
    L.cy = CELL_Y + KNOB_CY;

    L.foot1 = ngl_rect(MARGIN, (int16_t)(h - 96), (int16_t)(w - 2 * MARGIN), 32);
    L.foot2 = ngl_rect(MARGIN, (int16_t)(h - 56), (int16_t)(w - 2 * MARGIN), 32);
}

ngl_rect_t syn_ui_ctl_rect(int ctl)
{
    return (ctl >= 0 && ctl < C_COUNT) ? L.cell[ctl] : ngl_rect(0, 0, 0, 0);
}

int syn_ui_hit(int16_t x, int16_t y)
{
    if (ngl_rect_contains(&L.close, x, y)) {
        return HIT_CLOSE;
    }
    for (int i = 0; i < C_COUNT; i++) {
        if (ngl_rect_contains(&L.cell[i], x, y)) {
            return i;
        }
    }
    /* The instruments are a control too: tapping them takes the frame loop off
       its 60 Hz pacing, which is the only way to find out what the frame can
       actually do rather than what it was asked to do. */
    if (y >= L.foot1.y) {
        return HIT_FOOTER;
    }
    return HIT_NONE;
}

/*
 * Three hundred pixels of travel is the full range of a knob.
 *
 * Not the knob's own diameter, which is seventy: a control you can only set to
 * one of seventy values is a control that cannot be tuned, and reaching for a
 * rotary gesture on a capacitive panel is worse - the finger occludes the
 * thing it is turning. So the knob is a handle and the screen is the track,
 * which is how every touch synth that is pleasant to use does it.
 */
#define DRAG_SPAN 300.0f

float syn_ui_drag(float norm0, int16_t y0, int16_t y)
{
    float n = norm0 + (float)(y0 - y) / DRAG_SPAN;
    if (n < 0.0f) { n = 0.0f; }
    if (n > 1.0f) { n = 1.0f; }
    return n;
}

/* ------------------------------------------------------------------ */
/* Positions to values                                                 */
/* ------------------------------------------------------------------ */

/*
 * Logarithmic, because pitch and rate are both intervals rather than
 * quantities: half the knob's travel should be half the octaves, not half the
 * hertz. A linear pitch knob puts six of its seven octaves in the last tenth
 * of its rotation.
 */
static float logmap(float n, float lo, float hi)
{
    return lo * powf(hi / lo, n);
}

/* The inverse of logmap(), used once at startup so that the patch the app
   opens on can be written in hertz rather than in fractions of a turn. */
static float logmap_inv(float v, float lo, float hi)
{
    return logf(v / lo) / logf(hi / lo);
}

void syn_ui_defaults(syn_app_t *a)
{
    a->norm[C_FREQ]  = logmap_inv(110.0f, FREQ_LO, FREQ_HI);   /* A2 */
    a->norm[C_LEVEL] = 0.35f;
    a->norm[C_RATE]  = logmap_inv(5.0f, RATE_LO_MIN, RATE_LO_MAX);
    a->norm[C_AMT]   = 0.15f;
    a->norm[C_SHAPE] = 0.0f;
    a->norm[C_HILO]  = 0.0f;
    a->shape = SYN_TRI;
    a->hi    = false;
}

int16_t syn_ui_knob_cy(void)
{
    return L.cy;
}

void syn_ui_patch(syn_app_t *a)
{
    a->patch.freq      = logmap(a->norm[C_FREQ], FREQ_LO, FREQ_HI);
    a->patch.amp       = a->norm[C_LEVEL];
    a->patch.lfo_rate  = a->hi ? logmap(a->norm[C_RATE], RATE_HI_MIN, RATE_HI_MAX)
                               : logmap(a->norm[C_RATE], RATE_LO_MIN, RATE_LO_MAX);
    a->patch.lfo_amt   = a->norm[C_AMT];
    a->patch.lfo_shape = a->shape;
}

/*
 * Every number on this panel is formatted out of integers, and that is a hard
 * rule rather than a style: a float handed to snprintf is promoted to double
 * by the language, apps link -nostdlib against a table with only some of the
 * double helpers on it, and the result is an app that fails to load. The build
 * turns that into a compile error with -Werror=double-promotion, and this is
 * the file where it would otherwise keep happening.
 */
static void hz(char *buf, int size, float v)
{
    if (v < 10.0f) {
        const int n = (int)(v * 100.0f + 0.5f);
        snprintf(buf, size, "%d.%02d Hz", n / 100, n % 100);
    } else if (v < 1000.0f) {
        const int n = (int)(v * 10.0f + 0.5f);
        snprintf(buf, size, "%d.%d Hz", n / 10, n % 10);
    } else {
        snprintf(buf, size, "%d Hz", (int)(v + 0.5f));
    }
}

void syn_ui_value(const syn_app_t *a, int ctl, char *buf, int size)
{
    switch (ctl) {
    case C_FREQ:  hz(buf, size, a->patch.freq); break;
    case C_RATE:  hz(buf, size, a->patch.lfo_rate); break;
    case C_LEVEL: snprintf(buf, size, "%d%%", (int)(a->patch.amp * 100.0f + 0.5f)); break;
    case C_AMT:   snprintf(buf, size, "%d%%", (int)(a->patch.lfo_amt * 100.0f + 0.5f)); break;
    case C_SHAPE: snprintf(buf, size, "%s", syn_shape_name(a->shape)); break;
    case C_HILO:  snprintf(buf, size, "%s", a->hi ? "AUDIO" : "SUB"); break;
    default:      buf[0] = 0; break;
    }
}

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

/*
 * The knob, the selector and the switch are apps/common/widget's, not this
 * file's. They started here and moved out when apps/moog needed the same ones:
 * two synthesisers on one machine drawn in two hands would look like two
 * programs by two people, and a knob is a knob. What stays here is the layout,
 * which is the half that genuinely differs.
 */
static const uint8_t WAVE_OF_SHAPE[SYN_SHAPES] = {
    WG_SAW, WG_TRI, WG_SQUARE, WG_SH
};

static void paint_ctl(syn_app_t *a, int i)
{
    const turn_t *g = &a->gfx;
    const ngl_rect_t r = L.cell[i];

    turn_fill(g, r, TH_BG);
    turn_text_mid(g, r.x, r.y, r.w, CTL_NAME[i], &ngl_font_small, TH_TEXT_DIM, TH_BG);

    if (i == C_HILO) {
        wg_switch(g, ngl_rect((int16_t)(L.cx[i] - 58), (int16_t)(L.cy - 80), 116, 160),
                  a->hi, "HI", "LO");
    } else if (i == C_SHAPE) {
        wg_knob(g, L.cx[i], L.cy, KNOB_R,
                (float)a->shape / (float)(SYN_SHAPES - 1), SYN_SHAPES);
        wg_wave(g, L.cx[i], L.cy, 28, 16, WAVE_OF_SHAPE[a->shape], 2);
    } else {
        wg_knob(g, L.cx[i], L.cy, KNOB_R, a->norm[i], 0);
    }

    char buf[24];
    syn_ui_value(a, i, buf, sizeof buf);
    turn_text_mid(g, r.x, (int16_t)(r.y + VALUE_Y), r.w, buf,
                 &ngl_font_small, TH_TEXT, TH_BG);
}

/* ------------------------------------------------------------------ */
/* The header                                                          */
/* ------------------------------------------------------------------ */

static void paint_close(const turn_t *g)
{
    const ngl_rect_t r = L.close;
    turn_round(g, r, 10, TH_CLOSE_FILL);
    turn_round_frame(g, r, 10, TH_CLOSE_LINE, 2);

    const int16_t p = 16;
    turn_line_aa(g, (int16_t)(r.x + p), (int16_t)(r.y + p),
                   (int16_t)(r.x + r.w - p), (int16_t)(r.y + r.h - p), TH_CLOSE_X);
    turn_line_aa(g, (int16_t)(r.x + r.w - p), (int16_t)(r.y + p),
                   (int16_t)(r.x + p), (int16_t)(r.y + r.h - p), TH_CLOSE_X);
}

/*
 * The LFO's output as a centre-zero bar.
 *
 * It is the one thing on screen that is evidence rather than a readout: it is
 * fed from the value the audio thread last left behind, so a bar that is
 * moving means blocks are being generated, and a bar that has stopped means
 * they are not - which no frame counter on this panel could tell you, since
 * the frame counter would go on counting perfectly well over a dead codec.
 */
static void paint_lfo_bar(const turn_t *g, float lfo)
{
    const ngl_rect_t r = L.lfo_bar;
    const int16_t mid = (int16_t)(r.x + r.w / 2);

    turn_fill(g, r, TH_PANEL);
    turn_frame(g, r, TH_RULE, 1);

    if (lfo > 1.0f)  { lfo = 1.0f; }
    if (lfo < -1.0f) { lfo = -1.0f; }
    const int16_t n = (int16_t)(lfo * (float)(r.w / 2 - 2));

    if (n > 0) {
        turn_fill(g, ngl_rect(mid, (int16_t)(r.y + 2), n, (int16_t)(r.h - 4)), TH_ACCENT);
    } else if (n < 0) {
        turn_fill(g, ngl_rect((int16_t)(mid + n), (int16_t)(r.y + 2),
                             (int16_t)(-n), (int16_t)(r.h - 4)), TH_ACCENT);
    }
    turn_vline(g, mid, r.y, r.h, TH_EDGE);
}

static void paint_header(syn_app_t *a)
{
    const turn_t *g = &a->gfx;

    turn_fill(g, L.header, TH_BG);
    turn_text(g, MARGIN, 4, "SYNTH1", &ngl_font_large, TH_ACCENT, TH_BG);
    turn_text(g, (int16_t)(MARGIN + 200), 20, "LFO", &ngl_font_small,
             TH_TEXT_DIM, TH_BG);
    paint_lfo_bar(g, syn_audio_lfo());

    if (!a->audio) {
        turn_text(g, (int16_t)(L.lfo_bar.x + L.lfo_bar.w + 24), 20,
                 "no codec - silent", &ngl_font_small, TH_BAD, TH_BG);
    }
    paint_close(g);
    turn_hline(g, 0, (int16_t)(HEADER_H - 1), L.w, TH_RULE);
}

/* ------------------------------------------------------------------ */
/* The scope                                                           */
/* ------------------------------------------------------------------ */

/* Where the trace was last frame, per column, so only the part it has left has
   to be put back. -1 means "nothing drawn here yet". */
static int16_t s_top[PLOT_MAX], s_bot[PLOT_MAX];
static bool    s_primed;

static int16_t s_cap[SYN_SCOPE_N];

/* Eight divisions across, which is what a scope has. */
#define GRID_DIV 8

static inline bool grid_col(int16_t x, int16_t step)
{
    return step > 0 && (x % step) == 0;
}

/*
 * Put the background back over one column between y0 and y1 inclusive.
 *
 * The graticule is why this is not a fill with TH_BG: a vertical division runs
 * the whole height of the plot, so erasing the trace off one would rub a hole
 * in it, and the centre line has the same problem one row at a time. Redrawing
 * the whole graticule per frame would be the simpler code and eight thousand
 * more pixels than the trace itself.
 */
static void col_bg(const turn_t *g, int16_t x, int16_t y0, int16_t y1,
                   int16_t step, int16_t mid)
{
    if (y1 < y0) {
        return;
    }
    const bool gc = grid_col(x, step);
    turn_vline(g, (int16_t)(L.plot.x + x), y0, (int16_t)(y1 - y0 + 1),
              gc ? TH_RULE : TH_BG);
    if (!gc && mid >= y0 && mid <= y1) {
        turn_fill(g, ngl_rect((int16_t)(L.plot.x + x), mid, 1, 1), TH_RULE);
    }
}

static void paint_graticule(const turn_t *g)
{
    const ngl_rect_t p = L.plot;
    const int16_t step = (int16_t)(p.w / GRID_DIV);
    const int16_t mid  = (int16_t)(p.y + p.h / 2);

    turn_fill(g, L.scope, TH_BG);
    turn_frame(g, L.scope, TH_EDGE, 2);
    for (int16_t x = 0; x < p.w; x = (int16_t)(x + step)) {
        turn_vline(g, (int16_t)(p.x + x), p.y, p.h, TH_RULE);
    }
    turn_hline(g, p.x, mid, p.w, TH_RULE);
}

/*
 * One frame of trace.
 *
 * Three things are going on and each is a scope's own behaviour rather than a
 * shortcut:
 *
 *   the timebase follows the pitch. A fixed window shows one cycle of a low
 *   note and ninety of a high one; this picks about three periods of whatever
 *   is playing, so the picture stays the same picture as the knob is turned.
 *   The floor is the capture's length, which is why the bottom octave shows a
 *   slice - three periods of a low A is 109 ms and the buffer holds 43.
 *
 *   it triggers on a rising zero crossing, so the waveform stands still
 *   instead of sliding sideways at the difference between the pitch and the
 *   frame rate. That difference is what a free-running trace shows, and it is
 *   a genuinely confusing thing to watch.
 *
 *   each column is joined to the one before it. At three periods of a high
 *   note there are fewer samples than columns, and a column that only plots
 *   the samples that landed in it draws a dotted line. Reaching back to meet
 *   the previous column's span is what a real scope's Y-T interpolation does
 *   and costs a compare.
 */
static void paint_scope(syn_app_t *a)
{
    const turn_t *g = &a->gfx;
    const ngl_rect_t p = L.plot;

    if (!syn_audio_scope(s_cap)) {
        return;
    }

    /* Room for the trigger to be found late without running off the end. */
    const int search = 256;
    int show = (int)(3.0f * (float)NEOS_AUDIO_RATE / a->patch.freq);
    if (show < 96) { show = 96; }
    if (show > SYN_SCOPE_N - search) { show = SYN_SCOPE_N - search; }

    int start = 0;
    for (int i = 0; i < search; i++) {
        if (s_cap[i] <= 0 && s_cap[i + 1] > 0) {
            start = i;
            break;
        }
    }

    const int16_t step = (int16_t)(p.w / GRID_DIV);
    const int16_t mid  = (int16_t)(p.y + p.h / 2);
    const int32_t half = (p.h / 2) - 2;

    int16_t prev_top = 0, prev_bot = 0;
    bool    have_prev = false;

    for (int16_t x = 0; x < p.w; x++) {
        int i0 = start + (int)((int32_t)x * show / p.w);
        int i1 = start + (int)(((int32_t)x + 1) * show / p.w);
        if (i1 <= i0) {
            i1 = i0 + 1;
        }
        if (i1 > SYN_SCOPE_N) {
            i1 = SYN_SCOPE_N;
        }

        int16_t lo = s_cap[i0], hiv = s_cap[i0];
        for (int i = i0 + 1; i < i1; i++) {
            const int16_t v = s_cap[i];
            if (v < lo)  { lo = v; }
            if (v > hiv) { hiv = v; }
        }

        /* Sample up is screen up, so the maximum is the smaller y. */
        int16_t ntop = (int16_t)(mid - (int32_t)hiv * half / 32768);
        int16_t nbot = (int16_t)(mid - (int32_t)lo  * half / 32768);

        if (have_prev) {
            if (ntop > prev_bot) { ntop = prev_bot; }
            if (nbot < prev_top) { nbot = prev_top; }
        }

        if (s_primed) {
            const int16_t pt = s_top[x], pb = s_bot[x];
            if (nbot < pt || ntop > pb) {
                col_bg(g, x, pt, pb, step, mid);            /* nothing in common */
            } else {
                if (pt < ntop) { col_bg(g, x, pt, (int16_t)(ntop - 1), step, mid); }
                if (pb > nbot) { col_bg(g, x, (int16_t)(nbot + 1), pb, step, mid); }
            }
        }
        turn_vline(g, (int16_t)(p.x + x), ntop, (int16_t)(nbot - ntop + 1), TH_ACCENT);

        s_top[x] = ntop;
        s_bot[x] = nbot;
        prev_top = ntop;
        prev_bot = nbot;
        have_prev = true;
    }
    s_primed = true;
}

/* ------------------------------------------------------------------ */
/* The instruments                                                     */
/* ------------------------------------------------------------------ */

/*
 * Two lines, and between them they answer the question the app was written to
 * ask. The audio line is the one with the veto: if `under` is not zero the
 * sound broke, whatever the other line says.
 */
static void paint_foot(syn_app_t *a)
{
    const turn_t *g = &a->gfx;
    char buf[128];

    syn_meters_t m;
    syn_audio_meters(&m);

    turn_fill(g, L.foot1, TH_BG);
    snprintf(buf, sizeof buf,
             "audio  load %d.%d%%  blk %uus max %uus  lead %ums  under %u",
             (int)(m.load_pm / 10), (int)(m.load_pm % 10),
             (unsigned)m.gen_us, (unsigned)m.gen_us_max,
             (unsigned)(m.lead_us / 1000u), (unsigned)m.underruns);
    turn_text(g, L.foot1.x, L.foot1.y, buf, &ngl_font_small,
             m.underruns ? TH_BAD : TH_TEXT_DIM, TH_BG);

    turn_fill(g, L.foot2, TH_BG);
    snprintf(buf, sizeof buf,
             "ui  %u fps  frame %uus  paint %uus  %s  canvas %s",
             (unsigned)a->fps, (unsigned)a->work_us, (unsigned)a->paint_us,
             a->capped ? "60Hz cap - tap to free" : "uncapped - tap to cap",
             a->gfx.fast ? "sram" : "psram");
    turn_text(g, L.foot2.x, L.foot2.y, buf, &ngl_font_small, TH_TEXT_DIM, TH_BG);
}

/* ------------------------------------------------------------------ */
/* Putting it together                                                 */
/* ------------------------------------------------------------------ */

void syn_ui_repaint(syn_app_t *a)
{
    const turn_t *g = &a->gfx;

    turn_clear(g, TH_BG);
    paint_header(a);
    paint_graticule(g);
    s_primed = false;              /* the trace's memory went with the fill */
    paint_scope(a);
    for (int i = 0; i < C_COUNT; i++) {
        paint_ctl(a, i);
    }
    paint_foot(a);

    turn_present(g, ngl_rect(0, 0, g->w, g->h));

    a->repaint_all = false;
    a->ctl_dirty   = 0;
    a->scope_dirty = false;
    a->hud_dirty   = false;
}

uint32_t syn_ui_paint(syn_app_t *a)
{
    const turn_t *g = &a->gfx;
    uint32_t us = 0;

    if (a->repaint_all) {
        const uint32_t t0 = (uint32_t)neos_uptime_us();
        syn_ui_repaint(a);
        return (uint32_t)neos_uptime_us() - t0;
    }

    /*
     * The LFO bar, and only the LFO bar.
     *
     * It moves whenever the voice does, so it is the one thing in the header
     * that is redrawn every frame - and the header around it is a title and a
     * cross that have not changed since the app opened. Presenting the whole
     * strip to move a bar would be 72,000 pixels through the engine to alter
     * 4,000 of them, sixty times a second, which is most of a millisecond a
     * frame spent on the least important widget on the panel.
     */
    paint_lfo_bar(g, syn_audio_lfo());
    us += turn_present(g, L.lfo_bar);

    for (int i = 0; i < C_COUNT; i++) {
        if (a->ctl_dirty & (1u << i)) {
            paint_ctl(a, i);
            us += turn_present(g, L.cell[i]);
        }
    }
    a->ctl_dirty = 0;

    if (a->scope_dirty) {
        paint_scope(a);
        us += turn_present(g, L.scope);
        a->scope_dirty = false;
    }

    if (a->hud_dirty) {
        paint_foot(a);
        us += turn_present(g, L.foot1);
        us += turn_present(g, L.foot2);
        a->hud_dirty = false;
    }
    return us;
}
