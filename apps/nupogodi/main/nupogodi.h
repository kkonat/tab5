/*
 * Nu, pogodi! - Электроника ИМ-02, which is a Game & Watch underneath.
 *
 * The wolf catches eggs. What actually runs is the mask ROM out of a
 * КБ1013ВК1-2, a Soviet clone of the Sharp SM5A, on an emulated CPU (sm5a.c) -
 * so the difficulty curve, the hare at 200 and 500 points and the piezo are
 * not reimplemented here, they are what the 1856 bytes do. Nothing in this app
 * knows the rules of the game.
 *
 * What the app is, then, is the rest of the handheld: a segment LCD, four
 * buttons and a piezo.
 *
 *   npg_asset  loads the artwork and the ROM off the card
 *   npg_draw   turns 72 segment states into pixels, repainting as few as it can
 *   npg_sound  turns the piezo pin into something the ES8388 will play
 *   npg_panel  turns the glass into the four corner buttons and the mode switches
 *
 * The main loop is paced by audio, not by a timer: a block of sound is
 * exactly a block of emulated time, and neos_audio_write() blocks until the
 * codec has taken it. See nupogodi_app.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ngl.h"
#include "neos_api.h"

#include "sm5a.h"

/* ------------------------------------------------------------------ */
/* The asset                                                           */
/* ------------------------------------------------------------------ */

/** One LCD segment: where it is on the artwork, and where its coverage is. */
typedef struct {
    uint16_t x, y, w, h;
    uint32_t px;        /**< first nibble of this segment's coverage */
} npg_seg_t;

/** The SM5A's whole program space, so the ROM can just live in the struct. */
#define NPG_ROM_MAX 2048

/**
 * Everything that came off the card.
 *
 * The background is RGB565 because that is what the panel eats. The segments
 * are *coverage*, not alpha: 255 means "leave the background alone" and 0
 * means "black", and a lit segment multiplies rather than replaces - which is
 * what a real LCD segment does to the artwork printed behind it, and is why
 * the printed hens still show through a lit egg.
 *
 * The file packs coverage as nibbles; this holds it expanded to a byte each.
 * It doubles a few hundred kilobytes on a tablet with thirty-two megabytes,
 * and it takes the nibble arithmetic out of the one loop that runs per pixel.
 */
typedef struct {
    uint16_t     w, h;              /**< the artwork, in pixels */

    /*
     * The picture inside the dark border the artwork came with.
     *
     * MAME's SVG is the display on a black page, so what comes out of genlcd
     * has a flat black frame around it. On a tablet that frame is not artwork,
     * it is just black - and it is exactly where the case ought to be. So it
     * is measured at load time and then never drawn: the app hands ngl only
     * what is inside it and paints its own bezel in the space that frees up.
     *
     * A rectangle rather than one number because the four edges are not the
     * same. The border is whatever fell outside the drawing in somebody's SVG,
     * and taking the narrowest of the four off all of them - which is what one
     * number would mean - leaves a black band down whichever side had more.
     */
    ngl_rect_t   vis;
    uint16_t     nseg;
    uint16_t     rom_len;
    uint8_t      rom[NPG_ROM_MAX];  /**< the mask ROM */
    ngl_color_t *bg;                /**< w*h, owned */
    npg_seg_t   *seg;               /**< nseg entries, owned */
    uint8_t     *cov;               /**< one byte per segment pixel, owned */

    /*
     * Where the ROM keeps the time, as six BCD nibbles in its own RAM: hours
     * tens and units, minutes, seconds. `time_pm` is the bit inside the hours
     * tens nibble that means afternoon - these are twelve-hour clocks, and on
     * this machine that bit is what lights ДП or ПП. Zero means this game has
     * no clock, or nobody has worked out where it keeps it.
     */
    uint8_t      time_addr[6];
    uint8_t      time_pm;

    char         title[32];
} npg_asset_t;

