/*
 * Host tests for the Minimoog: the panel's geometry, and the instrument.
 *
 * The turn is not retested here. apps/common/turn is compared pixel for pixel
 * against ngl's own primitives in apps/synth1/test, at both rotations, and it
 * is the same code - retesting it would be testing the same lines twice and
 * saying nothing about this app.
 *
 * What is new here, and what these tests are about:
 *
 *   thirty-six rectangles that must not overlap, must be on the screen, and
 *   must each answer to a touch in their middle. That is a class of bug an eye
 *   catches on the page it is looking at and misses on the other one;
 *
 *   eighteen keys laid out from a semitone pattern, drawn and hit-tested by
 *   two separate pieces of arithmetic that have to agree, where disagreeing
 *   means a key that lights when you press the one beside it;
 *
 *   and a resonant ladder with the output fed back into its own input. That is
 *   a loop with a knob on its gain, and the way it fails is not a wrong note -
 *   it is a NaN that propagates to every sample after it and arrives at the
 *   speaker as full-scale noise. So it is driven at every extreme the panel can
 *   reach and checked for having stayed a number.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neos_sys.h"

#include "moog.h"
#include "mg_ui.h"
#include "mg_panel.h"

/* Included rather than linked: the shapes and the ladder are static, which is
   where they belong. */
#include "mg_dsp.c"

void       stub_present_reset(void);
extern int stub_present_calls;
extern int stub_present_refused;
void stub_set_meters(const mg_meters_t *m);
void stub_set_overload(bool on);

static int g_fail;
static int g_checks;

#define CHECK(cond, ...) do {                                       \
    g_checks++;                                                     \
    if (!(cond)) {                                                  \
        g_fail++;                                                   \
        printf("  FAIL %s:%d  ", __func__, __LINE__);               \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
    }                                                               \
} while (0)

#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol),      \
        "%s = %.9g, wanted %.9g (tol %g)", #a, (double)(a), (double)(b), (double)(tol))

#define LAND_W 1280
#define LAND_H  720

static mg_app_t g_app;

static void setup(ngl_rotation_t rot)
{
    memset(&g_app, 0, sizeof g_app);
    CHECK(turn_init(&g_app.gfx, rot), "canvas allocates");
    mg_ui_defaults(&g_app);
    mg_ui_layout(&g_app);
    mg_ui_patch(&g_app);
    g_app.grab   = HIT_NONE;
    g_app.audio  = true;
    g_app.capped = true;
    g_app.note   = (float)(MG_KEY_C + 12);
}

static void teardown(void) { turn_free(&g_app.gfx); }

static bool overlap(ngl_rect_t a, ngl_rect_t b)
{
    ngl_rect_t hit;
    return ngl_rect_intersect(&a, &b, &hit);
}

/* ================================================================== */
/* The panel's geometry                                               */
/* ================================================================== */

/*
 * Every control is on the screen, and no two on the same page overlap.
 *
 * The second half is the one that matters. A cell that overlaps its neighbour
 * is a finger that turns the wrong knob - and because mg_ui_hit() returns the
 * first match, it is always the *same* wrong knob, which reads as one control
 * being dead rather than as two being confused.
 */
static void test_cells_are_sane(void)
{
    setup(NGL_ROT_90);

    const ngl_rect_t screen = ngl_rect(0, 0, LAND_W, LAND_H);
    const ngl_rect_t keys   = mg_ui_keys_rect();

    for (int i = 0; i < MC_COUNT; i++) {
        const ngl_rect_t r = mg_ui_rect(i);
        CHECK(r.w > 0 && r.h > 0, "control %d (%s) has a rectangle",
              i, mg_defs[i].cap ? mg_defs[i].cap : "-");

        ngl_rect_t hit;
        CHECK(ngl_rect_intersect(&r, &screen, &hit) &&
              hit.w == r.w && hit.h == r.h,
              "control %d (%s) is wholly on screen: %d,%d %dx%d",
              i, mg_defs[i].cap ? mg_defs[i].cap : "-", r.x, r.y, r.w, r.h);

        CHECK(!overlap(r, keys) || mg_defs[i].kind == CK_SLIDER,
              "control %d (%s) is clear of the keyboard",
              i, mg_defs[i].cap ? mg_defs[i].cap : "-");
    }

    for (int i = 0; i < MC_COUNT; i++) {
        for (int j = i + 1; j < MC_COUNT; j++) {
            const bool same_page =
                (mg_defs[i].page == mg_defs[j].page) ||
                (mg_defs[i].page == MG_BOTH) || (mg_defs[j].page == MG_BOTH);
            if (!same_page) {
                continue;
            }
            CHECK(!overlap(mg_ui_rect(i), mg_ui_rect(j)),
                  "%s and %s overlap",
                  mg_defs[i].cap ? mg_defs[i].cap : "-",
                  mg_defs[j].cap ? mg_defs[j].cap : "-");
        }
    }
    teardown();
}

