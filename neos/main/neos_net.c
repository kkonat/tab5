/*
 * Wi-Fi, and the clock it sets.
 *
 * The P4 has no radio, so every call below goes over SDIO to the ESP32-C6
 * through ESP-Hosted. From this side that is invisible - the API is the
 * ordinary esp_wifi one - but three things about it are not:
 *
 *   The radio has a power rail on the io expander, and nothing in ESP-Hosted
 *   knows that. It has to be switched on and given a moment before the SDIO
 *   bus is enumerated, or the slave is simply not there.
 *
 *   The C6 runs firmware this project cannot read back or rebuild. If the host
 *   and the slave ever disagree about the protocol, it looks like a slave that
 *   does not answer, and the fix is on the other chip. docs/specs_fingerprint.md
 *   records everything known about the one on this unit.
 *
 *   Failure is normal and has to be a state, not an error return. There is no
 *   point in the boot where "is there Wi-Fi" has an answer, so neos_net_state()
 *   is the whole interface and every caller draws whatever it currently says.
 *
 * Credentials are ours, not the driver's. esp_wifi's own NVS store keeps one
 * network and re-joins it silently, which is the wrong shape for a tablet that
 * moves between places - so storage is set to RAM and the list below is what
 * the system remembers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"

#include "neos_bar.h"
#include "neos_net.h"
#include "neos_settings.h"
#include "neos_status.h"
#include "neos_sys.h"
#include "neos_time.h"

static const char *TAG = "net";

/* ------------------------------------------------------------------ */
/* Remembered networks                                                 */
/* ------------------------------------------------------------------ */

/*
 * Eight, in eight NVS keys named net0..net7.
 *
 * A fixed set of keys rather than one blob: a corrupt or half-written record
 * then costs one network rather than the list, and the whole thing can be read
 * with the same accessor everything else in the settings store uses.
 *
 * Eight because that is a home, an office, a phone hotspot and a few visits,
 * and the list is walked linearly on every scan result.
 */
#define KNOWN_MAX 8

typedef struct {
    char ssid[33];
    char pass[65];
} cred_t;

static cred_t s_known[KNOWN_MAX];
static int    s_nknown;

static void cred_key(int i, char *out, size_t n)
{
    snprintf(out, n, "net%d", i);
}

static void known_load(void)
{
    s_nknown = 0;
    for (int i = 0; i < KNOWN_MAX; i++) {
        char key[8];
        cred_key(i, key, sizeof(key));

        cred_t c;
        size_t len = sizeof(c);
        if (!neos_setting_blob(key, &c, &len) || len != sizeof(c) || !c.ssid[0]) {
            break;      /* the list is dense: the first gap is the end */
        }
        /* A record that has been through a resize or a bad write must not be
           able to run off the end of a print or a strcmp. */
        c.ssid[sizeof(c.ssid) - 1] = 0;
        c.pass[sizeof(c.pass) - 1] = 0;
        s_known[s_nknown++] = c;
    }
    ESP_LOGI(TAG, "%d remembered network%s", s_nknown, s_nknown == 1 ? "" : "s");
}

/** Write the in-memory list back, keys and all, and drop any tail. */
static void known_store(void)
{
    for (int i = 0; i < KNOWN_MAX; i++) {
        char key[8];
        cred_key(i, key, sizeof(key));
        if (i < s_nknown) {
            neos_setting_set_blob(key, &s_known[i], sizeof(cred_t));
        } else {
            neos_setting_erase(key);
        }
    }
}

