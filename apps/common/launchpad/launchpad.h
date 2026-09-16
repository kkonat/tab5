/*
 * launchpad.h - Novation Launchpad protocol, for the MK2 and the Mini MK3.
 *
 * Sources: the Launchpad MK2 Programmer's Reference Manual v1.03 and the
 * Launchpad Mini [MK3] Programmer's Reference Manual, both Focusrite/Novation.
 *
 * Ported from the STM32 tree (stm32/disco32F746G/lib/surface), where it sits
 * on ST's USB host middleware. Everything about the device is unchanged; what
 * differs is the transport, which is neos_midi.h here and is not passed in -
 * there is one USB-A socket, so there is one Launchpad, and a handle would be
 * a parameter with one possible value. Rather than a USBH_StatusTypeDef,
 * everything returns bool.
 *
 * ONE COORDINATE SYSTEM
 *
 * Both devices are a 9x9 arrangement: 64 square pads, 8 round buttons across
 * the top, 8 round buttons down the right, and the logo in the corner. Both
 * number the surface as decimal row/column,
 *
 *     led = row * 10 + column,  row 1 at the BOTTOM, column 1 at the LEFT
 *
 * so the pad grid is 11-88, the right-hand column is 19-89, the top row is
 * 91-98 and the logo is 99. That is used as the identifier everywhere in this
 * code, for both models - lp_led_t.
 *
 * It is exactly what a Mini MK3 in Programmer mode puts on the wire. It is not
 * quite what an MK2 does: the MK2 has no addressable logo, and its top row is
 * control changes 104-111 rather than 91-98. Those two differences are
 * translated at the wire, by lp_wire_index() and lp_led_from_wire(), and
 * nowhere else.
 *
 * WHAT DIFFERS BELOW THAT
 *
 *                        MK2                  Mini MK3
 *   SysEx header id      0x18                 0x0D
 *   inquiry family       0x69                 0x13
 *   surface mode         Session layout       Programmer mode
 *   LED SysEx            one per command      one command, typed entries
 *   RGB channel range    0-63                 0-127
 *   USB cable            0                    1  (cable 0 is the DAW port)
 *
 * That last row is the one with no visible symptom when wrong: the Mini MK3
 * multiplexes two MIDI ports onto one pair of bulk endpoints and distinguishes
 * them by the cable number in the USB-MIDI event packet. Programmer mode lives
 * on the second. Send to cable 0 and the device accepts the bytes in silence.
 */

#ifndef LAUNCHPAD_H
#define LAUNCHPAD_H

#include <stdbool.h>
#include <stdint.h>

#include "neos_midi.h"

/* --- Identity ------------------------------------------------------------ */

/* Focusrite/Novation. */
#define LP_USB_VID              0x1235U

/*
 * NO LAUNCHPAD HAS A FIXED PRODUCT ID, and a second one of the same model is
 * how you find that out.
 *
 * The bootloader's device ID of 1-16 is added to a base PID, so two identical
 * units on the same bus enumerate as two different products - which is exactly
 * the case that used to be reported as "MIDI 1235:xxxx, not a Launchpad". The
 * MK2 runs 0x0069 to 0x0078 and the Mini MK3 0x0113 to 0x0122.
 *
 * The ranges are still only a HINT. Novation's VID is what decides that asking
 * is worthwhile, the device inquiry reply is what decides the model, and a PID
 * in neither range is asked anyway rather than refused - see lp_identify().
 * That is the whole of the fix: a range that is one entry too short is a
 * device that answers for itself, not a device that does not work.
 */
#define LP_MK2_PID_FIRST        0x0069U
#define LP_MK2_PID_LAST         0x0078U
#define LP_MINI_MK3_PID_FIRST   0x0113U
#define LP_MINI_MK3_PID_LAST    0x0122U

typedef enum {
    LP_MODEL_NONE = 0,
    LP_MODEL_MK2,
    LP_MODEL_MINI_MK3,
} lp_model_t;

/* --- SysEx --------------------------------------------------------------- */

/* Bytes 1-4 of every message; byte 5 is the per-model id, byte 6 the command. */
#define LP_SYSEX_PREFIX         0xF0U, 0x00U, 0x20U, 0x29U, 0x02U
#define LP_SYSEX_HEADER_LEN     6U
#define LP_SYSEX_END            0xF7U

