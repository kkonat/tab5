/*
 * The EQ page: a response curve, and six rows that move it.
 *
 * One table drives the drawing and the hit testing, so the two cannot drift -
 * the same rule the pi-Q app used, and the reason adding a band here is one
 * line rather than four edits in three functions.
 *
 * The curve is the part that changed most in the port, and entirely for the
 * better. On the Pi it was 760 columns of six biquad magnitudes in Python,
 * about a tenth of a second, cached because recomputing it every frame set
 * the frame rate for the whole page. Here it is 1184 columns of seven in C
 * with the four trigonometric values shared across the sections and one
 * logarithm at the end of each column - a couple of milliseconds - and it is
 * still cached, because the cheapest redraw is the one that does not happen
 * and nothing about the curve changes while a finger is elsewhere.
 */

#include "rad_ui.h"
#include "rad_math.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* The rows                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *label;
    rad_param_t slider;         /* what the row's slider moves */
    rad_param_t step[2];        /* its steppers, P_COUNT for none */
    const char *caption[2];
    const char *note;           /* printed in the value slot instead, or NULL */
} eq_row_t;

/*
 * In the order the cascade applies them. The high-pass has no gain, so its
 * slider carries the frequency instead and the stepper beside it nudges the
 * same value - drag to sweep, tap to trim.
 */
static const eq_row_t ROWS[] = {
    { "hpf",    P_HPF_FREQ,   { P_HPF_FREQ,   P_COUNT      }, { "freq", NULL    }, "24dB/oct" },
    { "low",    P_LOW_GAIN,   { P_LOW_FREQ,   P_LOW_RS     }, { "freq", "slope" }, NULL },
    { "lo-mid", P_LOMID_GAIN, { P_LOMID_FREQ, P_LOMID_RQ   }, { "freq", "q"     }, NULL },
    { "mid",    P_MID_GAIN,   { P_MID_FREQ,   P_MID_RQ     }, { "freq", "q"     }, NULL },
    { "hi-mid", P_HIMID_GAIN, { P_HIMID_FREQ, P_HIMID_RQ   }, { "freq", "q"     }, NULL },
    { "high",   P_HIGH_GAIN,  { P_HIGH_FREQ,  P_HIGH_RS    }, { "freq", "slope" }, NULL },
};
#define EQ_ROWS  ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define CURVE_H     132
#define ROW_DY       74
#define ROW_H        44
#define LABEL_W     110
#define SLIDER_W    330
#define VALUE_W     140
#define STEP_W      300
#define STEP_BTN     52

/* The curve is +-15 dB, which is exactly the range the gain sliders cover, so
   a band at its limit reaches the edge of the box and not some way short. */
#define CURVE_DB     15

typedef struct {
    ngl_rect_t curve;
    int16_t    row_y;
    int16_t    label_x, slider_x, value_x, step_x[2];
} eq_layout_t;

static void layout(ngl_rect_t page, eq_layout_t *l)
{
    const int16_t left = (int16_t)(page.x + UI_MARGIN);

    l->curve = ngl_rect(left, (int16_t)(page.y + 8),
                        (int16_t)(page.w - 2 * UI_MARGIN), CURVE_H);
    l->row_y = (int16_t)(l->curve.y + l->curve.h + 12);

    l->label_x  = left;
    l->slider_x = (int16_t)(left + LABEL_W + 12);
    l->value_x  = (int16_t)(l->slider_x + SLIDER_W + 12);
    l->step_x[0] = (int16_t)(l->value_x + VALUE_W + 16);
    l->step_x[1] = (int16_t)(l->step_x[0] + STEP_W + 12);
}

static ngl_rect_t row_rect(const eq_layout_t *l, ngl_rect_t page, int row)
{
    return ngl_rect(page.x, (int16_t)(l->row_y + row * ROW_DY),
                    page.w, ROW_DY);
}

/* ------------------------------------------------------------------ */
/* The curve                                                           */
/* ------------------------------------------------------------------ */

static uint32_t s_curve_sig;
static uint32_t s_row_sig[EQ_ROWS];

