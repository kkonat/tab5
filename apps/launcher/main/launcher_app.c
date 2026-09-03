/*
 * The NeOS launcher - an app like any other.
 *
 * It lives on the card, is named by autorun.cfg, and is loaded through the
 * same ELF path as everything else. Nothing here is linked against ngl or the
 * OS: every ngl_* and neos_* symbol below is left undefined at link time and
 * resolved by the syscall table when NeOS loads this image, which is why the
 * whole shell costs a couple of kilobytes on the card instead of carrying its
 * own copy of the UI toolkit.
 *
 * The system bar is not drawn here. NeOS owns it, so it survives this app
 * handing over to another one, and its close button is the same button
 * wherever you are.
 *
 * Being an app rather than part of the firmware is the point: change one line
 * of autorun.cfg, or put a different card in, and a different shell comes up.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_orient.h"
#include "neos_status.h"

#define UI_MARGIN   20
#define UI_CARD_H   120
#define UI_CARD_GAP 12

/* Colours come from the system theme, not from here. */
#define COL_BG       TH_BG
#define COL_EDGE     TH_EDGE
#define COL_TEXT     TH_TEXT
#define COL_DIM      TH_TEXT_DIM
#define COL_ACCENT   TH_ACCENT

/*
 * What is actually on screen, rebuilt on every redraw.
 *
 * The registry order is not the drawn order - this app leaves itself out -
 * so a tap cannot be turned back into an app without keeping the mapping.
 */
#define MAX_SLOTS 12

static struct {
    ngl_rect_t  rect;
    const char *dir;
} s_slots[MAX_SLOTS];
static int s_nslots;

/* ------------------------------------------------------------------ */
/* The app list                                                        */
/* ------------------------------------------------------------------ */

static void ui_header(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    ngl_clear(sc, COL_BG);                 /* clears all, repaints the bar */

    const ngl_rect_t a = ngl_app_area();
    ngl_text(sc, UI_MARGIN, (int16_t)(a.y + 16), "NeOS", &ngl_font_large, COL_TEXT);
    ngl_line_aa(sc, UI_MARGIN, (int16_t)(a.y + 100),
                (int16_t)(a.w - UI_MARGIN), (int16_t)(a.y + 100), COL_ACCENT);
    ngl_flush();
}

static void ui_app_card(int slot, const neos_app_t *app)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t a = ngl_app_area();
    const int16_t y = (int16_t)(a.y + 120 + slot * (UI_CARD_H + UI_CARD_GAP));
    if (y + UI_CARD_H > a.y + a.h) {
        return;
    }
    const ngl_rect_t card = ngl_rect(UI_MARGIN, y,
                                     (int16_t)(a.w - 2 * UI_MARGIN), UI_CARD_H);

    /*
     * Outline, not a fill. Square corners on purpose: a rounded outline is
     * emitted as one span per scanline, which merges straight back into the
     * bounding box, whereas four straight edges stay four small dirty rects -
     * and boxy suits the terminal look anyway.
     */
    if (slot < MAX_SLOTS) {
        s_slots[slot].rect = card;
        s_slots[slot].dir  = app->dir;
        s_nslots = slot + 1;
    }

    ngl_draw_rect(sc, card, COL_EDGE, 2);

    /* The glyph carries the status, so every icon can share the theme's one
       icon green - no traffic-light colours in a monochrome UI. */
    const ngl_icon_t *ic;
    const char *note;
    if (app->crashes) {
        ic = ngl_icon_find("bomb", 32);
        note = "quarantined after a crash";
    } else if (!app->ok) {
        ic = ngl_icon_find("dizzy", 32);
        note = "manifest unreadable";
    } else {
        ic = ngl_icon_find("smile", 32);
        note = app->desc;
    }
    if (ic) {
        ngl_icon(sc, (int16_t)(card.x + 22), (int16_t)(y + 44), ic, TH_ICON);
    }

    ngl_text(sc, (int16_t)(card.x + 74), (int16_t)(y + 16),
             app->name, &ngl_font_large, COL_TEXT);
    if (note && note[0]) {
        ngl_text(sc, (int16_t)(card.x + 74), (int16_t)(y + 72), note,
                 &ngl_font_small, COL_DIM);
    }
    ngl_flush();
}

/*
 * Redrawn from the registry, not from a local copy. Rotating invalidates the
 * whole framebuffer, and NeOS already holds the scan result - a second copy
 * here would only be a way to disagree with it.
 */
static void ui_redraw_all(void)
{
    ui_header();

    /*
     * The shell leaves itself out. It is an app on the card like any other,
     * so it comes back in the registry, but a card you can tap to launch the
     * thing you are already looking at is just a way to lose your place.
     *
     * Matched on the directory, not the name: the name is display text from
     * the manifest and nothing stops two apps sharing one.
     */
    const char *self = neos_app_self();

    s_nslots = 0;
    const int n = neos_apps_count();
    int slot = 0;
    for (int i = 0; i < n; i++) {
        const neos_app_t *app = neos_apps_get(i);
        if (!app || (self && strcmp(app->dir, self) == 0)) {
            continue;
        }
        ui_app_card(slot++, app);
    }

    if (slot == 0) {
        ngl_surface_t *sc = ngl_screen();
        const ngl_rect_t a = ngl_app_area();
        if (sc) {
            ngl_text(sc, UI_MARGIN, (int16_t)(a.y + 140),
                     "No other apps on this card", &ngl_font_small, COL_DIM);
        }
    }

    /* One flush for the whole list. Flushing per card rotated several
       overlapping regions instead of one. */
    ngl_flush();
}

/** The orientation watcher calls this after it has rotated the screen. */
static void on_rotate(ngl_rotation_t r)
{
    printf("[launcher] rotated to %s, repainting\n", neos_orient_name(r));
    ui_redraw_all();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("[launcher] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    ui_redraw_all();

    const char *self = neos_app_self();
    int n = 0;
    for (int i = 0; i < neos_apps_count(); i++) {
        const neos_app_t *app = neos_apps_get(i);
        if (app && !(self && strcmp(app->dir, self) == 0)) {
            n++;
        }
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "%d app%s on this card", n, n == 1 ? "" : "s");
    neos_status_for(msg, 4000);

    /* Follow the tablet from here on. */
    neos_orient_start_watch(3000, on_rotate);

    /*
     * The event loop. A tap on a card asks NeOS to run that app and then
     * returns - one app is resident at a time, so handing over is exactly
     * "say what you want next, then get out of the way". NeOS brings this
     * shell back up when that app closes.
     */
    uint32_t seen = neos_apps_generation();

    for (;;) {
        /* An upload lands on the card while this is running, so the list is
           redrawn whenever NeOS says the registry moved. */
        const uint32_t now = neos_apps_generation();
        if (now != seen) {
            seen = now;
            printf("[launcher] card changed, redrawing\n");
            ui_redraw_all();
        }

        if (neos_app_close_requested()) {
            printf("[launcher] closing\n");
            return 0;
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty)) {
            for (int i = 0; i < s_nslots; i++) {
                const ngl_rect_t r = s_slots[i].rect;
                if (ngl_rect_contains(&r, tx, ty)) {
                    printf("[launcher] launching %s\n", s_slots[i].dir);
                    neos_exec(s_slots[i].dir);
                    return 0;
                }
            }
        }

        neos_sleep_ms(30);
    }
}
