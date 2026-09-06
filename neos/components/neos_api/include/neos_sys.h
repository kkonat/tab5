/*
 * NeOS system readouts - what an app can see and change about the machine.
 *
 * The rest of the ABI is about drawing and about apps. This half is about the
 * board: the accelerometer, the power monitor, the RTC, the card, the I2C bus
 * and the handful of rails that can be switched on and off. It exists because
 * an app runs with no drivers of its own - it cannot open I2C or reach the io
 * expander - so anything it wants to know about the hardware comes through
 * here or not at all.
 *
 * Nothing here trades in floats. Values cross as scaled integers - millivolts,
 * milliamps, milli-g, tenths of a degree - which keeps apps free of
 * double-precision printf and of any question about how a float is passed
 * between two separately linked images. Divide at the point of display.
 *
 * Every reader is non-blocking and safe to call from a draw loop: a device
 * that is absent or wedged reports failure rather than stalling, and the
 * caller shows a dash instead of a number. The one exception is documented
 * where it lives - neos_i2c_scan().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Probe the board and start the readers. Called once by NeOS during bring-up,
 * after the I2C bus is up. Not exported to apps - by the time one runs, this
 * has already happened.
 */
void neos_sys_init(void);

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

/* These return storage NeOS owns and never frees, stable for the life of the
   boot, so an app may hold the pointer rather than copying the string. */

/** "ESP32-P4 rev v1.3". */
const char *neos_chip(void);

/** Base MAC, formatted "80:f1:b2:d1:41:b2". */
const char *neos_mac(void);

/** Why the machine last restarted, in the words the boot log uses. */
const char *neos_reset_reason(void);

/** The ESP-IDF this firmware was built against, e.g. "v5.4.2". */
const char *neos_idf_version(void);

/**
 * Which NeOS build this is, e.g. "v0.42".
 *
 * A counter bumped once per firmware build, which is the same string the
 * system bar shows - so a details screen and the badge in the corner cannot
 * disagree about what is running.
 */
const char *neos_build(void);

/** When that build was compiled, as "Sep  4 2026 01:12:33". */
const char *neos_build_date(void);

/**
 * The ABI the firmware implements: major in the high 16 bits, minor in the low.
 *
 * Worth showing next to an app's own NEOS_ABI_MAJOR/MINOR, which is the
 * version it was compiled against. The two differing is normal and is exactly
 * what the guard chain in neos_abi.h exists to make safe - a difference in the
 * major is the one that would have refused to load.
 */
uint32_t neos_abi(void);

uint16_t neos_cpu_mhz(void);
uint8_t  neos_cores(void);

/** Milliseconds since boot - the machine's clock, not this app's. */
uint64_t neos_uptime_ms(void);

/**
 * Microseconds since boot, from the same timer neos_uptime_ms() divides down.
 *
 * Milliseconds cannot describe a 16.6 ms frame: a budget measured in them is
 * a number between 16 and 17, and the interesting part of an emulator's loop
 * is the part that rounds away. This is the raw counter, for pacing a frame
 * and for finding out where the time went inside one.
 *
 * Subtracting two of these is free, but dividing one in an app is not - see
 * neos_uptime_s() for why. Anything that wants a rate should scale the
 * difference, which fits in 32 bits for over an hour.
 */
uint64_t neos_uptime_us(void);

/**
 * The same thing in seconds.
 *
 * Not a convenience. Apps link -nostdlib and resolve against the syscall
 * table, which carries no compiler runtime, so dividing a 64-bit value by
 * 1000 in an app is a link error against __udivdi3 rather than a slow
 * instruction. Anything displaying an uptime wants seconds, and seconds fit
 * in 32 bits for the next century, so the division happens on this side once
 * instead of being a trap every app walks into.
 */
uint32_t neos_uptime_s(void);

/*
 * Internal RAM and PSRAM, in bytes.
 *
 * Total is what the heap was given, not what the chip carries: the
 * framebuffers and the loaded app image are already out of it by the time
 * anything asks, and a free and a total that disagreed about which pool they
 * described would be worse than no number at all.
 */
uint32_t neos_heap_free(void);
uint32_t neos_heap_total(void);
uint32_t neos_psram_free(void);
uint32_t neos_psram_total(void);

