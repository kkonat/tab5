/*
 * File upload over the console.
 *
 * Pulling the card out, walking it to a PC and putting it back is the slowest
 * part of changing anything, and it is the one step that has nothing to do
 * with the code. So NeOS listens on the same UART it logs to, and writes what
 * it is sent straight to the card.
 *
 * The wire format is deliberately dull:
 *
 *     @NEOSPUT <path> <size> <crc32>\n
 *     <size bytes, raw>
 *
 * There is a matching @NEOSDEL <path> for taking something off again,
 * which removes a directory and its contents when handed one - deleting an
 * app should not mean naming each of its files.
 *
 * and the reply is one line, @NEOSPUT-OK or @NEOSPUT-ERR. The header is text
 * so it survives being read by a human staring at a terminal, and the magic is
 * distinctive so that a log line can never be mistaken for the start of an
 * upload - both sides of this link are also carrying ESP_LOG output.
 *
 * Each block is requested before it is sent - the device says @NEOSPUT-RDY
 * and the host answers with at most one block. That is flow control: at
 * 921600 baud the wire delivers faster than FATFS commits, and without a
 * brake the receive buffer overruns on whichever block happens to land during
 * a metadata flush. One round trip per 4 KB costs about two percent.
 *
 * The CRC is checked after the file is closed, and a file that fails is
 * deleted rather than left on the card: a half-written app.elf that still
 * loads far enough to crash is much worse than one that is not there.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"

#include "neos_api.h"
#include "neos_status.h"
#include "neos_upload.h"

static const char *TAG = "upload";

#define RX_BUF          16384
#define TX_BUF          256
#define MAGIC           "@NEOSPUT "
#define MAGIC_DEL       "@NEOSDEL "
#define HDR_MAX         192
#define BLOCK           4096
#define BODY_TIMEOUT_MS 5000

/* Big enough for anything that fits in an app directory name plus a filename. */
#define PATH_MAX_REL 128

static uint8_t s_block[BLOCK];

/** One byte, or false on timeout. */
static bool rx_byte(uint8_t *b, int timeout_ms)
{
    return usb_serial_jtag_read_bytes(b, 1, pdMS_TO_TICKS(timeout_ms)) == 1;
}

/**
 * Collect a line, but only keep what follows the magic.
 *
 * The port carries log output as well, so this cannot assume the next line is
 * for it. Anything that does not start with the magic is discarded silently -
 * complaining about it would mean logging about every log line.
 */
static bool read_header(char *out, size_t out_sz, bool *is_delete)
{
    static char line[HDR_MAX];
    static size_t len;

    uint8_t c;
    while (rx_byte(&c, portMAX_DELAY)) {
        if (c == '\n' || c == '\r') {
            line[len] = 0;

            /* Both verbs have the same shape and the same length, so the path
               starts at the same offset either way. */
            const bool put = strncmp(line, MAGIC, sizeof(MAGIC) - 1) == 0;
            const bool del = strncmp(line, MAGIC_DEL, sizeof(MAGIC_DEL) - 1) == 0;
            if (put || del) {
                *is_delete = del;
                strlcpy(out, line + sizeof(MAGIC) - 1, out_sz);
            }
            len = 0;
            if (put || del) {
                return true;
            }
            continue;
        }
        if (len < sizeof(line) - 1) {
            line[len++] = (char)c;
        } else {
            len = 0;        /* overlong: not ours */
        }
    }
    return false;
}

/** mkdir every parent of `path`. The card arrives with no directories on it. */
static void make_parents(const char *path)
{
    char buf[PATH_MAX_REL + sizeof(BSP_SD_MOUNT_POINT) + 2];
    strlcpy(buf, path, sizeof(buf));

    for (char *p = strchr(buf + 1, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s: %s", buf, strerror(errno));
        }
        *p = '/';
    }
}

