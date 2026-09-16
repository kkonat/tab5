/*
 * radio - the whole app, which is a loop.
 *
 * Advance the stream, feed the speaker, draw whatever changed, and check
 * whether anyone has asked us to go. There is no scheduler entry for an app
 * on this machine - it runs as ordinary code on NeOS's own stack - so exiting
 * is something the app does and not something done to it, and everything
 * under here is written so that the check at the top of the loop is never
 * more than a few milliseconds away.
 *
 * The order of the three calls in the body is the one thing here that is not
 * arbitrary. The stream is polled first because it is the only stage that can
 * be starved by being late - a socket left unread while the codec is being
 * fed backs up in lwIP, and the window is a few hundred milliseconds wide.
 * The audio goes next, because it is what decides how much time the frame is
 * allowed to take. The screen goes last, out of whatever is left.
 */

#include "radio.h"
#include "rad_ui.h"

#include "neos_orient.h"

#include <string.h>

static rad_app_t s_app;

/* Holding a - or + repeats it: the first repeat only after REPEAT_DELAY, so a
   tap stays exactly one step, then one every REPEAT_INTERVAL. Frequencies
   step multiplicatively, so a held finger sweeps about an octave and a half a
   second. */
#define REPEAT_DELAY     250
#define REPEAT_INTERVAL  100
/* Most steps to take in one frame while catching up. Four is already a
   visible jump; more than that and a hitch would fling the value across its
   range. */
#define REPEAT_CATCHUP     4

#define SAVE_INTERVAL   1000
#define VOLUME_STEP        5

/* ------------------------------------------------------------------ */
/* Which way up                                                        */
/* ------------------------------------------------------------------ */

/*
 * Landscape, but not one particular landscape - lanscan's reasoning, and the
 * same answer. A station grid is three columns and a tape deck is wider than
 * it is tall, so portrait is refused; but turning a tablet end for end is
 * something people do without thinking about it, and an app that came up
 * upside down and stayed there is an app somebody has to put down and pick up
 * again.
 *
 * The reading is milli-g integers rather than neos_orient_read()'s floats,
 * because this app is compiled -Werror=double-promotion and a float on its
 * way into a comparison is one printf away from being a link failure.
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
        armed = false;          /* flat, or on its end: no opinion */
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
        since  = rad_now();
        return;
    }
    if (rad_now() - since < HOLD_MS) {
        return;
    }

    armed = false;
    neos_orient_lock(want);
    rad_ui_repaint(&s_app);
}

/* ------------------------------------------------------------------ */
/* Keys                                                                */
/* ------------------------------------------------------------------ */

/* "step:7+" -> 7, with @p tail left pointing at the '+'. -1 if the key is
   not this one. */
