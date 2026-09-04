/*
 * mandel.h - what the three files of MANDEL share.
 *
 * The split is by cost, as in the greenbox original: fractal.c owns
 * everything that runs per-pixel, palette.c owns colour, and mandel_app.c
 * owns the screen, the gestures and the view history.
 *
 * What changed for the Tab5 is the whole reason the program exists. On a
 * two-button handheld there was no way to steer, so the program searched for
 * somewhere worth looking and showed it to you. Here there is a 1280x720
 * touch panel, so steering is the point: drag a frame over anything and that
 * is the next view, tap to dive in on a spot, and the button in the bottom
 * left walks back out through the views you came in by. The search survives
 * as one button among four, because "show me somewhere good" is still the
 * fastest way to start.
 */
#ifndef MANDEL_H
#define MANDEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "ngl.h"

/* ------------------------------------------------------------ floats only */
/*
 * Single precision, and only single precision.
 *
 * The P4 is RV32IMAFC and the toolchain builds this -mabi=ilp32f, so a float
 * multiply is one instruction that needs nothing from a runtime library. A
 * double is the exact opposite: every operation on one is a call into libgcc,
 * apps link -nostdlib without libgcc, and NeOS's loader exports five double
 * helpers out of the dozens that exist. So a stray double here does not run
 * slowly - it fails to load, on the tablet, with one line in the log. That is
 * why main/CMakeLists.txt builds these files -Werror=double-promotion: the
 * one place that mistake is cheap to find is the compiler.
 *
 * The same reasoning rules out libm, which is not in the table at all. The
 * three functions this program cannot do without are below. Each is exact
 * enough for a colour index and none of them costs a call.
 */

/** log2, to about 1.4e-5 over the whole range. Zero and below give -60. */
static inline float flog2_(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    if (x <= 0.0f) {
        return -60.0f;              /* a superstable orbit, or a bug: cold */
    }

    /* Split off the exponent, leaving the mantissa in [1,2) where a series
       converges fast. */
    const int e = (int)((v.u >> 23) & 0xFFu) - 127;
    v.u = (v.u & 0x007FFFFFu) | 0x3F800000u;

    /* log2(m) = (2/ln2) * atanh((m-1)/(m+1)), and the argument is at most 1/3
       on [1,2), so four terms of the atanh series leave under 1.4e-5. */
    const float z  = (v.f - 1.0f) / (v.f + 1.0f);
    const float z2 = z * z;
    const float s  = z * (1.0f + z2 * (0.33333333f
                             + z2 * (0.2f + z2 * 0.14285714f)));
    return (float)e + 2.88539008f * s;
}

/** sqrt to within an ulp: halve the exponent, then two Newton steps. */
static inline float fsqrt_(float x)
{
    if (!(x > 0.0f)) {
        return 0.0f;
    }
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u = 0x1FBD1DF5u + (v.u >> 1);     /* within 3.5%, which Newton squares */
    float r = v.f;
    r = 0.5f * (r + x / r);
    r = 0.5f * (r + x / r);
    return r;
}

static inline float fabs_(float x) { return x < 0.0f ? -x : x; }
static inline int   iabs_(int x)   { return x < 0 ? -x : x; }

/** sin of a whole turn: t is in turns, not radians. Error under 4e-6. */
float fsin_turn(float t);
static inline float fcos_turn(float t) { return fsin_turn(t + 0.25f); }

/* ----------------------------------------------------------------- clocks */

static inline uint32_t now_ms(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u;
}

/* ----------------------------------------------------------------- scenes */

typedef enum {
    MODE_MANDEL = 0,
    MODE_JULIA  = 1,
    MODE_LYAP   = 2,
    MODE_COUNT  = 3,
} fr_mode_t;

#define SEQ_MAX 6               /* letters in a Lyapunov sequence */

