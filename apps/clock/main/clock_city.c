/*
 * Picking where the weather is, by pointing at it.
 *
 * The name in the place band is whatever the address the tablet is using
 * resolved to, which is right about as often as an IP database is - so it has
 * to be possible to say "no, here". The question is what a panel like this
 * could possibly ask it with, and the answer is not a list: neither of the two
 * faces that show the place has a scrolling list in it, and a menu of fifteen
 * names in a typeface would be the one thing on either face that gives the
 * whole act away.
 *
 * A map does not have that problem. A dot-matrix module showing an outline and
 * some marks is exactly the kind of thing these panels were built out of - one
 * more window in the same glass, addressed the same way - and it needs no
 * words at all, which is what makes it the only version of this that fits.
 *
 * So: a grid of dots, all of them visible because that is what an unlit cell
 * of a real matrix looks like; the border driven; the cities driven as squares
 * rather than dots, because a mark that means something else should not be the
 * same shape as the ground it is on. Touch anywhere and the nearest city wins,
 * which is what makes fifteen targets on a small panel usable - you are not
 * aiming at a square, you are aiming at a part of the country.
 *
 * What it does NOT do is move the weather. neos_weather.h has no way to say
 * where to fetch for - the service geolocates the address and that is the
 * whole of its interface - so this changes the name on the glass and nothing
 * else. Making the reading follow the pin is a syscall NeOS does not have yet.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"

#include "clock.h"

/* ------------------------------------------------------------------ */
/* The country                                                         */
/* ------------------------------------------------------------------ */

/*
 * Everything below is in hundredths of a degree, which is the only unit this
 * app has: it is built -Werror=double-promotion, so a coordinate is an integer
 * or it is a link error. A hundredth of a degree is about a kilometre, which
 * is four times finer than one cell of the grid and therefore free.
 */
#define LAT_MIN 4895
#define LAT_MAX 5490
#define LON_MIN 1405
#define LON_MAX 2420

/*
 * The border, once round, starting at the north-west corner and going east
 * along the coast.
 *
 * Simplified hard on purpose. The grid below is forty cells across for ten
 * degrees of longitude, so one cell is most of twenty kilometres and any
 * detail finer than that is thrown away by the rasteriser anyway - what has to
 * survive is the silhouette: the flat Baltic top, the notch Kaliningrad cuts
 * into it, the eastern bulge, the mountain arc along the south and the Oder
 * running straight down the west.
 */
static const int16_t BORDER[][2] = {
    /* the Baltic coast, west to east */
    { 5392, 1425 }, { 5405, 1455 }, { 5410, 1500 }, { 5415, 1530 },
    { 5418, 1580 }, { 5435, 1630 }, { 5458, 1670 }, { 5470, 1710 },
    { 5476, 1750 }, { 5480, 1810 }, { 5450, 1850 }, { 5435, 1870 },
    { 5442, 1910 }, { 5445, 1940 },
    /* the Kaliningrad border, flat across the top */
    { 5443, 1965 }, { 5440, 2050 }, { 5435, 2150 }, { 5433, 2250 },
    { 5440, 2290 }, { 5438, 2300 },
    /* Lithuania, down to the Suwalki gap */
    { 5430, 2310 }, { 5400, 2340 }, { 5385, 2352 },
    /* Belarus, following the Bug south */
    { 5330, 2380 }, { 5270, 2390 }, { 5240, 2360 }, { 5230, 2320 },
    { 5210, 2330 }, { 5195, 2370 }, { 5170, 2360 }, { 5150, 2320 },
    { 5120, 2350 }, { 5080, 2380 },
    /* Ukraine, with the bulge at Hrubieszow and the tip of the Bieszczady */
    { 5085, 2415 }, { 5050, 2400 }, { 5040, 2370 }, { 5020, 2340 },
    { 4980, 2300 }, { 4960, 2280 }, { 4940, 2260 }, { 4910, 2290 },
    { 4900, 2280 },
    /* Slovakia, west along the Carpathians and the Tatras */
    { 4920, 2250 }, { 4940, 2200 }, { 4930, 2150 }, { 4940, 2100 },
    { 4930, 2050 }, { 4920, 2020 }, { 4940, 1990 }, { 4960, 1960 },
    { 4950, 1930 }, { 4940, 1900 }, { 4950, 1880 },
    /* the Czech border, round the Sudetes */
    { 4990, 1860 }, { 4990, 1830 }, { 5000, 1800 }, { 5030, 1780 },
    { 5020, 1760 }, { 5030, 1740 }, { 5020, 1720 }, { 5010, 1700 },
    { 5015, 1680 }, { 5040, 1660 }, { 5060, 1640 }, { 5080, 1620 },
    { 5090, 1600 }, { 5090, 1570 }, { 5100, 1520 }, { 5090, 1490 },
    { 5095, 1480 },
    /* and Germany, straight up the Neisse and the Oder */
    { 5120, 1475 }, { 5150, 1470 }, { 5180, 1465 }, { 5200, 1460 },
    { 5225, 1455 }, { 5250, 1445 }, { 5270, 1440 }, { 5280, 1415 },
    { 5300, 1412 }, { 5320, 1425 }, { 5330, 1440 }, { 5350, 1440 },
    { 5370, 1428 },
};
#define BORDER_N ((int)(sizeof(BORDER) / sizeof(BORDER[0])))