static int key_number(const char *key, const char *prefix, const char **tail)
{
    if (!key || !rad_starts(key, prefix)) {
        return -1;
    }
    const char *p = key + strlen(prefix);
    if (*p < '0' || *p > '9') {
        return -1;
    }
    int value = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        value = value * 10 + (*p - '0');
    }
    if (tail) {
        *tail = p;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void set_volume(rad_app_t *app, int value)
{
    if (value < 0)   { value = 0; }
    if (value > 100) { value = 100; }
    if (value == app->volume) {
        return;
    }
    app->volume = value;
    rad_audio_volume(&app->audio, value);
    app->dirty = true;
}

static void play_index(rad_app_t *app, int index)
{
    if (index < 0 || index >= app->stations) {
        return;
    }
    app->selected = index;
    rad_audio_reset(&app->audio);
    rad_stream_play(&app->stream, &app->station[index], index);
}

static void step_param(rad_app_t *app, const char *key)
{
    const char *tail = NULL;
    const int   p    = key_number(key, "step:", &tail);
    if (p < 0 || p >= P_COUNT || !tail) {
        return;
    }
    app->params[p] = rad_step_param((rad_param_t)p, app->params[p], *tail == '+');
    rad_eq_design(&app->audio.eq, app->params);
    app->dirty = true;
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

static void on_press(rad_app_t *app, int16_t x, int16_t y)
{
    const char *target = rad_ui_hit(app, x, y);
    rad_copy(app->pressed, sizeof(app->pressed), target ? target : "");

    if (!target) {
        return;
    }

    if (rad_starts(target, "slider:")) {
        rad_copy(app->slider, sizeof(app->slider), target);
        const int p = key_number(target, "slider:", NULL);
        if (p >= 0 && p < P_COUNT) {
            app->params[p] = rad_eqpage_slider_value((rad_param_t)p,
                                                     rad_ui_page(ngl_app_area()), x);
            rad_eq_design(&app->audio.eq, app->params);
            app->dirty = true;
        }
        return;
    }

    if (rad_starts(target, "step:")) {
        /* On press rather than release: a stepper should move the moment it
           is touched, and the repeat clock has to start from that first
           step. */
        step_param(app, target);
        rad_copy(app->repeat, sizeof(app->repeat), target);
        app->repeat_at = rad_now() + REPEAT_DELAY;
        return;
    }

    if (rad_streq(target, "volbar")) {
        /* The bar jumps to the finger on the way down, rather than waiting
           for a drag: a tap somewhere along a slider means that value. */
        rad_copy(app->slider, sizeof(app->slider), "volume");
        set_volume(app, rad_ui_volume_from_x(x));
        return;
    }
}

static void on_drag(rad_app_t *app, int16_t x, int16_t y)
{
    (void)y;
    if (!app->slider[0]) {
        return;
    }

    if (rad_streq(app->slider, "volume")) {
        set_volume(app, rad_ui_volume_from_x(x));
        return;
    }

    const int p = key_number(app->slider, "slider:", NULL);
    if (p < 0 || p >= P_COUNT) {
        return;
    }
    const int value = rad_eqpage_slider_value((rad_param_t)p,
                                              rad_ui_page(ngl_app_area()), x);
    if (value != app->params[p]) {
        app->params[p] = value;
        rad_eq_design(&app->audio.eq, app->params);
        app->dirty = true;
    }
}

static void on_release(rad_app_t *app)
{
    char target[24];
    rad_copy(target, sizeof(target), app->pressed);

    app->pressed[0] = 0;
    app->slider[0]  = 0;
    app->repeat[0]  = 0;

    if (!target[0]) {
        return;
    }

    int n = key_number(target, "tab:", NULL);
    if (n >= 0 && n < TAB_COUNT) {
        if (app->tab != (rad_tab_t)n) {
            app->tab   = (rad_tab_t)n;
            app->dirty = true;      /* come back to the page you left */
            rad_ui_repaint(app);
        }
        return;
    }

    if (rad_streq(target, "play")) {
        if (rad_stream_live(&app->stream)) {
            rad_stream_stop(&app->stream);
            rad_audio_reset(&app->audio);
        } else if (app->stations) {
            play_index(app, app->selected >= 0 ? app->selected : 0);
        }
        return;
    }

    if (rad_streq(target, "volup")) {
        set_volume(app, app->volume + VOLUME_STEP);
        return;
    }
    if (rad_streq(target, "voldown")) {
        set_volume(app, app->volume - VOLUME_STEP);
        return;
    }

    if (rad_streq(target, "pageprev")) {
        if (app->page > 0) {
            app->page--;
            rad_ui_repaint(app);
        }
        return;
    }
    if (rad_streq(target, "pagenext")) {
        const int per = rad_ui_per_page();
        const int pages = (app->stations + per - 1) / per;
        if (app->page < pages - 1) {
            app->page++;
            rad_ui_repaint(app);
        }
        return;
    }

    n = key_number(target, "star", NULL);
    if (n >= 0 && n < app->stations) {
        /* One station starts by itself, or none does - so setting a star
           clears the others rather than adding to a set. */
        const bool was = app->station[n].autoplay;
        for (int i = 0; i < app->stations; i++) {
            app->station[i].autoplay = false;
        }
        app->station[n].autoplay = !was;
        (void)rad_stations_save(app->station, app->stations);
        rad_ui_repaint(app);
        return;
    }

    n = key_number(target, "row", NULL);
    if (n >= 0 && n < app->stations) {
        play_index(app, n);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static void draw_waiting(const char *line)
{
    static bool cleared;

    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t a = ngl_app_area();

    if (!cleared) {
        cleared = true;
        ngl_clear(sc, TH_BG);
    }
    const int16_t y = (int16_t)(a.y + a.h / 2 - ngl_font_small.height);
    ngl_fill_rect(sc, ngl_rect(a.x, y, a.w, ngl_font_small.height), TH_BG);

    const int16_t w = ngl_text_width(&ngl_font_small, line);
    ngl_text(sc, (int16_t)(a.x + (a.w - w) / 2), y, line, &ngl_font_small,
             TH_TEXT_DIM);
    ngl_flush();
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rad_app_t *app = &s_app;
    memset(app, 0, sizeof(*app));
    app->selected = -1;

    ngl_rotation_t r = neos_orient_get();
    if (r != NGL_ROT_90 && r != NGL_ROT_270) {
        r = NGL_ROT_90;
    }
    neos_orient_lock(r);

    rad_settings_load(app);
    app->stations = rad_stations_load(app->station, RAD_STATIONS_MAX);

    if (!rad_stream_init(&app->stream)) {
        draw_waiting("not enough memory for the stream buffer");
        while (!neos_app_close_requested()) {
            neos_sleep_ms(100);
        }
        neos_orient_unlock();
        return 0;
    }

    /*
     * A tablet whose codec will not come up is a tablet this app cannot do
     * its job on, and saying so is better than a silent radio that looks like
     * it is playing. Everything else still works, so the app stays up and the
     * card is the message.
     */
    const bool have_audio = rad_audio_init(&app->audio);
    rad_eq_design(&app->audio.eq, app->params);
    rad_audio_volume(&app->audio, app->volume);

    rad_ui_init(app);

    /* The station with the star starts by itself. */
    for (int i = 0; i < app->stations; i++) {
        if (app->station[i].autoplay) {
            app->page = i / (rad_ui_per_page() > 0 ? rad_ui_per_page() : 9);
            play_index(app, i);
            break;
        }
    }
    if (!have_audio && app->stream.state != RAD_ERROR) {
        rad_stream_stop(&app->stream);
        app->stream.state = RAD_ERROR;
        rad_copy(app->stream.error, sizeof(app->stream.error),
                 "the audio codec did not come up on this tablet");
    }

    bool     was_down = false;
    app->saved_at = rad_now();

    while (!neos_app_close_requested()) {
        follow_gravity();

        rad_stream_poll(&app->stream);
        (void)rad_audio_pump(&app->audio, &app->stream);

        /*
         * A server that names itself is allowed to rename a station that was
         * only ever a URL in the file. Anything the user actually typed is
         * left alone - a name in stations.conf is a decision, and an ICY
         * header is a guess.
         */
        if (app->stream.icy_name[0] && app->stream.station >= 0 &&
            app->stream.station < app->stations) {
            rad_station_t *st = &app->station[app->stream.station];
            if (rad_starts(st->name, "http")) {
                rad_copy(st->name, sizeof(st->name), app->stream.icy_name);
            }
        }

        /* --- touch --- */
        neos_touch_t touch = { 0, 0, false };
        (void)neos_touch(&touch);       /* false only on a dead panel, and
                                           `touch` is then the zero above */

        if (touch.down && !was_down) {
            on_press(app, touch.x, touch.y);
        } else if (touch.down && was_down) {
            on_drag(app, touch.x, touch.y);
        } else if (!touch.down && was_down) {
            on_release(app);
        }
        was_down = touch.down;

        /* Auto-repeat while a stepper is held. The finger has to still be on
           the button it started on: sliding off cancels, which is the only
           way to back out of a repeat once it is running. */
        if (app->repeat[0]) {
            const char *still = rad_ui_hit(app, touch.x, touch.y);
            if (!touch.down || !rad_streq(still, app->repeat)) {
                app->repeat[0]  = 0;
                app->pressed[0] = 0;
            } else {
                /* Step as many times as the elapsed time has earned, rather
                   than once per frame: a frame here can cost more than
                   REPEAT_INTERVAL, which would quietly stretch a 100 ms
                   repeat into whatever the frame rate happens to be. */
                const uint32_t now = rad_now();
                int stepped = 0;
                while ((int32_t)(now - app->repeat_at) >= 0 &&
                       stepped < REPEAT_CATCHUP) {
                    step_param(app, app->repeat);
                    app->repeat_at += REPEAT_INTERVAL;
                    stepped++;
                }
                if ((int32_t)(app->repeat_at - now) < 0) {
                    /* Stalled for much longer than one interval; drop the
                       backlog rather than firing it off in a burst. */
                    app->repeat_at = now + REPEAT_INTERVAL;
                }
            }
        }

        /* --- draw --- */
        rad_ui_draw(app);

        /* Again, because the draw is the long part of a turn and the codec
           holds only 30 ms. Cheap when there is nothing due: the lead is
           read, it is high enough, and the call returns. */
        (void)rad_audio_pump(&app->audio, &app->stream);

        /* Settings are written at most once a second: a slider drag would
           otherwise rewrite the file on every frame, and this one is on a
           card that may be pulled. */
        if (app->dirty && rad_now() - app->saved_at > SAVE_INTERVAL) {
            rad_settings_save(app);
            app->dirty    = false;
            app->saved_at = rad_now();
        }

        /*
         * Yield. While a station is playing the pump has already spent this
         * frame's time inside the codec, so the sleep is a token one that
         * keeps the idle task fed; with nothing playing there is nothing to
         * be prompt about and the loop can rest properly.
         */
        neos_sleep_ms(app->stream.state == RAD_PLAYING ? 1 : 15);
    }

    rad_stream_stop(&app->stream);
    rad_stream_free(&app->stream);
    rad_audio_free(&app->audio);
    rad_settings_save(app);
    neos_orient_unlock();
    return 0;
}
