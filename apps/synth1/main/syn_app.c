/*
 * Synth1 on the Tab5: the loop, the orientation and the finger.
 *
 *   drag a knob        up and down, anywhere - the knob picks up from where it
 *                      was rather than jumping to the finger, so a seventy
 *                      pixel control still has three hundred pixels of travel
 *   SHAPE              drag to step through the four LFO waveforms, or tap to
 *                      advance one
 *   RANGE              tap HI or LO. LO is vibrato, HI takes the LFO into the
 *                      audio band and the modulation becomes FM
 *   the instruments    tap the two lines at the bottom to come off the 60 Hz
 *                      pacing. Down there because no thumb rests on it, and
 *                      because what the frame can actually do is not something
 *                      a capped loop can tell you
 *   the cross          the way out. This app takes the whole panel, so there
 *                      is no system bar and no close button but that one
 *
 * Nothing in this file is the synthesiser and nothing in it draws. It brings
 * the panel up, decides which way round the tablet is, reads the glass and
 * paces the loop; the voice is syn_dsp.c on syn_audio.c's thread and the panel
 * is syn_ui.c. See synth1.h for how the three fit together.
 */
#include <stdio.h>

#include "synth1.h"
#include "syn_ui.h"

#include "neos_api.h"
#include "neos_orient.h"
#include "neos_sys.h"

/* How often the trace is redrawn. Thirty a second is smooth to watch and
   leaves two thirds of the frame for everything else; the scope is the one
   widget that changes every frame and so the one worth rationing. */
#define SCOPE_MS 33

/* The instruments. Any faster and the numbers cannot be read. */
#define HUD_MS 250

/* Past this a press is a drag and not a tap, which is what keeps a slightly
   smeared tap on the SHAPE knob from stepping the waveform twice. */
#define TAP_SLOP 8

/* How much of a turn is worth a repaint. A knob cell is fifty thousand pixels
   and a finger resting still on the glass still reports a pixel of jitter. */
#define KNOB_EPS 0.0015f

static syn_app_t a;

static ngl_rotation_t s_rot = NGL_ROT_90;
static int16_t        s_ax, s_ay, s_az;
static uint32_t       s_orient_at;

static inline uint32_t now_ms(void) { return (uint32_t)neos_uptime_ms(); }
static inline uint32_t now_us(void) { return (uint32_t)neos_uptime_us(); }

static inline int16_t iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

/* ------------------------------------------------------------------ */
/* Which way up                                                        */
/* ------------------------------------------------------------------ */

/*
 * Horizontal, and it may be turned over. That is the whole of the orientation
 * policy and it is the same one lab/defender has, for the same reason: a
 * control surface that reshuffled itself mid-note would be unplayable, so the
 * display is locked rather than left to the OS watcher.
 *
 * Only the sensor's x axis has a say and only when it is unambiguous. Held
 * portrait or laid flat on a desk there is no landscape answer to give, so
 * this keeps the one it had - which is exactly what a synthesiser lying on a
 * table wants. Half a g is about thirty degrees: past it the tablet is
 * definitely one way up.
 *
 * The lock is also what keeps the touch panel honest. neos_touch() reports in
 * screen coordinates with ngl's rotation already applied, so locking ngl to
 * the rotation the canvas is being drawn for is what makes a finger land where
 * it looks like it landed - the one place these two otherwise independent
 * pictures of "which way up" have to agree.
 */
#define FLIP_MG 500

static void pin(ngl_rotation_t r)
{
    s_rot = r;
    neos_orient_lock(r);
}

static void orient_init(void)
{
    if (neos_imu_accel_mg(&s_ax, &s_ay, &s_az) && (s_ax > 350 || s_ax < -350)) {
        s_rot = (s_ax > 0) ? NGL_ROT_270 : NGL_ROT_90;
    }
    pin(s_rot);
}

static bool orient_poll(void)
{
    const uint32_t t = now_ms();
    if (t - s_orient_at < 400u) {
        return false;
    }
    s_orient_at = t;

    if (!neos_imu_accel_mg(&s_ax, &s_ay, &s_az)) {
        return false;
    }
    if (s_ax > -FLIP_MG && s_ax < FLIP_MG) {
        return false;
    }
    const ngl_rotation_t want = (s_ax > 0) ? NGL_ROT_270 : NGL_ROT_90;
    if (want == s_rot) {
        return false;
    }
    printf("[synth1] turned over, now %s\n", neos_orient_name(want));
    pin(want);
    return true;
}

