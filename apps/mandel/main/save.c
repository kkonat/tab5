/*
 * save.c - putting a picture on the card.
 *
 * What gets written is the grayscale the palette was applied TO, not what is
 * on the panel: one byte per pixel, straight out of the index buffer, with no
 * controls and no status line over it because those were only ever drawn on
 * the screen surface and never went into the picture. That byte is the whole
 * point of saving this rather than a screenshot - it is the fractal's own
 * measurement of each pixel, so the image can be recoloured afterwards by
 * anything, and it is 8 bits instead of 16.
 *
 * ------------------------------------------------------------------ PNG
 *
 * PNG, because a file nobody can open is not a saved picture, and because it
 * is the one ordinary format with somewhere honest to put the view's
 * coordinates - tEXt chunks, which every image tool will show you and which
 * survive being copied about.
 *
 * There is no deflate here. PNG's payload is a zlib stream, and a zlib stream
 * is allowed to consist entirely of *stored* blocks - uncompressed, five
 * bytes of header per 64 KB. That makes a valid, universally readable PNG out
 * of two checksums and some bookkeeping, instead of a compressor this app has
 * no room for and no need of: the file lands about 850 KB where a compressed
 * one would be perhaps 300 KB, on a card with gigabytes free.
 *
 * ------------------------------------------------------------ re-rendering
 *
 * Every number needed to draw the picture again goes in the text chunks, and
 * each float goes in twice: once as decimal for a person, and once as the
 * exact bits for a machine. That is not belt and braces - a float carries
 * about seven digits and a view at 30000x needs nine to land back on the same
 * pixel, so the decimal genuinely cannot round-trip and the hex genuinely
 * can. Printing the decimal at all is a small effort too, since %f promotes
 * to double and this app has no double runtime; see fmt_f below.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neos_api.h"
#include "neos_sys.h"
#include "neos_time.h"

#include "mandel.h"

#define ALBUM "album"

/* ------------------------------------------------------------------ */
/* Checksums                                                           */
/* ------------------------------------------------------------------ */

/*
 * Bitwise rather than table-driven. A 1 KB table would save about 50 ms on
 * the one chunk big enough to matter, once, on a button press - and cost a
 * kilobyte of an app that is loaded off a card into PSRAM every time it runs.
 */
static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
        }
    }
    return ~c;
}

