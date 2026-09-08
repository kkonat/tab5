/*
 * lanscan - what else is on this network.
 *
 * A port of the LAnSCan terminal tool to the tablet. The protocol work is the
 * same work - DNS, mDNS, SSDP, NetBIOS, SNMP and a handful of banner grabs are
 * the same bytes on any machine - but two things about this one changed the
 * shape of the program, and both are worth knowing before reading any of it.
 *
 * There is one thread. An app runs as ordinary code on NeOS's own stack and
 * there is nothing to suspend it, so it exits by returning from main() and it
 * has to keep reaching that check. The desktop version runs a stage per thread
 * and blocks in each; here every stage is a state machine that is asked to
 * make some progress and give the loop back, and no call anywhere waits longer
 * than a frame. That is why neos_sock.h has no blocking mode: the alternative
 * to this design is not a slower scanner, it is a close button that stops
 * working for the duration of a port scan.
 *
 * And the tablet is on the segment it is looking at, which changes what the
 * cheapest complete question is. A host may ignore ICMP, close every port,
 * announce nothing and refuse NetBIOS, and it still has to answer an ARP
 * request, or nothing on the segment could reach it at all. So the census is
 * an ARP request to every address in range - a 42-byte broadcast each, no
 * socket, no timeout to wait out - and it is complete in a way that no amount
 * of probing above the link layer can be.
 *
 * The desktop tool knocks on 21 ports across the whole subnet to catch the
 * hosts ICMP misses, because it cannot assume it is one hop from what it is
 * looking at. Here that would be five thousand connects against a 16-socket
 * budget to answer a question ARP has already answered completely, so the
 * knock only visits addresses the census found.
 *
 * Nothing here guesses. An unknown MAC prefix stays blank rather than being
 * labelled with something plausible, and a device that only ever answered an
 * ARP request is shown as an address with a MAC and no opinion about what it
 * is - which is exactly what is known about it.
 */
#pragma once

/*
 * The C headers are here for declarations only. Apps link -nostdlib, so
 * nothing behind these is in the image: every one of the handful of functions
 * used - memcpy, memset, memmove, memcmp, strlen, strcmp, strchr, snprintf,
 * vsnprintf, malloc, free - resolves at load time out of the syscall table or
 * the loader's own libc list. Include one that offers something not on either
 * list and the app builds, then fails to load. lan_util.c has the rest.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngl.h"
#include "neos_api.h"
#include "neos_sock.h"
#include "neos_sys.h"

/* ------------------------------------------------------------------ */
/* Sizes                                                               */
/* ------------------------------------------------------------------ */

/*
 * A /24 is 254 addresses and this holds 96 of them, which is a home or small
 * office network with room over. The cap is on what is remembered, not on
 * what is swept: address 97 onward is still probed and still counted, it just
 * has nowhere to be written down, and the footer says so rather than quietly
 * showing a short list.
 */
#define LAN_MAX_DEVICES  96
#define LAN_MAX_HOSTS    256    /**< addresses swept in one pass */
#define LAN_MAX_PORTS     16    /**< open ports remembered per device */
#define LAN_MAX_SERVICES   6    /**< mDNS/UPnP service labels per device */

#define LAN_NAME_LEN      40
#define LAN_VENDOR_LEN    28
#define LAN_KIND_LEN      18
#define LAN_OS_LEN        12
#define LAN_DETAIL_LEN    72
#define LAN_SERVICE_LEN   24
#define LAN_BANNER_LEN    48
#define LAN_TITLE_LEN     40
#define LAN_SERVER_LEN    32

/* ------------------------------------------------------------------ */
/* What we know about one device                                       */
/* ------------------------------------------------------------------ */

/*
 * Where a name came from, best first. A name only gets overwritten by a
 * better source, so a device that announced itself over mDNS keeps that name
 * when a reverse lookup later produces "192-168-0-14.lan".
 *
 * A web page title is not in here on purpose: it is a label for a page, not a
 * name for a machine, and it is used only when nothing else named the device.
 */
