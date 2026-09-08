#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "esp_elf.h"
#include "esp_log.h"

#include "ngl.h"
#include "neos_net.h"
#include "neos_sock.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"
#include "neos_weather.h"
#include "neos_api.h"
#include "neos_boot.h"
#include "neos_touch.h"
#include "neos_syscalls.h"

/* ------------------------------------------------------------------ */
/* Syscall surface exported to apps                                    */
/* ------------------------------------------------------------------ */

int neos_log(const char *msg)
{
    ESP_LOGW("syscall", "app called neos_log(\"%s\")", msg ? msg : "(null)");
    return 0;
}

int neos_add(int a, int b)
{
    ESP_LOGW("syscall", "app called neos_add(%d, %d)", a, b);
    return a + b;
}

/* ------------------------------------------------------------------ */
/* The ABI guard chain                                                 */
/* ------------------------------------------------------------------ */

/*
 * One tiny object per minor version, exported like any other symbol. An app
 * references exactly the one it was built against, so the loader's ordinary
 * name resolution is the version check: too new an app cannot resolve its
 * guard and is refused before it runs a single instruction, an older one finds
 * its guard still on the list and loads.
 *
 * The value is never read - the name is the whole mechanism - but it is not
 * zero so that the object cannot be folded into .bss and share an address.
 * See neos_abi.h for what does and does not warrant a bump.
 */
#define NEOS_ABI_DEFINE(maj, min) const uint32_t neos_abi_##maj##_##min = ((maj) << 16) | (min);

NEOS_ABI_GUARDS(NEOS_ABI_DEFINE)

#define NEOS_ABI_EXPORT(maj, min) { "neos_abi_" #maj "_" #min, (void *)&neos_abi_##maj##_##min },

void neos_sleep_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*
 * The OS ABI. Every symbol here is a promise: apps built against it must keep
 * working across launcher rebuilds.
 *
 * Order carries no meaning - the loader resolves by name - so entries go
 * wherever they read best. Adding one is a NEOS_ABI_MINOR bump; removing or
 * changing one is a major.
 */
