/*
 * clock - the same time, drawn five ways.
 *
 * A face is a paint function and a frame rate. Everything a face needs to know
 * arrives in one clock_frame_t, everything it is allowed to draw on is the
 * rect in that struct, and the shell in clock_app.c owns the rest: what time
 * it is, when the screen moved, which face is showing and how that survives a
 * reboot. So adding a sixth face is a file and a row in the table, and it
 * cannot break the other five.
 *
 * The faces do not share a look on purpose - a Casio LCD and a VFD are not the
 * NeOS palette with different numbers in it - so ngl_theme.h is used by the
 * shell and by exactly one face. What they do share is the drawing below: three
 * ways of making a digit large enough to read across a room, on a library whose
 * biggest font is 48 pixels tall.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ngl.h"

#include "neos_api.h"
#include "neos_sys.h"

/* ------------------------------------------------------------------ */
/* A frame                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    ngl_rect_t     area;       /**< what this face may draw on */
    ngl_rotation_t rot;        /**< which way up, for anything that cares */
    bool           landscape;

    neos_rtc_t     t;          /**< local wall time */
    bool           have_time;  /**< false: the machine does not know it */

    /*
     * What has changed since the last paint. A face repaints as little as it
     * can get away with - this is a software blitter and a full-screen repaint
     * at 30 Hz is most of a core - so these are the whole scheduling story:
     *
     *   full  the screen rotated, the face was just switched to, or something
     *         drew over it. Nothing on screen can be trusted; paint all of it.
     *   sec   the second changed.
     *   min   the minute changed, and so did the date line and everything else
     *         that is not a clock.
     */
    bool           full;
    bool           sec;
    bool           min;

    uint32_t       ms;         /**< uptime, for anything that animates */
} clock_frame_t;

/* ------------------------------------------------------------------ */
/* A face                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *key;    /**< what goes in the settings file; never retitled */
    const char *name;   /**< what the picker shows */
    const char *blurb;  /**< one line under it */

    void      (*paint)(const clock_frame_t *f, const void *cfg);

    /**
     * Give back anything the face is holding. May be NULL.
     *
     * Only one face needs it - the LCD keeps its dithered ground in an
     * off-screen surface, which is megabytes and would otherwise sit there
     * for the life of the app after somebody looked at it once. Called when
     * the face is switched away from and before the app returns.
     */
    void      (*leave)(void);

    /**
     * Offered a tap before the face picker gets it. May be NULL.
     *
     * The shell owns the tap because switching faces has to work on every one
     * of them, but two faces have something on the glass worth pointing at -
     * the place band, which opens the map. So the face gets first refusal, and
     * returning true means both "dealt with" and "whatever I put on the screen
     * was opaque, paint it all again".
     */
    bool      (*tap)(const clock_frame_t *f, int16_t x, int16_t y);

    const void *cfg;    /**< passed back to paint; how the two LEDs differ */

    uint16_t    frame_ms;  /**< how often it wants to be called */
} clock_face_t;

extern const clock_face_t clock_face_neos;
extern const clock_face_t clock_face_led_green;
extern const clock_face_t clock_face_led_red;
extern const clock_face_t clock_face_lcd;
extern const clock_face_t clock_face_vfd;
extern const clock_face_t clock_face_epaper;

/* ------------------------------------------------------------------ */
/* Colour                                                              */
/* ------------------------------------------------------------------ */

/**
 * Blend @p b into @p a, t 0-255. t=0 is a, t=255 is b.
 *
 * Everything on a face that is not fully on or fully off goes through here:
 * the trail of the rain, the halo round a lit segment, the dead segments of an
 * LCD, the ghost an e-paper leaves behind. Done in RGB565 rather than by
 * blending onto the framebuffer, because a colour computed once and then
 * filled is one pass over the pixels instead of a read-modify-write per pixel.
 */
ngl_color_t clk_mix(ngl_color_t a, ngl_color_t b, uint8_t t);

/* ------------------------------------------------------------------ */
/* Big digits, three ways                                              */
/* ------------------------------------------------------------------ */

/*
 * ngl's largest font is 24x48, which is a caption on a 720x1280 panel and not
 * a clock. None of what follows adds a font: each takes a shape that is
 * already there and makes it big, so the app carries no 200-pixel glyph table.
 */

/**
 * A bitmap font drawn with each of its pixels as a @p scale square.
 *
 * The blocky one. Deliberately not smoothed - at scale 4 a 24x48 glyph becomes
 * 96x192 and the steps are the look, which is what the NeOS and e-paper faces
 * want for different reasons.
 *
 * Rows of set bits are coalesced into one fill, so a glyph costs a few dozen
 * rectangles rather than a few thousand.
 */
