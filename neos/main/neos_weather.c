/*
 * The weather service.
 *
 * One task, two GETs, a struct. It waits for an address, asks ip-api.com where
 * the tablet appears to be, asks open-meteo.com what the weather is there, and
 * then sits on a timer. Everything about it is arranged around the fact that
 * nobody is waiting for the answer: a clock draws whatever it has, so a fetch
 * that fails is a fetch retried later rather than an error anyone sees.
 *
 * Why a task and not the event loop: TLS to open-meteo wants a few kilobytes
 * of stack and a couple of seconds, and the Wi-Fi event loop is not a place to
 * spend either. It also means the network coming and going needs no handling
 * here - the loop looks at neos_net_state() when it wakes and goes back to
 * sleep if there is nothing to talk to.
 *
 * The location is looked up once per boot and kept. It is derived from the
 * address, so it changes when the tablet moves to a different network, which
 * is a reboot or a reconnect away and not worth a poll of its own; a forced
 * refresh re-reads it, which is the escape hatch for the case where it does
 * matter.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "neos_net.h"
#include "neos_weather.h"

static const char *TAG = "weather";

/* Long enough for either response with room to spare; both are one flat
   object and open-meteo's "current" block is under 300 bytes. */
#define BODY_MAX      2048

#define HTTP_TIMEOUT_MS   10000

/* How often a reading is replaced. Open-meteo updates on a 15 minute cadence
   and asks that clients do not poll faster than they publish. */
#define REFRESH_MS    (15 * 60 * 1000)

/* After a failure, rather than the full period - a tablet that has just come
   online should not wait a quarter of an hour for its first reading. */
#define RETRY_MS      (60 * 1000)

/* The loop's own tick. Everything it does is on a minutes-long schedule, so
   this only has to be short enough that a forced refresh feels immediate. */
#define POLL_MS       1000

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static neos_weather_t s_now;
static bool           s_have;
static int64_t        s_landed_us;

/*
 * A sequence counter, odd while s_now is being written.
 *
 * The reading is 44 bytes, so publishing it is several stores and an app can
 * be scheduled in the middle of them - which would hand back yesterday's city
 * with this afternoon's temperature. That is a rare and completely silent kind
 * of wrong, and the fix is four lines, so it is worth having.
 *
 * One writer and any number of readers, so the readers retry rather than lock:
 * nothing here can block the weather task, and a reader that loses a race just
 * copies again a few microseconds later.
 */
static volatile uint32_t s_seq;

/* Where we are, once ip-api has said. Formatted rather than numeric: they only
   ever go back into a URL, and formatting them once here keeps the one float
   in this file out of the fetch path. */
static char s_lat[16], s_lon[16];
static bool s_located;

static volatile bool s_want_refresh;
static volatile bool s_fetching;

static int64_t s_next_us;

/* ------------------------------------------------------------------ */
/* Reading it                                                          */
/* ------------------------------------------------------------------ */

bool neos_weather(neos_weather_t *out)
{
    if (!out || !s_have) {
        return false;
    }

    int64_t landed;
    for (;;) {
        const uint32_t before = s_seq;
        if (before & 1u) {
            continue;               /* a write is in progress */
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        *out   = s_now;
        landed = s_landed_us;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (s_seq == before) {
            break;
        }
    }

    /* Filled at the point of reading rather than kept up to date: the age is
       the one field that is wrong the instant it is stored. */
    out->age_s = (uint32_t)((esp_timer_get_time() - landed) / 1000000);
    return true;
}

/* The other side of it. Everything that touches s_now goes between these. */
static inline void publish_begin(void)
{
    s_seq++;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void publish_end(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    s_seq++;
}

void neos_weather_refresh(void)
{
    s_want_refresh = true;
}

bool neos_weather_fetching(void)
{
    return s_fetching;
}

/* ------------------------------------------------------------------ */
/* HTTP                                                                */
/* ------------------------------------------------------------------ */

/**
 * GET @p url into @p body, NUL-terminated. Returns false on anything at all.
 *
 * esp_http_client_read_response() rather than a read loop because open-meteo
 * answers chunked and a plain read would stop at the first chunk boundary,
 * which is a truncated object that then fails to parse for a reason that looks
 * like the server's fault.
 */
static bool http_get(const char *url, char *body, size_t body_sz)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Both services redirect http->https or bare->www depending on the
           day; following them is cheaper than pinning a form that changes. */
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }

    bool ok = false;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s: %s", url, esp_err_to_name(err));
        goto done;
    }

    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "no headers from %s", url);
        goto done;
    }

    const int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "%s answered %d", url, status);
        goto done;
    }

    const int got = esp_http_client_read_response(c, body, (int)body_sz - 1);
    if (got <= 0) {
        ESP_LOGW(TAG, "empty body from %s", url);
        goto done;
    }
    body[got] = 0;
    ok = true;

