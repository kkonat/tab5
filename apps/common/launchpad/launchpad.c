/*
 * Launchpad protocol for the MK2 and the Mini MK3. See launchpad.h for the
 * shared coordinate system, the table of what differs between them, and where
 * this came from.
 */

#include "launchpad.h"

#include <string.h>

/* --- Per-model constants ------------------------------------------------- */

typedef struct {
    lp_model_t  model;
    const char *name;
    uint8_t     sysex_id;        /* 6th header byte                          */
    uint8_t     inquiry_family;  /* byte 8 of the inquiry reply              */
    uint8_t     cable;           /* USB-MIDI cable carrying the surface      */
    uint8_t     rgb_max;         /* per-channel maximum on the wire          */
    uint8_t     has_logo_led;
} lp_model_desc_t;

static const lp_model_desc_t lp_models[] = {
    {
        .model          = LP_MODEL_MK2,
        .name           = "Launchpad MK2",
        .sysex_id       = LP_SYSEX_ID_MK2,
        .inquiry_family = 0x69U,
        .cable          = 0U,
        .rgb_max        = 63U,
        .has_logo_led   = 0U,
    },
    {
        .model          = LP_MODEL_MINI_MK3,
        .name           = "Launchpad Mini MK3",
        .sysex_id       = LP_SYSEX_ID_MINI_MK3,
        .inquiry_family = 0x13U,
        /*
         * Cable 1, not 0. The Mini MK3 puts two MIDI ports on one pair of bulk
         * endpoints - its configuration descriptor gives the OUT endpoint two
         * embedded jacks, BaAssocJackID {1, 3} - and the cable number selects
         * between them. Cable 0 is "LPMiniMK3 DAW", cable 1 is "LPMiniMK3
         * MIDI", which is the one Programmer mode speaks on.
         */
        .cable          = 1U,
        .rgb_max        = 127U,
        .has_logo_led   = 1U,
    },
};

#define LP_MODEL_COUNT  (sizeof(lp_models) / sizeof(lp_models[0]))

static const lp_model_desc_t *desc_for(lp_model_t model)
{
    for (uint32_t i = 0; i < LP_MODEL_COUNT; i++) {
        if (lp_models[i].model == model) {
            return &lp_models[i];
        }
    }

    return NULL;
}

/* --- Wire translation ---------------------------------------------------- */

/*
 * Canonical LED id -> the number the device uses for it.
 *
 * Identical on a Mini MK3, whose Programmer mode numbers the whole 9x9 as
 * row*10+col. On an MK2 the top row is control changes 104-111 instead, and
 * there is no logo LED.
 */
static uint8_t lp_wire_index(const lp_model_desc_t *d, lp_led_t led, uint8_t *is_cc)
{
    if (d->model == LP_MODEL_MK2) {
        if (lp_led_is_top(led)) {
            *is_cc = 1U;
            return (uint8_t)(104U + (lp_led_col(led) - 1U));
        }
        *is_cc = 0U;
        return led;
    }

    /* Mini MK3: the grid sends and receives notes, the two edges control
       changes. The number is the same either way. */
    *is_cc = (uint8_t)(!lp_led_is_pad(led));

    return led;
}

/* ...and back, for an incoming event. Returns 0 if it addresses nothing. */
static uint8_t lp_led_from_wire(const lp_model_desc_t *d, uint8_t is_cc, uint8_t number)
{
    if (d->model == LP_MODEL_MK2) {
        if (is_cc) {
            if ((number < 104U) || (number > 111U)) {
                return 0U;
            }
            return lp_led((uint8_t)(number - 104U + 1U), 9U);
        }
        return lp_led_valid(number) ? number : 0U;
    }

    return lp_led_valid(number) ? number : 0U;
}

/* Can this model light this LED at all? */
static uint8_t lp_led_supported(const lp_model_desc_t *d, lp_led_t led)
{
    if (!lp_led_valid(led)) {
        return 0U;
    }
    if ((led == LP_LED_LOGO) && !d->has_logo_led) {
        return 0U;
    }

    return 1U;
}

