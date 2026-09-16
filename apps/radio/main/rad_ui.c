/*
 * The tab strip, the NOW page, and the widgets the other pages borrow.
 *
 * Repainting is the part worth reading. ngl marks the bounds of every draw
 * call dirty, so a frame that draws one box costs that box's rows of panel
 * bandwidth and a frame that draws nothing costs nothing at all - and this
 * app spends most of its time with nothing changing on screen while a stream
 * plays. So each box hashes what it is about to say and skips the paint when
 * the hash is what it drew there last. A radio that is playing and whose
 * title has not changed draws nothing; one whose track just changed draws the
 * card. Neither is a full-screen repaint.
 *
 * That matters more here than it did on the Pi: this loop also has three
 * seconds of buffered audio to keep flowing, and a needless 1280-by-608
 * repaint is a needless twelve milliseconds not spent on the socket.
 */

#include "rad_ui.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

uint32_t rad_ui_hash(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        seed = (seed ^ p[i]) * 16777619u;       /* FNV-1a */
    }
    return seed;
}

uint32_t rad_ui_hash_str(uint32_t seed, const char *s)
{
    return s ? rad_ui_hash(seed, s, strlen(s)) : seed;
}

ngl_rect_t rad_ui_page(ngl_rect_t area)
{
    return ngl_rect(area.x, (int16_t)(area.y + UI_TAB_H),
                    area.w, (int16_t)(area.h - UI_TAB_H));
}

void rad_ui_text_mid(ngl_surface_t *sc, int16_t x, int16_t y, int16_t w,
                     const char *s, const ngl_font_t *font, ngl_color_t c)
{
    const int16_t tw = ngl_text_width(font, s);
    ngl_text(sc, (int16_t)(x + (w - tw) / 2), y, s, font, c);
}

void rad_ui_ellipsis(char *s, size_t size, const ngl_font_t *font, int16_t w)
{
    if (!s || size == 0 || ngl_text_width(font, s) <= w) {
        return;
    }
    /* Cut rather than wrap, and say so with a full stop or three: two short
       lines of a truncated name are harder to tell apart at a glance than one
       long one. */
    size_t n = strlen(s);
    while (n > 1) {
        s[--n] = 0;
        if (n + 4 > size) {
            continue;           /* no room for the dots yet */
        }
        if (n >= 3) {
            s[n] = s[n + 1] = s[n + 2] = '.';
            s[n + 3] = 0;
            if (ngl_text_width(font, s) <= w) {
                return;
            }
            s[n] = 0;           /* put it back and take another character */
        } else if (ngl_text_width(font, s) <= w) {
            return;
        }
    }
}

void rad_ui_button(ngl_surface_t *sc, ngl_rect_t r, const char *label,
                   const ngl_font_t *font, bool held, bool on, bool enabled)
{
    const ngl_color_t fill = on      ? TH_ACCENT
                           : held    ? TH_KEY_DOWN
                           : enabled ? TH_KEY_FILL
                                     : TH_BG;
    const ngl_color_t edge = on      ? TH_ACCENT
                           : enabled ? TH_EDGE
                                     : TH_RULE;
    const ngl_color_t ink  = on      ? TH_BG
                           : enabled ? TH_KEY_TEXT
                                     : TH_TEXT_FAINT;

    ngl_fill_round_rect(sc, r, 10, fill);
    ngl_draw_round_rect(sc, r, 10, edge, 1);

    if (label && *label) {
        rad_ui_text_mid(sc, r.x, (int16_t)(r.y + (r.h - font->height) / 2),
                        r.w, label, font, ink);
    }
}

