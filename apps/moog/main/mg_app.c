/*
 * The Minimoog on the Tab5: the loop, the orientation and the fingers.
 *
 *   the keyboard    an octave and a half, and more than one finger at a time.
 *                   The lowest key held is the one that sounds and the
 *                   contours are not restarted while any key is down, which is
 *                   the Model D's low-note priority and single trigger - play
 *                   legato and the pitch slides under a note already sounding
 *   a knob          drag it up and down from anywhere. It picks up from where
 *                   it was rather than jumping to the finger, so a 60-pixel
 *                   knob still has 300 pixels of travel
 *   a selector      drag to step through its positions, or tap to advance one
 *   a switch        tap the half you want
 *   < and >         the two pages
 *   the header      tap it to come off the 60 Hz pacing, which is the only way
 *                   to see what the frame can actually do
 *   the cross       the way out. This app takes the whole panel, so there is
 *                   no system bar and no close button but that one
 *
 * Nothing in this file is the instrument and nothing in it draws. The voice is
 * mg_dsp.c on mg_audio.c's thread and the panel is mg_ui.c; see moog.h for how
 * the three fit together.
 */
#include <stdio.h>

#include "moog.h"
#include "mg_ui.h"

#include "neos_api.h"
#include "neos_orient.h"
#include "neos_sys.h"

/* The instruments, and the overload lamp that rides with them. Any faster and
   the numbers cannot be read. */
#define HUD_MS 250

/* Past this a press is a drag and not a tap, which is what keeps a slightly
   smeared tap on a selector from stepping it twice. */
#define TAP_SLOP 8

/* How much of a turn is worth a repaint. A cell is twenty thousand pixels and
   a finger resting still on the glass still reports a pixel of jitter. */
#define CTL_EPS 0.0015f

static mg_app_t a;

static ngl_rotation_t s_rot = NGL_ROT_90;
static int16_t        s_ax, s_ay, s_az;
static uint32_t       s_orient_at;

static inline uint32_t now_ms(void) { return (uint32_t)neos_uptime_ms(); }
static inline uint32_t now_us(void) { return (uint32_t)neos_uptime_us(); }
static inline int16_t  iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

/* ------------------------------------------------------------------ */
/* Which way up                                                        */
/* ------------------------------------------------------------------ */

/*
 * Horizontal, and it may be turned over - the same policy apps/synth1 has and
 * for the same reason: a control surface that reshuffled itself mid-note would
 * be unplayable, so the display is locked rather than left to the OS watcher.
 *
 * Only the sensor's x axis has a say and only past half a g. Held portrait or
 * lying on a table there is no landscape answer to give, so this keeps the one
 * it had - which is what a synthesiser on a desk wants.
 *
 * The lock is also what keeps the touch panel honest. neos_touch_points()
 * reports in screen coordinates with ngl's rotation already applied, so
 * locking ngl to the rotation the canvas is drawn for is what makes a key land
 * where it looks like it landed.
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
    printf("[moog] turned over, now %s\n", neos_orient_name(want));
    pin(want);
    return true;
}

/* ------------------------------------------------------------------ */
/* The keyboard                                                        */
/* ------------------------------------------------------------------ */

/*
 * Low-note priority, and the gate is simply whether anything is down.
 *
 * The Model D takes the lowest key held rather than the last one pressed, and
 * it is worth keeping rather than quietly modernising: it is what lets a left
 * hand hold a bass note while the right hand plays over the top without the
 * bass being interrupted, and it is half of why Minimoog lines sound the way
 * they do. The other half is the single trigger, which is mg_dsp.c's - the
 * contours restart only when the gate rises, so a new key pressed while
 * another is held slides instead of striking.
 *
 * The pitch is left where it was when the last key comes up, so the release
 * tail decays at the note it was playing rather than jumping somewhere.
 */
static void update_note(void)
{
    int lowest = -1;
    for (int k = 0; k < MG_KEYS; k++) {
        if (a.held & (1u << k)) {
            lowest = k;
            break;
        }
    }
    if (lowest >= 0) {
        a.note = (float)(MG_KEY_C + lowest);
        a.gate = true;
    } else {
        a.gate = false;
    }
    mg_audio_perf(a.note, a.gate);
}

/* ------------------------------------------------------------------ */
/* The fingers                                                         */
/* ------------------------------------------------------------------ */

