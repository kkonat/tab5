#include <inttypes.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>

#include "neos_crash.h"

static const char *TAG = "neos";

/* Unchanged since the launcher owned this: renaming it would orphan the
   quarantine counters already on every device. */
#define NVS_NS "launcher"

/* ------------------------------------------------------------------ */
/* Crash breadcrumb, in NVS                                            */
/* ------------------------------------------------------------------ */
/*
 * NVS, not RTC memory. RTC survives a panic - that was measured - but the
 * physical button power-cycles the board, which wipes RTC entirely (also
 * measured: the boot counter reset to 1). Since that button is the escape
 * hatch from a wedged app, a breadcrumb that does not survive the escape
 * cannot attribute the hang that caused it. Flash costs two small writes per
 * launch, which is nothing for a human-initiated action.
 */
#define CRUMB_KEY "running"

void neos_crumb_enter(const char *app)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, CRUMB_KEY, app);
        nvs_commit(h);
        nvs_close(h);
    }
}

void neos_crumb_leave(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, CRUMB_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
}

/** Read the breadcrumb and clear it. True if the previous run left one. */
bool neos_crumb_take(char *out, size_t out_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_sz;
    bool got = (nvs_get_str(h, CRUMB_KEY, out, &len) == ESP_OK) && out[0] != 0;
    if (got) {
        nvs_erase_key(h, CRUMB_KEY);
        nvs_commit(h);
    }
    nvs_close(h);
    return got;
}

/* ------------------------------------------------------------------ */
/* Quarantine in NVS                                                   */
/* ------------------------------------------------------------------ */

uint32_t neos_quarantine_count(const char *app)
{
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, app, &n);
        nvs_close(h);
    }
    return n;
}

void neos_quarantine_add(const char *app)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t n = 0;
    nvs_get_u32(h, app, &n);
    n++;
    nvs_set_u32(h, app, n);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGE(TAG, "  quarantined \"%s\" (crash #%" PRIu32 ")", app, n);
}

/* ------------------------------------------------------------------ */
/* Deferred attribution                                                */
/* ------------------------------------------------------------------ */

static char s_died_in[48];
static bool s_was_a_fault;

void neos_crash_note(const char *app, bool was_a_fault)
{
    strlcpy(s_died_in, app ? app : "", sizeof(s_died_in));
    s_was_a_fault = was_a_fault;
}

void neos_crash_settle(const char *autorun_dir)
{
    if (!s_died_in[0]) {
        return;
    }
    /*
     * Not every reboot is a crash. A reflash over USB or an esp_restart()
     * leaves a breadcrumb exactly like a panic does, and counting those
     * would mean an app is quarantined for the crime of being open on the
     * developer's desk.
     */
    if (!s_was_a_fault) {
        ESP_LOGI(TAG, "  previous run ended inside \"%s\", but not by fault", s_died_in);
    } else if (autorun_dir && strcmp(s_died_in, autorun_dir) == 0) {
        ESP_LOGW(TAG, "  previous run ended inside the shell \"%s\" - not quarantining",
                 s_died_in);
    } else {
        neos_quarantine_add(s_died_in);
    }
    s_died_in[0] = 0;
}

void neos_quarantine_clear(const char *app)
{
    nvs_handle_t h;
    if (!app || !app[0] || nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t n = 0;
    if (nvs_get_u32(h, app, &n) == ESP_OK && n) {
        nvs_erase_key(h, app);
        nvs_commit(h);
        ESP_LOGI(TAG, "  \"%s\" ran cleanly, quarantine cleared", app);
    }
    nvs_close(h);
}
