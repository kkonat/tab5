/*
 * The battery detail page.
 *
 * The bar icon only has room for a colour; this is where a tap on it lands to
 * see the numbers behind that colour - the actual pack voltage, the current
 * flowing, and where that voltage sits against the 2S LiPo lines the icon
 * flashes over. Same source as the icon, same thresholds (NEOS_BATTERY_2S_*
 * in neos_bar.h), so the two can never disagree about whether the pack is in
 * danger.
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_bar.h"
#include "neos_panel.h"
#include "neos_sys.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "battpanel";

#define POLL_MS 40
#define TICK_MS 500     /* matches the bar's own widget tick, for the same flash rate */

#define MARGIN  16
#define ICON_W  96
#define ROW_H   48
#define GAP     10

/* Room beside the plot for the two voltage labels, "8.21" in the small font. */
#define PLOT_GUTTER      96
/*
 * The narrowest the y axis is allowed to get, in mV.
 *
 * A pack sitting still moves by a few millivolts, and an axis fitted to just
 * that would magnify sensor noise into what looks like a collapsing battery.
 * Floor the span so a flat pack plots as a flat line.
 */
#define PLOT_MIN_SPAN_MV 400

static ngl_rect_t s_panel, s_content;
static ngl_rect_t s_icon, s_volt, s_current_row, s_power_row, s_status_row;
static ngl_rect_t s_source_row, s_remain_row;
static ngl_rect_t s_plot;

/*
 * What the plot draws, one entry per column.
 *
 * Not one per sample: the record is two days at ten seconds, which is 34 KB, and
 * a panel that copied all of it out to draw six hundred pixels would be copying
 * 34 KB several times a second while a finger drags the trace about. NeOS
 * averages the window down to however many columns are asked for - see
 * neos_battery_history_avg() - so this is as wide as the widest plot the panel
 * can be given and never wider.
 *
 * Lifted out of the frame for the original reason too: this runs on the ui task,
 * which is also the one with lwIP underneath it, and two kilobytes of stack is
 * not free there.
 */
#define PLOT_MAX_COLS 1280
static uint16_t s_cols[PLOT_MAX_COLS];

static void layout(void)
{
    const ngl_rect_t app = ngl_app_area();
    s_panel = ngl_rect((int16_t)(app.x + 12), (int16_t)(app.y + 12),
                       (int16_t)(app.w - 24), (int16_t)(app.h - 24));

    s_content = ngl_rect((int16_t)(s_panel.x + 2),
                         (int16_t)(s_panel.y + NEOS_UI_TITLE_H + 1),
                         (int16_t)(s_panel.w - 4),
                         (int16_t)(s_panel.h - NEOS_UI_TITLE_H - 3));

    const int16_t x = (int16_t)(s_content.x + MARGIN);
    const int16_t w = (int16_t)(s_content.w - 2 * MARGIN);
    int16_t y = (int16_t)(s_content.y + 16);

    s_icon = ngl_rect(x, y, ICON_W, ICON_W);
    s_volt = ngl_rect((int16_t)(x + ICON_W + GAP), y,
                      (int16_t)(w - ICON_W - GAP), ICON_W);
    y = (int16_t)(y + ICON_W + GAP);

    s_current_row = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_power_row   = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_source_row  = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_remain_row  = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H);
    s_status_row  = ngl_rect(x, y, w, ROW_H); y = (int16_t)(y + ROW_H + GAP);

    /* Whatever is left under the rows. Clamped rather than assumed: the app
       area changes shape when the tablet is turned over, and a negative height
       here would be a rectangle that wraps instead of a plot that shrinks. */
    int16_t ph = (int16_t)(s_content.y + s_content.h - MARGIN - y);
    if (ph < 0) {
        ph = 0;
    }
    s_plot = ngl_rect(x, y, w, ph);
}

/* ------------------------------------------------------------------ */

static ngl_color_t level_color(bool danger, bool warn)
{
    return danger ? TH_BAD : (warn ? TH_WARN : TH_OK);
}

static void paint_icon(ngl_surface_t *s, bool have, bool danger, bool warn, bool blink_on)
{
    ngl_fill_rect(s, s_icon, TH_MODAL_BG);
    if (!have || (danger && !blink_on)) {
        return;
    }
    const ngl_icon_t *ic = ngl_icon_find("battery", 32);
    if (!ic) {
        return;
    }
    ngl_icon(s, (int16_t)(s_icon.x + (s_icon.w - ic->w) / 2),
             (int16_t)(s_icon.y + (s_icon.h - ic->h) / 2), ic,
             level_color(danger, warn));
}

static void paint_volt(ngl_surface_t *s, bool have, int32_t bus_mv,
                       bool danger, bool warn, bool blink_on)
{
    ngl_fill_rect(s, s_volt, TH_MODAL_BG);

    char buf[16];
    if (!have) {
        snprintf(buf, sizeof(buf), "%s",
                 neos_battery_monitor_ok() ? "no battery" : "no reading");
        neos_ui_text_left(s, s_volt, buf, &ngl_font_large, TH_TEXT_FAINT);
        return;
    }
    if (danger && !blink_on) {
        return;   /* the "off" half of the flash, same as the bar icon */
    }
    snprintf(buf, sizeof(buf), "%d.%02d V", (int)(bus_mv / 1000), (int)((bus_mv % 1000) / 10));
    neos_ui_text_left(s, s_volt, buf, &ngl_font_large, level_color(danger, warn));
}