/* The middle of a cell answers to a touch, on the page that cell is on. */
static void test_hit_testing(void)
{
    setup(NGL_ROT_90);

    for (int page = 0; page < MG_PAGES; page++) {
        g_app.page = (uint8_t)page;

        for (int i = 0; i < MC_COUNT; i++) {
            const ngl_rect_t r = mg_ui_rect(i);
            const int16_t cx = (int16_t)(r.x + r.w / 2);
            const int16_t cy = (int16_t)(r.y + r.h / 2);
            const int got = mg_ui_hit(&g_app, cx, cy);

            if (mg_defs[i].page == page || mg_defs[i].page == MG_BOTH) {
                CHECK(got == i, "page %d: the middle of %s hits %d, not %d",
                      page, mg_defs[i].cap ? mg_defs[i].cap : "-", got, i);
            } else {
                CHECK(got != i, "page %d: %s answers though it is not showing",
                      page, mg_defs[i].cap ? mg_defs[i].cap : "-");
            }
        }
    }

    /* The furniture. */
    g_app.page = 0;
    const ngl_rect_t keys = mg_ui_keys_rect();
    CHECK(mg_ui_hit(&g_app, (int16_t)(keys.x + 10), (int16_t)(keys.y + 10)) == HIT_KEYS,
          "the keyboard takes a touch");
    CHECK(mg_ui_hit(&g_app, (int16_t)(LAND_W - 40), 26) == HIT_CLOSE,
          "the cross is where it is drawn");
    CHECK(mg_ui_hit(&g_app, 20, 300) == HIT_PREV, "the left arrow");
    CHECK(mg_ui_hit(&g_app, (int16_t)(LAND_W - 130), 300) == HIT_NEXT, "the right arrow");
    CHECK(mg_ui_hit(&g_app, 600, 20) == HIT_METERS, "the header takes a tap");

    teardown();
}

/*
 * The keyboard's picture and its hit test are two separate pieces of
 * arithmetic over the same semitone pattern, and they have to agree exactly.
 * Where they do not, a key lights when you press the one beside it.
 */
static void test_keyboard(void)
{
    setup(NGL_ROT_90);
    const ngl_rect_t keys = mg_ui_keys_rect();

    int white = 0, black = 0;
    for (int k = 0; k < MG_KEYS; k++) {
        if (mg_key_black(k)) { black++; } else { white++; }
    }
    CHECK(white == 11, "eleven white keys, not %d", white);
    CHECK(black == 7, "seven black keys, not %d", black);

    for (int k = 0; k < MG_KEYS; k++) {
        const ngl_rect_t r = mg_key_rect(keys, k);

        /*
         * A black key is hit anywhere in its own rectangle. A white key is hit
         * in the part of it no black key covers, which for most of them is the
         * bottom - so the probe goes low, where a thumb actually lands.
         */
        const int16_t px = (int16_t)(r.x + r.w / 2);
        const int16_t py = mg_key_black(k) ? (int16_t)(r.y + r.h / 2)
                                           : (int16_t)(r.y + r.h - 8);
        const int got = mg_key_at(keys, px, py);
        CHECK(got == k, "key %d probes as %d", k, got);

        ngl_rect_t hit;
        CHECK(ngl_rect_intersect(&r, &keys, &hit) && hit.w == r.w,
              "key %d is inside the keyboard", k);
    }

    /* Nothing outside answers, and everything inside answers something. */
    CHECK(mg_key_at(keys, (int16_t)(keys.x - 5), (int16_t)(keys.y + 10)) == -1,
          "left of the keyboard is not a key");
    CHECK(mg_key_at(keys, 600, 100) == -1, "the panel is not a key");

    int unclaimed = 0;
    for (int16_t x = keys.x; x < keys.x + keys.w - 4; x = (int16_t)(x + 7)) {
        for (int16_t y = keys.y; y < keys.y + keys.h; y = (int16_t)(y + 11)) {
            if (mg_key_at(keys, x, y) < 0) {
                unclaimed++;
            }
        }
    }
    /* The last white key may not reach the right edge, since the width is
       divided eleven ways and the remainder is left over. A few columns of
       that is the whole of what may be unclaimed. */
    CHECK(unclaimed < 40, "%d points inside the keyboard belong to no key",
          unclaimed);

    teardown();
}

