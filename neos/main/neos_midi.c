/*
 * USB MIDI host. See neos_midi.h for what an app sees and why it is here at
 * all; this is the driver behind it.
 *
 * WHAT THE IDF GIVES US AND WHAT IT DOES NOT
 *
 * The host library handles the controller, enumeration and the four transfer
 * types. On top of it the IDF ships class drivers for CDC-ACM, HID, MSC and
 * UVC - and none for MIDI, which is why this file exists. It is a small class
 * driver: claim the interface whose descriptor says audio class, MIDIStreaming
 * subclass, find its bulk pair, and pump 4-byte packets across them. The whole
 * of USB-MIDI 1.0 that matters on the wire is that packet and the SysEx
 * framing in neos_midi_send_sysex().
 *
 * ONE TASK OWNS EVERY USB CALL
 *
 * Nothing below calls into usb_host_* from an app's thread. The stack is
 * installed, pumped, torn down, and every transfer submitted, from midi_task;
 * transfer callbacks run inside usb_host_client_handle_events(), which is that
 * same task. So the device can appear, vanish and be reaped without anything
 * needing to be atomic with respect to the USB state machine.
 *
 * What does cross threads is the two packet rings, and they are the only thing
 * the mutex covers. An app queueing a frame of LED data is memcpy under a lock
 * and a wake-up; it never waits for the wire.
 *
 * WHY A RING AND NOT THE CALLER'S BUFFER
 *
 * A full 9x9 Launchpad refresh is a 250-odd byte SysEx, which is 84 event
 * packets and - on a full-speed device, which every Launchpad is - four 64-byte
 * bulk transfers. Making the caller hold its buffer alive across all four and
 * poll for completion is how the USB state machine ends up in the application.
 * The ring lets an app queue a whole message in one call and forget it.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

#include "neos_midi.h"
#include "neos_sys.h"

static const char *TAG = "neos_midi";

/* ------------------------------------------------------------------ */
/* USB-MIDI 1.0 constants                                              */
/* ------------------------------------------------------------------ */

/*
 * The interface lives under the audio class; the subclass is what identifies
 * it. There is no separate MIDI class code - a MIDI device is an audio device
 * as far as bInterfaceClass is concerned, which is why a driver that matched
 * on class alone would also claim a USB sound card.
 */
#define MIDI_INTF_CLASS     0x01U   /* USB_CLASS_AUDIO */
#define MIDI_INTF_SUBCLASS  0x03U   /* MIDISTREAMING */

/* Class-specific endpoint descriptor, and its one subtype. */
#define MIDI_CS_ENDPOINT    0x25U
#define MIDI_MS_GENERAL     0x01U

/* Code index numbers. The full set is table 4-1 of the class spec. */
#define MIDI_CIN_SYSEX      0x04U   /* SysEx begins or continues  */
#define MIDI_CIN_SYSEX_1    0x05U   /* ...ends, 1 byte in packet  */
#define MIDI_CIN_SYSEX_2    0x06U   /* ...ends, 2 bytes           */
#define MIDI_CIN_SYSEX_3    0x07U   /* ...ends, 3 bytes           */

/* ------------------------------------------------------------------ */
/* Sizing                                                              */
/* ------------------------------------------------------------------ */

/*
 * The transmit ring holds four full-grid Launchpad refreshes, so an app can
 * queue a frame without first checking whether the last one has drained. The
 * receive ring is smaller on purpose: a surface sends a packet per press, and
 * an app far enough behind to fill 128 of them has stopped polling, which is
 * a bug that a bigger ring hides for another second and a half.
 */
#define TX_PACKETS  384
#define RX_PACKETS  128

/*
 * One transfer buffer. 512 bytes is a high-speed bulk max packet, and eight
 * full-speed ones - every Launchpad enumerates at full speed even on this
 * port, so in practice this is how many 64-byte packets the driver is willing
 * to have in flight at once rather than a single USB transaction.
 */
#define XFER_BYTES  512