static void send(void)
{
    mg_ui_patch(&a);
    mg_audio_patch(&a.patch);
}

static void press(int16_t x, int16_t y)
{
    a.grab       = mg_ui_hit(&a, x, y);
    a.grab_x     = x;
    a.grab_y     = y;
    a.grab_moved = false;

    if (a.grab >= 0 && a.grab < MC_COUNT) {
        a.grab_v = a.v[a.grab];
    }
    neos_idle_poke();
}

static void drag(int16_t x, int16_t y)
{
    if (a.grab < 0 || a.grab >= MC_COUNT) {
        return;
    }
    if (!a.grab_moved &&
        (iabs16((int16_t)(x - a.grab_x)) > TAP_SLOP ||
         iabs16((int16_t)(y - a.grab_y)) > TAP_SLOP)) {
        a.grab_moved = true;
    }
    if (!a.grab_moved || mg_defs[a.grab].kind == CK_SWITCH) {
        return;                       /* a switch is tapped, never dragged */
    }

    const float was = a.v[a.grab];
    const float now = mg_ui_drag(a.grab, a.grab_v, a.grab_y, y);

    if (now > was + CTL_EPS || now < was - CTL_EPS) {
        a.v[a.grab] = now;
        mg_mark(&a, a.grab);
        send();
    }
}

/* A tap is a press and a release that did not travel. Everything that is a
   button rather than a knob happens here. */
static void tap(bool *quit)
{
    switch (a.grab) {
    case HIT_CLOSE:
        *quit = true;
        return;

    case HIT_PREV:
    case HIT_NEXT:
        /* Two pages, so either arrow is the other one. Both are drawn and both
           work, because a disabled arrow at one end of a two-page book is a
           control that is wrong half the time. */
        a.page = (uint8_t)(a.page ^ 1u);
        a.repaint_all = true;
        return;

    case HIT_METERS:
        a.capped = !a.capped;
        a.head_dirty = true;
        printf("[moog] %s\n", a.capped ? "paced to 60 Hz" : "uncapped");
        return;

    default:
        break;
    }
    if (a.grab < 0 || a.grab >= MC_COUNT) {
        return;
    }

    const mg_def_t *d = &mg_defs[a.grab];

    if (d->kind == CK_SWITCH) {
        /*
         * Which half was touched, rather than a toggle: a switch drawn with
         * both its positions labelled should go to the one that was pressed,
         * or the legends are decoration.
         */
        const ngl_rect_t r = mg_ui_rect(a.grab);
        const bool want = (a.grab_y < r.y + r.h / 2);
        if (want != (a.v[a.grab] > 0.5f)) {
            a.v[a.grab] = want ? 1.0f : 0.0f;
            mg_mark(&a, a.grab);
            send();
        }
    } else if (d->kind == CK_SELECT && d->steps > 1) {
        a.v[a.grab] = (float)(((int)a.v[a.grab] + 1) % d->steps);
        mg_mark(&a, a.grab);
        send();
    }
}

/*
 * One frame of touch, for a panel that has to accept a chord and a knob at the
 * same time.
 *
 * NeOS reports every finger on the glass but gives them no identity - the
 * order is the controller's and a point does not stay at the same index
 * between calls - so anything that needs to follow one finger has to match it
 * by position itself. That is done here rather than anywhere else:
 *
 *   every point inside the keyboard is a key, and which finger is on which key
 *   does not matter, because what comes out is a set of notes and not a
 *   gesture;
 *
 *   of the points outside it, one is the control being turned. With a grab
 *   already running it is whichever is nearest to where that finger was last
 *   frame, which is right as long as two fingers are not on two knobs - and
 *   turning two knobs at once is not something this panel offers.
 */