/*
 * The percentage, beside the volts rather than in a row of its own.
 *
 * It is the same measurement said the other way round, and putting it on the
 * same line is what stops it reading as a second, independent opinion about
 * the battery.
 */
static void paint_pct(ngl_surface_t *s, bool have, const neos_power_t *p,
                      bool danger, bool warn, bool blink_on)
{
    if (!have || (danger && !blink_on)) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", neos_battery_soc(p));
    neos_ui_text_right(s, s_volt, buf, &ngl_font_large, level_color(danger, warn));
}

static void paint_source(ngl_surface_t *s, bool have)
{
    ngl_fill_rect(s, s_source_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_source_row, "Source", &ngl_font_small, TH_TEXT_DIM);

    const char *msg = "--";
    ngl_color_t c   = TH_TEXT_FAINT;
    if (have) {
        switch (neos_battery_source()) {
        case NEOS_PWR_BATTERY:
            msg = "on battery";     c = TH_TEXT;  break;
        case NEOS_PWR_CHARGING:
            msg = "charging";       c = TH_OK;    break;
        case NEOS_PWR_EXTERNAL:
            /*
             * Named for what was measured. The pack is neither supplying the
             * tablet nor taking anything, which on this board is what a
             * charger with charging switched off looks like - so the label
             * says the pack is idle rather than guessing why.
             */
            msg = "external, pack idle"; c = TH_TEXT_DIM; break;
        default:
            break;
        }
    }
    neos_ui_text_right(s, s_source_row, msg, &ngl_font_small, c);
}

