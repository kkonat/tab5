/*
 * The system settings store, firmware side.
 *
 * One NVS namespace holds everything the machine remembers about itself
 * across a reboot: the backlight, the switchable rails, whether taps click,
 * the time zone, and the Wi-Fi networks it has been told about. They are kept
 * together because they are erased together - neos_settings_reset() is a
 * single promise that the tablet comes back the way the board leaves it, and
 * that promise is only as good as the number of places settings can hide.
 *
 * Not exported to apps. An app changes a setting by calling the thing the
 * setting is about - neos_backlight_set(), neos_tap_sound_set() - which is
 * what saves it. Handing out a key-value store would make "what does this
 * tablet remember" unanswerable.
 *
 * Every getter leaves its output alone and returns false when the key has
 * never been written. That distinction is the whole design: a first boot must
 * apply nothing rather than apply a default this file invented, or the stored
 * settings stop being the only thing that explains the machine's state.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Open the store. Called once during bring-up, before anything reads it. */
void neos_settings_open(void);

bool neos_setting_u8(const char *key, uint8_t *out);
void neos_setting_set_u8(const char *key, uint8_t v);

bool neos_setting_i32(const char *key, int32_t *out);
void neos_setting_set_i32(const char *key, int32_t v);

/**
 * A blob, for the things that are not a number - a stored network, say.
 *
 * @p len is in-out: the size of the buffer going in, how much was read coming
 * out. False if the key is absent or the value does not fit, which are the
 * same thing to a caller that can only use a whole record.
 */
bool neos_setting_blob(const char *key, void *out, size_t *len);
void neos_setting_set_blob(const char *key, const void *v, size_t len);

/** Forget one key. Erasing a key that was never there is not an error. */
void neos_setting_erase(const char *key);