static int known_index(const char *ssid)
{
    for (int i = 0; i < s_nknown; i++) {
        if (strcmp(s_known[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Most recently used first.
 *
 * Which matters when the list is full: the network that gets dropped is the
 * one that has gone longest without being joined, which on a tablet that
 * travels is almost always the right one to lose.
 */
static void known_remember(const char *ssid, const char *pass)
{
    int at = known_index(ssid);
    if (at < 0) {
        at = s_nknown < KNOWN_MAX ? s_nknown++ : KNOWN_MAX - 1;
    }
    cred_t c = {0};
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    strlcpy(c.pass, pass ? pass : "", sizeof(c.pass));

    memmove(&s_known[1], &s_known[0], (size_t)at * sizeof(cred_t));
    s_known[0] = c;
    known_store();
    ESP_LOGI(TAG, "remembered \"%s\" (%d stored)", ssid, s_nknown);
}

bool neos_net_known(const char *ssid)
{
    return ssid && known_index(ssid) >= 0;
}

int neos_net_known_count(void)
{
    return s_nknown;
}

void neos_net_forget(const char *ssid)
{
    const int at = ssid ? known_index(ssid) : -1;
    if (at < 0) {
        return;
    }
    memmove(&s_known[at], &s_known[at + 1],
            (size_t)(s_nknown - at - 1) * sizeof(cred_t));
    s_nknown--;
    memset(&s_known[s_nknown], 0, sizeof(cred_t));
    known_store();
    ESP_LOGI(TAG, "forgot \"%s\"", ssid);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static neos_net_state_t s_state = NEOS_NET_OFF;
static bool             s_started;

/*
 * What bringup() created, so that powering the radio down can undo it.
 *
 * These used to be discarded - the netif handle thrown away and the event
 * handlers registered with a NULL instance. That is fine for something brought
 * up once and never taken down, and it is exactly what stops it being brought
 * up twice: the second esp_netif_create_default_wifi_sta() on a live netif
 * aborts, so keeping the handles is what makes the radio switchable rather
 * than only startable.
 */
static esp_netif_t                *s_netif;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;

/* Set from any task, acted on by net_task: 1 power up, 0 power down. */
static volatile int s_power_req = -1;

static char s_ssid[33];        /* what we are on, or trying to get on to */
static char s_ip[16];
static int  s_rssi;

/* What the reconnect logic is aiming at, and how many goes it has had. */
static char s_want[33];
static int  s_tries;
static int64_t s_retry_at;
static int64_t s_rescan_at;

static bool          s_scanning;
static neos_net_ap_t s_aps[NEOS_NET_SCAN_MAX];
static int           s_nap;

/*
 * Whether the last scan was asked for by a person.
 *
 * A scan the Wi-Fi panel started must not silently associate with something
 * just because it is known and strong - the user is standing there choosing.
 * A scan this file started on its own is exactly the opposite.
 */
static bool s_scan_is_users;

/* An automatic scan is in flight and its results are ours to act on. */
static bool s_autojoin;

neos_net_state_t neos_net_state(void) { return s_state; }
const char      *neos_net_ssid(void)  { return s_state >= NEOS_NET_CONNECTING ? s_ssid : ""; }
const char      *neos_net_ip(void)    { return s_state == NEOS_NET_ONLINE ? s_ip : ""; }
bool             neos_net_scanning(void) { return s_scanning; }

int neos_net_rssi(void)
{
    if (s_state != NEOS_NET_ONLINE) {
        return 0;
    }
    /*
     * Asked of the driver rather than remembered from the scan. The number is
     * live and the whole point of showing it is that it changes as the tablet
     * is carried around; a value from the last scan would be a signal strength
     * for wherever the user was standing a minute ago.
     */
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        s_rssi = rec.rssi;
    }
    return s_rssi;
}

static void set_state(neos_net_state_t st)
{
    if (st == s_state) {
        return;
    }
    s_state = st;
    /* The bar icon is the only thing every screen shows, so it is told at
       once rather than picking the change up on its next half-second tick. */
    neos_bar_widgets_refresh();
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

/*
 * SNTP is started once, on the first address, and left running.
 *
 * esp_netif_sntp keeps its own timer and re-syncs on a long period, which is
 * what a device with a drifting RTC wants - and the callback writes the RX8130
 * every time, so a tablet that has been online at any point in the last few
 * hours comes back from a power cut with a good clock.
 */
static void sntp_landed(struct timeval *tv)
{
    if (tv) {
        neos_time_set_utc((int64_t)tv->tv_sec);
        neos_status_for("clock set from the network", 2500);
        neos_bar_widgets_refresh();
    }
}

static bool s_sntp_up;

static void sntp_start(void)
{
    if (s_sntp_up) {
        esp_netif_sntp_start();
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start                 = true;
    cfg.sync_cb               = sntp_landed;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew     = IP_EVENT_STA_GOT_IP;

    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_up = true;
        ESP_LOGI(TAG, "SNTP started against pool.ntp.org");
    } else {
        ESP_LOGW(TAG, "SNTP would not start");
    }
}

void neos_net_sntp_restart(void)
{
    if (s_state != NEOS_NET_ONLINE) {
        return;
    }
    if (!s_sntp_up) {
        sntp_start();
        return;
    }
    esp_netif_sntp_start();
    neos_status_for("asking a time server", 2000);
}

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

/*
 * One row per name, strongest kept.
 *
 * A mesh answers from three radios on three channels with the same SSID, and a
 * list that showed it three times would be a list about access points. What
 * the user is choosing between is networks.
 */
static void collect_scan(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        s_nap = 0;
        return;
    }
    if (n > 3 * NEOS_NET_SCAN_MAX) {
        n = 3 * NEOS_NET_SCAN_MAX;    /* bounded: this allocation is on a task stack budget */
    }

    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        s_nap = 0;
        return;
    }
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
        free(recs);
        s_nap = 0;
        return;
    }

    int out = 0;
    for (int i = 0; i < (int)n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) {
            continue;              /* hidden: there is nothing to show or tap */
        }

        int at = -1;
        for (int j = 0; j < out; j++) {
            if (strcmp(s_aps[j].ssid, ssid) == 0) {
                at = j;
                break;
            }
        }
        if (at >= 0) {
            if (recs[i].rssi > s_aps[at].rssi) {
                s_aps[at].rssi = recs[i].rssi;
                s_aps[at].chan = recs[i].primary;
            }
            continue;
        }
        if (out >= NEOS_NET_SCAN_MAX) {
            continue;
        }
        memset(&s_aps[out], 0, sizeof(s_aps[out]));
        strlcpy(s_aps[out].ssid, ssid, sizeof(s_aps[out].ssid));
        s_aps[out].rssi   = recs[i].rssi;
        s_aps[out].chan   = recs[i].primary;
        s_aps[out].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        s_aps[out].known  = known_index(s_aps[out].ssid) >= 0;
        out++;
    }
    free(recs);

    /* Insertion sort by signal. Two dozen entries at most, and it keeps the
       strongest - which is what someone is usually looking for - at the top
       without the list jumping about between scans the way a stable sort on a
       noisy key would. */
    for (int i = 1; i < out; i++) {
        const neos_net_ap_t key = s_aps[i];
        int j = i - 1;
        while (j >= 0 && s_aps[j].rssi < key.rssi) {
            s_aps[j + 1] = s_aps[j];
            j--;
        }
        s_aps[j + 1] = key;
    }
    s_nap = out;
}

bool neos_net_scan_start(void)
{
    if (!s_started || s_scanning) {
        return false;
    }
    const wifi_scan_config_t cfg = { .show_hidden = false };
    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
        return false;
    }
    s_scanning = true;
    return true;
}

int neos_net_scan_results(neos_net_ap_t *out, int max)
{
    if (!out || max <= 0) {
        return s_nap;
    }
    for (int i = 0; i < s_nap && i < max; i++) {
        out[i] = s_aps[i];
        out[i].known = known_index(s_aps[i].ssid) >= 0;   /* may have moved since */
    }
    return s_nap;
}

/* ------------------------------------------------------------------ */
/* Associating                                                         */
/* ------------------------------------------------------------------ */

static bool associate(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));

    /*
     * No minimum authmode. The stock default refuses anything weaker than
     * WPA2, which is right for a product and wrong for a tablet that has to be
     * able to join whatever is in the room - including the open network in a
     * cafe and the WPA network on somebody's old router.
     */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    esp_wifi_disconnect();
    if (esp_wifi_connect() != ESP_OK) {
        return false;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    set_state(NEOS_NET_CONNECTING);
    return true;
}