done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Where we are                                                        */
/* ------------------------------------------------------------------ */

/**
 * Print a JSON number back out with six decimals.
 *
 * cJSON hands over a double and the coordinates have to go into a URL, so a
 * float reaches printf exactly once, here. Six places is about 10 cm, which is
 * far more than an address-derived position deserves and costs nothing.
 */
static void fmt_coord(char *out, size_t n, double v)
{
    snprintf(out, n, "%.6f", v);
}

static bool locate(void)
{
    static char body[BODY_MAX];

    if (!http_get("http://ip-api.com/json/?fields=status,city,lat,lon",
                  body, sizeof(body))) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "ip-api sent something that is not JSON");
        return false;
    }

    bool ok = false;
    const cJSON *st   = cJSON_GetObjectItemCaseSensitive(root, "status");
    const cJSON *lat  = cJSON_GetObjectItemCaseSensitive(root, "lat");
    const cJSON *lon  = cJSON_GetObjectItemCaseSensitive(root, "lon");
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");

    if (cJSON_IsString(st) && strcmp(st->valuestring, "success") == 0 &&
        cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        fmt_coord(s_lat, sizeof(s_lat), lat->valuedouble);
        fmt_coord(s_lon, sizeof(s_lon), lon->valuedouble);

        publish_begin();
        strlcpy(s_now.place, cJSON_IsString(city) ? city->valuestring : "",
                sizeof(s_now.place));
        publish_end();
        ok = true;
        ESP_LOGI(TAG, "located: %s at %s,%s",
                 s_now.place[0] ? s_now.place : "(unnamed)", s_lat, s_lon);
    } else {
        ESP_LOGW(TAG, "ip-api would not say where we are");
    }

    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------------ */
/* What it is doing there                                              */
/* ------------------------------------------------------------------ */

static bool observe(void)
{
    static char body[BODY_MAX];
    char url[192];

    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code,is_day",
             s_lat, s_lon);

    if (!http_get(url, body, sizeof(body))) {
        return false;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "open-meteo sent something that is not JSON");
        return false;
    }

    bool ok = false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cJSON_IsObject(cur)) {
        const cJSON *t   = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
        const cJSON *w   = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
        const cJSON *day = cJSON_GetObjectItemCaseSensitive(cur, "is_day");

        if (cJSON_IsNumber(t) && cJSON_IsNumber(w)) {
            /* The one place a float becomes an integer, so that nothing past
               this line has to have an opinion about rounding. */
            const double c10 = t->valuedouble * 10.0;

            publish_begin();
            s_now.temp_c10 = (int16_t)(c10 >= 0 ? c10 + 0.5 : c10 - 0.5);
            s_now.code     = (uint16_t)w->valueint;
            s_now.is_day   = cJSON_IsNumber(day) ? (day->valueint != 0) : 1;
            s_landed_us    = esp_timer_get_time();
            publish_end();

            /* Last, and after the sequence has gone even again, so that a
               reader which sees s_have can never be looking at a half-written
               reading. */
            s_have = true;
            ok     = true;

            ESP_LOGI(TAG, "%d.%d C, WMO %u, %s",
                     s_now.temp_c10 / 10, (s_now.temp_c10 < 0 ? -s_now.temp_c10 : s_now.temp_c10) % 10,
                     (unsigned)s_now.code, s_now.is_day ? "day" : "night");
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "open-meteo answered without a current reading");
    }

    cJSON_Delete(root);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        const bool forced = s_want_refresh;
        const int64_t now = esp_timer_get_time();

        if (!forced && now < s_next_us) {
            continue;
        }
        if (neos_net_state() != NEOS_NET_ONLINE) {
            /* Not a failure and not worth backing off for: the moment an
               address arrives, the next tick fetches. */
            continue;
        }

        s_want_refresh = false;
        s_fetching     = true;

        /* A forced refresh re-locates as well, because "the weather here is
           wrong" and "here is wrong" are the same complaint from the outside. */
        if (forced || !s_located) {
            s_located = locate();
        }

        const bool ok = s_located && observe();
        s_fetching = false;

        s_next_us = esp_timer_get_time() +
                    (int64_t)(ok ? REFRESH_MS : RETRY_MS) * 1000;
    }
}

void neos_weather_init(void)
{
    /* 6 KB: mbedtls needs most of it for the handshake to open-meteo, and this
       task does nothing else, so the peak is the budget. */
    xTaskCreate(weather_task, "weather", 6144, NULL, 3, NULL);
    ESP_LOGI(TAG, "waiting for a network");
}
