/*
 * Host tests for the two things in this app that are arithmetic rather than
 * taste: the turn, and the voice.
 *
 * ------------------------------------------------------------------- the turn
 *
 * turn.c draws a landscape picture into a portrait canvas so that nothing
 * has to be rotated on its way to the glass. That is a coordinate transform
 * applied by hand in a dozen places, and every way of getting it wrong - one
 * axis flipped, both flipped, the two orientations swapped, text mirrored -
 * produces something that still fills the screen and still responds to touch.
 * On a tablet, at arm's length, "the labels are backwards" is easy to see and
 * "the picture is correct at one rotation and mirrored at the other" is not.
 *
 * So the test is differential and it does not trust this app for the answer.
 * The same drawing is done twice: once through turn.c into a 720x1280
 * canvas, and once through ngl's own primitives into an ordinary 1280x720
 * landscape surface. Then every logical pixel of the second is looked up in
 * the first through phys_index() - which is copied verbatim from
 * ngl_screen.c, because that function *is* the definition of which way up this
 * panel is. If the two pictures agree pixel for pixel at both landscape
 * rotations, the turn is right; if they do not, the diff says where.
 *
 * ------------------------------------------------------------------ the voice
 *
 * The oscillator's own correctness is measurable rather than a matter of
 * listening: a pitch is a zero-crossing count, an octave of modulation is a
 * pitch that doubles, and a glide is the absence of a step. The one that
 * really needs it is exp2_fast(), which replaced powf() in the inner loop -
 * a hand-written exponential that is a little bit wrong is a synthesiser that
 * is a little bit out of tune, which is the sort of thing that gets blamed on
 * everything else first.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neos_sys.h"        /* NEOS_AUDIO_RATE, which every length here is in */

#include "synth1.h"
#include "turn.h"
#include "syn_ui.h"

/* Included rather than linked: exp2_fast() and blep() are the subject and are
   static, which is where they belong - a header for them would be a header
   with two callers, both in this file. */
#include "syn_dsp.c"

void       stub_present_reset(void);
extern int stub_present_calls;
extern int stub_present_refused;
extern ngl_rect_t stub_last_present;
void stub_set_lfo(float v);
void stub_set_scope(const int16_t *pcm, bool have);
void stub_set_meters(const syn_meters_t *m);

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

#define PANEL_W  720
#define PANEL_H 1280
#define LAND_W  PANEL_H
#define LAND_H  PANEL_W

/*
 * ngl_screen.c's phys_index(), copied.
 *
 * Copied on purpose and not shared: this is the reference the app is being
 * held against, so it has to be an independent statement of the mapping. If
 * the two are ever made to share an implementation, this test stops testing
 * anything.
 */
static size_t phys_index(ngl_rotation_t rot, int16_t lx, int16_t ly)
{
    switch (rot) {
    case NGL_ROT_90:
        return (size_t)lx * PANEL_W + (PANEL_W - 1 - ly);
    case NGL_ROT_270:
        return (size_t)(PANEL_H - 1 - lx) * PANEL_W + ly;
    default:
        abort();
    }
}

static const ngl_rotation_t ROTS[2] = { NGL_ROT_90, NGL_ROT_270 };
static const char *rotname(ngl_rotation_t r)
{
    return r == NGL_ROT_90 ? "ROT_90" : "ROT_270";
}

/* ================================================================== */
/* The map                                                            */
/* ================================================================== */

static void test_map_agrees_with_ngl(void)
{
    turn_t g;
    CHECK(turn_init(&g, NGL_ROT_90), "canvas allocates");

    for (int r = 0; r < 2; r++) {
        turn_rotate(&g, ROTS[r]);

        int bad = 0;
        for (int16_t y = 0; y < LAND_H && bad < 4; y++) {
            for (int16_t x = 0; x < LAND_W && bad < 4; x++) {
                int16_t px, py;
                turn_point(&g, x, y, &px, &py);
                const size_t want = phys_index(ROTS[r], x, y);
                const size_t got  = (size_t)py * PANEL_W + (size_t)px;
                if (got != want) {
                    bad++;
                    printf("  FAIL %s  (%d,%d) -> panel (%d,%d) = %zu, "
                           "phys_index says %zu\n",
                           rotname(ROTS[r]), x, y, px, py, got, want);
                }
            }
        }
        g_checks++;
        if (bad) { g_fail++; }
    }
    turn_free(&g);
}

/* Every logical pixel lands somewhere on the panel, and no two land on the
   same place. A transform that was off by one in either axis would still pass
   the comparison above on most pixels and fail this outright. */
static void test_map_is_a_bijection(void)
{
    turn_t g;
    CHECK(turn_init(&g, NGL_ROT_90), "canvas allocates");

    uint8_t *seen = calloc(PANEL_W * PANEL_H, 1);
    CHECK(seen != NULL, "scratch allocates");

    for (int r = 0; r < 2; r++) {
        turn_rotate(&g, ROTS[r]);
        memset(seen, 0, PANEL_W * PANEL_H);

        int collisions = 0, offpanel = 0;
        for (int16_t y = 0; y < LAND_H; y++) {
            for (int16_t x = 0; x < LAND_W; x++) {
                int16_t px, py;
                turn_point(&g, x, y, &px, &py);
                if (px < 0 || px >= PANEL_W || py < 0 || py >= PANEL_H) {
                    offpanel++;
                    continue;
                }
                const size_t i = (size_t)py * PANEL_W + (size_t)px;
                if (seen[i]) { collisions++; }
                seen[i] = 1;
            }
        }
        CHECK(offpanel == 0, "%s: %d logical pixels fell off the panel",
              rotname(ROTS[r]), offpanel);
        CHECK(collisions == 0, "%s: %d logical pixels collided",
              rotname(ROTS[r]), collisions);
    }
    free(seen);
    turn_free(&g);
}

