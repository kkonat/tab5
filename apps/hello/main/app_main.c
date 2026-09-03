/*
 * hello - the smallest useful NeOS app.
 *
 * It says hello and then stays up until it is closed, which is the shape every
 * app with a screen has: draw once, then sit in a loop watching for the close
 * request. Returning from main() is how an app exits - NeOS cannot kill it
 * from outside, because the app is running on NeOS's own stack.
 *
 * Nothing here is linked against ngl. Every ngl_* and neos_* symbol is left
 * undefined at link time and resolved from the syscall table when the app is
 * loaded, so the whole thing is a couple of kilobytes on the card.
 */
#include <stdio.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"

int main(int argc, char **argv)
{
    const char *name = argc > 0 ? argv[0] : "hello";

    printf("[%s] hello\n", name);
    neos_log("hello");

    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        printf("[%s] no screen, nothing to draw\n", name);
        return 0;
    }

    /*
     * Claim the app area, not the whole panel. The strip along the top is the
     * shell's system bar and ngl will not let an app draw there anyway, but
     * asking for the area is how an app stays correct across rotations.
     */
    const ngl_rect_t a = ngl_app_area();

    ngl_clear(sc, TH_BG);

    const char *msg = "hello";
    const int16_t w = ngl_text_width(&ngl_font_large, msg);
    ngl_text(sc, (int16_t)(a.x + (a.w - w) / 2),
             (int16_t)(a.y + a.h / 2 - ngl_font_large.height),
             msg, &ngl_font_large, TH_TEXT);

    const char *hint = "tap the top-right corner to close";
    const int16_t hw = ngl_text_width(&ngl_font_small, hint);
    ngl_text(sc, (int16_t)(a.x + (a.w - hw) / 2),
             (int16_t)(a.y + a.h / 2 + 20),
             hint, &ngl_font_small, TH_TEXT_DIM);

    ngl_flush();

    /*
     * Idle until asked to go. neos_sleep_ms() yields to the scheduler - a busy
     * loop here would starve the orientation watcher and the status line, both
     * of which are ordinary tasks in the firmware.
     */
    while (!neos_app_close_requested()) {
        neos_sleep_ms(100);
    }

    printf("[%s] closing\n", name);
    return 0;
}