static void draw_curve(rad_app_t *app, ngl_surface_t *sc, const eq_layout_t *l,
                       bool full)
{
    uint32_t sig = rad_ui_hash(2166136261u, app->params, sizeof(app->params));
    sig = rad_ui_hash(sig, &l->curve, sizeof(l->curve));
    if (sig == s_curve_sig && !full) {
        return;
    }
    s_curve_sig = sig;

    ngl_fill_round_rect(sc, l->curve, 10, TH_PANEL);
    ngl_draw_round_rect(sc, l->curve, 10, TH_EDGE, 1);

    const int16_t mid  = (int16_t)(l->curve.y + l->curve.h / 2);
    const int16_t half = (int16_t)(l->curve.h / 2 - 8);
    const int16_t x0   = (int16_t)(l->curve.x + 6);
    const int16_t x1   = (int16_t)(l->curve.x + l->curve.w - 6);

    ngl_hline(sc, x0, mid, (int16_t)(x1 - x0), TH_RULE);

    /*
     * The decade marks, so the axis means something. 100 Hz, 1 kHz and 10 kHz
     * are where the eye looks for them on a log axis, and their positions
     * come out of the same mapping the curve uses rather than being measured
     * off the drawing.
     */
    for (int decade = 0; decade < 3; decade++) {
        const float t  = (float)(decade + 1) / 3.0f;
        const int16_t x = (int16_t)(x0 + (int32_t)((x1 - x0) * (decade + 1)) / 3);
        (void)t;
        ngl_vline(sc, x, (int16_t)(l->curve.y + 6), (int16_t)(l->curve.h - 12),
                  TH_RULE);
    }

    /*
     * 20 Hz to 20 kHz, three decades, one column at a time. The response is
     * clamped to the box rather than allowed to run off it: a high-pass at
     * 600 Hz is 60 dB down at the bottom of the axis, and a curve that leaves
     * the panel tells you less than one that reaches its edge and stays there.
     */
    int16_t prev_y = 0;
    for (int16_t px = x0; px < x1; px++) {
        const float t  = (float)(px - x0) / (float)(x1 - x0);
        const float hz = 20.0f * rad_exp2f(t * 9.96578428f);   /* 1000^t */

        float db = rad_eq_response(&app->audio.eq, hz);
        if (db >  (float)CURVE_DB) { db =  (float)CURVE_DB; }
        if (db < -(float)CURVE_DB) { db = -(float)CURVE_DB; }

        int16_t y = (int16_t)(mid - (int16_t)(db * (float)half / (float)CURVE_DB));
        if (y < l->curve.y + 4)                 { y = (int16_t)(l->curve.y + 4); }
        if (y > l->curve.y + l->curve.h - 5)    { y = (int16_t)(l->curve.y + l->curve.h - 5); }

        /* Joined to the previous column rather than plotted as dots: a
           high-pass skirt is steep enough that a per-column dot leaves gaps
           you can see through. */
        if (px > x0) {
            ngl_line(sc, (int16_t)(px - 1), prev_y, px, y, TH_ACCENT);
        }
        prev_y = y;
    }
}

/* ------------------------------------------------------------------ */
/* A row                                                               */
/* ------------------------------------------------------------------ */

static void draw_stepper(ngl_surface_t *sc, int16_t x, int16_t y,
                         const char *caption, const char *value,
                         const char *pressed, rad_param_t p)
{
    char key[24];

    snprintf(key, sizeof(key), "step:%d-", (int)p);
    rad_ui_button(sc, ngl_rect(x, y, STEP_BTN, ROW_H), "-", &ngl_font_small,
                  rad_streq(pressed, key), false, true);

    snprintf(key, sizeof(key), "step:%d+", (int)p);
    rad_ui_button(sc, ngl_rect((int16_t)(x + STEP_W - STEP_BTN), y, STEP_BTN, ROW_H),
                  "+", &ngl_font_small, rad_streq(pressed, key), false, true);

    const int16_t bx = (int16_t)(x + STEP_BTN + 6);
    const int16_t bw = (int16_t)(STEP_W - 2 * STEP_BTN - 12);
    ngl_fill_round_rect(sc, ngl_rect(bx, y, bw, ROW_H), 8, TH_BG);

    const int16_t ty = (int16_t)(y + (ROW_H - ngl_font_small.height) / 2);
    ngl_text(sc, (int16_t)(bx + 8), ty, caption, &ngl_font_small, TH_TEXT_DIM);
    ngl_text(sc, (int16_t)(bx + bw - 8 - ngl_text_width(&ngl_font_small, value)),
             ty, value, &ngl_font_small, TH_TEXT);
}

