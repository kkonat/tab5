#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/m5stack_tab5.h"
#include "sdmmc_cmd.h"
#include "ngl.h"

#include "neos_app.h"
#include "neos_bar.h"
#include "neos_api.h"
#include "neos_boot.h"
#include "neos_cfg.h"
#include "neos_crash.h"
#include "neos_msg.h"
#include "neos_net.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "neos";

#define AUTORUN_PATH BSP_SD_MOUNT_POINT "/autorun.cfg"
#define AUTORUN_KEY  "app"

/* Long enough that a failing chain cannot spin the CPU, short enough that
   pushing a card in feels immediate. */
#define RETRY_MS 1000

static char s_exec_req[sizeof(((neos_app_t *)0)->dir)];

static volatile bool s_close_requested;
static char          s_self[sizeof(((neos_app_t *)0)->dir)];

void neos_app_request_close(void)
{
    s_close_requested = true;
}

bool neos_app_close_requested(void)
{
    return s_close_requested;
}

const char *neos_app_self(void)
{
    return s_self;
}

void neos_exec(const char *dir)
{
    if (dir && dir[0]) {
        strlcpy(s_exec_req, dir, sizeof(s_exec_req));
        ESP_LOGI(TAG, "app requested \"%s\" next", s_exec_req);
    }
}

/*
 * The OS follows the tablet too.
 *
 * A message screen is up precisely when no app is running, so there is no app
 * to repaint it - the orientation watcher rotates the panel either way, but
 * without this the text would come back sideways, or not at all. Whichever
 * app is running replaces this callback with its own, and gets it taken back
 * when it returns.
 */
static void on_system_rotate(ngl_rotation_t r)
{
    ESP_LOGI(TAG, "rotated to %s", neos_orient_name(r));
    neos_msg_repaint();
}

static void watch_orientation_for_os(void)
{
    neos_orient_start_watch(3000, on_system_rotate);
}

/*
 * Every callback an app registered points into an ELF that is about to be
 * freed. The bar painter, the status region and the orientation watcher are
 * all function pointers into the app image, so they have to be revoked here
 * rather than trusted to be re-registered by whatever runs next.
 */
static void revoke_app_callbacks(void)
{
    /*
     * A panel outlives nothing. The keyboard may be up over an app that has
     * just been asked to close, or the Wi-Fi list over an app whose card has
     * been pulled - and a modal panel with no app underneath it is a screen
     * with the wrong thing behind it and no way to get the right thing back.
     */
    neos_ui_close();
    while (neos_ui_active()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    /*
     * An app that pinned the orientation is not necessarily the one that
     * unpins it - a crash leaves the lock set, and the system would come back
     * up stuck the way that app wanted it. The lock is process-wide state
     * held on the app's behalf, so it is released here with the rest.
     */
    neos_orient_unlock();
    neos_orient_stop_watch();
    neos_bar_set_closable(false);
    neos_bar_init();          /* an app may have reserved its own */
    watch_orientation_for_os();
}

/* ------------------------------------------------------------------ */
/* Card removal                                                        */
/* ------------------------------------------------------------------ */

static volatile bool s_card_gone;

/*
 * There is no card-detect line on this board, so the card is asked directly:
 * sdmmc_get_status() is a CMD13 to the card itself, which fails as soon as it
 * is not there any more. Cheap enough at 2 Hz, and unlike watching the mount
 * point it cannot be fooled by cached directory entries.
 *
 * On removal the running app is asked to close. NeOS cannot take the screen
 * back by force - the app is ordinary code running on this stack - so an app
 * that never polls neos_app_close_requested() will keep running with a dead
 * filesystem underneath it. Every app NeOS ships polls.
 */
static void card_watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));

        sdmmc_card_t *card = bsp_sdcard_get_handle();
        if (!card || s_card_gone) {
            continue;
        }
        if (sdmmc_get_status(card) != ESP_OK) {
            ESP_LOGW(TAG, "card removed");
            s_card_gone = true;
            neos_app_request_close();
        }
    }
}

/** Block until a card is mounted, saying so on screen. */
static void wait_for_card(void)
{
    if (bsp_sdcard_mount() == ESP_OK) {
        ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);
        return;
    }

    /*
     * A failed mount leaves the SD power rail acquired. The BSP creates the
     * on-chip LDO control handle before it goes looking for a card, and only
     * ever releases it in bsp_sdcard_unmount() - so a bare retry loop fails
     * forever after the first miss, with "can't acquire the channel, already
     * in use by others", and a card pushed in later is never seen. Unmounting
     * after each failure is what makes waiting for a card work at all.
     */
    bsp_sdcard_unmount();

    ESP_LOGW(TAG, "no SD card - waiting");
    neos_msg(NEOS_MSG_WAIT, "No SD card", "Insert a card to start");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
        if (bsp_sdcard_mount() == ESP_OK) {
            break;
        }
        bsp_sdcard_unmount();
    }
    ESP_LOGI(TAG, "card inserted, mounted at %s", BSP_SD_MOUNT_POINT);
}

