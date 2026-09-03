/*
 * matrix - falling glyph rain.
 *
 * A full-screen animation on a machine whose only path to the panel is a
 * software blit, so most of what is here is about not redrawing the screen.
 *
 * The rain is a grid of font cells. Each column owns a head that walks down
 * one cell at a time and a trail behind it that fades to black. Repainting a
 * whole trail every step would be ~15 glyphs per column per step; instead the
 * fade is quantised into FADE_LEVELS bands, and a trail cell is repainted only
 * on the step where a band boundary passes over it. That is one glyph per
 * band, whatever the trail's length, plus the new head and the cell the tail
 * has just left.
 *
 * Flushing is the other half. ngl's flush writes the cache back a whole panel
 * row at a time, so what a frame costs is set by the rows it touches, not by
 * the pixels - which makes narrow per-column flushes the worst possible shape
 * and one whole-screen flush the best. So the frame's updates are drawn in
 * horizontal bands, one flush per band, with bands sized to stay under ngl's
 * "this flush was big" log threshold and untouched bands skipped entirely.
 *
 * Nothing here is linked against ngl: every ngl_* and neos_* symbol is
 * resolved from the syscall table when NeOS loads the image.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_status.h"

#define FRAME_MS      33

/* Cells, not pixels: 1280/16 columns in landscape, 1280/32 rows in portrait. */
#define MAX_COLS      88
#define MAX_ROWS      44

/* Steps in the trail's gradient, and so glyph repaints per column per step. */
#define FADE_LEVELS   8

/* Columns come in a few brightnesses; depth, cheaply. */
#define DIM_CLASSES   3

/* Head, tail erase, one repaint per band, and the odd flicker. */
#define MAX_UPD       (MAX_COLS * (FADE_LEVELS + 3))

/* ngl logs any flush bigger than this, which at 30 fps would be a flood. */
#define BAND_MAX_PX   190000

/* The head is hot enough to read as white without leaving the palette. */
#define COL_HEAD      NGL_RGB(210, 255, 205)

static const char GLYPHS[] =
    "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ:.=+*<>|/\\#$%&@?!{}[]~^";
#define NGLYPHS ((int)(sizeof(GLYPHS) - 1))

/* ------------------------------------------------------------------ */
/* Random                                                              */
/* ------------------------------------------------------------------ */

/*
 * xorshift32, because the ABI has no rand() and this needs no quality - only
 * to not repeat the same rain on every run, which is what the seed is for.
 */
static uint32_t s_rng = 2463534242u;

static inline uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/** Inclusive at both ends. */
static inline int rnd_range(int lo, int hi)
{
    return hi <= lo ? lo : lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static inline char rnd_glyph(void)
{
    return GLYPHS[rnd() % (uint32_t)NGLYPHS];
}

static void seed_rng(void)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* Uptime alone repeats when the app is opened at the same moment twice;
       a stack address moves with whatever the loader placed before us. */
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

/* How fast the rain falls: frames per cell, low and high. Tap to cycle. */
static const struct {
    const char *name;
    uint8_t     lo, hi;
} RATES[] = {
    { "steady",   1, 5 },
    { "fast",     1, 2 },
    { "drifting", 3, 9 },
};
#define NRATES ((int)(sizeof(RATES) / sizeof(RATES[0])))

static int s_rate;

static struct col {
    int16_t head;            /* row of the leading glyph; runs off both ends */
    int16_t len;             /* trail length, in cells */
    uint8_t speed;           /* frames per cell */
    uint8_t tick;            /* frames until the next step */
    uint8_t dim;             /* which brightness class this column draws in */
    char    ch[MAX_ROWS];    /* the glyph standing in each cell, 0 if none */
} s_col[MAX_COLS];

static ngl_rect_t s_area;                /* what the app may draw in */
static int16_t    s_cw, s_ch;            /* cell size = the small font's */
static int        s_cols, s_rows;
static int        s_band_rows, s_nbands; /* flush granularity, in cells */

static ngl_color_t s_fade[DIM_CLASSES][FADE_LEVELS];

/* This frame's changed cells, bucketed into bands only when drawing. */
typedef struct {
    int16_t     x, y;        /* cell coordinates */
    char        ch;          /* 0 = erase the cell */
    ngl_color_t col;
} upd_t;

static upd_t s_upd[MAX_UPD];
static int   s_nupd;

static void push(int16_t cx, int16_t cy, char ch, ngl_color_t col)
{
    if (s_nupd >= MAX_UPD) {
        return;
    }
    upd_t *u = &s_upd[s_nupd++];
    u->x   = cx;
    u->y   = cy;
    u->ch  = ch;
    u->col = col;
}

/* ------------------------------------------------------------------ */
/* Palette                                                             */
/* ------------------------------------------------------------------ */

static void build_palette(void)
{
    /* Quarters, so the classes stay in the theme's green rather than becoming
       three different colours. */
    static const uint8_t weight[DIM_CLASSES] = { 4, 3, 2 };

    for (int d = 0; d < DIM_CLASSES; d++) {
        for (int i = 0; i < FADE_LEVELS; i++) {
            /* Quadratic, not linear: the glow sits close behind the head and
               the rest is a long dark tail, which is what makes the head read
               as the bright thing on screen. */
            const int t = FADE_LEVELS - 1 - i;
            const int span = (FADE_LEVELS - 1) * (FADE_LEVELS - 1);
            int g = 26 + (229 * t * t) / span;
            g = g * weight[d] / 4;
            s_fade[d][i] = NGL_RGB(g / 6, g, g / 4);
        }
    }
}

/** Which band of the gradient a cell `k` behind the head belongs to. */
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
    c->speed = (uint8_t)rnd_range(RATES[s_rate].lo, RATES[s_rate].hi);
    c->tick  = (uint8_t)rnd_range(1, c->speed);
    c->dim   = (uint8_t)(rnd() % DIM_CLASSES);

    /* Start above the screen, by a random distance: that is both how the rain
       staggers in at launch and the gap between one drop and the next. */
    c->head = (int16_t)-rnd_range(1, s_rows);
    memset(c->ch, 0, sizeof(c->ch));
}