static void paint_current(ngl_surface_t *s, bool have, int32_t current_ma)
{
    ngl_fill_rect(s, s_current_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_current_row, "Current", &ngl_font_small, TH_TEXT_DIM);

    char buf[24];
    if (have) {
        snprintf(buf, sizeof(buf), "%d mA", (int)current_ma);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    neos_ui_text_right(s, s_current_row, buf, &ngl_font_small, TH_TEXT);
}

static void paint_power(ngl_surface_t *s, bool have, int32_t power_mw)
{
    ngl_fill_rect(s, s_power_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_power_row, "Power", &ngl_font_small, TH_TEXT_DIM);

    char buf[24];
    if (have) {
        snprintf(buf, sizeof(buf), "%d mW", (int)power_mw);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    neos_ui_text_right(s, s_power_row, buf, &ngl_font_small, TH_TEXT);
}

#define RUNTIME_WINDOW 90     /* fit over the last 15 minutes of history */
#define RUNTIME_MIN    30     /* ... and say nothing before there are five */

/**
 * Fit a straight line through the recent record.
 *
 * @param mv_per_sample  slope, in millivolts per history period. Negative while
 *                       the pack is going down.
 * @param last_mv        the most recent reading the fit actually saw.
 * @return false when there is not enough of a record to fit anything, which is
 *         the first quarter of an hour after a boot and any stretch the monitor
 *         was quiet through.
 *
 * Shared by both estimates below. A discharge and a charge are the same
 * measurement with the sign flipped and a different destination, and doing the
 * least squares twice would be two chances to do it differently.
 */
static bool history_slope(float *mv_per_sample, int32_t *last_mv)
{
    const int n = neos_battery_history_count();
    if (n < RUNTIME_MIN) {
        return false;
    }

    /*
     * The last quarter of an hour at full resolution: one bucket per sample, so
     * nothing is averaged away under the fit. Asking for the window rather than
     * the record is what keeps this a 90-entry read of a two-day ring.
     */
    int first = n - RUNTIME_WINDOW;
    if (first < 0) {
        first = 0;
    }
    const int want = n - first;
    if (want > PLOT_MAX_COLS ||
        neos_battery_history_avg(s_cols, want, first, want) != want) {
        return false;
    }

    /* Least squares over the window, gaps skipped. x is the sample index,
       which is time in units of one history period. */
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    int   m = 0;
    int32_t seen = 0;
    for (int i = 0; i < want; i++) {
        if (!s_cols[i]) {
            continue;
        }
        const float x = (float)i;
        const float y = (float)s_cols[i];
        sx  += x;
        sy  += y;
        sxx += x * x;
        sxy += x * y;
        seen = s_cols[i];
        m++;
    }
    if (m < RUNTIME_MIN) {
        return false;
    }

    const float denom = (float)m * sxx - sx * sx;
    if (denom <= 0.0f) {
        return false;
    }
    *mv_per_sample = ((float)m * sxy - sx * sy) / denom;
    *last_mv       = seen;
    return true;
}

/**
 * Percent per minute, from a slope in millivolts per sample.
 *
 * The charge curve is what makes this more than a unit conversion: the same
 * millivolts per minute is worth very different amounts of charge at 7.9 V and
 * at 7.1 V, so the slope has to be taken through the curve's local steepness at
 * the voltage the pack is actually at. Ruling the voltage line straight down to
 * the danger mark instead reads hours too long in the middle and minutes too
 * short at the end - which is the failure people already expect from battery
 * meters.
 */
static bool pct_per_min(float mv_per_sample, int32_t at_mv, float *out)
{
    /*
     * The curve's local steepness, percent per millivolt, measured across a
     * 100 mV span centred on where the pack is now rather than differentiated
     * - the table is piecewise linear and has corners, and a derivative taken
     * exactly on one of them is whichever side it happened to land.
     */
    const int span_pct = neos_battery_soc_mv(at_mv + 50) -
                         neos_battery_soc_mv(at_mv - 50);
    if (span_pct <= 0) {
        return false;
    }

    const float per_min = 60000.0f / (float)NEOS_BATTERY_HIST_PERIOD_MS;
    *out = mv_per_sample * ((float)span_pct / 100.0f) * per_min;
    return true;
}

/** A float count of minutes, clamped to something a row can say. */
static bool minutes_from(float mins, int *minutes)
{
    if (mins < 0.0f) {
        return false;
    }
    if (mins > 99.0f * 60.0f) {
        mins = 99.0f * 60.0f;   /* a rate this slow means "ages", not a number */
    }
    *minutes = (int)mins;
    return true;
}

/*
 * How long the pack has left, from how fast it is actually going down.
 *
 * Not from a configured capacity. This board does not report the pack's mAh and
 * nothing in the tree records it, so a runtime built on a number somebody
 * guessed would be wrong by whatever that guess was wrong by, invisibly. Either
 * the capacity has been learned from a real discharge, in which case the current
 * draw answers it directly and responds the moment the load changes, or the
 * history is the measurement: fit the recent slope, take it through the charge
 * curve, and divide what is left by it.
 */
static bool runtime_left(int soc_now, const neos_power_t *p, int *minutes)
{
    const int32_t cap = neos_battery_capacity_mah();
    if (cap > 0 && p && p->current_ma > 0) {
        const int32_t left_mah = cap * soc_now / 100;
        const int32_t mins     = left_mah * 60 / p->current_ma;
        *minutes = (int)(mins > 99 * 60 ? 99 * 60 : mins);
        return true;
    }

    float   slope = 0;
    int32_t at_mv = 0;
    if (!history_slope(&slope, &at_mv) || slope >= 0.0f) {
        return false;           /* flat or rising: nothing to extrapolate */
    }

    float per_min = 0;
    if (!pct_per_min(slope, at_mv, &per_min) || per_min >= -0.0001f) {
        return false;
    }
    return minutes_from((float)soc_now / -per_min, minutes);
}

/*
 * How long until the pack is full, which is not the same shape of question.
 *
 * A lithium pack is charged in two phases and only the first is a constant
 * current: once the terminals reach the top of the range the charger holds the
 * voltage there and the current tapers away, so the last tenth of the charge
 * goes in at a fraction of the rate the first nine tenths did. Dividing what is
 * missing by the present current is therefore right until about ninety percent
 * and then reads badly short - which is the half of a charging estimate people
 * actually watch.
 *
 * So the capacity path splits the remaining charge at that knee and puts the
 * tail in at a third of the present current, which is about what the taper
 * averages. It is a model and not a measurement, and it is the reason the slope
 * path below is preferred when there is one: a fitted slope flattens out on its
 * own as the taper starts, because the pack really is rising more slowly.
 */
#define CHARGE_CV_KNEE_PCT 90
#define CHARGE_CV_TAPER    3

static bool charge_left(int soc_now, const neos_power_t *p, int *minutes)
{
    float   slope = 0;
    int32_t at_mv = 0;
    float   per_min = 0;

    /*
     * The slope first, the opposite way round from the discharge estimate.
     *
     * Charging is the case where the measurement beats the model: the taper is
     * visible in the record as a slope that is flattening, and an estimate taken
     * off it slows down with the pack instead of having to be told that packs
     * slow down. It needs a quarter of an hour of charging to say anything,
     * which is what the capacity path below covers.
     */
    if (history_slope(&slope, &at_mv) && slope > 0.0f &&
        pct_per_min(slope, at_mv, &per_min) && per_min > 0.0001f) {
        return minutes_from((float)(100 - soc_now) / per_min, minutes);
    }

    const int32_t cap = neos_battery_capacity_mah();
    const int32_t in_ma = p ? -p->current_ma : 0;     /* into the pack */
    if (cap <= 0 || in_ma <= 0) {
        return false;
    }

    int cc_pct = CHARGE_CV_KNEE_PCT - soc_now;        /* at the present current */
    if (cc_pct < 0) {
        cc_pct = 0;
    }
    int cv_pct = 100 - (soc_now > CHARGE_CV_KNEE_PCT ? soc_now : CHARGE_CV_KNEE_PCT);
    if (cv_pct < 0) {
        cv_pct = 0;
    }

    const int32_t mins = (cap * cc_pct / 100) * 60 / in_ma
                       + (cap * cv_pct / 100) * 60 * CHARGE_CV_TAPER / in_ma;
    *minutes = (int)(mins > 99 * 60 ? 99 * 60 : mins);
    return true;
}

/*
 * The one row that answers a different question depending on which way the
 * charge is flowing.
 *
 * Time left and time to full are the same row because they are the same thing to
 * whoever is looking at it - "how long" - and because only one of them can be
 * true at once. The label changes with it rather than being a heading over both:
 * a row labelled "Remaining" showing the time until a charge finishes is a row
 * that will be misread, once, by somebody deciding whether to unplug it.
 */
static void paint_remaining(ngl_surface_t *s, bool have, const neos_power_t *p)
{
    const neos_power_src_t src = have ? neos_battery_source() : NEOS_PWR_UNKNOWN;
    const bool charging = (src == NEOS_PWR_CHARGING);

    ngl_fill_rect(s, s_remain_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_remain_row, charging ? "Until full" : "Remaining",
                      &ngl_font_small, TH_TEXT_DIM);

    char        buf[48];
    const char *msg;
    ngl_color_t c = TH_TEXT_FAINT;
    int         mins = 0;
    const int   soc = have ? neos_battery_soc(p) : 0;

    if (!have) {
        msg = "--";
    } else if (charging) {
        if (soc >= 99) {
            /* The curve tops out before the charger does. Saying five more
               minutes at this point would be a number about the model rather
               than about the pack. */
            msg = "topping off";
            c   = TH_OK;
        } else if (charge_left(soc, p, &mins)) {
            snprintf(buf, sizeof(buf), "%dh %02dm", mins / 60, mins % 60);
            msg = buf;
            c   = TH_OK;
        } else {
            /* Honest about which half is missing: the slope has not had long
               enough yet and the pack has never taught us its capacity. */
            msg = "measuring";
        }
    } else if (src != NEOS_PWR_BATTERY) {
        msg = "on external power";
    } else if (!runtime_left(soc, p, &mins)) {
        msg = "measuring";
    } else {
        snprintf(buf, sizeof(buf), "%dh %02dm", mins / 60, mins % 60);
        msg = buf;
        c   = TH_TEXT;
    }
    neos_ui_text_right(s, s_remain_row, msg, &ngl_font_small, c);
}

static void paint_status(ngl_surface_t *s, bool have, bool danger, bool warn)
{
    ngl_fill_rect(s, s_status_row, TH_MODAL_BG);
    neos_ui_text_left(s, s_status_row, "Status", &ngl_font_small, TH_TEXT_DIM);

    const char *msg;
    ngl_color_t c;
    if (!have) {
        /* Two different faults that both read as nothing: no monitor on the
           board at all, or a monitor answering off an empty rail because the
           pack has been taken out. */
        msg = neos_battery_monitor_ok() ? "no battery" : "power monitor not found";
        c   = TH_TEXT_FAINT;
    } else if (danger) {
        msg = "DANGER - charge the pack now";
        c   = TH_BAD;
    } else if (warn) {
        msg = "getting low - charge soon";
        c   = TH_WARN;
    } else {
        msg = "healthy";
        c   = TH_OK;
    }
    neos_ui_text_right(s, s_status_row, msg, &ngl_font_small, c);
}

/* ------------------------------------------------------------------ */
/* The plot                                                            */
/* ------------------------------------------------------------------ */

/*
 * What part of the record is on screen.
 *
 * The whole hour at once is the right default and the wrong thing to be stuck
 * with: the interesting part of a discharge is usually a few minutes wide - the
 * moment a charger came out, the dip when the radio joined a network - and at
 * three hundred and sixty points across five hundred pixels that is four pixels
 * of trace. So the view is a window over the record rather than the record:
 * pinch changes how wide it is, a drag moves it, and the y axis re-fits to
 * whatever is inside it.
 *
 * Held in samples rather than in pixels or minutes, because samples are what the
 * history is indexed by and the other two are derived. s_view_skip counts the
 * newest samples pushed off the right-hand edge, so zero is "following the
 * present" and that is also the state the plot returns to on its own whenever
 * the window is as wide as the record.
 */
#define PLOT_MIN_SAMPLES 12      /* two minutes: below this there is no trend */

/*
 * The window the plot opens at, and it is not the whole record.
 *
 * The record is two days. Two days across six hundred pixels is five minutes a
 * column, which is the right view for "what has this pack been doing since
 * Tuesday" and the wrong one for every other question - and for the first two
 * days after a boot it would be a stub of trace against an empty axis. An hour
 * is what fills the plot within a minute of switching the tablet on and still
 * shows a discharge bending. The rest is a pinch away, and the label says which
 * it is.
 */
#define PLOT_DEFAULT_SAMPLES (3600000 / NEOS_BATTERY_HIST_PERIOD_MS)

static int s_view_n    = PLOT_DEFAULT_SAMPLES;
static int s_view_skip;

/** The window, reset to the hour it opens at, following live. */
static void view_reset(void)
{
    s_view_n    = PLOT_DEFAULT_SAMPLES;
    s_view_skip = 0;
}

/** Keep the window inside a record of @p n samples. */
static void view_clamp(int n)
{
    if (s_view_n < PLOT_MIN_SAMPLES)        { s_view_n = PLOT_MIN_SAMPLES; }
    if (s_view_n > NEOS_BATTERY_HIST_N)     { s_view_n = NEOS_BATTERY_HIST_N; }

    /*
     * Panning stops where the record does. The oldest sample the window may put
     * against its right edge is the one that still leaves a window's worth of
     * record to its left - anything further back is scrolling into a past that
     * was never recorded.
     */
    int max_skip = n - s_view_n;
    if (max_skip < 0) {
        max_skip = 0;
    }
    if (s_view_skip > max_skip) { s_view_skip = max_skip; }
    if (s_view_skip < 0)        { s_view_skip = 0; }
}

static bool view_is_default(void)
{
    return s_view_skip == 0 && s_view_n == PLOT_DEFAULT_SAMPLES;
}

/** The framed area of the plot: everything right of the label gutter. */
static ngl_rect_t plot_box(void)
{
    return ngl_rect((int16_t)(s_plot.x + PLOT_GUTTER), s_plot.y,
                    (int16_t)(s_plot.w - PLOT_GUTTER), s_plot.h);
}

/** Where the "fit" button sits, inside the top right of the frame. */
static ngl_rect_t fit_rect(void)
{
    const ngl_rect_t box = plot_box();
    const int16_t w = 72, h = 34;
    return ngl_rect((int16_t)(box.x + box.w - w - 6), (int16_t)(box.y + 6), w, h);
}

/*
 * Millivolts to a row inside the box, clamped.
 *
 * The clamp is what lets the caller hand this a threshold that is off the top
 * of the axis without checking first - it lands on the edge instead of being
 * drawn outside the frame.
 */
static int16_t plot_y(const ngl_rect_t *box, int32_t mv, int32_t lo, int32_t span)
{
    int32_t t = (mv - lo) * (box->h - 1) / span;
    if (t < 0) {
        t = 0;
    }
    if (t > box->h - 1) {
        t = box->h - 1;
    }
    return (int16_t)(box->y + box->h - 1 - t);
}

/*
 * Sample index to a column, within the window.
 *
 * The newest visible sample is hard against the right edge and the window is
 * s_view_n samples wide whether or not that much has been recorded, so the axis
 * always means the same stretch of time and a fresh boot grows a short trace
 * leftward into it. Stretching what exists to fill the box would fill the plot
 * immediately and quietly redefine the x axis every ten seconds, which is the
 * one thing a time axis must not do.
 */
static int16_t plot_x(const ngl_rect_t *box, int col, int cols)
{
    if (cols < 2) {
        return (int16_t)(box->x + box->w - 1);
    }
    const int32_t t = (int32_t)col * (box->w - 1) / (cols - 1);
    return (int16_t)(box->x + t);
}

/** The inverse: how many samples back from the right edge column @p x is. */
static int plot_age_at(const ngl_rect_t *box, int16_t x)
{
    const int32_t span = (s_view_n > 1) ? (s_view_n - 1) : 1;
    const int32_t dx   = (int32_t)(box->x + box->w - 1 - x);
    if (box->w <= 1) {
        return 0;
    }
    return (int)((dx * span) / (box->w - 1));
}

/* ------------------------------------------------------------------ */
/* Gestures                                                           */
/* ------------------------------------------------------------------ */

/*
 * Two fingers change the scale, one finger moves the window, and both are read
 * from the live point set rather than from taps.
 *
 * Taps are the wrong end of it for the same reason a slider does not wait for a
 * release: a plot that only moved once the finger came off would be a plot you
 * had to aim. The gesture is anchored at its start - the span between the
 * fingers, the window width at that moment, and which sample was under the
 * midpoint - so the whole pinch is one continuous transform of one starting
 * state instead of a hundred increments that drift.
 */
static bool  s_g_active;
static int   s_g_fingers;
static int   s_g_span0;        /* pixels between the fingers when it started */
static int   s_g_view_n0;
static int   s_g_age0;         /* sample under the anchor, as an age from the right */
static int16_t s_g_anchor;     /* the x that sample has to stay at */
static int   s_g_skip0;
static int16_t s_g_x0;

static void gesture_end(void)
{
    s_g_active  = false;
    s_g_fingers = 0;
}

/**
 * Advance a pinch or a drag. True while it owns the finger, which is also the
 * signal to repaint the plot.
 */
static bool plot_gesture(void)
{
    neos_touch_t pts[NEOS_TOUCH_MAX];
    const int n = neos_touch_points_os(pts, NEOS_TOUCH_MAX);
    const ngl_rect_t box = plot_box();

    if (n == 0 || box.w <= 1) {
        const bool was = s_g_active;
        gesture_end();
        return was;
    }

    /*
     * A gesture has to start inside the frame, and not on the one control that
     * is inside the frame. Starting outside it is somebody reaching for the
     * close box and the plot must not follow that finger; starting on the fit
     * button would make the button unpressable, because the tap that would have
     * worked it is the same tap this function drains as the end of a drag.
     */
    if (!s_g_active) {
        if (!ngl_rect_contains(&box, pts[0].x, pts[0].y)) {
            return false;
        }
        const ngl_rect_t fit = fit_rect();
        if (!view_is_default() && ngl_rect_contains(&fit, pts[0].x, pts[0].y)) {
            return false;
        }
        s_g_active  = true;
        s_g_fingers = 0;        /* set below, so a pinch and a drag both seed */
    }

    const int fingers = (n >= 2) ? 2 : 1;
    const bool restart = (fingers != s_g_fingers);

    if (fingers == 2) {
        int span = pts[0].x - pts[1].x;
        if (span < 0) {
            span = -span;
        }
        if (span < 16) {
            span = 16;          /* two fingers together is not an infinite zoom */
        }
        const int16_t mid = (int16_t)((pts[0].x + pts[1].x) / 2);

        if (restart) {
            s_g_fingers = 2;
            s_g_span0   = span;
            s_g_view_n0 = s_view_n;
            s_g_skip0   = s_view_skip;
            s_g_anchor  = mid;
            s_g_age0    = plot_age_at(&box, mid) + s_view_skip;
            return true;
        }

        /*
         * Fingers apart means fewer samples across the box. The window scales by
         * the inverse of the span so that the pinch is reversible: going back to
         * where the fingers started puts the scale back exactly, rather than
         * somewhere near it.
         */
        int want = (int)(((int64_t)s_g_view_n0 * s_g_span0) / span);
        if (want < PLOT_MIN_SAMPLES)    { want = PLOT_MIN_SAMPLES; }
        if (want > NEOS_BATTERY_HIST_N) { want = NEOS_BATTERY_HIST_N; }
        s_view_n = want;

        /* Keep the sample that was under the midpoint under the midpoint: a zoom
           that walked the data sideways would be a zoom nobody could aim. */
        s_view_skip = s_g_age0 - plot_age_at(&box, s_g_anchor);
        if (s_view_skip < 0) {
            s_view_skip = 0;
        }
        return true;
    }

    if (restart) {
        s_g_fingers = 1;
        s_g_x0      = pts[0].x;
        s_g_skip0   = s_view_skip;
        return true;
    }

    /*
     * Dragging right pulls older samples into view, which is the direction the
     * data moves under the finger rather than the direction the window moves
     * over it - the paper follows the hand.
     */
    const int32_t span = (s_view_n > 1) ? (s_view_n - 1) : 1;
    const int32_t dx   = (int32_t)(pts[0].x - s_g_x0);
    s_view_skip = s_g_skip0 + (int)((dx * span) / (box.w - 1));
    if (s_view_skip < 0) {
        s_view_skip = 0;
    }
    return true;
}

/* ------------------------------------------------------------------ */

/*
 * A round number of millivolts per gridline, chosen so the axis carries about
 * five of them.
 *
 * Five because that is as many horizontal rules as a plot this size can take
 * before they compete with the trace, and because a label every fifth of the
 * axis is enough to read a value off it by eye. The candidates are the steps a
 * person would pick - 50 mV, 100 mV, a quarter of a volt - so the labels land on
 * numbers like 7.50 rather than on 7.43.
 */
#define PLOT_STEPS 5

static int32_t nice_step(int32_t span)
{
    static const int32_t STEP[] = {
        5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000,
    };
    for (size_t i = 0; i < sizeof(STEP) / sizeof(STEP[0]); i++) {
        if (span / STEP[i] <= PLOT_STEPS) {
            return STEP[i];
        }
    }
    return STEP[sizeof(STEP) / sizeof(STEP[0]) - 1];
}

/** A voltage in the gutter, level with its gridline. */
static void volt_label(ngl_surface_t *s, int16_t y, int32_t mv, ngl_color_t c)
{
    /* Wide enough for the worst case an int can print, not for the volts this
       will actually hold - the truncation warning is fatal here. */
    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%02d", (int)(mv / 1000), (int)((mv % 1000) / 10));
    const ngl_rect_t r = ngl_rect(s_plot.x,
                                  (int16_t)(y - ngl_font_small.height / 2),
                                  (int16_t)(PLOT_GUTTER - 10), ngl_font_small.height);
    neos_ui_text_right(s, r, buf, &ngl_font_small, c);
}

/*
 * The grid: a dotted rule and a label at every round step across the axis.
 *
 * Dotted rather than solid, and in the rule colour rather than the trace's: a
 * grid is there to be read through. The labelled steps are also what makes the
 * auto-fitted y axis honest - an axis that silently rescales itself needs to say
 * what it has rescaled to, or a pack that moved by twenty millivolts looks
 * exactly like one that moved by two hundred.
 */
static void paint_grid(ngl_surface_t *s, const ngl_rect_t *box,
                       int32_t lo, int32_t hi, int32_t span)
{
    const int32_t step = nice_step(span);
    int32_t mv = ((lo + step - 1) / step) * step;    /* first multiple above lo */

    for (; mv <= hi; mv += step) {
        const int16_t y = plot_y(box, mv, lo, span);
        for (int16_t x = (int16_t)(box->x + 1); x < (int16_t)(box->x + box->w - 1);
             x = (int16_t)(x + 6)) {
            ngl_pixel(s, x, y, TH_RULE);
        }
        volt_label(s, y, mv, TH_TEXT_FAINT);
    }
}

/* A threshold, dashed so it reads as a reference and not as recorded data. */
static void plot_level(ngl_surface_t *s, const ngl_rect_t *box, int32_t mv,
                       int32_t lo, int32_t hi, int32_t span, ngl_color_t c)
{
    if (mv < lo || mv > hi) {
        return;         /* off the axis: drawn at the edge it would be a lie */
    }
    const int16_t y = plot_y(box, mv, lo, span);
    for (int16_t x = box->x; x < (int16_t)(box->x + box->w); x = (int16_t)(x + 8)) {
        ngl_hline(s, x, y, 4, c);
    }
}

/** How wide the window is, and how far back its right edge is, as text. */
static void paint_axis_labels(ngl_surface_t *s, const ngl_rect_t *box)
{
    const int32_t per_min = 60000 / NEOS_BATTERY_HIST_PERIOD_MS;
    const int span_min = (int)((s_view_n + per_min - 1) / per_min);
    const int back_min = (int)(s_view_skip / per_min);

    char buf[32];
    if (span_min >= 60) {
        snprintf(buf, sizeof(buf), "%dh %02dm", span_min / 60, span_min % 60);
    } else {
        snprintf(buf, sizeof(buf), "%d min", span_min);
    }

    /* Two days back is a distance nobody counts in minutes. */
    char back[24];
    if (back_min >= 120) {
        snprintf(back, sizeof(back), "-%dh %02dm", back_min / 60, back_min % 60);
    } else {
        snprintf(back, sizeof(back), "-%d min", back_min);
    }

    const ngl_rect_t r = ngl_rect((int16_t)(box->x + 6),
                                  (int16_t)(box->y + box->h - ngl_font_small.height - 4),
                                  (int16_t)(box->w - 12), ngl_font_small.height);
    neos_ui_text_left(s, r, buf, &ngl_font_small, TH_TEXT_FAINT);

    /* Only when it is not the present. "now" needs no label; four minutes ago
       does, or the trace is a shape with no time attached to it. */
    if (back_min > 0) {
        neos_ui_text_right(s, r, back, &ngl_font_small, TH_ACCENT);
    }
}

static void paint_plot(ngl_surface_t *s)
{
    ngl_fill_rect(s, s_plot, TH_MODAL_BG);
    if (s_plot.w <= PLOT_GUTTER + 16 || s_plot.h < 48) {
        return;         /* turned on its side, and there is no room for one */
    }

    const ngl_rect_t box = plot_box();

    /* The frame before the data, so an empty plot still looks like a plot -
       "nothing recorded yet" and "nothing drawn" should not look the same. */
    ngl_draw_rect(s, box, TH_RULE, 1);

    const int n = neos_battery_history_count();
    view_clamp(n);

    /*
     * One bucket per column, or one per sample when the window holds fewer
     * samples than there are columns - there is nothing to be gained from
     * averaging a window into more buckets than it has readings, and drawing it
     * that way is what makes a zoomed-in trace a line between real points rather
     * than a staircase.
     *
     * The window may start before the record does, and deliberately is not
     * clamped to it: those buckets come back empty, so a fresh boot grows its
     * trace leftward into an axis that already means an hour instead of the axis
     * quietly meaning something new every ten seconds.
     */
    int cols = box.w - 2;
    if (cols > PLOT_MAX_COLS) { cols = PLOT_MAX_COLS; }
    if (cols > s_view_n)      { cols = s_view_n; }
    if (cols < 2)             { cols = 2; }

    const int first = n - s_view_skip - s_view_n;
    if (neos_battery_history_avg(s_cols, cols, first, s_view_n) != cols) {
        neos_ui_text_centred(s, box, "recording", &ngl_font_small, TH_TEXT_FAINT);
        return;
    }

    int32_t lo = INT32_MAX, hi = INT32_MIN;
    int have = 0;
    for (int i = 0; i < cols; i++) {
        if (!s_cols[i]) {
            continue;   /* nothing recorded in that column */
        }
        if (s_cols[i] < lo) { lo = s_cols[i]; }
        if (s_cols[i] > hi) { hi = s_cols[i]; }
        have++;
    }
    if (have < 2) {
        neos_ui_text_centred(s, box, n > 2 ? "nothing recorded in this window"
                                           : "recording",
                             &ngl_font_small, TH_TEXT_FAINT);
        paint_axis_labels(s, &box);
        return;
    }

    /*
     * The axis fits what is in the window, not what is in the record.
     *
     * That is the whole point of being able to zoom: a five-minute window over a
     * pack that moved thirty millivolts should show thirty millivolts of axis,
     * not the two volts the last hour covered. The floor below keeps that from
     * turning sensor noise into a cliff, and the grid says which is which.
     */
    if (hi - lo < PLOT_MIN_SPAN_MV) {
        const int32_t grow = (PLOT_MIN_SPAN_MV - (hi - lo)) / 2;
        lo -= grow;
        hi += grow;
    }
    const int32_t air = (hi - lo) / 10;     /* a little room above and below */
    lo -= air;
    hi += air;
    const int32_t span = hi - lo;

    paint_grid(s, &box, lo, hi, span);
    plot_level(s, &box, NEOS_BATTERY_2S_WARN_MV,   lo, hi, span, TH_WARN);
    plot_level(s, &box, NEOS_BATTERY_2S_DANGER_MV, lo, hi, span, TH_BAD);

    /* The trace, broken wherever the monitor went quiet rather than ruled
       straight across the gap as if it had been reading all along. */
    int16_t px = 0, py = 0;
    bool prev = false;
    for (int i = 0; i < cols; i++) {
        if (!s_cols[i]) {
            prev = false;
            continue;
        }
        const int16_t cx = plot_x(&box, i, cols);
        const int16_t cy = plot_y(&box, s_cols[i], lo, span);
        if (prev) {
            ngl_line_aa(s, px, py, cx, cy, TH_ACCENT);
        } else {
            ngl_pixel(s, cx, cy, TH_ACCENT);
        }
        px = cx;
        py = cy;
        prev = true;
    }

    paint_axis_labels(s, &box);

    /* The way back, and only while there is somewhere to go back to. A button
       that does nothing is worse than no button, and the default view is the
       state in which this one would. */
    if (!view_is_default()) {
        neos_ui_button(s, fit_rect(), "fit", false);
    }
}

/* ------------------------------------------------------------------ */

void neos_panel_battery(void)
{
    if (!ngl_screen()) {
        return;
    }
    layout();

    /*
     * A fresh window every time the page is opened.
     *
     * Where the plot was left zoomed is state about a gesture, not about the
     * pack, and coming back to a page an hour later to find it showing four
     * minutes from last time would read as a plot that had stopped recording.
     */
    view_reset();
    gesture_end();

    neos_power_t p = {0};
    bool have = neos_power_read(&p);
    bool danger = have && p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;
    bool warn   = have && !danger && p.bus_mv < NEOS_BATTERY_2S_WARN_MV;
    bool blink_on = true;

    ngl_overlay_restore();
    ngl_surface_t *s = ngl_overlay_begin(ngl_app_area());
    if (s) {
        neos_ui_dim(s);
        neos_ui_frame(s, s_panel, "Battery");
        paint_icon(s, have, danger, warn, blink_on);
        paint_volt(s, have, p.bus_mv, danger, warn, blink_on);
        paint_pct(s, have, &p, danger, warn, blink_on);
        paint_current(s, have, p.current_ma);
        paint_power(s, have, p.power_mw);
        paint_source(s, have);
        paint_remaining(s, have, &p);
        paint_status(s, have, danger, warn);
        paint_plot(s);
        ngl_overlay_end();
        ngl_flush();
    }

    uint32_t since = 0;
    ngl_rect_t area = ngl_app_area();
    uint32_t seq = neos_battery_history_seq();

    bool plot_moved = false;
    int  drain = 0;

    while (!neos_ui_should_close()) {
        /*
         * The gesture first, and its answer decides whether the tap below is a
         * tap at all: a drag ends with a finger coming off the glass, and that
         * looks exactly like a tap on whatever was under it. A zoom must not
         * finish by pressing the fit button or closing the panel.
         *
         * It has to outlast the gesture by a poll or two. The touch task
         * publishes "no fingers" before it publishes the tap that release
         * produced, so the poll that sees the gesture end can be the poll before
         * the tap exists - draining only while a finger is down would let
         * exactly one tap through, at the end of every drag.
         */
        const bool dragging = plot_gesture();
        if (dragging) {
            plot_moved = true;
            drain = 3;
        }
        if (drain > 0) {
            drain--;
            neos_touch_tap_os(NULL, NULL);
        }

        int16_t tx = 0, ty = 0;
        if (drain == 0 && neos_touch_tap_os(&tx, &ty)) {
            const ngl_rect_t close = neos_ui_close_rect(s_panel);
            if (ngl_rect_contains(&close, tx, ty)) {
                break;
            }
            const ngl_rect_t fit = fit_rect();
            if (!view_is_default() && ngl_rect_contains(&fit, tx, ty)) {
                view_reset();
                plot_moved = true;
            }
        }

        bool all = false;
        const ngl_rect_t a = ngl_app_area();
        if (a.x != area.x || a.y != area.y || a.w != area.w || a.h != area.h) {
            area = a;
            layout();
            all = true;
        }

        since += POLL_MS;
        const bool tick = since >= TICK_MS;
        if (tick) {
            since = 0;
        }

        /*
         * A moved plot is repainted now, not on the next tick. Half a second
         * between the finger and the trace is the difference between dragging a
         * plot and asking one to move.
         */
        if (plot_moved && !tick && !all) {
            ngl_surface_t *pg = ngl_overlay_begin(s_plot);
            if (pg) {
                paint_plot(pg);
                ngl_overlay_end();
                ngl_flush();
            }
            plot_moved = false;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        if (!tick && !all) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }

        have = neos_power_read(&p);
        danger = have && p.bus_mv > 0 && p.bus_mv < NEOS_BATTERY_2S_DANGER_MV;
        warn   = have && !danger && p.bus_mv < NEOS_BATTERY_2S_WARN_MV;

        /* Flip every tick while in danger, same as the bar icon, so the two
           flash in step instead of at odds with each other. */
        if (danger) {
            blink_on = !blink_on;
        } else {
            blink_on = true;
        }

        ngl_surface_t *sc;
        if (all) {
            ngl_overlay_restore();
            sc = ngl_overlay_begin(ngl_app_area());
        } else {
            sc = ngl_overlay_begin(s_panel);
        }
        if (sc) {
            if (all) {
                neos_ui_dim(sc);
                neos_ui_frame(sc, s_panel, "Battery");
            }
            paint_icon(sc, have, danger, warn, blink_on);
            paint_volt(sc, have, p.bus_mv, danger, warn, blink_on);
            paint_pct(sc, have, &p, danger, warn, blink_on);
            paint_current(sc, have, p.current_ma);
            paint_power(sc, have, p.power_mw);
            paint_source(sc, have);
            paint_remaining(sc, have, &p);
            paint_status(sc, have, danger, warn);
            /*
             * Only when there is a new point. The rows above move twice a
             * second, the trace once every ten, and redrawing three hundred
             * line segments for pixels that cannot have changed is the
             * expensive half of this panel.
             */
            const uint32_t now_seq = neos_battery_history_seq();
            if (all || plot_moved || now_seq != seq) {
                seq = now_seq;
                plot_moved = false;
                paint_plot(sc);
            }
            ngl_overlay_end();
            ngl_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    ESP_LOGI(TAG, "battery page closed");
}
