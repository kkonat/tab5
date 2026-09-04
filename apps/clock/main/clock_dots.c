/*
 * The 5x7 dot matrix alphabet.
 *
 * Hand-written rather than generated, because at five columns by seven rows a
 * rasteriser has no room to make a decision - every glyph at this size is one
 * of a handful of shapes that were settled in the 1970s and have been copied
 * from datasheet to datasheet since. Rendering a TTF into a 5x7 cell produces
 * something worse and less familiar.
 *
 * One byte per column, bit 0 the top row and bit 6 the bottom, which is the
 * order the parts themselves shift out and the order every published table
 * uses. Codes 32 to 90 - space through Z - which covers the digits, the
 * separators and the uppercase alphabet a VFD would have had. Lowercase folds
 * to uppercase in the lookup rather than doubling the table, because a real
 * one had no lowercase either.
 */
#include <stddef.h>
#include <stdint.h>

#include "clock.h"

#define DOT_FIRST 32
#define DOT_LAST  90

const uint8_t clk_dot5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00,   /* 32  ' ' */
    0x00, 0x00, 0x5F, 0x00, 0x00,   /* 33  '!' */
    0x00, 0x07, 0x00, 0x07, 0x00,   /* 34  '"' */
    0x14, 0x7F, 0x14, 0x7F, 0x14,   /* 35  '#' */
    0x24, 0x2A, 0x7F, 0x2A, 0x12,   /* 36  '$' */
    0x23, 0x13, 0x08, 0x64, 0x62,   /* 37  '%' */
    0x36, 0x49, 0x55, 0x22, 0x50,   /* 38  '&' */
    0x00, 0x05, 0x03, 0x00, 0x00,   /* 39  '\'' */
    0x00, 0x1C, 0x22, 0x41, 0x00,   /* 40  '(' */
    0x00, 0x41, 0x22, 0x1C, 0x00,   /* 41  ')' */
    0x14, 0x08, 0x3E, 0x08, 0x14,   /* 42  '*' */
    0x08, 0x08, 0x3E, 0x08, 0x08,   /* 43  '+' */
    0x00, 0x50, 0x30, 0x00, 0x00,   /* 44  ',' */
    0x08, 0x08, 0x08, 0x08, 0x08,   /* 45  '-' */
    0x00, 0x60, 0x60, 0x00, 0x00,   /* 46  '.' */
    0x20, 0x10, 0x08, 0x04, 0x02,   /* 47  '/' */
    0x3E, 0x51, 0x49, 0x45, 0x3E,   /* 48  '0' */
    0x00, 0x42, 0x7F, 0x40, 0x00,   /* 49  '1' */
    0x42, 0x61, 0x51, 0x49, 0x46,   /* 50  '2' */
    0x21, 0x41, 0x45, 0x4B, 0x31,   /* 51  '3' */
    0x18, 0x14, 0x12, 0x7F, 0x10,   /* 52  '4' */
    0x27, 0x45, 0x45, 0x45, 0x39,   /* 53  '5' */
    0x3C, 0x4A, 0x49, 0x49, 0x30,   /* 54  '6' */
    0x01, 0x71, 0x09, 0x05, 0x03,   /* 55  '7' */
    0x36, 0x49, 0x49, 0x49, 0x36,   /* 56  '8' */
    0x06, 0x49, 0x49, 0x29, 0x1E,   /* 57  '9' */
    0x00, 0x36, 0x36, 0x00, 0x00,   /* 58  ':' */
    0x00, 0x56, 0x36, 0x00, 0x00,   /* 59  ';' */
    0x08, 0x14, 0x22, 0x41, 0x00,   /* 60  '<' */
    0x14, 0x14, 0x14, 0x14, 0x14,   /* 61  '=' */
    0x00, 0x41, 0x22, 0x14, 0x08,   /* 62  '>' */
    0x02, 0x01, 0x51, 0x09, 0x06,   /* 63  '?' */
    0x3E, 0x41, 0x5D, 0x55, 0x1E,   /* 64  '@' */
    0x7E, 0x11, 0x11, 0x11, 0x7E,   /* 65  'A' */
    0x7F, 0x49, 0x49, 0x49, 0x36,   /* 66  'B' */
    0x3E, 0x41, 0x41, 0x41, 0x22,   /* 67  'C' */
    0x7F, 0x41, 0x41, 0x22, 0x1C,   /* 68  'D' */
    0x7F, 0x49, 0x49, 0x49, 0x41,   /* 69  'E' */
    0x7F, 0x09, 0x09, 0x09, 0x01,   /* 70  'F' */
    0x3E, 0x41, 0x49, 0x49, 0x7A,   /* 71  'G' */
    0x7F, 0x08, 0x08, 0x08, 0x7F,   /* 72  'H' */
    0x00, 0x41, 0x7F, 0x41, 0x00,   /* 73  'I' */
    0x20, 0x40, 0x41, 0x3F, 0x01,   /* 74  'J' */
    0x7F, 0x08, 0x14, 0x22, 0x41,   /* 75  'K' */
    0x7F, 0x40, 0x40, 0x40, 0x40,   /* 76  'L' */
    0x7F, 0x02, 0x0C, 0x02, 0x7F,   /* 77  'M' */
    0x7F, 0x04, 0x08, 0x10, 0x7F,   /* 78  'N' */
    0x3E, 0x41, 0x41, 0x41, 0x3E,   /* 79  'O' */
    0x7F, 0x09, 0x09, 0x09, 0x06,   /* 80  'P' */
    0x3E, 0x41, 0x51, 0x21, 0x5E,   /* 81  'Q' */
    0x7F, 0x09, 0x19, 0x29, 0x46,   /* 82  'R' */
    0x46, 0x49, 0x49, 0x49, 0x31,   /* 83  'S' */
    0x01, 0x01, 0x7F, 0x01, 0x01,   /* 84  'T' */
    0x3F, 0x40, 0x40, 0x40, 0x3F,   /* 85  'U' */
    0x1F, 0x20, 0x40, 0x20, 0x1F,   /* 86  'V' */
    0x3F, 0x40, 0x38, 0x40, 0x3F,   /* 87  'W' */
    0x63, 0x14, 0x08, 0x14, 0x63,   /* 88  'X' */
    0x07, 0x08, 0x70, 0x08, 0x07,   /* 89  'Y' */
    0x61, 0x51, 0x49, 0x45, 0x43,   /* 90  'Z' */
};