#define PKT NEOS_MIDI_PACKET

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static struct {
    /*
     * volatile where an app's thread and midi_task both look. Nothing here is
     * a lock - the rings have one - these are the four one-word facts the two
     * sides exchange, and the compiler must not cache any of them across a
     * delay loop.
     */
    volatile bool            running;    /* midi_task should keep going      */
    TaskHandle_t volatile    task;

    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    uint8_t                  intf;       /* bInterfaceNumber, when claimed   */
    bool                     claimed;
    bool                     gone;       /* DEV_GONE seen; reap when idle    */

    usb_transfer_t          *in;
    usb_transfer_t          *out;
    bool                     in_busy;
    volatile bool            out_busy;

    volatile neos_midi_state_t state;
    neos_midi_dev_t          info;
    uint32_t                 seq;
    uint32_t                 overruns;

    /* The rail, as it was before open() touched it, and whether we did. */
    bool                     rail_was_on;
    bool                     rail_ours;

    SemaphoreHandle_t        lock;

    uint8_t                  tx[TX_PACKETS * PKT];
    uint16_t                 tx_rd;       /* index of the oldest packet      */
    uint16_t                 tx_n;        /* packets queued                  */

    uint8_t                  rx[RX_PACKETS * PKT];
    uint16_t                 rx_rd;
    uint16_t                 rx_n;
} s;

#define LOCK()   xSemaphoreTake(s.lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s.lock)

/* ------------------------------------------------------------------ */
/* The rings                                                           */
/* ------------------------------------------------------------------ */

/* Both are called with the lock held. */

static void tx_put(const uint8_t *packets, int n)
{
    for (int i = 0; i < n; i++) {
        const uint16_t at = (uint16_t)((s.tx_rd + s.tx_n) % TX_PACKETS);
        memcpy(&s.tx[at * PKT], &packets[i * PKT], PKT);
        s.tx_n++;
    }
}

static void rx_put(const uint8_t *packet)
{
    if (s.rx_n == RX_PACKETS) {
        s.overruns++;
        return;
    }
    const uint16_t at = (uint16_t)((s.rx_rd + s.rx_n) % RX_PACKETS);
    memcpy(&s.rx[at * PKT], packet, PKT);
    s.rx_n++;
}

/* ------------------------------------------------------------------ */
/* Transfers                                                           */
/* ------------------------------------------------------------------ */

static void in_done(usb_transfer_t *t);

static void in_submit(void)
{
    if (!s.in || s.in_busy || s.state != NEOS_MIDI_READY) {
        return;
    }

    /*
     * A bulk IN transfer must ask for a whole number of max packets, so the
     * request is rounded DOWN to the endpoint's size rather than up to ours -
     * a device that sends one 64-byte packet into a 512-byte request returns
     * short, which is fine, but a request that is not a multiple of the
     * endpoint size is rejected outright.
     */
    const int mps = s.info.in_mps ? s.info.in_mps : 64;
    s.in->num_bytes = (XFER_BYTES / mps) * mps;

    s.in_busy = true;
    if (usb_host_transfer_submit(s.in) != ESP_OK) {
        s.in_busy = false;
    }
}

static void in_done(usb_transfer_t *t)
{
    s.in_busy = false;

    switch (t->status) {
    case USB_TRANSFER_STATUS_COMPLETED: {
        const int n = t->actual_num_bytes / PKT;
        LOCK();
        for (int i = 0; i < n; i++) {
            /*
             * A packet of four zero bytes is padding, not a message: the class
             * spec lets a device fill out a transfer with them, and a Launchpad
             * does. Passing them up would have every app filter them again.
             */
            const uint8_t *p = &t->data_buffer[i * PKT];
            if (p[0] || p[1] || p[2] || p[3]) {
                rx_put(p);
            }
        }
        UNLOCK();
        in_submit();
        break;
    }
    case USB_TRANSFER_STATUS_STALL:
        /* Clearing a halt is a control transfer, and we own the control pipe
           here, so it can be done from the callback's own context. */
        ESP_LOGW(TAG, "IN endpoint stalled");
        usb_host_endpoint_clear(s.dev, t->bEndpointAddress);
        in_submit();
        break;
    default:
        /* CANCELED or NO_DEVICE: the cable is out, or we are tearing down.
           Either way there is nothing to resubmit onto. */
        break;
    }
}

static void out_done(usb_transfer_t *t)
{
    s.out_busy = false;

    if (t->status == USB_TRANSFER_STATUS_STALL) {
        ESP_LOGW(TAG, "OUT endpoint stalled");
        usb_host_endpoint_clear(s.dev, t->bEndpointAddress);
    }
}

/*
 * Move whatever is queued onto the wire, one transfer at a time.
 *
 * Called from midi_task after every round of events, which is also just after
 * out_done() has run, so a long message walks out of the ring a transfer per
 * loop with no timer involved.
 */
