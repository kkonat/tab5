#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#include "esp_elf.h"
#include "esp_log.h"

#include "ngl.h"
#include "neos_orient.h"
#include "neos_status.h"
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
    ESP_ELFSYM_EXPORT(ngl_from_panel),

    /* Formatting and strings. The loader carries a libc table of its own but
       not these, and every app that draws a number needs them. */
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(vsnprintf),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strlen),

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

    /* ngl: text */
    ESP_ELFSYM_EXPORT(ngl_text),
    ESP_ELFSYM_EXPORT(ngl_text_bg),
    ESP_ELFSYM_EXPORT(ngl_text_width),
    ESP_ELFSYM_EXPORT(ngl_font_small),
    ESP_ELFSYM_EXPORT(ngl_font_large),

    /* the boot chain: how one app hands over to the next */
    ESP_ELFSYM_EXPORT(neos_exec),

    /* the app registry, so a shell does not have to walk the card itself */
    ESP_ELFSYM_EXPORT(neos_apps_scan),
    ESP_ELFSYM_EXPORT(neos_apps_count),
    ESP_ELFSYM_EXPORT(neos_apps_get),
    ESP_ELFSYM_EXPORT(neos_apps_find),
    ESP_ELFSYM_EXPORT(neos_apps_generation),

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