void rad_ui_slider(ngl_surface_t *sc, int16_t x, int16_t y, int16_t w,
                   int permille, bool active)
{
    const ngl_rect_t track = ngl_rect(x, y, w, UI_SLIDER_H);
    ngl_fill_round_rect(sc, track, UI_SLIDER_H / 2, TH_BG);
    ngl_draw_round_rect(sc, track, UI_SLIDER_H / 2, TH_EDGE, 1);

    if (permille < 0)    { permille = 0; }
    if (permille > 1000) { permille = 1000; }

    const int16_t span   = (int16_t)(w - 4);
    const int16_t filled = (int16_t)((int32_t)span * permille / 1000);
    if (filled > 0) {
        ngl_fill_round_rect(sc,
            ngl_rect((int16_t)(x + 2), (int16_t)(y + 2),
                     (int16_t)(filled < 8 ? 8 : filled), UI_SLIDER_H - 4),
            (UI_SLIDER_H - 4) / 2, TH_ACCENT);
    }

    /* A knob, so the value is readable at a glance even at the ends, where a
       filled bar alone says "nearly nothing" and "nearly everything" with the
       same few pixels. */
    const int16_t knob = (int16_t)(x + 2 + filled);
    ngl_fill_round_rect(sc, ngl_rect((int16_t)(knob - 9), (int16_t)(y - 5),
                                     18, UI_SLIDER_H + 10), 7,
                        active ? TH_GLOW : TH_TEXT);
}