/* ------------------------------------------------------------------ */
/* The finger                                                          */
/* ------------------------------------------------------------------ */

static void mark(int ctl)
{
    a.ctl_dirty |= (1u << ctl);
}

static void send(void)
{
    syn_ui_patch(&a);
    syn_audio_set(&a.patch);
}

static void press(int16_t x, int16_t y)
{
    a.grab       = syn_ui_hit(x, y);
    a.grab_x     = x;
    a.grab_y     = y;
    a.grab_moved = false;

    if (a.grab >= 0 && a.grab < C_COUNT) {
        a.grab_norm = (a.grab == C_SHAPE)
                      ? (float)a.shape / (float)(SYN_SHAPES - 1)
                      : a.norm[a.grab];
    }
    neos_idle_poke();
}

static void drag(int16_t x, int16_t y)
{
    if (a.grab < 0 || a.grab >= C_COUNT) {
        return;
    }
    if (!a.grab_moved &&
        (iabs16((int16_t)(x - a.grab_x)) > TAP_SLOP ||
         iabs16((int16_t)(y - a.grab_y)) > TAP_SLOP)) {
        a.grab_moved = true;
    }
    if (!a.grab_moved) {
        return;
    }

    const float n = syn_ui_drag(a.grab_norm, a.grab_y, y);

    if (a.grab == C_SHAPE) {
        const int s = (int)(n * (float)(SYN_SHAPES - 1) + 0.5f);
        if (s != a.shape) {
            a.shape = s;
            mark(C_SHAPE);
            send();
        }
        return;
    }
    if (a.grab == C_HILO) {
        return;                       /* a switch, and switches are tapped */
    }

    const float was = a.norm[a.grab];
    if (n > was + KNOB_EPS || n < was - KNOB_EPS) {
        a.norm[a.grab] = n;
        mark(a.grab);
        send();
    }
}

/* A tap is a press and a release that did not travel. Everything that is a
   button rather than a knob happens here. */
