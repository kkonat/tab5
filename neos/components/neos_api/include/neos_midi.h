/*
 * USB MIDI, host side.
 *
 * The USB-A socket is a host port: it is the ESP32-P4's USB 2.0 OTG
 * high-speed controller, on the UTMI PHY, which is the one the IDF's host
 * library installs by default on this target. The USB-C socket is the other
 * peripheral and is how the tablet is programmed, so nothing here touches it.
 *
 * WHY THIS IS IN THE ABI AND NOT IN AN APP
 *
 * Everything an app could plausibly do itself stays out of neos_*: a LAN scan
 * is the app's own work, so neos_sock.h hands over a socket rather than a
 * scanner. A USB host stack is not that. It is a driver - an interrupt, a
 * client task, DMA-capable transfer buffers and a device that can vanish
 * between two instructions - and there is exactly one root port, so two apps
 * holding their own copies of it is not a slow tablet, it is two stacks
 * fighting over one controller. It also has to outlive nothing: an app opens
 * it, uses it, closes it, and the port is free again.
 *
 * So this is the same shape as the rest of the ABI: the firmware owns the
 * driver, and an app is handed the one thing a USB-MIDI device actually
 * exchanges - 4-byte event packets.
 *
 * THE WIRE FORMAT
 *
 * USB-MIDI 1.0 does not put raw MIDI bytes on the bulk endpoints. It sends
 * fixed 4-byte event packets:
 *
 *     byte 0   (cable number << 4) | code index number
 *     byte 1-3 the MIDI message, zero-padded
 *
 * The code index number is normally the status nibble - 0x9 for note-on, 0xB
 * for control change - which makes byte 0 look redundant next to byte 1. It is
 * not: it carries the cable number, so one pair of endpoints can multiplex up
 * to 16 MIDI ports, and it is what makes SysEx framable, since SysEx has no
 * length. neos_midi_send_sysex() does that framing, which is the only part of
 * the encoding an app would otherwise have to get right to say anything at all.
 *
 * The cable number matters more than it looks. A Launchpad Mini MK3 puts two
 * ports on one endpoint pair - cable 0 is its DAW port, cable 1 the MIDI port -
 * and a message sent to the wrong one is accepted in silence. See
 * apps/common/launchpad.
 *
 * ONE DEVICE
 *
 * The firmware is built with CONFIG_USB_HOST_HUBS_SUPPORTED off, so what is
 * plugged into the socket is what there is. That is not a limitation this
 * header is hiding - a hub needs its own enumeration and a device address per
 * port, which is a different piece of work - it is why there is no device
 * handle anywhere below. The functions all mean "the device in the socket".
 *
 * POWER
 *
 * VBUS on the USB-A socket is a rail on an IO expander, NEOS_FEAT_USB_5V, and
 * it is off on a tablet whose owner has never asked for it. neos_midi_open()
 * switches it on, because a host stack with no VBUS is a stack that can never
 * see anything, and neos_midi_close() puts it back the way it found it - so an
 * app cannot quietly leave the socket live, and cannot quietly turn off a rail
 * its owner had deliberately switched on either.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes in one USB-MIDI event packet. Every count below is in packets. */
#define NEOS_MIDI_PACKET 4

/** Longest SysEx neos_midi_send_sysex() will frame, F0 and F7 included. */
#define NEOS_MIDI_SYSEX_MAX 512

/**
 * The code index numbers in the low nibble of byte 0, for an app reading what
 * neos_midi_recv() handed it.
 *
 * Not the full set - table 4-1 of the class spec has sixteen - just the ones a
 * control surface sends. They are spec constants and so are safe in a header
 * that apps compile into themselves: unlike a NeOS enum, these cannot be
 * renumbered by anything on this side.
 */
#define NEOS_MIDI_CIN_SYSEX      0x04u  /**< SysEx begins or continues       */
#define NEOS_MIDI_CIN_SYSEX_END1 0x05u  /**< ...ends, 1 byte in this packet  */
#define NEOS_MIDI_CIN_SYSEX_END2 0x06u  /**< ...ends, 2 bytes                */
#define NEOS_MIDI_CIN_SYSEX_END3 0x07u  /**< ...ends, 3 bytes                */
#define NEOS_MIDI_CIN_NOTE_OFF   0x08u
#define NEOS_MIDI_CIN_NOTE_ON    0x09u
#define NEOS_MIDI_CIN_CC         0x0Bu  /**< control change                  */

/** Cable number and code index number out of byte 0 of an event packet. */
#define NEOS_MIDI_CABLE(b0) ((uint8_t)((b0) >> 4))
#define NEOS_MIDI_CIN(b0)   ((uint8_t)((b0) & 0x0Fu))

