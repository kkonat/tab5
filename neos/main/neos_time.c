/*
 * Clock arithmetic and the zone.
 *
 * The calendar conversions are done here rather than through mktime() and
 * localtime() because the zone this system has is one number, set by hand, and
 * pushing it through a TZ string and the newlib zone machinery would mean
 * formatting a POSIX string, calling tzset(), and then trusting a rule table
 * to agree with what the owner chose on the clock page. Two functions of
 * twenty lines are less code than that and cannot disagree with the setting.
 *
 * The days-from-civil algorithm is Howard Hinnant's, which is exact for every
 * year the RX8130 can hold and involves no tables and no loops.
 */
#include <string.h>

#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "neos_settings.h"
#include "neos_sys.h"
#include "neos_time.h"

static const char *TAG = "time";

#define SETTING_TZ  "tzmin"
#define SETTING_DST "tzdst"

static int      s_tz_min;          /* standard offset from UTC, in minutes */
static bool     s_dst;
static bool     s_synced;
static int64_t  s_sync_us;         /* esp_timer stamp of the last SNTP set */

/* ------------------------------------------------------------------ */
/* Civil <-> epoch                                                     */
/* ------------------------------------------------------------------ */

/**
 * Days since 1970-01-01 for a proleptic Gregorian date.
 *
 * The trick is to shift the year so it starts in March: leap day then lands at
 * the end of the shifted year and stops being a special case anywhere in the
 * arithmetic.
 */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                              /* 0..399 */
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/** The inverse. */
static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                        /* 0..146096 */
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy  = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;                          /* 0..11 */
    *d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    *m = (unsigned)(mp + (mp < 10 ? 3 : -9));
    *y = (int)(yy + (*m <= 2));
}

/** Broken-down fields, read as UTC, to seconds since 1970. */
static int64_t epoch_from(const neos_rtc_t *t)
{
    return days_from_civil(t->year, t->month, t->day) * 86400
         + (int64_t)t->hour * 3600 + (int64_t)t->min * 60 + t->sec;
}

/** Seconds since 1970 to broken-down fields, as UTC. */
static void fields_from(int64_t epoch, neos_rtc_t *out)
{
    int64_t days = epoch / 86400;
    int64_t rem  = epoch % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    int y = 0;
    unsigned mo = 0, d = 0;
    civil_from_days(days, &y, &mo, &d);

    memset(out, 0, sizeof(*out));
    out->year  = (int16_t)y;
    out->month = (uint8_t)mo;
    out->day   = (uint8_t)d;
    out->hour  = (uint8_t)(rem / 3600);
    out->min   = (uint8_t)((rem / 60) % 60);
    out->sec   = (uint8_t)(rem % 60);
    /* 1970-01-01 was a Thursday, which is where the 4 comes from. The extra
       +7 is for dates before the epoch, where the remainder goes negative. */
    out->wday  = (uint8_t)(((days % 7) + 11) % 7);
}

/* ------------------------------------------------------------------ */
/* The clocks                                                          */
/* ------------------------------------------------------------------ */

