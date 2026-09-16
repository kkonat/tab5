/*
 * wifiscan - the whole app, which is a loop.
 *
 * Draw, ask the radio for another scan, check whether anyone has asked us to
 * go. There is no scheduler entry for an app on this machine - it runs as
 * ordinary code on NeOS's own stack - so exiting is something the app does and
 * not something done to it, and nothing under here takes longer than a frame.
 *
 * The one thing worth knowing about this loop is that it never blocks on the
 * radio. A scan is two or three seconds and the call that starts it returns at
 * once, so the wait is a flag polled at the top rather than a call sat inside;
 * that is what lets the span buttons stay live while a scan is in the air.
 */

#include "wifiscan.h"
#include "neos_orient.h"

static ws_model_t s_model;

/* ------------------------------------------------------------------ */
/* Which way up                                                        */
/* ------------------------------------------------------------------ */

/*
 * Landscape, but not one particular landscape.
 *
 * A spectrum is a wide picture - 94 MHz of band, and a name for every network
 * standing up out of it - and the tablet is 720 across the other way up, which
 * is half the frequency axis and four characters of every SSID. So portrait is
 * refused. But turning a tablet end for end is something people do without
 * thinking about it, and an app that came up upside down and stayed there is
 * an app somebody has to put down and pick up again. So the two landscape
 * rotations are followed and the two portrait ones are not.
 *
 * The reading is milli-g integers rather than neos_orient_read()'s floats
 * because a float on its way into a comparison here is one promotion away from
 * a link failure - see the note in main/CMakeLists.txt. neos_orient.c's own
 * classify() is the source of the sign convention: with gravity mostly along
 * x, +x is NGL_ROT_270 and -x is NGL_ROT_90.
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
        since  = ws_now();
        return;
    }
    if (ws_now() - since < HOLD_MS) {
        return;
    }

    armed = false;
    neos_orient_lock(want);
    ws_ui_repaint();
}

/* ------------------------------------------------------------------ */

/*
 * How often round.
 *
 * The picture only changes when a scan lands, so this is not a frame rate - it
 * is how long a tap on a span button can sit unanswered, and how long the app
 * can take to notice it has been asked to close. Thirty milliseconds is under
 * both thresholds and leaves the core alone the rest of the time; a tick that
 * finds nothing to do draws nothing at all.
 */
#define TICK_MS 30

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

    ws_model_init(&s_model);
    ws_ui_init();

    while (!neos_app_close_requested()) {
        follow_gravity();
        ws_model_pump(&s_model);
        ws_ui_tick(&s_model);
        neos_sleep_ms(TICK_MS);
    }

    neos_orient_unlock();
    return 0;
}