/** True if `rel` is a plain relative path - no escaping the card. */
static bool path_is_sane(const char *rel)
{
    if (!rel[0] || rel[0] == '/' || strlen(rel) >= PATH_MAX_REL) {
        return false;
    }
    if (strstr(rel, "..")) {
        return false;
    }
    for (const char *p = rel; *p; p++) {
        if (*p < 0x20 || *p == ':' || *p == '\\') {
            return false;
        }
    }
    return true;
}

/**
 * Remove a file, or a directory and everything in it.
 *
 * Deleting an app means deleting its directory: the ELF, the manifest, and
 * whatever else it shipped with. Making the host enumerate that would mean
 * the host has to know what an app is made of, which is exactly the thing
 * the manifest exists to stop.
 */
static bool remove_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0;
    }

    DIR *d = opendir(path);
    if (!d) {
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char child[320];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        remove_tree(child);
    }
    closedir(d);
    return rmdir(path) == 0;
}

static void do_delete(const char *rel)
{
    char full[PATH_MAX_REL + sizeof(BSP_SD_MOUNT_POINT) + 2];
    snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, rel);

    if (!remove_tree(full)) {
        printf("@NEOSPUT-ERR cannot delete %s\n", rel);
        ESP_LOGE(TAG, "delete %s failed: %s", full, strerror(errno));
        return;
    }

    ESP_LOGI(TAG, "deleted %s", full);
    neos_apps_scan();
    printf("@NEOSPUT-OK %s deleted\n", rel);
}

static void receive(const char *rel, long size, uint32_t want_crc)
{
    char full[PATH_MAX_REL + sizeof(BSP_SD_MOUNT_POINT) + 2];
    snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, rel);

    make_parents(full);

    FILE *f = fopen(full, "wb");
    if (!f) {
        printf("@NEOSPUT-ERR cannot create %s\n", rel);
        ESP_LOGE(TAG, "fopen %s: %s", full, strerror(errno));
        return;
    }

    uint32_t crc = 0;
    long left = size;
    bool ok = true;
    int last_pct = -1;

    neos_status_progress(0);

    while (left > 0) {
        const int want = (int)(left < BLOCK ? left : BLOCK);

        /* Ask for exactly what will fit, then take it. */
        printf("@NEOSPUT-RDY %d\n", want);
        fflush(stdout);

        int got = 0;
        while (got < want) {
            const int n = usb_serial_jtag_read_bytes(s_block + got, want - got,
                                                     pdMS_TO_TICKS(BODY_TIMEOUT_MS));
            if (n <= 0) {
                break;
            }
            got += n;
        }
        if (got != want) {
            ESP_LOGE(TAG, "timed out with %ld bytes to go", left);
            ok = false;
            break;
        }
        if (fwrite(s_block, 1, (size_t)got, f) != (size_t)got) {
            ESP_LOGE(TAG, "write failed: %s", strerror(errno));
            ok = false;
            break;
        }
        crc = esp_rom_crc32_le(crc, s_block, (size_t)got);
        left -= got;

        const int pct = (int)(100 - (left * 100 / size));
        if (pct != last_pct) {
            last_pct = pct;
            neos_status_progress(pct);
        }
    }
    fclose(f);
    neos_status_progress(-1);

    if (!ok) {
        unlink(full);
        printf("@NEOSPUT-ERR transfer failed\n");
        return;
    }
    if (crc != want_crc) {
        unlink(full);
        printf("@NEOSPUT-ERR crc mismatch\n");
        ESP_LOGE(TAG, "crc %08lx, expected %08lx",
                 (unsigned long)crc, (unsigned long)want_crc);
        return;
    }

    ESP_LOGI(TAG, "wrote %s, %ld bytes", full, size);

    /* The registry is what every app list is drawn from, so a new app is not
       really on the tablet until this has run. */
    neos_apps_scan();

    printf("@NEOSPUT-OK %s %ld\n", rel, size);
}