void    clk_blocky(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                   const ngl_font_t *f, int16_t scale, ngl_color_t c);
int16_t clk_blocky_w(const ngl_font_t *f, const char *str, int16_t scale);

/**
 * Seven-segment geometry.
 *
 * `thick` is the segment across its short axis, `gap` the notch where two
 * segments meet, and `slant` how far the top of a digit leans right of its
 * bottom - zero for a clock radio, a few percent of the height for a watch.
 * Ends are mitred rather than square, which is what makes a drawn segment read
 * as a segment instead of a bar.
 */
typedef struct {
    int16_t w, h;
    int16_t thick;
    int16_t gap;
    int16_t slant;

    /**
     * Grow every segment by this much in all four directions, without moving
     * any of them.
     *
     * How the LED face gets its halo: the same digit is drawn two or three
     * times, biggest and dimmest first, and the bloom is the difference. It is
     * a field on the geometry rather than an argument because it must not
     * disturb the layout - widening `thick` would push the verticals around
     * and the rings would not line up with the core.
     */
    int16_t bleed;
} clk_seg7_t;

/** Fill in thick/gap from w/h with the proportions a real display uses. */
void clk_seg7_fit(clk_seg7_t *g, int16_t w, int16_t h, int16_t slant);

/**
 * One digit, dead segments and all. @p d is 0-9, or negative for a blank.
 *
 * @p off is what an unlit segment is painted, and is the difference between
 * the two faces that use this: an LED's dead segments are nearly the
 * background, an LCD's are a clearly visible ghost of the 8 underneath.
 *
 * A blank digit still paints its dead segments, so `d = -1` draws the full
 * figure eight in @p off and nothing in @p on - which is how both faces lay
 * down their ghost layer before anything is lit over it.
 */
void clk_seg7_digit(ngl_surface_t *s, int16_t x, int16_t y, int d,
                    const clk_seg7_t *g, ngl_color_t on, ngl_color_t off);

/**
 * Just the lit segments, for a halo or a shadow under the real ones.
 *
 * A blank digit lights nothing here, the same as above - the two disagreeing
 * would show up as a clock that does not know the time drawing solid eights.
 */
void clk_seg7_lit(ngl_surface_t *s, int16_t x, int16_t y, int d,
                  const clk_seg7_t *g, ngl_color_t c);

/**
 * The seven segments, and all fourteen of the cell below.
 *
 * A mask rather than a digit, for the two callers that need to say which
 * electrodes are driven without going through a character: the ghost layer,
 * which is every one of them, and the LCD's transition, which is part-way
 * between two characters and is several masks at several tints.
 */
#define CLK_SEG7_ALL   0x007Fu
#define CLK_SEG14_ALL  0x3FFFu

/** Which segments @p d lights; 0 for a blank. */
uint16_t clk_seg7_mask_of(int d);

/** Paint exactly the segments in @p mask, and nothing else. */
void clk_seg7_paint(ngl_surface_t *s, int16_t x, int16_t y, uint16_t mask,
                    const clk_seg7_t *g, ngl_color_t c);

/** The two dots of a colon, sized and placed to match a digit of this geometry. */
void clk_seg7_colon(ngl_surface_t *s, int16_t x, int16_t y,
                    const clk_seg7_t *g, ngl_color_t c);

/** How wide a colon cell is, so a caller can lay out around one. */
int16_t clk_seg7_colon_w(const clk_seg7_t *g);

/**
 * Fourteen segments: the same cell, spelling.
 *
 * A seven-segment display cannot write a word - there is no shape of the seven
 * that reads as a K or an M - so every panel that had to name something used
 * fourteen: the seven above, the middle split in two, and a star of six
 * through the centre. That is the whole difference, which is why the geometry
 * is the same struct and clk_seg14_fit() only picks a thinner stroke and a
 * wider notch than a seven of the same size would want.
 *
 * Capitals, digits and the marks between space and underscore. Lowercase folds
 * to uppercase, because the hardware had none.
 */
typedef clk_seg7_t clk_seg14_t;

void clk_seg14_fit(clk_seg14_t *g, int16_t w, int16_t h, int16_t slant);

/** Which of the fourteen @p ch lights; 0 for a blank or an absent glyph. */
uint16_t clk_seg14_mask_of(char ch);

/** Paint exactly the segments in @p mask. */
void clk_seg14_paint(ngl_surface_t *s, int16_t x, int16_t y, uint16_t mask,
                     const clk_seg14_t *g, ngl_color_t c);

