/*
 * The glyph rain, under a clock.
 *
 * The same idea as the matrix app and the same reason for the shape of it: on
 * a machine whose only path to the panel is a software blit, an effect like
 * this is mostly a story about not redrawing the screen. Each column owns a
 * head that walks down one cell at a time and a trail that fades behind it;
 * the fade is quantised into FADE_LEVELS bands so a trail cell is repainted
 * only on the step where a band boundary crosses it, which is one glyph per
 * band however long the trail is.
 *
 * Three things differ, all because this is a background rather than the whole
 * screen:
 *
 *   It is dimmer. The clock is the thing being read, so the rain tops out
 *   around a third of the brightness the matrix app uses and never reaches
 *   white. A background that competes with the foreground is a bug.
 *
 *   It mixes kanji in. Two fonts, one grid: the cells are 16x32 either way, so
 *   a cell holds a character code and the code says which font draws it - 128
 *   and up is clock_kanji.c, below is the system font. Nothing else in here
 *   knows there are two.
 *
 *   It steps around the clock. The time is drawn over the top, and a drop that
 *   walked through it would paint one cell of a digit black and leave it there
 *   until the next second. So the face reserves the rectangles the text
 *   occupies and this skips any cell that touches one - which also looks
 *   better than compositing: the rain parts around the numbers rather than
 *   running behind them.
 */
#include <string.h>
#include <time.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "clock.h"

/* The kanji face, from tools/genkanji. Same cell as ngl_font_small. */
extern const ngl_font_t clock_font_kanji;

/* Portrait is 45 columns of 16 and 40 rows of 32; landscape is 80 by 22. */
#define MAX_COLS      80
#define MAX_ROWS      40

#define FADE_LEVELS   8
#define DIM_CLASSES   3

/* Head, tail erase, one repaint per band, and the odd flicker. */
#define MAX_UPD       (MAX_COLS * (FADE_LEVELS + 3))

/* ngl logs any flush bigger than this, which at this rate would be a flood. */
#define BAND_MAX_PX   190000

/* How much of the rain is kanji, out of 256. Enough to read as a second
   alphabet, not so much that the latin stops being the texture. */
#define KANJI_SHARE   96

/* ------------------------------------------------------------------ */
/* Random                                                              */
/* ------------------------------------------------------------------ */

static uint32_t s_rng = 2463534242u;