/*
 * The degree sign, which is not in ASCII and which a display that shows a
 * temperature cannot do without. Reached as 0xB0 - its Latin-1 value, and the
 * byte CLK_DEG puts in a string - rather than by extending the table, because
 * everything between 91 and 175 would have to be filled in to get to it.
 */
static const uint8_t DEGREE[5] = { 0x00, 0x07, 0x05, 0x07, 0x00 };

/*
 * L with a stroke, which is the one Polish letter that is not its base letter
 * with something added above or below it - the stroke goes through the stem,
 * so there is nowhere to put it but inside the cell. The stem moves one column
 * right of where a plain L has it, which is the only way to get anything to
 * the left of the stroke on a matrix five columns wide.
 */
static const uint8_t L_STROKE[5] = { 0x08, 0x7F, 0x48, 0x40, 0x40 };

/*
 * And the marks, for the eight letters that are.
 *
 * Which columns of the blank row above or below the cell each one occupies.
 * Two dots for an acute and for an ogonek, one for the dot over a Z - which is
 * what those two marks are, and at this size is the only thing that tells a
 * Z-acute from a Z-dot.
 *
 * The row is there because the pitch leaves it: a 5x7 cell drawn on a grid of
 * pitch has a blank line above and below before the next one starts, and a
 * real matrix module with an international character set put its accents in
 * exactly that row.
 */
static const uint8_t ACCENT[CLK_PL_N] = {
    CLK_ACC_BELOW | 0x0C,   /* A ogonek */
    CLK_ACC_ABOVE | 0x0C,   /* C acute  */
    CLK_ACC_BELOW | 0x0C,   /* E ogonek */
    0,                      /* L stroke - inside the cell, above */
    CLK_ACC_ABOVE | 0x0C,   /* N acute  */
    CLK_ACC_ABOVE | 0x0C,   /* O acute  */
    CLK_ACC_ABOVE | 0x0C,   /* S acute  */
    CLK_ACC_ABOVE | 0x06,   /* Z acute  */
    CLK_ACC_ABOVE | 0x04,   /* Z dot    */
};

uint8_t clk_dot5x7_accent(char ch)
{
    const uint8_t c = (uint8_t)ch;
    return (c >= CLK_PL_FIRST && c < CLK_PL_FIRST + CLK_PL_N)
           ? ACCENT[c - CLK_PL_FIRST] : 0;
}

/* Declared here as well as in clock_draw.c's extern, so this file's own
   definition is checked against a prototype rather than standing alone. */
const uint8_t *clk_dot5x7_glyph(char ch);

const uint8_t *clk_dot5x7_glyph(char ch)
{
    uint8_t c = (uint8_t)ch;

    if (c == 0xB0) {
        return DEGREE;
    }
    if (c == CLK_L_STROKE) {
        return L_STROKE;
    }
    if (c >= CLK_PL_FIRST && c < CLK_PL_FIRST + CLK_PL_N) {
        /* The other eight are their base letter with a mark drawn beside the
           cell, so the shape inside the cell is the base letter's. */
        c = (uint8_t)clk_pl_base((char)c);
    }
    if (c >= 'a' && c <= 'z') {
        c = (uint8_t)(c - 'a' + 'A');
    }
    if (c < DOT_FIRST || c > DOT_LAST) {
        return 0;                    /* draw nothing rather than a box */
    }
    return &clk_dot5x7[(size_t)(c - DOT_FIRST) * 5];
}