static void tx_pump(void)
{
    if (!s.out || s.out_busy || s.state != NEOS_MIDI_READY) {
        return;
    }

    LOCK();
    int n = s.tx_n;
    if (n > XFER_BYTES / PKT) {
        n = XFER_BYTES / PKT;
    }
    for (int i = 0; i < n; i++) {
        memcpy(&s.out->data_buffer[i * PKT], &s.tx[s.tx_rd * PKT], PKT);
        s.tx_rd = (uint16_t)((s.tx_rd + 1) % TX_PACKETS);
        s.tx_n--;
    }
    UNLOCK();

    if (n == 0) {
        return;
    }

    s.out->num_bytes = n * PKT;
    s.out_busy = true;
    if (usb_host_transfer_submit(s.out) != ESP_OK) {
        s.out_busy = false;
        ESP_LOGW(TAG, "%d packets dropped: OUT transfer refused", n);
    }
}

/* ------------------------------------------------------------------ */
/* Descriptors                                                         */
/* ------------------------------------------------------------------ */

/*
 * A USB string descriptor is UTF-16LE and this keeps the low byte of each
 * unit, which is ASCII for every device name anyone has ever shipped and
 * mojibake for the rest. The alternative is a UTF-8 conversion for a field
 * that exists so a panel can print "Launchpad Mini MK3".
 */
static void str_copy(char *dst, size_t cap, const usb_str_desc_t *src)
{
    dst[0] = '\0';
    if (!src || src->bLength < 2) {
        return;
    }

    const size_t units = (size_t)(src->bLength - 2) / 2;
    size_t at = 0;
    for (size_t i = 0; i < units && at + 1 < cap; i++) {
        const uint16_t u = src->wData[i];
        dst[at++] = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
    }
    dst[at] = '\0';
}

/*
 * Find the MIDIStreaming interface and its bulk pair.
 *
 * Alternate setting 0 only. A MIDI device is allowed to have alternates and in
 * practice none does; claiming alt 0 of the first matching interface is what
 * every host on a desk does too.
 */
static bool find_endpoints(const usb_config_desc_t *cfg)
{
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    int off = 0;

    /*
     * Note on the walk: usb_parse_next_descriptor() stops one descriptor
     * early - its bounds test is `offset + bLength >= wTotalLength` where a
     * `>` is what the arithmetic wants - so the very last descriptor in a
     * configuration is never returned. That is always a trailing
     * class-specific endpoint descriptor on the devices this sees, so what it
     * costs is a jack count, which is why cables_in/out are given a sane 1
     * before the walk rather than after it.
     */

    /* Walk to the first audio/MIDIStreaming interface descriptor. */
    const usb_intf_desc_t *ms = NULL;
    while ((d = usb_parse_next_descriptor_of_type(
                d, cfg->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) != NULL) {
        const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
        if (i->bInterfaceClass == MIDI_INTF_CLASS &&
            i->bInterfaceSubClass == MIDI_INTF_SUBCLASS &&
            i->bAlternateSetting == 0) {
            ms = i;
            break;
        }
    }
    if (!ms) {
        return false;
    }

    s.intf = ms->bInterfaceNumber;
    s.info.cables_in = 1;
    s.info.cables_out = 1;

    /*
     * From here, walk descriptors in order until the next interface. Each bulk
     * endpoint is followed by its class-specific descriptor, whose
     * bNumEmbMIDIJack is how many cables that endpoint carries - the only
     * place a device says it has more than one MIDI port. So the walk has to
     * be sequential rather than a search by type: which endpoint the jack
     * count belongs to is decided by what came before it.
     */
    uint8_t last_ep_dir_in = 0;
    bool    have_in = false, have_out = false;

    while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off)) != NULL) {
        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            break;
        }

        if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)d;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
                USB_BM_ATTRIBUTES_XFER_BULK) {
                continue;
            }
            last_ep_dir_in = (uint8_t)((ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0);

            if (last_ep_dir_in && !have_in) {
                s.in->bEndpointAddress = ep->bEndpointAddress;
                s.info.in_mps = USB_EP_DESC_GET_MPS(ep);
                have_in = true;
            } else if (!last_ep_dir_in && !have_out) {
                s.out->bEndpointAddress = ep->bEndpointAddress;
                s.info.out_mps = USB_EP_DESC_GET_MPS(ep);
                have_out = true;
            }
            continue;
        }

        if (d->bDescriptorType == MIDI_CS_ENDPOINT && d->bLength >= 4) {
            const uint8_t *raw = (const uint8_t *)d;
            if (raw[2] == MIDI_MS_GENERAL && raw[3] > 0) {
                if (last_ep_dir_in) {
                    s.info.cables_in = raw[3];
                } else {
                    s.info.cables_out = raw[3];
                }
            }
        }
    }

    return have_in && have_out;
}