int rad_ui_slider_permille(int16_t x0, int16_t w, int16_t x)
{
    const int32_t span = w - 4;
    if (span <= 0) {
        return 0;
    }
    int32_t at = ((int32_t)x - x0 - 2) * 1000 / span;
    if (at < 0)    { at = 0; }
    if (at > 1000) { at = 1000; }
    return (int)at;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/*
 * One table for the drawing and the hit test, so the two cannot drift. Every
 * number below is derived from the area the firmware handed over rather than
 * written down, which is what lets the same code lay out the 1280-wide
 * landscape it is designed for without falling apart if the bar changes
 * height.
 */
typedef struct {
    ngl_rect_t page;
    ngl_rect_t card;
    ngl_rect_t play;
    ngl_rect_t vol_down, vol_up;
    ngl_rect_t vol_bar;
    int16_t    stations_y;
    ngl_rect_t page_prev, page_next;
    ngl_rect_t list;
    int16_t    cell_w, cell_h, cell_gap;
    int        cols, rows, per_page;
} now_layout_t;

#define STAR_W  64

static void layout_now(ngl_rect_t area, now_layout_t *l)
{
    const ngl_rect_t p = rad_ui_page(area);
    l->page = p;

    const int16_t left  = (int16_t)(p.x + UI_MARGIN);
    const int16_t width = (int16_t)(p.w - 2 * UI_MARGIN);

    l->card = ngl_rect(left, (int16_t)(p.y + 12), width, 150);

    const int16_t cy = (int16_t)(l->card.y + l->card.h + 16);
    l->play     = ngl_rect(left, cy, 200, 80);
    l->vol_down = ngl_rect((int16_t)(left + 224), cy, 88, 80);
    l->vol_up   = ngl_rect((int16_t)(left + width - 88), cy, 88, 80);
    l->vol_bar  = ngl_rect((int16_t)(l->vol_down.x + l->vol_down.w + 20),
                           (int16_t)(cy + 26),
                           (int16_t)(l->vol_up.x - 20 -
                                     (l->vol_down.x + l->vol_down.w + 20)),
                           UI_SLIDER_H);

    l->stations_y = (int16_t)(cy + 80 + 20);
    l->page_prev  = ngl_rect((int16_t)(left + width - 2 * 56 - 8),
                             (int16_t)(l->stations_y - 6), 56, 44);
    l->page_next  = ngl_rect((int16_t)(left + width - 56),
                             (int16_t)(l->stations_y - 6), 56, 44);

    l->cols     = 3;
    l->rows     = 3;
    l->cell_gap = 10;
    l->cell_w   = (int16_t)((width - (l->cols - 1) * 12) / l->cols);
    l->cell_h   = 84;
    l->per_page = l->cols * l->rows;

    const int16_t list_y = (int16_t)(l->stations_y + 44);
    l->list = ngl_rect(left, list_y, width,
                       (int16_t)(l->rows * (l->cell_h + l->cell_gap)));

    /* If the panel is short - portrait, or a taller system bar one day - the
       grid loses rows rather than running off the bottom. Paging still
       reaches every station, so nothing becomes unreachable. */
    while (l->rows > 1 && list_y + l->rows * (l->cell_h + l->cell_gap) > p.y + p.h) {
        l->rows--;
        l->per_page = l->cols * l->rows;
        l->list.h = (int16_t)(l->rows * (l->cell_h + l->cell_gap));
    }
}

static void cell_origin(const now_layout_t *l, int slot, int16_t *x, int16_t *y)
{
    const int col = slot % l->cols;
    const int row = slot / l->cols;
    *x = (int16_t)(l->list.x + col * (l->cell_w + 12));
    *y = (int16_t)(l->list.y + row * (l->cell_h + l->cell_gap));
}

static int page_count(const rad_app_t *app, const now_layout_t *l)
{
    const int n = (app->stations + l->per_page - 1) / l->per_page;
    return n < 1 ? 1 : n;
}

/* ------------------------------------------------------------------ */
/* The tab strip                                                       */
/* ------------------------------------------------------------------ */

static const char *const TAB_NAME[TAB_COUNT] = { "NOW", "EQ" };

static uint32_t s_tab_sig;
static uint32_t s_card_sig;
static uint32_t s_ctrl_sig;
static uint32_t s_cell_sig[9];
static uint32_t s_head_sig;
static bool     s_full;

void rad_ui_repaint(rad_app_t *app)
{
    (void)app;
    s_full     = true;
    s_tab_sig  = 0;
    s_card_sig = 0;
    s_ctrl_sig = 0;
    s_head_sig = 0;
    for (int i = 0; i < 9; i++) {
        s_cell_sig[i] = 0;
    }
}

void rad_ui_init(rad_app_t *app)
{
    rad_ui_repaint(app);
}

static void draw_tabs(rad_app_t *app, ngl_surface_t *sc, ngl_rect_t area)
{
    uint32_t sig = rad_ui_hash(2166136261u, &app->tab, sizeof(app->tab));
    sig = rad_ui_hash_str(sig, app->pressed);
    if (sig == s_tab_sig && !s_full) {
        return;
    }
    s_tab_sig = sig;

    ngl_fill_rect(sc, ngl_rect(area.x, area.y, area.w, UI_TAB_H), TH_BAR_BG);
    ngl_hline(sc, area.x, (int16_t)(area.y + UI_TAB_H - 1), area.w, TH_RULE);

    ngl_text(sc, (int16_t)(area.x + UI_MARGIN),
             (int16_t)(area.y + (UI_TAB_H - ngl_font_small.height) / 2),
             "radio", &ngl_font_small, TH_ACCENT);

    const int16_t x0 = (int16_t)(area.x + UI_MARGIN + 140);
    for (int i = 0; i < TAB_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "tab:%d", i);

        const ngl_rect_t r = ngl_rect((int16_t)(x0 + i * (UI_TAB_W + UI_TAB_GAP)),
                                      (int16_t)(area.y + 8),
                                      UI_TAB_W, UI_TAB_H - 16);
        rad_ui_button(sc, r, TAB_NAME[i], &ngl_font_small,
                      rad_streq(app->pressed, key), app->tab == (rad_tab_t)i, true);
    }
}

/* ------------------------------------------------------------------ */
/* The NOW card                                                        */
/* ------------------------------------------------------------------ */