/*
 * turn_prect() has to be exactly the rectangle turn_point() maps the corners to.
 *
 * They are separate code - one is for the fast paths that hand a whole
 * rectangle to ngl, the other for the vertex ones - and the only thing keeping
 * them in step is that both were derived from the same four lines. A rect map
 * that is one pixel out puts a seam between a widget and the fill behind it,
 * which reads as a rendering artefact rather than as a coordinate bug.
 */
static void test_prect_matches_point(void)
{
    turn_t g;
    CHECK(turn_init(&g, NGL_ROT_90), "canvas allocates");

    const ngl_rect_t cases[] = {
        ngl_rect(0, 0, LAND_W, LAND_H),
        ngl_rect(0, 0, 1, 1),
        ngl_rect(24, 64, 1232, 272),
        ngl_rect(1279, 719, 1, 1),
        ngl_rect(100, 7, 13, 251),
    };

    for (int r = 0; r < 2; r++) {
        turn_rotate(&g, ROTS[r]);
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            const ngl_rect_t lr = cases[i];
            const ngl_rect_t pr = turn_prect(&g, lr);

            int16_t ax, ay, bx, by;
            turn_point(&g, lr.x, lr.y, &ax, &ay);
            turn_point(&g, (int16_t)(lr.x + lr.w - 1), (int16_t)(lr.y + lr.h - 1),
                      &bx, &by);

            const int16_t x0 = ax < bx ? ax : bx, x1 = ax > bx ? ax : bx;
            const int16_t y0 = ay < by ? ay : by, y1 = ay > by ? ay : by;

            CHECK(pr.x == x0 && pr.y == y0 &&
                  pr.w == (int16_t)(x1 - x0 + 1) && pr.h == (int16_t)(y1 - y0 + 1),
                  "%s case %zu: prect (%d,%d,%d,%d), corners give (%d,%d,%d,%d)",
                  rotname(ROTS[r]), i, pr.x, pr.y, pr.w, pr.h,
                  x0, y0, x1 - x0 + 1, y1 - y0 + 1);
        }
    }
    turn_free(&g);
}

/* ================================================================== */
/* The turn, end to end                                               */
/* ================================================================== */

/*
 * Draw the same thing two ways and compare every pixel.
 *
 * `paint_turned` goes through turn.c into the portrait canvas;
 * `paint_flat` goes through ngl into an ordinary landscape surface. The two
 * are then compared through phys_index(), so what is being checked is not
 * "turn.c drew something" but "turn.c drew the same picture, correctly
 * turned".
 */
typedef void (*turned_fn)(const turn_t *g);
typedef void (*flat_fn)(ngl_surface_t *s);

static int compare_pictures(const char *what, turned_fn turned, flat_fn flat)
{
    int worst = 0;

    for (int r = 0; r < 2; r++) {
        turn_t g;
        if (!turn_init(&g, ROTS[r])) {
            printf("  FAIL %s: no canvas\n", what);
            return 1;
        }
        ngl_surface_t *ref = ngl_surface_new(LAND_W, LAND_H);
        if (!ref) {
            printf("  FAIL %s: no reference surface\n", what);
            turn_free(&g);
            return 1;
        }

        turn_clear(&g, NGL_BLACK);
        ngl_clear(ref, NGL_BLACK);
        turned(&g);
        flat(ref);

        int diff = 0;
        int16_t fx = -1, fy = -1;
        for (int16_t y = 0; y < LAND_H; y++) {
            const ngl_color_t *row = ngl_surface_row(ref, y);
            for (int16_t x = 0; x < LAND_W; x++) {
                int16_t px, py;
                turn_point(&g, x, y, &px, &py);
                const ngl_color_t got = ngl_surface_row(g.s, py)[px];
                if (got != row[x]) {
                    if (!diff) { fx = x; fy = y; }
                    diff++;
                }
            }
        }
        if (diff) {
            printf("  FAIL %s at %s: %d pixels differ, first at (%d,%d)\n",
                   what, rotname(ROTS[r]), diff, fx, fy);
            worst += diff;
        }
        ngl_surface_free(ref);
        turn_free(&g);
    }

    g_checks++;
    if (worst) { g_fail++; }
    return worst;
}

/* --- rectangles --- */

static void t_rects(const turn_t *g)
{
    turn_fill(g, ngl_rect(10, 20, 300, 140), NGL_RED);
    turn_fill(g, ngl_rect(0, 0, 1, 1), NGL_WHITE);
    turn_fill(g, ngl_rect(1279, 719, 1, 1), NGL_WHITE);
    turn_frame(g, ngl_rect(400, 30, 200, 200), NGL_GREEN, 3);
    turn_round(g, ngl_rect(700, 100, 180, 120), 20, NGL_BLUE);
    turn_round_frame(g, ngl_rect(950, 100, 180, 120), 20, NGL_YELLOW, 4);
}

