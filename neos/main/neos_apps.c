#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "bsp/m5stack_tab5.h"

#include "neos_app.h"
#include "neos_api.h"
#include "neos_crash.h"

static const char *TAG = "neos";

#define APPS_DIR BSP_SD_MOUNT_POINT "/apps"

static neos_app_t s_apps[NEOS_APPS_MAX];
static int        s_count;
static uint32_t   s_generation;

int neos_apps_scan(void)
{
    s_count = 0;

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
