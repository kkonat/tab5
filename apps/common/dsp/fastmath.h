/*
 * The four bits of arithmetic every voice on this machine needs, and which
 * must not exist twice.
 *
 * Header-only inlines rather than a translation unit, because each of these is
 * a handful of instructions in the middle of a loop that runs 48,000 times a
 * second and a call through the syscall table would cost more than the
 * function does.
 *
 * Shared between apps/synth1 and apps/moog for one reason: exp2_fast() is a
 * hand-rolled exponential, and an app carrying its own copy of a hand-rolled
 * exponential is an app that will one day be a few cents out from the other
 * one for reasons nobody can find. apps/synth1/test measures the error of the
 * copy that lives here, in cents, which is the unit in which being wrong here
 * would be noticed.
 */
#ifndef FASTMATH_H
#define FASTMATH_H

#include <stdint.h>

/*
 * 2^x, without floorf() or powf().
 *
 * Both are in the syscall table as of ABI 1.21 and both are a call through it,
 * which is a jump to another image and back - per sample, in the one loop that
 * is not allowed to be late. What replaces them is the standard split: take
 * the integer part by converting to int, build 2^i by writing the exponent
 * field of a float directly, and take the fractional part off a series for
 * exp(f * ln2).
 *
 * The order is set by the top of the interval and not by the middle, which is
 * the mistake this started as. A Taylor series about zero is at its worst at f
 * just under 1, and truncating at the fifth term leaves 9e-5 there - about a
 * seventh of a cent, which is inaudible but is thirty times the
 * single-precision floor and so was arbitrary rather than chosen. Seven terms
 * take it to under a hundredth of a cent, which is the rounding of the float
 * itself, and cost two fused multiply-adds in a loop that has hundreds.
 *
 * The clamp is not decoration. x arrives as a modulation depth that a knob
 * scales, and an exponent field built out of an unclamped integer is a NaN or
 * a denormal - either of which reaches the codec as a click and neither of
 * which is easy to trace back to this line.
 */
static inline float exp2_fast(float x)
{
    if (x < -14.0f) { x = -14.0f; }
    if (x >  14.0f) { x =  14.0f; }

    int i = (int)x;
    if (x < 0.0f && (float)i != x) {
        i--;                       /* (int) truncates toward zero; we want floor */
    }
    const float f = x - (float)i;

    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f +
                    f * (0.00961813f + f * (0.00133336f + f * (0.00015404f +
                    f * 0.00001525f))))));

    union { float f; uint32_t u; } s;
    s.u = (uint32_t)((i + 127) << 23);
    return s.f * p;
}

/**
 * The PolyBLEP correction at a discontinuity.
 *
 * @p t is how far the phase is past the step, in turns, and @p dt is one
 * sample of phase. Outside the two samples either side of the step there is
 * nothing to correct and this is a compare and a return.
 *
 * Subtracted at a falling edge and added at a rising one, it takes the worst
 * of the aliasing off a saw or a pulse: a step has every harmonic, and every
 * harmonic above half the sample rate folds back down as an inharmonic
 * whistle that moves the wrong way when the pitch knob is turned.
 */
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

static inline uint32_t xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/** An xorshift word as a float in -1..1, taking the top bits: the low bits of
    an xorshift are its weakest and noise is where that would be audible. */
static inline float rand_bipolar(uint32_t r)
{
    return (float)(int32_t)(r >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/** Phase wrapped into [0,1). A loop and not a subtract: under audio-rate
    modulation the increment can exceed a whole turn in one sample. */
static inline float wrap1(float p)
{
    while (p >= 1.0f) { p -= 1.0f; }
    while (p <  0.0f) { p += 1.0f; }
    return p;
}

#endif /* FASTMATH_H */