typedef enum {
    LAN_NAME_MDNS = 0,
    LAN_NAME_NBT,
    LAN_NAME_SSDP,
    LAN_NAME_SNMP,
    LAN_NAME_DNS,
    LAN_NAME_TITLE,
    LAN_NAME_NONE,
} lan_name_src_t;

/** How we know an address is live. A bitmask, because usually several ways. */
#define LAN_SEEN_ARP    0x0001u
#define LAN_SEEN_ICMP   0x0002u
#define LAN_SEEN_TCP    0x0004u
#define LAN_SEEN_MDNS   0x0008u
#define LAN_SEEN_SSDP   0x0010u
#define LAN_SEEN_NBT    0x0020u
#define LAN_SEEN_SNMP   0x0040u
#define LAN_SEEN_DNS    0x0080u
#define LAN_SEEN_SELF   0x0100u
#define LAN_SEEN_GW     0x0200u

typedef struct {
    uint16_t port;
    char     service[14];
    char     banner[LAN_BANNER_LEN];
    char     title[LAN_TITLE_LEN];
    char     server[LAN_SERVER_LEN];
    bool     probed;        /**< fingerprinted already; do not do it twice */
} lan_port_t;

typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool     has_mac;

    uint8_t  ttl;           /**< the reply's, not ours: 0 when nothing replied */
    uint16_t rtt_ms;        /**< 0 when unmeasured */
    uint16_t seen;          /**< LAN_SEEN_* */

    char     name[LAN_NAME_LEN];
    uint8_t  name_src;      /**< lan_name_src_t of what is in `name` */
    char     vendor[LAN_VENDOR_LEN];
    char     kind[LAN_KIND_LEN];
    char     os[LAN_OS_LEN];
    char     detail[LAN_DETAIL_LEN];   /**< UPnP / SNMP / model, best line */
    char     workgroup[18];

    char     services[LAN_MAX_SERVICES][LAN_SERVICE_LEN];
    uint8_t  nservices;

    lan_port_t ports[LAN_MAX_PORTS];
    uint8_t    nports;

    /* SSDP said where its description XML is; the upnp stage goes and gets it. */
    uint32_t upnp_ip;
    uint16_t upnp_port;
    char     upnp_path[64];
    bool     upnp_done;
} lan_device_t;

/* ------------------------------------------------------------------ */
/* The registry                                                        */
/* ------------------------------------------------------------------ */

/*
 * One table, one writer. Every probe writes here and the drawing reads it, and
 * because there is one thread there is no lock - which is worth stating rather
 * than leaving as an absence, since the desktop version's Registry exists
 * almost entirely to hold one.
 *
 * `revision` moves whenever anything changed. The UI compares it and does not
 * even walk the table when it has not, so an idle screen costs nothing.
 */
typedef struct {
    lan_device_t *dev;
    int           count;
    int           overflow;     /**< live addresses there was no room for */
    uint32_t      revision;
} lan_registry_t;

bool lan_model_init(lan_registry_t *reg);
void lan_model_free(lan_registry_t *reg);
void lan_model_clear(lan_registry_t *reg);

/** The device for @p ip, creating it if there is room. NULL when full. */
lan_device_t *lan_seen(lan_registry_t *reg, uint32_t ip, uint16_t how);

/** The device for @p ip, or NULL. Never creates. */
lan_device_t *lan_find(lan_registry_t *reg, uint32_t ip);

void lan_set_mac(lan_registry_t *reg, uint32_t ip, const uint8_t mac[6]);
void lan_set_name(lan_registry_t *reg, uint32_t ip, lan_name_src_t src, const char *name);
void lan_set_alive(lan_registry_t *reg, uint32_t ip, uint8_t ttl, uint16_t rtt_ms);
void lan_add_service(lan_registry_t *reg, uint32_t ip, const char *label);
void lan_set_detail(lan_registry_t *reg, uint32_t ip, const char *text);

/** The port record for (ip, port), creating it if there is room. */
lan_port_t *lan_add_port(lan_registry_t *reg, uint32_t ip, uint16_t port);

/* ------------------------------------------------------------------ */
/* Strings, addresses and arithmetic                                   */
/* ------------------------------------------------------------------ */