/* ================================================================== */
/* Values and dragging                                                */
/* ================================================================== */

static void test_values(void)
{
    setup(NGL_ROT_90);
    char buf[32];

    for (int i = 0; i < MC_COUNT; i++) {
        if (mg_defs[i].kind == CK_SWITCH) {
            continue;                    /* its legends are its value */
        }
        for (int end = 0; end < 3; end++) {
            const mg_def_t *d = &mg_defs[i];
            if (d->kind == CK_SELECT) {
                g_app.v[i] = (float)((end * (d->steps - 1)) / 2);
            } else {
                g_app.v[i] = (end == 0) ? 0.0f : (end == 1) ? 0.5f : 1.0f;
            }
            mg_ui_patch(&g_app);
            mg_ui_value(&g_app, i, buf, sizeof buf);

            CHECK(buf[0] != 0, "%s at %d has a value", d->cap ? d->cap : "-", end);
            CHECK(strlen(buf) <= 8,
                  "%s at %d reads \"%s\", too wide for its cell",
                  d->cap ? d->cap : "-", end, buf);
            CHECK(strstr(buf, "nan") == NULL && strstr(buf, "inf") == NULL,
                  "%s at %d reads \"%s\"", d->cap ? d->cap : "-", end, buf);
        }
    }
    teardown();
}

static void test_drag(void)
{
    setup(NGL_ROT_90);

    /* Relative and clamped. */
    NEAR(mg_ui_drag(MC_CUTOFF, 0.5f, 400, 400), 0.5f, 1e-6);
    CHECK(mg_ui_drag(MC_CUTOFF, 0.5f, 400, 300) > 0.5f, "up increases");
    CHECK(mg_ui_drag(MC_CUTOFF, 0.5f, 400, 500) < 0.5f, "down decreases");
    CHECK(mg_ui_drag(MC_CUTOFF, 0.5f, 400, -9000) == 1.0f, "clamps at the top");
    CHECK(mg_ui_drag(MC_CUTOFF, 0.5f, 400, 9000) == 0.0f, "clamps at the bottom");

    /* A selector lands on a detent, never between two. */
    for (int16_t y = 600; y >= 100; y = (int16_t)(y - 3)) {
        const float v = mg_ui_drag(MC_O1_WAVE, 0.0f, 600, y);
        CHECK(v == (float)(int)v, "a selector landed at %g", (double)v);
        CHECK(v >= 0.0f && v <= (float)(WAVE_N - 1), "a selector left its range at %g",
              (double)v);
    }

    /* Both ends of a selector are reachable, which a rounding that truncated
       rather than rounded would quietly prevent at one end. */
    CHECK(mg_ui_drag(MC_O1_RANGE, 0.0f, 600, 100) == (float)(RANGE_N - 1),
          "a selector reaches its top");
    CHECK(mg_ui_drag(MC_O1_RANGE, (float)(RANGE_N - 1), 100, 600) == 0.0f,
          "and its bottom");

    /* The fader travels with the finger: half its own length is half its
       range, where a knob's 300-pixel track would have run out long before. */
    const ngl_rect_t f = mg_ui_rect(MC_VOLUME);
    const int16_t half = (int16_t)((f.h - 64) / 2);
    NEAR(mg_ui_drag(MC_VOLUME, 0.0f, 600, (int16_t)(600 - half)), 0.5f, 0.02);

    teardown();
}

/* ================================================================== */
/* Painting                                                           */
/* ================================================================== */