static uint32_t adler32_of(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        /* 5552 is the most bytes that cannot overflow b before a modulo. */
        size_t k = n > 5552 ? 5552 : n;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------------ */
/* A buffer that refuses to overrun                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *p;
    size_t   cap, len;
    bool     bad;
} buf_t;

static void put(buf_t *b, const void *src, size_t n)
{
    if (b->bad || b->len + n > b->cap) {
        b->bad = true;
        return;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_u8(buf_t *b, uint8_t v) { put(b, &v, 1); }

static void put_be32(buf_t *b, uint32_t v)
{
    const uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                           (uint8_t)(v >> 8),  (uint8_t)v };
    put(b, t, 4);
}

/** A PNG chunk: length, type, data, CRC over type+data. */
static void chunk(buf_t *b, const char *type, const uint8_t *data, size_t n)
{
    put_be32(b, (uint32_t)n);
    const size_t at = b->len;
    put(b, type, 4);
    put(b, data, n);
    if (!b->bad) {
        put_be32(b, crc32_of(b->p + at, 4 + n));
    }
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/*
 * A float as decimal, without printf.
 *
 * "%f" takes a double, so a float argument is promoted on the way into the
 * varargs - which calls __extendsfdf2, which is not in NeOS's symbol table,
 * which means the app does not load at all. Six decimals by integer
 * arithmetic instead; the exact value travels alongside as hex.
 */
static void fmt_f(char *out, size_t n, float v)
{
    const bool neg = v < 0.0f;
    if (neg) {
        v = -v;
    }
    if (!(v < 2000.0f)) {                   /* also catches NaN */
        snprintf(out, n, "%s", neg ? "-big" : "big");
        return;
    }
    const uint32_t s = (uint32_t)(v * 1000000.0f + 0.5f);
    snprintf(out, n, "%s%u.%06u", neg ? "-" : "",
             (unsigned)(s / 1000000u), (unsigned)(s % 1000000u));
}

/** The same float as the 32 bits it actually is. */
static void fmt_x(char *out, size_t n, float v)
{
    union { float f; uint32_t u; } b;
    b.f = v;
    snprintf(out, n, "0x%08X", (unsigned)b.u);
}

static void text(buf_t *b, const char *key, const char *val)
{
    uint8_t t[220];
    const size_t kl = strlen(key), vl = strlen(val);
    if (kl + 1 + vl > sizeof(t)) {
        return;
    }
    memcpy(t, key, kl);
    t[kl] = 0;                              /* keyword and text, NUL between */
    memcpy(t + kl + 1, val, vl);
    chunk(b, "tEXt", t, kl + 1 + vl);
}

static void text_f(buf_t *b, const char *key, const char *keyx, float v)
{
    char s[32];
    fmt_f(s, sizeof s, v);
    text(b, key, s);
    fmt_x(s, sizeof s, v);
    text(b, keyx, s);
}

const char *mode_name(uint8_t mode)
{
    switch (mode) {
    case MODE_JULIA: return "julia";
    case MODE_LYAP:  return "lyapunov";
    default:         return "mandelbrot";
    }
}

static void meta(buf_t *b, const scene_t *s, int w, int h)
{
    char v[96], z[16];

    zoom_text(z, sizeof z, s->mode, s->hw);

    text(b, "Software", "NeOS mandel");
    text(b, "Title", mode_name(s->mode));

    snprintf(v, sizeof v, "%s at zoom %s, %u iterations, %dx%d",
             mode_name(s->mode), z, (unsigned)s->maxiter, w, h);
    text(b, "Comment", v);

    /* The grey in this file is the palette index, so what it means depends on
       these three: without them the picture can be recoloured but not
       reproduced. */
    snprintf(v, sizeof v, "index; vmax %u, cycles %u, phase %u",
             (unsigned)(s->vmax + 0.5f), (unsigned)s->cycles, (unsigned)s->phase);
    text(b, "mandel.shade", v);

    text(b, "mandel.mode", mode_name(s->mode));
    text(b, "mandel.zoom", z);
    text_f(b, "mandel.cx", "mandel.cx.hex", s->cx);
    text_f(b, "mandel.cy", "mandel.cy.hex", s->cy);
    text_f(b, "mandel.halfwidth", "mandel.halfwidth.hex", s->hw);

    snprintf(v, sizeof v, "%u", (unsigned)s->maxiter);
    text(b, "mandel.iter", v);
    snprintf(v, sizeof v, "%dx%d", w, h);
    text(b, "mandel.size", v);

    if (s->mode == MODE_JULIA) {
        text_f(b, "mandel.jx", "mandel.jx.hex", s->jx);
        text_f(b, "mandel.jy", "mandel.jy.hex", s->jy);
    }
    if (s->mode == MODE_LYAP) {
        char word[SEQ_MAX + 1];
        int len = s->seq_len > SEQ_MAX ? SEQ_MAX : s->seq_len;
        for (int i = 0; i < len; i++) {
            word[i] = (char)('a' + (s->seq[i] & 1));
        }
        word[len] = 0;
        text(b, "mandel.sequence", word);
    }
}

/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

#define TEXT_CAP  2048
#define BLOCK_MAX 65535

static void make_name(char *out, size_t n)
{
    neos_rtc_t t;

    /*
     * The RTC is battery backed, so this is usually a real date even with no
     * network. When it is not - a fresh board, a dead cell - a wrong date in
     * the filename would sort wrongly forever, so say uptime instead and be
     * obviously not a date.
     */
    if (neos_time_local(&t) && t.year >= 2020) {
        snprintf(out, n, ALBUM "/mandel%04d%02u%02u-%02u%02u%02u.png",
                 (int)t.year, (unsigned)t.month, (unsigned)t.day,
                 (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
    } else {
        snprintf(out, n, ALBUM "/mandel-up%09u.png",
                 (unsigned)neos_uptime_ms());
    }
}

int save_png(const scene_t *s, const uint8_t *gray, int w, int h,
             char *name_out, size_t name_sz)
{
    if (!s || !gray || w <= 0 || h <= 0) {
        return -10;
    }

    /*
     * The scanlines, each with its filter byte. Built whole rather than
     * emitted row by row because a row is 1281 bytes and a stored block is
     * 65535, so the two do not divide - and splitting rows across block
     * boundaries by hand is a page of off-by-one risk to save a megabyte on a
     * machine with thirty-two of them.
     */
    const size_t row = (size_t)w + 1;
    const size_t raw_len = row * (size_t)h;

    uint8_t *raw = malloc(raw_len);
    if (!raw) {
        return -11;
    }
    for (int y = 0; y < h; y++) {
        raw[row * (size_t)y] = 0;           /* filter 0: none */
        memcpy(raw + row * (size_t)y + 1, gray + (size_t)y * w, (size_t)w);
    }

    const size_t nblk = (raw_len + BLOCK_MAX - 1) / BLOCK_MAX;
    const size_t idat = 2 + nblk * 5 + raw_len + 4;
    const size_t cap  = 8 + (12 + 13) + TEXT_CAP + (12 + idat) + 12;

    buf_t b = { .p = malloc(cap), .cap = cap, .len = 0, .bad = false };
    if (!b.p) {
        free(raw);
        return -12;
    }

    static const uint8_t SIG[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    put(&b, SIG, sizeof SIG);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8]  = 8;       /* bits per sample */
    ihdr[9]  = 0;       /* colour type 0: greyscale */
    ihdr[10] = 0;       /* deflate */
    ihdr[11] = 0;       /* adaptive filtering */
    ihdr[12] = 0;       /* not interlaced */
    chunk(&b, "IHDR", ihdr, sizeof ihdr);

    meta(&b, s, w, h);

    /* IDAT, written in place: the zlib stream is long and copying it twice
       would be another 850 KB for nothing. */
    put_be32(&b, (uint32_t)idat);
    const size_t idat_at = b.len;
    put(&b, "IDAT", 4);

    put_u8(&b, 0x78);       /* deflate, 32K window */
    put_u8(&b, 0x01);       /* no preset dictionary, and 0x7801 divides by 31 */

    for (size_t off = 0; off < raw_len; off += BLOCK_MAX) {
        size_t n = raw_len - off;
        if (n > BLOCK_MAX) {
            n = BLOCK_MAX;
        }
        put_u8(&b, (uint8_t)(off + n >= raw_len ? 1 : 0));   /* stored; final? */
        put_u8(&b, (uint8_t)n);                              /* LEN, little */
        put_u8(&b, (uint8_t)(n >> 8));
        put_u8(&b, (uint8_t)~(uint8_t)n);                    /* and its inverse */
        put_u8(&b, (uint8_t)~(uint8_t)(n >> 8));
        put(&b, raw + off, n);
    }
    put_be32(&b, adler32_of(raw, raw_len));
    free(raw);

    if (!b.bad) {
        put_be32(&b, crc32_of(b.p + idat_at, 4 + idat));
    }
    chunk(&b, "IEND", NULL, 0);

    if (b.bad) {
        free(b.p);
        return -13;
    }

    char name[80];
    make_name(name, sizeof name);

    const int rc = neos_file_write(name, b.p, b.len);
    free(b.p);
    if (rc != 0) {
        return rc;
    }
    if (name_out && name_sz) {
        snprintf(name_out, name_sz, "%s", name);
    }
    printf("[mandel] wrote %s, %u bytes\n", name, (unsigned)b.len);
    return 0;
}