/*
 * Apps link -nostdlib, and what the loader's table carries is memcpy, memset,
 * strlen, strcmp, strchr, snprintf and not a great deal else. These are the
 * handful of missing pieces, written rather than wished for.
 */
size_t lan_copy(char *dst, size_t size, const char *src);
size_t lan_copy_n(char *dst, size_t size, const char *src, size_t n);

/** Collapse whitespace, drop control characters, truncate. In place is fine. */
void lan_clean(char *dst, size_t size, const char *src);

bool  lan_ieq(const char *a, const char *b);          /**< case-insensitive == */
bool  lan_has(const char *haystack, const char *needle);  /**< case-insensitive */
const char *lan_after(const char *haystack, const char *needle);
int   lan_atoi(const char *s);

void lan_ip_str(uint32_t ip, char *buf, size_t size);
void lan_mac_str(const uint8_t mac[6], char *buf, size_t size);

/** The one's complement checksum every IP protocol here needs. */
uint16_t lan_checksum(const void *data, int len);

/** Milliseconds since boot, as everything here measures time. */
uint32_t lan_now(void);

/* ------------------------------------------------------------------ */
/* The TCP pool                                                        */
/* ------------------------------------------------------------------ */

/*
 * Every stage that speaks TCP - the knock, the banner grabs, the HTTP probes
 * and the UPnP fetches - is the same machine with different payloads: connect
 * without blocking, wait to become writable, ask whether that meant connected
 * or refused, and for the talking kinds send something and read the reply.
 *
 * So there is one pool and the stages queue work into it. It also means the
 * socket budget is accounted for in one place, which matters when the ceiling
 * is 16 for the whole app.
 */
#define LAN_TCP_SLOTS   8
#define LAN_TCP_BUF  1536

/**
 * @param open  the handshake completed. False means refused, unreachable or
 *              never answered, and @p data is empty.
 * @param data  what the peer said, NUL-terminated. Empty unless the job asked
 *              to read.
 */
typedef void (*lan_tcp_done_fn)(uint32_t ip, uint16_t port, uint8_t tag,
                                bool open, const char *data, int len,
                                void *ctx);

void lan_tcp_begin(lan_tcp_done_fn on_done, void *ctx, uint32_t timeout_ms);
void lan_tcp_end(void);

/** True when a job can be pushed right now. */
bool lan_tcp_room(void);

/** True when nothing is in flight. */
bool lan_tcp_idle(void);

/**
 * Queue one connection.
 *
 * @param req      what to send once connected, or NULL to send nothing
 * @param want     bytes to read before giving up on the reply. 0 means the
 *                 job is a knock: the answer is whether it connected, and the
 *                 socket is closed as soon as that is known.
 * @param tag      handed back untouched, so a caller can tell its jobs apart
 */
bool lan_tcp_push(uint32_t ip, uint16_t port,
                  const void *req, int req_len, int want, uint8_t tag);

/** Drive everything in flight. Never waits longer than @p ms. */
void lan_tcp_tick(uint32_t ms);

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

extern const uint16_t lan_top_ports[];
extern const int      lan_top_ports_count;
extern const uint16_t lan_deep_ports[];
extern const int      lan_deep_ports_count;

/** "http", "ssh", ... or "" for a port with no well-known name. */
const char *lan_service_name(uint16_t port);

/** Whether to speak HTTP at this port rather than listen for a banner. */
bool lan_is_http_port(uint16_t port);

/** Ports whose service talks first, so a nudge would only confuse it. */
bool lan_talks_first(uint16_t port);

/* ------------------------------------------------------------------ */
/* DNS wire format                                                     */
/* ------------------------------------------------------------------ */

#define LAN_DNS_A     1
#define LAN_DNS_PTR  12
#define LAN_DNS_TXT  16
#define LAN_DNS_SRV  33

/** Encode "_ipp._tcp.local" into @p out. Returns bytes written, or 0. */
int lan_dns_name(uint8_t *out, int size, const char *name);

/** Build a query for @p count (name, type) pairs. Returns its length. */
int lan_dns_query(uint8_t *out, int size, const char *const *names,
                  const uint16_t *types, int count, uint16_t txid, bool unicast);