/* Internal 0-63 to the model's wire range. */
static uint8_t lp_scale_rgb(const lp_model_desc_t *d, uint8_t v)
{
    if (v > LP_RGB_MAX) {
        v = LP_RGB_MAX;
    }
    if (d->rgb_max == LP_RGB_MAX) {
        return v;
    }

    return (uint8_t)(((uint32_t)v * d->rgb_max) / LP_RGB_MAX);
}

/* --- Inquiry ------------------------------------------------------------- */

static const uint8_t lp_inquiry[] = { 0xF0U, 0x7EU, 0x7FU, 0x06U, 0x01U, 0xF7U };

/*
 * Reply shape, common to both models:
 *
 *   F0 7E <id> 06 02 00 20 29 <family> <...> <4 bytes version> F7
 *
 * Bytes 5-7 are Novation's manufacturer id and byte 8 the family: 0x69 for the
 * MK2, 0x13 for the Mini MK3. The version is four bytes holding one decimal
 * digit each. Only the fixed prefix is relied on here.
 */
#define LP_INQ_MIN_LEN      17U

void lp_reset(lp_device_t *dev)
{
    (void)memset(dev, 0, sizeof(*dev));
    dev->state      = LP_ABSENT;
    dev->model      = LP_MODEL_NONE;
    dev->model_name = "";
}

void lp_identify(lp_device_t *dev, uint16_t vid, uint16_t pid)
{
    dev->vid = vid;
    dev->pid = pid;

    dev->have_firmware = 0U;
    dev->pid_known     = 0U;
    dev->model         = LP_MODEL_NONE;
    dev->model_name    = "";
    dev->rx_len        = 0U;
    dev->rx_active     = 0U;

    if (dev->vid != LP_USB_VID) {
        dev->state = LP_MIDI_DEVICE;
        return;
    }

    /*
     * THE VID DECIDES THAT WE ASK; THE REPLY DECIDES WHAT IT IS.
     *
     * The PID only picks the provisional model, which is what the code has to
     * talk to for the second and a half before the inquiry comes back - and
     * what it keeps if the inquiry never does.
     *
     * A Novation PID in neither range is asked ANYWAY, on the Mini MK3's
     * protocol, rather than being written off as "not a Launchpad". That is
     * the case a second unit of the same model produces: the device ID is
     * added to the base PID, so two identical Launchpads are two different
     * products and no table of PIDs stays complete. Guessing wrong costs a
     * device that lights nothing, which is exactly what refusing to talk to it
     * costs; guessing right - and the whole current range is MK3-protocol -
     * costs nothing at all.
     */
    if ((dev->pid >= LP_MK2_PID_FIRST) && (dev->pid <= LP_MK2_PID_LAST)) {
        dev->model     = LP_MODEL_MK2;
        dev->pid_known = 1U;
    } else if ((dev->pid >= LP_MINI_MK3_PID_FIRST) &&
               (dev->pid <= LP_MINI_MK3_PID_LAST)) {
        dev->model     = LP_MODEL_MINI_MK3;
        dev->pid_known = 1U;
    } else {
        dev->model     = LP_MODEL_MINI_MK3;
        dev->pid_known = 0U;
    }

    dev->model_name = desc_for(dev->model)->name;
    dev->state      = LP_IDENTIFYING;
}

