/*
 * The artwork and the ROM, off the card.
 *
 * Neither is in this repo and neither can be: the ROM is Elektronika's and
 * the artwork is a MAME romset's, so what is checked in is the tool that
 * turns a copy you already have into the file this reads (tools/genlcd/).
 * The app's whole relationship with them is here.
 *
 * The container is described in tools/genlcd/README.md and is deliberately
 * dull - a header, a background, a segment table, a coverage blob, a ROM. It
 * is not MAME's SVG and not bzhxx's .gw: an app with no XML parser and no
 * decompressor wants the pixels already pixels, and the one machine that
 * needs to understand the clever formats is the one with Inkscape on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nupogodi.h"
#include "neos_status.h"

/* ------------------------------------------------------------------ */
/* The container                                                       */
/* ------------------------------------------------------------------ */

#define NPG_MAGIC  "NEOSLCD2"
#define NPG_HDR    80
#define NPG_SEGENT 12

/*
 * How big a file to be ready for.
 *
 * neos_file_read() will not tell you how long a file is - it refuses a buffer
 * that is too small rather than truncating, and says so with -6 - so the size
 * has to be arrived at by offering more until it stops complaining. Starting
 * at half a megabyte gets a 320x240 asset in one go and a 720p one in four
 * tries, each of which costs a stat and an allocation that is immediately
 * freed. The ceiling is what a 1280x720 background plus its segments comes
 * to with room to spare; past that the file is not one of ours.
 */
#define TRY_FIRST  (512u * 1024u)
#define TRY_MAX    (24u * 1024u * 1024u)

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */

static uint8_t *slurp(const char *rel, uint32_t *len_out)
{
    for (uint32_t cap = TRY_FIRST; cap <= TRY_MAX; cap *= 2) {
        uint8_t *buf = malloc(cap);
        if (!buf) {
            neos_status("no memory for the artwork");
            return NULL;
        }
        const int n = neos_file_read(rel, buf, cap);
        if (n >= 0) {
            *len_out = (uint32_t)n;
            return buf;
        }
        free(buf);
        if (n != -6) {
            /* -3 is the ordinary "you have not put the file there yet". */
            neos_status(n == -3 ? "no artwork on the card" : "the card would not read");
            return NULL;
        }
    }
    neos_status("the artwork is implausibly large");
    return NULL;
}

/* ------------------------------------------------------------------ */

/*
 * The picture inside the artwork's black border.
 *
 * MAME draws the display on a black page, so genlcd's background comes back
 * with the picture floating in a flat dark frame. That frame is not artwork -
 * it is the part of the case this app would rather draw itself - so it is
 * measured here and then simply not shown.
 *
 * Each edge is measured on its own, by walking in along the middle of it
 * until the colour stops being near-black. The middle rather than a corner,
 * because a corner is dark on both counts and would agree with any border at
 * all; each edge on its own because they are not equal - the border is
 * whatever fell outside the drawing, and one number for all four would leave
 * a black band down whichever side had the most.
 *
 * Capped, because an artwork that is genuinely dark near an edge - a night
 * scene, a black case - would otherwise be cropped into. Past a small
 * fraction of the picture it is not a border, it is the picture.
 */
#define MARGIN_MAX_PCT 10

static bool is_dark(ngl_color_t c)
{
    return ((c >> 11) & 0x1f) <= 4 && ((c >> 5) & 0x3f) <= 8 && (c & 0x1f) <= 4;
}

/** How many of `n` steps from `first` along `stride` are black. */
static uint16_t dark_run(const ngl_color_t *first, int stride, uint16_t n)
{
    uint16_t i = 0;
    while (i < n && is_dark(first[(int)i * stride])) {
        i++;
    }
    return i;
}

static ngl_rect_t picture_in(const ngl_color_t *bg, uint16_t w, uint16_t h)
{
    const uint16_t cx = (uint16_t)(w / 2), cy = (uint16_t)(h / 2);
    const uint16_t hcap = (uint16_t)(w * MARGIN_MAX_PCT / 100);
    const uint16_t vcap = (uint16_t)(h * MARGIN_MAX_PCT / 100);

    const uint16_t l = dark_run(bg + (size_t)cy * w,               1,      hcap);
    const uint16_t r = dark_run(bg + (size_t)cy * w + (w - 1),    -1,      hcap);
    const uint16_t t = dark_run(bg + cx,                          (int)w,  vcap);
    const uint16_t b = dark_run(bg + (size_t)(h - 1) * w + cx,   -(int)w,  vcap);

    return ngl_rect((int16_t)l, (int16_t)t,
                    (int16_t)(w - l - r), (int16_t)(h - t - b));
}

/* ------------------------------------------------------------------ */