/*
 * The cities. Fifteen, which is as many marks as forty cells of grid can carry
 * without two of them landing on the same one.
 *
 * `key` is what goes in the settings file and is never retitled, for the same
 * reason the face is stored by key: this list will grow, and a stored 7 would
 * quietly become a different town.
 *
 * `name` is UTF-8 as the place is actually spelled, and goes through
 * clk_pl_from_utf8() like anything else - so the VFD prints the diacritics and
 * the LCD folds them, and neither of them has a second spelling compiled in.
 */
static const struct {
    const char *key;
    const char *name;
    int16_t     lat, lon;
} CITY[] = {
    { "warszawa",  "Warszawa",     5223, 2101 },
    { "krakow",    "Kraków",       5006, 1994 },
    { "lodz",      "Łódź",         5177, 1946 },
    { "wroclaw",   "Wrocław",      5111, 1704 },
    { "poznan",    "Poznań",       5241, 1693 },
    { "gdansk",    "Gdańsk",       5435, 1865 },
    { "szczecin",  "Szczecin",     5343, 1455 },
    { "bydgoszcz", "Bydgoszcz",    5312, 1801 },
    { "lublin",    "Lublin",       5125, 2257 },
    { "bialystok", "Białystok",    5313, 2316 },
    { "katowice",  "Katowice",     5026, 1902 },
    { "rzeszow",   "Rzeszów",      5004, 2199 },
    { "olsztyn",   "Olsztyn",      5378, 2049 },
    { "kielce",    "Kielce",       5087, 2063 },
    { "zielona",   "Zielona Góra", 5194, 1551 },
};
#define CITY_N ((int)(sizeof(CITY) / sizeof(CITY[0])))

/* ------------------------------------------------------------------ */
/* The choice                                                          */
/* ------------------------------------------------------------------ */

static int s_chosen = -2;          /* -2: the settings file has not been read */

static void ensure_loaded(void)
{
    if (s_chosen != -2) {
        return;
    }
    s_chosen = -1;

    const char *key = clk_prefs_city();
    if (!key) {
        return;
    }
    for (int i = 0; i < CITY_N; i++) {
        if (strcmp(CITY[i].key, key) == 0) {
            s_chosen = i;
            return;
        }
    }
    /* A key this build does not know is not an error - the list can shrink
       between versions - so it falls back to wherever the tablet thinks it is,
       which is the same thing a first run does. */
}

const char *clk_city_chosen(void)
{
    ensure_loaded();
    return s_chosen >= 0 ? CITY[s_chosen].name : 0;
}

/* ------------------------------------------------------------------ */
/* The panel                                                           */
/* ------------------------------------------------------------------ */

/*
 * Forty cells by thirty-eight, which is not arbitrary: Poland is about six
 * hundred and ninety kilometres across and six hundred and fifty down, so a
 * grid of very nearly square cells over that box is very nearly square itself,
 * and the country comes out the shape it is without anything being corrected
 * for latitude.
 */
#define GRID_W 40
#define GRID_H 38

#define PANEL_MARGIN 24
#define PANEL_PAD    18

static struct {
    ngl_rect_t panel, map;
    int16_t    pitch, dot;
} P;

static void panel_layout(ngl_rect_t area)
{
    int16_t pitch = (int16_t)((area.w - 2 * PANEL_MARGIN - 2 * PANEL_PAD) / GRID_W);
    const int16_t by_h = (int16_t)((area.h - 2 * PANEL_MARGIN - 2 * PANEL_PAD) / GRID_H);
    if (by_h < pitch) {
        pitch = by_h;
    }
    if (pitch < 3) {
        pitch = 3;
    }
    P.pitch = pitch;
    P.dot   = (int16_t)(pitch * 4 / 5);   /* the same dot the VFD draws */
    if (P.dot < 1) {
        P.dot = 1;
    }

    const int16_t mw = (int16_t)(GRID_W * pitch);
    const int16_t mh = (int16_t)(GRID_H * pitch);
    const int16_t pw = (int16_t)(mw + 2 * PANEL_PAD);
    const int16_t ph = (int16_t)(mh + 2 * PANEL_PAD);

    P.panel = ngl_rect((int16_t)(area.x + (area.w - pw) / 2),
                       (int16_t)(area.y + (area.h - ph) / 2), pw, ph);
    P.map = ngl_rect((int16_t)(P.panel.x + PANEL_PAD),
                     (int16_t)(P.panel.y + PANEL_PAD), mw, mh);
}

/** Cell of the grid a coordinate falls in. Clamped, so nothing lands outside. */
static void project(int16_t lat, int16_t lon, int16_t *cx, int16_t *cy)
{
    int32_t x = (int32_t)(lon - LON_MIN) * (GRID_W - 1) / (LON_MAX - LON_MIN);
    int32_t y = (int32_t)(LAT_MAX - lat) * (GRID_H - 1) / (LAT_MAX - LAT_MIN);
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x > GRID_W - 1) { x = GRID_W - 1; }
    if (y > GRID_H - 1) { y = GRID_H - 1; }
    *cx = (int16_t)x;
    *cy = (int16_t)y;
}

