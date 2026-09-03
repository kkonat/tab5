/*
 * fractal.c - the per-pixel half of MANDEL.
 *
 * Three kernels, one raster, and the search that decides where to point them
 * when the user has not said.
 *
 * Nothing here touches the panel. fr_at() answers one question about one
 * pixel - is this outside, and how far - and mandel_app.c turns the answer
 * into a colour. That split is what lets the location search render a
 * 32-pixel-wide preview through exactly the same code that later fills the
 * screen: the preview is not an approximation of the picture, it IS the
 * picture, sampled coarsely.
 *
 * All three kernels return the same thing: FR_INSIDE for a point in the set
 * (or, for Lyapunov, a chaotic one), and otherwise a value that grows
 * smoothly with distance from the boundary. Smoothly matters - an escape
 * count is an integer, and an integer painted through a 255-colour cycle
 * gives the concentric banding that makes a fractal look like a contour map.
 * The fractional part costs two log2s per escaped pixel and removes it.
 *
 * Everything is float, on purpose and under duress in equal measure. The P4
 * has an FPU and no double runtime; see the note at the top of mandel.h.
 */

#include "mandel.h"

/* ==================================================================== maths */

static uint32_t s_rng = 0x2545F491u;

void rnd_seed(uint32_t s) { s_rng = s ? s : 0x2545F491u; }

