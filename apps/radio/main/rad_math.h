/*
 * The float maths this app uses, over the ABI's.
 *
 * This was two hundred and seventy lines of minimax polynomial once, because
 * apps link -nostdlib and the syscall table carried no transcendentals. As of
 * ABI 1.21 it carries the float half of <math.h> - and only the float half,
 * which is the same rule this file was written to keep. So what is left here
 * is the two constants and the two one-liners that <math.h> spells in double.
 *
 * Everything is single precision, and the whole app is compiled
 * -Werror=double-promotion for the reason the clock app gives: the loader's
 * table carries a handful of the double helpers out of the dozens that exist,
 * so a stray promotion is an app that will not load rather than one that runs
 * slowly. That is why M_PI is not used below - it is a double, and writing
 * RAD_TAU as 2 * M_PI would promote every angle in the tape deck.
 */
#pragma once

#include <math.h>

#define RAD_PI   3.14159265358979323846f
#define RAD_TAU  6.28318530717958647692f

static inline float rad_fabsf(float x) { return x < 0.0f ? -x : x; }

/** sqrt(x*x + y*y), without the overflow care hypot() is owed - nothing here
    is within thirty orders of magnitude of the edge. */
static inline float rad_hypotf(float x, float y) { return sqrtf(x * x + y * y); }

#define rad_sqrtf(x)     sqrtf(x)
#define rad_sinf(x)      sinf(x)
#define rad_cosf(x)      cosf(x)
#define rad_atan2f(y, x) atan2f((y), (x))
#define rad_log10f(x)    log10f(x)

/** 10^x. powf rather than exp10f, which newlib does not have. */
static inline float rad_pow10f(float x) { return powf(10.0f, x); }

/** 2^x, for the EQ page's log frequency axis. */
static inline float rad_exp2f(float x) { return powf(2.0f, x); }

/**
 * x mod y, y > 0, result in [0, y).
 *
 * fmodf takes the sign of x, and every caller here wants an angle that has
 * gone round rather than one that has gone negative.
 */
static inline float rad_modf(float x, float y)
{
    if (y <= 0.0f) {
        return 0.0f;
    }
    const float m = fmodf(x, y);
    return m < 0.0f ? m + y : m;
}
