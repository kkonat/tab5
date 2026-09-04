#include <math.h>
#include <inttypes.h>

#include "neos_orient.h"
#include "esp_log.h"
#include "bmi270.h"
#include "bsp/m5stack_tab5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "orient";

/*
 * Below this much in-plane gravity the tablet is effectively flat and the
 * accelerometer cannot say which way is "up" on screen. 0.35 g is roughly a
 * 20 degree tilt.
 */
#define FLAT_THRESHOLD_G 0.35f

static bmi270_handle_t *s_imu;
static ngl_rotation_t    s_rot = NGL_ROT_0;
static bool             s_have_confident_reading;
static bool             s_locked;
static void           (*s_on_change)(ngl_rotation_t);
static TaskHandle_t     s_watch;
static uint32_t         s_watch_period_ms;

esp_err_t neos_orient_init(void)
{
    const bmi270_driver_config_t drv = {
        .addr      = BMI270_I2C_ADDRESS_L,
        .interface = BMI270_USE_I2C,
        .i2c_bus   = bsp_i2c_get_handle(),
    };

    esp_err_t err = bmi270_create(&drv, &s_imu);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bmi270_create: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id = 0;
    if (bmi270_get_chip_id(s_imu, &id) == ESP_OK) {
        ESP_LOGI(TAG, "BMI270 chip id 0x%02X", id);
    }

    const bmi270_config_t cfg = {
        .acce_odr   = BMI270_ACC_ODR_50_HZ,
        .acce_range = BMI270_ACC_RANGE_2_G,
        .gyro_odr   = BMI270_GYR_ODR_50_HZ,
        .gyro_range = BMI270_GYR_RANGE_2000_DPS,
    };
    err = bmi270_start(s_imu, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bmi270_start: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "accelerometer running at 50 Hz, +/-2 g");
    return ESP_OK;
}

esp_err_t neos_orient_read(float *x, float *y, float *z)
{
    if (!s_imu) {
        return ESP_ERR_INVALID_STATE;
    }
    float ax = 0, ay = 0, az = 0;
    esp_err_t err = bmi270_get_acce_data(s_imu, &ax, &ay, &az);
    if (x) { *x = ax; }
    if (y) { *y = ay; }
    if (z) { *z = az; }
    return err;
}

esp_err_t neos_orient_read_gyro(float *x, float *y, float *z)
{
    if (!s_imu) {
        return ESP_ERR_INVALID_STATE;
    }
    float gx = 0, gy = 0, gz = 0;
    esp_err_t err = bmi270_get_gyro_data(s_imu, &gx, &gy, &gz);
    if (x) { *x = gx; }
    if (y) { *y = gy; }
    if (z) { *z = gz; }
    return err;
}

/*
 * Map gravity to a screen rotation.
 *
 * The panel is natively portrait 720x1280. Whichever in-plane axis carries the
 * most gravity is pointing down, and that tells us which edge of the panel is
 * currently at the bottom.
 *
 * The signs here are calibrated against the physical board - see
 * docs/orientation.md for the measurements they came from.
 */
static ngl_rotation_t classify(float x, float y)
{
    /*
     * Pick the rotation whose "logical down" points the same way as gravity.
     * From phys_index(): ROT_0 -> +y, ROT_180 -> -y, ROT_90 -> -x, ROT_270 -> +x.
     */
    if (fabsf(x) >= fabsf(y)) {
        return x > 0 ? NGL_ROT_270 : NGL_ROT_90;
    }
    return y > 0 ? NGL_ROT_0 : NGL_ROT_180;
}

bool neos_orient_update(void)
{
    float x = 0, y = 0, z = 0;
    if (neos_orient_read(&x, &y, &z) != ESP_OK) {
        return false;
    }

    const float inplane = sqrtf(x * x + y * y);
    if (inplane < FLAT_THRESHOLD_G) {
        /* Lying flat: no usable signal. Hold whatever we last knew. */
        return false;
    }

    const ngl_rotation_t r = classify(x, y);
    s_have_confident_reading = true;
    if (r != s_rot) {
        ESP_LOGI(TAG, "orientation %s -> %s  (x=%.2f y=%.2f z=%.2f)",
                 neos_orient_name(s_rot), neos_orient_name(r), x, y, z);
        s_rot = r;
        return true;
    }
    return false;
}

ngl_rotation_t neos_orient_get(void)
{
    return s_rot;
}

const char *neos_orient_name(ngl_rotation_t r)
{
    switch (r) {
    case NGL_ROT_0:   return "portrait";
    case NGL_ROT_90:  return "landscape-left";
    case NGL_ROT_180: return "portrait-flipped";
    case NGL_ROT_270: return "landscape-right";
    default:         return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Auto-rotation                                                       */
/* ------------------------------------------------------------------ */

void neos_orient_lock(ngl_rotation_t r)
{
    s_locked = true;
    s_rot = r;
    /*
     * Driven off the display rotation, not off s_rot.
     *
     * s_rot is what gravity last said, which is not the same thing as what
     * the screen is actually showing - a rotation refused while a panel was
     * up, or one this lock is now overriding, leaves the two apart. Comparing
     * against s_rot there means an app that pins the orientation it is
     * already "in" pins nothing: the panel stays where it was, and the system
     * bar - close button included - stays with it, in the pre-freeze place.
     */
    if (ngl_rotation() != r) {
        ngl_set_rotation(r);
        if (s_on_change) {
            s_on_change(r);
        }
    }
    ESP_LOGI(TAG, "orientation locked to %s", neos_orient_name(r));
}

void neos_orient_unlock(void)
{
    if (s_locked) {
        s_locked = false;
        ESP_LOGI(TAG, "orientation unlocked, following gravity again");
    }
}

bool neos_orient_is_locked(void)
{
    return s_locked;
}

static void watch_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(s_watch_period_ms));
        if (s_locked) {
            continue;
        }
        /*
         * Re-offer as well as report changes.
         *
         * ngl refuses a rotation while a modal panel is up, and update() only
         * ever returns true the first time gravity moves - so a refused
         * rotation is never mentioned again and the screen stays behind the
         * tablet until it happens to be turned twice. Comparing against what
         * the display is actually showing catches that on the next tick.
         */
        const bool moved = neos_orient_update();
        if (moved || ngl_rotation() != s_rot) {
            ngl_set_rotation(s_rot);
            if (s_on_change) {
                s_on_change(s_rot);
            }
        }
    }
}

void neos_orient_start_watch(uint32_t period_ms, void (*on_change)(ngl_rotation_t))
{
    s_on_change = on_change;
    s_watch_period_ms = period_ms;

    /* One task for the life of the system. Apps come and go and each one
       registers its own callback, so spawning a task per registration would
       leak a task per app launch - and every dead one would keep calling a
       callback inside an ELF that has since been freed. */
    if (s_watch) {
        ESP_LOGI(TAG, "watch callback replaced, every %" PRIu32 " ms", period_ms);
        return;
    }
    xTaskCreate(watch_task, "orient", 3072, NULL, 4, &s_watch);
    ESP_LOGI(TAG, "watching orientation every %" PRIu32 " ms", period_ms);
}

void neos_orient_stop_watch(void)
{
    /* The task stays; only the callback into app memory is dropped. */
    s_on_change = NULL;
}