static void f_rects(ngl_surface_t *s)
{
    ngl_fill_rect(s, ngl_rect(10, 20, 300, 140), NGL_RED);
    ngl_fill_rect(s, ngl_rect(0, 0, 1, 1), NGL_WHITE);
    ngl_fill_rect(s, ngl_rect(1279, 719, 1, 1), NGL_WHITE);
    ngl_draw_rect(s, ngl_rect(400, 30, 200, 200), NGL_GREEN, 3);
    ngl_fill_round_rect(s, ngl_rect(700, 100, 180, 120), 20, NGL_BLUE);
    ngl_draw_round_rect(s, ngl_rect(950, 100, 180, 120), 20, NGL_YELLOW, 4);
}

/* --- lines, which is where an axis flip shows up as a mirror --- */

static void t_lines(const turn_t *g)
{
    turn_hline(g, 40, 300, 500, NGL_GREEN);
    turn_vline(g, 40, 300, 200, NGL_RED);
    turn_line(g, 100, 100, 600, 400, NGL_WHITE);
    turn_line(g, 600, 100, 100, 400, NGL_YELLOW);
    turn_line(g, 900, 650, 1200, 80, NGL_BLUE);
}

static void f_lines(ngl_surface_t *s)
{
    ngl_fill_rect(s, ngl_rect(40, 300, 500, 1), NGL_GREEN);
    ngl_fill_rect(s, ngl_rect(40, 300, 1, 200), NGL_RED);
    ngl_line(s, 100, 100, 600, 400, NGL_WHITE);
    ngl_line(s, 600, 100, 100, 400, NGL_YELLOW);
    ngl_line(s, 900, 650, 1200, 80, NGL_BLUE);
}

/* --- text, the one thing the map alone cannot carry --- */

static void t_text(const turn_t *g)
{
    turn_text(g, 40, 40, "Lj 123 %", &ngl_font_small, NGL_WHITE, NGL_BLACK);
    turn_text(g, 40, 120, "PITCH", &ngl_font_large, NGL_GREEN, NGL_BLACK);
    /* Long enough to cross turn_text()'s chunking boundary, which is where a
       run that is composed backwards would come apart. */
    turn_text(g, 40, 260,
             "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
             &ngl_font_small, NGL_YELLOW, NGL_RED);
    turn_text(g, 1279 - 16 * 4, 700, "edge", &ngl_font_small, NGL_BLUE, NGL_BLACK);
}