/* ------------------------------------------------------------------ */
/* Attach and detach                                                   */
/* ------------------------------------------------------------------ */

static void device_open(uint8_t addr)
{
    if (s.dev) {
        /* No hub support, so this should not happen. If it ever does, the
           first device keeps the port rather than being silently replaced. */
        ESP_LOGW(TAG, "a second device at %u, ignored", addr);
        return;
    }

    if (usb_host_device_open(s.client, addr, &s.dev) != ESP_OK) {
        ESP_LOGW(TAG, "could not open device %u", addr);
        return;
    }

    memset(&s.info, 0, sizeof(s.info));
    s.info.addr = addr;

    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(s.dev, &dd) == ESP_OK) {
        s.info.vid = dd->idVendor;
        s.info.pid = dd->idProduct;
    }

    usb_device_info_t di;
    if (usb_host_device_info(s.dev, &di) == ESP_OK) {
        str_copy(s.info.vendor, sizeof(s.info.vendor), di.str_desc_manufacturer);
        str_copy(s.info.product, sizeof(s.info.product), di.str_desc_product);
    }

    const usb_config_desc_t *cfg = NULL;
    if (usb_host_get_active_config_descriptor(s.dev, &cfg) != ESP_OK ||
        !find_endpoints(cfg)) {
        ESP_LOGI(TAG, "%04x:%04x is not a MIDI device", s.info.vid, s.info.pid);
        s.state = NEOS_MIDI_OTHER;
        return;
    }

    if (usb_host_interface_claim(s.client, s.dev, s.intf, 0) != ESP_OK) {
        ESP_LOGW(TAG, "interface %u refused", s.intf);
        s.state = NEOS_MIDI_OTHER;
        return;
    }
    s.claimed = true;

    s.in->device_handle  = s.dev;
    s.out->device_handle = s.dev;

    /* A fresh device: whatever the last one left in the rings is not its. */
    LOCK();
    s.tx_rd = s.tx_n = 0;
    s.rx_rd = s.rx_n = 0;
    UNLOCK();

    s.info.seq = ++s.seq;
    s.state    = NEOS_MIDI_READY;

    ESP_LOGI(TAG, "%04x:%04x %s%s%s ready - in ep %02x/%u x%u, out ep %02x/%u x%u",
             s.info.vid, s.info.pid,
             s.info.vendor, s.info.vendor[0] ? " " : "", s.info.product,
             s.in->bEndpointAddress, s.info.in_mps, s.info.cables_in,
             s.out->bEndpointAddress, s.info.out_mps, s.info.cables_out);

    in_submit();
}

/*
 * Let go of a device that has gone, once nothing is still in flight on it.
 *
 * The DEV_GONE message and the cancellation callbacks for the transfers that
 * were outstanding arrive in no guaranteed order, and usb_host_device_close()
 * refuses while a transfer is still owned by the stack. So the event only sets
 * a flag and this runs from the loop until both transfers are back.
 */
