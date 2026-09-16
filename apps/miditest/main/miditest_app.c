/*
 * miditest - power the USB-A socket, find a Launchpad on it, light it.
 *
 * WHAT IT IS FOR
 *
 * The USB-A socket is a host port and until now nothing had ever used it. This
 * is the app that says whether it works, and it is written so that each thing
 * that can be wrong says so separately rather than all of them arriving as a
 * surface that stays dark:
 *
 *   the rail          neos_midi_open() switches USB 5 V on; if the expander
 *                     refuses, the state line says "off" and nothing else runs
 *   enumeration       "idle" means powered and nothing detected - a cable, a
 *                     socket or a device that will not enumerate
 *   the class match   "not MIDI" means it enumerated and is not a MIDI device,
 *                     which is also what a hub looks like (there is no hub
 *                     support in this firmware)
 *   the OUT pipe      "ready" and a lit surface
 *   the IN pipe       the inquiry reply, which is what turns the model name
 *                     from a guess into the device's own answer, and then the
 *                     pads echoing presses back onto the screen
 *
 * So the screen is the test report. The lights are the headline.
 *
 * WHAT IT DOES TO THE DEVICE
 *
 * Puts it in the mode whose numbering matches the LED indices - Programmer
 * mode on a Mini MK3 - lights the whole 9x9 dim white, and brightens whatever
 * is held down. On the way out it clears the surface and puts the mode back,
 * because Programmer mode also disables the device's own setup menu and
 * leaving it there would be rude to whoever picks the Launchpad up next.
 */

#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_midi.h"
#include "neos_status.h"
#include "neos_sys.h"

#include "launchpad.h"

/*
 * Dim white, on the internal 0-63 scale, and what a held pad goes to.
 *
 * Eight is about an eighth of full, which on a Launchpad's LEDs is clearly lit
 * in a lit room and does not light the ceiling in a dark one. It is also where
 * the power draw stays sane: 81 LEDs at full white is most of what the socket
 * can give, and this tablet may be running off its battery.
 */
#define DIM  8
#define LIT 48

/* How long to wait for the device inquiry reply before trusting the PID. */
#define IDENTIFY_MS 1500

static lp_device_t lp;

/* Press state of the whole 9x9, indexed [row 1..9][col 1..9] via lp_led(). */
static uint8_t held[100];

/* ------------------------------------------------------------------ */
/* The device                                                          */
/* ------------------------------------------------------------------ */

/*
 * Light the surface the way this app wants it, from scratch.
 *
 * Also the tap action: a surface that has been unplugged and back in, or that
 * missed a message, is one press away from being right again without closing
 * the app.
 */
static void light_all(void)
{
    memset(held, 0, sizeof(held));
    lp_set_all_rgb(&lp, DIM, DIM, DIM);
}

/* The device has said what it is, or the inquiry timed out and the PID stands. */
static void surface_up(void)
{
    lp.state = LP_READY;
    lp_enter_surface_mode(&lp);
    light_all();
}