bool lp_identify_rx(lp_device_t *dev, const uint8_t *packet)
{
    uint8_t cin = NEOS_MIDI_CIN(packet[0]);
    uint32_t n;

    if (dev->state != LP_IDENTIFYING) {
        return false;
    }

    switch (cin) {
    case NEOS_MIDI_CIN_SYSEX:      n = 3U; break;
    case NEOS_MIDI_CIN_SYSEX_END1: n = 1U; break;
    case NEOS_MIDI_CIN_SYSEX_END2: n = 2U; break;
    case NEOS_MIDI_CIN_SYSEX_END3: n = 3U; break;
    default:                       return false;
    }

    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = packet[1U + i];

        if (b == 0xF0U) {
            dev->rx_active = 1U;
            dev->rx_len    = 0U;
        }
        if (!dev->rx_active) {
            continue;
        }
        if (dev->rx_len >= sizeof(dev->rx_sysex)) {
            dev->rx_active = 0U;    /* too long to be the reply; resynchronise */
            dev->rx_len    = 0U;
            continue;
        }
        dev->rx_sysex[dev->rx_len++] = b;
    }

    if ((cin == NEOS_MIDI_CIN_SYSEX) || !dev->rx_active) {
        return false;
    }
    dev->rx_active = 0U;

    if ((dev->rx_len < LP_INQ_MIN_LEN) ||
        (dev->rx_sysex[1] != 0x7EU) ||
        (dev->rx_sysex[3] != 0x06U) || (dev->rx_sysex[4] != 0x02U) ||
        (dev->rx_sysex[5] != 0x00U) || (dev->rx_sysex[6] != 0x20U) ||
        (dev->rx_sysex[7] != 0x29U)) {
        return false;
    }

    /*
     * Trust the reply over the PID. If they disagree the device is the one
     * telling the truth about itself.
     */
    for (uint32_t i = 0; i < LP_MODEL_COUNT; i++) {
        if (lp_models[i].inquiry_family == dev->rx_sysex[8]) {
            dev->model      = lp_models[i].model;
            dev->model_name = lp_models[i].name;
            dev->device_id  = dev->rx_sysex[2];
            (void)memcpy(dev->firmware, &dev->rx_sysex[12], 4U);
            dev->have_firmware = 1U;
            dev->state         = LP_READY;
            /* The device has now said what it is, so the PID no longer needs
               to have been recognised for the model to be trusted. */
            dev->pid_known     = 1U;

            return true;
        }
    }

    return false;
}

bool lp_send_inquiry(void)
{
    /*
     * Sent on both cables. At this point the model is only guessed from the
     * PID, so which cable carries the surface is not yet known - and a device
     * that ignores the inquiry on the wrong one would simply never answer.
     * Cable 0 exists on every USB-MIDI device; cable 1 only on the Mini MK3,
     * where a stray message is harmless.
     */
    if (!neos_midi_send_sysex(0U, lp_inquiry, (int)sizeof(lp_inquiry))) {
        return false;
    }

    return neos_midi_send_sysex(1U, lp_inquiry, (int)sizeof(lp_inquiry));
}

/* --- Surface mode -------------------------------------------------------- */

static bool lp_send_cmd(const lp_model_desc_t *d, uint8_t cmd, uint8_t arg)
{
    const uint8_t msg[] = {
        LP_SYSEX_PREFIX, d->sysex_id, cmd, arg, LP_SYSEX_END
    };

    return neos_midi_send_sysex(d->cable, msg, (int)sizeof(msg));
}

bool lp_enter_surface_mode(const lp_device_t *dev)
{
    const lp_model_desc_t *d = desc_for(dev->model);

    if (d == NULL) {
        return false;
    }

    if (d->model == LP_MODEL_MK2) {
        /* Session layout: the one whose note numbering matches the LED
           indices the SysEx messages use. */
        return lp_send_cmd(d, LP_MK2_LAYOUT, LP_MK2_LAYOUT_SESSION);
    }

    /*
     * Programmer mode. Without it the Mini MK3 is in a Custom mode, where the
     * grid sends whatever that mode was configured with rather than 11-88, and
     * only the 8x8 responds to lighting.
     */
    return lp_send_cmd(d, LP_MK3_MODE, 1U);
}

bool lp_leave_surface_mode(const lp_device_t *dev)
{
    const lp_model_desc_t *d = desc_for(dev->model);

    if (d == NULL) {
        return false;
    }
    if (d->model == LP_MODEL_MK2) {
        return lp_send_cmd(d, LP_MK2_LAYOUT, LP_MK2_LAYOUT_SESSION);
    }

    /* Back to Live mode. Programmer mode also disables the device's own setup
       menu, so leaving it behind would be rude. */
    return lp_send_cmd(d, LP_MK3_MODE, 0U);
}