static void col_step(struct col *c, int16_t cx)
{
    c->head++;

    const int h = c->head;
    const int tail = h - c->len;

    if (tail >= s_rows) {          /* the whole trail has left the bottom */
        col_reset(c);
        return;
    }

    /* The cell the trail has just left goes back to black. */
    if (tail >= 0 && tail < s_rows && c->ch[tail]) {
        push(cx, (int16_t)tail, 0, 0);
        c->ch[tail] = 0;
    }

    /*
     * Repaint only where a band boundary has moved onto a cell. `prev` is the
     * level the cell one nearer the head was painted with on the last step,
     * which is exactly what this cell is showing now; -1 stands for the head's
     * own colour, so the cell directly behind the head always cools off.
     */
    int prev = -1;
    for (int k = 1; k < c->len; k++) {
        const int r = h - k;
        if (r < 0) {
            break;                 /* everything further back is off the top */
        }
        const int lvl = fade_level(k, c->len);
        if (r < s_rows && lvl != prev && c->ch[r]) {
            push(cx, (int16_t)r, c->ch[r], s_fade[c->dim][lvl]);
        }
        prev = lvl;
    }

    if (h >= 0 && h < s_rows) {
        c->ch[h] = rnd_glyph();
        push(cx, (int16_t)h, c->ch[h], COL_HEAD);
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
 * The updates are scattered over the whole screen, so drawing them in the
 * order they were produced and flushing once would hand ngl eight dirty rects
 * that fold into something close to the full screen. Walking the list once per
 * band instead keeps every dirty rect inside its band - the list is a few
 * hundred entries, so the extra passes cost nothing next to a single glyph.
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
                /* _bg, not plain text: the glyph has to replace whatever stood
                   in the cell before, and one pass beats clear-then-draw. */
                const char str[2] = { u->ch, 0 };
                ngl_text_bg(sc, px, py, str, &ngl_font_small, u->col, TH_BG);
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
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/** (Re)build the grid for the current screen, and start the rain over. */
static void layout(void)
{
    s_area = ngl_app_area();
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

    ngl_surface_t *sc = ngl_screen();
    if (sc) {
        ngl_clear(sc, TH_BG);
        ngl_flush();
    }

    printf("[matrix] %dx%d cells of %dx%d, %d bands of %d\n",
           s_cols, s_rows, s_cw, s_ch, s_nbands, s_band_rows);
}

static bool area_moved(void)
{
    const ngl_rect_t a = ngl_app_area();
    return a.x != s_area.x || a.y != s_area.y || a.w != s_area.w || a.h != s_area.h;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("[matrix] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[matrix] no screen, nothing to draw\n");
        return 0;
    }

    seed_rng();
    build_palette();
    layout();

    neos_status_for("matrix - tap to change the rain", 3000);

    while (!neos_app_close_requested()) {
        /* The tablet can be turned over while this runs: NeOS rotates the
           screen underneath us and the grid no longer fits. Nothing reports
           that, so watch the area itself. */
        if (area_moved()) {
            printf("[matrix] screen changed, relaying out\n");
            layout();
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty)) {
            s_rate = (s_rate + 1) % NRATES;
            /* Re-speed the columns where they stand rather than resetting
               them - the rain changes pace without blinking out. */
            for (int i = 0; i < s_cols; i++) {
                s_col[i].speed = (uint8_t)rnd_range(RATES[s_rate].lo, RATES[s_rate].hi);
                if (s_col[i].tick > s_col[i].speed) {
                    s_col[i].tick = s_col[i].speed;
                }
            }
            char msg[48];
            snprintf(msg, sizeof(msg), "rain: %s", RATES[s_rate].name);
            neos_status_for(msg, 1500);
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
        neos_sleep_ms(FRAME_MS);
    }

    printf("[matrix] closing\n");
    return 0;
}