#define LP_SYSEX_ID_MK2         0x18U
#define LP_SYSEX_ID_MINI_MK3    0x0DU

/* MK2 commands. */
#define LP_MK2_LED_PALETTE      0x0AU   /* <led> <colour>      , up to x80 */
#define LP_MK2_LED_RGB          0x0BU   /* <led> <r> <g> <b>   , up to x80 */
#define LP_MK2_LED_ALL          0x0EU   /* <colour>                        */
#define LP_MK2_TEXT             0x14U
#define LP_MK2_LAYOUT           0x22U   /* <layout>                        */
#define LP_MK2_LAYOUT_SESSION   0x00U

/* Mini MK3 commands. */
#define LP_MK3_LAYOUT           0x00U   /* <layout>                        */
#define LP_MK3_LED              0x03U   /* <colourspec>...     , up to x81 */
#define LP_MK3_TEXT             0x07U
#define LP_MK3_MODE             0x0EU   /* 0 = Live, 1 = Programmer        */
#define LP_MK3_DAW_MODE         0x10U   /* 0 = Standalone, 1 = DAW         */
#define LP_MK3_BRIGHTNESS       0x08U

/* Mini MK3 <colourspec> lighting types. */
#define LP_MK3_LIGHT_STATIC     0x00U   /* + 1 byte palette index          */
#define LP_MK3_LIGHT_FLASH      0x01U   /* + 2 bytes, colour B then A      */
#define LP_MK3_LIGHT_PULSE      0x02U   /* + 1 byte palette index          */
#define LP_MK3_LIGHT_RGB        0x03U   /* + 3 bytes, 0-127 each           */

/*
 * The internal RGB scale, used by everything above the wire. The MK2 wants
 * 0-63 and the Mini MK3 0-127; picking the narrower as the common currency
 * means scaling only ever widens, so no model loses resolution it could have
 * shown.
 */
#define LP_RGB_MAX              63U

/*
 * A few of the 128 palette indices, for lp_set_led() and lp_clear_all().
 *
 * The full table is in the STM32 tree, where a panel draws an on-screen
 * Launchpad and needs to know what each velocity looks like. Nothing here
 * does: the RGB messages say what they mean, and the only index this file
 * itself sends is OFF.
 */
#define LP_COLOUR_OFF           0U
#define LP_COLOUR_WHITE         3U
#define LP_COLOUR_RED           5U
#define LP_COLOUR_AMBER         9U
#define LP_COLOUR_YELLOW       13U
#define LP_COLOUR_GREEN        21U
#define LP_COLOUR_CYAN         37U
#define LP_COLOUR_BLUE         45U
#define LP_COLOUR_PINK         53U

/* --- Geometry ------------------------------------------------------------ */

#define LP_GRID                 8U      /* the square pads are 8x8 */
#define LP_SIDE                 9U      /* the whole surface is 9x9 */

#define LP_LED_LOGO             99U

/* row*10 + col, row 1 at the bottom, col 1 at the left. */
typedef uint8_t lp_led_t;

static inline lp_led_t lp_led(uint8_t col, uint8_t row)
{
    return (lp_led_t)((row * 10U) + col);
}

static inline uint8_t lp_led_row(lp_led_t led) { return (uint8_t)(led / 10U); }
static inline uint8_t lp_led_col(lp_led_t led) { return (uint8_t)(led % 10U); }

static inline uint8_t lp_led_valid(lp_led_t led)
{
    uint8_t r = lp_led_row(led);
    uint8_t c = lp_led_col(led);

    return (uint8_t)((r >= 1U) && (r <= 9U) && (c >= 1U) && (c <= 9U));
}

/* The 8x8 of square pads, as opposed to the round buttons around two edges. */
static inline uint8_t lp_led_is_pad(lp_led_t led)
{
    return (uint8_t)((lp_led_row(led) <= 8U) && (lp_led_col(led) <= 8U));
}

static inline uint8_t lp_led_is_top(lp_led_t led)  { return (uint8_t)(lp_led_row(led) == 9U); }
static inline uint8_t lp_led_is_side(lp_led_t led) { return (uint8_t)(lp_led_col(led) == 9U); }

/* --- Device state -------------------------------------------------------- */