/* Break @p text into at most @p max lines that each fit @p w. */
static int wrap(const char *text, const ngl_font_t *font, int16_t w,
                char out[][96], int max)
{
    int lines = 0;
    const char *p = text;

    while (*p && lines < max) {
        char        line[96];
        int         n    = 0;
        const char *fits = NULL;

        while (p[n] && n + 1 < (int)sizeof(line)) {
            line[n] = p[n];
            line[n + 1] = 0;
            n++;
            if (ngl_text_width(font, line) > w) {
                n--;
                line[n] = 0;
                break;
            }
            /* Remember the last word boundary that still fitted, so the break
               lands between words when it can and mid-word when it cannot -
               which is what a stream title full of one long URL needs. */
            if (p[n] == ' ') {
                fits = p + n;
            }
        }

        if (p[n] && fits && fits > p) {
            n = (int)(fits - p);
            line[n] = 0;
        }
        if (n == 0) {
            break;
        }

        rad_copy(out[lines], 96, line);
        lines++;
        p += n;
        while (*p == ' ') {
            p++;
        }
    }

    /* Anything left over is said with a full stop or three on the last line
       rather than silently dropped. */
    if (*p && lines > 0) {
        char last[96];
        rad_copy(last, sizeof(last), out[lines - 1]);
        size_t at = strlen(last);
        while (at > 0) {
            char probe[100];
            snprintf(probe, sizeof(probe), "%s...", last);
            if (ngl_text_width(font, probe) <= w) {
                rad_copy(out[lines - 1], 96, probe);
                break;
            }
            last[--at] = 0;
        }
    }
    return lines;
}

