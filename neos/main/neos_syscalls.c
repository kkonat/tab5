#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
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
#include "neos_screen.h"
#include "neos_tasks.h"
#include "neos_touch.h"
#include "neos_ui.h"
#include "neos_syscalls.h"

/* ------------------------------------------------------------------ */
/* Syscall surface exported to apps                                    */
/* ------------------------------------------------------------------ */

/*
 * The long-long division helpers, from libgcc.
 *
 * Declared here because there is no header for them - they are what the
 * compiler emits, not something anybody calls by name - and exported because
 * this ABI hands apps a uint64_t and then could not divide it.
 * neos_uptime_ms() and neos_uptime_us() both return one, so
 * `neos_uptime_ms() / 1000` is the most natural line anybody will ever write
 * against this header, and until now it produced an app that would not load
 * with "Can't find common __udivdi3" - a trap set by the ABI itself, sprung
 * at the loader, three steps from anything to do with time.
 *
 * Four entries, against code the firmware is already linked with. Taking their
 * addresses is also what pulls them in, since the firmware does not otherwise
 * divide a 64-bit number.
 *
 * Note which ones are NOT here: the float and double helpers stay out, and
 * apps stay compiled -Werror=double-promotion. Dividing a timestamp is a thing
 * every app does; doing arithmetic in double is not.
 */
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern long long          __divdi3(long long, long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern long long          __moddi3(long long, long long);

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
 * The app-facing name for a bar panel.
 *
 * A thunk rather than exporting neos_ui_open() itself, because the two enums are
 * deliberately separate: neos_panel_t is NeOS's own list and grows whenever a
 * panel is added, while neos_syspanel_t is ABI and every value in it is compiled
 * into apps already on the card. Mapping them here by name means a panel can be
 * added, renumbered or reordered on this side without touching anything an app
 * was built against - and an app naming a panel this firmware does not have gets
 * nothing rather than whatever landed at that number.
 */
void neos_syspanel_open(neos_syspanel_t p)
{
    switch (p) {
    case NEOS_SYSPANEL_WIFI:    neos_ui_open(NEOS_PANEL_WIFI);    break;
    case NEOS_SYSPANEL_CLOCK:   neos_ui_open(NEOS_PANEL_CLOCK);   break;
    case NEOS_SYSPANEL_BATTERY: neos_ui_open(NEOS_PANEL_BATTERY); break;
    default: break;
    }
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
    /* strstr is the one everybody reaches for and the one the loader's table
       does not have, so two apps have now written it. The other three are its
       neighbours: comparing a bounded or case-blind string is what reading a
       header, a config line or a filename comes down to. */
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strcasecmp),
    ESP_ELFSYM_EXPORT(strncasecmp),

    /* Sixty-four bit division; see the note above these. */
    ESP_ELFSYM_EXPORT(__udivdi3),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(__umoddi3),
    ESP_ELFSYM_EXPORT(__moddi3),

    /*
     * The float half of <math.h>, and only the float half.
     *
     * An app that wants a cosine currently writes one - radio carries two
     * hundred and seventy lines of minimax polynomial to draw a tape deck and
     * design six biquads - or reaches for cos() and finds out at load time
     * that the double helpers are not all here. The first is a waste and the
     * second teaches the wrong lesson at the worst moment.
     *
     * Exporting the f variants alone settles it in the direction the rest of
     * the ABI already points: sinf() works, sin() still does not, and the
     * reason is the same one every other header gives - this machine has a
     * single-precision FPU and nothing that crosses this line trades in
     * doubles. A dozen entries against newlib the firmware already links.
     */
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),
    ESP_ELFSYM_EXPORT(tanf),
    ESP_ELFSYM_EXPORT(atanf),
    ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(sqrtf),
    ESP_ELFSYM_EXPORT(expf),
    ESP_ELFSYM_EXPORT(logf),
    ESP_ELFSYM_EXPORT(log10f),
    ESP_ELFSYM_EXPORT(powf),
    ESP_ELFSYM_EXPORT(fmodf),
    ESP_ELFSYM_EXPORT(floorf),
    ESP_ELFSYM_EXPORT(ceilf),

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
     * Asking for a scan is on this side of that line, and it was an oversight
     * that it was not here already: neos_net.h documents the poll-and-reread
     * loop for apps, and an app that can watch neos_net_scanning() but never
     * make it true can only ever redraw whatever NeOS last happened to find.
     * Which is nothing at all once the tablet is associated, since the
     * reconnect loop only scans while it is looking for somewhere to go.
     *
     * It steers nothing. The radio comes back to its home channel on its own,
     * the call refuses when the stack is down or a scan is already up, and
     * autojoin acts only on a scan the network task started itself - so an app
     * scanning cannot associate the tablet with anything. What it does cost is
     * a couple of seconds of the link being off-channel, which is why the one
     * app that uses this leaves a gap between passes rather than running them
     * back to back.
     */
    ESP_ELFSYM_EXPORT(neos_net_scan_start),

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
    /* The name lookup every connection starts with. Here rather than in each
       app, which is where it had got to being written twice. */
    ESP_ELFSYM_EXPORT(neos_resolve),

    /* TLS, for the hosts that will not talk without it. The one thing on the
       wire an app genuinely cannot do for itself: mbedTLS and a certificate
       bundle are larger than any app on the card, and both are already in
       this image for the weather. */
    ESP_ELFSYM_EXPORT(neos_tls_open),
    ESP_ELFSYM_EXPORT(neos_tls_status),
    ESP_ELFSYM_EXPORT(neos_tls_send),
    ESP_ELFSYM_EXPORT(neos_tls_recv),
    ESP_ELFSYM_EXPORT(neos_tls_fd),
    ESP_ELFSYM_EXPORT(neos_tls_close),
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

    /*
     * The pages behind the bar's icons, raised from somewhere that is not the
     * bar. Everything on them belongs to NeOS, so there is nothing here but the
     * way in - see neos_api.h.
     */
    ESP_ELFSYM_EXPORT(neos_syspanel_open),

    /*
     * What the machine is running, and the one thing that can be done about it.
     *
     * The list is read-only and the kill refuses everything NeOS needs, which is
     * what makes this safe to hand out at all - see neos_api.h for where that
     * line falls.
     */
    ESP_ELFSYM_EXPORT(neos_tasks),
    ESP_ELFSYM_EXPORT(neos_task_state_name),
    ESP_ELFSYM_EXPORT(neos_task_kill),

    /*
     * Background services: an app leaving a task behind, and NeOS keeping its
     * image loaded for as long as that task runs. The player case is the whole
     * reason - see neos_api.h.
     */
    ESP_ELFSYM_EXPORT(neos_service_start),
    ESP_ELFSYM_EXPORT(neos_service_stopping),
    ESP_ELFSYM_EXPORT(neos_service_count),

    /*
     * Sleeping, which on this machine is the screen and nothing else. An app can
     * ask for it, ask to be left alone by it, and set how long it waits.
     */
    ESP_ELFSYM_EXPORT(neos_screen_off),
    ESP_ELFSYM_EXPORT(neos_screen_on),
    ESP_ELFSYM_EXPORT(neos_screen_is_off),
    ESP_ELFSYM_EXPORT(neos_idle_poke),
    ESP_ELFSYM_EXPORT(neos_idle_timeout_min),
    ESP_ELFSYM_EXPORT(neos_idle_timeout_set),

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
