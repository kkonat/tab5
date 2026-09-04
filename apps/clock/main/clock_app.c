/*
 * clock - the shell.
 *
 * Owns the four things none of the faces should have to: what time it is, when
 * the screen moved, which face is showing, and how that survives a reboot.
 * A face gets a clock_frame_t and a rectangle and is asked to paint; it is
 * never told about rotation, taps, the settings file or the other faces.
 *
 * The loop is built around doing as little as possible. This is a software
 * blitter with no hardware acceleration and no partial-panel update, so the
 * only way a clock runs all day without eating a core is for a frame in which
 * nothing changed to cost nothing - which is what the sec/min/full flags in
 * clock_frame_t are for. A face that ignores them still works; it is simply
 * expensive.
 *
 * The tick is read from the clock rather than counted. Sleeping for a face's
 * frame time and incrementing a second every twenty frames drifts, and drifts
 * visibly on a display somebody is looking at precisely to know what time it
 * is - so every frame asks, and the flags come from what changed.
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"

#include "clock.h"

/* ------------------------------------------------------------------ */
/* The faces                                                           */
/* ------------------------------------------------------------------ */

/*
 * Order is the order the picker shows them in, and nothing else depends on it -
 * the settings file stores a key, so this list can be reordered or grown
 * without a card full of clocks changing their minds about which face they
 * were set to.
 */
static const clock_face_t *const FACES[] = {
    &clock_face_neos,
    &clock_face_led_green,
    &clock_face_led_red,
    &clock_face_lcd,
    &clock_face_vfd,
    &clock_face_epaper,
};
#define NFACES ((int)(sizeof(FACES) / sizeof(FACES[0])))

static int s_face;