/**
 * Allocate @p n bytes of internal SRAM, 64-byte aligned. Released with free().
 *
 * malloc() on this machine is PSRAM for anything over a kilobyte, which is the
 * right default - there are 32 MB of it and 32 MB is what makes an app able to
 * hold a picture. It is the wrong default for the few bytes an app touches
 * millions of times a second: an emulator's address space, its tile ROM, the
 * frame it is composing. Those want to be in the internal pool, where a miss
 * costs a cache line and not a PSRAM burst.
 *
 * NULL when the pool cannot cover it, and deliberately not a quiet fall back
 * to PSRAM: an app that asked for this asked because the difference matters,
 * and one that is silently given the slow memory has no way to find out. Check
 * neos_heap_free() first if the answer changes what you would allocate.
 *
 * The alignment is a cache line, so a block from here can be a PPA source -
 * see ngl_blit_scale(). Internal RAM is scarce; this is not the allocator to
 * reach for by default.
 */
void *neos_alloc_fast(size_t n);

/* ------------------------------------------------------------------ */
/* Sensors                                                             */
/* ------------------------------------------------------------------ */

/**
 * Accelerometer, in milli-g. 1000 on an axis means gravity lies along it.
 *
 * The same BMI270 the orientation service reads, at the rate that service
 * configured. This does not start a conversion, it collects the latest one,
 * so polling it every frame costs one I2C read and no waiting.
 */
bool neos_imu_accel_mg(int16_t *x, int16_t *y, int16_t *z);

/** Gyroscope, in degrees per second. */
bool neos_imu_gyro_dps(int16_t *x, int16_t *y, int16_t *z);

/** Die temperature in tenths of a degree C. INT16_MIN if unavailable. */
int16_t neos_die_temp_c10(void);

/**
 * The battery-backed RTC.
 *
 * wday is 0-6 from Sunday. A cell that has never been set reads back as
 * something implausible, which this rejects rather than passing on - so false
 * means "no usable time", not only "no chip".
 */
typedef struct {
    int16_t year;                       /**< full year, e.g. 2026 */
    uint8_t month, day, hour, min, sec, wday;
    uint8_t reserved;
} neos_rtc_t;

bool neos_rtc_read(neos_rtc_t *out);

/**
 * Set the battery-backed RTC to a wall-clock time.
 *
 * The chip holds *local* time, not UTC. It is the tablet's clock of last
 * resort - the thing that still knows what time it is after a week in a
 * drawer with no network - and every reader of it, from the system bar to a
 * file timestamp, wants the time a person would say. The conversion from
 * network time happens once, on the way in; see neos_time.h.
 *
 * wday is 0-6 from Sunday and is written as given, not derived, because the
 * caller already had to work out the date and this is not the place to have a
 * second opinion about it. False if the values are not a real time, or if
 * there is no chip.
 */
bool neos_rtc_set(const neos_rtc_t *t);

/* ------------------------------------------------------------------ */
/* Tap sounds                                                          */
/* ------------------------------------------------------------------ */

/**
 * Whether a touch makes a noise.
 *
 * Off unless it has been switched on, and switching it on is what brings the
 * codec up - a tablet nobody asked for sounds from should not be running an
 * I2S channel and holding the speaker amplifier powered.
 *
 * The click is played by NeOS on the press edge of every touch, everywhere,
 * so an app gets it without asking and cannot make it inconsistent with the
 * rest of the system.
 */
bool neos_tap_sound(void);

/** Switch it. False if the audio codec would not come up. Saved to NVS. */
bool neos_tap_sound_set(bool on);

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

/**
 * The only rate there is.
 *
 * The I2S channel is clocked once, at bring-up, and the codec plays whatever
 * it is handed at the rate the channel runs at - so asking for another rate
 * would not resample anything, it would play the right samples at the wrong
 * speed. Rather than offer a parameter that has one legal value, the rate is
 * a constant and resampling is the app's business, where it knows what its
 * source is and can filter accordingly.
 */
#define NEOS_AUDIO_RATE 48000