#define RETRY_MAX   3
#define RETRY_MS    3000
#define RESCAN_MS   30000

bool neos_net_connect(const char *ssid, const char *pass, bool remember)
{
    if (!s_started || !ssid || !ssid[0]) {
        return false;
    }

    /* NULL means "whatever we already know about this one", which is what the
       panel passes when a stored network is tapped. */
    const char *use = pass;
    if (!use) {
        const int at = known_index(ssid);
        use = at >= 0 ? s_known[at].pass : "";
    }

    strlcpy(s_want, ssid, sizeof(s_want));
    s_tries = 0;
    s_retry_at = 0;

    if (!associate(ssid, use)) {
        s_want[0] = 0;
        return false;
    }
    if (remember) {
        known_remember(ssid, use);
    }
    return true;
}

void neos_net_disconnect(void)
{
    s_want[0] = 0;
    s_ssid[0] = 0;
    s_ip[0] = 0;
    if (s_started) {
        esp_wifi_disconnect();
    }
    set_state(NEOS_NET_IDLE);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    switch (id) {
    case WIFI_EVENT_STA_START:
        set_state(NEOS_NET_IDLE);
        /* Something to join may well be in the room already, so the first
           scan goes out immediately rather than after the idle period. */
        s_rescan_at = 0;
        break;

    case WIFI_EVENT_STA_CONNECTED:
        /* Associated, but not usable until there is an address - the state
           stays CONNECTING so that nothing tries to open a socket yet. */
        s_tries = 0;
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        s_ip[0] = 0;
        if (s_want[0] && s_tries < RETRY_MAX) {
            s_tries++;
            s_retry_at = esp_timer_get_time() + (int64_t)RETRY_MS * 1000;
            ESP_LOGW(TAG, "\"%s\" dropped (reason %d), retry %d of %d",
                     s_want, d ? d->reason : 0, s_tries, RETRY_MAX);
            set_state(NEOS_NET_CONNECTING);
        } else {
            if (s_want[0]) {
                ESP_LOGW(TAG, "giving up on \"%s\" (reason %d)",
                         s_want, d ? d->reason : 0);
                neos_status_for("could not join that network", 3000);
            }
            s_want[0] = 0;
            s_ssid[0] = 0;
            s_rescan_at = esp_timer_get_time() + (int64_t)RESCAN_MS * 1000;
            set_state(NEOS_NET_IDLE);
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        collect_scan();
        s_scanning = false;
        break;

    default:
        break;
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *e = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    s_tries = 0;
    set_state(NEOS_NET_ONLINE);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s  %s", s_ssid, s_ip);
    neos_status_for(msg, 3000);
    ESP_LOGI(TAG, "online: %s at %s", s_ssid, s_ip);

    sntp_start();
}

/* ------------------------------------------------------------------ */
/* The reconnect loop                                                  */
/* ------------------------------------------------------------------ */

/*
 * Everything that has to happen on a clock rather than on an event: the retry
 * after a drop, and the periodic look around for a network we know.
 *
 * A task rather than two timers because both decisions read the same state and
 * a timer callback runs on the esp_timer task, where blocking on the Wi-Fi
 * driver is a thing to avoid. One second is far finer than either deadline
 * needs and costs nothing.
 */
static void bringup(void);
static void teardown(void);

static void net_task(void *arg)
{
    (void)arg;
    bringup();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Before anything else: there is no point retrying an association on
           a radio somebody has just asked to switch off. */
        if (s_power_req >= 0) {
            const bool want = s_power_req == 1;
            s_power_req = -1;
            if (want && !s_started) {
                bringup();
            } else if (!want && s_started) {
                teardown();
            }
            continue;
        }

        const int64_t now = esp_timer_get_time();

        if (s_state == NEOS_NET_CONNECTING && s_want[0] &&
            s_retry_at && now >= s_retry_at) {
            s_retry_at = 0;
            const int at = known_index(s_want);
            associate(s_want, at >= 0 ? s_known[at].pass : "");
            continue;
        }

        if (s_state == NEOS_NET_IDLE && s_nknown > 0 && !s_scanning &&
            s_rescan_at <= now) {
            s_rescan_at = now + (int64_t)RESCAN_MS * 1000;
            s_scan_is_users = false;
            s_autojoin = neos_net_scan_start();
            continue;
        }

        /*
         * A finished scan we started ourselves is the moment to join
         * something. Strongest first, so the list order is also the
         * preference order and there is no second policy to reason about.
         *
         * Exactly once per scan, which is what s_autojoin is for. Acting on
         * whatever results happen to be lying about would mean retrying the
         * network that just refused us once a second forever, on results from
         * before it refused.
         */
        if (s_autojoin && !s_scanning && !s_scan_is_users) {
            s_autojoin = false;
            if (s_state != NEOS_NET_IDLE) {
                continue;
            }
            for (int i = 0; i < s_nap; i++) {
                const int at = known_index(s_aps[i].ssid);
                if (at < 0) {
                    continue;
                }
                ESP_LOGI(TAG, "joining known network \"%s\" (%d dBm)",
                         s_aps[i].ssid, s_aps[i].rssi);
                strlcpy(s_want, s_aps[i].ssid, sizeof(s_want));
                s_tries = 0;
                associate(s_known[at].ssid, s_known[at].pass);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */

/*
 * Everything that talks to the radio, on the radio's own task.
 *
 * Bringing the C6 up means powering a rail, waiting, resetting a chip over a
 * GPIO and enumerating an SDIO bus - a second or so when it works and longer
 * when it does not. None of the rest of the system needs the answer, and the
 * boot chain in particular must not be held behind it, so neos_net_init() only
 * starts this and returns.
 */
static void bringup(void)
{
    /*
     * The rail first, then a pause.
     *
     * ESP-Hosted resets the C6 over GPIO 15 and then enumerates it over SDIO,
     * and none of that means anything while the co-processor is unpowered -
     * which it is on a board that has never had Wi-Fi switched on, because the
     * enable line is on an io expander no part of the Wi-Fi stack can see.
     */
    if (!neos_feature_set_raw(NEOS_FEAT_WIFI, true)) {
        ESP_LOGW(TAG, "could not power the radio - the io expander refused");
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    /*
     * The transport, which on the first boot is already up.
     *
     * ESP-Hosted starts itself from a C constructor before app_main runs, so
     * nothing here ever had to ask for it - and esp_hosted_init() is guarded,
     * so calling it again then costs nothing. It matters on the way back from
     * teardown(): that stops the SDIO tasks so the rail can be cut, and this
     * is the only thing that starts them again. Without it the radio powers
     * up and esp_wifi_init() reports the C6 as absent, because from the
     * host's side it now is.
     */
    const int herr = esp_hosted_init();
    if (herr != 0) {
        ESP_LOGE(TAG, "esp_hosted_init: %d - the C6 did not come back", herr);
        set_state(NEOS_NET_ABSENT);
        return;
    }

    /* ESP_ERR_INVALID_STATE from the event loop means somebody got there
       first, which is a success for our purposes and not worth refusing over. */
    const esp_err_t lerr = esp_event_loop_create_default();
    if (esp_netif_init() != ESP_OK ||
        (lerr != ESP_OK && lerr != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "no netif or event loop - networking is off this boot");
        set_state(NEOS_NET_ABSENT);
        return;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        ESP_LOGE(TAG, "no station interface");
        set_state(NEOS_NET_ABSENT);
        return;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        /*
         * On this board that almost always means the C6 did not answer over
         * SDIO. See the file header: the slave firmware is not ours and cannot
         * be inspected from here, so this is reported as "no radio" rather
         * than dressed up as a driver fault.
         */
        ESP_LOGE(TAG, "esp_wifi_init: %s - is the C6 answering?", esp_err_to_name(err));
        set_state(NEOS_NET_ABSENT);
        return;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL, &s_h_ip);

    /* Ours, not the driver's - see the file header. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        set_state(NEOS_NET_ABSENT);
        return;
    }

    s_started = true;
    ESP_LOGI(TAG, "radio up over SDIO to the C6");
}

/*
 * The radio off, in the order the hardware requires.
 *
 * The rail is last and it is the whole point of the order. The C6 is a second
 * chip at the end of an SDIO bus, and ESP-Hosted keeps tasks pumping that bus
 * on the assumption that something is answering; take its power away first and
 * those tasks start failing writes to a chip that is no longer there, decide
 * the link is unrecoverable, and reboot the tablet - which is what an app
 * toggling this rail directly used to do.
 *
 * esp_wifi_stop() and esp_wifi_deinit() are not enough on their own. They
 * quiesce the Wi-Fi API but leave the transport underneath it running; only
 * esp_hosted_deinit() stops the SDIO tasks, and with it in the sequence the
 * rail can be cut with nothing left talking to the far end.
 */
static void teardown(void)
{
    if (!s_started) {
        return;
    }
    s_started = false;
    s_scanning = false;
    s_autojoin = false;
    s_want[0] = 0;
    s_ssid[0] = 0;
    s_ip[0] = 0;
    s_nap = 0;

    esp_wifi_stop();
    esp_wifi_deinit();

    /* Registered by bringup(), and re-registered by the next one. */
    if (s_h_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        s_h_wifi = NULL;
    }
    if (s_h_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
        s_h_ip = NULL;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }

    esp_hosted_deinit();          /* the step that makes the next line safe */
    neos_feature_set_raw(NEOS_FEAT_WIFI, false);

    set_state(NEOS_NET_OFF);
    ESP_LOGI(TAG, "radio powered down");
}

void neos_net_power(bool on)
{
    /*
     * Recorded, not done. Everything either half of this touches blocks on the
     * Wi-Fi driver for seconds at a time, and the caller is a finger on a
     * toggle - on the app task, inside a repaint. net_task owns the radio and
     * is the only place either half is allowed to run.
     */
    s_power_req = on ? 1 : 0;
}

bool neos_net_powered(void)
{
    return s_started;
}

void neos_net_init(void)
{
    known_load();
    xTaskCreate(net_task, "net", 5120, NULL, 3, NULL);
}

/* The panel calls this so that a scan it started does not get joined out from
   under the user by the reconnect loop. */
void neos_net_scan_is_users(bool yes)
{
    s_scan_is_users = yes;
}