/** Read a possibly compressed name. Returns the offset after it. */
int lan_dns_read_name(const uint8_t *buf, int len, int off, char *out, size_t size);

/** One record, as handed to the walker. */
typedef struct {
    char        name[96];
    uint16_t    type;
    const uint8_t *rdata;
    int         rdlen;
} lan_dns_rr_t;

typedef void (*lan_dns_rr_fn)(const lan_dns_rr_t *rr, const uint8_t *buf, int len,
                              void *ctx);

/** Walk every record in a response. Never reads past @p len, never asserts. */
void lan_dns_walk(const uint8_t *buf, int len, lan_dns_rr_fn fn, void *ctx);

/** "192.168.0.1" -> "1.0.168.192.in-addr.arpa" */
void lan_dns_reverse(uint32_t ip, char *out, size_t size);

/* ------------------------------------------------------------------ */
/* Protocol modules                                                    */
/* ------------------------------------------------------------------ */

/*
 * Each of these owns its own socket and its own deadline, and each is driven
 * by the engine calling start/poll until done() goes true. They are the same
 * shape on purpose: the engine's stage table is then a row of function
 * pointers rather than a switch with a case per protocol.
 */
typedef struct lan_scan lan_scan_t;

bool lan_icmp_start(lan_scan_t *s);
void lan_icmp_poll(lan_scan_t *s);
bool lan_icmp_done(lan_scan_t *s);
void lan_icmp_stop(lan_scan_t *s);

bool lan_dnsq_start(lan_scan_t *s);
void lan_dnsq_poll(lan_scan_t *s);
bool lan_dnsq_done(lan_scan_t *s);
void lan_dnsq_stop(lan_scan_t *s);

bool lan_nbt_start(lan_scan_t *s);
void lan_nbt_poll(lan_scan_t *s);
bool lan_nbt_done(lan_scan_t *s);
void lan_nbt_stop(lan_scan_t *s);

bool lan_snmp_start(lan_scan_t *s);
void lan_snmp_poll(lan_scan_t *s);
bool lan_snmp_done(lan_scan_t *s);
void lan_snmp_stop(lan_scan_t *s);

/*
 * mDNS and SSDP are not stages. Both are "shout and wait": the answers come
 * back over the whole life of the scan and there is nothing to block on, so
 * their sockets stay open from the first stage to the last and the engine
 * polls them on every tick regardless of what else is happening. That is the
 * single-threaded reading of the desktop version's two background threads.
 */
bool lan_mdns_open(lan_scan_t *s);
void lan_mdns_poll(lan_scan_t *s);
void lan_mdns_close(lan_scan_t *s);

bool lan_ssdp_open(lan_scan_t *s);
void lan_ssdp_poll(lan_scan_t *s);
void lan_ssdp_close(lan_scan_t *s);

/* ------------------------------------------------------------------ */
/* Vendors                                                             */
/* ------------------------------------------------------------------ */

/**
 * MAC prefix to manufacturer.
 *
 * Two sources. A short built-in table of the prefixes that change how you read
 * a row - hypervisors and single-board computers - and, if it is on the card,
 * apps/lanscan/oui.bin: the IEEE registry sorted by prefix, searched in place
 * with neos_file_read_at() rather than read into memory. Thirty-odd thousand
 * entries is a megabyte, and a binary search over it is sixteen reads of
 * thirty-two bytes.
 *
 * Returns false when the prefix is not in either, which stays blank on screen.
 * A locally administered address - most modern phones - has no manufacturer at
 * all and is reported as such rather than as unknown.
 */
bool lan_oui_lookup(const uint8_t mac[6], char *out, size_t size);
bool lan_oui_have_db(void);
int  lan_oui_db_entries(void);

/* ------------------------------------------------------------------ */
/* Classification                                                      */
/* ------------------------------------------------------------------ */

/** Fill in `kind` and `os` from whatever evidence the device has by now. */
void lan_classify(lan_device_t *d);

