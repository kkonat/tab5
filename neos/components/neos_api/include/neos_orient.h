/*
 * NeOS orientation service.
 *
 * Reads the BMI270 accelerometer and reports which way up the tablet is, so
 * the launcher and the graphics layer can rotate to match.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "ngl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the BMI270 on the BSP I2C bus. */
esp_err_t neos_orient_init(void);

/** Raw accelerometer reading in g. Any pointer may be NULL. */
esp_err_t neos_orient_read(float *x, float *y, float *z);

/**
 * Raw gyroscope reading in degrees per second. Any pointer may be NULL.
 *
 * Nothing in the orientation logic uses it - which way up the tablet is comes
 * from gravity alone - but the part is already running at 50 Hz for that, so
 * the rates are there for the asking and a system readout may as well show
 * them.
 */
esp_err_t neos_orient_read_gyro(float *x, float *y, float *z);

/**
 * Current orientation, as a rotation to apply to the display.
 *
 * When the tablet lies flat, gravity is almost entirely on Z and the in-plane
 * axes carry no usable signal - orientation is genuinely undetermined. In that
 * case this keeps returning the last confident answer rather than flapping.
 */
ngl_rotation_t neos_orient_get(void);

/** Sample the sensor and update the cached orientation. Returns true if it changed. */
bool neos_orient_update(void);

/** Human-readable name, for logs. */
const char *neos_orient_name(ngl_rotation_t r);

/* ------------------------------------------------------------------ */
/* Auto-rotation                                                       */
/* ------------------------------------------------------------------ */

/**
 * Pin the display to one orientation and stop following gravity.
 *
 * For apps that only make sense one way up - a keyboard, a game, a camera
 * viewfinder. The lock is process-wide and the launcher clears it when the app
 * exits, so a crashing app cannot leave the system stuck sideways.
 */
void neos_orient_lock(ngl_rotation_t r);

/** Resume following the accelerometer. */
void neos_orient_unlock(void);

bool neos_orient_is_locked(void);

/**
 * Start the background watcher.
 *
 * Samples every `period_ms` and, when the orientation changes and nothing has
 * locked it, rotates the screen and calls `on_change` so the owner can repaint.
 * Rotation invalidates the whole framebuffer, so repainting is not optional.
 */
void neos_orient_start_watch(uint32_t period_ms, void (*on_change)(ngl_rotation_t));

/**
 * Drop the change callback.
 *
 * Called by the core when an app returns: the callback points into the ELF
 * image that is about to be freed. Rotation itself keeps working.
 */
void neos_orient_stop_watch(void);

#ifdef __cplusplus
}
#endif