/**
 * Take the speaker for as long as this app is running.
 *
 * Brings the codec up if tap sounds had not already, and holds it up until
 * neos_audio_close(). False if the part will not play, and an app that wants
 * to be usable on a tablet with no working codec has to carry on without it -
 * so this is worth checking rather than assuming.
 *
 * Tap clicks are dropped while a stream is open. Two writers into one codec
 * would interleave into each other's buffers, and a click under a game's own
 * sound is not something anyone would hear anyway.
 *
 * Idempotent: a second open by an app that already holds the stream is a
 * no-op returning true. NeOS closes the stream when the app returns from
 * main(), so an app that exits without closing does not leave the amplifier
 * powered - but closing is still the honest thing to do.
 */
bool neos_audio_open(void);

/**
 * Play @p n mono 16-bit frames at NEOS_AUDIO_RATE.
 *
 * Blocks until the codec has taken them, which makes it the app's frame
 * clock: an emulator or a synth can drive its whole loop off the rate audio
 * drains at and never needs a timer. Returns the number of frames accepted,
 * or negative if the stream is not open.
 *
 * There is no queue and no callback. A block that is late is a gap in the
 * sound, and an app that cannot keep up wants to find that out by blocking
 * here rather than by discovering its buffer was dropped.
 */
int neos_audio_write(const int16_t *frames, int n);

/**
 * Microseconds of sound handed over that the speaker has not played yet.
 *
 * The cushion, in other words, and the only thing worth knowing before
 * deciding whether there is room to do something expensive this frame. An app
 * driving itself off neos_audio_write() is asleep inside that call whenever it
 * is ahead; what it cannot tell from there is how far ahead, and so whether
 * the full repaint it was about to do will be paid for out of the cushion or
 * out of the sound.
 *
 * Zero means the next block is already late. There is no free-space query
 * underneath this - the codec layer does not offer one - so the figure is the
 * sound written minus the time elapsed, which is the same quantity as long as
 * writes block, and is re-based rather than allowed to go negative when they
 * have not kept up.
 *
 * Zero when no stream is open, so an app that never opened one is not a
 * special case.
 */
int32_t neos_audio_lead_us(void);

/**
 * Output level, 0-100, applied to the codec and so to whatever plays next.
 *
 * Reset to the system's own level when the stream closes, so an app cannot
 * leave the tablet louder than it found it. False if the stream is not open.
 */
bool neos_audio_gain(uint8_t percent);

/** Give the speaker back. Safe to call without a stream open. */
void neos_audio_close(void);

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */

/**
 * The INA226 across the main rail.
 *
 * shunt_uv is what the part actually measures and is exact. current_ma and
 * power_mw are derived from it through the shunt resistance, which is a
 * constant in the firmware rather than something the chip can be asked - so
 * if the amps ever read off by a clean factor, that constant is the suspect.
 * See neos_sys.c.
 */
typedef struct {
    int32_t bus_mv;
    int32_t shunt_uv;
    int32_t current_ma;
    int32_t power_mw;
} neos_power_t;

bool neos_power_read(neos_power_t *out);

/** Where the monitor answered on the I2C bus, or -1 if nothing did. */
int neos_power_monitor_addr(void);

/* ------------------------------------------------------------------ */
/* Display backlight                                                   */
/* ------------------------------------------------------------------ */

/** Current backlight duty, 0-100. */
int neos_backlight(void);

/**
 * Set the backlight, 0-100, clamped to a floor above zero.
 *
 * A screen an app can switch off is a screen nobody can switch back on: the
 * only input this machine has is the one you would no longer be able to see.
 */
bool neos_backlight_set(int percent);

/* ------------------------------------------------------------------ */
/* Switchable rails                                                    */
/* ------------------------------------------------------------------ */

/*
 * The io expander lines apps are allowed to drive.
 *
 * Deliberately not the whole expander. The display and touch enables sit on
 * it too, and an app that can pull either is an app that can end the session
 * until somebody reaches the reset button.
 *
 * The numbers are ABI - apps switch on them - so append, never renumber.
 */
typedef enum {
    NEOS_FEAT_SPEAKER   = 0,
    NEOS_FEAT_CAMERA    = 1,
    NEOS_FEAT_WIFI      = 2,   /**< the ESP32-C6 radio co-processor */
    NEOS_FEAT_USB_5V    = 3,   /**< 5 V out on the USB-A socket */
    NEOS_FEAT_CHARGE    = 4,   /**< let the battery charge at all */
    NEOS_FEAT_CHARGE_QC = 5,   /**< fast charge, when the supply offers it */
    NEOS_FEAT_EXT_5V    = 6,   /**< 5 V out on the external port */
    NEOS_FEAT_ANTENNA   = 7,   /**< external Wi-Fi antenna instead of internal */
    NEOS_FEAT_COUNT
} neos_feature_t;