static inline uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static inline int rnd_range(int lo, int hi)
{
    return hi <= lo ? lo : lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static const char LATIN[] =
    "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ:.=+*<>|/#$%&@?";
#define NLATIN ((int)(sizeof(LATIN) - 1))

/** A character code: latin as itself, kanji as its index in its own font. */
static uint8_t rnd_glyph(void)
{
    if ((rnd() & 0xFF) < KANJI_SHARE) {
        const int n = clock_font_kanji.last - clock_font_kanji.first + 1;
        return (uint8_t)(clock_font_kanji.first + (int)(rnd() % (uint32_t)n));
    }
    return (uint8_t)LATIN[rnd() % (uint32_t)NLATIN];
}

static void seed_rng(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    s_rng ^= (uint32_t)ts.tv_nsec * 2654435761u;
    s_rng ^= (uint32_t)(uintptr_t)&ts;
    if (s_rng == 0) {
        s_rng = 1;
    }
    (void)rnd();
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct col {
    int16_t head;              /* row of the leading glyph; runs off both ends */
    int16_t len;
    uint8_t speed;             /* frames per cell */
    uint8_t tick;
    uint8_t dim;               /* which brightness class this column draws in */
    uint8_t ch[MAX_ROWS];      /* the code standing in each cell, 0 if none */
} s_col[MAX_COLS];

static ngl_rect_t s_area;
static int16_t    s_cw, s_ch;
static int        s_cols, s_rows;
static int        s_band_rows, s_nbands;
static bool       s_ready;

static ngl_color_t s_fade[DIM_CLASSES][FADE_LEVELS];
static ngl_color_t s_head;

/* Cells the clock occupies, in cell coordinates, inclusive. */
static struct { int16_t x0, y0, x1, y1; } s_hole[CLK_RAIN_HOLES];
static int s_nholes;

typedef struct {
    int16_t     x, y;
    uint8_t     ch;            /* 0 = erase the cell */
    ngl_color_t col;
} upd_t;

static upd_t s_upd[MAX_UPD];
static int   s_nupd;

/** True if the clock is standing on this cell. */
static inline bool blocked(int16_t cx, int16_t cy)
{
    for (int i = 0; i < s_nholes; i++) {
        if (cx >= s_hole[i].x0 && cx <= s_hole[i].x1 &&
            cy >= s_hole[i].y0 && cy <= s_hole[i].y1) {
            return true;
        }
    }
    return false;
}

static void push(int16_t cx, int16_t cy, uint8_t ch, ngl_color_t col)
{
    if (s_nupd >= MAX_UPD || blocked(cx, cy)) {
        return;
    }
    upd_t *u = &s_upd[s_nupd++];
    u->x   = cx;
    u->y   = cy;
    u->ch  = ch;
    u->col = col;
}

/* ------------------------------------------------------------------ */
/* Reservations                                                        */
/* ------------------------------------------------------------------ */

void clk_rain_clear_reserved(void)
{
    s_nholes = 0;
}

void clk_rain_reserve(ngl_rect_t r)
{
    if (s_nholes >= CLK_RAIN_HOLES || !s_ready || r.w <= 0 || r.h <= 0) {
        return;
    }

    /* Rounded outwards: a cell the text touches at all is a cell the text
       owns, or the rain clips a stroke by a pixel and it shows. */
    const int16_t x0 = (int16_t)((r.x - s_area.x) / s_cw);
    const int16_t y0 = (int16_t)((r.y - s_area.y) / s_ch);
    const int16_t x1 = (int16_t)((r.x + r.w - 1 - s_area.x) / s_cw);
    const int16_t y1 = (int16_t)((r.y + r.h - 1 - s_area.y) / s_ch);

    s_hole[s_nholes].x0 = x0;
    s_hole[s_nholes].y0 = y0;
    s_hole[s_nholes].x1 = x1;
    s_hole[s_nholes].y1 = y1;
    s_nholes++;
}

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static void build_palette(void)
{
    /* Quarters, so the classes read as depth rather than as three colours. */
    static const uint8_t weight[DIM_CLASSES] = { 4, 3, 2 };

    for (int d = 0; d < DIM_CLASSES; d++) {
        for (int i = 0; i < FADE_LEVELS; i++) {
            /*
             * Quadratic, not linear: the glow sits close behind the head and
             * the rest is a long dark tail, which is what makes the head read
             * as the bright thing. The 110 ceiling is where this differs from
             * the matrix app's 255 - the clock has to win.
             */
            const int t = FADE_LEVELS - 1 - i;
            const int span = (FADE_LEVELS - 1) * (FADE_LEVELS - 1);
            int g = 14 + (96 * t * t) / span;
            g = g * weight[d] / 4;
            s_fade[d][i] = NGL_RGB(g / 6, g, g / 4);
        }
    }
    /* Bright enough to be the head of a drop, dim enough to sit behind a
       clock drawn in TH_TEXT. */
    s_head = NGL_RGB(70, 160, 65);
}

static inline int fade_level(int k, int len)
{
    const int l = (k * FADE_LEVELS) / (len > 1 ? len : 1);
    return l >= FADE_LEVELS ? FADE_LEVELS - 1 : l;
}

/* ------------------------------------------------------------------ */
/* Columns                                                             */
/* ------------------------------------------------------------------ */

static void col_reset(struct col *c)
{
    c->len   = (int16_t)rnd_range(5, s_rows);
    c->speed = (uint8_t)rnd_range(2, 7);
    c->tick  = (uint8_t)rnd_range(1, c->speed);
    c->dim   = (uint8_t)(rnd() % DIM_CLASSES);
    c->head  = (int16_t)-rnd_range(1, s_rows);
    memset(c->ch, 0, sizeof(c->ch));
}

static void col_step(struct col *c, int16_t cx)
{
    c->head++;

    const int h = c->head;
    const int tail = h - c->len;

    if (tail >= s_rows) {
        col_reset(c);
        return;
    }

    if (tail >= 0 && tail < s_rows && c->ch[tail]) {
        push(cx, (int16_t)tail, 0, 0);
        c->ch[tail] = 0;
    }

    /*
     * Repaint only where a band boundary has moved onto a cell. `prev` is the
     * level the cell one nearer the head was painted with on the last step,
     * which is what this cell is showing now; -1 stands for the head's own
     * colour, so the cell behind the head always cools off.
     */
    int prev = -1;
    for (int k = 1; k < c->len; k++) {
        const int r = h - k;
        if (r < 0) {
            break;
        }
        const int lvl = fade_level(k, c->len);
        if (r < s_rows && lvl != prev && c->ch[r]) {
            push(cx, (int16_t)r, c->ch[r], s_fade[c->dim][lvl]);
        }
        prev = lvl;
    }

    if (h >= 0 && h < s_rows) {
        c->ch[h] = rnd_glyph();
        push(cx, (int16_t)h, c->ch[h], s_head);
    }
}

/** Re-roll one glyph in the trail: the flicker the effect is known for. */
static void col_flicker(struct col *c, int16_t cx)
{
    if ((rnd() & 15) != 0 || c->len < 2) {
        return;
    }
    const int k = rnd_range(1, c->len - 1);
    const int r = c->head - k;
    if (r < 0 || r >= s_rows || !c->ch[r]) {
        return;
    }
    c->ch[r] = rnd_glyph();
    push(cx, (int16_t)r, c->ch[r], s_fade[c->dim][fade_level(k, c->len)]);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Band by band, one flush each.
 *
 * ngl's flush writes the cache back a whole panel row at a time, so a frame
 * costs what its rows cost and not what its pixels do - which makes scattered
 * updates plus one flush the worst possible shape and horizontal bands the
 * best. The list is a few hundred entries, so walking it once per band costs
 * nothing next to a single glyph.
 */
static void draw_frame(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        s_nupd = 0;
        return;
    }

    for (int b = 0; b < s_nbands; b++) {
        const int16_t y0 = (int16_t)(b * s_band_rows);
        const int16_t y1 = (int16_t)(y0 + s_band_rows);
        bool any = false;

        for (int i = 0; i < s_nupd; i++) {
            const upd_t *u = &s_upd[i];
            if (u->y < y0 || u->y >= y1) {
                continue;
            }
            const int16_t px = (int16_t)(s_area.x + u->x * s_cw);
            const int16_t py = (int16_t)(s_area.y + u->y * s_ch);

            if (u->ch) {
                /* One font or the other, by code. _bg rather than plain text:
                   the glyph has to replace whatever stood in the cell, and one
                   pass beats clear-then-draw. */
                const char str[2] = { (char)u->ch, 0 };
                const ngl_font_t *f = (u->ch >= clock_font_kanji.first)
                                          ? &clock_font_kanji : &ngl_font_small;
                ngl_text_bg(sc, px, py, str, f, u->col, TH_BG);
            } else {
                ngl_fill_rect(sc, ngl_rect(px, py, s_cw, s_ch), TH_BG);
            }
            any = true;
        }

        if (any) {
            ngl_flush();
        }
    }

    s_nupd = 0;
}

/* ------------------------------------------------------------------ */

void clk_rain_layout(ngl_rect_t area)
{
    if (!s_ready) {
        seed_rng();
        build_palette();
        s_ready = true;
    }

    s_area = area;
    s_cw = (int16_t)ngl_font_small.width;
    s_ch = (int16_t)ngl_font_small.height;

    s_cols = s_area.w / s_cw;
    s_rows = s_area.h / s_ch;
    if (s_cols > MAX_COLS) { s_cols = MAX_COLS; }
    if (s_rows > MAX_ROWS) { s_rows = MAX_ROWS; }
    if (s_cols < 1) { s_cols = 1; }
    if (s_rows < 2) { s_rows = 2; }

    /* As tall as a band can be without ngl deciding the flush is worth a log
       line - which is also about where a flush stops being cheap. */
    s_band_rows = BAND_MAX_PX / (s_area.w * s_ch);
    if (s_band_rows < 1) { s_band_rows = 1; }
    if (s_band_rows > s_rows) { s_band_rows = s_rows; }
    s_nbands = (s_rows + s_band_rows - 1) / s_band_rows;

    for (int i = 0; i < s_cols; i++) {
        col_reset(&s_col[i]);
    }
    s_nupd = 0;
    s_nholes = 0;
}

void clk_rain_step(void)
{
    if (!s_ready) {
        return;
    }
    for (int i = 0; i < s_cols; i++) {
        struct col *c = &s_col[i];
        if (--c->tick == 0) {
            c->tick = c->speed;
            col_step(c, (int16_t)i);
        }
        col_flicker(c, (int16_t)i);
    }
    draw_frame();
}