/**
 * Read `rel` off the card and unpack it.
 *
 * The artwork is drawn at exactly the size it arrives at; nothing here
 * scales. tools/genlcd renders the SVG straight to the size the app has room
 * for, and a resampler on a desktop with the vectors still in hand will
 * always beat one on the tablet working from the result - so an asset built
 * for a different screen is centred at its own size rather than stretched.
 *
 * False on any failure, having freed whatever it had got as far as
 * allocating. The reason goes to the status line.
 */
bool npg_asset_load(npg_asset_t *a, const char *rel);

void npg_asset_free(npg_asset_t *a);



/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

/**
 * Build the compose buffer and paint the first frame.
 *
 * `at` is where the picture goes - `a->vis` on the screen, so it is that
 * rectangle's size and not the asset's own. Everything outside it belongs to
 * the case, and nothing here will ever paint there: the border is still in the
 * compose buffer, because a segment's shadow can reach into it and has to have
 * somewhere to land, it just never reaches the screen.
 *
 * False if the compose buffer would not allocate, which on a 32 MB tablet
 * means the asset is implausible rather than that memory is tight.
 */
bool npg_draw_begin(const npg_asset_t *a, ngl_rect_t at);

/**
 * Repaint whatever changed since the last call, and flush.
 *
 * Reads the segment states straight out of the core. Nothing happens at all
 * on a tick where no segment moved, which is most of them - the LCD in this
 * thing changes a handful of segments a few times a second, and repainting
 * the whole 1.4 MB background at 128 Hz to find that out would be the most
 * expensive thing the app does.
 */
void npg_draw_update(void);

/** Force the next update to repaint everything. After a panel came up. */
void npg_draw_invalidate(void);

void npg_draw_end(void);

/* ------------------------------------------------------------------ */
/* Sound                                                               */
/* ------------------------------------------------------------------ */

/**
 * One block of emulated time.
 *
 * SM5A_CLOCK_HZ and NEOS_AUDIO_RATE are 32768 and 48000, whose ratio is
 * exactly 256:375 - so a block of 256 piezo samples is a block of 375 codec
 * frames with nothing left over and no drift to carry between blocks. At
 * 128 instructions it is 7.8125 ms of the game, which is also the latency
 * from a tap to the noise it makes.
 */
#define NPG_BLOCK_TICKS  256
#define NPG_BLOCK_INSTR  (NPG_BLOCK_TICKS / SM5A_CLK_DIV)
#define NPG_BLOCK_FRAMES 375

/** Take the speaker. False if the codec will not come up; the app plays on. */
bool npg_sound_begin(void);

/** The core's write_r, and the only thing that may be passed as one. */
void npg_sound_pin(uint8_t r_out);

/**
 * Hand the block just recorded to the codec, and block until it is taken.
 *
 * This is the app's frame clock. Returns false once the stream has gone away,
 * after which the caller has to pace itself.
 */
bool npg_sound_flush(void);

void npg_sound_end(void);

/* ------------------------------------------------------------------ */
/* The buttons                                                         */
/* ------------------------------------------------------------------ */

/*
 * The key matrix as the ROM reads it. R2-R4 select a row, K1-K4 come back.
 * Row R2 is unused on this machine, which is why it is not here.
 */
typedef enum {
    NPG_KEY_RIGHT_DOWN = 0,   /* R3 */
    NPG_KEY_RIGHT_UP,
    NPG_KEY_LEFT_DOWN,
    NPG_KEY_LEFT_UP,

    NPG_KEY_TIME,             /* R4 */
    NPG_KEY_GAME_B,
    NPG_KEY_GAME_A,
    NPG_KEY_ALARM,

    NPG_KEY_N,
} npg_key_t;