static void device_reap(void)
{
    if (s.in_busy || s.out_busy) {
        return;
    }

    if (s.claimed) {
        usb_host_interface_release(s.client, s.dev, s.intf);
        s.claimed = false;
    }
    if (s.dev) {
        usb_host_device_close(s.client, s.dev);
        s.dev = NULL;
    }
    if (s.in) {
        s.in->device_handle = NULL;
    }
    if (s.out) {
        s.out->device_handle = NULL;
    }

    LOCK();
    s.tx_rd = s.tx_n = 0;
    s.rx_rd = s.rx_n = 0;
    UNLOCK();

    memset(&s.info, 0, sizeof(s.info));
    s.gone  = false;
    s.state = NEOS_MIDI_IDLE;
    ESP_LOGI(TAG, "device gone");
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        device_open(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl == s.dev) {
            /*
             * The state goes back now and the handles are let go later, when
             * device_reap() can. Leaving it at READY until then would be a
             * window in which an app queues packets for a device that is on
             * the desk, and in which in_submit() parks another transfer on it.
             */
            s.gone  = true;
            s.state = NEOS_MIDI_IDLE;
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The task                                                            */
/* ------------------------------------------------------------------ */

static void stack_teardown(void)
{
    if (s.dev) {
        s.gone = true;
        /*
         * Cancelling is what brings the callbacks in. A device still in the
         * socket has an IN transfer parked on it that will otherwise sit there
         * until something arrives, and usb_host_device_close() will not have
         * it.
         */
        if (s.in_busy) {
            usb_host_endpoint_halt(s.dev, s.in->bEndpointAddress);
            usb_host_endpoint_flush(s.dev, s.in->bEndpointAddress);
        }
        if (s.out_busy) {
            usb_host_endpoint_halt(s.dev, s.out->bEndpointAddress);
            usb_host_endpoint_flush(s.dev, s.out->bEndpointAddress);
        }
        for (int i = 0; i < 50 && s.dev; i++) {
            usb_host_client_handle_events(s.client, pdMS_TO_TICKS(10));
            device_reap();
        }
    }

    if (s.client) {
        usb_host_client_deregister(s.client);
        s.client = NULL;
    }

    usb_host_device_free_all();
    for (int i = 0; i < 100; i++) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(10), &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }

    if (s.in) {
        usb_host_transfer_free(s.in);
        s.in = NULL;
    }
    if (s.out) {
        usb_host_transfer_free(s.out);
        s.out = NULL;
    }

    usb_host_uninstall();
}

static void midi_task(void *arg)
{
    (void)arg;

    while (s.running) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);

        /* Ten milliseconds is the idle cost of having the stack open; a send
           cuts it short with usb_host_client_unblock(). */
        usb_host_client_handle_events(s.client, pdMS_TO_TICKS(10));

        if (s.gone) {
            device_reap();
        }
        tx_pump();
        in_submit();
    }

    stack_teardown();

    s.state = NEOS_MIDI_OFF;
    s.task  = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* The app-facing half                                                 */
/* ------------------------------------------------------------------ */

bool neos_midi_open(void)
{
    if (s.task) {
        return true;
    }

    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return false;
        }
    }

    /*
     * The rail first. The host stack powers the root port at install time and
     * then waits for a connect, so VBUS has to be there before it looks - and
     * a tablet that has never been asked for USB power has this off.
     */
    s.rail_was_on = neos_feature(NEOS_FEAT_USB_5V);
    s.rail_ours   = !s.rail_was_on;
    if (s.rail_ours && !neos_feature_set(NEOS_FEAT_USB_5V, true)) {
        ESP_LOGW(TAG, "the expander would not switch USB 5 V on");
        s.rail_ours = false;
    }

    const usb_host_config_t hcfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        goto fail_rail;
    }

    const usb_host_client_config_t ccfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event,
            .callback_arg          = NULL,
        },
    };
    err = usb_host_client_register(&ccfg, &s.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(err));
        goto fail_host;
    }

    if (usb_host_transfer_alloc(XFER_BYTES, 0, &s.in) != ESP_OK ||
        usb_host_transfer_alloc(XFER_BYTES, 0, &s.out) != ESP_OK) {
        ESP_LOGE(TAG, "no memory for transfers");
        goto fail_client;
    }
    s.in->callback  = in_done;
    s.out->callback = out_done;
    s.in->timeout_ms  = 0;      /* bulk IN waits for the device, forever */
    s.out->timeout_ms = 1000;

    s.overruns = 0;
    s.state    = NEOS_MIDI_IDLE;
    s.running  = true;

    /* Through a local, because s.task is volatile - it is how this side sees
       the task exit - and xTaskCreate() wants a plain TaskHandle_t *. */
    TaskHandle_t h = NULL;
    if (xTaskCreate(midi_task, "usbmidi", 4096, NULL, 5, &h) != pdPASS) {
        ESP_LOGE(TAG, "no task");
        s.running = false;
        goto fail_xfers;
    }
    s.task = h;

    ESP_LOGI(TAG, "open");
    return true;

fail_xfers:
    if (s.in)  { usb_host_transfer_free(s.in);  s.in = NULL; }
    if (s.out) { usb_host_transfer_free(s.out); s.out = NULL; }
fail_client:
    usb_host_client_deregister(s.client);
    s.client = NULL;
fail_host:
    usb_host_uninstall();
fail_rail:
    if (s.rail_ours) {
        neos_feature_set(NEOS_FEAT_USB_5V, false);
        s.rail_ours = false;
    }
    s.state = NEOS_MIDI_OFF;
    return false;
}