/** One character, dead electrodes and all. @p off equal to @p on draws no ghost. */
void clk_seg14_char(ngl_surface_t *s, int16_t x, int16_t y, char ch,
                    const clk_seg14_t *g, ngl_color_t on, ngl_color_t off);

/**
 * 5x7 dot matrix, one round dot per cell.
 *
 * The VFD face's whole alphabet, because a vacuum fluorescent display is a
 * grid of phosphor dots and anything drawn as solid strokes stops looking like
 * one. @p pitch is centre to centre, @p dot the diameter.
 *
 * Passing @p off something other than the background lights every dot in the
 * cell first, which is the unlit-phosphor grid you can see on a real one when
 * the room is dark.
 */
/*
 * A letter with a mark on it is drawn in the blank row above or below its
 * seven, which is the row the pitch leaves between one line of cells and the
 * next. So a marked glyph stands a pitch taller than clk_dots_h() reports, the
 * same way a descender does - a caller erasing its own box has to leave a
 * pitch of margin, which every one of them already does for the bloom.
 */
void    clk_dots(ngl_surface_t *s, int16_t x, int16_t y, const char *str,
                 int16_t pitch, int16_t dot, ngl_color_t on, ngl_color_t off);
int16_t clk_dots_w(const char *str, int16_t pitch);
int16_t clk_dots_h(int16_t pitch);

/* ------------------------------------------------------------------ */
/* Letters that are not ASCII                                          */
/* ------------------------------------------------------------------ */

/*
 * One byte per letter, because on both of these displays a letter is one
 * addressable position on the glass. A name arriving as UTF-8 - which is how
 * the weather service hands one over - would otherwise put a two-byte L with a
 * stroke into two cells and draw neither, which is a gap in the middle of the
 * word rather than a letter.
 *
 * So names are converted once, up front, into this: ASCII in capitals, the
 * nine Polish letters as the codes below, and anything else dropped. The two
 * faces then do what their hardware can - see clock_text.c for why they differ
 * and why neither answer is a preference.
 */
#define CLK_PL_FIRST 0x80
#define CLK_PL_N     9

enum {
    CLK_A_OGONEK = CLK_PL_FIRST,
    CLK_C_ACUTE, CLK_E_OGONEK, CLK_L_STROKE, CLK_N_ACUTE,
    CLK_O_ACUTE, CLK_S_ACUTE, CLK_Z_ACUTE, CLK_Z_DOT,
};

/** @p c without its mark, for a display that has nowhere to put one. */
char clk_pl_base(char c);

/**
 * @p in as UTF-8 into @p out in the encoding above. Returns the length.
 *
 * Uppercases as it goes, since neither display has lowercase, and flattens
 * Latin-1's own accents to their base letters - the tablet can be anywhere,
 * and ZURICH beats ZRICH.
 */
size_t clk_pl_from_utf8(const char *in, char *out, size_t n);

/**
 * The mark on @p ch, for the matrix that can draw one.
 *
 * The low five bits are which columns of the row it occupies and the two flags
 * are which row - the blank one above the cell or the blank one below it. Zero
 * for a letter that has no mark, or has it inside the cell.
 */
#define CLK_ACC_ABOVE 0x20
#define CLK_ACC_BELOW 0x40

uint8_t clk_dot5x7_accent(char ch);

/* ------------------------------------------------------------------ */
/* Small things every face needs                                       */
/* ------------------------------------------------------------------ */

/**
 * An icon at one pixel per @p step, thresholded rather than blended.
 *
 * For the e-paper face, and it is not a stylisation: electronic ink is one bit
 * per pixel with no grey at all, so an alpha-blended icon is the one thing
 * that panel physically cannot show. Sampling the coverage on a coarse grid
 * and filling blocks is what the hardware would actually do.
 */
void clk_icon_blocky(ngl_surface_t *s, int16_t x, int16_t y, const ngl_icon_t *ic,
                     int16_t step, uint8_t threshold, ngl_color_t c);

/**
 * Fill @p s with a vertical gradient from @p top to @p bot, dithered.
 *
 * RGB565 has about seven distinct values between the two ends of a subtle
 * gradient, so a smooth ramp painted straight into it is seven visible bands
 * however many rows it is drawn in. Ordered dithering trades that for noise
 * the eye integrates instead, which is the whole reason this exists.
 *
 * Per-pixel and therefore not something to call every frame - it is meant to
 * be run once into an off-screen surface that is then blitted from.
 */
void clk_gradient_dither(ngl_surface_t *s, ngl_color_t top, ngl_color_t bot);

