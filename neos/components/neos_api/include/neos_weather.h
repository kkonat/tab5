/*
 * What it is doing outside.
 *
 * The same shape as neos_net.h, and for the same reason. There is one radio,
 * one connection and one place worth knowing about, so fetching is NeOS's job
 * and an app reads the answer - it does not open a socket, it does not hold an
 * API key, and two apps showing the weather cannot disagree about it or fetch
 * it twice.
 *
 * It also has to be this side of the boundary on plain mechanical grounds:
 * apps link -nostdlib against a syscall table with no TLS, no HTTP and no JSON
 * in it, and nothing in that list is something the ABI wants to grow.
 *
 * Two services, both free and both without an account:
 *
 *   ip-api.com   turns the address the tablet is using into a latitude, a
 *                longitude and a city name. Over plain HTTP, because the free
 *                tier is HTTP only - which is fine for a coordinate that is
 *                already public to every server the tablet talks to, and is
 *                the reason nothing secret is ever sent to it.
 *
 *   open-meteo.com  turns those coordinates into a temperature and a WMO
 *                weather code, over HTTPS against the IDF certificate bundle.
 *
 * Neither is asked anything about the tablet beyond where it appears to be.
 *
 * The reading crosses as scaled integers, like everything else in neos_sys.h:
 * tenths of a degree, not a float.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One observation.
 *
 * `code` is a WMO 4677 present-weather code, which is what open-meteo speaks:
 * 0 clear, 1-3 increasingly cloudy, 45/48 fog, 51-57 drizzle, 61-67 rain,
 * 71-77 snow, 80-82 showers, 85/86 snow showers, 95-99 thunderstorms.
 *
 * Deliberately handed over raw rather than as an icon name or a sentence. Which
 * picture code 61 is worth, and whether it is drawn differently at night, is a
 * display decision and belongs where the drawing is - the same line neos_net.h
 * draws around signal strength and bars.
 */
typedef struct {
    uint32_t age_s;        /**< seconds since this landed; it is a forecast */
    int16_t  temp_c10;     /**< tenths of a degree C */
    uint16_t code;         /**< WMO 4677 present weather */
    uint8_t  is_day;       /**< the sun is up where this was measured */
    uint8_t  reserved[3];
    char     place[32];    /**< city the address resolved to, or "" */
} neos_weather_t;

/**
 * Start the service. Called once by NeOS during bring-up, after the network.
 *
 * Returns at once. There is nothing to wait for: it starts a task that sits
 * until an address turns up, and on a tablet that never joins a network that
 * task simply never gets past its first wait.
 */
void neos_weather_init(void);

/**
 * The last reading, or false if none has landed.
 *
 * False is the normal state for the first half-minute after a connection and
 * forever without one, so it is a case to draw rather than an error - show
 * nothing, not a zero.
 */
bool neos_weather(neos_weather_t *out);

/**
 * Ask again, now.
 *
 * The service refreshes on its own timer, so nothing has to call this. It is
 * here for the same reason neos_net_sntp_restart() is: a reading somebody can
 * see is stale is one they want fixed while they are looking at it. A no-op
 * when there is no network or a fetch is already running.
 */
void neos_weather_refresh(void);

/** True while a fetch is in flight, so a UI can say so instead of stalling. */
bool neos_weather_fetching(void);

/*
 * Filled by the firmware into storage the app owns, so the layout is compiled
 * into every app that reads one. Same rule as neos_net_ap_t: appending a field
 * is a minor, moving one is a major, and this is where you find out.
 */
_Static_assert(sizeof(neos_weather_t) == 44, "neos_weather_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_weather_t, age_s)    ==  0, "neos_weather_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_weather_t, temp_c10) ==  4, "neos_weather_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_weather_t, code)     ==  6, "neos_weather_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_weather_t, is_day)   ==  8, "neos_weather_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_weather_t, place)    == 12, "neos_weather_t layout is frozen for ABI v1");

#ifdef __cplusplus
}
#endif