void neos_midi_close(void)
{
    if (!s.task) {
        return;
    }

    /*
     * No usb_host_client_unblock() here, although it would shave a few
     * milliseconds off. The task deregisters the client as part of its own
     * teardown, so a handle read from this side races with being freed on
     * that one - and the loop's event timeout means the flag is noticed
     * within ten milliseconds regardless.
     *
     * The task does the teardown itself - see the note at the top about every
     * USB call belonging to it - and clears s.task on the way out.
     */
    s.running = false;
    for (int i = 0; i < 200 && s.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s.task) {
        ESP_LOGE(TAG, "the USB task did not stop");
        return;
    }

    if (s.rail_ours) {
        neos_feature_set(NEOS_FEAT_USB_5V, s.rail_was_on);
        s.rail_ours = false;
    }

    ESP_LOGI(TAG, "closed");
}

neos_midi_state_t neos_midi_state(void)
{
    return s.state;
}

const char *neos_midi_state_name(neos_midi_state_t st)
{
    switch (st) {
    case NEOS_MIDI_OFF:   return "off";
    case NEOS_MIDI_IDLE:  return "idle";
    case NEOS_MIDI_OTHER: return "not MIDI";
    case NEOS_MIDI_READY: return "ready";
    default:              return "?";
    }
}

bool neos_midi_device(neos_midi_dev_t *out)
{
    if (!out || s.state != NEOS_MIDI_READY) {
        return false;
    }
    *out = s.info;
    return true;
}

bool neos_midi_send(const uint8_t *packets, int n)
{
    if (!packets || n <= 0 || s.state != NEOS_MIDI_READY) {
        return false;
    }

    LOCK();
    const bool room = (TX_PACKETS - s.tx_n) >= n;
    if (room) {
        tx_put(packets, n);
    }
    UNLOCK();

    if (!room) {
        return false;
    }

    usb_host_client_unblock(s.client);
    return true;
}

/*
 * SysEx into event packets.
 *
 * Three bytes per packet with CIN 4 while more follows, and a final packet
 * carrying one, two or three bytes with CIN 5, 6 or 7. The message includes
 * its own F0 and F7, so a 6-byte inquiry is two packets: {4, F0, 7E, 7F} then
 * {6, 06, 01, F7}... except that 6 bytes divides exactly by 3, so it is
 * {4, F0, 7E, 7F} then {7, 06, 01, F7}. The tail CIN is the length of the last
 * group, which is what the switch below computes.
 */
bool neos_midi_send_sysex(uint8_t cable, const uint8_t *data, int len)
{
    if (!data || len < 2 || len > NEOS_MIDI_SYSEX_MAX ||
        s.state != NEOS_MIDI_READY) {
        return false;
    }

    uint8_t packets[((NEOS_MIDI_SYSEX_MAX + 2) / 3) * PKT];
    const uint8_t hi = (uint8_t)((cable & 0x0FU) << 4);
    int at = 0;
    int n  = 0;

    while (len - at > 3) {
        packets[n * PKT + 0] = (uint8_t)(hi | MIDI_CIN_SYSEX);
        packets[n * PKT + 1] = data[at + 0];
        packets[n * PKT + 2] = data[at + 1];
        packets[n * PKT + 3] = data[at + 2];
        at += 3;
        n++;
    }

    const int tail = len - at;
    const uint8_t cin = (tail == 1) ? MIDI_CIN_SYSEX_1
                      : (tail == 2) ? MIDI_CIN_SYSEX_2
                                    : MIDI_CIN_SYSEX_3;

    packets[n * PKT + 0] = (uint8_t)(hi | cin);
    packets[n * PKT + 1] = data[at];
    packets[n * PKT + 2] = (tail > 1) ? data[at + 1] : 0;
    packets[n * PKT + 3] = (tail > 2) ? data[at + 2] : 0;
    n++;

    return neos_midi_send(packets, n);
}

int neos_midi_recv(uint8_t *packets, int max)
{
    if (!packets || max <= 0) {
        return 0;
    }

    LOCK();
    int n = s.rx_n;
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        memcpy(&packets[i * PKT], &s.rx[s.rx_rd * PKT], PKT);
        s.rx_rd = (uint16_t)((s.rx_rd + 1) % RX_PACKETS);
        s.rx_n--;
    }
    UNLOCK();

    return n;
}

int neos_midi_pending(void)
{
    if (!s.lock) {
        return 0;
    }

    LOCK();
    const int n = s.tx_n;
    UNLOCK();

    return n;
}

bool neos_midi_drain(uint32_t ms)
{
    for (uint32_t waited = 0;; waited += 5) {
        if (neos_midi_pending() == 0 && !s.out_busy) {
            return true;
        }
        if (waited >= ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

uint32_t neos_midi_overruns(void)
{
    return s.overruns;
}