typedef enum {
    NEOS_MIDI_OFF = 0,  /**< the stack is not running                        */
    NEOS_MIDI_IDLE,     /**< running; nothing in the socket                  */
    NEOS_MIDI_OTHER,    /**< something in the socket, but it is not MIDI     */
    NEOS_MIDI_READY,    /**< a MIDI device, endpoints open                   */
} neos_midi_state_t;

/** How many characters of a USB string descriptor are kept, NUL included. */
#define NEOS_MIDI_NAME 32

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint16_t in_mps;        /**< bulk IN max packet size, 64 on a full-speed device */
    uint16_t out_mps;
    uint8_t  addr;          /**< the address enumeration gave it */
    /**
     * Embedded MIDI jacks on the OUT endpoint, which is how many cables the
     * device listens on. 1 unless its class-specific endpoint descriptor says
     * otherwise - a Launchpad Mini MK3 says 2.
     */
    uint8_t  cables_out;
    uint8_t  cables_in;
    uint8_t  reserved;
    /**
     * Bumped every time a device is opened. An app that wants to know a cable
     * was pulled and pushed back in while it was not looking compares this,
     * not the state: unplug and replug between two polls looks identical to
     * never having moved.
     */
    uint32_t seq;
    char     product[NEOS_MIDI_NAME];
    char     vendor[NEOS_MIDI_NAME];
} neos_midi_dev_t;

/**
 * Power the socket, install the host stack, and start watching for a device.
 *
 * Returns false if the stack would not install, which is the one failure worth
 * distinguishing: nothing plugged in is not an error, it is NEOS_MIDI_IDLE.
 * Calling it twice is harmless and returns true.
 *
 * Costs about 6 KB of heap and one task while it is open.
 */
bool neos_midi_open(void);

/**
 * Close the device, uninstall the stack, and put the 5 V rail back.
 *
 * Safe to call when nothing is open. An app that forgets is not a leak that
 * survives it - the app runner calls this on the way out - but it is a socket
 * that stays powered until then.
 */
void neos_midi_close(void);

/** Where things stand. Cheap: answered from memory, not from the bus. */
neos_midi_state_t neos_midi_state(void);

/** "off", "idle", "not MIDI", "ready". */
const char *neos_midi_state_name(neos_midi_state_t s);

/**
 * Describe the device in the socket. False, and @p out untouched, unless the
 * state is NEOS_MIDI_READY.
 */
bool neos_midi_device(neos_midi_dev_t *out);

/**
 * Queue @p n event packets - 4 * @p n bytes - for the device.
 *
 * All or nothing: if the queue has no room for the whole lot, nothing is
 * queued and this returns false. That is not tidiness. Half of a SysEx is not
 * a short message, it is a device left waiting for an F7 that is never coming,
 * and the next message it does get is read as the tail of this one.
 *
 * Returns as soon as the packets are in the queue; the transfer happens on the
 * stack's own task. neos_midi_drain() is how to wait for it.
 */
bool neos_midi_send(const uint8_t *packets, int n);

/**
 * Frame a complete SysEx onto @p cable and queue it.
 *
 * @p data runs from F0 to F7 inclusive and every byte between them must have
 * bit 7 clear - this does not check, because a caller that has built a message
 * with a high bit in it has a bug that a rejection here would only move.
 *
 * All or nothing, for the reason neos_midi_send() gives.
 */
bool neos_midi_send_sysex(uint8_t cable, const uint8_t *data, int len);

/**
 * Take up to @p max received event packets, oldest first.
 *
 * Returns how many were written to @p packets, which is 0 when there is
 * nothing waiting. Never blocks.
 */
int neos_midi_recv(uint8_t *packets, int max);

/** Packets queued for transmission and not yet on the wire. */
int neos_midi_pending(void);

/**
 * Wait until the queue has drained, or @p ms has passed.
 *
 * True if it drained. Worth doing before neos_midi_close(), which cancels
 * whatever is still in flight - otherwise the last thing an app sent is the
 * one message the device never sees.
 */
bool neos_midi_drain(uint32_t ms);

/**
 * Packets dropped because the receive queue was full, since the stack opened.
 *
 * A device sends whether anyone is reading or not, and the alternative to
 * dropping is stalling the endpoint, which loses the same data and upsets the
 * device as well. An app that sees this climbing is not polling
 * neos_midi_recv() often enough.
 */
uint32_t neos_midi_overruns(void);

#ifdef __cplusplus
}
#endif