static void test_paint(void)
{
    const mg_meters_t m = { 5000, 0, 900, 1400, 24000, 180 };

    for (int page = 0; page < MG_PAGES; page++) {
        setup(NGL_ROT_90);
        g_app.page = (uint8_t)page;
        stub_set_meters(&m);
        stub_set_overload(page == 1);

        stub_present_reset();
        g_app.repaint_all = true;
        (void)mg_ui_paint(&g_app);
        CHECK(stub_present_calls >= 1, "page %d: a full repaint presents", page);
        CHECK(stub_present_refused == 0,
              "page %d: full repaint offered %d rectangles the device would refuse",
              page, stub_present_refused);

        /* Every control on this page, dirty at once. */
        stub_present_reset();
        for (int i = 0; i < MC_COUNT; i++) {
            mg_mark(&g_app, i);
        }
        g_app.keys_changed = (1u << MG_KEYS) - 1u;
        g_app.head_dirty   = true;
        (void)mg_ui_paint(&g_app);
        CHECK(stub_present_refused == 0,
              "page %d: dirty repaint offered %d rectangles the device would refuse",
              page, stub_present_refused);
        CHECK(g_app.ctl_dirty[0] == 0 && g_app.ctl_dirty[1] == 0 &&
              g_app.keys_changed == 0 && !g_app.head_dirty,
              "page %d: the dirt was cleared", page);

        teardown();
    }
}

/*
 * Painting one control at a time has to arrive at the picture a full repaint
 * would have drawn.
 *
 * This is the test the per-cell repainting exists to be held to. The way it
 * goes wrong is not a crash: it is a cell whose neighbour's ticks or a black
 * key's overhang were rubbed out and not put back, which on a moving panel
 * reads as something smearing and gets shrugged at.
 */
static uint16_t *snapshot(const mg_app_t *a)
{
    uint16_t *out = malloc((size_t)LAND_W * LAND_H * sizeof(uint16_t));
    if (!out) {
        return NULL;
    }
    for (int16_t y = 0; y < LAND_H; y++) {
        for (int16_t x = 0; x < LAND_W; x++) {
            int16_t px, py;
            turn_point(&a->gfx, x, y, &px, &py);
            out[(size_t)y * LAND_W + x] = ngl_surface_row(a->gfx.s, py)[px];
        }
    }
    return out;
}

static void test_incremental_matches_full(void)
{
    /* A sequence of moves that ends somewhere definite, applied two ways. */
    static const struct { int ctl; float v; } MOVES[] = {
        { MC_CUTOFF,   0.20f }, { MC_EMPHASIS, 0.90f }, { MC_CUTOFF, 0.75f },
        { MC_LS,       0.10f }, { MC_FA,       0.60f }, { MC_LD,     0.33f },
        { MC_KBD2,     1.0f  }, { MC_ONN,      1.0f  }, { MC_NCOLOUR, 0.0f },
    };
    const int n = (int)(sizeof MOVES / sizeof MOVES[0]);

    /* --- one control at a time --- */
    setup(NGL_ROT_90);
    g_app.page = 1;
    g_app.repaint_all = true;
    (void)mg_ui_paint(&g_app);

    for (int i = 0; i < n; i++) {
        g_app.v[MOVES[i].ctl] = MOVES[i].v;
        mg_ui_patch(&g_app);
        mg_mark(&g_app, MOVES[i].ctl);
        (void)mg_ui_paint(&g_app);
    }
    /* A few keys, pressed and released, which is the other incremental path. */
    g_app.held = (1u << 0) | (1u << 4) | (1u << 7);
    g_app.keys_changed = g_app.held;
    (void)mg_ui_paint(&g_app);
    g_app.keys_changed = g_app.held;
    g_app.held = 0;
    (void)mg_ui_paint(&g_app);

    uint16_t *inc = snapshot(&g_app);
    teardown();

    /* --- and all at once, into a fresh canvas --- */
    setup(NGL_ROT_90);
    g_app.page = 1;
    for (int i = 0; i < n; i++) {
        g_app.v[MOVES[i].ctl] = MOVES[i].v;
    }
    mg_ui_patch(&g_app);
    g_app.repaint_all = true;
    (void)mg_ui_paint(&g_app);

    uint16_t *full = snapshot(&g_app);

    int diff = 0;
    int16_t fx = -1, fy = -1;
    if (inc && full) {
        for (int16_t y = 0; y < LAND_H; y++) {
            for (int16_t x = 0; x < LAND_W; x++) {
                const size_t i = (size_t)y * LAND_W + x;
                if (inc[i] != full[i]) {
                    if (!diff) { fx = x; fy = y; }
                    diff++;
                }
            }
        }
    }
    CHECK(inc && full, "snapshots allocate");
    CHECK(diff == 0, "%d pixels survived the incremental path that a full "
          "repaint does not have, first at (%d,%d)", diff, fx, fy);

    free(inc);
    free(full);
    teardown();
}

