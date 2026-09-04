/*
 * Screen capture over the console.
 *
 * Photographing the panel is how a screenshot of this thing got taken until
 * now, and a photograph of a 720x1280 display is not something you can put in
 * a README or diff against last week's. The pixels are already in RAM, and the
 * same USB link that carries an upload can carry them back:
 *
 *     @NEOSCAP\n                                     (send the whole screen)
 *     @NEOSCAP <first> <count>\n                     (send just those rows)
 *
 *     @NEOSCAP-HDR <w> <h> <rot>\n
 *     @NEOSCAP-D <row> <crc32> <base64 of the RLE-coded row>\n   (one per row)
 *     @NEOSCAP-END <rows sent>\n
 *
 * with @NEOSCAP-ERR <reason> instead if there is nothing to send.
 *
 * What is captured is the logical back buffer - what apps draw into, system
 * bar and any panel on top included, already rotated - so the image arrives
 * the way up it was being looked at. Nothing is locked while it is read: a
 * frame drawn during the capture shows up half-drawn, which is what a
 * screenshot of a running app is anyway, and holding the screen lock for the
 * length of a transfer would stall the UI instead.
 *
 * The rest of this is about sharing a console with everything else that writes
 * to one, which is the whole difficulty.
 *
 * A line cannot be written atomically, and no amount of care here makes it so.
 * Each FreeRTOS task has its own stdout, so the per-FILE lock that makes printf
 * atomic within a task buys nothing between them; an app's printf resolves to
 * the loader's libc rather than to anything NeOS owns, so it cannot be gated
 * either. A log line or an app's frame report lands in the middle of a row and
 * splits it across two lines, and that is a thing that happens, not a thing to
 * design around.
 *
 * So rows are numbered and checksummed, and the host asks again for the ones
 * that did not survive. A retried row is read from the screen as it is then,
 * not from a snapshot: a capture is already smeared across the time it takes,
 * and a megabyte of PSRAM to make one row of it consistent would be paying for
 * the wrong thing.
 *
 * Two things narrow the window rather than closing it. The payload goes
 * straight to the USB endpoint instead of through printf, which keeps it off
 * the UART the console is also duplicated to - that UART runs at 115200 and
 * was the whole transfer time. And ESP_LOG is muted for the duration, which
 * removes the frequent case (ngl logs a line per flush) if not the general
 * one; how many lines that cost is reported afterwards.
 *
 * Base64 because the port is also carrying text and the host has to be able to
 * tell one from the other; RLE first because a UI screen is mostly flat fill,
 * which turns 1.8 MB into tens of KB.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"

#include "neos_screencap.h"

static const char *TAG = "screencap";

/* Long enough to ride out a host that is busy, short enough that a host that
   has gone away does not hold the console task for a minute. */
#define EMIT_TIMEOUT_MS 2000

/* Half of neos_upload.c's TX_BUF, so a piece always fits the transmit ring
   with room to spare. */
#define EMIT_CHUNK 128

/* "@NEOSCAP-D 1279 deadbeef " and room to spare. */
#define ROW_PREFIX_MAX 48

/*
 * The token stream, one byte of tag then payload:
 *
 *   0x00..0x7F  literal, (tag + 1) pixels follow, little-endian RGB565
 *   0x80..0xFF  run, one pixel follows, repeated (tag - 0x80 + 2) times
 *
 * Runs cap at 129 rather than going to a wider count because the whole point
 * of a byte tag is that the decoder is six lines long; a flat 1280-pixel row
 * costs thirty bytes at this cap, which is already nothing.
 */
#define RLE_LIT_MAX 128
#define RLE_RUN_MAX 129

/** Worst case: every pixel its own literal, one tag per RLE_LIT_MAX of them. */
static size_t rle_bound(int w)
{
    return (size_t)w * 2 + (size_t)w / RLE_LIT_MAX + 2;
}

