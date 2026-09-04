/*
 * Ну, погоди! - the wiring.
 *
 * There is very little program here, which is the point: the game is 1856
 * bytes of КБ1013ВК1-2 machine code on the card, and this file's job is to
 * give that code the four pins it expects and get out of the way.
 *
 * The loop is paced by the speaker.
 *
 * A block is 128 instructions, which is 256 clock ticks, which is 256 samples
 * of the piezo pin, which resamples to exactly 375 frames at 48 kHz, which is
 * 7.8125 ms. neos_audio_write() does not return until the codec has taken
 * them, so handing over a block of sound is the same act as letting 7.8 ms of
 * game time go by - there is no timer anywhere in this app and no way for the
 * emulated clock and the audible one to drift apart, because they are the
 * same clock. If the codec will not come up the loop falls back to sleeping,
 * which keeps the game playable on a tablet with no working speaker at the
 * cost of the timing being approximately rather than exactly right.
 */
#include <stdio.h>
#include <string.h>

#include "nupogodi.h"
#include "ngl_theme.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"

/*
 * How often the LCD is looked at, in blocks.
 *
 * Every fourth block is 32 Hz, which is far more than a segment LCD driven by
 * a watch crystal can do - the display changes a handful of times a second -
 * and it keeps the panel flush rate somewhere sensible. Checking every block
 * would cost 72 comparisons at 128 Hz, which is nothing, but would let a
 * single-block flicker turn into a flush.
 */
#define DRAW_EVERY 4

/* Without the speaker there is no clock, so one has to be invented. */
#define FALLBACK_MS 8

/*
 * How long after a reset the game's clock can be set, in blocks - so, in
 * 7.8 ms units. One second.
 *
 * A cold start clears RAM, and the ROM does it in its own time rather than in
 * the reset vector: seeding the clock immediately after sm5a_reset() writes
 * six nibbles that the program then wipes on its way up, and the symptom is a
 * clock stuck at 12:00 with everything else working perfectly. So the seed
 * waits until the machine is demonstrably past its own initialisation, which
 * it is within a few milliseconds and certainly within a second.
 */
#define CLOCK_SEED_BLOCKS 128u

static npg_asset_t   s_asset;
static ngl_rotation_t s_rot = NGL_ROT_90;

/* ------------------------------------------------------------------ */
/* The four pins                                                       */
/* ------------------------------------------------------------------ */

static uint8_t bus_k(uint8_t r_out)  { return npg_panel_k(r_out); }

/*
 * BA is a factory test pad, unpopulated on the board. Grounding it is the
 * infinite-lives cheat, and the ROM only looks at it out of reset - which is
 * why the LIVES switch does nothing until ACL is pressed after it.
 */
static uint8_t bus_ba(void) { return npg_panel_cheat() ? 0 : 1; }
static uint8_t bus_b(void)  { return 1; }

static const sm5a_bus_t s_bus = {
    .program = NULL,          /* filled in once the ROM is off the card */
    .read_k  = bus_k,
    .read_ba = bus_ba,
    .read_b  = bus_b,
    .write_r = npg_sound_pin,
};

static sm5a_bus_t s_bus_live;

/* ------------------------------------------------------------------ */
/* Orientation                                                         */
/* ------------------------------------------------------------------ */

static float absf(float v) { return v < 0.0f ? -v : v; }

/*
 * Landscape, and pinned there.
 *
 * The artwork is half again as wide as it is tall and the controls live down
 * both sides of it, so there is no portrait layout of this that is worth
 * having. Which way up is still the tablet's business - the accelerometer
 * decides once at startup and again if it is turned over.
 */
static void orient_pin(ngl_rotation_t r)
{
    s_rot = r;
    neos_orient_lock(r);
    ngl_bar_paint();
    ngl_flush();
}