/* ================================================================== */
/* The instrument                                                     */
/* ================================================================== */

static void render(mg_voice_t *v, const mg_patch_t *p, const mg_perf_t *perf,
                   int16_t *out, int n)
{
    for (int i = 0; i < n; i += MG_BLOCK) {
        const int m = (n - i < MG_BLOCK) ? (n - i) : MG_BLOCK;
        mg_voice_render(v, p, perf, out + i, m);
    }
}

static int crossings(const int16_t *pcm, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) {
        if (pcm[i - 1] <= 0 && pcm[i] > 0) { c++; }
    }
    return c;
}

static double rms(const int16_t *pcm, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        s += (double)pcm[i] * (double)pcm[i];
    }
    return sqrt(s / n);
}

/* A patch that plays one clean saw, wide open, with no contour in the way. */
static void plain_patch(mg_patch_t *p)
{
    memset(p, 0, sizeof *p);
    p->range[0] = RANGE_8;  p->wave[0] = WAVE_SAW;
    p->range[1] = RANGE_8;  p->wave[1] = WAVE_SAW;
    p->range[2] = RANGE_8;  p->wave[2] = WAVE_TRI;
    p->vol[0] = 0.8f;       p->on[0] = true;
    p->osc3_kbd = true;
    p->cutoff   = 4.0f;               /* wide open */
    p->emphasis = 0.0f;
    p->contour  = 0.0f;
    p->fa = p->la = 0.001f;
    p->fd = p->ld = 1.0f;
    p->fs = p->ls = 1.0f;
    p->master = 1.0f;
}

static void test_pitch_and_range(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE];

    /* Middle C at 8' is 261.6 Hz, and every RANGE step is an octave on it. */
    static const struct { int range; double hz; } CASES[] = {
        { RANGE_32, 65.41 }, { RANGE_16, 130.8 }, { RANGE_8, 261.6 },
        { RANGE_4, 523.3 },  { RANGE_2, 1046.5 },
    };

    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        mg_voice_t v;
        mg_voice_init(&v, (float)NEOS_AUDIO_RATE);

        mg_patch_t p;
        plain_patch(&p);
        p.range[0] = (uint8_t)CASES[i].range;

        const mg_perf_t perf = { 60.0f, true };
        render(&v, &p, &perf, pcm, NEOS_AUDIO_RATE);

        const int c = crossings(pcm, NEOS_AUDIO_RATE);
        CHECK(fabs(c - CASES[i].hz) <= 2.0,
              "%s played %d cycles in a second, wanted %.1f",
              mg_range_name(CASES[i].range), c, CASES[i].hz);
    }
}

static void test_tune_and_detune(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE];

    mg_voice_t v;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);

    mg_patch_t p;
    plain_patch(&p);
    p.tune = 12.0f;                   /* far past the knob, but the maths is the maths */

    const mg_perf_t perf = { 60.0f, true };
    render(&v, &p, &perf, pcm, NEOS_AUDIO_RATE);
    const int c = crossings(pcm, NEOS_AUDIO_RATE);
    CHECK(fabs(c - 523.3) <= 3.0, "an octave of tune gave %d cycles, wanted 523", c);

    /* Oscillator 2's own knob moves it and leaves oscillator 1 alone. */
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    plain_patch(&p);
    p.vol[0] = 0.0f; p.on[0] = false;
    p.vol[1] = 0.8f; p.on[1] = true;
    p.detune[1] = 12.0f;
    render(&v, &p, &perf, pcm, NEOS_AUDIO_RATE);
    CHECK(fabs(crossings(pcm, NEOS_AUDIO_RATE) - 523.3) <= 3.0,
          "oscillator 2 detuned an octave");
}

/*
 * The gate, the contours, and the single trigger.
 *
 * The last is the one worth a test: the Model D does not restart its contours
 * for a key pressed while another is held, which is what makes a legato line
 * slide instead of striking. It is one comparison in mg_voice_render() and it
 * is invisible until somebody plays legato and wonders why every note has an
 * attack.
 */