static size_t rle_row(const ngl_color_t *px, int n, uint8_t *out)
{
    size_t o = 0;

    for (int i = 0; i < n; ) {
        int run = 1;
        while (i + run < n && px[i + run] == px[i] && run < RLE_RUN_MAX) {
            run++;
        }
        if (run >= 2) {
            out[o++] = (uint8_t)(0x80 + (run - 2));
            out[o++] = (uint8_t)(px[i] & 0xFF);
            out[o++] = (uint8_t)(px[i] >> 8);
            i += run;
            continue;
        }

        /* A literal reaches to wherever the next run starts: extending it past
           a repeated pair would cost more than closing it and coding the run. */
        int lit = 1;
        while (i + lit < n && lit < RLE_LIT_MAX &&
               !(i + lit + 1 < n && px[i + lit] == px[i + lit + 1])) {
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1);
        for (int k = 0; k < lit; k++) {
            out[o++] = (uint8_t)(px[i + k] & 0xFF);
            out[o++] = (uint8_t)(px[i + k] >> 8);
        }
        i += lit;
    }
    return o;
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Standard base64 with padding. Returns the length written; no NUL. */
static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    size_t i = 0;

    for (; i + 3 <= n; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
    }
    if (i < n) {
        const bool two = (n - i) == 2;
        const uint32_t v = ((uint32_t)in[i] << 16) | (two ? ((uint32_t)in[i + 1] << 8) : 0);
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = two ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

/**
 * Straight to the endpoint, not through printf: the console is duplicated onto
 * UART0 at 115200, and a screen's worth of base64 down that is seconds of
 * blocking for output nobody reads.
 *
 * In pieces, because the driver's transmit path is a ring buffer and an item
 * larger than the whole ring is not "wait for room", it is refused on the
 * spot - a 3 KB row handed over in one call fails instantly and looks exactly
 * like a host that stopped reading. EMIT_CHUNK stays under the ring TX_BUF
 * gives it, and the USB endpoint is 64 bytes anyway, so nothing is lost by it.
 *
 * False means the host really did stop draining, which is how a capture ends
 * when the cable comes out.
 */
static bool emit(const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        const size_t want = (len - done > EMIT_CHUNK) ? EMIT_CHUNK : len - done;
        const int n = usb_serial_jtag_write_bytes(buf + done, want,
                                                  pdMS_TO_TICKS(EMIT_TIMEOUT_MS));
        if (n <= 0) {
            return false;
        }
        done += (size_t)n;
    }
    return true;
}

static bool emitf(const char *fmt, ...)
{
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return n > 0 && emit(line, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Muting the log for the duration                                     */
/* ------------------------------------------------------------------ */

static int s_dropped;

static int log_swallow(const char *fmt, va_list ap)
{
    (void)fmt;
    (void)ap;
    s_dropped++;
    return 0;
}

/* ------------------------------------------------------------------ */

void neos_screencap_send(const char *arg)
{
    ngl_surface_t *s = ngl_screen();
    if (!s) {
        emitf("@NEOSCAP-ERR no screen\n");
        return;
    }

    const int w = ngl_surface_w(s);
    const int h = ngl_surface_h(s);
    if (w <= 0 || h <= 0) {
        emitf("@NEOSCAP-ERR screen is %dx%d\n", w, h);
        return;
    }

    /* No argument means the whole screen; the host names a range when it is
       asking again for rows that did not arrive intact. */
    int first = 0;
    int count = h;
    if (!arg || sscanf(arg, "%d %d", &first, &count) != 2) {
        first = 0;
        count = h;
    }
    if (first < 0) {
        first = 0;
    }
    if (count > h - first) {
        count = h - first;
    }
    if (count <= 0) {
        emitf("@NEOSCAP-ERR no rows %d..%d on a %d row screen\n", first, first + count, h);
        return;
    }

    /* Allocated for the transfer rather than kept: a screenshot is a thing
       that happens a few times a day, and this is 6 KB of it. */
    const size_t rle_max = rle_bound(w);
    uint8_t *rle = malloc(rle_max);
    char *line = malloc(ROW_PREFIX_MAX + (rle_max + 2) / 3 * 4 + 2);
    if (!rle || !line) {
        free(rle);
        free(line);
        emitf("@NEOSCAP-ERR out of memory\n");
        return;
    }

    ESP_LOGI(TAG, "sending %dx%d, rows %d..%d", w, h, first, first + count - 1);

    s_dropped = 0;
    const vprintf_like_t saved = esp_log_set_vprintf(log_swallow);

    int sent = 0;
    bool ok = emitf("@NEOSCAP-HDR %d %d %d\n", w, h, (int)ngl_rotation());

    for (int y = first; ok && y < first + count; y++) {
        const ngl_color_t *row = ngl_surface_row(s, (int16_t)y);
        if (!row) {
            break;
        }

        const uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)row,
                                              (size_t)w * sizeof(ngl_color_t));
        const int n = snprintf(line, ROW_PREFIX_MAX, "@NEOSCAP-D %d %08x ", y, (unsigned)crc);
        if (n <= 0 || n >= ROW_PREFIX_MAX) {
            break;
        }

        size_t len = (size_t)n + b64_encode(rle, rle_row(row, w, rle), line + n);
        line[len++] = '\n';

        ok = emit(line, len);
        sent++;

        /* This task is priority 4 and the endpoint will take everything it is
           given; the touch and status tasks still want their turn. */
        taskYIELD();
    }

    if (ok) {
        emitf("@NEOSCAP-END %d\n", sent);
    }

    esp_log_set_vprintf(saved);

    if (!ok) {
        ESP_LOGW(TAG, "gave up after %d rows - the host stopped reading", sent);
    } else if (s_dropped) {
        ESP_LOGI(TAG, "sent %d rows; %d log lines dropped meanwhile", sent, s_dropped);
    }

    free(rle);
    free(line);
}