/** Everything needed to draw one picture, and small enough to keep 24 of. */
typedef struct {
    uint8_t  mode;
    uint8_t  seq_len;           /* Lyapunov: how many letters are in use */
    uint8_t  seq[SEQ_MAX];      /* 0 = a, 1 = b */
    uint8_t  cycles;            /* times round the palette across the range */
    uint8_t  phase;             /* where in the palette the shallow end sits */
    uint16_t maxiter;
    float    cx, cy;            /* view centre, in the mode's own plane */
    float    hw;                /* half the width of the view */
    float    jx, jy;            /* Julia parameter; unused by the other two */
    float    vmax;              /* the value one full palette run should span */
} scene_t;

/* --------------------------------------------------------------- fractal.c */

#define FR_INSIDE  (-1.0f)      /* in the set, or - for Lyapunov - chaotic */

void  fr_scene(const scene_t *s);       /* which fractal, from here on */
void  fr_view(int w, int h);            /* raster the scene's view onto w x h */
float fr_at(int px, int py);            /* FR_INSIDE, or a value >= 0 */
void  fr_at_ss4(int px, int py, float out[4]);   /* its four quarter-points */

/** Where a pixel of the current raster lands in the plane. */
void  fr_world(int px, int py, float *wx, float *wy);

/** The width of one pixel, in the plane. A drag-zoom is this times a rectangle. */
float fr_pitch(void);

/*
 * How many palette steps between neighbouring samples counts as a step worth
 * supersampling away. Small enough to catch the fringe, large enough that an
 * ordinary gradient - two or three steps per pixel at most - costs nothing.
 */
#define FR_JUMP_STEPS 5
int   fr_jump(uint8_t a, uint8_t b);    /* is there a visible step between them */

int   fr_find(uint8_t mode, scene_t *out);   /* somewhere worth looking */
void  fr_home(uint8_t mode, scene_t *out);   /* a fresh scene, framed whole */
float fr_home_hw(uint8_t mode);

/*
 * Where a mode sits when nothing is zoomed in, and nothing else.
 *
 * Separate from fr_home because zooming out is not the same as starting over:
 * fr_home invents a Julia parameter and a Lyapunov word, which is right for a
 * new scene and quite wrong for the last press of ZOOM OUT, where the whole
 * point is that it is the same fractal seen from further away.
 */
void  fr_home_view(uint8_t mode, float *cx, float *cy, float *hw);
bool  fr_valid(const scene_t *s);

/*
 * The floor under a zoom, and it is a property of the arithmetic rather than
 * a matter of taste.
 *
 * One pixel is 2*hw/1280 of the plane wide. When that gets down to a float's
 * ulp, neighbouring pixels start rounding to the same coordinate: the kernel
 * returns the same value for both, supersampling can no longer tell the
 * quarter-points apart, and the picture stair-steps.
 *
 * Where that happens was measured rather than guessed, by rendering a row
 * against a double-precision reference down the seahorse valley: at 32000x
 * not one pixel in 1280 repeats its neighbour's value, and at 64000x thirty
 * per cent of them do. So the floor is set one step above the cliff. Zooming
 * near the far left of the set, where coordinates are close to 2 and the ulp
 * is four times larger, reaches it four times sooner - the antenna is a
 * narrow target and coming out slightly soft there is the honest price.
 *
 * 32000x is a little deeper than the 13000x the greenbox version reached in
 * Q28 fixed point, which is not what one expects from single precision - Q28
 * spent its bits on a range this never needs.
 */
#define HW_MIN      5.0e-5f
#define MAXITER_CAP 500

/** Iterations worth spending at this zoom, in this mode. */
uint16_t fr_iters_for(uint8_t mode, float hw);

/** What one palette run should span, given what the coarse pass just saw. */
float fr_vmax_for(uint8_t mode, const float *coarse, int n, uint16_t maxiter);

/* maths, shared with palette.c */
uint32_t rnd(void);
void     rnd_seed(uint32_t s);
int      rnd_range(int lo, int hi);
float    rnd_unit(void);        /* [0,1) */

