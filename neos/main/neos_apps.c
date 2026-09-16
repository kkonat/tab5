#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/m5stack_tab5.h"

#include "neos_app.h"
#include "neos_api.h"
#include "neos_crash.h"

static const char *TAG = "neos";

#define APPS_DIR BSP_SD_MOUNT_POINT "/apps"

/*
 * The registry, on the heap and in PSRAM rather than in a static array.
 *
 * NEOS_APPS_MAX is set well past anything a card will really hold, which is
 * the right way round for a limit nobody should ever meet - but only if
 * meeting it is what costs, rather than declaring it. An entry is 272 bytes,
 * so the cap as a static array is 68 KB of internal RAM reserved for ever
 * against a card with a dozen directories on it, out of the same pool NeOS,
 * the radio and every app's fast allocations come from. Out of PSRAM it is 68
 * KB of 32 MB, taken once at boot.
 *
 * Allocated at the first scan and kept: neos_apps_get() and neos_apps_find()
 * hand out pointers into it that callers hold across a redraw, so it must not
 * move, and there is no moment worth freeing it at.
 */
static neos_app_t *s_apps;
static int         s_count;
static uint32_t    s_generation;

int neos_apps_scan(void)
{
    s_count = 0;

    if (!s_apps) {
        s_apps = heap_caps_calloc(NEOS_APPS_MAX, sizeof(neos_app_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_apps) {
            /* No PSRAM, or none left. Internal will do - the alternative is a
               tablet with no apps on it, which is worse than a tight heap. */
            s_apps = calloc(NEOS_APPS_MAX, sizeof(neos_app_t));
        }
        if (!s_apps) {
            ESP_LOGE(TAG, "no memory for the app registry");
            return 0;
        }
    }

    DIR *d = opendir(APPS_DIR);
    if (!d) {
        ESP_LOGE(TAG, "no %s on the card", APPS_DIR);
        return 0;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < NEOS_APPS_MAX) {
        if (e->d_name[0] == '.') {
            continue;
        }

        neos_app_t *a = &s_apps[s_count];
        memset(a, 0, sizeof(*a));
        strlcpy(a->dir, e->d_name, sizeof(a->dir));

        char appdir[288];
        snprintf(appdir, sizeof(appdir), "%s/%s", APPS_DIR, e->d_name);

        a->ok = neos_app_manifest(appdir, a->name, sizeof(a->name),
                                  a->entry, sizeof(a->entry),
                                  a->desc, sizeof(a->desc),
                                  a->category, sizeof(a->category));
        if (!a->ok) {
            strlcpy(a->name, e->d_name, sizeof(a->name));
        }
        a->crashes = neos_quarantine_count(e->d_name);

        ESP_LOGI(TAG, "app \"%s\"%s%s", a->dir,
                 a->ok ? "" : " (manifest unreadable)",
                 a->crashes ? " QUARANTINED" : "");
        s_count++;
    }
    closedir(d);

    /*
     * Say so when the shelf is full, because the way this used to fail was in
     * silence: the loop above stops at the cap and the thirteenth directory on
     * the card simply never appeared, which reads as an upload that did not
     * work rather than as a limit that was reached.
     */
    if (s_count >= NEOS_APPS_MAX) {
        ESP_LOGW(TAG, "the registry is full at %d - any further app directory "
                      "on the card is not being listed", NEOS_APPS_MAX);
    }

    s_generation++;
    ESP_LOGI(TAG, "%d app%s on the card", s_count, s_count == 1 ? "" : "s");
    return s_count;
}

int neos_apps_count(void)
{
    return s_count;
}

const neos_app_t *neos_apps_get(int idx)
{
    return (idx >= 0 && idx < s_count) ? &s_apps[idx] : NULL;
}

const neos_app_t *neos_apps_find(const char *dir)
{
    if (!dir) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].dir, dir) == 0) {
            return &s_apps[i];
        }
    }
    return NULL;
}

uint32_t neos_apps_generation(void)
{
    return s_generation;
}

/*
 * The way back out of quarantine - see neos_api.h for why there has to be one.
 *
 * The registry entry is corrected in place rather than by rescanning. A rescan
 * would give the same answer, but it walks the card and reads every manifest
 * to learn one thing this already knows, and the card is removable: an app let
 * out of quarantine while the card is being written would come back with a
 * half-uploaded neighbour's manifest unreadable, which is a change nobody
 * asked for arriving on the back of one they did.
 *
 * The generation still moves, because a shell polls it to find out that a card
 * it is showing is no longer what it drew.
 */
bool neos_apps_unquarantine(const char *dir)
{
    neos_app_t *a = NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].dir, dir ? dir : "") == 0) {
            a = &s_apps[i];
            break;
        }
    }
    if (!a || !a->crashes) {
        return false;          /* no such app, or it was never quarantined */
    }

    neos_quarantine_clear(a->dir);
    a->crashes = 0;
    s_generation++;
    ESP_LOGI(TAG, "\"%s\" let out of quarantine", a->dir);
    return true;
}