static int64_t now_utc(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

static void set_utc(int64_t epoch)
{
    const struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/** Write the given UTC instant to the RTC, as local time. */
static bool rtc_write_local(int64_t utc)
{
    neos_rtc_t local;
    fields_from(utc + (int64_t)neos_tz_total_min() * 60, &local);
    return neos_rtc_set(&local);
}

/*
 * Has the system clock ever been set?
 *
 * A P4 that has just powered up starts its clock at the epoch, so any year
 * before the one this firmware was written in means nobody has told it
 * anything. Cheaper and more honest than a flag, which would have to be right
 * across a software restart that keeps the clock running.
 */
static bool clock_is_set(void)
{
    return now_utc() > 1735689600;      /* 2025-01-01 */
}

bool neos_time_utc(neos_rtc_t *out)
{
    if (!out || !clock_is_set()) {
        return false;
    }
    fields_from(now_utc(), out);
    return true;
}

bool neos_time_local(neos_rtc_t *out)
{
    if (!out) {
        return false;
    }
    if (clock_is_set()) {
        fields_from(now_utc() + (int64_t)neos_tz_total_min() * 60, out);
        return true;
    }
    /* No system clock yet: the chip is the only thing that knows, and what it
       holds is already local. */
    return neos_rtc_read(out);
}

bool neos_time_synced(void)
{
    return s_synced;
}

uint32_t neos_time_since_sync_s(void)
{
    if (!s_synced) {
        return 0;
    }
    return (uint32_t)((esp_timer_get_time() - s_sync_us) / 1000000);
}

/* ------------------------------------------------------------------ */
/* The zone                                                            */
/* ------------------------------------------------------------------ */

int neos_tz_offset_min(void)
{
    return s_tz_min;
}

bool neos_tz_dst(void)
{
    return s_dst;
}

int neos_tz_total_min(void)
{
    return s_tz_min + (s_dst ? 60 : 0);
}

/*
 * Changing the zone does not change the instant.
 *
 * The system clock is UTC and stays exactly where it is; what moves is the
 * wall time, and the only thing that stores wall time is the RTC. So the chip
 * is rewritten and nothing else happens - which is also why setting the zone
 * on a tablet that has never known the time is a no-op rather than an error.
 */
static void zone_changed(void)
{
    if (clock_is_set()) {
        rtc_write_local(now_utc());
    }
    ESP_LOGI(TAG, "zone now UTC%+d:%02d%s",
             neos_tz_total_min() / 60, (neos_tz_total_min() % 60 + 60) % 60,
             s_dst ? " (DST)" : "");
}

void neos_tz_offset_set(int minutes)
{
    /* Real zones run from -12:00 to +14:00; anything outside that is a slip in
       the caller, and a clock quietly six hours out is hard to attribute. */
    if (minutes < -720 || minutes > 840 || minutes == s_tz_min) {
        return;
    }
    s_tz_min = minutes;
    neos_setting_set_i32(SETTING_TZ, minutes);
    zone_changed();
}

void neos_tz_dst_set(bool on)
{
    if (on == s_dst) {
        return;
    }
    s_dst = on;
    neos_setting_set_u8(SETTING_DST, on ? 1 : 0);
    zone_changed();
}

/* ------------------------------------------------------------------ */
/* Setting it                                                          */
/* ------------------------------------------------------------------ */

void neos_time_set_utc(int64_t epoch_s)
{
    set_utc(epoch_s);
    s_synced  = true;
    s_sync_us = esp_timer_get_time();

    neos_rtc_t local;
    fields_from(epoch_s + (int64_t)neos_tz_total_min() * 60, &local);
    if (neos_rtc_set(&local)) {
        ESP_LOGI(TAG, "clock set from the network: %04d-%02u-%02u %02u:%02u:%02u local",
                 local.year, local.month, local.day, local.hour, local.min, local.sec);
    } else {
        ESP_LOGW(TAG, "clock set, but the RTC would not take it - it will not survive a power cut");
    }
}

bool neos_time_set_local(const neos_rtc_t *t)
{
    if (!t) {
        return false;
    }
    /*
     * The weekday is derived rather than trusted. It is the one field a user
     * setting a date has no way to get right, and the one that has to agree
     * with the other six or the calendar page draws the month wrong.
     */
    neos_rtc_t fixed = *t;
    const int64_t local_epoch = epoch_from(&fixed);
    fields_from(local_epoch, &fixed);

    const int64_t utc = local_epoch - (int64_t)neos_tz_total_min() * 60;
    set_utc(utc);

    /* Set by hand is not synced. The clock page says which, because a time
       somebody typed and a time from a stratum-2 server are not the same
       claim about how right it is. */
    s_synced = false;
    return neos_rtc_set(&fixed);
}

/* ------------------------------------------------------------------ */

void neos_time_init(void)
{
    int32_t tz = 0;
    if (neos_setting_i32(SETTING_TZ, &tz)) {
        s_tz_min = (int)tz;
    }
    uint8_t dst = 0;
    if (neos_setting_u8(SETTING_DST, &dst)) {
        s_dst = dst != 0;
    }

    /*
     * Seed the system clock from the chip.
     *
     * Without this the machine runs from the epoch until a network turns up,
     * which on a tablet that mostly has no network is most of its life - and
     * every timestamp, every log line and the clock in the system bar would be
     * wrong in a way the RTC could have prevented.
     */
    neos_rtc_t rtc;
    if (neos_rtc_read(&rtc)) {
        set_utc(epoch_from(&rtc) - (int64_t)neos_tz_total_min() * 60);
        ESP_LOGI(TAG, "clock seeded from the RTC: %04d-%02u-%02u %02u:%02u:%02u local, "
                      "UTC%+d:%02d%s",
                 rtc.year, rtc.month, rtc.day, rtc.hour, rtc.min, rtc.sec,
                 neos_tz_total_min() / 60, (neos_tz_total_min() % 60 + 60) % 60,
                 s_dst ? " DST" : "");
    } else {
        ESP_LOGW(TAG, "the RTC has no usable time - the clock starts at the epoch");
    }
}