static void test_gate_and_contours(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE];

    mg_voice_t v;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);

    mg_patch_t p;
    plain_patch(&p);
    p.la = 0.100f;  p.ld = 0.200f;  p.ls = 0.6f;

    /* Nothing held: silence, and not a very quiet oscillator. */
    mg_perf_t perf = { 60.0f, false };
    render(&v, &p, &perf, pcm, 4800);
    int nonzero = 0;
    for (int i = 2400; i < 4800; i++) {
        if (pcm[i] != 0) { nonzero++; }
    }
    CHECK(nonzero == 0, "%d non-zero samples with no key down", nonzero);

    /* Key down: the attack takes about 100 ms to arrive, so an early window is
       quieter than a later one. */
    perf.gate = true;
    render(&v, &p, &perf, pcm, 24000);
    const double early = rms(pcm, 1200);
    const double late  = rms(pcm + 12000, 4800);
    CHECK(early < late * 0.5, "the attack is not a step: early %.0f, late %.0f",
          early, late);
    CHECK(late > 500.0, "the note actually sounds: %.0f", late);

    /* A second key pressed while the first is held must not restart the
       contour - the level should not dip back toward zero. */
    const float level_before = v.lenv;
    perf.note = 64.0f;
    render(&v, &p, &perf, pcm, 2400);
    CHECK(v.lenv > level_before * 0.8f,
          "the contour restarted on a legato note: %g -> %g",
          (double)level_before, (double)v.lenv);

    /*
     * Key up: it decays away. Measured against the level it was sustaining at
     * rather than against zero, because the release is an exponential and an
     * exponential never arrives - what matters is that a second after the key
     * came up there is forty decibels less of it, not that it is bit-exact
     * silence.
     */
    const double sustained = rms(pcm + 12000, 4800);
    perf.gate = false;
    render(&v, &p, &perf, pcm, NEOS_AUDIO_RATE);
    CHECK(v.lenv < 0.01f, "the contour released to %g", (double)v.lenv);
    CHECK(rms(pcm + 40000, 8000) < sustained * 0.01,
          "the tail is %.0f against a sustain of %.0f",
          rms(pcm + 40000, 8000), sustained);
}

/* The ladder does what a low-pass does: turning the cutoff down takes energy
   out of a sawtooth. Measured rather than asserted, because a filter that is
   wired up backwards still produces a plausible-looking waveform. */
static void test_filter(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 2];

    double last = 1e12;
    for (float cut = 4.0f; cut >= -2.0f; cut -= 2.0f) {
        mg_voice_t v;
        mg_voice_init(&v, (float)NEOS_AUDIO_RATE);

        mg_patch_t p;
        plain_patch(&p);
        p.cutoff = cut;

        const mg_perf_t perf = { 72.0f, true };     /* an octave above middle C */
        render(&v, &p, &perf, pcm, sizeof pcm / sizeof pcm[0]);

        const double r = rms(pcm + 8000, 12000);
        CHECK(r < last, "cutoff %.0f gave %.0f, which is not below %.0f",
              (double)cut, r, last);
        last = r;
    }
}

/*
 * Every extreme the panel can reach, driven at once, and the only question is
 * whether what comes out is still a number.
 *
 * Self-oscillation at full emphasis with the output fed back into the mixer is
 * a loop with a knob on its gain held together by a hand-written cubic. It is
 * meant to settle; if it ever does not, the failure reaches the speaker as
 * full-scale noise and there is nothing downstream to catch it.
 */