/**
 * The degree sign, as a string to paste into one.
 *
 * The system font stops at 126 and has no such glyph, so the two renderers
 * that can show a temperature part company here: the dot matrix carries the
 * shape and picks this byte up (see clock_dots.c), and everything drawn with
 * an ngl font gets a ring drawn next to the number instead.
 */
#define CLK_DEG "\xB0"

/** A degree ring, centred on (x,y). For the faces whose font has no glyph. */
void clk_degree(ngl_surface_t *s, int16_t x, int16_t y, int16_t r, ngl_color_t c);

/**
 * Centre a time block @p h tall over an info strip @p strip_h tall.
 *
 * Every face lays out the same way - one big thing, one small line under it,
 * the pair centred in what is left of the screen - and each one measures its
 * big thing differently, so what is shared is only the vertical stacking. Pass
 * a @p strip_h of zero for a face that has no strip; the block is then centred
 * on its own.
 *
 * Both rectangles come back the full width of @p area, not the width of the
 * content, because a face that repaints its time by filling first needs the
 * whole strip of screen the digits could ever occupy - not just the one this
 * minute's digits happen to want. Centring horizontally is left to the caller,
 * which is the only thing that knows how wide its own digits are.
 */
void clk_stack(ngl_rect_t area, int16_t h, int16_t strip_h,
               int16_t gap, ngl_rect_t *out_time, ngl_rect_t *out_strip);

/**
 * @p r grown by @p n on every side.
 *
 * Every face here draws something that spills past the box it was measured
 * into - a phosphor bloom, an LED halo, the wide dim pass under a VFD dot -
 * and the erase before a repaint has to cover the spill or last second's glow
 * survives this second's digits. So the faces measure the text and inflate.
 */
static inline ngl_rect_t clk_inflate(ngl_rect_t r, int16_t n)
{
    return ngl_rect((int16_t)(r.x - n), (int16_t)(r.y - n),
                    (int16_t)(r.w + 2 * n), (int16_t)(r.h + 2 * n));
}

/**
 * Which way gravity points, in the coordinates the app is drawing in.
 *
 * Both components are in milli-g and are the in-plane part only, so a tablet
 * lying flat reports roughly nothing and one held upright reports about 1000
 * downwards. False if there is no usable accelerometer, in which case a face
 * has to look right without one.
 *
 * Deriving this is the whole reason it is not three lines in the LCD face: the
 * sensor reports in the panel's frame, which does not move when the screen
 * rotates, so a tilt has to come back through the same transform a touch does
 * before it can be compared against anything drawn.
 */
bool clk_gravity(const clock_frame_t *f, int16_t *gx, int16_t *gy);

/** "SUN".."SAT" and "JAN".."DEC", for the date line. */
const char *clk_wday(const neos_rtc_t *t);
const char *clk_month(const neos_rtc_t *t);

/* ------------------------------------------------------------------ */
/* The weather                                                         */
/* ------------------------------------------------------------------ */

/*
 * A real display does not draw a picture of the weather. Every symbol it can
 * ever show is etched on the glass at the factory, all of them at once, and
 * "showing" one means driving that one electrode. The rest do not go away -
 * they sit there as faint shapes, which is how you can tell at a glance that
 * the panel has a snowflake on it at all.
 *
 * So the icon set below is fixed and ordered, and the faces built on that kind
 * of hardware draw the whole row every time and light one of them.
 */

#define CLK_WX_SET_N 10

/** Icon @p i of the fixed set, in the order it is printed on the glass. */
const ngl_icon_t *clk_wx_set(int i);

/**
 * Which of them the weather is, or -1 for none.
 *
 * -1 is the ordinary answer, not an error: no network, or none fetched yet.
 * A face draws the row faint and lights nothing.
 */
int clk_wx_index(const clock_frame_t *f);

/** The temperature, in tenths of a degree. False if there is no reading. */
bool clk_wx_temp_c10(int16_t *out);

/**
 * Where the reading came from, in capitals. False if nothing has landed.
 *
 * The city the address resolved to, which is the one piece of the weather that
 * is a word rather than a number - so the faces that can only show numbers put
 * it through a fourteen-segment marquee and the ones that cannot show it at
 * all leave it out.
 */
bool clk_wx_place(char *out, size_t n);

/**
 * @p cells characters of @p src, wound on by @p step.
 *
 * A marquee on a display of fixed cells does not slide: there is nowhere for
 * half a character to be, so the text steps along one whole cell at a time and
 * the panel redraws. That is what this returns - the window of the string that
 * is over the cells at step @p step, padded with spaces so the window is
 * always full and separated from its own tail by a run of them.
 *
 * A string that already fits is centred and does not move, which is what the
 * real ones did too.
 */
