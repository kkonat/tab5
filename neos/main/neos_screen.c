/*
 * Turning the screen off, and deciding when to.
 *
 * The deciding is one task with a clock and two questions: has anything happened
 * lately, and if the screen is already off, has the tablet been picked up. Both
 * are cheap enough to ask four times a second, and neither needs to be asked
 * from anywhere else - which is the point of it being a task rather than a hook
 * in every input path.
 *
 * Off means the backlight at zero. Nothing else: not the panel's DISPOFF, and
 * not the CPU frequency. See below for why each of those is left alone.
 * Everything else keeps running, which is the point - this is a tablet with a
 * player on it, and a sleep that silenced the player would be a sleep nobody
 * would switch on.
 */
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"

#include "neos_api.h"
#include "neos_screen.h"
#include "neos_settings.h"
#include "neos_sys.h"

static const char *TAG = "screen";

/*
 * How often the idle clock is looked at, and how often a sleeping tablet is
 * asked whether it should wake.
 *
 * Faster while asleep, because this is the wake latency for both wake sources:
 * the accelerometer, which is only read here, and touch, which is seen by the
 * touch task and then waited for here. An eighth of a second is under what
 * anybody reads as a delay between putting a finger down and the screen coming
 * back.
 */
#define POLL_AWAKE_MS  1000
#define POLL_ASLEEP_MS 120

/*
 * How far the tablet has to move to count as picked up, in milli-g on any one
 * axis, measured against where it was lying when the screen went off.
 *
 * A BMI270 sitting still on a desk wanders by a few milli-g. A tablet being
 * lifted swings a good fraction of a g onto at least one axis, because the hand
 * that lifts it also tilts it. 200 mg is comfortably above the noise and below
 * anything deliberate - and deliberately not so sensitive that a lorry going
 * past the window turns the screen on.
 */
#define MOTION_MG 200

/** Where the saved timeout lives. Minutes, 0 for never. */
#define SETTING_IDLE "idlemin"

static volatile bool    s_off;
static volatile bool    s_wake_req;
static volatile int64_t s_last_activity_ms;
static int              s_timeout_min;

/* Gravity as it was when the screen went off, for the comparison above. */
static int16_t s_rest_x, s_rest_y, s_rest_z;
static bool    s_rest_ok;

static int64_t now_ms(void)
{
    return (int64_t)(neos_uptime_ms());
}

/* ------------------------------------------------------------------ */
/* Off and on                                                          */
/* ------------------------------------------------------------------ */

/*
 * Both transitions happen on the watcher task and nowhere else.
 *
 * Not tidiness. The first version turned the screen back on from inside the
 * touch task, which is the task that notices the finger - so anything that
 * blocked in there took input with it, and a wake that failed left a tablet
 * with a dark screen and no way to ask again. Doing it here means the worst a
 * broken wake can cost is the screen: touch keeps being polled, the app keeps
 * running, and the next poll tries again.
 */
static void screen_down(void)
{
    /*
     * The accelerometer is read before the light goes out rather than after, so
     * the resting position is the one the tablet was in while it was still being
     * looked at. A read that fails leaves wake-on-movement out of action for
     * this sleep, which is the honest outcome on a board with no working IMU -
     * touch still wakes it.
     */
    s_rest_ok = neos_imu_accel_mg(&s_rest_x, &s_rest_y, &s_rest_z);

    /*
     * The backlight, and only the backlight.
     *
     * The panel has a DISPOFF command and this used to send it as well, on the
     * grounds that a panel which has stopped driving its source lines costs less
     * than one still scanning PSRAM behind a dark backlight. On this tablet that
     * is a trap. The second board revision uses an ST7123, which is a TDDI part:
     * one chip is both the display driver and the touch controller - see
     * bsp_display.c, "LCD ST7123, Touch ST7123". Tell it to stop displaying and
     * it stops reporting touches, so the screen goes dark and the one input that
     * is supposed to bring it back no longer arrives. Measured, not guessed: with
     * DISPOFF sent, picking the tablet up woke it and touching it did not.
     *
     * Zero duty on the backlight is a LEDC register write with nothing to
     * sequence and nothing to fail, which is what a sleep has to be. The panel
     * keeps scanning; that is the price.
     */
    neos_backlight_blank(true);
    s_off = true;
    ESP_LOGI(TAG, "screen off");
}