/* --- Lighting ------------------------------------------------------------ */

bool lp_set_led(const lp_device_t *dev, lp_led_t led, uint8_t colour)
{
    const lp_model_desc_t *d = desc_for(dev->model);
    uint8_t is_cc;
    uint8_t index;

    if ((d == NULL) || !lp_led_supported(d, led)) {
        return false;
    }

    index  = lp_wire_index(d, led, &is_cc);
    colour = (uint8_t)(colour & 0x7FU);

    if (d->model == LP_MODEL_MK2) {
        const uint8_t msg[] = {
            LP_SYSEX_PREFIX, d->sysex_id, LP_MK2_LED_PALETTE, index, colour,
            LP_SYSEX_END
        };

        return neos_midi_send_sysex(d->cable, msg, (int)sizeof(msg));
    }

    {
        const uint8_t msg[] = {
            LP_SYSEX_PREFIX, d->sysex_id, LP_MK3_LED,
            LP_MK3_LIGHT_STATIC, index, colour,
            LP_SYSEX_END
        };

        return neos_midi_send_sysex(d->cable, msg, (int)sizeof(msg));
    }
}

bool lp_clear_all(const lp_device_t *dev)
{
    const lp_model_desc_t *d = desc_for(dev->model);

    if (d == NULL) {
        return false;
    }

    if (d->model == LP_MODEL_MK2) {
        return lp_send_cmd(d, LP_MK2_LED_ALL, LP_COLOUR_OFF);
    }

    /*
     * The Mini MK3 has no "all LEDs" command, so every LED is named. 81
     * entries is exactly the documented maximum for one message.
     */
    {
        uint8_t msg[LP_SYSEX_HEADER_LEN + 1U + (LP_SIDE * LP_SIDE * 3U) + 1U];
        uint32_t at = 0;
        static const uint8_t prefix[] = { LP_SYSEX_PREFIX };

        (void)memcpy(&msg[at], prefix, sizeof(prefix));
        at += sizeof(prefix);
        msg[at++] = d->sysex_id;
        msg[at++] = LP_MK3_LED;

        for (uint8_t row = 1; row <= LP_SIDE; row++) {
            for (uint8_t col = 1; col <= LP_SIDE; col++) {
                msg[at++] = LP_MK3_LIGHT_STATIC;
                msg[at++] = lp_led(col, row);
                msg[at++] = LP_COLOUR_OFF;
            }
        }
        msg[at++] = LP_SYSEX_END;

        return neos_midi_send_sysex(d->cable, msg, (int)at);
    }
}

bool lp_set_leds_rgb(const lp_device_t *dev, const lp_led_rgb_t *leds, uint32_t n)
{
    const lp_model_desc_t *d = desc_for(dev->model);

    /* Sized for the wider of the two: the Mini MK3 spends 5 bytes per entry
       (type, index, r, g, b) against the MK2's 4. */
    uint8_t  msg[LP_SYSEX_HEADER_LEN + 1U + (LP_LEDS_MAX * 5U) + 1U];
    uint32_t at    = 0;
    uint32_t wrote = 0;
    static const uint8_t prefix[] = { LP_SYSEX_PREFIX };

    if ((d == NULL) || (n > LP_LEDS_MAX)) {
        return false;
    }

    (void)memcpy(&msg[at], prefix, sizeof(prefix));
    at += sizeof(prefix);
    msg[at++] = d->sysex_id;
    msg[at++] = (d->model == LP_MODEL_MK2) ? LP_MK2_LED_RGB : LP_MK3_LED;

    for (uint32_t i = 0; i < n; i++) {
        uint8_t is_cc;

        if (!lp_led_supported(d, leds[i].led)) {
            continue;
        }

        if (d->model != LP_MODEL_MK2) {
            msg[at++] = LP_MK3_LIGHT_RGB;
        }
        msg[at++] = lp_wire_index(d, leds[i].led, &is_cc);

        /* Every SysEx byte must have bit 7 clear. A channel over the model's
           maximum would not merely look wrong, it would break the message
           frame - so the scaling clamps rather than trusting. */
        msg[at++] = lp_scale_rgb(d, leds[i].r);
        msg[at++] = lp_scale_rgb(d, leds[i].g);
        msg[at++] = lp_scale_rgb(d, leds[i].b);
        wrote++;
    }

    if (wrote == 0U) {
        return true;            /* nothing this model can light; not a failure */
    }

    msg[at++] = LP_SYSEX_END;

    return neos_midi_send_sysex(d->cable, msg, (int)at);
}

