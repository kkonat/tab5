#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "neos_abi.h"
#include "neos_app.h"
#include "neos_crash.h"

static const char *TAG = "neos";

/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_size = got;
    return buf;
}

bool neos_app_manifest(const char *dir, char *name, size_t name_sz,
                          char *entry, size_t entry_sz,
                          char *desc, size_t desc_sz)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);

    size_t sz = 0;
    uint8_t *raw = read_file(path, &sz);
    if (!raw) {
        ESP_LOGE(TAG, "  no readable manifest.json");
        return false;
    }

    cJSON *root = cJSON_ParseWithLength((const char *)raw, sz);
    free(raw);
    if (!root) {
        ESP_LOGE(TAG, "  manifest.json is not valid JSON");
        return false;
    }

    bool ok = false;
    const cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *je = cJSON_GetObjectItemCaseSensitive(root, "entry");
    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "description");
    if (cJSON_IsString(jn) && cJSON_IsString(je)) {
        strlcpy(name, jn->valuestring, name_sz);
        strlcpy(entry, je->valuestring, entry_sz);
        if (desc && desc_sz) {
            strlcpy(desc, cJSON_IsString(jd) ? jd->valuestring : "", desc_sz);
        }
        ok = true;
    } else {
        ESP_LOGE(TAG, "  manifest missing \"name\" or \"entry\"");
    }
    cJSON_Delete(root);
    return ok;
}

bool neos_app_run(const char *appdir, const char *dirname,
                    const char *name, const char *entry, int slot)
{
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", appdir, entry);

    size_t sz = 0;
    uint8_t *image = read_file(path, &sz);
    if (!image) {
        ESP_LOGE(TAG, "  cannot read %s", entry);
        return false;
    }

    esp_elf_t elf;
    if (esp_elf_init(&elf) != 0) {
        free(image);
        return false;
    }

    int err = esp_elf_relocate(&elf, image);
    free(image);
    if (err == -ENOSYS) {
        /*
         * Something the app references is not in the syscall table, and the
         * loader has already logged which name. If that name is a neos_abi_*
         * guard then this is the version check firing: the app was built
         * against an ABI this firmware does not implement.
         */
        ESP_LOGE(TAG, "  unresolved symbol - app built for a newer NeOS?"
                      " this firmware is ABI %d.%d", NEOS_ABI_MAJOR, NEOS_ABI_MINOR);
        esp_elf_deinit(&elf);
        return false;
    }
    if (err != 0) {
        ESP_LOGE(TAG, "  relocate failed: %d", err);
        esp_elf_deinit(&elf);
        return false;
    }

    char slotstr[4];
    snprintf(slotstr, sizeof(slotstr), "%d", slot);
    char *argv[] = { (char *)name, slotstr };

    neos_crumb_enter(dirname);
    ESP_LOGI(TAG, "  ---- running \"%s\" ----", name);
    err = esp_elf_request(&elf, 0, 2, argv);
    ESP_LOGI(TAG, "  ---- \"%s\" returned, status %d ----", name, err);
    neos_crumb_leave();

    esp_elf_deinit(&elf);
    return err == 0;
}