static void test_stability(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 2];
    const int n = (int)(sizeof pcm / sizeof pcm[0]);

    static const float NOTES[] = { 36.0f, 60.0f, 84.0f };

    for (int wave = 0; wave < WAVE_N; wave++) {
        for (int range = 0; range < RANGE_N; range++) {
            for (size_t ni = 0; ni < sizeof NOTES / sizeof NOTES[0]; ni++) {
                mg_voice_t v;
                mg_voice_init(&v, (float)NEOS_AUDIO_RATE);

                mg_patch_t p;
                plain_patch(&p);

                /* Everything up, everything on, everything routed. */
                for (int o = 0; o < 3; o++) {
                    p.range[o] = (uint8_t)range;
                    p.wave[o]  = (uint8_t)wave;
                    p.vol[o]   = 1.0f;
                    p.on[o]    = true;
                }
                p.detune[1] = 7.0f;
                p.detune[2] = -7.0f;
                p.noise_vol = 1.0f;  p.noise_on = true;  p.noise_pink = (wave & 1);
                p.fb_vol    = 1.0f;  p.fb_on    = true;  p.fb_mode    = true;
                p.emphasis  = 1.0f;
                p.contour   = 1.0f;
                p.osc_mod   = true;
                p.filt_mod  = true;
                p.mod_mix   = 0.5f;
                p.kbd_track = KBD_FULL;
                p.cutoff    = -4.0f + (float)range * 2.0f;
                p.glide     = 0.3f;
                p.master    = 1.0f;

                const mg_perf_t perf = { NOTES[ni], true };
                render(&v, &p, &perf, pcm, n);

                /*
                 * The voice's state and not the samples. An int16 cannot hold
                 * a NaN, so a filter that has gone to pieces writes zeroes to
                 * the output and reads as a perfectly quiet pass - the only
                 * place the failure is visible is in the state it left behind.
                 *
                 * 2.05 is what sat() allows. Anything above it means the soft
                 * clip was bypassed or is not doing its job, which is the
                 * failure this whole test exists for.
                 */
                bool bad = false;
                if (!(v.y4 == v.y4) || !(v.y1 == v.y1) || !(v.lenv == v.lenv) ||
                    !(v.fenv == v.fenv) || !(v.fb == v.fb)) {
                    bad = true;
                }
                if (fabsf(v.y4) > 2.05f || fabsf(v.fb) > 2.05f) {
                    bad = true;
                }
                CHECK(!bad, "wave %s range %s note %.0f left the filter at "
                      "y4=%g fb=%g lenv=%g",
                      mg_wave_name(wave), mg_range_name(range), (double)NOTES[ni],
                      (double)v.y4, (double)v.fb, (double)v.lenv);
            }
        }
    }
}

/*
 * The feedback return has to do something, and the something must not be DC.
 *
 * Two ways this control fails and neither looks like a bug in the feedback.
 * Too little gain and it only tints the sound, which reads as a dead knob -
 * that is what it did when the return was tapped after the master fader, where
 * the fader's own 0.49 held the loop gain under one however far the INPUT knob
 * was turned. Too much gain without AC coupling and the loop latches: the
 * ladder passes DC, the saturator pins, the feedback holds it pinned, and what
 * comes out is silence with the overload lamp on.
 *
 * So the test is that the sound changes a lot, and that it is still a sound -
 * which is what counting zero crossings is for. A latched output has none.
 */
static void test_feedback(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 2];
    const int n = (int)(sizeof pcm / sizeof pcm[0]);

    mg_patch_t p;
    plain_patch(&p);
    p.cutoff   = 1.0f;
    p.emphasis = 0.5f;
    p.master   = 0.5f;            /* the level a listener would actually pick */

    const mg_perf_t perf = { 60.0f, true };

    mg_voice_t v;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    const double off = rms(pcm + 8000, 12000);
    const int    xoff = crossings(pcm + 8000, 12000);

    /* ON, FDBK, and the input knob at the top. */
    p.fb_on   = true;
    p.fb_mode = true;
    p.fb_vol  = 1.0f;

    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    const double on  = rms(pcm + 8000, 12000);
    const int    xon = crossings(pcm + 8000, 12000);

    CHECK(on > off * 1.25, "feedback at full changed the level from %.0f to "
          "%.0f, which is not enough to be the control doing anything", off, on);
    CHECK(xon > 20, "the feedback loop latched: %d zero crossings in a quarter "
          "of a second (%d without it)", xon, xoff);

    /* And the switch really is the switch. */
    p.fb_mode = false;                        /* FDBK -> OFF */
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    NEAR(rms(pcm + 8000, 12000), off, off * 0.02);

    p.fb_mode = true;
    p.fb_on   = false;                        /* the ON switch */
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    NEAR(rms(pcm + 8000, 12000), off, off * 0.02);

    printf("       feedback: %.0f rms off, %.0f on, %d crossings on\n",
           off, on, xon);
}

/*
 * The master fader is a listening level and nothing else.
 *
 * With the feedback return in circuit it would be easy for it to become a tone
 * control as well - which is exactly what it was, when the return was tapped
 * after it. The check is that halving the fader halves the output and leaves
 * the waveform alone, measured by the crossing count rather than the level.
 */