static void pad_press(lp_led_t led, uint8_t pressed)
{
    if (!lp_led_valid(led)) {
        return;
    }
    held[led] = pressed;

    const uint8_t v = pressed ? LIT : DIM;
    const lp_led_rgb_t one = { .led = led, .r = v, .g = v, .b = v };

    lp_set_leds_rgb(&lp, &one, 1);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/*
 * The on-screen surface is drawn from `held`, not from what was sent to the
 * device. That is the point of it: a pad that lights on the screen and not on
 * the Launchpad is an OUT pipe problem, and one that lights on the Launchpad
 * and not on the screen is an IN pipe problem. A mirror that drew what it had
 * just sent could not tell those apart.
 */
static void draw_surface(ngl_surface_t *sc, ngl_rect_t a)
{
    const int16_t cell = (int16_t)((a.h - 8) / LP_SIDE);
    const int16_t gap  = 4;
    const int16_t side = (int16_t)(cell * LP_SIDE);
    const int16_t x0   = (int16_t)(a.x + a.w - side - 16);
    const int16_t y0   = (int16_t)(a.y + (a.h - side) / 2);

    for (uint8_t row = 1; row <= LP_SIDE; row++) {
        for (uint8_t col = 1; col <= LP_SIDE; col++) {
            const lp_led_t led = lp_led(col, row);

            /* Row 9 is the top row on the device and the top row here, so the
               screen counts down while the device counts up. */
            const ngl_rect_t r = {
                .x = (int16_t)(x0 + (col - 1) * cell),
                .y = (int16_t)(y0 + (LP_SIDE - row) * cell),
                .w = (int16_t)(cell - gap),
                .h = (int16_t)(cell - gap),
            };

            ngl_color_t fill = TH_PANEL;
            if (lp.state == LP_READY) {
                fill = held[led] ? TH_GLOW : NGL_RGB(32, 32, 32);
            }

            /* The round buttons are round; the logo is not a button at all. */
            if (led == LP_LED_LOGO) {
                ngl_draw_round_rect(sc, r, 6, TH_RULE, 1);
            } else if (lp_led_is_pad(led)) {
                ngl_fill_round_rect(sc, r, 4, fill);
                ngl_draw_round_rect(sc, r, 4, TH_EDGE, 1);
            } else {
                ngl_fill_round_rect(sc, r, (int16_t)(r.w / 2), fill);
            }
        }
    }
}

static void draw(ngl_surface_t *sc)
{
    const ngl_rect_t a = ngl_app_area();
    char line[80];

    ngl_clear(sc, TH_BG);

    int16_t y = (int16_t)(a.y + 12);
    const int16_t x = (int16_t)(a.x + 16);
    const int16_t step = (int16_t)(ngl_font_small.height + 8);

    ngl_text(sc, x, y, "USB-A host, MIDI", &ngl_font_large, TH_TEXT);
    y = (int16_t)(y + ngl_font_large.height + 14);

    const neos_midi_state_t ms = neos_midi_state();
    snprintf(line, sizeof(line), "port      %s", neos_midi_state_name(ms));
    ngl_text(sc, x, y, line, &ngl_font_small,
             ms == NEOS_MIDI_READY ? TH_OK : TH_WARN);
    y = (int16_t)(y + step);

    snprintf(line, sizeof(line), "5 V rail  %s",
             neos_feature(NEOS_FEAT_USB_5V) ? "on" : "off");
    ngl_text(sc, x, y, line, &ngl_font_small, TH_TEXT_DIM);
    y = (int16_t)(y + step);

    neos_midi_dev_t d;
    if (neos_midi_device(&d)) {
        snprintf(line, sizeof(line), "device    %04x:%04x", d.vid, d.pid);
        ngl_text(sc, x, y, line, &ngl_font_small, TH_TEXT);
        y = (int16_t)(y + step);

        if (d.product[0]) {
            snprintf(line, sizeof(line), "          %s", d.product);
            ngl_text(sc, x, y, line, &ngl_font_small, TH_TEXT_DIM);
            y = (int16_t)(y + step);
        }
        snprintf(line, sizeof(line), "endpoints in %u B x%u, out %u B x%u",
                 d.in_mps, d.cables_in, d.out_mps, d.cables_out);
        ngl_text(sc, x, y, line, &ngl_font_small, TH_TEXT_DIM);
        y = (int16_t)(y + step);
    }

    y = (int16_t)(y + 6);

    switch (lp.state) {
    case LP_ABSENT:
        ngl_text(sc, x, y, "waiting for a Launchpad", &ngl_font_small, TH_TEXT_DIM);
        break;
    case LP_UNKNOWN_DEVICE:
        ngl_text(sc, x, y, "not a MIDI device", &ngl_font_small, TH_BAD);
        break;
    case LP_MIDI_DEVICE:
        ngl_text(sc, x, y, "MIDI, but not Novation", &ngl_font_small, TH_WARN);
        break;
    case LP_IDENTIFYING:
        snprintf(line, sizeof(line), "asking... looks like %s", lp.model_name);
        ngl_text(sc, x, y, line, &ngl_font_small, TH_WARN);
        break;
    case LP_READY:
        ngl_text(sc, x, y, lp.model_name, &ngl_font_small, TH_OK);
        y = (int16_t)(y + step);
        if (lp.have_firmware) {
            snprintf(line, sizeof(line), "firmware  %u%u%u%u, device id %u",
                     lp.firmware[0], lp.firmware[1], lp.firmware[2],
                     lp.firmware[3], lp.device_id);
        } else {
            snprintf(line, sizeof(line), "no inquiry reply - model is a %s",
                     lp.pid_known ? "guess from the PID"
                                  : "guess, and the PID is unknown too");
        }
        ngl_text(sc, x, y, line, &ngl_font_small,
                 lp.have_firmware ? TH_TEXT_DIM : TH_WARN);
        break;
    default:
        break;
    }

    const uint32_t over = neos_midi_overruns();
    if (over) {
        snprintf(line, sizeof(line), "%lu packets dropped",
                 (unsigned long)over);
        ngl_text(sc, x, (int16_t)(a.y + a.h - 2 * step), line,
                 &ngl_font_small, TH_BAD);
    }

    ngl_text(sc, x, (int16_t)(a.y + a.h - step), "tap to relight",
             &ngl_font_small, TH_TEXT_FAINT);

    draw_surface(sc, a);
    ngl_flush();
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return 0;
    }

    lp_reset(&lp);

    if (!neos_midi_open()) {
        neos_status_for("USB host would not start", 4000);
    }

    uint32_t identify_started = 0;
    uint32_t last_seq = 0;
    bool     dirty = true;

    while (!neos_app_close_requested()) {
        const neos_midi_state_t ms = neos_midi_state();

        /*
         * Attach and detach are driven off the sequence number, not off the
         * state: unplug and plug back in between two passes of this loop reads
         * as READY both times, and everything below would then be talking to
         * the new device with the old device's identity.
         */
        neos_midi_dev_t d;
        if (ms == NEOS_MIDI_READY && neos_midi_device(&d) && d.seq != last_seq) {
            last_seq = d.seq;
            lp_identify(&lp, d.vid, d.pid);
            if (lp.state == LP_IDENTIFYING) {
                lp_send_inquiry();
                identify_started = (uint32_t)neos_uptime_ms();
            }
            dirty = true;
        } else if (ms != NEOS_MIDI_READY && lp.state != LP_ABSENT) {
            lp_reset(&lp);
            memset(held, 0, sizeof(held));
            last_seq = 0;
            if (ms == NEOS_MIDI_OTHER) {
                lp.state = LP_UNKNOWN_DEVICE;
            }
            dirty = true;
        }

        /* Everything the device has said since the last pass. */
        uint8_t packets[16 * NEOS_MIDI_PACKET];
        int n;
        while ((n = neos_midi_recv(packets, 16)) > 0) {
            for (int i = 0; i < n; i++) {
                const uint8_t *p = &packets[i * NEOS_MIDI_PACKET];

                if (lp.state == LP_IDENTIFYING) {
                    if (lp_identify_rx(&lp, p)) {
                        surface_up();
                        dirty = true;
                    }
                    continue;
                }

                lp_led_t led;
                uint8_t  pressed, value;
                if (lp.state == LP_READY &&
                    lp_decode_event(&lp, p, &led, &pressed, &value)) {
                    pad_press(led, pressed);
                    dirty = true;
                }
            }
        }

        /*
         * The inquiry never came back. Go with the model the PID suggested
         * rather than sitting here: a Launchpad that lights up and is called
         * by the wrong name is a better answer than one that stays dark, and
         * the screen says which of the two this is.
         */
        if (lp.state == LP_IDENTIFYING &&
            (uint32_t)neos_uptime_ms() - identify_started > IDENTIFY_MS) {
            surface_up();
            dirty = true;
        }

        int16_t tx, ty;
        if (neos_touch_tap(&tx, &ty) && lp.state == LP_READY) {
            light_all();
            dirty = true;
        }

        if (dirty) {
            draw(sc);
            dirty = false;
        }

        neos_sleep_ms(20);
    }

    /*
     * Put the device back. The drain matters: neos_midi_close() cancels what
     * is still in flight, so without it the last two messages - the ones that
     * undo everything this app did - are the ones that never arrive.
     */
    if (lp.state == LP_READY) {
        lp_clear_all(&lp);
        lp_leave_surface_mode(&lp);
        neos_midi_drain(500);
    }
    neos_midi_close();

    return 0;
}