uint32_t rnd(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

int rnd_range(int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* 24 bits is exactly a float's mantissa: anything finer would be thrown away
   by the conversion anyway. */
float rnd_unit(void)
{
    return (float)(rnd() >> 8) * (1.0f / 16777216.0f);
}

static float rnd_between(float lo, float hi)
{
    return hi <= lo ? lo : lo + (hi - lo) * rnd_unit();
}

/*
 * sin over a whole turn, folded onto [0, 1/4] and then a Taylor series to
 * x^9, measured at under 4e-6 across the quarter. Accuracy matters here in a way
 * it would not for an animation: a Julia parameter is picked by walking the
 * boundary of the main cardioid, and a coarse sine walks beside it instead of
 * on it, which is the difference between a dendrite and a blob.
 */
float fsin_turn(float t)
{
    t = t - (float)(int)t;
    if (t < 0.0f) {
        t += 1.0f;
    }
    float sign = 1.0f;
    if (t >= 0.5f) { t -= 0.5f; sign = -1.0f; }
    if (t > 0.25f) { t = 0.5f - t; }

    const float x  = t * 6.28318531f;
    const float x2 = x * x;
    const float r  = x * (1.0f + x2 * (-1.0f / 6.0f
                            + x2 * (1.0f / 120.0f
                            + x2 * (-1.0f / 5040.0f
                            + x2 * (1.0f / 362880.0f)))));
    return sign * r;
}

/* ==================================================================== scene */

static uint8_t  s_mode;
static uint16_t s_maxiter = 100;
static float    s_jx, s_jy;
static uint8_t  s_seq[SEQ_MAX];
static uint8_t  s_len = 2;

static float s_cx, s_cy, s_hw;      /* the view, in world units */
static float s_x0, s_y0, s_step;    /* the raster it is currently mapped to */
static int   s_vw = 1, s_vh = 1;    /* and that raster's size */

/*
 * The escape radius, squared. Two would do to decide whether a point leaves,
 * but the smooth count is read off how far the last iteration overshot, and
 * from a bailout of 2 that overshoot is a coarse thing. At 256 the estimate is
 * good to a few thousandths of an iteration, for the cost of about two extra
 * iterations per escaped pixel - which is nothing next to the hundreds spent
 * on the pixels that do not escape at all.
 */
#define BAILOUT2   65536.0f
#define LOG2_LOG2B 3.0f             /* log2(log2(256)) */

/* Lyapunov settles before it measures: the first LY_WARM steps of the map are
   thrown away so the exponent is taken on the attractor rather than on the
   way to it, and LY_ITER of them are averaged. */
#define LY_WARM  30
#define LY_ITER  70

void fr_scene(const scene_t *s)
{
    s_mode    = s->mode;
    s_maxiter = s->maxiter ? s->maxiter : 100;
    s_jx      = s->jx;
    s_jy      = s->jy;
    s_cx      = s->cx;
    s_cy      = s->cy;
    s_hw      = s->hw > 0.0f ? s->hw : 1.6f;
    s_len     = s->seq_len ? s->seq_len : 2;
    if (s_len > SEQ_MAX) {
        s_len = SEQ_MAX;
    }
    for (int i = 0; i < SEQ_MAX; i++) {
        s_seq[i] = (uint8_t)(s->seq[i] & 1);
    }

    fr_view(s_vw, s_vh);            /* keep the mapping valid meanwhile */
}

void fr_view(int w, int h)
{
    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    s_vw = w;
    s_vh = h;

    /* One pitch for both axes, so a view is a rectangle of the world with the
       same shape as the panel and nothing is ever stretched. The half-pitch
       puts the sample in the middle of its pixel rather than on its corner,
       which is what makes a preview line up with the render it stands for. */
    s_step = (s_hw * 2.0f) / (float)w;
    s_x0 = s_cx - s_step * (float)w * 0.5f + s_step * 0.5f;
    s_y0 = s_cy - s_step * (float)h * 0.5f + s_step * 0.5f;
}

float fr_pitch(void) { return s_step; }

void fr_world(int px, int py, float *wx, float *wy)
{
    if (wx) { *wx = s_x0 + (float)px * s_step; }
    if (wy) { *wy = s_y0 + (float)py * s_step; }
}

/* ------------------------------------------------------------- the kernels */

/*
 * Escape time for z <- z*z + c, shared by both quadratic modes: Mandelbrot
 * hands it c = the pixel and z = 0, Julia hands it z = the pixel and a c that
 * is the same all over the screen. One kernel, because it is one iteration.
 *
 * Six flops and one compare per iteration, all of them single instructions on
 * this core. This loop is where the entire program spends its time; nothing
 * else in these three files is worth optimising until it is.
 */
static float escape(float cx, float cy, float zx, float zy)
{
    const int maxit = (int)s_maxiter;
    float x2 = zx * zx;
    float y2 = zy * zy;
    int   n  = 0;

    while (x2 + y2 <= BAILOUT2) {
        if (n >= maxit) {
            return FR_INSIDE;
        }
        zy = 2.0f * zx * zy + cy;
        zx = x2 - y2 + cx;
        x2 = zx * zx;
        y2 = zy * zy;
        n++;
    }

    /*
     * nu = n - log2(log2|z| / log2 B). The escaped |z| says how far past the
     * escape radius this last iteration overshot, and subtracting it makes
     * the value continuous across the whole exterior instead of stepping once
     * per iteration.
     */
    const float l2 = 0.5f * flog2_(x2 + y2);        /* log2|z| */
    return (float)n + LOG2_LOG2B - flog2_(l2);
}

/*
 * The main cardioid and the period-2 bulb, tested in closed form. They are
 * most of the black in a wide view and every point in them costs a full
 * maxiter otherwise, so this is the difference between a first screen in a
 * third of a second and one in two.
 */
static bool in_bulbs(float x, float y)
{
    const float y2 = y * y;
    const float xq = x - 0.25f;
    const float q  = xq * xq + y2;

    if (q * (q + xq) < 0.25f * y2) {
        return true;                                /* the cardioid */
    }
    const float xp = x + 1.0f;
    return (xp * xp + y2) < 0.0625f;                /* the period-2 bulb */
}

/*
 * Lyapunov. The logistic map is driven by a repeating word over two rates -
 * "ab", "aabab" - and the exponent is the average of log2|r(1-2x)| along the
 * orbit. Negative means the orbit settles, and those points are drawn;
 * positive means it never does, and chaos is painted as interior, because the
 * whole picture is the shape of the border between the two.
 *
 * The one value that needs care is the derivative when it lands on zero, and
 * flog2_(0) answering -60 turns that into a large negative term - which is
 * what a superstable orbit ought to contribute.
 */
static float lyapunov(float ra, float rb)
{
    float x   = 0.5f;
    float sum = 0.0f;
    int   k   = 0;

    for (int i = 0; i < LY_WARM + LY_ITER; i++) {
        const float r = s_seq[k] ? rb : ra;
        if (++k >= s_len) {
            k = 0;
        }
        if (i >= LY_WARM) {
            sum += flog2_(fabs_(r * (1.0f - 2.0f * x)));
        }
        x = r * x * (1.0f - x);
    }

    const float lam = sum * (1.0f / (float)LY_ITER);
    if (lam >= 0.0f) {
        return FR_INSIDE;                           /* chaotic */
    }
    /* Scaled so the ordered tongues span a range the palette can walk, the
       same way an escape count does. */
    return -lam * 100.0f;
}

/* ---------------------------------------------------------------- sampling */

static float sample(float a, float b)
{
    switch (s_mode) {
    case MODE_JULIA:  return escape(s_jx, s_jy, a, b);
    case MODE_LYAP:   return lyapunov(a, b);
    default:          return in_bulbs(a, b) ? FR_INSIDE : escape(a, b, 0.0f, 0.0f);
    }
}

float fr_at(int px, int py)
{
    return sample(s_x0 + (float)px * s_step, s_y0 + (float)py * s_step);
}

/*
 * The four quarter-points of one pixel. With the centre the caller already
 * has, that is a quincunx: five samples, and the cheapest arrangement that
 * puts one on each diagonal.
 *
 * They are handed back rather than combined here because the combination is
 * of COLOURS, and colour is not this file's business. Averaging the values
 * first would be the wrong operation and not much cheaper - halfway along a
 * filament the five values are scattered across the whole range, and the
 * colour of their average is not the average of their colours. It also could
 * not soften the edge of the set, where four of the five samples have no
 * value at all.
 */
void fr_at_ss4(int px, int py, float out[4])
{
    const float q = s_step * 0.25f;
    const float x = s_x0 + (float)px * s_step;
    const float y = s_y0 + (float)py * s_step;

    out[0] = sample(x - q, y - q);
    out[1] = sample(x + q, y - q);
    out[2] = sample(x - q, y + q);
    out[3] = sample(x + q, y + q);
}

/*
 * Is there a visible step in colour between these two pixels? Asked in
 * palette steps rather than in iterations, so that it means the same thing at
 * every zoom, in every mode and under every palette: the question is whether
 * the picture jumps here, and the palette is what decides that.
 */
int fr_jump(uint8_t a, uint8_t b)
{
    if ((a == 0) != (b == 0)) {
        return 1;                       /* one of them is interior */
    }
    int d = (int)a - (int)b;
    if (d < 0) { d = -d; }
    if (d > 128) { d = 256 - d; }       /* the cycle wraps */
    return d > FR_JUMP_STEPS;
}

/* =============================================================== the ranges */

float fr_home_hw(uint8_t mode)
{
    switch (mode) {
    case MODE_JULIA: return 1.5f;
    case MODE_LYAP:  return 0.95f;
    default:         return 1.6f;
    }
}

/*
 * Iterations worth spending at this zoom. The detail near the boundary gets
 * finer as you descend and a count that does not follow it turns filaments
 * into blobs; a count that overshoots only costs time on the black.
 */
uint16_t fr_iters_for(uint8_t mode, float hw)
{
    if (mode == MODE_LYAP) {
        return LY_ITER;                 /* fixed: it is an average, not a count */
    }
    if (hw <= 0.0f) {
        hw = fr_home_hw(mode);
    }
    float l = flog2_(fr_home_hw(mode) / hw);
    if (l < 0.0f) {
        l = 0.0f;
    }
    int it = 90 + (int)(l * 26.0f);
    if (it > MAXITER_CAP) {
        it = MAXITER_CAP;
    }
    return (uint16_t)it;
}

/*
 * What one trip round the palette should span, measured rather than assumed.
 *
 * Handing the palette the whole iteration range is right for a wide view and
 * badly wrong for a deep one: past a few hundred times in, every pixel on
 * screen escapes somewhere in the top fifth of the count, and a cycle
 * stretched over the whole range paints all of it in two adjacent colours.
 * The coarse pass has already sampled the frame by the time this is called,
 * so the range that is actually on screen is a fact and not a guess.
 *
 * The 97th percentile rather than the maximum: a handful of pixels deep in
 * one filament should not be allowed to flatten everything else.
 */
float fr_vmax_for(uint8_t mode, const float *coarse, int n, uint16_t maxiter)
{
    int   hist[64];
    int   cnt = 0;
    float hi  = 0.0f;

    for (int i = 0; i < 64; i++) {
        hist[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        if (coarse[i] >= 0.0f) {
            cnt++;
            if (coarse[i] > hi) {
                hi = coarse[i];
            }
        }
    }
    if (cnt < 32 || hi <= 0.0f) {
        /* Nothing but interior on screen, or nothing measured: fall back to
           the range the mode can produce at all. */
        return mode == MODE_LYAP ? 300.0f : (float)maxiter;
    }

    /* Binned on sqrt(v), because that is the axis the palette walks. */
    const float sh = fsqrt_(hi);
    const float k  = 63.0f / sh;
    for (int i = 0; i < n; i++) {
        if (coarse[i] >= 0.0f) {
            int b = (int)(fsqrt_(coarse[i]) * k);
            if (b < 0)  { b = 0; }
            if (b > 63) { b = 63; }
            hist[b]++;
        }
    }

    const int want = cnt - cnt / 32;
    int acc = 0, bin = 63;
    for (int i = 0; i < 64; i++) {
        acc += hist[i];
        if (acc >= want) {
            bin = i;
            break;
        }
    }

    const float s = ((float)(bin + 1) / 63.0f) * sh;
    const float v = s * s;
    return v < 1.0f ? 1.0f : v;
}

bool fr_valid(const scene_t *s)
{
    if (s->mode >= MODE_COUNT) {
        return false;
    }
    if (!(s->hw > 0.0f) || s->hw > 8.0f) {
        return false;
    }
    if (s->maxiter < 8 || s->maxiter > MAXITER_CAP) {
        return false;
    }
    if (s->cycles < 1 || s->cycles > 8) {
        return false;
    }
    return true;
}

/* ================================================================ the search */
/*
 * Pointing this somewhere at random gives a dull picture nearly every time:
 * the inside of the set is one flat colour, the far outside is another, and
 * the only place worth rendering is the hair between them. Two things find it.
 *
 * The first is a bisection. Take any point known to be inside and any point
 * known to be outside; the segment between them crosses the boundary, so
 * halve it until the ends are one screen-width apart and the crossing is
 * somewhere in the middle, at exactly the scale about to be drawn. That is a
 * guarantee rather than a hope, and it costs about forty escape tests.
 *
 * The second is the preview. Straddling the boundary is necessary and not
 * sufficient - a filament edge can be perfectly smooth and perfectly boring -
 * so each candidate is rendered 32 pixels wide through the real kernel and
 * scored for how much is actually going on.
 */

#define PRE_MAX     32
#define FIND_TRIES  40
#define FIND_MS     700
/*
 * The bar a candidate has to clear to be accepted on the spot. Scores run
 * from about 100 to about 210 in practice, and the two ends are easy to tell
 * apart by eye: 120 is a smooth wash with one filament across it, 200 is a
 * frame busy corner to corner. Whatever scored highest is used when the clock
 * runs out, so a demanding bar never means no picture.
 */
#define FIND_GOOD   175

static float s_prev[PRE_MAX * PRE_MAX];

static bool cls_inside(float x, float y) { return sample(x, y) == FR_INSIDE; }

/*
 * Land on the boundary at the scale hw. Returns false when the box handed in
 * has no interior at all - a dust Julia set has none - and the caller falls
 * back to a wide view, which is the right picture for that case anyway.
 */
static bool find_boundary(float hw, float *ox, float *oy,
                          float x_lo, float x_hi, float y_lo, float y_hi)
{
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    bool  have_in = false, have_out = false;

    for (int i = 0; i < 160 && !(have_in && have_out); i++) {
        const float x = rnd_between(x_lo, x_hi);
        const float y = rnd_between(y_lo, y_hi);
        if (cls_inside(x, y)) {
            if (!have_in)  { ax = x; ay = y; have_in  = true; }
        } else {
            if (!have_out) { bx = x; by = y; have_out = true; }
        }
    }
    if (!have_in || !have_out) {
        return false;
    }

    for (int i = 0; i < 40; i++) {
        const float dx = bx - ax, dy = by - ay;
        if (fabs_(dx) <= hw && fabs_(dy) <= hw) {
            break;
        }
        const float mx = ax + dx * 0.5f, my = ay + dy * 0.5f;
        if (cls_inside(mx, my)) { ax = mx; ay = my; }
        else                    { bx = mx; by = my; }
    }

    *ox = (ax + bx) * 0.5f;
    *oy = (ay + by) * 0.5f;
    return true;
}

/* hw scaled by 0.707 u times over: a log-uniform zoom, which is the only kind
   that makes sense when every level looks like the one above it. */
static float zoom_down(float hw, int u)
{
    for (int i = 0; i < u; i++) {
        hw *= 0.70710678f;
    }
    return hw;
}

static void propose(uint8_t mode, scene_t *s, int u)
{
    for (size_t i = 0; i < sizeof(*s); i++) {
        ((uint8_t *)s)[i] = 0;
    }
    s->mode    = mode;
    s->seq_len = 2;
    s->seq[0]  = 0;
    s->seq[1]  = 1;

    switch (mode) {

    case MODE_JULIA: {
        /*
         * c is picked on the boundary of the main cardioid or of the period-2
         * bulb and then nudged off it. That is where the interesting Julia
         * sets live: well inside is a fat featureless blob, far outside is
         * dust, and the boundary itself is a dendrite. The nudge is a few
         * thousandths - small enough to stay in the interesting band, random
         * enough that connected and dusty both come up.
         */
        const float th = rnd_unit();
        if (rnd() & 1u) {
            /* the cardioid: c = e^it/2 - e^2it/4 */
            s->jx = fcos_turn(th) * 0.5f - fcos_turn(th * 2.0f) * 0.25f;
            s->jy = fsin_turn(th) * 0.5f - fsin_turn(th * 2.0f) * 0.25f;
        } else {
            /* the period-2 bulb: c = -1 + e^it/4 */
            s->jx = -1.0f + fcos_turn(th) * 0.25f;
            s->jy =         fsin_turn(th) * 0.25f;
        }
        const float eps = rnd_between(0.0006f, 0.010f);
        s->jx += rnd_between(-eps, eps);
        s->jy += rnd_between(-eps, eps);

        s->hw      = zoom_down(1.5f, u);
        s->maxiter = fr_iters_for(mode, s->hw);
        s->cx = s->cy = 0.0f;
        fr_scene(s);
        if (u > 2 && !find_boundary(s->hw, &s->cx, &s->cy,
                                    -1.5f, 1.5f, -1.5f, 1.5f)) {
            s->cx = s->cy = 0.0f;
            s->hw = 1.5f;
            s->maxiter = fr_iters_for(mode, s->hw);
        }
        break;
    }

    case MODE_LYAP: {
        /*
         * A random word over {a,b}, forced to contain both letters, because
         * "aaa" is the plain logistic map and draws a set of vertical
         * stripes. The window is a square somewhere in [2,4]^2, which is
         * where the map is defined and where the ordered tongues grow.
         */
        s->seq_len = (uint8_t)rnd_range(2, SEQ_MAX);
        for (int i = 0; i < s->seq_len; i++) {
            s->seq[i] = (uint8_t)(rnd() & 1u);
        }
        s->seq[rnd_range(0, s->seq_len - 1)] = 0;
        s->seq[rnd_range(0, s->seq_len - 1)] = 1;
        if (s->seq_len == 2 && s->seq[0] == s->seq[1]) {
            s->seq[1] ^= 1u;
        }

        s->maxiter = LY_ITER;
        float hw = zoom_down(0.95f, u);
        if (hw < 0.004f) {
            hw = 0.004f;
        }
        s->hw = hw;

        /* Keep the whole window inside the domain: past [2,4] the map runs
           away and the exponent stops meaning anything. */
        const float lo = 2.0f + hw, hi = 4.0f - hw;
        s->cx = rnd_between(lo, hi);
        s->cy = rnd_between(lo, hi);
        fr_scene(s);
        if (find_boundary(hw, &s->cx, &s->cy, lo, hi, lo, hi)) {
            if (s->cx < lo) { s->cx = lo; } else if (s->cx > hi) { s->cx = hi; }
            if (s->cy < lo) { s->cy = lo; } else if (s->cy > hi) { s->cy = hi; }
        }
        break;
    }

    default: {
        float hw = zoom_down(1.6f, u);
        if (hw < HW_MIN) {
            hw = HW_MIN;
        }
        s->hw      = hw;
        s->maxiter = fr_iters_for(mode, hw);
        s->cx      = -0.6f;
        s->cy      = 0.0f;
        fr_scene(s);
        if (find_boundary(hw, &s->cx, &s->cy, -2.2f, 0.7f, -1.25f, 1.25f)) {
            /* Off-centre by up to half a view, so the boundary is not always
               pinned to the middle of the screen like a specimen. */
            s->cx += rnd_between(-hw * 0.5f, hw * 0.5f);
            s->cy += rnd_between(-hw * 0.5f, hw * 0.5f);
        } else {
            s->hw = 1.6f;
            s->maxiter = fr_iters_for(mode, s->hw);
        }
        break;
    }
    }

    /* One trip round the palette across the range, or two. More than two and
       the bands near the boundary are thinner than a pixel, which reads as
       speckle rather than as detail. vmax is left for the coarse pass to
       measure - see fr_vmax_for. */
    s->vmax   = mode == MODE_LYAP ? 300.0f : (float)s->maxiter;
    s->cycles = (uint8_t)rnd_range(1, 2);
    s->phase  = (uint8_t)(rnd() & 255u);
}

/*
 * How much is going on in a preview, out of about 210.
 *
 * Three terms, because each one alone has a dull picture that passes it.
 * Variety counts how much of the value range is used at all - a wash of two
 * shades scores nothing. Edges count how often the frame crosses between
 * inside and outside, which is the boundary being present rather than merely
 * nearby. Gradient measures how fast the value moves between neighbours,
 * relative to the range within the frame, so that it means the same thing for
 * an escape count and for a Lyapunov exponent.
 */
static int score_preview(int w, int h)
{
    const int n = w * h;
    int   n_in = 0;
    float vmin = 1e30f, vmax = -1.0f;

    for (int i = 0; i < n; i++) {
        const float v = s_prev[i];
        if (v < 0.0f) { n_in++; continue; }
        if (v < vmin) { vmin = v; }
        if (v > vmax) { vmax = v; }
    }
    if (n - n_in < n / 24) {
        return 0;                       /* all interior, nothing to look at */
    }

    const float span = vmax - vmin + 1e-6f;
    int      edges = 0, npair = 0;
    float    grad  = 0.0f;
    uint32_t bits  = 0;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float a = s_prev[y * w + x];
            if (a >= 0.0f) {
                int b16 = (int)((a - vmin) * 16.0f / span);
                if (b16 > 15) { b16 = 15; }
                bits |= 1u << b16;
            }
            for (int d = 0; d < 2; d++) {
                const int nx = x + (d == 0), ny = y + (d == 1);
                if (nx >= w || ny >= h) {
                    continue;
                }
                const float b = s_prev[ny * w + nx];
                if ((a < 0.0f) != (b < 0.0f)) { edges++; continue; }
                if (a < 0.0f) { continue; }
                grad += fabs_(a - b);
                npair++;
            }
        }
    }

    int nb = 0;
    for (uint32_t m = bits; m; m >>= 1) {
        nb += (int)(m & 1u);
    }

    int s_var  = nb * 6;                                    /* 0..96 */
    int s_edge = (edges * 200) / n;
    if (s_edge > 70) { s_edge = 70; }
    int s_grad = npair ? (int)((grad * 400.0f) / ((float)npair * span)) : 0;
    if (s_grad > 40) { s_grad = 40; }

    int sc = s_var + s_edge + s_grad;
    if ((n_in * 256) / n > 200) { sc /= 3; }            /* nearly all black */
    if (nb <= 2 && edges * 40 < n) { sc /= 2; }         /* a wash with an edge */
    return sc;
}

void fr_home_view(uint8_t mode, float *cx, float *cy, float *hw)
{
    switch (mode) {
    case MODE_JULIA: *cx = 0.0f;  *cy = 0.0f; break;
    /* [2,4]^2 is where the logistic map is defined, so its middle is home. */
    case MODE_LYAP:  *cx = 3.0f;  *cy = 3.0f; break;
    /* Not zero: the set runs from -2 to 0.25, so its middle is a little left. */
    default:         *cx = -0.6f; *cy = 0.0f; break;
    }
    *hw = fr_home_hw(mode);
}

void fr_home(uint8_t mode, scene_t *out)
{
    /* propose() at zoom 0 for the things a home view still has to invent -
       a Julia parameter, a Lyapunov word, a palette phase - then the view
       itself is overwritten with the one that frames the whole picture. */
    propose(mode, out, 0);
    fr_home_view(mode, &out->cx, &out->cy, &out->hw);
    out->maxiter = fr_iters_for(mode, out->hw);
    out->vmax    = mode == MODE_LYAP ? 300.0f : (float)out->maxiter;
}

int fr_find(uint8_t mode, scene_t *out)
{
    /* The screen's shape, latched before the previews start moving it. */
    const int aw = s_vw, ah = s_vh;
    int pw, ph;
    if (aw >= ah) { pw = PRE_MAX; ph = (PRE_MAX * ah) / aw; }
    else          { ph = PRE_MAX; pw = (PRE_MAX * aw) / ah; }
    if (pw < 4) { pw = 4; }
    if (ph < 4) { ph = 4; }

    /*
     * The scale is drawn once and every candidate is proposed at it, because
     * a search free to pick the zoom as well would always come back with the
     * deepest one it tried - detail is what the score measures, and there is
     * more of it further down. That is a viewer that only ever shows
     * filaments. Fixing the scale first turns the search into the question it
     * should be asking: given this much of the plane, where is the best of it.
     */
    int u;
    switch (mode) {
    case MODE_JULIA: u = rnd_range(0, 10); break;
    case MODE_LYAP:  u = rnd_range(0, 8);  break;
    default:         u = rnd_range(0, 26); break;
    }

    const uint32_t start = now_ms();
    int best = -1;

    for (int t = 0; t < FIND_TRIES; t++) {
        scene_t s;
        propose(mode, &s, u);

        fr_scene(&s);
        fr_view(pw, ph);
        for (int y = 0; y < ph; y++) {
            for (int x = 0; x < pw; x++) {
                s_prev[y * pw + x] = fr_at(x, y);
            }
        }

        const int sc = score_preview(pw, ph);
        if (sc > best) { best = sc; *out = s; }
        if (sc >= FIND_GOOD) { break; }
        /* Always give it three honest tries; after that the clock decides,
           because a program that thinks for two seconds before drawing looks
           broken however good the picture turns out to be. */
        if (t >= 2 && now_ms() - start >= FIND_MS) { break; }
    }

    /* Leave the raster the way it was found, so a caller that does not
       re-raster is not silently left drawing a 32-pixel preview. */
    fr_scene(out);
    fr_view(aw, ah);
    return best;
}