static void draw_dot(ngl_surface_t *s, int16_t cx, int16_t cy, ngl_color_t c)
{
    const int16_t inset = (int16_t)((P.pitch - P.dot) / 2);
    const ngl_rect_t r = ngl_rect((int16_t)(P.map.x + cx * P.pitch + inset),
                                  (int16_t)(P.map.y + cy * P.pitch + inset),
                                  P.dot, P.dot);
    if (P.dot <= 2) {
        ngl_fill_rect(s, r, c);
    } else {
        ngl_fill_round_rect(s, r, (int16_t)(P.dot / 2), c);
    }
}

/** A city: the whole cell, square. A mark that means something else. */
static void draw_mark(ngl_surface_t *s, int16_t cx, int16_t cy, ngl_color_t c)
{
    ngl_fill_rect(s, ngl_rect((int16_t)(P.map.x + cx * P.pitch),
                              (int16_t)(P.map.y + cy * P.pitch),
                              P.pitch, P.pitch), c);
}

/** Every cell between two of the border's points, walked one at a time. */
static void draw_edge(ngl_surface_t *s, int a, int b, ngl_color_t c)
{
    int16_t x0, y0, x1, y1;
    project(BORDER[a][0], BORDER[a][1], &x0, &y0);
    project(BORDER[b][0], BORDER[b][1], &x1, &y1);

    const int16_t dx = (int16_t)(x1 - x0), dy = (int16_t)(y1 - y0);
    int16_t n = (int16_t)((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                          ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy));
    if (n < 1) {
        n = 1;
    }
    /* One step per cell of the longer axis, which is the coarsest line that
       still leaves no gaps - and on a grid of dots a gap is a hole in the
       country rather than an artefact. */
    for (int16_t i = 0; i <= n; i++) {
        draw_dot(s, (int16_t)(x0 + (int32_t)dx * i / n),
                 (int16_t)(y0 + (int32_t)dy * i / n), c);
    }
}

static void panel_paint(ngl_surface_t *s, ngl_color_t bg, ngl_color_t on,
                        ngl_color_t off)
{
    ngl_fill_rect(s, P.panel, bg);
    ngl_draw_rect(s, P.panel, on, 2);

    /* The whole matrix first. On a real one the cells are there whether they
       are driven or not, and a map drawn on a grid you cannot see is just a
       map. */
    for (int16_t y = 0; y < GRID_H; y++) {
        for (int16_t x = 0; x < GRID_W; x++) {
            draw_dot(s, x, y, off);
        }
    }

    for (int i = 0; i < BORDER_N; i++) {
        draw_edge(s, i, (i + 1) % BORDER_N, on);
    }

    for (int i = 0; i < CITY_N; i++) {
        int16_t cx, cy;
        project(CITY[i].lat, CITY[i].lon, &cx, &cy);
        draw_mark(s, cx, cy, on);
    }

    ngl_flush();
}

/**
 * Which city a touch meant.
 *
 * Nearest in cells rather than in kilometres, because the cells are what is on
 * the glass and what the finger was aimed at - and because the projection is
 * near enough square that the two answers only differ where they do not
 * matter. Squared distance, so nothing has to take a root.
 */
static int nearest(int16_t tx, int16_t ty)
{
    const int16_t px = (int16_t)((tx - P.map.x) / P.pitch);
    const int16_t py = (int16_t)((ty - P.map.y) / P.pitch);

    int best = -1;
    int32_t bd = 0;
    for (int i = 0; i < CITY_N; i++) {
        int16_t cx, cy;
        project(CITY[i].lat, CITY[i].lon, &cx, &cy);
        const int32_t dx = cx - px, dy = cy - py;
        const int32_t d = dx * dx + dy * dy;
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    return best;
}

bool clk_city_pick(const clock_frame_t *f, ngl_color_t bg, ngl_color_t on,
                   ngl_color_t off)
{
    ngl_surface_t *s = ngl_screen();
    if (!s || !f) {
        return false;
    }
    ensure_loaded();

    ngl_rect_t area = f->area;
    panel_layout(area);
    panel_paint(s, bg, on, off);

    while (!neos_app_close_requested()) {
        const ngl_rect_t now = ngl_app_area();
        if (now.x != area.x || now.y != area.y || now.w != area.w || now.h != area.h) {
            /* Turned over with the map open. */
            area = now;
            panel_layout(area);
            panel_paint(s, bg, on, off);
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty)) {
            if (!ngl_rect_contains(&P.map, tx, ty)) {
                return false;      /* off the map: nothing was meant by it */
            }
            const int i = nearest(tx, ty);
            if (i < 0) {
                return false;
            }
            s_chosen = i;
            clk_prefs_save_city(CITY[i].key);
            return true;
        }

        neos_sleep_ms(30);
    }
    return false;
}