static void upload_task(void *arg)
{
    (void)arg;
    char hdr[HDR_MAX];

    for (;;) {
        bool is_delete = false;
        if (!read_header(hdr, sizeof(hdr), &is_delete)) {
            continue;
        }

        char rel[PATH_MAX_REL] = {0};
        long size = 0;
        unsigned long crc = 0;

        if (is_delete) {
            if (sscanf(hdr, "%127s", rel) != 1 || !path_is_sane(rel)) {
                printf("@NEOSPUT-ERR bad path\n");
                continue;
            }
            if (!bsp_sdcard_get_handle()) {
                printf("@NEOSPUT-ERR no card\n");
                continue;
            }
            do_delete(rel);
            continue;
        }

        if (sscanf(hdr, "%127s %ld %lx", rel, &size, &crc) != 3) {
            printf("@NEOSPUT-ERR bad header\n");
            continue;
        }
        if (!path_is_sane(rel)) {
            printf("@NEOSPUT-ERR bad path\n");
            continue;
        }
        if (size <= 0) {
            printf("@NEOSPUT-ERR bad size\n");
            continue;
        }
        if (!bsp_sdcard_get_handle()) {
            printf("@NEOSPUT-ERR no card\n");
            continue;
        }

        ESP_LOGI(TAG, "receiving %s, %ld bytes", rel, size);
        receive(rel, size, (uint32_t)crc);
    }
}

esp_err_t neos_upload_init(void)
{
    /*
     * The link is the chip own USB Serial/JTAG, not a UART.
     *
     * This is worth being explicit about, because the console makes it look
     * like a UART: the primary console is UART0 and the USB port only gets
     * output because it is configured as the secondary. Reading UART0 here
     * therefore compiles, runs, logs cheerfully - and receives nothing ever,
     * because the bytes are arriving at a different peripheral. Worse, the
     * host end blocks rather than failing, since nobody is draining the USB
     * endpoint.
     */
    const usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = RX_BUF,
        .tx_buffer_size = TX_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install((usb_serial_jtag_driver_config_t *)&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(upload_task, "upload", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "listening on USB for " MAGIC "headers");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Writing a file on an app's behalf                                   */
/* ------------------------------------------------------------------ */

/*
 * Apps cannot open files, and that is on purpose rather than an oversight:
 * the loader's libc table has fwrite and fclose but no fopen, so there is no
 * way for an app to be holding a FILE* when the card is pulled - which is a
 * thing that happens on this machine, has no warning, and would otherwise
 * leave a half-written file and a handle pointing at a dead filesystem.
 *
 * So the card stays NeOS's, and an app hands over a finished file instead.
 * One call, one file, opened and closed inside it, with the same two guards
 * the console upload path uses: the path cannot escape the card, and a write
 * that fails takes the partial file with it rather than leaving something
 * that looks saved.
 */
int neos_file_write(const char *rel, const void *data, size_t len)
{
    if (!rel || (!data && len)) {
        return -1;
    }
    if (!path_is_sane(rel)) {
        ESP_LOGE(TAG, "app asked to write a path it may not: \"%s\"", rel);
        return -2;
    }

    char full[PATH_MAX_REL + sizeof(BSP_SD_MOUNT_POINT) + 2];
    snprintf(full, sizeof(full), "%s/%s", BSP_SD_MOUNT_POINT, rel);
    make_parents(full);

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s: %s", full, strerror(errno));
        return -3;
    }

    /*
     * In blocks, because a megabyte handed to FATFS in one call is a
     * megabyte during which nothing else on this core runs, and the touch
     * task is on it.
     */
    const uint8_t *p = (const uint8_t *)data;
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        if (want > 16384) {
            want = 16384;
        }
        if (fwrite(p + done, 1, want, f) != want) {
            ESP_LOGE(TAG, "write %s: %s", full, strerror(errno));
            fclose(f);
            unlink(full);
            return -4;
        }
        done += want;
        taskYIELD();
    }

    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close %s: %s", full, strerror(errno));
        unlink(full);
        return -5;
    }

    ESP_LOGI(TAG, "app wrote %s, %u bytes", full, (unsigned)len);
    return 0;
}
