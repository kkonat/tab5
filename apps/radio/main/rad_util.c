/*
 * The small shared things: strings, the ring, and the parameter table.
 *
 * The string helpers are here because the syscall table carries strcmp,
 * strlen and memmove and stops there. That is enough - what the rest of this
 * app wanted was a bounded copy that always terminates and a case-blind
 * prefix test for reading HTTP headers, and both are four lines.
 */

#include "radio.h"
#include "rad_math.h"

#include <string.h>
#include <stdlib.h>

uint32_t rad_now(void)
{
    return neos_uptime_ms();
}

bool rad_streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static inline char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool rad_starts(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return false;
    }
    for (; *needle; needle++, hay++) {
        if (lower(*hay) != lower(*needle)) {
            return false;
        }
    }
    return true;
}

/*
 * The table carries strchr and strcmp but not strstr, so here it is. Naive,
 * and rightly: the two callers look for "auto" in a flags field and
 * "StreamTitle='" in a metadata block, neither of which is long enough for
 * anything cleverer to pay for itself.
 */
const char *rad_find(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return NULL;
    }
    if (!*needle) {
        return hay;
    }
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return hay;
        }
    }
    return NULL;
}

void rad_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return;
    }
    size_t at = 0;
    if (src) {
        for (; src[at] && at + 1 < size; at++) {
            dst[at] = src[at];
        }
    }
    dst[at] = 0;
}

/* ------------------------------------------------------------------ */
/* The ring                                                            */
/* ------------------------------------------------------------------ */

/*
 * One slot is always left empty, so head == tail means empty and there is no
 * separate count to keep in step with the two pointers. The alternative - a
 * count, updated by both ends - is the same information stored twice, and the
 * producer and consumer here are the same thread anyway, so the usual reason
 * to prefer it does not apply.
 */

bool rad_ring_init(rad_ring_t *r, uint32_t size)
{
    r->buf  = (uint8_t *)malloc(size);
    r->size = size;
    r->head = r->tail = 0;
    return r->buf != NULL;
}

void rad_ring_free(rad_ring_t *r)
{
    free(r->buf);
    r->buf  = NULL;
    r->size = 0;
    r->head = r->tail = 0;
}

void rad_ring_reset(rad_ring_t *r)
{
    r->head = r->tail = 0;
}

uint32_t rad_ring_used(const rad_ring_t *r)
{
    return (r->head >= r->tail) ? (r->head - r->tail)
                                : (r->size - r->tail + r->head);
}

uint32_t rad_ring_room(const rad_ring_t *r)
{
    return r->size ? r->size - rad_ring_used(r) - 1 : 0;
}

uint32_t rad_ring_write(rad_ring_t *r, const uint8_t *src, uint32_t n)
{
    const uint32_t room = rad_ring_room(r);
    if (n > room) {
        n = room;
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t run = r->size - r->head;
        if (run > n - done) {
            run = n - done;
        }
        memcpy(r->buf + r->head, src + done, run);
        r->head = (r->head + run) % r->size;
        done += run;
    }
    return done;
}

uint32_t rad_ring_read(rad_ring_t *r, uint8_t *dst, uint32_t n)
{
    const uint32_t used = rad_ring_used(r);
    if (n > used) {
        n = used;
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t run = r->size - r->tail;
        if (run > n - done) {
            run = n - done;
        }
        memcpy(dst + done, r->buf + r->tail, run);
        r->tail = (r->tail + run) % r->size;
        done += run;
    }
    return done;
}

/*
 * The decoder is handed a pointer into the ring rather than a copy, because
 * minimp3 wants a run of bytes to hunt a frame header in and copying a
 * kilobyte per frame to achieve that would be a memcpy of the entire stream.
 * What it gets is however much is contiguous before the wrap, which is at
 * worst half the ring and always far more than the 1441 bytes a frame can be.
 */
uint32_t rad_ring_peek(const rad_ring_t *r, const uint8_t **at)
{
    *at = r->buf + r->tail;
    return (r->head >= r->tail) ? (r->head - r->tail) : (r->size - r->tail);
}