static ngl_rotation_t orient_want(ngl_rotation_t fallback, float threshold)
{
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (neos_orient_read(&ax, &ay, &az) != 0 || absf(ax) < threshold) {
        return fallback;
    }
    return ax > 0.0f ? NGL_ROT_270 : NGL_ROT_90;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/*
 * Where the artwork goes, and what is left for the controls.
 *
 * The columns are reserved first and the artwork gets what remains, rather
 * than the other way round: a thumb needs a certain width whatever size the
 * picture happens to be, and an artwork that grew until the buttons were
 * slivers would be the wrong trade.
 */
static ngl_rect_t artwork_box(ngl_rect_t area)
{
    return ngl_rect(area.x, area.y, area.w, (int16_t)(area.h - NPG_STRIP_H));
}

/*
 * Centre the artwork in its box; it was already scaled to fit at load time.
 *
 * Horizontally centred and vertically top-aligned, because the box is the
 * whole width but only as tall as the strip left it - so the artwork is
 * height-limited, fills the box vertically, and what is spare is the margin
 * down each side that the thumb pads live in.
 */
static ngl_rect_t artwork_at(ngl_rect_t box, const npg_asset_t *a)
{
    /*
     * The visible size, not the asset's: what the romset's artwork carries
     * round the picture is a black border, and it is centred and laid out
     * around as if it were not there - which is what leaves the case room to
     * put a bezel where it used to be. See npg_asset_t::vis.
     */
    return ngl_rect((int16_t)(box.x + (box.w - a->vis.w) / 2),
                    (int16_t)(box.y + (box.h - a->vis.h) / 2),
                    a->vis.w, a->vis.h);
}

/* ------------------------------------------------------------------ */
/* The clock                                                           */
/* ------------------------------------------------------------------ */

/*
 * Seed the game's clock from the tablet's.
 *
 * These machines were watches that happened to play a game, and the ROM keeps
 * the time as six BCD nibbles in its own RAM which it advances off the same
 * divider that runs everything else. Setting it through the buttons is a
 * minute a press, so this writes the nibbles instead - which is reaching into
 * a running program's memory, and is justified by there being no other way
 * and by the tablet already knowing what time it is.
 *
 * It is a twelve-hour clock with a flag bit for the afternoon, which is what
 * lights ДП or ПП above the digits, and midnight and noon are both stored as
 * "12" with the flag doing the telling. Getting that encoding wrong shows up
 * as a clock that is right for eleven hours a day.
 *
 * Not to be called straight after a reset - see CLOCK_SEED_BLOCKS.
 */
static void set_clock(const npg_asset_t *a)
{
    if (!a->time_pm) {
        return;      /* nobody worked out where this game keeps its clock */
    }

    neos_rtc_t t;
    if (!neos_time_local(&t)) {
        return;
    }

    uint8_t hm, hl;
    const int h = t.hour;

    if (h == 0) {                    /* midnight is 12, morning */
        hm = 1;
        hl = 2;
    } else if (h < 12) {
        hm = (uint8_t)(h / 10);
        hl = (uint8_t)(h % 10);
    } else if (h == 12) {            /* noon is 12, afternoon */
        hm = (uint8_t)(a->time_pm + 1);
        hl = 2;
    } else {
        hm = (uint8_t)(a->time_pm + (h - 12) / 10);
        hl = (uint8_t)((h - 12) % 10);
    }

    sm5a_poke(a->time_addr[0], hm);
    sm5a_poke(a->time_addr[1], hl);
    sm5a_poke(a->time_addr[2], (uint8_t)(t.min / 10));
    sm5a_poke(a->time_addr[3], (uint8_t)(t.min % 10));
    sm5a_poke(a->time_addr[4], (uint8_t)(t.sec / 10));
    sm5a_poke(a->time_addr[5], (uint8_t)(t.sec % 10));

    printf("[npg] seed %02d:%02d:%02d -> nibbles %u %u %u %u %u %u, read back "
           "%u %u %u %u %u %u\n",
           t.hour, t.min, t.sec,
           hm, hl, t.min / 10, t.min % 10, t.sec / 10, t.sec % 10,
           sm5a_peek(a->time_addr[0]), sm5a_peek(a->time_addr[1]),
           sm5a_peek(a->time_addr[2]), sm5a_peek(a->time_addr[3]),
           sm5a_peek(a->time_addr[4]), sm5a_peek(a->time_addr[5]));
}

/* ------------------------------------------------------------------ */

static void repaint_all(void)
{
    /*
     * ngl_clear() first, because it is what puts the system bar back; then
     * the case over the app area, because the bar is not ours to paint copper.
     */
    ngl_surface_t *sc = ngl_screen();
    if (sc) {
        ngl_clear(sc, TH_BG);
    }
    npg_panel_ground();
    npg_panel_paint();
    npg_draw_invalidate();
    npg_draw_update();      /* which flushes */
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Landscape first: everything below is sized from the app area, and the
       app area is a different shape afterwards. */
    orient_pin(orient_want(NGL_ROT_90, 0.35f));

    ngl_rect_t area = ngl_app_area();
    ngl_rect_t box  = artwork_box(area);

    char path[128];
    snprintf(path, sizeof(path), "apps/%s/nupogodi.lcd", neos_app_self());

    if (!npg_asset_load(&s_asset, path)) {
        /*
         * The status line already says why. Nothing else is worth drawing:
         * without the ROM there is no game, and a screenful of instructions
         * about romsets is not what an app is for - tools/genlcd/README.md is.
         */
        ngl_surface_t *sc = ngl_screen();
        if (sc) {
            ngl_clear(sc, TH_BG);
            ngl_text(sc, (int16_t)(area.x + 40), (int16_t)(area.y + 40),
                     "no nupogodi.lcd on the card", &ngl_font_large, TH_BAD);
            ngl_text(sc, (int16_t)(area.x + 40),
                     (int16_t)(area.y + 40 + ngl_font_large.height + 16),
                     "see tools/genlcd", &ngl_font_small, TH_TEXT_DIM);
            ngl_flush();
        }
        while (!neos_app_close_requested()) {
            neos_sleep_ms(100);
        }
        neos_orient_unlock();
        return 0;
    }

    const ngl_rect_t art = artwork_at(box, &s_asset);

    npg_panel_style_load();
    npg_panel_layout(area, art);

    ngl_surface_t *sc = ngl_screen();
    if (sc) {
        ngl_clear(sc, TH_BG);
    }
    npg_panel_ground();
    npg_panel_paint();

    if (!npg_draw_begin(&s_asset, art)) {
        neos_status("no memory to draw the artwork");
        npg_asset_free(&s_asset);
        neos_orient_unlock();
        return 0;
    }

    if (s_asset.title[0]) {
        neos_status_for(s_asset.title, 2500);
    }

    /* The speaker is optional; the game is not. */
    const bool sound = npg_sound_begin();

    s_bus_live = s_bus;
    s_bus_live.program = s_asset.rom;
    sm5a_reset(&s_bus_live);

    bool     acl_was = false;
    bool     was_busy = false;
    uint32_t block = 0;

    /* The block at which to put the clock right; 0 once it has been done. */
    uint32_t clock_at = CLOCK_SEED_BLOCKS;

    uint16_t pc_lo = 0xffff, pc_hi = 0;

    /*
     * Is the emulated second a second?
     *
     * The whole pacing argument above rests on neos_audio_write() blocking for
     * exactly as long as the sound it took, and if that is wrong the game does
     * not crash - it runs fast or slow, which is the kind of bug that gets
     * argued about from memory rather than measured. So it is measured, once,
     * over the first ten emulated seconds, and it only says anything if the
     * answer is wrong by more than a couple of percent.
     */
    const uint32_t t0 = neos_uptime_ms();
    bool           checked = false;

    while (!neos_app_close_requested()) {
        /*
         * A system panel is over us: our draws are dropped and our taps
         * withheld, so there is nothing to do but stay out of the way. The
         * emulator is stopped rather than run blind - a game that carried on
         * for the length of a Wi-Fi password would be a game lost to the
         * keyboard.
         */
        if (neos_ui_busy()) {
            was_busy = true;
            neos_sleep_ms(50);
            continue;
        }
        if (was_busy) {
            was_busy = false;
            repaint_all();
            /*
             * The emulator was stopped for as long as the panel was up, so the
             * game's clock is now exactly that far behind. It is a watch: put
             * it right rather than leaving it slow by however long somebody
             * spent typing a Wi-Fi password. No delay needed here - the
             * machine has been up for a while and is not about to clear RAM.
             */
            set_clock(&s_asset);
        }

        /* Turned over. Both landscape rotations share a back buffer, but the
           bar and everything drawn into the app area have to come back. */
        const ngl_rotation_t want = orient_want(s_rot, 0.5f);
        if (want != s_rot) {
            orient_pin(want);
            area = ngl_app_area();
            box  = artwork_box(area);
            npg_panel_layout(area, artwork_at(box, &s_asset));
            repaint_all();
        }

        bool restyled = false;
        sm5a_keys_active(npg_panel_poll(&restyled));
        if (restyled) {
            repaint_all();
        }

        /* ACL is a pad, not a key: the ROM never sees it, the part just
           starts again. On the press edge only, or holding it would be a
           machine that never got past its reset vector. */
        const bool acl = npg_panel_acl();
        if (acl && !acl_was) {
            sm5a_reset(&s_bus_live);
            clock_at = block + CLOCK_SEED_BLOCKS;   /* once it has cleared RAM */
            npg_draw_invalidate();
        }
        acl_was = acl;

        sm5a_run(NPG_BLOCK_INSTR);
        if (sm5a_pc() < pc_lo) pc_lo = sm5a_pc();
        if (sm5a_pc() > pc_hi) pc_hi = sm5a_pc();

        if (!npg_sound_flush()) {
            neos_sleep_ms(FALLBACK_MS);
        }

        block++;

        if (clock_at && block >= clock_at) {
            clock_at = 0;
            set_clock(&s_asset);
        }

        if (block % 384u == 0) {
            unsigned sum = 0;
            for (int i = 0; i < 0x50; i++) {
                sum = sum * 31u + sm5a_peek((uint8_t)i);
            }
            printf("[npg] t+%us halt=%d ram %u %u %u %u %u %u sum=%08x pc %03X..%03X\n",
                   (unsigned)(block / 128u), sm5a_halted(),
                   sm5a_peek(s_asset.time_addr[0]), sm5a_peek(s_asset.time_addr[1]),
                   sm5a_peek(s_asset.time_addr[2]), sm5a_peek(s_asset.time_addr[3]),
                   sm5a_peek(s_asset.time_addr[4]), sm5a_peek(s_asset.time_addr[5]),
                   sum, pc_lo, pc_hi);
            pc_lo = 0xffff;
            pc_hi = 0;
        }

        if (block % DRAW_EVERY == 0) {
            npg_draw_update();
        }

        /* 1280 blocks is ten seconds of the game, whatever the wall clock
           thinks. Anything but ten seconds here means the pacing is wrong. */
        if (!checked && block == 1280u) {
            checked = true;
            const uint32_t ms = neos_uptime_ms() - t0;
            if (ms < 9800u || ms > 10200u) {
                printf("[nupogodi] ten emulated seconds took %u ms - %s\n",
                       (unsigned)ms, sound ? "the codec is not pacing us"
                                           : "no speaker, so this is a guess");
            }
        }
    }

    if (sm5a_illegal_count()) {
        printf("[nupogodi] %d illegal opcodes, last $%04X at $%04X - "
               "is that really an im-02 dump?\n",
               sm5a_illegal_count(), sm5a_illegal_op(), sm5a_illegal_pc());
    }

    npg_sound_end();
    npg_draw_end();
    npg_asset_free(&s_asset);
    neos_orient_unlock();
    return 0;
}