static esp_elf_symbol_table_t neos_syscalls[] = {
    NEOS_ABI_GUARDS(NEOS_ABI_EXPORT)

    /* misc */
    ESP_ELFSYM_EXPORT(neos_log),
    ESP_ELFSYM_EXPORT(neos_add),
    ESP_ELFSYM_EXPORT(neos_sleep_ms),
    ESP_ELFSYM_EXPORT(neos_app_close_requested),
    ESP_ELFSYM_EXPORT(neos_app_self),

    /* touch */
    ESP_ELFSYM_EXPORT(neos_touch),
    ESP_ELFSYM_EXPORT(neos_touch_tap),
    ESP_ELFSYM_EXPORT(neos_touch_points),
    ESP_ELFSYM_EXPORT(ngl_from_panel),

    /*
     * Text input.
     *
     * The keyboard is a system panel and an app never draws one - it asks for
     * a string and blocks. neos_ui_busy() is the only other half of the story
     * an app can see, and it is advisory: everything that makes a panel modal
     * is enforced below this line, not by the app cooperating.
     */
    ESP_ELFSYM_EXPORT(neos_input_text),
    ESP_ELFSYM_EXPORT(neos_ui_busy),

    /* Formatting and strings. The loader carries a libc table of its own but
       not these, and every app that draws a number needs them. */
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(vsnprintf),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strlen),
    /*
     * memmove is here because the compiler asks for it whether the app does
     * or not: an overlapping array shift, and sometimes a plain struct copy,
     * is emitted as a call to it. The loader's table has memcpy and memset
     * but not this one, so without it an app that never types the name fails
     * to load - which is a confusing morning.
     */
    ESP_ELFSYM_EXPORT(memmove),
    /* memcmp is here for the same reason and with the same surprise: the
       loader's table has memcpy and memset but neither of the other two, and
       gcc will synthesise a call to this one for a struct comparison the app
       never wrote as a function call. */
    ESP_ELFSYM_EXPORT(memcmp),

    /* ngl: screen and surfaces */
    ESP_ELFSYM_EXPORT(ngl_screen),
    ESP_ELFSYM_EXPORT(ngl_flush),
    ESP_ELFSYM_EXPORT(ngl_dirty),
    ESP_ELFSYM_EXPORT(ngl_dirty_all),
    ESP_ELFSYM_EXPORT(ngl_surface_new),
    ESP_ELFSYM_EXPORT(ngl_surface_wrap),
    ESP_ELFSYM_EXPORT(ngl_surface_free),
    ESP_ELFSYM_EXPORT(ngl_surface_w),
    ESP_ELFSYM_EXPORT(ngl_surface_h),
    ESP_ELFSYM_EXPORT(ngl_surface_bounds),
    ESP_ELFSYM_EXPORT(ngl_surface_clip),
    ESP_ELFSYM_EXPORT(ngl_clip_set),
    ESP_ELFSYM_EXPORT(ngl_app_area),
    ESP_ELFSYM_EXPORT(ngl_bar_height),
    ESP_ELFSYM_EXPORT(ngl_bar_rect),
    ESP_ELFSYM_EXPORT(ngl_rotation),
    ESP_ELFSYM_EXPORT(ngl_icon),
    ESP_ELFSYM_EXPORT(ngl_icon_find),

    /* orientation control */
    /*
     * neos_orient_read is the raw sensor, and an app wants it for the one
     * thing lock/get cannot express: pinning the display while still
     * following gravity. A viewer that must stay landscape has to keep
     * sensing to know when the tablet has been turned over, but an unlocked
     * display is one the watcher may rotate to portrait mid-render - which
     * reallocates the back buffer from another task. Handing over gravity
     * lets the app lock the panel and decide for itself, at a point in its
     * own loop where nothing is half-drawn.
     */
    ESP_ELFSYM_EXPORT(neos_orient_read),
    ESP_ELFSYM_EXPORT(neos_orient_lock),
    ESP_ELFSYM_EXPORT(neos_orient_unlock),
    ESP_ELFSYM_EXPORT(neos_orient_is_locked),
    ESP_ELFSYM_EXPORT(neos_orient_get),
    ESP_ELFSYM_EXPORT(neos_orient_name),

    /* status line */
    ESP_ELFSYM_EXPORT(neos_status),
    ESP_ELFSYM_EXPORT(neos_status_for),
    ESP_ELFSYM_EXPORT(neos_status_clear),
    ESP_ELFSYM_EXPORT(neos_status_paint),

    /* ngl: geometry */
    ESP_ELFSYM_EXPORT(ngl_rect_intersect),
    ESP_ELFSYM_EXPORT(ngl_rect_union),
    ESP_ELFSYM_EXPORT(ngl_rect_contains),

    /* ngl: primitives */
    ESP_ELFSYM_EXPORT(ngl_clear),
    ESP_ELFSYM_EXPORT(ngl_pixel),
    ESP_ELFSYM_EXPORT(ngl_pixel_blend),
    ESP_ELFSYM_EXPORT(ngl_fill_rect),
    ESP_ELFSYM_EXPORT(ngl_draw_rect),
    ESP_ELFSYM_EXPORT(ngl_fill_round_rect),
    ESP_ELFSYM_EXPORT(ngl_draw_round_rect),
    ESP_ELFSYM_EXPORT(ngl_hline),
    ESP_ELFSYM_EXPORT(ngl_vline),
    ESP_ELFSYM_EXPORT(ngl_line),
    ESP_ELFSYM_EXPORT(ngl_line_aa),

    /* ngl: blitting */
    ESP_ELFSYM_EXPORT(ngl_blit),
    ESP_ELFSYM_EXPORT(ngl_blit_key),
    ESP_ELFSYM_EXPORT(ngl_blit_scale),
    /* Straight to the glass, for something composing a small picture every
       frame - it skips the back buffer and the rotation with it. See ngl.h. */
    ESP_ELFSYM_EXPORT(ngl_panel_scale),
    ESP_ELFSYM_EXPORT(ngl_panel_size),
    ESP_ELFSYM_EXPORT(ngl_blit_p8),

    /* ngl: text */
    ESP_ELFSYM_EXPORT(ngl_text),
    ESP_ELFSYM_EXPORT(ngl_text_bg),
    ESP_ELFSYM_EXPORT(ngl_text_width),
    ESP_ELFSYM_EXPORT(ngl_font_small),
    ESP_ELFSYM_EXPORT(ngl_font_large),

    /* the machine itself */
    ESP_ELFSYM_EXPORT(neos_chip),
    ESP_ELFSYM_EXPORT(neos_mac),
    ESP_ELFSYM_EXPORT(neos_reset_reason),
    ESP_ELFSYM_EXPORT(neos_idf_version),
    ESP_ELFSYM_EXPORT(neos_build),
    ESP_ELFSYM_EXPORT(neos_build_date),
    ESP_ELFSYM_EXPORT(neos_abi),
    ESP_ELFSYM_EXPORT(neos_cpu_mhz),
    ESP_ELFSYM_EXPORT(neos_cores),
    ESP_ELFSYM_EXPORT(neos_uptime_ms),
    ESP_ELFSYM_EXPORT(neos_uptime_s),
    ESP_ELFSYM_EXPORT(neos_uptime_us),
    ESP_ELFSYM_EXPORT(neos_heap_free),
    ESP_ELFSYM_EXPORT(neos_heap_total),
    ESP_ELFSYM_EXPORT(neos_psram_free),
    ESP_ELFSYM_EXPORT(neos_psram_total),
    /* Internal RAM by request, for the bytes an app touches millions of times
       a second. malloc() is PSRAM past a kilobyte and stays the default. */
    ESP_ELFSYM_EXPORT(neos_alloc_fast),

    /* sensors */
    ESP_ELFSYM_EXPORT(neos_imu_accel_mg),
    ESP_ELFSYM_EXPORT(neos_imu_gyro_dps),
    ESP_ELFSYM_EXPORT(neos_die_temp_c10),
    ESP_ELFSYM_EXPORT(neos_rtc_read),
    ESP_ELFSYM_EXPORT(neos_rtc_set),

    /* the clock, and the zone that turns it into a wall time */
    ESP_ELFSYM_EXPORT(neos_time_local),
    ESP_ELFSYM_EXPORT(neos_time_utc),
    ESP_ELFSYM_EXPORT(neos_time_synced),
    ESP_ELFSYM_EXPORT(neos_time_since_sync_s),
    ESP_ELFSYM_EXPORT(neos_time_set_local),
    ESP_ELFSYM_EXPORT(neos_tz_offset_min),
    ESP_ELFSYM_EXPORT(neos_tz_offset_set),
    ESP_ELFSYM_EXPORT(neos_tz_dst),
    ESP_ELFSYM_EXPORT(neos_tz_dst_set),
    ESP_ELFSYM_EXPORT(neos_tz_total_min),

    /*
     * The network, readable but not steerable.
     *
     * There is one radio and one set of stored credentials, so associating is
     * NeOS's job and the Wi-Fi panel is where it happens - see neos_net.h.
     * What an app gets is everything it needs to show the state and nothing it
     * needs to change it.
     */
    ESP_ELFSYM_EXPORT(neos_net_state),
    ESP_ELFSYM_EXPORT(neos_net_ssid),
    ESP_ELFSYM_EXPORT(neos_net_rssi),
    ESP_ELFSYM_EXPORT(neos_net_ip),
    ESP_ELFSYM_EXPORT(neos_net_known),
    ESP_ELFSYM_EXPORT(neos_net_known_count),
    ESP_ELFSYM_EXPORT(neos_net_scanning),
    ESP_ELFSYM_EXPORT(neos_net_scan_results),

    /*
     * The wire underneath it, which is a different question and gets a
     * different answer - see the header for why one of these lists is
     * read-only and the other is not.
     *
     * Nothing here decides whether the tablet has a network. It decides what
     * an app may put on the one NeOS already joined, which is the layer below
     * the weather's HTTP rather than a second way to reach it: an app still
     * has no TLS, no HTTP and no JSON, and adding those was never the trade
     * being made here.
     */
    ESP_ELFSYM_EXPORT(neos_sock_open),
    ESP_ELFSYM_EXPORT(neos_sock_close),
    ESP_ELFSYM_EXPORT(neos_sock_bind),
    ESP_ELFSYM_EXPORT(neos_sock_connect),
    ESP_ELFSYM_EXPORT(neos_sock_status),
    ESP_ELFSYM_EXPORT(neos_sock_send),
    ESP_ELFSYM_EXPORT(neos_sock_recv),
    ESP_ELFSYM_EXPORT(neos_sock_sendto),
    ESP_ELFSYM_EXPORT(neos_sock_recvfrom),
    ESP_ELFSYM_EXPORT(neos_sock_wait),
    ESP_ELFSYM_EXPORT(neos_sock_set),
    ESP_ELFSYM_EXPORT(neos_sock_join),
    ESP_ELFSYM_EXPORT(neos_iface),
    ESP_ELFSYM_EXPORT(neos_neigh_table),
    ESP_ELFSYM_EXPORT(neos_neigh_ask),

    /*
     * The weather, on the same terms as the network it arrives over: NeOS
     * does the fetching, an app reads the reading. What is not here is a
     * way to ask for a different place - there is one tablet in one room,
     * and an app that could move it would be an app that decides where the
     * machine thinks it is for whatever runs next.
     */
    ESP_ELFSYM_EXPORT(neos_weather),
    ESP_ELFSYM_EXPORT(neos_weather_refresh),
    ESP_ELFSYM_EXPORT(neos_weather_fetching),

    /* power */
    ESP_ELFSYM_EXPORT(neos_power_read),
    ESP_ELFSYM_EXPORT(neos_power_monitor_addr),

    /* backlight and the rails an app is allowed to switch */
    ESP_ELFSYM_EXPORT(neos_backlight),
    ESP_ELFSYM_EXPORT(neos_backlight_set),
    ESP_ELFSYM_EXPORT(neos_feature),
    ESP_ELFSYM_EXPORT(neos_feature_set),
    ESP_ELFSYM_EXPORT(neos_feature_name),
    ESP_ELFSYM_EXPORT(neos_settings_reset),
    ESP_ELFSYM_EXPORT(neos_tap_sound),
    ESP_ELFSYM_EXPORT(neos_tap_sound_set),

    /* the speaker, for an app that generates its own sound */
    ESP_ELFSYM_EXPORT(neos_audio_open),
    ESP_ELFSYM_EXPORT(neos_audio_write),
    ESP_ELFSYM_EXPORT(neos_audio_lead_us),
    ESP_ELFSYM_EXPORT(neos_audio_gain),
    ESP_ELFSYM_EXPORT(neos_audio_close),

    /* the card and the bus it shares the board with */
    ESP_ELFSYM_EXPORT(neos_sd_mounted),
    ESP_ELFSYM_EXPORT(neos_sd_bytes),
    ESP_ELFSYM_EXPORT(neos_sd_name),
    ESP_ELFSYM_EXPORT(neos_sd_type),
    ESP_ELFSYM_EXPORT(neos_sd_speed_khz),
    ESP_ELFSYM_EXPORT(neos_sd_bus_width),
    ESP_ELFSYM_EXPORT(neos_sd_mount),
    ESP_ELFSYM_EXPORT(neos_sd_free_bytes),
    ESP_ELFSYM_EXPORT(neos_i2c_scan),
    ESP_ELFSYM_EXPORT(neos_i2c_name),

    /* the card, for an app that has something to keep */
    ESP_ELFSYM_EXPORT(neos_file_write),
    ESP_ELFSYM_EXPORT(neos_file_read),
    ESP_ELFSYM_EXPORT(neos_file_size),
    ESP_ELFSYM_EXPORT(neos_file_read_at),

    /* the boot chain: how one app hands over to the next */
    ESP_ELFSYM_EXPORT(neos_exec),

    /* the app registry, so a shell does not have to walk the card itself */
    ESP_ELFSYM_EXPORT(neos_apps_scan),
    ESP_ELFSYM_EXPORT(neos_apps_count),
    ESP_ELFSYM_EXPORT(neos_apps_get),
    ESP_ELFSYM_EXPORT(neos_apps_find),
    ESP_ELFSYM_EXPORT(neos_apps_generation),
    /* The shell's way of letting a crashed app be tried again. Quarantine has
       to be undoable by the person holding the tablet - see neos_api.h. */
    ESP_ELFSYM_EXPORT(neos_apps_unquarantine),

    /* Game mode: the app takes the whole panel and draws its own way out.
       Undone by NeOS when the app returns - see neos_api.h. */
    ESP_ELFSYM_EXPORT(neos_fullscreen),
    ESP_ELFSYM_EXPORT(neos_is_fullscreen),

    /* the system bar is owned by whichever app is the shell */
    ESP_ELFSYM_EXPORT(ngl_reserve_top),
    ESP_ELFSYM_EXPORT(ngl_bar_paint),
    ESP_ELFSYM_EXPORT(ngl_set_rotation),
    ESP_ELFSYM_EXPORT(neos_status_init),
    ESP_ELFSYM_EXPORT(neos_orient_start_watch),

    ESP_ELFSYM_END,
};

void neos_syscalls_register(void)
{
    esp_elf_register_symbol(neos_syscalls);
}
