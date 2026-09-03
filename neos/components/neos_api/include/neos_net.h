/*
 * Networking.
 *
 * The P4 has no radio. Everything here goes over SDIO to the ESP32-C6 running
 * ESP-Hosted, which means the radio is a separate chip with its own firmware
 * that this project cannot read back or rebuild - see docs/specs_fingerprint.md
 * for what is known about the one on this unit. The practical consequence is
 * that "no Wi-Fi" has one more cause than usual, and every call here has to be
 * able to say so rather than assume the hardware is simply idle.
 *
 * NeOS owns the connection, not the app. There is one radio, one set of
 * credentials and one system bar icon, and an app that could associate and
 * disassociate would be an app that decides whether the tablet has a network -
 * including for whatever runs after it. Apps read the state; the Wi-Fi panel,
 * which is NeOS, changes it.
 *
 * Signal strength is in dBm, negative, closer to zero is stronger. Nothing
 * here converts it to bars: how many bars a number is worth is a display
 * decision and belongs where the drawing is.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring the radio up and reconnect to the best known network.
 *
 * Called once by NeOS during bring-up. Returns immediately - association takes
 * seconds and happens on the event loop, so the state below is what tells you
 * how it went.
 */
void neos_net_init(void);

/**
 * Say that the scan now running was asked for by a person.
 *
 * The Wi-Fi panel sets this while it is open. Without it the reconnect loop
 * would take the results of the panel's own scan and associate with whatever
 * it recognised - which is right when nobody is looking and wrong when
 * somebody is standing there choosing. Not exported to apps.
 */
void neos_net_scan_is_users(bool yes);

/**
 * Switch the radio itself on or off.
 *
 * Not the same thing as disconnecting: this takes power away from the C6, so
 * it costs a few seconds to come back and there is nothing to scan in the
 * meantime. Use neos_net_disconnect() to leave a network with the radio still
 * running.
 *
 * Returns immediately. Both directions block on the Wi-Fi driver for seconds
 * and are run on the network task, so what this does is record the wish; the
 * state below is what says whether it has happened yet.
 *
 * Apps reach this through neos_feature_set(NEOS_FEAT_WIFI, ...), which routes
 * here rather than driving the enable line - see neos_sys.c. Pulling that pin
 * out from under a live SDIO transport makes ESP-Hosted reboot the tablet.
 */
void neos_net_power(bool on);

/** Whether the radio is powered and the stack is up. */
bool neos_net_powered(void);

typedef enum {
    NEOS_NET_ABSENT = 0,   /**< no radio: the co-processor did not answer */
    NEOS_NET_OFF,          /**< switched off, or never started */
    NEOS_NET_IDLE,         /**< up, associated with nothing */
    NEOS_NET_CONNECTING,
    NEOS_NET_ONLINE,       /**< associated, and holding an address */
} neos_net_state_t;

neos_net_state_t neos_net_state(void);

/** The network being used, or "" when there is not one. */
const char *neos_net_ssid(void);

/** Its signal in dBm, or 0 when not associated. */
int neos_net_rssi(void);

/** The address, formatted, or "" when there is not one yet. */
const char *neos_net_ip(void);

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

#define NEOS_NET_SCAN_MAX 24

typedef struct {
    char    ssid[33];      /**< 32 bytes plus a terminator, as the standard has it */
    int8_t  rssi;          /**< dBm */
    uint8_t chan;
    bool    secure;        /**< anything other than an open network */
    bool    known;         /**< there is a stored password for it */
    uint8_t reserved[3];   /**< pads the record to 40, so the next field is free */
} neos_net_ap_t;

/**
 * Start a scan. Returns false if the radio is not in a state to do one.
 *
 * Asynchronous, because a scan takes a couple of seconds across all channels
 * and a UI that blocked for it would be a UI that could not draw the list it
 * is scanning for. Poll neos_net_scanning() and re-read the results when it
 * goes false; the previous results stay readable throughout, which is what
 * lets the panel show a live list rather than an empty one every few seconds.
 */
bool neos_net_scan_start(void);
bool neos_net_scanning(void);

/**
 * The last scan's results, strongest first, one entry per name.
 *
 * Returns how many were found, which may exceed @p max - the array fills to
 * max and the count is still the truth. Duplicates are collapsed: a mesh puts
 * the same network on three channels from three radios, and a list that showed
 * it three times would be a list about access points rather than about
 * networks, which is not what anybody is choosing between.
 */
int neos_net_scan_results(neos_net_ap_t *out, int max);

/* ------------------------------------------------------------------ */
/* Connecting - NeOS only                                              */
/* ------------------------------------------------------------------ */

/*
 * None of the rest of this file is exported to apps, for the reason at the
 * top: there is one radio and one set of credentials, and an app that could
 * associate would be an app that decides whether the tablet has a network,
 * including for whatever runs after it. The Wi-Fi panel is the only caller.
 *
 * They are declared here rather than in a private header so that the whole
 * story of the network is in one file - what is shared and what is not is a
 * line in the syscall table, not a line between headers.
 */

/**
 * Associate with @p ssid.
 *
 * @param pass      the passphrase, or NULL/"" for an open network. NULL also
 *                  means "use the stored one" if there is one.
 * @param remember  save it, so this network is picked up automatically on
 *                  every boot from now on.
 *
 * Returns false only if the request could not be made at all. Whether the
 * password was right is not known yet when this returns and shows up as the
 * state going back to IDLE a few seconds later.
 */
bool neos_net_connect(const char *ssid, const char *pass, bool remember);

/** Drop the association. Does not forget the password. */
void neos_net_disconnect(void);

/** Whether there is a stored password for @p ssid. */
bool neos_net_known(const char *ssid);

/** Forget one network's password, so it is no longer joined automatically. */
void neos_net_forget(const char *ssid);

/** How many networks are remembered. */
int neos_net_known_count(void);

/* ------------------------------------------------------------------ */
/* Network time                                                        */
/* ------------------------------------------------------------------ */

/**
 * Ask a time server again, now.
 *
 * SNTP starts on its own the moment an address arrives and re-syncs on a long
 * period after that, so nothing has to call this - it is here because a clock
 * that is visibly wrong is a thing people want to fix while they are looking
 * at it, rather than in an hour. A no-op when there is no network.
 *
 * What lands goes to neos_time_set_utc(), which is what writes the RTC.
 */
void neos_net_sntp_restart(void);

/*
 * neos_net_ap_t is filled by the firmware into storage the app owns, so its
 * layout is compiled into every app that reads one. Same rule as neos_app_t:
 * appending a field is a minor, moving one is a major, and this is where you
 * find out.
 */
_Static_assert(sizeof(neos_net_ap_t) == 40, "neos_net_ap_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_net_ap_t, ssid)   ==  0, "neos_net_ap_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_net_ap_t, rssi)   == 33, "neos_net_ap_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_net_ap_t, chan)   == 34, "neos_net_ap_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_net_ap_t, secure) == 35, "neos_net_ap_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_net_ap_t, known)  == 36, "neos_net_ap_t layout is frozen for ABI v1");

#ifdef __cplusplus
}
#endif