/** Whether the rail is on. Reads the expander, falling back to the last write. */
bool neos_feature(neos_feature_t f);

/**
 * Switch a rail. False if the expander did not take it.
 *
 * The new state is remembered across reboots - see neos_setting_reset() for
 * what that means and how to undo it.
 */
bool neos_feature_set(neos_feature_t f, bool on);

/** Short display name, e.g. "Speaker". */
const char *neos_feature_name(neos_feature_t f);

/**
 * Drive the enable line and nothing else. NeOS only, not exported to apps.
 *
 * neos_feature_set() sends NEOS_FEAT_WIFI to the network service instead of to
 * the pin, because the radio needs its transport stopped before it loses
 * power. This is what that service uses once it has done so.
 */
bool neos_feature_set_raw(neos_feature_t f, bool on);

/* ------------------------------------------------------------------ */
/* Settings that outlive the app that changed them                     */
/* ------------------------------------------------------------------ */

/**
 * Forget every stored setting and go back to what the board does on its own.
 *
 * Backlight and the rails above are saved to NVS when an app changes them and
 * restored during boot, so the machine comes back the way it was left rather
 * than the way an app happened to leave it running. That makes them system
 * settings, not app state - which is also why there has to be a way back:
 * a rail switched off by an app that has since been deleted would otherwise
 * stay off forever with nothing left to explain it.
 *
 * Takes effect at the next boot for anything already applied.
 */
void neos_settings_reset(void);

/* ------------------------------------------------------------------ */
/* The card                                                            */
/* ------------------------------------------------------------------ */

bool     neos_sd_mounted(void);
uint64_t neos_sd_bytes(void);

/** The card's product name, or an empty string when nothing is mounted. */
const char *neos_sd_name(void);

/** "SDHC/SDXC", "SDSC", "MMC", "SDIO", or "" with no card. */
const char *neos_sd_type(void);

/** The bus clock actually negotiated, in kHz. */
uint32_t neos_sd_speed_khz(void);

/** Data lines in use: 4, or 1 on a card that would not do wide mode. */
int neos_sd_bus_width(void);

/** Where the card is mounted, e.g. "/sdcard". */
const char *neos_sd_mount(void);

/**
 * Free space, in bytes.
 *
 * Counted by walking the allocation table, so unlike everything else here it
 * costs real time on a large card - put it behind a tap or a slow refresh, not
 * on a draw loop.
 */
uint64_t neos_sd_free_bytes(void);

/* ------------------------------------------------------------------ */
/* The I2C bus                                                         */
/* ------------------------------------------------------------------ */

/**
 * Probe every 7-bit address and write the ones that answered into addrs.
 *
 * Returns how many were found, which may exceed max - the array fills to max
 * and the count is still the truth. This is the one call here that takes real
 * time and talks to every device on the bus, so it belongs behind a tap, not
 * in a draw loop.
 */
int neos_i2c_scan(uint8_t *addrs, int max);

/** What is known to live at that address on this board, or "" if unrecognised. */
const char *neos_i2c_name(uint8_t addr);

/*
 * Both structs are filled by the firmware into storage the app owns, so their
 * layouts get compiled into every app that reads one. Same rule as
 * neos_app_t: appending a field is a minor, moving one is a major, and this
 * is where you find out rather than in a wrong number on the panel.
 */
_Static_assert(sizeof(neos_rtc_t) == 10, "neos_rtc_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_rtc_t, year)  == 0, "neos_rtc_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_rtc_t, month) == 2, "neos_rtc_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_rtc_t, sec)   == 6, "neos_rtc_t layout is frozen for ABI v1");
_Static_assert(sizeof(neos_power_t) == 16, "neos_power_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_power_t, bus_mv)     ==  0, "neos_power_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_power_t, shunt_uv)   ==  4, "neos_power_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_power_t, current_ma) ==  8, "neos_power_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_power_t, power_mw)   == 12, "neos_power_t layout is frozen for ABI v1");

#ifdef __cplusplus
}
#endif