static void f_text(ngl_surface_t *s)
{
    ngl_text_bg(s, 40, 40, "Lj 123 %", &ngl_font_small, NGL_WHITE, NGL_BLACK);
    ngl_text_bg(s, 40, 120, "PITCH", &ngl_font_large, NGL_GREEN, NGL_BLACK);
    ngl_text_bg(s, 40, 260,
                "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                &ngl_font_small, NGL_YELLOW, NGL_RED);
    ngl_text_bg(s, 1279 - 16 * 4, 700, "edge", &ngl_font_small, NGL_BLUE, NGL_BLACK);
}

static void test_turn_end_to_end(void)
{
    compare_pictures("rectangles", t_rects, f_rects);
    compare_pictures("lines", t_lines, f_lines);
    compare_pictures("text", t_text, f_text);
}

/*
 * The disc has no ngl counterpart to be compared against, so it is checked
 * against its own definition instead: every pixel inside the radius is set,
 * every pixel outside it is not, and the centre is where it was asked for.
 */
static void test_disc(void)
{
    for (int r = 0; r < 2; r++) {
        turn_t g;
        CHECK(turn_init(&g, ROTS[r]), "canvas allocates");
        turn_clear(&g, NGL_BLACK);

        const int16_t cx = 400, cy = 300, rad = 70;
        turn_disc(&g, cx, cy, rad, NGL_WHITE);

        int wrong_in = 0, wrong_out = 0;
        for (int16_t y = cy - rad - 3; y <= cy + rad + 3; y++) {
            for (int16_t x = cx - rad - 3; x <= cx + rad + 3; x++) {
                int16_t px, py;
                turn_point(&g, x, y, &px, &py);
                const bool on = ngl_surface_row(g.s, py)[px] == NGL_WHITE;
                const int dx = x - cx, dy = y - cy;
                const int d2 = dx * dx + dy * dy;

                /* One pixel of slack at the rim, which is where rounding a
                   square root to an integer span legitimately lands. */
                if (d2 <= (rad - 1) * (rad - 1) && !on) { wrong_in++; }
                if (d2 >= (rad + 2) * (rad + 2) && on)  { wrong_out++; }
            }
        }
        CHECK(wrong_in == 0, "%s: %d pixels missing inside the disc",
              rotname(ROTS[r]), wrong_in);
        CHECK(wrong_out == 0, "%s: %d pixels painted outside the disc",
              rotname(ROTS[r]), wrong_out);
        turn_free(&g);
    }
}

/*
 * Everything the panel hands over has to be a rectangle the device would
 * accept. ngl_panel_scale() clips to nothing rather than clamping, so a
 * rectangle a pixel outside the panel is not a slightly wrong picture, it is a
 * widget that silently stops updating.
 */
static void test_present_rects(void)
{
    for (int r = 0; r < 2; r++) {
        turn_t g;
        CHECK(turn_init(&g, ROTS[r]), "canvas allocates");
        stub_present_reset();

        (void)turn_present(&g, ngl_rect(0, 0, LAND_W, LAND_H));
        (void)turn_present(&g, ngl_rect(24, 64, 1232, 272));
        (void)turn_present(&g, ngl_rect(1279, 719, 1, 1));
        (void)turn_present(&g, ngl_rect(0, 0, 1, 1));

        CHECK(stub_present_calls == 4, "%s: %d rectangles offered",
              rotname(ROTS[r]), stub_present_calls);
        CHECK(stub_present_refused == 0,
              "%s: %d rectangles the device would refuse",
              rotname(ROTS[r]), stub_present_refused);

        /* A rectangle that is wholly outside is dropped here rather than being
           offered and refused there. */
        stub_present_reset();
        (void)turn_present(&g, ngl_rect(2000, 2000, 10, 10));
        CHECK(stub_present_calls == 0, "%s: an off-screen rect was still offered",
              rotname(ROTS[r]));

        turn_free(&g);
    }
}

/* ================================================================== */
/* The voice                                                          */
/* ================================================================== */

/*
 * exp2_fast() against the real thing, over the whole range it can be handed.
 *
 * A cent is 1/1200 of an octave, so a relative error of 0.06% is one cent. The
 * tolerance here is a hundredth of that, which is what a fifth-order series
 * gets: the point of measuring it rather than asserting "close enough" is that
 * this is the one approximation in the app whose error would be heard as the
 * instrument being out of tune rather than as anything obviously broken.
 */
static void test_exp2_fast(void)
{
    double worst = 0.0;
    float  at = 0.0f;

    for (float x = -14.0f; x <= 14.0f; x += 0.0009765625f) {
        const double want = pow(2.0, (double)x);
        const double got  = (double)exp2_fast(x);
        const double rel  = fabs(got - want) / want;
        if (rel > worst) { worst = rel; at = x; }
    }
    /*
     * Three parts in a million, which is a three-hundredth of a cent and is
     * about where single-precision rounding of the polynomial itself lands.
     * Tightening it further would be a test of float epsilon rather than of
     * the series.
     */
    CHECK(worst < 3e-6, "exp2_fast is off by %.3g (relative) at x = %g", worst, at);
    printf("       exp2_fast: worst relative error %.3g, %.5f cents\n",
           worst, worst * 1200.0 / log(2.0));

    /* The clamp, which is there so an unclamped exponent field cannot become a
       NaN on its way to the codec. */
    CHECK(exp2_fast(-1000.0f) > 0.0f, "clamped low and still a number");
    CHECK(exp2_fast(1000.0f) < 1e6f, "clamped high");
    CHECK(exp2_fast(0.0f) == 1.0f, "2^0 is exactly 1");
    NEAR(exp2_fast(1.0f), 2.0f, 1e-5);
    NEAR(exp2_fast(-1.0f), 0.5f, 1e-6);
}

/* Count rising zero crossings, which is the pitch. */
static int crossings(const int16_t *pcm, int n)
{
    int c = 0;
    for (int i = 1; i < n; i++) {
        if (pcm[i - 1] <= 0 && pcm[i] > 0) { c++; }
    }
    return c;
}

static void render(syn_voice_t *v, const syn_patch_t *p, int16_t *out, int n)
{
    const int step = SYN_BLOCK;
    for (int i = 0; i < n; i += step) {
        const int m = (n - i < step) ? (n - i) : step;
        syn_voice_render(v, p, out + i, m);
    }
}

static void test_pitch(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE];      /* one second */

    const float want[] = { 55.0f, 110.0f, 440.0f, 1760.0f };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        syn_voice_t v;
        syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

        syn_patch_t p = { want[i], 0.8f, 1.0f, 0.0f, SYN_TRI };
        render(&v, &p, pcm, NEOS_AUDIO_RATE);

        const int c = crossings(pcm, NEOS_AUDIO_RATE);
        /* Within one cycle of the second: the glide at the start means the
           first few milliseconds are not at pitch yet, by design. */
        CHECK(abs(c - (int)want[i]) <= 2,
              "%g Hz came out as %d crossings in a second", (double)want[i], c);
    }
}

/*
 * An octave of modulation is a pitch that doubles, and a square LFO is the one
 * shape that says so unambiguously: the first half of its cycle is +1 and the
 * second is -1, so the oscillator should spend one at twice the pitch and the
 * other at half it. This is exp2_fast() and the modulation path checked
 * together, in the units that matter.
 */
static void test_modulation_depth(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE];

    syn_voice_t v;
    syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

    /* A 1 Hz square at full depth around 440 Hz: half a second at 880, half at
       220. The LFO starts at phase 0, which is the +1 half. */
    syn_patch_t p = { 440.0f, 0.8f, 1.0f, 1.0f, SYN_SQR };
    render(&v, &p, pcm, NEOS_AUDIO_RATE);

    const int hi = crossings(pcm, NEOS_AUDIO_RATE / 2);
    const int lo = crossings(pcm + NEOS_AUDIO_RATE / 2, NEOS_AUDIO_RATE / 2);

    /* Half a second each, so 440 and 110 cycles. A couple either way covers
       the glide into the first half and the edge landing mid-cycle. */
    CHECK(abs(hi - 440) <= 3, "the up half ran at %d cycles, wanted 440", hi);
    CHECK(abs(lo - 110) <= 3, "the down half ran at %d cycles, wanted 110", lo);
}

static void test_output_is_bounded(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 4];

    /* Every shape, at the top of the audio-rate range and full depth, which is
       the most violent thing the panel can ask for. */
    for (int shape = 0; shape < SYN_SHAPES; shape++) {
        syn_voice_t v;
        syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

        syn_patch_t p = { 3520.0f, 1.0f, 2000.0f, 1.0f, shape };
        render(&v, &p, pcm, sizeof pcm / sizeof pcm[0]);

        int over = 0;
        for (size_t i = 0; i < sizeof pcm / sizeof pcm[0]; i++) {
            if (pcm[i] > 32000 || pcm[i] < -32000) { over++; }
        }
        CHECK(over == 0, "shape %s: %d samples past full scale",
              syn_shape_name(shape), over);
    }
}