/*
 * What the controls take out of the app area before the artwork gets any.
 *
 * Only the strip along the bottom. Height is what the artwork is short of -
 * it is half again as wide as it is tall, so on a 1280x720 panel it runs out
 * of height long before it runs out of width - so the strip is as shallow as
 * a row of legible buttons can be, and the artwork gets everything above it.
 *
 * The thumb pads then cost nothing at all. Fitting the artwork by height
 * leaves a margin down each side whether anything wants it or not, and that
 * is where they go: hard against the edges of the glass, which is where a
 * thumb already is when the tablet is held in two hands.
 */
/*
 * A switch is a button with its legend under it, the way it is on the case:
 * a pill of moulded plastic and the word printed below it. That is two rows
 * rather than one, which is what the height is - the pill, a hair of gap, and
 * a line of ngl_font_small, which is 32 tall.
 */
#define NPG_PILL_H  40      /**< the moulded part you actually press */
#define NPG_PILL_W  132     /**< at its widest; the strip may allow less */
#define NPG_CHIP_H  76      /**< the pill and its legend together */
#define NPG_STRIP_H 88      /**< what the row of them takes off the bottom */

/*
 * A thumb pad, at its biggest.
 *
 * The margin beside the artwork is whatever its aspect left over, and on a
 * wide panel that can be a lot - but a button does not get better by being
 * bigger than a thumb, it just gets louder. So the pads are capped and sit
 * against the outer edge, and any margin past that stays empty.
 */
#define NPG_PAD_W   168
#define NPG_PAD_H   300

/*
 * How much of a pad's height is not the button.
 *
 * The arrow is printed on the case above or below the rubber rather than on
 * it, which is where it is on the real machine - so the pad rectangle has to
 * hold both, and this is the arrow's share. What is left is the button, which
 * is why making this smaller is how the button gets bigger.
 */
#define NPG_ARROW_H 64

/*
 * How the case is drawn. The layout is identical either way - only the paint
 * differs, which is what stops a second style from being a second app.
 */
typedef enum {
    NPG_STYLE_NEOS = 0,     /**< phosphor green on black, like every other app */
    NPG_STYLE_EL,           /**< cream plastic, moulded seats, dark rubber */
    NPG_STYLE_N,
} npg_style_t;

/** Lay the controls out around an artwork occupying `art` inside `area`. */
void npg_panel_layout(ngl_rect_t area, ngl_rect_t art);

/** Fill the app area with the case, behind everything else. */
void npg_panel_ground(void);

/**
 * Paint the bezel around the glass.
 *
 * The artwork stops at `art` (npg_panel_layout's second argument) and the
 * case starts there, so something has to happen at the join. On the real
 * machine the display is sunk into the moulding, and that is what this draws:
 * a recess, shadowed along the top and left and catching the light along the
 * bottom and right, which is the whole difference between a screen with a
 * picture on it and an object with a screen in it.
 */
void npg_panel_frame(void);

/** Draw the controls. Call once; npg_panel_poll repaints what it changes. */
void npg_panel_paint(void);

/**
 * Read the glass and update which keys are down.
 *
 * Multi-touch, because the four corners are a joystick and holding one while
 * pressing another is a thing people do. Returns true if any key is down,
 * which is what sm5a_keys_active() wants.
 *
 * @param restyled  set when STYLE was pressed, which means everything on the
 *                  screen is now the wrong colour and the caller has to
 *                  repaint the lot. Reported rather than done here, because a
 *                  poll that repainted the artwork would be a poll that
 *                  needed the asset.
 */
bool npg_panel_poll(bool *restyled);

/** Which style is in force, and how to change it. Saved to the card. */
uint8_t npg_panel_style(void);
void    npg_panel_style_load(void);
void    npg_panel_style_set(uint8_t style);

/** The K lines for row `r_out`, as the ROM would read them. */
uint8_t npg_panel_k(uint8_t r_out);

/** True while ACL - the reset pad - is being held. */
bool npg_panel_acl(void);

/** The infinite-lives pad, which on the real board is unpopulated. */
bool npg_panel_cheat(void);