/* ------------------------------------------------------------------ */
/* The scan                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    LAN_STAGE_LOCAL = 0,
    LAN_STAGE_ARP_CACHE,
    LAN_STAGE_ICMP,
    LAN_STAGE_ARP,
    LAN_STAGE_KNOCK,
    LAN_STAGE_VENDOR,
    LAN_STAGE_DNS,
    LAN_STAGE_NBT,
    LAN_STAGE_SNMP,
    LAN_STAGE_PORTS,
    LAN_STAGE_BANNER,
    LAN_STAGE_UPNP,
    LAN_STAGE_WATCH,
    LAN_STAGE_COUNT,
} lan_stage_t;

/** How thorough the deep port pass is. Nothing else changes with it. */
typedef enum {
    LAN_DEPTH_QUICK = 0,   /**< no deep pass at all */
    LAN_DEPTH_NORMAL,      /**< the first 48 of the deep list */
    LAN_DEPTH_DEEP,        /**< all of it */
    LAN_DEPTH_COUNT,
} lan_depth_t;

struct lan_scan {
    lan_registry_t reg;

    neos_iface_t iface;
    bool         have_iface;
    uint32_t     net_base;      /**< first host address, host order */
    int          net_hosts;     /**< how many addresses this pass sweeps */
    uint8_t      net_prefix;    /**< the mask, as a prefix length */
    bool         clamped;       /**< the real subnet was larger than we sweep */

    lan_stage_t stage;
    bool        stage_entered;
    uint32_t    stage_started;
    char        note[80];       /**< what the footer says about right now */

    lan_depth_t depth;
    bool        running;
    /* Asked for, as opposed to under way. A start can fail - no address at
       the moment the button was pressed - and this is what makes the tick
       keep trying rather than the app going quiet until it is relaunched. */
    bool        wanted;
    uint32_t    retry_ms;
    bool        first_pass_done;
    uint32_t    started_ms;
    uint32_t    elapsed_ms;
    uint32_t    cycle;
    uint32_t    next_cycle_ms;

    /* Per-stage cursors. Kept here rather than in each module's statics so
       that a scan is one object and starting a second one cannot inherit half
       of the first. */
    int      cursor;
    int      sent;
    int      hits;
    uint32_t deadline;

    /*
     * Pacing, by the clock rather than by the tick.
     *
     * The main loop has no frame rate - it runs as fast as the work allows,
     * which during a sweep is very fast indeed - so "a batch per tick" is not
     * a rate at all. Handing lwIP 254 addresses in a few milliseconds
     * overruns the ARP queue, and the sends that fail are addresses that
     * silently never get probed.
     */
    uint32_t sweep_next_ms;
    uint32_t harvest_ms;    /**< the neighbour cache is a tcpip round trip */
    int      retry;         /**< consecutive failures on the current address */

    /* The ICMP sweep, which is the one stage that has to match replies to
       sends: the reply carries back the sequence number and nothing else. */
    int      icmp_fd;
    uint32_t icmp_sent_at[LAN_MAX_HOSTS];

    int      udp_fd;            /**< the DNS / NetBIOS / SNMP stages, in turn */

    int      mdns_fd;           /**< bound to 5353, joined to the group */
    int      mdns_q_fd;         /**< the ephemeral port QU answers come to */
    uint32_t mdns_next_ms;
    int      mdns_round;

    int      ssdp_fd;
    uint32_t ssdp_next_ms;
    int      ssdp_round;
};

bool lan_engine_start(lan_scan_t *s);
void lan_engine_tick(lan_scan_t *s);
void lan_engine_stop(lan_scan_t *s);
void lan_engine_rescan(lan_scan_t *s);

const char *lan_stage_name(lan_stage_t stage);
const char *lan_depth_name(lan_depth_t depth);

/* ------------------------------------------------------------------ */
/* The screen                                                          */
/* ------------------------------------------------------------------ */

void lan_ui_init(lan_scan_t *s);
void lan_ui_tick(lan_scan_t *s);

/**
 * Throw away what the screen is believed to hold and draw the next frame in
 * full.
 *
 * For the one thing that invalidates the whole picture without changing any
 * of the data behind it: a rotation. ngl reallocates the back buffer, so
 * every row this app thinks it has already drawn is gone, and the row
 * signatures that would otherwise say "unchanged, skip it" are now lies.
 */
void lan_ui_repaint(void);