static void route_touch(bool *quit)
{
    neos_touch_t pts[NEOS_TOUCH_MAX];
    int n = neos_touch_points(pts, NEOS_TOUCH_MAX);
    if (n > NEOS_TOUCH_MAX) {
        n = NEOS_TOUCH_MAX;
    }

    const ngl_rect_t keys = mg_ui_keys_rect();

    uint32_t held = 0;
    int   ctl = -1;
    int32_t best = 0x7fffffff;

    for (int i = 0; i < n; i++) {
        const int k = mg_key_at(keys, pts[i].x, pts[i].y);
        if (k >= 0) {
            held |= 1u << k;
            continue;
        }
        if (a.grab != HIT_NONE) {
            const int32_t dx = pts[i].x - a.last_x;
            const int32_t dy = pts[i].y - a.last_y;
            const int32_t d  = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                ctl  = i;
            }
        } else if (ctl < 0) {
            ctl = i;
        }
    }

    /* --- the keys --- */
    if (held != a.held) {
        a.keys_changed |= held ^ a.held;
        a.held = held;
        update_note();
    }

    /* --- the one control --- */
    if (ctl >= 0) {
        if (a.grab == HIT_NONE) {
            press(pts[ctl].x, pts[ctl].y);
        } else {
            drag(pts[ctl].x, pts[ctl].y);
        }
        a.last_x = pts[ctl].x;
        a.last_y = pts[ctl].y;
    } else if (a.grab != HIT_NONE) {
        if (!a.grab_moved) {
            tap(quit);
        }
        a.grab = HIT_NONE;
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("[moog] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[moog] no screen, nothing to draw\n");
        return 0;
    }

    orient_init();

    /*
     * The whole panel. Asked for before the canvas is laid out, because this
     * is what decides whether the top 56 px belongs to the system bar or to
     * the app - and an app that draws straight to the framebuffer, as this one
     * does, would otherwise be fighting the bar's own flush for those rows.
     */
    if (!neos_fullscreen(true)) {
        printf("[moog] no game mode - the system bar will overlap the header\n");
    }

    if (!turn_init(&a.gfx, s_rot)) {
        printf("[moog] no memory for a canvas\n");
        neos_fullscreen(false);
        neos_orient_unlock();
        return 0;
    }

    mg_ui_defaults(&a);
    mg_ui_layout(&a);
    mg_ui_patch(&a);

    a.grab   = HIT_NONE;
    a.capped = true;
    a.page   = 0;
    a.note   = (float)(MG_KEY_C + 12);
    a.audio  = mg_audio_start();
    mg_audio_patch(&a.patch);
    mg_audio_perf(a.note, false);

    printf("[moog] canvas %dx%d in %s, %s\n",
           (int)a.gfx.pw, (int)a.gfx.ph, a.gfx.fast ? "internal RAM" : "PSRAM",
           neos_orient_name(s_rot));

    a.repaint_all = true;
    (void)mg_ui_paint(&a);

    bool     quit = false;
    uint32_t hud_at = now_ms(), fps_at = now_ms();
    uint32_t frames = 0;

    /*
     * neos_app_close_requested() is still polled, and in game mode it is not
     * the bar's button - it is NeOS's four-finger escape, which is the one way
     * out this app does not own. An app that took the panel and then ignored it
     * would be exactly the app that mode should not exist for.
     *
     * Four fingers is also a chord, so playing one will occasionally ask this
     * app to close. That is NeOS's decision and not this app's to argue with:
     * the touch task sees the gesture before the app does, and an app that
     * could swallow it would be an app with no way out at all.
     */
    while (!neos_app_close_requested() && !quit) {
        const uint32_t t0 = now_us();

        /*
         * A system panel is over the app. Every draw would be dropped where it
         * touched a pixel, so the work is skipped rather than done and thrown
         * away. What is on the glass over this app is not in ngl's back buffer,
         * so coming back is a full repaint and not optional; see turn.h.
         */
        if (neos_ui_busy()) {
            a.repaint_all = true;
            neos_sleep_ms(30);
            continue;
        }

        if (orient_poll()) {
            turn_rotate(&a.gfx, s_rot);
            mg_ui_layout(&a);
            a.repaint_all = true;
        }

        route_touch(&quit);

        const uint32_t now = now_ms();
        if (now - hud_at >= HUD_MS) {
            hud_at = now;
            a.head_dirty = true;
        }

        a.paint_us = mg_ui_paint(&a);

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

            mg_meters_t m;
            mg_audio_meters(&m);
            printf("[moog] %u fps  frame %u us  paint %u us | "
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
         * else run.
         */
        const uint32_t spent = now_us() - t0;
        if (a.capped && spent < 15000u) {
            neos_sleep_ms((15000u - spent) / 1000u + 1u);
        } else {
            neos_sleep_ms(1);
        }
    }

    printf("[moog] closing\n");
    mg_audio_stop();
    turn_free(&a.gfx);
    neos_fullscreen(false);
    neos_orient_unlock();
    return 0;
}