/*
 * A parameter that jumps must not make the waveform jump.
 *
 * Checked in two places, because the output alone cannot say it. A sawtooth
 * already has one discontinuity per cycle, so counting discontinuities counts
 * mostly the ones that are meant to be there - which is how the first version
 * of this test managed to fail against correct code.
 *
 * What separates the two cases cleanly is the direction. A sawtooth's reset is
 * a fall; between resets it only ever rises, and it rises by one ramp slope
 * per sample. A level that was assigned rather than chased puts an extra
 * *rise* in the middle of a ramp, of up to eighty per cent of full scale,
 * where the ramp itself can only manage a couple of thousand counts. So the
 * measurement is the largest single-sample rise, and the two cases are an
 * order of magnitude apart rather than tangled together.
 *
 * The smoothed state is then checked directly for what the output cannot show:
 * that the chase is actually a chase - neither instant nor stuck - and that it
 * arrives.
 */
static void test_glide(void)
{
    static int16_t pcm[24000];              /* half a second */

    syn_voice_t v;
    syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

    syn_patch_t p = { 55.0f, 0.2f, 1.0f, 0.0f, SYN_TRI };
    render(&v, &p, pcm, 4800);              /* 100 ms to settle */
    NEAR(v.amp, 0.2f, 0.005);
    NEAR(v.freq, 55.0f, 0.5);

    /* Both knobs, hard over, between one block and the next. */
    p.freq = 1760.0f;
    p.amp  = 1.0f;

    syn_voice_render(&v, &p, pcm + 4800, SYN_BLOCK);
    CHECK(v.amp > 0.5f && v.amp < 0.85f,
          "level after one block is %g - assigned or stuck, not chased",
          (double)v.amp);
    CHECK(v.freq > 300.0f && v.freq < 1200.0f,
          "pitch after one block is %g - the slower chase did not happen",
          (double)v.freq);

    render(&v, &p, pcm + 4800 + SYN_BLOCK, 24000 - 4800 - SYN_BLOCK);
    NEAR(v.amp, 1.0f, 0.002);
    NEAR(v.freq, 1760.0f, 2.0);

    int max_rise = 0;
    for (int i = 4801; i < 24000; i++) {
        const int d = pcm[i] - pcm[i - 1];
        if (d > max_rise) { max_rise = d; }
    }
    /* The ramp at 1760 Hz and full level climbs 2347 counts a sample; an
       assigned level would add up to 25,600 somewhere. 6000 is between the
       two and near neither. */
    CHECK(max_rise < 6000, "the waveform rose %d counts in one sample", max_rise);
    printf("       glide: largest single-sample rise %d counts\n", max_rise);
}

static void test_lfo_shapes(void)
{
    static int16_t pcm[NEOS_AUDIO_RATE / 8];

    for (int shape = 0; shape < SYN_SHAPES; shape++) {
        syn_voice_t v;
        syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

        syn_patch_t p = { 440.0f, 0.5f, 8.0f, 0.5f, shape };
        render(&v, &p, pcm, sizeof pcm / sizeof pcm[0]);

        CHECK(v.lfo_out >= -1.3f && v.lfo_out <= 1.3f,
              "shape %s left the LFO at %g", syn_shape_name(shape),
              (double)v.lfo_out);
        CHECK(v.lfo_out == v.lfo_out, "shape %s produced a NaN",
              syn_shape_name(shape));
        CHECK(syn_shape_name(shape)[0] != '?', "shape %d has a name", shape);
    }
    CHECK(syn_shape_name(-1)[0] == '?', "an unknown shape says so");
    CHECK(syn_shape_name(SYN_SHAPES)[0] == '?', "and so does one past the end");
}

/* Silence is silence, not a very quiet oscillator, so the amplifier is not
   being driven by something nobody asked to hear. */
static void test_silence(void)
{
    static int16_t pcm[4096];

    syn_voice_t v;
    syn_voice_init(&v, (float)NEOS_AUDIO_RATE);

    syn_patch_t p = { 440.0f, 0.0f, 5.0f, 0.0f, SYN_TRI };
    render(&v, &p, pcm, 4096);

    int nonzero = 0;
    for (int i = 2048; i < 4096; i++) {
        if (pcm[i] != 0) { nonzero++; }
    }
    CHECK(nonzero == 0, "%d non-zero samples at zero level", nonzero);
}

/* ================================================================== */
/* The panel                                                          */
/* ================================================================== */

static syn_app_t g_app;

static void ui_setup(void)
{
    memset(&g_app, 0, sizeof g_app);
    CHECK(turn_init(&g_app.gfx, NGL_ROT_90), "canvas allocates");
    syn_ui_defaults(&g_app);
    syn_ui_layout(&g_app);
    syn_ui_patch(&g_app);
    g_app.grab  = HIT_NONE;
    g_app.audio = true;
}

static void ui_teardown(void)
{
    turn_free(&g_app.gfx);
}

static void test_defaults(void)
{
    ui_setup();

    /* Written in hertz in syn_ui.c and turned into knob positions there; this
       is the round trip, which is the only thing that could silently drift if
       a range constant were changed. */
    NEAR(g_app.patch.freq, 110.0f, 0.05);
    NEAR(g_app.patch.lfo_rate, 5.0f, 0.005);
    CHECK(g_app.shape == SYN_TRI, "opens on a triangle");
    CHECK(!g_app.hi, "opens in the sub-audio range");

    ui_teardown();
}

