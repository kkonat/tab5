/*
 * lanscan - the whole app, which is a loop.
 *
 * Draw, advance the scan, check whether anyone has asked us to go. There is
 * no scheduler entry for an app on this machine - it runs as ordinary code on
 * NeOS's own stack - so exiting is something the app does and not something
 * done to it, and everything under here is written so that the check at the
 * top of the loop is never more than a few milliseconds away.
 */

#include "lanscan.h"
#include "ngl_theme.h"
#include "neos_net.h"
#include "neos_orient.h"

static lan_scan_t s_scan;

/* ------------------------------------------------------------------ */
/* Which way up                                                        */
/* ------------------------------------------------------------------ */

/*
 * Landscape, but not one particular landscape.
 *
 * A table is columns, and the tablet is 720 across the other way up, which is
 * an address and a MAC and nothing else. So portrait is refused - but turning
 * a tablet end for end is something people do without thinking about it, and
 * an app that came up upside down and stayed there would be an app somebody
 * has to put down and pick up again. So the two landscape rotations are
 * followed and the two portrait ones are not.
 *
 * The reading is milli-g integers rather than neos_orient_read()'s floats,
 * because this app is compiled with -Werror=double-promotion and a float on
 * its way into a comparison is one printf away from being a link failure.
 * neos_orient.c's own classify() is the source of the sign convention: with
 * gravity mostly along x, +x is NGL_ROT_270 and -x is NGL_ROT_90.
 *
 * FLIP_MG is well past flat, so a tablet lying on a desk - where x and y are
 * both near zero and the answer is genuinely undetermined - keeps whatever it
 * had. HOLD_MS is the other half of that: a reading has to persist, so
 * carrying the tablet across a room does not rotate the screen twice on the
 * way.
 */
#define FLIP_MG   500
#define HOLD_MS   600

static void follow_gravity(void)
{
    static ngl_rotation_t wanted;
    static uint32_t       since;
    static bool           armed;

    int16_t ax = 0, ay = 0, az = 0;
    if (!neos_imu_accel_mg(&ax, &ay, &az)) {
        return;
    }

    const int16_t mag = ax < 0 ? (int16_t)-ax : ax;
    if (mag < FLIP_MG) {
        armed = false;      /* flat, or on its end: no opinion */
        return;
    }

    const ngl_rotation_t want = (ax > 0) ? NGL_ROT_270 : NGL_ROT_90;
    if (want == ngl_rotation()) {
        armed = false;
        return;
    }

    if (!armed || want != wanted) {
        armed  = true;
        wanted = want;
        since  = lan_now();
        return;
    }
    if (lan_now() - since < HOLD_MS) {
        return;
    }

    armed = false;
    neos_orient_lock(want);
    lan_ui_repaint();
}

/* Something to look at while there is nothing to look at: the address is not
   there yet, or there is no network at all and the Wi-Fi panel is where that
   gets fixed. Not an error - a tablet that has just woken up is in this state
   for a second or two every time. */
static void draw_waiting(const char *line)
{
    static bool cleared;

    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t a = ngl_app_area();

    /* The ground is painted once. After that only the strip the sentence sits
       in is repainted, because this is redrawn every second and clearing 1280
       by 664 to change one digit would be a screen that visibly blinks while
       it waits. */
    if (!cleared) {
        cleared = true;
        ngl_clear(sc, TH_BG);
    }
    const int16_t y = (int16_t)(a.y + a.h / 2 - ngl_font_small.height);
    ngl_fill_rect(sc, ngl_rect(a.x, y, a.w, ngl_font_small.height), TH_BG);

    const int16_t w = ngl_text_width(&ngl_font_small, line);
    ngl_text(sc, (int16_t)(a.x + (a.w - w) / 2), y,
             line, &ngl_font_small, TH_TEXT_DIM);
    ngl_flush();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Start in whichever landscape the tablet is already in, and follow it
     * from there - see follow_gravity(). The lock is what stops the system
     * watcher rotating into portrait behind our back; NeOS clears it when the
     * app returns, so this cannot leave the system stuck sideways.
     */
    ngl_rotation_t r = neos_orient_get();
    if (r != NGL_ROT_90 && r != NGL_ROT_270) {
        r = NGL_ROT_90;
    }
    neos_orient_lock(r);

    if (!lan_model_init(&s_scan.reg)) {
        draw_waiting("not enough memory for the device table");
        while (!neos_app_close_requested()) {
            neos_sleep_ms(100);
        }
        neos_orient_unlock();
        return 0;
    }

    s_scan.depth   = LAN_DEPTH_NORMAL;
    s_scan.icmp_fd = -1;
    s_scan.udp_fd  = -1;
    s_scan.mdns_fd = -1;
    s_scan.mdns_q_fd = -1;
    s_scan.ssdp_fd = -1;

    lan_ui_init(&s_scan);

    /*
     * Wait for an address before starting.
     *
     * NeOS owns the connection and an app only reads its state, so there is
     * nothing to do here but wait and say so. Coming out of the Wi-Fi panel
     * with a fresh association, this is a second; with no network configured
     * at all it is forever, and the message says where to go.
     */
    bool     started = false;
    uint32_t waited  = 0;
    uint32_t drawn   = 0;

    while (!neos_app_close_requested()) {
        follow_gravity();

        if (!started) {
            /*
             * The test is "do we have an address", not "does neos_net_state()
             * say ONLINE", and the difference is the whole reason this reads
             * the way it does. The state enum is a summary meant for a status
             * bar; an address on the interface is the actual precondition for
             * putting a packet on the wire. Asking the enum first meant an app
             * that sat saying "waiting for a network" next to a Wi-Fi icon,
             * which is the worst of both - it neither scanned nor explained
             * itself. So lan_engine_start() is simply attempted, since it asks
             * neos_iface() and that is the real question, and the state is
             * used only to word the wait.
             */
            if (lan_engine_start(&s_scan)) {
                started = true;
                lan_ui_init(&s_scan);
            } else {
                /* The seconds are there so that waiting looks different from
                   hung. It is not hung: the close check is at the top of this
                   loop and nothing below it takes longer than a frame. */
                if (waited == 0) {
                    waited = lan_now();
                }
                const uint32_t secs = (lan_now() - waited) / 1000;

                if (secs != drawn || drawn == 0) {
                    drawn = secs;

                    const char *why;
                    switch (neos_net_state()) {
                    case NEOS_NET_ABSENT:
                        why = "no radio - this tablet cannot see a network";
                        break;
                    case NEOS_NET_OFF:
                        why = "the radio is off - turn it on from the bar";
                        break;
                    case NEOS_NET_CONNECTING:
                        why = "joining a network";
                        break;
                    case NEOS_NET_ONLINE:
                        why = "joined, waiting for an address";
                        break;
                    default:
                        why = "no network - join one from the bar";
                        break;
                    }

                    char line[96];
                    snprintf(line, sizeof(line), "%s   (%us)", why,
                             (unsigned)secs);
                    draw_waiting(line);
                }
                neos_sleep_ms(100);
                continue;
            }
        }

        /*
         * Order matters. The engine is what waits - its TCP stages spend
         * their tick inside neos_sock_wait() - so drawing first means the
         * screen shows the state that produced this frame rather than the one
         * before it, and the loop's idle time is spent in the one place that
         * has something to wait for.
         */
        lan_ui_tick(&s_scan);
        lan_engine_tick(&s_scan);
    }

    lan_engine_stop(&s_scan);
    lan_model_free(&s_scan.reg);
    neos_orient_unlock();
    return 0;
}