static void screen_up(void)
{
    s_off      = false;
    s_wake_req = false;
    s_last_activity_ms = now_ms();

    neos_backlight_blank(false);

    /*
     * The framebuffer holds whatever was on the glass when it went dark, and the
     * bar stopped repainting itself while it was off - so the clock in the corner
     * is as stale as the sleep was long. Pushing the whole screen back is what
     * makes waking up look like nothing happened rather than like the last app
     * redrawing itself a piece at a time.
     */
    ngl_bar_paint();
    ngl_dirty_all();
    ngl_flush();

    ESP_LOGI(TAG, "screen on");
}

void neos_screen_off(void)
{
    if (s_off) {
        return;
    }
    screen_down();
}

void neos_screen_on(void)
{
    s_last_activity_ms = now_ms();
    if (s_off) {
        /*
         * Asked for, not done here. This is called from whichever task noticed -
         * touch, an app, the console - and the one task that may work the
         * display is the watcher, which will pick this up within
         * POLL_ASLEEP_MS.
         */
        s_wake_req = true;
    }
}

bool neos_screen_is_off(void)
{
    return s_off;
}

void neos_idle_poke(void)
{
    neos_screen_on();       /* which stamps the clock whether it was off or not */
}

/* ------------------------------------------------------------------ */
/* The setting                                                         */
/* ------------------------------------------------------------------ */

int neos_idle_timeout_min(void)
{
    return s_timeout_min;
}

bool neos_idle_timeout_set(int minutes)
{
    if (minutes < 0 || minutes > NEOS_IDLE_MAX_MIN) {
        return false;
    }

    /*
     * The write only happens when the value actually moved.
     *
     * This is on a slider, and a slider hands over a value on every poll of the
     * finger - forty a second, all of them the same number while the knob sits
     * between two steps. Every one of those would be a flash commit. The
     * backlight solves the same problem with a settle timer because its value is
     * continuous; eleven positions do not need one, they just need not to write
     * the same minute eleven hundred times.
     */
    if (minutes == s_timeout_min) {
        s_last_activity_ms = now_ms();
        return true;
    }
    s_timeout_min = minutes;
    neos_setting_set_u8(SETTING_IDLE, (uint8_t)minutes);

    /*
     * Changing it counts as activity. Somebody who has just dragged the slider
     * is somebody who is here, and a tablet that went dark a moment after being
     * told to wait five minutes would be reading the setting right and the room
     * wrong.
     */
    s_last_activity_ms = now_ms();
    ESP_LOGI(TAG, "sleep after %d min%s", minutes, minutes ? "" : " (never)");
    return true;
}

/* ------------------------------------------------------------------ */
/* The watcher                                                         */
/* ------------------------------------------------------------------ */

/** Has the tablet been moved since the screen went off? */
static bool moved(void)
{
    if (!s_rest_ok) {
        return false;
    }
    int16_t x = 0, y = 0, z = 0;
    if (!neos_imu_accel_mg(&x, &y, &z)) {
        return false;
    }
    return abs(x - s_rest_x) > MOTION_MG ||
           abs(y - s_rest_y) > MOTION_MG ||
           abs(z - s_rest_z) > MOTION_MG;
}

static void screen_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_off ? POLL_ASLEEP_MS : POLL_AWAKE_MS));

        if (s_off) {
            if (s_wake_req) {
                screen_up();
            } else if (moved()) {
                ESP_LOGI(TAG, "picked up");
                screen_up();
            }
            continue;
        }

        if (s_timeout_min <= 0) {
            continue;
        }
        const int64_t idle = now_ms() - s_last_activity_ms;
        if (idle >= (int64_t)s_timeout_min * 60000) {
            ESP_LOGI(TAG, "idle for %d min", s_timeout_min);
            screen_down();
        }
    }
}

void neos_screen_init(void)
{
    uint8_t stored = 0;
    if (neos_setting_u8(SETTING_IDLE, &stored) && stored <= NEOS_IDLE_MAX_MIN) {
        s_timeout_min = stored;
    }
    s_last_activity_ms = now_ms();

    /*
     * Small stack, low priority. All this task does is subtract two numbers,
     * occasionally read six bytes over I2C, and once in a while write one LEDC
     * register - it must never be what is in front of the touch poller.
     */
    xTaskCreate(screen_task, "screen", 3072, NULL, 2, NULL);

    ESP_LOGI(TAG, "idle watcher up, sleep after %d min%s",
             s_timeout_min, s_timeout_min ? "" : " (never)");
}