static void draw_card(rad_app_t *app, ngl_surface_t *sc, const now_layout_t *l)
{
    const rad_stream_t *s = &app->stream;

    uint32_t sig = rad_ui_hash(2166136261u, &s->state, sizeof(s->state));
    sig = rad_ui_hash_str(sig, s->title);
    sig = rad_ui_hash_str(sig, s->error);
    sig = rad_ui_hash(sig, &s->retry_in, sizeof(s->retry_in));
    sig = rad_ui_hash(sig, &s->attempts, sizeof(s->attempts));
    sig = rad_ui_hash(sig, &s->buffering, sizeof(s->buffering));
    sig = rad_ui_hash(sig, &s->station, sizeof(s->station));
    if (sig == s_card_sig && !s_full) {
        return;
    }
    s_card_sig = sig;

    ngl_fill_round_rect(sc, l->card, 12, TH_PANEL);
    ngl_draw_round_rect(sc, l->card, 12, TH_EDGE, 1);

    const int16_t tx = (int16_t)(l->card.x + 24);
    const int16_t ty = (int16_t)(l->card.y + 14);
    const int16_t tw = (int16_t)(l->card.w - 48);

    char caption[RAD_ERROR_MAX + RAD_NAME_MAX];
    char body[3][96];

    switch (s->state) {
    case RAD_ERROR: {
        ngl_text(sc, tx, ty, "PROBLEM", &ngl_font_small, TH_BAD);
        const int lines = wrap(s->error, &ngl_font_small, tw, body, 3);
        for (int i = 0; i < lines; i++) {
            ngl_text(sc, tx, (int16_t)(ty + 40 + i * 34), body[i],
                     &ngl_font_small, TH_TEXT);
        }
        break;
    }

    case RAD_RETRYING:
        /* Not an error page: the station is still selected and the app is
           still trying. What went wrong, and a countdown, so that a screen
           which says "reconnecting" and then sits there for fifteen seconds
           is visibly alive rather than hung. */
        snprintf(caption, sizeof(caption), "RECONNECTING   attempt %d",
                 s->attempts + 1);
        ngl_text(sc, tx, ty, caption, &ngl_font_small, TH_WARN);
        snprintf(caption, sizeof(caption), "retrying in %ds", s->retry_in);
        ngl_text(sc, tx, (int16_t)(ty + 38), caption, &ngl_font_large, TH_TEXT);
        if (s->error[0]) {
            char one[96];
            rad_copy(one, sizeof(one), s->error);
            rad_ui_ellipsis(one, sizeof(one), &ngl_font_small, tw);
            ngl_text(sc, tx, (int16_t)(ty + 96), one, &ngl_font_small,
                     TH_TEXT_DIM);
        }
        break;

    case RAD_RESOLVING:
    case RAD_CONNECTING:
        ngl_text(sc, tx, ty, "CONNECTING", &ngl_font_small, TH_TEXT_DIM);
        ngl_text(sc, tx, (int16_t)(ty + 44),
                 s->state == RAD_RESOLVING ? "looking up the host"
                                           : "opening the stream",
                 &ngl_font_large, TH_TEXT_DIM);
        break;

    case RAD_STOPPED:
        ngl_text(sc, tx, ty, "STOPPED", &ngl_font_small, TH_TEXT_DIM);
        ngl_text(sc, tx, (int16_t)(ty + 44),
                 app->stations ? "tap a station" : "stations.conf is empty",
                 &ngl_font_large, TH_TEXT_DIM);
        break;

    case RAD_PLAYING:
    default: {
        const char *name = (s->station >= 0 && s->station < app->stations)
                         ? app->station[s->station].name : "";
        if (s->buffering) {
            snprintf(caption, sizeof(caption), "BUFFERING   %s", name);
        } else {
            snprintf(caption, sizeof(caption), "NOW PLAYING   %s", name);
        }
        rad_ui_ellipsis(caption, sizeof(caption), &ngl_font_small, tw);
        ngl_text(sc, tx, ty, caption, &ngl_font_small, TH_ACCENT);

        const bool  have = s->title[0] != 0;
        const char *what = have ? s->title : "(no track info yet)";
        const int lines = wrap(what, &ngl_font_large, tw, body, 2);
        for (int i = 0; i < lines; i++) {
            ngl_text(sc, tx, (int16_t)(ty + 40 + i * 52), body[i],
                     &ngl_font_large, have ? TH_TEXT : TH_TEXT_DIM);
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

static void draw_transport(rad_app_t *app, ngl_surface_t *sc, const now_layout_t *l)
{
    const bool live = rad_stream_live(&app->stream);

    uint32_t sig = rad_ui_hash(2166136261u, &live, sizeof(live));
    sig = rad_ui_hash(sig, &app->volume, sizeof(app->volume));
    sig = rad_ui_hash_str(sig, app->pressed);
    if (sig == s_ctrl_sig && !s_full) {
        return;
    }
    s_ctrl_sig = sig;

    /* The play/pause key, drawn rather than labelled: two bars or a triangle
       reads at arm's length in a way the words do not. */
    rad_ui_button(sc, l->play, NULL, &ngl_font_small,
                  rad_streq(app->pressed, "play"), live, true);

    const ngl_color_t ink = live ? TH_BG : TH_KEY_TEXT;
    const int16_t     cx  = (int16_t)(l->play.x + l->play.w / 2);
    const int16_t     top = (int16_t)(l->play.y + 18);
    const int16_t     h   = (int16_t)(l->play.h - 36);

    if (live) {
        ngl_fill_rect(sc, ngl_rect((int16_t)(cx - 20), top, 14, h), ink);
        ngl_fill_rect(sc, ngl_rect((int16_t)(cx + 6), top, 14, h), ink);
    } else {
        for (int16_t row = 0; row < h; row++) {
            const int16_t half = row < h / 2 ? row : (int16_t)(h - row);
            ngl_fill_rect(sc, ngl_rect((int16_t)(cx - 17), (int16_t)(top + row),
                                       (int16_t)(half < 1 ? 1 : half), 1), ink);
        }
    }

    rad_ui_button(sc, l->vol_down, "-", &ngl_font_large,
                  rad_streq(app->pressed, "voldown"), false, true);
    rad_ui_button(sc, l->vol_up, "+", &ngl_font_large,
                  rad_streq(app->pressed, "volup"), false, true);

    char caption[32];
    snprintf(caption, sizeof(caption), "volume %d%%", app->volume);
    ngl_fill_rect(sc, ngl_rect(l->vol_bar.x, (int16_t)(l->vol_bar.y - 40),
                               l->vol_bar.w, 34), TH_BG);
    rad_ui_text_mid(sc, l->vol_bar.x, (int16_t)(l->vol_bar.y - 38),
                    l->vol_bar.w, caption, &ngl_font_small, TH_TEXT_DIM);

    rad_ui_slider(sc, l->vol_bar.x, l->vol_bar.y, l->vol_bar.w,
                  app->volume * 10, rad_streq(app->slider, "volume"));
}

/* ------------------------------------------------------------------ */
/* The station grid                                                    */
/* ------------------------------------------------------------------ */

static void draw_stations(rad_app_t *app, ngl_surface_t *sc, const now_layout_t *l)
{
    const int pages = page_count(app, l);

    uint32_t head = rad_ui_hash(2166136261u, &app->page, sizeof(app->page));
    head = rad_ui_hash(head, &pages, sizeof(pages));
    head = rad_ui_hash_str(head, app->pressed);
    if (head != s_head_sig || s_full) {
        s_head_sig = head;

        ngl_fill_rect(sc, ngl_rect(l->list.x, (int16_t)(l->stations_y - 6),
                                   l->list.w, 46), TH_BG);
        ngl_text(sc, l->list.x, l->stations_y, "STATIONS", &ngl_font_small,
                 TH_TEXT_DIM);

        if (pages > 1) {
            char label[24];
            snprintf(label, sizeof(label), "%d/%d", app->page + 1, pages);
            ngl_text(sc,
                     (int16_t)(l->page_prev.x - 24 -
                               ngl_text_width(&ngl_font_small, label)),
                     l->stations_y, label, &ngl_font_small, TH_TEXT_DIM);
            rad_ui_button(sc, l->page_prev, "<", &ngl_font_small,
                          rad_streq(app->pressed, "pageprev"), false,
                          app->page > 0);
            rad_ui_button(sc, l->page_next, ">", &ngl_font_small,
                          rad_streq(app->pressed, "pagenext"), false,
                          app->page < pages - 1);
        }
    }

    for (int slot = 0; slot < l->per_page && slot < 9; slot++) {
        const int index = app->page * l->per_page + slot;

        int16_t x, y;
        cell_origin(l, slot, &x, &y);
        const ngl_rect_t cell = ngl_rect(x, y, l->cell_w, l->cell_h);

        if (index >= app->stations) {
            if (s_cell_sig[slot] != 0 || s_full) {
                s_cell_sig[slot] = 0;
                ngl_fill_rect(sc, cell, TH_BG);
            }
            continue;
        }

        const rad_station_t *st = &app->station[index];
        const bool current = app->stream.station == index &&
                             rad_stream_live(&app->stream);
        char key[16];
        snprintf(key, sizeof(key), "row%d", index);
        const bool down = rad_streq(app->pressed, key);

        uint32_t sig = rad_ui_hash_str(2166136261u, st->name);
        sig = rad_ui_hash(sig, &st->autoplay, sizeof(st->autoplay));
        sig = rad_ui_hash(sig, &st->secure, sizeof(st->secure));
        sig = rad_ui_hash(sig, &current, sizeof(current));
        sig = rad_ui_hash(sig, &down, sizeof(down));
        const bool chosen = app->selected == index;
        sig = rad_ui_hash(sig, &chosen, sizeof(chosen));
        sig |= 1u;                        /* 0 is reserved for "empty slot" */
        if (sig == s_cell_sig[slot] && !s_full) {
            continue;
        }
        s_cell_sig[slot] = sig;

        ngl_fill_round_rect(sc, cell, 10,
                            (down || chosen) ? TH_KEY_LATCH : TH_PANEL);
        ngl_draw_round_rect(sc, cell, 10, current ? TH_ACCENT : TH_EDGE, 1);

        /* The star: which station starts by itself when the app opens. */
        const char *star = st->autoplay ? "*" : "o";
        rad_ui_text_mid(sc, cell.x, (int16_t)(cell.y + (cell.h - ngl_font_large.height) / 2),
                        STAR_W, star, &ngl_font_large,
                        st->autoplay ? TH_ACCENT : TH_TEXT_FAINT);
        ngl_vline(sc, (int16_t)(cell.x + STAR_W), (int16_t)(cell.y + 12),
                  (int16_t)(cell.h - 24), TH_RULE);

        char name[RAD_NAME_MAX];
        rad_copy(name, sizeof(name), st->name);
        const int16_t room = (int16_t)(cell.w - STAR_W - 28);
        rad_ui_ellipsis(name, sizeof(name), &ngl_font_small, room);

        ngl_text(sc, (int16_t)(cell.x + STAR_W + 16),
                 (int16_t)(cell.y + (cell.h - ngl_font_small.height) / 2),
                 name, &ngl_font_small, TH_TEXT);
    }
}

/* ------------------------------------------------------------------ */
/* Drawing, and being touched                                          */
/* ------------------------------------------------------------------ */

void rad_ui_draw(rad_app_t *app)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t area = ngl_app_area();

    now_layout_t l;
    layout_now(area, &l);

    if (s_full) {
        ngl_clear(sc, TH_BG);
    }

    switch (app->tab) {
    case TAB_NOW:
        draw_card(app, sc, &l);
        draw_transport(app, sc, &l);
        draw_stations(app, sc, &l);
        break;

    case TAB_EQ:
        rad_eqpage_draw(app, l.page, s_full);
        break;

    default:
        break;
    }

    draw_tabs(app, sc, area);

    s_full = false;
    ngl_flush();
}

const char *rad_ui_hit(rad_app_t *app, int16_t x, int16_t y)
{
    static char key[24];

    const ngl_rect_t area = ngl_app_area();
    now_layout_t l;
    layout_now(area, &l);

    /* The strip first, from any page. */
    if (y >= area.y && y < area.y + UI_TAB_H) {
        const int16_t x0 = (int16_t)(area.x + UI_MARGIN + 140);
        for (int i = 0; i < TAB_COUNT; i++) {
            const int16_t bx = (int16_t)(x0 + i * (UI_TAB_W + UI_TAB_GAP));
            if (x >= bx && x < bx + UI_TAB_W) {
                snprintf(key, sizeof(key), "tab:%d", i);
                return key;
            }
        }
        return NULL;
    }

    if (app->tab == TAB_EQ) {
        return rad_eqpage_hit(l.page, x, y);
    }

    if (ngl_rect_contains(&l.play, x, y))     { return "play"; }
    if (ngl_rect_contains(&l.vol_down, x, y)) { return "voldown"; }
    if (ngl_rect_contains(&l.vol_up, x, y))   { return "volup"; }

    /* The volume track is given a taller target than it is drawn: a slider
       28 pixels high is a hard thing to land on, and there is nothing else
       on that row to hit by mistake. */
    const ngl_rect_t bar = ngl_rect(l.vol_bar.x, (int16_t)(l.vol_bar.y - 16),
                                    l.vol_bar.w, UI_SLIDER_H + 32);
    if (ngl_rect_contains(&bar, x, y)) {
        return "volbar";
    }

    const int pages = page_count(app, &l);
    if (pages > 1) {
        if (ngl_rect_contains(&l.page_prev, x, y)) { return "pageprev"; }
        if (ngl_rect_contains(&l.page_next, x, y)) { return "pagenext"; }
    }

    for (int slot = 0; slot < l.per_page; slot++) {
        const int index = app->page * l.per_page + slot;
        if (index >= app->stations) {
            break;
        }
        int16_t cx, cy;
        cell_origin(&l, slot, &cx, &cy);
        const ngl_rect_t cell = ngl_rect(cx, cy, l.cell_w, l.cell_h);
        if (ngl_rect_contains(&cell, x, y)) {
            snprintf(key, sizeof(key), x < cx + STAR_W ? "star%d" : "row%d",
                     index);
            return key;
        }
    }
    return NULL;
}

int rad_ui_per_page(void)
{
    now_layout_t l;
    layout_now(ngl_app_area(), &l);
    return l.per_page;
}

int rad_ui_volume_from_x(int16_t x)
{
    now_layout_t l;
    layout_now(ngl_app_area(), &l);
    return (rad_ui_slider_permille(l.vol_bar.x, l.vol_bar.w, x) + 5) / 10;
}