void rad_ring_skip(rad_ring_t *r, uint32_t n)
{
    const uint32_t used = rad_ring_used(r);
    if (n > used) {
        n = used;
    }
    r->tail = (r->tail + n) % r->size;
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

/*
 * The ranges the sliders and steppers move between, scaled the way radio.h
 * describes. These are the pi-Q app's numbers unchanged - the filters are the
 * same filters, and a setting carried over from that machine should mean the
 * same thing here.
 *
 * The keys are what the settings file on the card uses, and they are the
 * synth's parameter names, so a .settings.json copied off the Pi is readable
 * by eye against this table.
 */
const rad_range_t rad_ranges[P_COUNT] = {
    /* key          lo      hi   dec  unit   default */
    /* 20 Hz is the bottom of hearing and below anything a stream carries, so
       the low end of this range is the off switch. */
    { "hpfFreq",    20,    600,  0,  "Hz",     120 },
    { "lowFreq",    40,   1000,  0,  "Hz",     120 },
    { "lowGain",  -150,    150,  1,  "dB",       0 },
    { "lowRs",      30,    200,  2,  "",       100 },
    { "loMidFreq", 100,   2000,  0,  "Hz",     300 },
    { "loMidGain",-150,    150,  1,  "dB",       0 },
    { "loMidRq",    10,    200,  2,  "",       100 },
    { "midFreq",   150,   8000,  0,  "Hz",    1000 },
    { "midGain",  -150,    150,  1,  "dB",       0 },
    { "midRq",      10,    200,  2,  "",       100 },
    { "hiMidFreq", 800,  12000,  0,  "Hz",    3500 },
    { "hiMidGain",-150,    150,  1,  "dB",       0 },
    { "hiMidRq",    10,    200,  2,  "",       100 },
    { "highFreq", 1500,  16000,  0,  "Hz",    6000 },
    { "highGain", -150,    150,  1,  "dB",       0 },
    { "highRs",     30,    200,  2,  "",       100 },
};

int rad_clamp_param(rad_param_t p, int value)
{
    const rad_range_t *r = &rad_ranges[p];
    if (value < r->lo) { return r->lo; }
    if (value > r->hi) { return r->hi; }
    return value;
}

int rad_step_param(rad_param_t p, int value, bool up)
{
    const rad_range_t *r = &rad_ranges[p];

    if (r->unit[0] == 'H') {
        /*
         * A sixth of an octave, multiplicatively. A fixed step in hertz is
         * useless at one end of these ranges and far too coarse at the other,
         * and at a repeat every 100 ms this sweeps about an octave and a half
         * a second - which is a frequency control that can be found by ear.
         *
         * 2^(1/6) is 1.12246, and the rounding is what stops a value getting
         * stuck: at 20 Hz a multiply by 1.122 lands on 22, but at the small
         * end of a fine-grained parameter an integer step could round back to
         * where it started, so the result is forced to move by at least one.
         */
        const int next = up ? (value * 1122 + 500) / 1000
                            : (value * 1000 + 561) / 1122;
        if (next == value) {
            return rad_clamp_param(p, value + (up ? 1 : -1));
        }
        return rad_clamp_param(p, next);
    }

    /* 0.05 for the narrow ones, 0.5 for the gains - both five, once the
       value is scaled by its own number of decimals. */
    const int step = 5;
    return rad_clamp_param(p, value + (up ? step : -step));
}

void rad_format_param(rad_param_t p, int value, char *out, size_t size)
{
    const rad_range_t *r = &rad_ranges[p];

    if (r->decimals == 0) {
        snprintf(out, size, "%d%s", value, r->unit);
        return;
    }

    /* The sign is pulled out first so that -0.4 does not print as "-0.-4",
       which is what integer division and remainder do with a negative. */
    const int   scale = (r->decimals == 1) ? 10 : 100;
    const bool  neg   = value < 0;
    const int   mag   = neg ? -value : value;
    const char *sign  = neg ? "-" : (value > 0 && r->unit[0] == 'd') ? "+" : "";

    if (r->decimals == 1) {
        snprintf(out, size, "%s%d.%d%s", sign, mag / scale, mag % scale, r->unit);
    } else {
        snprintf(out, size, "%s%d.%02d%s", sign, mag / scale, mag % scale, r->unit);
    }
}