static void test_hit_testing(void)
{
    ui_setup();

    for (int i = 0; i < C_COUNT; i++) {
        const ngl_rect_t r = syn_ui_ctl_rect(i);
        const int16_t cx = (int16_t)(r.x + r.w / 2);
        const int16_t cy = (int16_t)(r.y + r.h / 2);
        CHECK(syn_ui_hit(cx, cy) == i, "the middle of cell %d hits it", i);
        CHECK(r.x >= 0 && r.x + r.w <= LAND_W, "cell %d is on screen", i);
        CHECK(r.y >= 0 && r.y + r.h <= LAND_H, "cell %d fits vertically", i);
    }

    /* The cells must not overlap, or a finger on one turns two. */
    for (int i = 1; i < C_COUNT; i++) {
        const ngl_rect_t a = syn_ui_ctl_rect(i - 1);
        const ngl_rect_t b = syn_ui_ctl_rect(i);
        CHECK(a.x + a.w <= b.x, "cell %d clears cell %d", i, i - 1);
    }

    /* The way out, which in fullscreen is the only one the app owns. */
    CHECK(syn_ui_hit((int16_t)(LAND_W - 24 - 26), 28) == HIT_CLOSE,
          "the cross is where it is drawn");
    CHECK(syn_ui_hit((int16_t)(LAND_W / 2), (int16_t)(LAND_H - 20)) == HIT_FOOTER,
          "the instruments take a tap");
    CHECK(syn_ui_hit(600, 200) == HIT_NONE, "the scope takes nothing");

    ui_teardown();
}

static void test_drag(void)
{
    ui_setup();

    /* Relative and clamped: a knob picks up where it was and stops at the
       ends rather than wrapping. */
    NEAR(syn_ui_drag(0.5f, 400, 400), 0.5f, 1e-6);
    CHECK(syn_ui_drag(0.5f, 400, 300) > 0.5f, "up increases");
    CHECK(syn_ui_drag(0.5f, 400, 500) < 0.5f, "down decreases");
    CHECK(syn_ui_drag(0.5f, 400, -10000) == 1.0f, "clamps at the top");
    CHECK(syn_ui_drag(0.5f, 400, 10000) == 0.0f, "clamps at the bottom");

    /* Monotonic over the whole travel. */
    float last = -1.0f;
    for (int16_t y = 600; y >= 100; y -= 5) {
        const float n = syn_ui_drag(0.0f, 600, y);
        CHECK(n >= last, "monotonic at y = %d", y);
        last = n;
    }
    ui_teardown();
}

static void test_values(void)
{
    ui_setup();

    char buf[32];

    /* Both ends of every knob, because the formatting switches between three
       forms and the seams are exactly at the ends. */
    for (int ctl = 0; ctl < C_COUNT; ctl++) {
        for (int end = 0; end < 3; end++) {
            g_app.norm[ctl] = (end == 0) ? 0.0f : (end == 1) ? 0.5f : 1.0f;
            syn_ui_patch(&g_app);
            syn_ui_value(&g_app, ctl, buf, sizeof buf);
            CHECK(buf[0] != 0, "control %d at %d has a value", ctl, end);
            CHECK(strlen(buf) <= 10,
                  "control %d at %d reads \"%s\", which is too wide for the cell",
                  ctl, end, buf);
            CHECK(strchr(buf, '?') == NULL && strstr(buf, "nan") == NULL,
                  "control %d at %d reads \"%s\"", ctl, end, buf);
        }
    }

    /* The range switch really does change what the rate knob means. */
    g_app.norm[C_RATE] = 1.0f;
    g_app.hi = false;
    syn_ui_patch(&g_app);
    NEAR(g_app.patch.lfo_rate, 20.0f, 0.02);
    g_app.hi = true;
    syn_ui_patch(&g_app);
    NEAR(g_app.patch.lfo_rate, 2000.0f, 2.0);

    ui_teardown();
}

/*
 * A full repaint, and then a dirty one, with every rectangle checked against
 * what the device would accept. This is the test that would have caught a
 * widget laid out past the edge of the panel.
 */