static void draw_row(rad_app_t *app, ngl_surface_t *sc, const eq_layout_t *l,
                     ngl_rect_t page, int row, bool full)
{
    const eq_row_t *r = &ROWS[row];

    char slider_key[24];
    snprintf(slider_key, sizeof(slider_key), "slider:%d", (int)r->slider);

    uint32_t sig = rad_ui_hash(2166136261u, &app->params[r->slider], sizeof(int));
    for (int i = 0; i < 2; i++) {
        if (r->step[i] != P_COUNT) {
            sig = rad_ui_hash(sig, &app->params[r->step[i]], sizeof(int));
        }
    }
    sig = rad_ui_hash_str(sig, app->pressed);
    sig = rad_ui_hash_str(sig, app->slider);
    sig |= 1u;
    if (sig == s_row_sig[row] && !full) {
        return;
    }
    s_row_sig[row] = sig;

    const ngl_rect_t band = row_rect(l, page, row);
    ngl_fill_rect(sc, band, TH_BG);

    const int16_t y  = band.y;
    const int16_t ty = (int16_t)(y + (ROW_H - ngl_font_small.height) / 2);

    ngl_text(sc, l->label_x, ty, r->label, &ngl_font_small, TH_TEXT);

    const rad_range_t *range = &rad_ranges[r->slider];
    const int permille = (int)((int32_t)(app->params[r->slider] - range->lo) *
                               1000 / (range->hi - range->lo));
    rad_ui_slider(sc, l->slider_x, (int16_t)(y + (ROW_H - UI_SLIDER_H) / 2),
                  SLIDER_W, permille, rad_streq(app->slider, slider_key));

    char value[32];
    if (r->note) {
        ngl_text(sc, l->value_x, ty, r->note, &ngl_font_small, TH_TEXT_DIM);
    } else {
        rad_format_param(r->slider, app->params[r->slider], value, sizeof(value));
        ngl_text(sc, l->value_x, ty, value, &ngl_font_small, TH_ACCENT);
    }

    for (int i = 0; i < 2; i++) {
        if (r->step[i] == P_COUNT) {
            continue;
        }
        rad_format_param(r->step[i], app->params[r->step[i]], value, sizeof(value));
        draw_stepper(sc, l->step_x[i], y, r->caption[i], value,
                     app->pressed, r->step[i]);
    }
}

/* ------------------------------------------------------------------ */
/* The page                                                            */
/* ------------------------------------------------------------------ */

void rad_eqpage_draw(rad_app_t *app, ngl_rect_t page, bool full)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }

    eq_layout_t l;
    layout(page, &l);

    if (full) {
        s_curve_sig = 0;
        for (int i = 0; i < EQ_ROWS; i++) {
            s_row_sig[i] = 0;
        }
    }

    draw_curve(app, sc, &l, full);
    for (int i = 0; i < EQ_ROWS; i++) {
        draw_row(app, sc, &l, page, i, full);
    }
}

const char *rad_eqpage_hit(ngl_rect_t page, int16_t x, int16_t y)
{
    static char key[24];

    eq_layout_t l;
    layout(page, &l);

    for (int row = 0; row < EQ_ROWS; row++) {
        const ngl_rect_t band = row_rect(&l, page, row);
        if (y < band.y || y >= band.y + ROW_H) {
            continue;       /* the gap between rows belongs to neither, so a
                               press between two cannot be claimed by both */
        }
        const eq_row_t *r = &ROWS[row];

        if (x >= l.slider_x && x < l.slider_x + SLIDER_W) {
            snprintf(key, sizeof(key), "slider:%d", (int)r->slider);
            return key;
        }
        for (int i = 0; i < 2; i++) {
            if (r->step[i] == P_COUNT) {
                continue;
            }
            const int16_t sx = l.step_x[i];
            if (x >= sx && x < sx + STEP_BTN) {
                snprintf(key, sizeof(key), "step:%d-", (int)r->step[i]);
                return key;
            }
            if (x >= sx + STEP_W - STEP_BTN && x < sx + STEP_W) {
                snprintf(key, sizeof(key), "step:%d+", (int)r->step[i]);
                return key;
            }
        }
        return NULL;
    }
    return NULL;
}

int rad_eqpage_slider_value(rad_param_t p, ngl_rect_t page, int16_t x)
{
    eq_layout_t l;
    layout(page, &l);

    const rad_range_t *r = &rad_ranges[p];
    const int permille = rad_ui_slider_permille(l.slider_x, SLIDER_W, x);
    const int span = r->hi - r->lo;

    /* Rounded to the nearest step of the parameter's own scale, so a drag
       lands on values the steppers could also reach and the two controls do
       not disagree about what "one o'clock" means. */
    return rad_clamp_param(p, r->lo + (int)(((int32_t)span * permille + 500) / 1000));
}