/* --------------------------------------------------------------- palette.c */
/*
 * 256 colours: entry 0 is the interior, entries 1..255 are one full turn
 * through the palette and wrap seamlessly, because a fractal's exterior is a
 * cycle - the bands never stop, they only get thinner.
 *
 * Every colour this program can draw lies between deep bottle green and neon
 * cyan, by construction: palette.c picks its stops out of one green gamut, so
 * a fresh palette is a different picture on the same machine rather than a
 * different machine.
 */
extern ngl_color_t g_pal[256];
/*
 * The same 256 colours before RGB565 threw four fifths of them away, and the
 * 8x8 threshold that puts them back. See the note over g_bayer8 in palette.c:
 * everything that draws a pixel of the fractal goes through pal_px, and only
 * flat fills use g_pal directly.
 */
extern uint8_t       g_pal8[256][3];
extern const uint8_t g_bayer8[64];
extern float       g_pal_k;         /* value -> position in the cycle */
extern float       g_pal_phase;     /* where the cycle starts */

void     pal_new(uint32_t seed);    /* 0 = pick a fresh one */
uint32_t pal_seed(void);
void     pal_map(const scene_t *s); /* fit the cycle to this scene */

/*
 * Value to colour, and the square root in the middle of it is the whole
 * difference between a picture and a rash.
 *
 * Escape counts pile up as the boundary is approached - the last few pixels
 * before it can cover half the range - so a straight linear mapping spends
 * most of the palette in the fringe, where the bands are already thinner than
 * a pixel, and the result is confetti. The root compresses that end and gives
 * the room back to the open water, which is where a gradient can be seen.
 */
static inline uint8_t pal_shade(float v)
{
    if (v < 0.0f) {
        return 0;
    }
    const int i = (int)(fsqrt_(v) * g_pal_k + g_pal_phase) & 255;
    return i ? (uint8_t)i : 1;      /* 0 belongs to the interior */
}

/**
 * Pack 8-bit RGB into RGB565 with the ordered dither for this pixel.
 *
 * The threshold is the fraction of a 565 step that this cell stands for: the
 * red and blue steps are 8 apart in eight-bit terms and green's are 4, so the
 * 0..63 matrix entry is scaled by 1/8 and 1/16 respectively. A colour that
 * sits three quarters of the way between two representable values therefore
 * rounds up in three cells out of four, and the eye averages the cell back to
 * the colour that was asked for.
 */
static inline ngl_color_t pal_mix(int r, int g, int b, int x, int y)
{
    const int d = g_bayer8[((y & 7) << 3) | (x & 7)];
    r += d >> 3;
    g += d >> 4;
    b += d >> 3;
    if (r > 255) { r = 255; }
    if (g > 255) { g = 255; }
    if (b > 255) { b = 255; }
    return NGL_RGB(r, g, b);
}

/** A palette entry as this pixel should be drawn. */
static inline ngl_color_t pal_px(uint8_t s, int x, int y)
{
    /* The interior is one flat colour and is meant to read as a shape rather
       than a gradient, so it is the one thing here that is not dithered. */
    if (!s) {
        return g_pal[0];
    }
    return pal_mix(g_pal8[s][0], g_pal8[s][1], g_pal8[s][2], x, y);
}

/* ------------------------------------------------------------------ save.c */

/**
 * Write the current picture to album/ on the card as a greyscale PNG.
 *
 * @param gray  one byte per pixel, w*h of them - the palette index, which is
 *              what the palette was applied TO and so what a viewer can
 *              recolour or this program can be pointed back at.
 * @param name_out  the path written, for the toast.
 * @return 0, or negative - see the codes in save.c and neos_file_write.
 */
int save_png(const scene_t *s, const uint8_t *gray, int w, int h,
             char *name_out, size_t name_sz);

/** "mandelbrot", "julia", "lyapunov". One spelling, three callers. */
const char *mode_name(uint8_t mode);

/* -------------------------------------------------------------- mandel_app.c */

/** How far in a view is against its whole-picture one, as "x1.0" or "x32000". */
void zoom_text(char *buf, size_t n, uint8_t mode, float hw);

#endif /* MANDEL_H */