static void test_paint(void)
{
    static int16_t pcm[SYN_SCOPE_N];
    for (int i = 0; i < SYN_SCOPE_N; i++) {
        pcm[i] = (int16_t)(20000.0 * sin(i * 0.05));
    }

    for (int r = 0; r < 2; r++) {
        memset(&g_app, 0, sizeof g_app);
        CHECK(turn_init(&g_app.gfx, ROTS[r]), "canvas allocates");
        syn_ui_defaults(&g_app);
        syn_ui_layout(&g_app);
        syn_ui_patch(&g_app);
        g_app.grab  = HIT_NONE;
        g_app.audio = true;
        stub_set_scope(pcm, true);
        stub_set_lfo(0.4f);

        syn_meters_t m = { 100, 0, 180, 260, 24000, 36 };
        stub_set_meters(&m);

        stub_present_reset();
        g_app.repaint_all = true;
        (void)syn_ui_paint(&g_app);
        CHECK(stub_present_calls >= 1, "%s: a full repaint presents",
              rotname(ROTS[r]));
        CHECK(stub_present_refused == 0,
              "%s: full repaint offered %d rectangles the device would refuse",
              rotname(ROTS[r]), stub_present_refused);

        /* And now the incremental path, with everything marked. */
        stub_present_reset();
        g_app.ctl_dirty   = (1u << C_COUNT) - 1u;
        g_app.scope_dirty = true;
        g_app.hud_dirty   = true;
        (void)syn_ui_paint(&g_app);
        CHECK(stub_present_calls == 1 + C_COUNT + 1 + 2,
              "%s: %d rectangles, wanted header + %d cells + scope + 2 rows",
              rotname(ROTS[r]), stub_present_calls, C_COUNT);
        CHECK(stub_present_refused == 0,
              "%s: dirty repaint offered %d rectangles the device would refuse",
              rotname(ROTS[r]), stub_present_refused);

        CHECK(g_app.ctl_dirty == 0 && !g_app.scope_dirty && !g_app.hud_dirty,
              "%s: the dirt was cleared", rotname(ROTS[r]));

        /* The second pass over the scope is the one that has to erase what the
           first drew; run it a few times with a moving trace to be sure the
           span bookkeeping does not walk off. */
        for (int k = 0; k < 8; k++) {
            for (int i = 0; i < SYN_SCOPE_N; i++) {
                pcm[i] = (int16_t)(20000.0 * sin(i * 0.05 + k * 0.7));
            }
            stub_set_scope(pcm, true);
            g_app.scope_dirty = true;
            (void)syn_ui_paint(&g_app);
        }
        CHECK(stub_present_refused == 0, "%s: the scope stayed in bounds",
              rotname(ROTS[r]));

        turn_free(&g_app.gfx);
    }
}

/*
 * The incremental scope has to converge on the picture a full repaint would
 * have drawn. This is the test the column-wise erase exists to be held to.
 *
 * Erasing per column rather than clearing the box is the difference between a
 * few thousand pixels a frame and a third of a megapixel, and the way it goes
 * wrong is not a crash: it is a trail of old trace left behind where this
 * frame's span is shorter than last frame's, or a hole rubbed in the graticule
 * where a division ran under the waveform. Both look like something smearing,
 * on a moving display, which is exactly the sort of thing that gets watched
 * for a while and then shrugged at.
 *
 * So the same final waveform is arrived at two ways - once by painting a
 * sequence of them incrementally, and once by painting it alone into a fresh
 * canvas - and the two are compared pixel for pixel over the scope.
 */
static uint16_t *snapshot_scope(const syn_app_t *a, const ngl_rect_t *box)
{
    uint16_t *out = malloc((size_t)box->w * box->h * sizeof(uint16_t));
    if (!out) {
        return NULL;
    }
    for (int16_t y = 0; y < box->h; y++) {
        for (int16_t x = 0; x < box->w; x++) {
            int16_t px, py;
            turn_point(&a->gfx, (int16_t)(box->x + x), (int16_t)(box->y + y), &px, &py);
            out[(size_t)y * box->w + x] = ngl_surface_row(a->gfx.s, py)[px];
        }
    }
    return out;
}

static void fill_wave(int16_t *pcm, int n, double phase, double amp)
{
    for (int i = 0; i < n; i++) {
        /* Deliberately not the voice: what is wanted here is a trace that
           changes shape as well as position, so that a column's span grows on
           some frames and shrinks on others. Shrinking is the case that leaves
           a trail. */
        const double t = i * 0.017 + phase;
        pcm[i] = (int16_t)(amp * 30000.0 * (sin(t) + 0.4 * sin(3.1 * t + 1.0)));
    }
}

static void test_scope_incremental_matches_full(void)
{
    static int16_t pcm[SYN_SCOPE_N];

    /* Amplitudes that fall as well as rise, so the trace is asked to give
       screen back and not only to take more of it. */
    static const double AMP[] = { 1.0, 0.7, 0.2, 0.9, 0.05, 0.6 };
    const int steps = (int)(sizeof AMP / sizeof AMP[0]);

    for (int r = 0; r < 2; r++) {
        ngl_rect_t box;
        uint16_t *inc = NULL, *full = NULL;

        /* --- the incremental way --- */
        memset(&g_app, 0, sizeof g_app);
        CHECK(turn_init(&g_app.gfx, ROTS[r]), "canvas allocates");
        syn_ui_defaults(&g_app);
        syn_ui_layout(&g_app);
        syn_ui_patch(&g_app);
        g_app.grab  = HIT_NONE;
        g_app.audio = true;
        stub_set_lfo(0.0f);

        fill_wave(pcm, SYN_SCOPE_N, 0.0, AMP[0]);
        stub_set_scope(pcm, true);
        g_app.repaint_all = true;
        (void)syn_ui_paint(&g_app);

        for (int k = 1; k < steps; k++) {
            fill_wave(pcm, SYN_SCOPE_N, k * 0.37, AMP[k]);
            stub_set_scope(pcm, true);
            g_app.scope_dirty = true;
            (void)syn_ui_paint(&g_app);
        }
        /* The whole screen rather than the scope's own rectangle: it needs no
           layout knowledge here, and it also catches an incremental pass that
           disturbed something it had no business touching. */
        box = ngl_rect(0, 0, LAND_W, LAND_H);
        inc = snapshot_scope(&g_app, &box);
        turn_free(&g_app.gfx);

        /* --- and the wholesale one --- */
        memset(&g_app, 0, sizeof g_app);
        CHECK(turn_init(&g_app.gfx, ROTS[r]), "canvas allocates");
        syn_ui_defaults(&g_app);
        syn_ui_layout(&g_app);
        syn_ui_patch(&g_app);
        g_app.grab  = HIT_NONE;
        g_app.audio = true;

        fill_wave(pcm, SYN_SCOPE_N, (steps - 1) * 0.37, AMP[steps - 1]);
        stub_set_scope(pcm, true);
        g_app.repaint_all = true;
        (void)syn_ui_paint(&g_app);
        full = snapshot_scope(&g_app, &box);

        int diff = 0;
        int16_t fx = -1, fy = -1;
        if (inc && full) {
            for (int16_t y = 0; y < box.h; y++) {
                for (int16_t x = 0; x < box.w; x++) {
                    const size_t i = (size_t)y * box.w + x;
                    if (inc[i] != full[i]) {
                        if (!diff) { fx = x; fy = y; }
                        diff++;
                    }
                }
            }
        }
        CHECK(inc && full, "snapshots allocate");
        CHECK(diff == 0,
              "%s: %d pixels survived six incremental repaints that a full "
              "one does not have, first at (%d,%d)",
              rotname(ROTS[r]), diff, box.x + fx, box.y + fy);

        free(inc);
        free(full);
        turn_free(&g_app.gfx);
    }
}