static int face_by_key(const char *key)
{
    if (key) {
        for (int i = 0; i < NFACES; i++) {
            if (strcmp(FACES[i]->key, key) == 0) {
                return i;
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* The picker                                                          */
/* ------------------------------------------------------------------ */

/*
 * Drawn by the app, not by NeOS.
 *
 * ngl's overlay machinery - the thing that saves the pixels underneath and
 * drops everyone else's draws - is not exported to apps, and should not be: an
 * app that could take the screen from the OS could take it from the close
 * button. So this is an ordinary opaque panel drawn over the face, and getting
 * back is an ordinary full repaint. The face has to be able to do one anyway.
 */

#define PICK_MARGIN 28
#define PICK_TITLE  56
#define PICK_PAD    14

static ngl_rect_t s_rows[NFACES];
static ngl_rect_t s_panel;

static void pick_layout(ngl_rect_t area)
{
    s_panel = ngl_rect((int16_t)(area.x + PICK_MARGIN),
                       (int16_t)(area.y + PICK_MARGIN),
                       (int16_t)(area.w - 2 * PICK_MARGIN),
                       (int16_t)(area.h - 2 * PICK_MARGIN));

    /* The rows divide whatever is left after the title, so the panel fits both
       ways round without any of this being tuned per orientation. */
    const int16_t top = (int16_t)(s_panel.y + PICK_TITLE);
    const int16_t avail = (int16_t)(s_panel.h - PICK_TITLE - PICK_PAD);
    const int16_t rh = (int16_t)(avail / NFACES);

    for (int i = 0; i < NFACES; i++) {
        s_rows[i] = ngl_rect((int16_t)(s_panel.x + PICK_PAD),
                             (int16_t)(top + i * rh),
                             (int16_t)(s_panel.w - 2 * PICK_PAD),
                             (int16_t)(rh - 6));
    }
}

static void pick_paint(ngl_surface_t *s)
{
    ngl_fill_round_rect(s, s_panel, 12, TH_MODAL_BG);
    ngl_draw_round_rect(s, s_panel, 12, TH_MODAL_EDGE, 2);

    ngl_text(s, (int16_t)(s_panel.x + PICK_PAD + 4),
             (int16_t)(s_panel.y + (PICK_TITLE - ngl_font_large.height) / 2),
             "Face", &ngl_font_large, TH_TEXT);
    ngl_hline(s, (int16_t)(s_panel.x + PICK_PAD),
              (int16_t)(s_panel.y + PICK_TITLE - 4),
              (int16_t)(s_panel.w - 2 * PICK_PAD), TH_RULE);

    for (int i = 0; i < NFACES; i++) {
        const ngl_rect_t r = s_rows[i];
        const bool on = (i == s_face);

        ngl_fill_round_rect(s, r, 8, on ? TH_KEY_LATCH : TH_KEY_FILL);
        ngl_draw_round_rect(s, r, 8, on ? TH_ACCENT : TH_KEY_EDGE, on ? 2 : 1);

        /* Name and blurb stacked, so the row says what the face is without the
           list needing a second column. */
        const int16_t x = (int16_t)(r.x + 14);
        const int16_t both = (int16_t)(ngl_font_large.height + ngl_font_small.height + 2);
        const int16_t y = (int16_t)(r.y + (r.h - both) / 2);

        ngl_text(s, x, y, FACES[i]->name, &ngl_font_large,
                 on ? TH_GLOW : TH_KEY_TEXT);
        ngl_text(s, x, (int16_t)(y + ngl_font_large.height + 2), FACES[i]->blurb,
                 &ngl_font_small, on ? TH_TEXT_DIM : TH_TEXT_FAINT);
    }

    ngl_flush();
}

/**
 * Run the picker until something is chosen or it is dismissed.
 *
 * @return the chosen face, or -1 if nothing changed.
 *
 * Blocking, which is what makes it modal without anything having to be
 * suspended: one app is resident and it is the one that stopped here.
 */
static int pick_run(void)
{
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        return -1;
    }

    ngl_rect_t area = ngl_app_area();
    pick_layout(area);
    ngl_clear(s, TH_BG);
    pick_paint(s);

    /* Whatever tap opened this has already been collected, so the first one
       that arrives from here is a real answer. */
    while (!neos_app_close_requested()) {
        const ngl_rect_t now = ngl_app_area();
        if (now.x != area.x || now.y != area.y || now.w != area.w || now.h != area.h) {
            /* Turned over with the list open. */
            area = now;
            pick_layout(area);
            ngl_clear(s, TH_BG);
            pick_paint(s);
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty)) {
            for (int i = 0; i < NFACES; i++) {
                if (ngl_rect_contains(&s_rows[i], tx, ty)) {
                    return i;
                }
            }
            /* Anywhere else, panel included, closes it. A list of six things
               does not need a cancel button on it. */
            return -1;
        }

        neos_sleep_ms(30);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static bool area_moved(const ngl_rect_t *a)
{
    const ngl_rect_t n = ngl_app_area();
    return n.x != a->x || n.y != a->y || n.w != a->w || n.h != a->h;
}

int main(int argc, char **argv)
{
    printf("[clock] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[clock] no screen, nothing to draw\n");
        return 0;
    }

    const int saved = face_by_key(clk_prefs_load());
    s_face = saved >= 0 ? saved : 0;
    printf("[clock] face \"%s\"%s\n", FACES[s_face]->key,
           saved >= 0 ? "" : " (nothing saved, using the default)");

    clock_frame_t f;
    memset(&f, 0, sizeof(f));
    f.area = ngl_app_area();
    f.full = true;

    /* Impossible values, so the first frame counts as both edges whatever the
       clock says - including on a machine that does not know the time. */
    int last_sec = -1, last_min = -1;
    bool was_busy = false;

    neos_status_for("clock - tap to change the face", 3000);

    while (!neos_app_close_requested()) {
        const clock_face_t *face = FACES[s_face];

        /*
         * A system panel - the keyboard, the Wi-Fi list, NeOS's own clock page
         * - drops everything an app draws for as long as it is up. Nothing
         * reports that it has gone, so what is on screen when it does is
         * whatever was underneath, and the only safe answer is to paint the
         * lot again.
         */
        const bool busy = neos_ui_busy();
        if (busy) {
            was_busy = true;
            neos_sleep_ms(100);
            continue;
        }
        if (was_busy) {
            was_busy = false;
            f.full = true;
        }

        /* NeOS rotates the screen underneath a running app and does not tell
           it, so the area itself is what gets watched. */
        if (area_moved(&f.area)) {
            f.area = ngl_app_area();
            f.full = true;
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap(&tx, &ty)) {
            /*
             * The face first. Two of them have a band worth pointing at, and
             * a tap that lands on it should not also be the tap that opens the
             * list of faces - so a face that consumed one says so, and gets
             * the full repaint it needs on the way back.
             */
            if (face->tap && face->tap(&f, tx, ty)) {
                f.area = ngl_app_area();
                f.full = true;
                last_sec = last_min = -1;
                continue;
            }

            const int chosen = pick_run();
            if (chosen >= 0 && chosen != s_face) {
                /* Before the switch, not after: the face being left is the one
                   holding whatever it allocated, and it is about to stop being
                   reachable from the table. */
                if (face->leave) {
                    face->leave();
                }
                s_face = chosen;
                clk_prefs_save_face(FACES[s_face]->key);
                neos_status_for(FACES[s_face]->name, 1200);
            }
            /* Either way the panel was over the face, so it all comes back. */
            f.area = ngl_app_area();
            f.full = true;
            last_sec = last_min = -1;
            continue;
        }

        f.rot       = ngl_rotation();
        f.landscape = f.area.w > f.area.h;
        f.have_time = neos_time_local(&f.t);
        f.ms        = (uint32_t)neos_uptime_ms();

        const int sec = f.have_time ? f.t.sec : -1;
        const int min = f.have_time ? f.t.min : -1;
        f.sec = f.full || sec != last_sec;
        f.min = f.full || min != last_min;
        last_sec = sec;
        last_min = min;

        face->paint(&f, face->cfg);
        f.full = false;

        neos_sleep_ms(face->frame_ms);
    }

    if (FACES[s_face]->leave) {
        FACES[s_face]->leave();
    }

    printf("[clock] closing\n");
    return 0;
}