bool lp_set_grid_rgb(const lp_device_t *dev, const uint8_t *rgb)
{
    lp_led_rgb_t leds[LP_GRID * LP_GRID];

    for (uint8_t row = 0; row < LP_GRID; row++) {
        for (uint8_t col = 0; col < LP_GRID; col++) {
            const uint8_t *px = &rgb[((row * LP_GRID) + col) * 3U];
            lp_led_rgb_t  *e  = &leds[(row * LP_GRID) + col];

            /* Caller counts rows downward from the top; the device counts them
               upward from the bottom. */
            e->led = lp_led((uint8_t)(col + 1U), (uint8_t)(LP_GRID - row));
            e->r   = px[0];
            e->g   = px[1];
            e->b   = px[2];
        }
    }

    return lp_set_leds_rgb(dev, leds, LP_GRID * LP_GRID);
}

bool lp_set_all_rgb(const lp_device_t *dev, uint8_t r, uint8_t g, uint8_t b)
{
    lp_led_rgb_t leds[LP_LEDS_MAX];
    uint32_t     n = 0;
    bool         ok = true;

    for (uint8_t row = 1; row <= LP_SIDE; row++) {
        for (uint8_t col = 1; col <= LP_SIDE; col++) {
            leds[n].led = lp_led(col, row);
            leds[n].r   = r;
            leds[n].g   = g;
            leds[n].b   = b;
            n++;

            if (n == LP_LEDS_MAX) {
                ok = lp_set_leds_rgb(dev, leds, n) && ok;
                n  = 0;
            }
        }
    }

    if (n > 0U) {
        ok = lp_set_leds_rgb(dev, leds, n) && ok;
    }

    return ok;
}

/* --- Incoming events ----------------------------------------------------- */

bool lp_decode_event(const lp_device_t *dev, const uint8_t *packet,
                     lp_led_t *led, uint8_t *pressed, uint8_t *value)
{
    const lp_model_desc_t *d = desc_for(dev->model);
    uint8_t cin    = NEOS_MIDI_CIN(packet[0]);
    uint8_t cable  = NEOS_MIDI_CABLE(packet[0]);
    uint8_t number = packet[2];
    uint8_t data   = packet[3];
    uint8_t is_cc;
    uint8_t id;

    if (d == NULL) {
        return false;
    }
    /*
     * Ignore the other cable. On a Mini MK3 the DAW port keeps reporting the
     * same presses, so without this every press would be handled twice.
     */
    if (cable != d->cable) {
        return false;
    }

    switch (cin) {
    case NEOS_MIDI_CIN_NOTE_ON:
    case NEOS_MIDI_CIN_NOTE_OFF:
        is_cc = 0U;
        break;
    case NEOS_MIDI_CIN_CC:
        is_cc = 1U;
        break;
    default:
        return false;
    }

    id = lp_led_from_wire(d, is_cc, number);
    if (id == 0U) {
        return false;
    }

    *led   = id;
    *value = data;
    /* Both models report a release as note-on with velocity 0, and accept a
       real note-off as well. */
    *pressed = (uint8_t)((cin != NEOS_MIDI_CIN_NOTE_OFF) && (data > 0U));

    return true;
}