/* The scope is asked for a trace before the audio thread has captured one, on
   every single start. It must draw nothing rather than a screenful of whatever
   was in the buffer. */
static void test_scope_before_audio(void)
{
    ui_setup();
    stub_set_scope(NULL, false);
    stub_present_reset();

    g_app.repaint_all = true;
    (void)syn_ui_paint(&g_app);
    CHECK(stub_present_refused == 0, "an empty scope still presents cleanly");

    ui_teardown();
}

/* ================================================================== */
/* One frame, as a picture                                            */
/* ================================================================== */

/*
 * Paint the panel and write it out as it would appear on the glass.
 *
 * The tests above prove the turn is arithmetically right. They say nothing
 * about whether the layout is any good - whether a value overflows its cell,
 * whether the trace is legible against the graticule, whether the knob reads
 * as a knob - and those are the things that otherwise cost a build, a card and
 * a walk to the tablet to find out.
 *
 * It reads back through turn_point(), so what lands in the file is the picture
 * the panel would be showing rather than the canvas's own layout: if the turn
 * were wrong this would be visibly wrong too, which is the other half of its
 * value.
 */
static int shoot(const char *path, ngl_rotation_t rot)
{
    static int16_t pcm[SYN_SCOPE_N];

    memset(&g_app, 0, sizeof g_app);
    if (!turn_init(&g_app.gfx, rot)) {
        printf("no canvas\n");
        return 1;
    }
    syn_ui_defaults(&g_app);
    syn_ui_layout(&g_app);
    syn_ui_patch(&g_app);
    g_app.grab   = HIT_NONE;
    g_app.audio  = true;
    g_app.capped = true;
    g_app.fps    = 58;
    g_app.work_us = 4120;
    g_app.paint_us = 2260;

    /* A real block from the real voice, so the trace is the app's own output
       and not a sine somebody drew. */
    syn_voice_t v;
    syn_voice_init(&v, (float)NEOS_AUDIO_RATE);
    for (int i = 0; i < SYN_SCOPE_N; i += SYN_BLOCK) {
        const int n = (SYN_SCOPE_N - i < SYN_BLOCK) ? SYN_SCOPE_N - i : SYN_BLOCK;
        syn_voice_render(&v, &g_app.patch, pcm + i, n);
    }
    stub_set_scope(pcm, true);
    stub_set_lfo(v.lfo_out);

    const syn_meters_t m = { 12000, 0, 168, 291, 24000, 34 };
    stub_set_meters(&m);

    g_app.repaint_all = true;
    (void)syn_ui_paint(&g_app);

    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("cannot write %s\n", path);
        turn_free(&g_app.gfx);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", LAND_W, LAND_H);
    for (int16_t y = 0; y < LAND_H; y++) {
        for (int16_t x = 0; x < LAND_W; x++) {
            int16_t px, py;
            turn_point(&g_app.gfx, x, y, &px, &py);
            const ngl_color_t c = ngl_surface_row(g_app.gfx.s, py)[px];
            /* RGB565 out to eight bits a channel, replicating the top bits
               into the bottom so that white comes out white. */
            const int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
            fputc((r << 3) | (r >> 2), f);
            fputc((g << 2) | (g >> 4), f);
            fputc((b << 3) | (b >> 2), f);
        }
    }
    fclose(f);
    printf("wrote %s (%dx%d, %s)\n", path, LAND_W, LAND_H, rotname(rot));

    turn_free(&g_app.gfx);
    return 0;
}

/* ================================================================== */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "shot") == 0) {
        int rc = shoot("shot-90.ppm", NGL_ROT_90);
        rc |= shoot("shot-270.ppm", NGL_ROT_270);
        return rc;
    }

    printf("synth1 host tests\n");

    printf("  the map\n");
    test_map_agrees_with_ngl();
    test_map_is_a_bijection();
    test_prect_matches_point();

    printf("  the turn\n");
    test_turn_end_to_end();
    test_disc();
    test_present_rects();

    printf("  the voice\n");
    test_exp2_fast();
    test_pitch();
    test_modulation_depth();
    test_output_is_bounded();
    test_glide();
    test_lfo_shapes();
    test_silence();

    printf("  the panel\n");
    test_defaults();
    test_hit_testing();
    test_drag();
    test_values();
    test_paint();
    test_scope_incremental_matches_full();
    test_scope_before_audio();

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