typedef enum {
    LP_ABSENT = 0,      /* nothing plugged in                                */
    LP_UNKNOWN_DEVICE,  /* a USB device, but not one that speaks MIDI        */
    LP_MIDI_DEVICE,     /* a MIDI device, but not a Launchpad this knows     */
    LP_IDENTIFYING,     /* looks right; device inquiry sent, awaiting reply  */
    LP_READY,           /* confirmed                                         */
} lp_state_t;

typedef struct {
    lp_state_t state;
    lp_model_t model;
    const char *model_name;
    uint16_t   vid;
    uint16_t   pid;
    uint8_t    device_id;
    uint8_t    firmware[4];       /* one decimal digit per byte              */
    uint8_t    have_firmware;

    /* Whether the PID landed in a range this file knows, as opposed to the
       model having been guessed so that there is something to talk to while
       the inquiry is outstanding. Only a panel reads it - a guess that the
       device never corrects is worth saying out loud. */
    uint8_t    pid_known;

    /* Reassembly of the device inquiry reply. */
    uint8_t    rx_sysex[40];
    uint32_t   rx_len;
    uint8_t    rx_active;
} lp_device_t;

/* --- API ----------------------------------------------------------------- */

void lp_reset(lp_device_t *dev);

/* Classify a freshly enumerated device from its VID and PID. */
void lp_identify(lp_device_t *dev, uint16_t vid, uint16_t pid);

/*
 * Feed every received event packet here while identifying. Reassembles the
 * device inquiry reply and promotes to LP_READY. Returns true when it just did.
 */
bool lp_identify_rx(lp_device_t *dev, const uint8_t *packet);

/* Send the MIDI device inquiry, F0 7E 7F 06 01 F7, to every plausible cable. */
bool lp_send_inquiry(void);

/*
 * Put the surface into the mode whose note numbering matches the LED indices:
 * Session layout on an MK2, Programmer mode on a Mini MK3.
 */
bool lp_enter_surface_mode(const lp_device_t *dev);

/* Hand the device back the way it was found. */
bool lp_leave_surface_mode(const lp_device_t *dev);

bool lp_set_led(const lp_device_t *dev, lp_led_t led, uint8_t colour);
bool lp_clear_all(const lp_device_t *dev);

/* One LED and the colour to put on it, on the internal 0-LP_RGB_MAX scale. */
typedef struct {
    lp_led_t led;
    uint8_t  r, g, b;
} lp_led_rgb_t;

/*
 * The documented ceiling for one lighting message is 80 entries on an MK2 and
 * 81 on a Mini MK3; 80 is what both accept.
 */
#define LP_LEDS_MAX             80U

/*
 * Light an arbitrary set of LEDs in one message - the top row, the scene
 * column, one transport button, an arrow across the pads.
 *
 * This is the general form and lp_set_grid_rgb() is the special case; the
 * per-model differences (the MK2's 4-byte entries against the Mini MK3's
 * typed 5-byte ones, the top row's control-change numbering, the channel
 * range) all live here rather than in the caller. An LED the model cannot
 * light - the MK2's logo - is skipped rather than refused, so a caller can
 * name the whole 9x9 without asking which device it is talking to.
 */
bool lp_set_leds_rgb(const lp_device_t *dev, const lp_led_rgb_t *leds, uint32_t n);

/*
 * Light the whole 8x8 grid in one message. `rgb` is 64 triplets in row-major
 * order from the TOP-LEFT pad - the order the screen draws in, and the reverse
 * of the note numbering's bottom-up rows. Channels are 0-LP_RGB_MAX.
 */
bool lp_set_grid_rgb(const lp_device_t *dev, const uint8_t *rgb);

/*
 * Light every LED on the surface one colour, pads and round buttons alike.
 *
 * 81 LEDs is one entry over what a single message may carry, so this is two
 * messages on a Mini MK3 and three on an MK2 (whose entries are shorter but
 * whose logo does not exist). The caller does not need to know that; what it
 * does need to know is that the surface therefore lights in two goes a
 * millisecond apart, which is visible if the colour is bright.
 */
bool lp_set_all_rgb(const lp_device_t *dev, uint8_t r, uint8_t g, uint8_t b);

/*
 * Decode an incoming event packet into a button event. Returns true if the
 * packet was one. `value` is velocity or controller value; pressed is
 * value != 0.
 */
bool lp_decode_event(const lp_device_t *dev, const uint8_t *packet,
                     lp_led_t *led, uint8_t *pressed, uint8_t *value);

#endif /* LAUNCHPAD_H */