void clk_marquee(char *out, size_t n, const char *src, int cells, uint32_t step);

/**
 * The whole annunciator row: every icon, @p active lit, the rest faint.
 *
 * Laid out to fill @p r, which is what makes the positions fixed - a symbol
 * does not move when a different one lights up, because on the real thing it
 * could not. Pass -1 for @p active to light none.
 */
void clk_wx_icon_row(ngl_surface_t *s, ngl_rect_t r, int active,
                     ngl_color_t on, ngl_color_t off);

/**
 * Where symbol @p i of the fixed set lands inside @p r.
 *
 * For a face that cannot just call clk_wx_icon_row() - the LCD lights its
 * symbol part-way through a transition and throws a shadow under it, so it
 * draws that one itself and has to land on the same pixels the faint version
 * underneath is already on.
 */
ngl_rect_t clk_wx_icon_slot(ngl_rect_t r, int i);

/* ------------------------------------------------------------------ */
/* The date line, for the faces that can draw one                      */
/* ------------------------------------------------------------------ */

/*
 * Only for displays that can put an arbitrary glyph anywhere at any size: the
 * NeOS face, which is a terminal, and the e-paper one, which is a bitmap. The
 * LCD and the VFD lay their own bands out instead, out of the cells their
 * hardware has - seven and fourteen segments on one, a 5x7 matrix on the
 * other - because on those two the date is not a line of text, it is a fixed
 * number of fixed positions.
 */

/** Date, weekday and weather on one line, centred in @p r. NULL font: small. */
void clk_wx_strip(ngl_surface_t *s, ngl_rect_t r, const clock_frame_t *f,
                  const ngl_font_t *font, ngl_color_t fg, ngl_color_t bg);

/**
 * The same line at one resolution, for e-paper.
 *
 * Text through clk_blocky() and the icon through clk_icon_blocky(), both on
 * the same @p scale grid the time above is drawn on - so the whole panel reads
 * as one coarse device rather than as big chunky digits with a crisp caption
 * underneath them.
 */
void clk_wx_strip_blocky(ngl_surface_t *s, ngl_rect_t r, const clock_frame_t *f,
                         const ngl_font_t *font, int16_t scale,
                         ngl_color_t fg, ngl_color_t bg);

/* ------------------------------------------------------------------ */
/* The rain                                                            */
/* ------------------------------------------------------------------ */

/*
 * The NeOS face's background. Its own file because it is the only thing in the
 * app with state that has to survive between frames, and because it is a
 * second animation running under a clock rather than part of one.
 */

/** (Re)build the grid for @p area and start the rain over. */
void clk_rain_layout(ngl_rect_t area);

/**
 * Keep this rectangle clear.
 *
 * The clock is drawn over the rain, and a drop that stepped through it would
 * repaint one cell of a digit black and leave it there until the next second.
 * So the rain is told where the text is and steps around it - which also looks
 * better than compositing would: the rain parts around the time rather than
 * running behind it. Up to CLK_RAIN_HOLES of them; further calls are ignored.
 */
void clk_rain_reserve(ngl_rect_t r);
void clk_rain_clear_reserved(void);

#define CLK_RAIN_HOLES 4

/** Advance one step and draw what changed. */
void clk_rain_step(void);

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

/**
 * The chosen face, by key, or NULL if the card has never been written to.
 *
 * A key rather than an index: the table is going to be reordered eventually
 * and a stored 3 would then silently become a different face. The string
 * survives that.
 */
const char *clk_prefs_load(void);

/** The city the place band was pointed at, by key, or NULL for automatic. */
const char *clk_prefs_city(void);

/** Remember one. Failure is logged and otherwise ignored - it is a clock. */
void clk_prefs_save_face(const char *key);
void clk_prefs_save_city(const char *key);

/* ------------------------------------------------------------------ */
/* Where the weather is                                                */
/* ------------------------------------------------------------------ */

/**
 * The city that was picked off the map, as UTF-8, or NULL for automatic.
 *
 * Only the name: neos_weather.h has no way to say where to fetch for, so this
 * changes what the place band says and nothing else.
 */
const char *clk_city_chosen(void);

/**
 * Put the map up and wait. True if a city was chosen.
 *
 * Blocking and modal, the same way the face picker in clock_app.c is and for
 * the same reason - one app is resident and it is the one that stopped here.
 * The three colours are the face's own, so the window reads as a module in the
 * same glass rather than as a dialog from somewhere else. Touching off the map
 * returns false and changes nothing.
 */
bool clk_city_pick(const clock_frame_t *f, ngl_color_t bg, ngl_color_t on,
                   ngl_color_t off);