static void test_master_is_only_level(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 2];
    const int n = (int)(sizeof pcm / sizeof pcm[0]);

    mg_patch_t p;
    plain_patch(&p);
    p.cutoff   = 1.0f;
    p.emphasis = 0.5f;
    p.fb_on    = true;
    p.fb_mode  = true;
    p.fb_vol   = 0.8f;

    const mg_perf_t perf = { 60.0f, true };

    p.master = 1.0f;
    mg_voice_t v;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    const int loud_x = crossings(pcm + 8000, 12000);

    p.master = 0.25f;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    render(&v, &p, &perf, pcm, n);
    const int quiet_x = crossings(pcm + 8000, 12000);

    /* The same waveform, quieter. A few crossings either way is the quiet one
       losing its smallest wiggles into the 16-bit floor. */
    CHECK(abs(loud_x - quiet_x) < loud_x / 8 + 4,
          "the fader changed the waveform: %d crossings loud, %d quiet",
          loud_x, quiet_x);
}

/* Whatever the panel is set to, the codec gets something inside full scale. */
static void test_output_bounded(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 4];
    const int n = (int)(sizeof pcm / sizeof pcm[0]);

    setup(NGL_ROT_90);
    for (int i = 0; i < MC_COUNT; i++) {
        g_app.v[i] = 1.0f;
    }
    /* Selectors want a position, not a fraction. */
    g_app.v[MC_O1_RANGE] = RANGE_2;
    g_app.v[MC_O2_RANGE] = RANGE_2;
    g_app.v[MC_O3_RANGE] = RANGE_2;
    g_app.v[MC_O1_WAVE]  = WAVE_SAW;
    g_app.v[MC_O2_WAVE]  = WAVE_NARROW;
    g_app.v[MC_O3_WAVE]  = WAVE_SQUARE;
    mg_ui_patch(&g_app);

    mg_voice_t v;
    mg_voice_init(&v, (float)NEOS_AUDIO_RATE);
    const mg_perf_t perf = { 60.0f, true };
    render(&v, &g_app.patch, &perf, pcm, n);

    int over = 0;
    for (int i = 0; i < n; i++) {
        if (pcm[i] > 30500 || pcm[i] < -30500) { over++; }
    }
    CHECK(over == 0, "%d samples past what the clamp allows", over);
    teardown();
}

/* ================================================================== */
/* One frame, as a picture                                            */
/* ================================================================== */

static int shoot(const char *path, int page, uint32_t held)
{
    setup(NGL_ROT_90);
    g_app.page = (uint8_t)page;
    g_app.held = held;
    g_app.fps  = 59;

    const mg_meters_t m = { 12000, 0, 890, 1310, 24000, 178 };
    stub_set_meters(&m);
    stub_set_overload(false);

    g_app.repaint_all = true;
    (void)mg_ui_paint(&g_app);

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        teardown();
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", LAND_W, LAND_H);
    for (int16_t y = 0; y < LAND_H; y++) {
        for (int16_t x = 0; x < LAND_W; x++) {
            int16_t px, py;
            turn_point(&g_app.gfx, x, y, &px, &py);
            const ngl_color_t c = ngl_surface_row(g_app.gfx.s, py)[px];
            const int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
            fputc((r << 3) | (r >> 2), f);
            fputc((g << 2) | (g >> 4), f);
            fputc((b << 3) | (b >> 2), f);
        }
    }
    fclose(f);
    printf("wrote %s (page %d)\n", path, page + 1);
    teardown();
    return 0;
}

/* ================================================================== */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "shot") == 0) {
        int rc = shoot("shot-page1.ppm", 0, 0);
        rc |= shoot("shot-page2.ppm", 1, (1u << 0) | (1u << 4) | (1u << 7));
        return rc;
    }

    printf("moog host tests\n");

    printf("  the panel\n");
    test_cells_are_sane();
    test_hit_testing();
    test_keyboard();
    test_values();
    test_drag();
    test_paint();
    test_incremental_matches_full();

    printf("  the instrument\n");
    test_pitch_and_range();
    test_tune_and_detune();
    test_gate_and_contours();
    test_filter();
    test_feedback();
    test_master_is_only_level();
    test_stability();
    test_output_bounded();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
