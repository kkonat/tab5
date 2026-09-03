/*
 * NeOS - system bring-up.
 *
 * Everything low-level happens here and nothing is drawn: NVS, crash
 * attribution for the previous boot, I2C, the orientation sensor, the panel,
 * the graphics library and the syscall table apps are resolved against.
 * The last thing it does is hand the screen to the launcher.
 *
 * Display comes from the official Espressif BSP (espressif/m5stack_tab5_noglib):
 * MIPI-DSI, the PHY LDO, panel detection (ILI9881C / ST7123) and backlight are
 * all handled there. No LVGL - under the B2 architecture NeOS owns the panel
 * and framebuffer, and apps bring their own graphics stack.
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_private/startup_internal.h"

#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"
#include "ngl.h"

#include "neos_audio.h"
#include "neos_boot.h"
#include "neos_app.h"
#include "neos_crash.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_syscalls.h"
#include "neos_sys.h"
#include "neos_time.h"
#include "neos_touch.h"
#include "neos_upload.h"

static const char *TAG = "neos";

/*
 * How much internal RAM is left by the time the scheduler starts.
 *
 * This runs in the secondary init stage, which is after do_global_ctors() and
 * before esp_startup_start_app() - the one point where everything that
 * initialises itself behind the system's back has already done so and nothing
 * has been scheduled yet. ESP-Hosted brings the whole SDIO transport up from a
 * C constructor, so this is the only place its cost can be seen.
 *
 * It matters because FreeRTOS allocates the idle task's TCB from internal RAM
 * with an assert, not an error: run out here and the machine panics inside
 * vTaskStartScheduler with no app having run and nothing to point at.
 */
ESP_SYSTEM_INIT_FN(neos_report_early_heap, SECONDARY, BIT(0), 200)
{
    ESP_EARLY_LOGI(TAG, "%u B internal RAM left for the scheduler",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

/*
 * Did the last boot end in something an app should be blamed for?
 *
 * A panic or a watchdog obviously counts. So does a power-cycle: the button
 * is the documented way out of an app that has wedged the tablet, and it is
 * the only signal that ever gets left behind. A USB reflash or a software
 * restart does not - that is a developer at a desk, not a fault.
 */
static bool reason_is_fault(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
        return true;
    default:
        return false;
    }
}

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on (button, or plugged in)";
    case ESP_RST_EXT:      return "external reset pin";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "USB (esptool)";
    default:               return "other";
    }
}

/* ------------------------------------------------------------------ */
/* Display                                                             */
/* ------------------------------------------------------------------ */

static esp_lcd_panel_handle_t s_panel;

static esp_err_t display_init(void)
{
    bsp_display_config_t cfg = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };

    bsp_lcd_handles_t h = {0};
    esp_err_t err = bsp_display_new_with_handles(&cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new_with_handles: %s", esp_err_to_name(err));
        return err;
    }
    s_panel = h.panel;

    /* The BSP does reset + init only; turning the panel on is explicit. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_ERROR_CHECK(bsp_display_brightness_init());
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    ESP_LOGI(TAG, "display up: %dx%d, RGB565, %d DSI lanes @ %d Mbps",
             BSP_LCD_H_RES, BSP_LCD_V_RES,
             BSP_LCD_MIPI_DSI_LANE_NUM, BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS);
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "==== NeOS ====");

    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s", reason_name(rr));

    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* --- did the previous run come back? --- */
    char died_in[48] = {0};
    if (neos_crumb_take(died_in, sizeof(died_in))) {
        ESP_LOGE(TAG, "PREVIOUS RUN DIED INSIDE \"%s\" (%s)", died_in, reason_name(rr));
        neos_crash_note(died_in, reason_is_fault(rr));
    } else {
        ESP_LOGI(TAG, "previous run exited cleanly");
    }

    /* --- screen first, so everything after this is visible --- */
    ESP_ERROR_CHECK(bsp_i2c_init());
    neos_orient_init();
    if (display_init() == ESP_OK) {
        if (ngl_screen_init(s_panel, BSP_LCD_H_RES, BSP_LCD_V_RES) == 0) {

            /* Follow gravity. Flat on a desk gives no in-plane signal, in
               which case this keeps the default rather than guessing. */
            neos_orient_update();
            ngl_set_rotation(neos_orient_get());
            ESP_LOGI(TAG, "orientation: %s", neos_orient_name(neos_orient_get()));

        }
    }

    /*
     * After the display, not before: this is what restores the saved backlight,
     * and the BSP has to have brought the LEDC channel up before anyone can
     * set a duty on it. Everything else it touches only needs I2C.
     */
    neos_sys_init();

    /*
     * Both of these read settings neos_sys_init() has just made available, and
     * one of them needs the RTC it has just probed. The clock is seeded before
     * anything can ask what time it is; the codec is brought up only if tap
     * sounds were ever switched on, so on most boots this line does nothing.
     */
    neos_time_init();
    neos_audio_init();

    neos_touch_init();
    neos_upload_init();

    neos_syscalls_register();

#ifdef NEOS_CALIBRATE_ORIENT
    /* Temporary: dump raw gravity so the axis mapping can be derived from the
       real board rather than guessed. Remove once docs/orientation.md is written. */
    ESP_LOGW(TAG, ">>> ORIENTATION CALIBRATION: hold the tablet still in one");
    ESP_LOGW(TAG, ">>> position, I will read gravity for 30 s <<<");
    for (int i = 0; i < 60; i++) {
        float x = 0, y = 0, z = 0;
        if (neos_orient_read(&x, &y, &z) == ESP_OK) {
            ESP_LOGI("accel", "x=%+.2f y=%+.2f z=%+.2f   guess=%s",
                     x, y, z, neos_orient_name(neos_orient_get()));
        }
        neos_orient_update();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#endif

    /* Hardware is up and the ABI is published. What runs from here is the
       card's business, not ours. */
    neos_boot();

}