bool npg_asset_load(npg_asset_t *a, const char *rel)
{
    memset(a, 0, sizeof(*a));

    uint32_t len = 0;
    uint8_t *blob = slurp(rel, &len);
    if (!blob) {
        return false;
    }

    if (len < NPG_HDR || memcmp(blob, NPG_MAGIC, 8) != 0) {
        neos_status("that file is not artwork");
        free(blob);
        return false;
    }

    const uint16_t w    = rd16(blob + 8);
    const uint16_t h    = rd16(blob + 10);
    const uint16_t nseg = rd16(blob + 12);

    const uint32_t rom_off = rd32(blob + 16), rom_len = rd32(blob + 20);
    const uint32_t bg_off  = rd32(blob + 24);
    const uint32_t seg_off = rd32(blob + 28);
    const uint32_t cov_off = rd32(blob + 32), cov_len = rd32(blob + 36);

    const uint32_t bg_len  = (uint32_t)w * h * 2;
    const uint32_t seg_len = (uint32_t)nseg * NPG_SEGENT;

    /*
     * Every offset is checked against the length that was actually read, not
     * against the one the header claims, because a truncated file is the
     * likely failure here - a card pulled mid-write - and it would otherwise
     * be found by reading off the end of the allocation.
     */
    const bool sane =
        w && h && nseg && nseg <= SM5A_SEGMENTS &&
        rom_len && rom_len <= NPG_ROM_MAX &&
        (uint64_t)rom_off + rom_len <= len &&
        (uint64_t)bg_off  + bg_len  <= len &&
        (uint64_t)seg_off + seg_len <= len &&
        (uint64_t)cov_off + cov_len <= len;

    if (!sane) {
        neos_status("the artwork is truncated");
        free(blob);
        return false;
    }

    a->w       = w;
    a->h       = h;
    a->nseg    = nseg;
    a->rom_len = (uint16_t)rom_len;
    memcpy(a->rom, blob + rom_off, rom_len);
    memcpy(a->time_addr, blob + 40, sizeof(a->time_addr));
    a->time_pm = blob[46];
    memcpy(a->title, blob + 48, sizeof(a->title) - 1);
    a->title[sizeof(a->title) - 1] = 0;

    /* --- the background ------------------------------------------- */

    a->bg = malloc(bg_len);
    if (!a->bg) {
        neos_status("no memory for the background");
        goto fail;
    }
    memcpy(a->bg, blob + bg_off, bg_len);
    a->vis = picture_in(a->bg, w, h);
    printf("[npg] artwork %ux%u, picture %dx%d at %d,%d\n", w, h,
           a->vis.w, a->vis.h, a->vis.x, a->vis.y);

    /* --- the segment table, and how much coverage it adds up to ---- */

    a->seg = malloc((size_t)nseg * sizeof(npg_seg_t));
    if (!a->seg) {
        neos_status("no memory for the segments");
        goto fail;
    }

    size_t total = 0;
    for (int i = 0; i < nseg; i++) {
        const uint8_t *e = blob + seg_off + (size_t)i * NPG_SEGENT;
        const uint16_t sx = rd16(e), sy = rd16(e + 2);
        const uint16_t sw = rd16(e + 4), sh = rd16(e + 6);
        const uint32_t sp = rd32(e + 8);

        /*
         * A zero-sized segment is normal: a game does not have to use all 72
         * of the part's drivers, and the ones it does not are not in the
         * artwork. They stay in the table so that an index is an index.
         */
        if (sw && sh && (uint64_t)sp + (uint64_t)sw * sh > (uint64_t)cov_len * 2) {
            neos_status("a segment points outside the artwork");
            goto fail;
        }

        a->seg[i].x  = sx;
        a->seg[i].y  = sy;
        a->seg[i].w  = sw;
        a->seg[i].h  = sh;
        a->seg[i].px = (uint32_t)total;
        total += (size_t)sw * sh;
    }

    /* --- the coverage, unpacked and scaled ------------------------- */

    a->cov = malloc(total ? total : 1);
    if (!a->cov) {
        neos_status("no memory for the segments");
        goto fail;
    }

    for (int i = 0; i < nseg; i++) {
        const uint8_t *e = blob + seg_off + (size_t)i * NPG_SEGENT;
        const uint16_t sw = rd16(e + 4), sh = rd16(e + 6);
        const uint32_t sp = rd32(e + 8);
        if (!sw || !sh) {
            continue;
        }
        uint8_t *dst = a->cov + a->seg[i].px;

        for (uint32_t p = 0; p < (uint32_t)sw * sh; p++) {
            /*
             * One nibble per pixel, low nibble first, expanded so that 15
             * becomes 255 - the value the renderer reads as "this pixel of
             * the segment is not there".
             */
            const uint32_t n = sp + p;
            const uint8_t  b = blob[cov_off + (n >> 1)];
            const uint8_t  v = (n & 1) ? (b >> 4) : (b & 0xf);
            dst[p] = (uint8_t)(v | (v << 4));
        }
    }

    free(blob);
    return true;

fail:
    free(blob);
    npg_asset_free(a);
    return false;
}

void npg_asset_free(npg_asset_t *a)
{
    free(a->bg);
    free(a->seg);
    free(a->cov);
    a->bg = NULL;
    a->seg = NULL;
    a->cov = NULL;
    a->nseg = 0;
}
