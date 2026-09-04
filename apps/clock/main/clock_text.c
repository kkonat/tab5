/*
 * Polish, on hardware that has one cell per letter.
 *
 * The weather service hands over a city name as UTF-8, which is the right
 * thing for it to do and the wrong thing for anything downstream here: on a
 * fourteen-segment cell or a 5x7 matrix a letter is one addressable position,
 * and a two-byte L-with-stroke arriving as two bytes is two blank cells with a
 * gap where the name should be. That is what this file exists to stop.
 *
 * So there is one internal encoding, one byte per letter, and everything that
 * draws a name takes it: ASCII passes through in capitals, the nine Polish
 * letters become the CLK_PL_* codes, and anything else is dropped rather than
 * spaced - a name is easier to read short than perforated.
 *
 * The two displays then part company over what they can do with those codes,
 * and both answers are the hardware's rather than a preference:
 *
 *   A fourteen-segment cell has fourteen electrodes and none of them is an
 *   accent. There is nowhere to put one, so the LCD folds to the base letter
 *   and shows LODZ - which is what every segment display in Poland has always
 *   done, and is why the ones in railway stations are shouted at.
 *
 *   A dot matrix has rows above and below the cell going spare, because the
 *   glyph is seven of them and the pitch leaves a blank one either side. So
 *   the VFD draws the mark there, and gets the name right.
 */
#include <string.h>

#include "clock.h"

/*
 * The nine letters, in the order of the CLK_PL_* codes, with the code points
 * UTF-8 will deliver them as. Upper and lower both, so the fold to capitals
 * happens here rather than twice downstream.
 */
static const struct {
    uint16_t up, lo;
    char     base;
} PL[CLK_PL_N] = {
    { 0x0104, 0x0105, 'A' },   /* A ogonek */
    { 0x0106, 0x0107, 'C' },   /* C acute  */
    { 0x0118, 0x0119, 'E' },   /* E ogonek */
    { 0x0141, 0x0142, 'L' },   /* L stroke */
    { 0x0143, 0x0144, 'N' },   /* N acute  */
    { 0x00D3, 0x00F3, 'O' },   /* O acute  */
    { 0x015A, 0x015B, 'S' },   /* S acute  */
    { 0x0179, 0x017A, 'Z' },   /* Z acute  */
    { 0x017B, 0x017C, 'Z' },   /* Z dot    */
};

char clk_pl_base(char c)
{
    const uint8_t u = (uint8_t)c;
    if (u >= CLK_PL_FIRST && u < CLK_PL_FIRST + CLK_PL_N) {
        return PL[u - CLK_PL_FIRST].base;
    }
    return c;
}

/**
 * Latin-1's own accented capitals, flattened.
 *
 * Not Polish and not this app's business, except that the tablet can be
 * anywhere and a name is better as ZURICH than as ZRICH. One row per block of
 * the Latin-1 table, which is laid out by base letter and is the reason this
 * is nine lines rather than a hundred-entry table.
 */
static char latin1_base(uint16_t cp)
{
    if (cp >= 0x00E0 && cp <= 0x00FE) {
        cp = (uint16_t)(cp - 0x20);          /* to its capital */
    }
    if (cp >= 0x00C0 && cp <= 0x00C5) { return 'A'; }
    if (cp == 0x00C6)                 { return 'A'; }
    if (cp == 0x00C7)                 { return 'C'; }
    if (cp >= 0x00C8 && cp <= 0x00CB) { return 'E'; }
    if (cp >= 0x00CC && cp <= 0x00CF) { return 'I'; }
    if (cp == 0x00D1)                 { return 'N'; }
    if (cp >= 0x00D2 && cp <= 0x00D6) { return 'O'; }
    if (cp == 0x00D8)                 { return 'O'; }
    if (cp >= 0x00D9 && cp <= 0x00DC) { return 'U'; }
    if (cp == 0x00DD)                 { return 'Y'; }
    return 0;
}

/**
 * One code point off the front of @p p, and how many bytes it was.
 *
 * Deliberately forgiving: a byte that is not the start of anything, or a
 * sequence that runs off the end, is taken as one byte of nothing and the walk
 * carries on. A name is not worth refusing to draw over.
 */
static uint16_t utf8_next(const char *p, int *len)
{
    const uint8_t a = (uint8_t)p[0];

    if (a < 0x80) {
        *len = 1;
        return a;
    }
    if ((a & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *len = 2;
        return (uint16_t)(((a & 0x1F) << 6) | (p[1] & 0x3F));
    }
    if ((a & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *len = 3;
        return (uint16_t)(((a & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
    }
    *len = 1;
    return 0;
}

size_t clk_pl_from_utf8(const char *in, char *out, size_t n)
{
    size_t o = 0;

    if (!out || n == 0) {
        return 0;
    }
    out[0] = 0;
    if (!in) {
        return 0;
    }

    while (*in && o + 1 < n) {
        int len = 1;
        const uint16_t cp = utf8_next(in, &len);
        in += len;

        char c = 0;
        if (cp >= 'a' && cp <= 'z') {
            c = (char)(cp - 'a' + 'A');
        } else if (cp < 0x80) {
            c = (char)cp;
        } else {
            for (int i = 0; i < CLK_PL_N; i++) {
                if (cp == PL[i].up || cp == PL[i].lo) {
                    c = (char)(CLK_PL_FIRST + i);
                    break;
                }
            }
            if (!c) {
                c = latin1_base(cp);
            }
        }

        /* Nothing this can draw: drop it. A blank cell in the middle of a name
           reads as a word break that is not there. */
        if (c) {
            out[o++] = c;
        }
    }

    out[o] = 0;
    return o;
}