/**
 * Get the card's autorun app, with its list scanned and the name checked.
 * False means the reason is already on screen and the card should be re-read.
 */
static bool card_autorun(char *out, size_t out_sz)
{
    if (!neos_cfg_get(AUTORUN_PATH, AUTORUN_KEY, out, out_sz)) {
        ESP_LOGE(TAG, "no readable %s", AUTORUN_PATH);
        neos_msg(NEOS_MSG_MISSING, "No autorun.cfg",
                 "The card does not say what to run");
        return false;
    }
    ESP_LOGI(TAG, "autorun: %s", out);

    /* Before the scan, so the counts it reads are already correct. */
    neos_crash_settle(out);

    neos_apps_scan();

    const neos_app_t *a = neos_apps_find(out);
    if (!a) {
        ESP_LOGE(TAG, "autorun app \"%s\" is not on the card", out);
        neos_msg(NEOS_MSG_MISSING, out, "autorun.cfg names an app that is not here");
        return false;
    }
    if (!a->ok) {
        ESP_LOGE(TAG, "autorun app \"%s\" has no usable manifest", out);
        neos_msg(NEOS_MSG_BAD, out, "manifest.json is missing or unreadable");
        return false;
    }
    return true;
}

/** Run apps until something makes the card unusable. */
static void run_chain(const char *autorun)
{
    char next[sizeof(s_exec_req)];
    strlcpy(next, autorun, sizeof(next));

    /*
     * Whether the app about to run was asked for, or is just what NeOS falls
     * back to. It decides what quarantine means: refusing to start something
     * automatically is boot-loop protection, but refusing a deliberate tap is
     * just a dead end, since the only way an app ever leaves quarantine is by
     * being run again and not crashing.
     */
    bool asked_for = false;

    for (;;) {
        const neos_app_t *a = neos_apps_find(next);
        const bool blocked = a && a->crashes && !asked_for;

        if (a && a->crashes && asked_for) {
            ESP_LOGW(TAG, "\"%s\" is quarantined but was asked for - running it", next);
        }

        if (!a || !a->ok || blocked) {
            /* Only ever reached for an exec request, since the autorun app
               was checked before the chain started - so fall back to it. */
            ESP_LOGE(TAG, "cannot run \"%s\", returning to \"%s\"", next, autorun);
            neos_msg(a && a->crashes ? NEOS_MSG_BAD : NEOS_MSG_MISSING, next,
                     a && a->crashes ? "quarantined after a crash" : "not on the card");
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            if (strcmp(next, autorun) == 0) {
                return;         /* the shell itself is gone: re-read the card */
            }
            strlcpy(next, autorun, sizeof(next));
            continue;
        }

        char appdir[288];
        snprintf(appdir, sizeof(appdir), "%s/apps/%s", BSP_SD_MOUNT_POINT, a->dir);

        s_exec_req[0] = 0;
        s_close_requested = false;
        strlcpy(s_self, a->dir, sizeof(s_self));
        ESP_LOGI(TAG, "==== running \"%s\" ====", a->name);
        neos_touch_drop();
        neos_msg_none();          /* the app owns the screen now */
        neos_bar_set_closable(strcmp(a->dir, autorun) != 0);
        const bool clean = neos_app_run(appdir, a->dir, a->name, a->entry, 0);
        revoke_app_callbacks();
        if (clean) {
            neos_quarantine_clear(a->dir);
        }
        neos_touch_drop();
        ESP_LOGI(TAG, "==== \"%s\" returned ====", a->name);

        /* A crash last run shows up as a quarantine count on the next scan. */
        if (s_card_gone) {
            ESP_LOGW(TAG, "card is gone, back to waiting for one");
            return;
        }

        neos_apps_scan();

        if (s_exec_req[0]) {
            strlcpy(next, s_exec_req, sizeof(next));
            asked_for = true;
        } else {
            asked_for = false;
            /* Nothing requested: the card's shell comes back up. */
            strlcpy(next, autorun, sizeof(next));
            vTaskDelay(pdMS_TO_TICKS(200));   /* guard against a spin */
        }
    }
}

void neos_boot(void)
{
    neos_bar_init();
    neos_ui_init();

    /*
     * After the bar, because the radio reports itself through the status line
     * and the Wi-Fi icon, and before the card, because a network that is up by
     * the time the first app draws is a network the first app can use. It
     * returns at once either way - see neos_net_init().
     */
    neos_net_init();

    xTaskCreate(card_watch_task, "cardwatch", 3072, NULL, 3, NULL);
    watch_orientation_for_os();

    for (;;) {
        wait_for_card();
        s_card_gone = false;

        char autorun[sizeof(s_exec_req)];
        if (card_autorun(autorun, sizeof(autorun))) {
            run_chain(autorun);
        }

        /*
         * Every path back to the top unmounts, so wait_for_card() always
         * starts from a released power rail - both because the BSP only frees
         * it on unmount, and because the card may well have been pulled, which
         * is the usual reason to be back here.
         */
        bsp_sdcard_unmount();
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
}