static bool tap(int16_t y, bool *quit)
{
    switch (a.grab) {
    case HIT_CLOSE:
        *quit = true;
        return true;

    case HIT_FOOTER:
        a.capped = !a.capped;
        a.hud_dirty = true;
        printf("[synth1] %s\n", a.capped ? "paced to 60 Hz" : "uncapped");
        return true;

    case C_SHAPE:
        a.shape = (a.shape + 1) % SYN_SHAPES;
        mark(C_SHAPE);
        send();
        return true;

    case C_HILO: {
        /* Which half of the switch was touched, rather than a toggle: a switch
           that is drawn with its two positions labelled should go to the one
           that was pressed, or the legend is decoration. */
        const bool want = (y < syn_ui_knob_cy());
        if (want != a.hi) {
            a.hi = want;
            mark(C_HILO);
            mark(C_RATE);        /* the same knob position is a different rate */
            send();
        }
        return true;
    }
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("[synth1] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[synth1] no screen, nothing to draw\n");
        return 0;
    }

    orient_init();

    /*
     * The whole panel. Asked for before the canvas is laid out, because this
     * is what decides whether the top 56 px belongs to the system bar or to
     * the app - and an app that draws straight to the framebuffer, as this one
     * does, would otherwise be fighting the bar's own flush for those rows.
     *
     * What it takes on with it is the way out: the bar's close button is the
     * one control that is in the same place in every app and this removes it,
     * so syn_ui.c draws its own. NeOS keeps the four-finger escape either way.
     */
    if (!neos_fullscreen(true)) {
        printf("[synth1] no game mode - the system bar will overlap the header\n");
    }

    if (!turn_init(&a.gfx, s_rot)) {
        printf("[synth1] no memory for a %d x %d canvas\n",
               (int)a.gfx.pw, (int)a.gfx.ph);
        neos_fullscreen(false);
        neos_orient_unlock();
        return 0;
    }

    syn_ui_defaults(&a);
    syn_ui_layout(&a);
    syn_ui_patch(&a);

    a.grab   = HIT_NONE;
    a.capped = true;
    a.audio  = syn_audio_start();
    syn_audio_set(&a.patch);

    printf("[synth1] canvas %dx%d in %s, panel %dx%d, %s\n",
           (int)a.gfx.pw, (int)a.gfx.ph, a.gfx.fast ? "internal RAM" : "PSRAM",
           (int)a.gfx.pw, (int)a.gfx.ph, neos_orient_name(s_rot));

    a.repaint_all = true;
    (void)syn_ui_paint(&a);

    bool     quit = false;
    bool     was_down = false;
    uint32_t scope_at = now_ms(), hud_at = now_ms(), fps_at = now_ms();
    uint32_t frames = 0;

    /*
     * neos_app_close_requested() is still polled, and in game mode it is not
     * the bar's button - it is NeOS's four-finger escape, which is the one way
     * out this app does not own. An app that took the panel and then ignored
     * it would be exactly the app that mode should not exist for.
     */
    while (!neos_app_close_requested() && !quit) {
        const uint32_t t0 = now_us();

        /*
         * A system panel is over the app - the keyboard, the Wi-Fi list. Every
         * draw would be dropped where it touched a pixel, so the work is
         * skipped rather than done and thrown away. What is on the glass over
         * this app's rectangle is not in ngl's back buffer, so coming back is
         * a full repaint and not optional; see turn.h.
         */
        if (neos_ui_busy()) {
            a.repaint_all = true;
            neos_sleep_ms(30);
            continue;
        }

        if (orient_poll()) {
            turn_rotate(&a.gfx, s_rot);
            syn_ui_layout(&a);
            a.repaint_all = true;
        }

        neos_touch_t t = { 0, 0, false };
        (void)neos_touch(&t);       /* false only on a dead panel, and `t` is
                                       then the zero above */

        if (t.down && !was_down) {
            press(t.x, t.y);
        } else if (t.down && was_down) {
            drag(t.x, t.y);
        } else if (!t.down && was_down) {
            if (!a.grab_moved) {
                (void)tap(a.grab_y, &quit);
            }
            a.grab = HIT_NONE;
        }
        was_down = t.down;

        const uint32_t now = now_ms();
        if (now - scope_at >= SCOPE_MS) {
            scope_at = now;
            a.scope_dirty = true;
        }
        if (now - hud_at >= HUD_MS) {
            hud_at = now;
            a.hud_dirty = true;
        }

        a.paint_us = syn_ui_paint(&a);

        /*
         * Everything the frame is judged on has now happened. The frame counter
         * is outside it, because a frame time that includes the cost of
         * displaying the frame time is not a measurement of anything.
         */
        a.work_us = now_us() - t0;

        frames++;
        if (now - fps_at >= 1000u) {
            a.fps  = frames * 1000u / (now - fps_at);
            frames = 0;
            fps_at = now;

            syn_meters_t m;
            syn_audio_meters(&m);
            printf("[synth1] %u fps  frame %u us  paint %u us | "
                   "audio %u.%u%%  blk %u us (max %u)  lead %u us  under %u\n",
                   (unsigned)a.fps, (unsigned)a.work_us, (unsigned)a.paint_us,
                   (unsigned)(m.load_pm / 10), (unsigned)(m.load_pm % 10),
                   (unsigned)m.gen_us, (unsigned)m.gen_us_max,
                   (unsigned)m.lead_us, (unsigned)m.underruns);
        }

        /*
         * Paced to 60 Hz, which is as fast as the panel is: frames beyond it
         * are work nobody sees, and here they would be work taken off the one
         * task that has somewhere to be. Uncapped it still yields a tick,
         * because an app on this machine that never sleeps never lets anything
         * else run - and the rate that then appears is what the frame can
         * actually do, which a capped loop cannot tell you.
         */
        const uint32_t spent = now_us() - t0;
        if (a.capped && spent < 15000u) {
            neos_sleep_ms((15000u - spent) / 1000u + 1u);
        } else {
            neos_sleep_ms(1);
        }
    }

    printf("[synth1] closing\n");
    syn_audio_stop();
    turn_free(&a.gfx);
    neos_fullscreen(false);
    neos_orient_unlock();
    return 0;
}
