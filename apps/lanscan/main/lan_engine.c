/*
 * The pipeline.
 *
 * Cheapest question first, so the table is worth looking at within a second
 * and keeps getting richer:
 *
 *   local      our address, mask, gateway - and how big the sweep is
 *   arp-cache  neighbours the stack already knew about, for nothing
 *   icmp       an echo to every address: RTT, reply TTL, and ARP for free
 *   arp        ask every address on the segment outright - the census
 *   knock      21 common ports against the hosts that turned out to exist
 *   vendor     MAC prefix to manufacturer
 *   dns        reverse lookups
 *   netbios    node status: Windows and Samba names, workgroups, more MACs
 *   snmp       sysDescr and sysName from anything answering "public"
 *   ports      the deep port list, as far as the depth setting says
 *   banner     what each open port says when you connect, or ask over HTTP
 *   upnp       the description XML that SSDP pointed at
 *   watch      hold, and run the cheap half again every half minute
 *
 * mDNS and SSDP are not in that list. They are polled on every tick from the
 * first stage to the last - see lan_mdns.c for why that is the single-threaded
 * spelling of the desktop version's two background threads.
 *
 * Every stage is entered, ticked and left by this file, and no stage is
 * allowed to block. The whole scan is one call per frame from lanscan_app.c,
 * which is what keeps the close button working while a port scan is running.
 */

#include <stdarg.h>

#include "lanscan.h"

/* Which pool result belongs to which stage. */
#define TAG_KNOCK   0
#define TAG_BANNER  1
#define TAG_HTTP    2
#define TAG_UPNP    3

#define KNOCK_TIMEOUT_MS   700
#define BANNER_TIMEOUT_MS 1500
#define WATCH_PERIOD_MS  30000

static const char *const s_stage_names[LAN_STAGE_COUNT] = {
    "local", "arp cache", "icmp sweep", "arp harvest", "tcp knock",
    "mac vendors", "reverse dns", "netbios", "snmp", "deep ports",
    "banners", "upnp", "watching",
};

const char *lan_stage_name(lan_stage_t stage)
{
    return (stage >= 0 && stage < LAN_STAGE_COUNT) ? s_stage_names[stage] : "";
}

const char *lan_depth_name(lan_depth_t depth)
{
    switch (depth) {
    case LAN_DEPTH_QUICK:  return "quick";
    case LAN_DEPTH_NORMAL: return "normal";
    case LAN_DEPTH_DEEP:   return "deep";
    default:               return "";
    }
}

static int deep_port_count(lan_depth_t depth)
{
    switch (depth) {
    case LAN_DEPTH_QUICK:  return 0;
    case LAN_DEPTH_NORMAL: return 48 < lan_deep_ports_count ? 48 : lan_deep_ports_count;
    default:               return lan_deep_ports_count;
    }
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static bool in_range(const lan_scan_t *s, uint32_t ip)
{
    return ip >= s->net_base && ip < s->net_base + (uint32_t)s->net_hosts;
}

/*
 * Fold the neighbour cache into the registry.
 *
 * Called on every tick of the sweep and not once at the end, because the
 * table holds 64 entries and a /24 sweep asks about 254 addresses: an entry
 * that resolved early is gone by the time the last packet goes out. Reading
 * as we go is the difference between finding everything on the segment and
 * finding whichever devices happened to be last.
 */
#define HARVEST_EVERY_MS 10

static int harvest_arp(lan_scan_t *s)
{
    /*
     * Often, but not as often as the loop turns over.
     *
     * neos_neigh_table() is not a memory read: the ARP table belongs to the
     * tcpip thread, so every call is a message to it and a wait for the
     * answer. Called from a loop with nothing else in it that was thousands
     * of round trips a second at a thread the whole network depends on, which
     * is a good way to make the stack unwell. Every 10 ms is a hundred a
     * second, against entries that live for seconds - two orders of magnitude
     * of margin on the thing this is racing.
     */
    if ((int32_t)(lan_now() - s->harvest_ms) < 0) {
        return 0;
    }
    s->harvest_ms = lan_now() + HARVEST_EVERY_MS;

    neos_neigh_t table[64];
    const int    total = neos_neigh_table(table, (int)(sizeof(table) / sizeof(table[0])));
    const int    n     = total < 64 ? total : 64;
    int          used  = 0;

    for (int i = 0; i < n; i++) {
        if (!in_range(s, table[i].ip)) {
            continue;
        }
        lan_set_mac(&s->reg, table[i].ip, table[i].mac);
        used++;
    }
    return used;
}

static void classify_all(lan_scan_t *s)
{
    for (int i = 0; i < s->reg.count; i++) {
        lan_classify(&s->reg.dev[i]);
    }
    s->reg.revision++;
}

/*
 * What the footer says about right now.
 *
 * The revision only moves when the text actually changed, which matters more
 * than it looks: a stage like the sweep writes its progress on every tick,
 * and a revision that moved each time would repaint the whole table thirty
 * times a second to show the same words.
 */
static void note(lan_scan_t *s, const char *fmt, ...)
{
    char    buf[sizeof(s->note)];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (strcmp(buf, s->note) != 0) {
        lan_copy(s->note, sizeof(s->note), buf);
        s->reg.revision++;
    }
}

/* ------------------------------------------------------------------ */
/* What the TCP pool hands back                                        */
/* ------------------------------------------------------------------ */

/* The first line of whatever the service said, which is the banner. */
static void take_banner(lan_scan_t *s, uint32_t ip, uint16_t port,
                        const char *data, int len)
{
    if (len <= 0) {
        return;
    }
    char line[LAN_BANNER_LEN * 2];
    int  at = 0;
    for (int i = 0; i < len && at + 1 < (int)sizeof(line); i++) {
        if (data[i] == '\r' || data[i] == '\n') {
            break;
        }
        line[at++] = data[i];
    }
    line[at] = 0;

    lan_port_t *p = lan_add_port(&s->reg, ip, port);
    if (p) {
        lan_clean(p->banner, sizeof(p->banner), line);
    }
}

/* Status line, Server header and <title>, which between them identify a box
   better than anything else it will volunteer over TCP. */
static void take_http(lan_scan_t *s, uint32_t ip, uint16_t port,
                      const char *data, int len)
{
    if (len <= 0) {
        return;
    }
    lan_port_t *p = lan_add_port(&s->reg, ip, port);
    if (!p) {
        return;
    }

    const char *server = lan_after(data, "\nServer:");
    if (server) {
        char value[LAN_SERVER_LEN * 2];
        int  at = 0;
        while (*server == ' ') {
            server++;
        }
        while (*server && *server != '\r' && *server != '\n' &&
               at + 1 < (int)sizeof(value)) {
            value[at++] = *server++;
        }
        value[at] = 0;
        lan_clean(p->server, sizeof(p->server), value);
    }

    const char *title = lan_after(data, "<title");
    if (title) {
        /* Skip whatever attributes the tag carries before its '>'. */
        while (*title && *title != '>') {
            title++;
        }
        if (*title == '>') {
            title++;
            char value[LAN_TITLE_LEN * 2];
            int  at = 0;
            while (*title && *title != '<' && at + 1 < (int)sizeof(value)) {
                value[at++] = *title++;
            }
            value[at] = 0;
            lan_clean(p->title, sizeof(p->title), value);
        }
    }

    /* A page with a title is a device with a name a person would recognise,
       and it is the last resort in the name ordering for exactly that
       reason - it names a page, not a machine. */
    if (p->title[0]) {
        lan_set_name(&s->reg, ip, LAN_NAME_TITLE, p->title);
    }
}

/* One element's text out of the UPnP description. No XML parser: the document
   is flat, the four interesting tags are unique in it, and a parser would be
   more code than the thing it parses. */
static bool xml_value(const char *doc, const char *tag, char *out, size_t size)
{
    char open[32];
    snprintf(open, sizeof(open), "<%s>", tag);

    const char *p = lan_after(doc, open);
    if (!p) {
        return false;
    }
    char raw[128];
    int  at = 0;
    while (*p && *p != '<' && at + 1 < (int)sizeof(raw)) {
        raw[at++] = *p++;
    }
    raw[at] = 0;
    lan_clean(out, size, raw);
    return out[0] != 0;
}

static void take_upnp(lan_scan_t *s, uint32_t ip, const char *data, int len)
{
    if (len <= 0) {
        return;
    }
    lan_device_t *d = lan_find(&s->reg, ip);
    if (d) {
        d->upnp_done = true;
    }

    char friendly[LAN_NAME_LEN];
    char maker[40];
    char model[40];

    const bool have_friendly = xml_value(data, "friendlyName", friendly, sizeof(friendly));
    const bool have_maker    = xml_value(data, "manufacturer", maker, sizeof(maker));
    const bool have_model    = xml_value(data, "modelName", model, sizeof(model));

    if (have_friendly) {
        lan_set_name(&s->reg, ip, LAN_NAME_SSDP, friendly);
    }
    if (have_maker || have_model) {
        /* Both halves are given an explicit share of the line rather than
           being allowed to run into it in order. A maker with a long name
           would otherwise leave no room for the model, which is the half
           that says what the thing is. */
        char line[LAN_DETAIL_LEN];
        snprintf(line, sizeof(line), "%.34s %.34s",
                 have_maker ? maker : "", have_model ? model : "");
        lan_set_detail(&s->reg, ip, line);

        /* A device that says who made it is better evidence than an OUI: the
           prefix belongs to whoever bought the radio module. */
        if (have_maker && d && d->vendor[0] == 0) {
            lan_clean(d->vendor, sizeof(d->vendor), maker);
        }
    }
    if (have_friendly || have_maker || have_model) {
        s->hits++;
    }
}

static void on_tcp(uint32_t ip, uint16_t port, uint8_t tag,
                   bool open, const char *data, int len, void *ctx)
{
    lan_scan_t *s = (lan_scan_t *)ctx;

    if (!open) {
        return;
    }
    switch (tag) {
    case TAG_KNOCK:
        lan_add_port(&s->reg, ip, port);
        s->hits++;
        break;
    case TAG_BANNER:
        take_banner(s, ip, port, data, len);
        break;
    case TAG_HTTP:
        take_http(s, ip, port, data, len);
        break;
    case TAG_UPNP:
        take_upnp(s, ip, data, len);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The TCP-driven stages                                               */
/* ------------------------------------------------------------------ */

/*
 * Port-major, not device-major: every host is tried on 80 before anything is
 * tried on 8080. With eight sockets and twenty devices that means every row
 * on screen gets its most likely port within the first second, instead of one
 * row being finished while the rest are blank.
 *
 * `cursor` walks the port list and `sent` walks the address space, and both
 * are indices into fixed things rather than into the registry - which can
 * grow underneath this stage whenever mDNS answers.
 */
/*
 * A tick's worth of feeding, and no more.
 *
 * Two things bound this loop, and both of them are bugs that were in it.
 *
 * lan_tcp_push() can fail - neos_sock_open() returns NEOS_SOCK_ERR when the
 * stack has no socket to give, which on a deep pass happens the moment the
 * connect-and-close rate outruns lwIP's PCB pool. A failed push leaves the
 * slot free, so "while there is room" was still true, so the loop pushed and
 * failed and pushed again forever: no draw, no close-button poll, an app that
 * has to be power-cycled. A push that fails means come back next tick.
 *
 * And the walk itself is over ports times addresses, which for the deep list
 * over a /24 is forty-eight thousand steps. Almost all of them are skipped -
 * there is no device at that address - so the loop can cross the whole space
 * without ever filling a slot, and one tick becomes one stage. The guard is
 * what keeps a tick a tick.
 */
#define FEED_PER_TICK  400

static bool knock_feed(lan_scan_t *s, const uint16_t *list, int count)
{
    int steps = 0;

    while (lan_tcp_room()) {
        if (s->cursor >= count) {
            return true;
        }
        if (++steps > FEED_PER_TICK) {
            return false;
        }
        if (s->sent >= s->net_hosts) {
            s->sent = 0;
            s->cursor++;
            continue;
        }
        const uint32_t ip = s->net_base + (uint32_t)s->sent;
        s->sent++;
        if (!lan_find(&s->reg, ip)) {
            continue;
        }
        if (!lan_tcp_push(ip, list[s->cursor], NULL, 0, 0, TAG_KNOCK)) {
            /* No socket to be had. Put the address back and try next tick. */
            s->sent--;
            return false;
        }
    }
    return false;
}

/*
 * The fingerprint pass. One job per open port that has not had one, and the
 * kind of job depends on the port: a service that talks first is listened to,
 * one that does not gets a newline to see whether that shakes something
 * loose, and a web port gets a GET.
 *
 * TLS ports are left alone. An app has no TLS, so the honest thing to do with
 * 443 is nothing - sending a plaintext GET at it and reporting the alert that
 * comes back as a banner would be inventing a reading.
 */
static bool banner_feed(lan_scan_t *s)
{
    int steps = 0;

    while (lan_tcp_room()) {
        if (s->cursor >= s->net_hosts) {
            return true;
        }
        if (++steps > FEED_PER_TICK) {
            return false;
        }
        const uint32_t ip = s->net_base + (uint32_t)s->cursor;
        lan_device_t  *d  = lan_find(&s->reg, ip);
        if (!d || s->sent >= d->nports) {
            s->cursor++;
            s->sent = 0;
            continue;
        }

        lan_port_t *p = &d->ports[s->sent];
        if (p->probed) {
            s->sent++;
            continue;
        }

        bool pushed;
        if (lan_is_http_port(p->port)) {
            char ipstr[16];
            lan_ip_str(ip, ipstr, sizeof(ipstr));
            char req[192];
            const int n = snprintf(req, sizeof(req),
                                   "GET / HTTP/1.1\r\nHost: %s\r\n"
                                   "User-Agent: NeOS-lanscan/1.0\r\n"
                                   "Accept: */*\r\nConnection: close\r\n\r\n",
                                   ipstr);
            pushed = lan_tcp_push(ip, p->port, req, n, LAN_TCP_BUF - 1, TAG_HTTP);
        } else if (lan_talks_first(p->port)) {
            pushed = lan_tcp_push(ip, p->port, NULL, 0, 256, TAG_BANNER);
        } else {
            pushed = lan_tcp_push(ip, p->port, "\r\n", 2, 256, TAG_BANNER);
        }

        if (!pushed) {
            return false;          /* no socket; this port stays unprobed */
        }
        /* Marked only once the probe is really in flight, so a push that
           failed does not quietly count as a service we looked at. */
        p->probed = true;
        s->sent++;
    }
    return false;
}

static bool upnp_feed(lan_scan_t *s)
{
    int steps = 0;

    while (lan_tcp_room()) {
        if (s->cursor >= s->net_hosts) {
            return true;
        }
        if (++steps > FEED_PER_TICK) {
            return false;
        }
        const uint32_t ip = s->net_base + (uint32_t)s->cursor;

        lan_device_t *d = lan_find(&s->reg, ip);
        if (!d || !d->upnp_port || d->upnp_done) {
            s->cursor++;
            continue;
        }

        char ipstr[16];
        lan_ip_str(ip, ipstr, sizeof(ipstr));
        char req[192];
        const int n = snprintf(req, sizeof(req),
                               "GET %s HTTP/1.1\r\nHost: %s:%u\r\n"
                               "User-Agent: NeOS-lanscan/1.0\r\n"
                               "Connection: close\r\n\r\n",
                               d->upnp_path, ipstr, (unsigned)d->upnp_port);
        if (!lan_tcp_push(ip, d->upnp_port, req, n, LAN_TCP_BUF - 1, TAG_UPNP)) {
            return false;
        }
        s->cursor++;
        s->sent++;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void work_out_range(lan_scan_t *s)
{
    const uint32_t ip   = s->iface.ip;
    const uint32_t mask = s->iface.mask ? s->iface.mask : 0xFFFFFF00u;

    const uint32_t network   = ip & mask;
    const uint32_t broadcast = network | ~mask;

    s->net_prefix = 0;
    for (uint32_t m = mask; m & 0x80000000u; m <<= 1) {
        s->net_prefix++;
    }

    uint32_t base  = network + 1;
    uint32_t hosts = (broadcast > network + 1) ? (broadcast - network - 1) : 0;

    /*
     * A /16 is 65,534 addresses, which at the pace the sweep runs is four
     * minutes of ICMP before the first port is tried. When the real subnet is
     * larger than one pass can honestly cover, sweep the /24 the tablet is
     * sitting in and say so in the footer - a scan that quietly did an eighth
     * of what it claimed would be worse than one that says which eighth.
     */
    if (hosts > LAN_MAX_HOSTS) {
        base       = (ip & 0xFFFFFF00u) + 1;
        hosts      = 254;
        s->clamped = true;
    } else {
        s->clamped = false;
    }

    s->net_base  = base;
    s->net_hosts = (int)hosts;
}

bool lan_engine_start(lan_scan_t *s)
{
    s->icmp_fd = -1;
    s->udp_fd  = -1;
    s->mdns_fd = -1;
    s->mdns_q_fd = -1;
    s->ssdp_fd = -1;

    s->have_iface = neos_iface(&s->iface);
    if (!s->have_iface || s->iface.ip == 0) {
        note(s, "no address yet - join a network from the bar");
        s->running = false;
        return false;
    }

    work_out_range(s);

    s->stage           = LAN_STAGE_LOCAL;
    s->stage_entered   = false;
    s->running         = true;
    s->wanted          = true;
    s->first_pass_done = false;
    s->started_ms      = lan_now();
    s->elapsed_ms      = 0;
    s->cycle           = 0;

    lan_mdns_open(s);
    lan_ssdp_open(s);
    return true;
}

void lan_engine_stop(lan_scan_t *s)
{
    lan_icmp_stop(s);
    if (s->udp_fd >= 0) {
        neos_sock_close(s->udp_fd);
        s->udp_fd = -1;
    }
    lan_tcp_end();
    lan_mdns_close(s);
    lan_ssdp_close(s);
    s->running = false;
}

/*
 * Start again from nothing.
 *
 * The wanted flag is the point. lan_engine_start() can fail - there may be no
 * address at the instant the button was pressed, and on a marginal
 * association there often is not - and without somewhere to record that a
 * scan was asked for, a failed start was permanent: running went false, the
 * tick returned immediately from then on, and the app sat showing a stale
 * table with "no network" under it until it was relaunched. One press of
 * RESCAN at the wrong moment killed it.
 *
 * So asking for a scan and managing to begin one are two different things
 * now, and the tick keeps trying.
 */
void lan_engine_rescan(lan_scan_t *s)
{
    lan_engine_stop(s);
    lan_model_clear(&s->reg);
    s->wanted   = true;
    s->retry_ms = 0;
    if (!lan_engine_start(s)) {
        s->stage = LAN_STAGE_LOCAL;
    }
}

/* ------------------------------------------------------------------ */
/* One stage at a time                                                 */
/* ------------------------------------------------------------------ */

static void enter(lan_scan_t *s, lan_stage_t stage)
{
    s->stage         = stage;
    s->stage_entered = false;
    s->stage_started = lan_now();
    s->reg.revision++;
}

static void stage_local(lan_scan_t *s)
{
    lan_device_t *self = lan_seen(&s->reg, s->iface.ip, LAN_SEEN_SELF);
    if (self) {
        lan_set_mac(&s->reg, s->iface.ip, s->iface.mac);
        lan_copy(self->name, sizeof(self->name), "this tablet");
        self->name_src = LAN_NAME_MDNS;   /* nothing should overwrite it */
    }
    if (s->iface.gw && in_range(s, s->iface.gw)) {
        lan_seen(&s->reg, s->iface.gw, LAN_SEEN_GW);
    }

    char ipstr[16];
    lan_ip_str(s->net_base & 0xFFFFFF00u, ipstr, sizeof(ipstr));
    snprintf(s->note, sizeof(s->note), "%s/%u | %d addresses%s",
             ipstr, (unsigned)s->net_prefix, s->net_hosts,
             s->clamped ? " (subnet is larger; sweeping this /24)" : "");
    enter(s, LAN_STAGE_ARP_CACHE);
}

static void stage_vendor(lan_scan_t *s)
{
    int known = 0;
    for (int i = 0; i < s->reg.count; i++) {
        lan_device_t *d = &s->reg.dev[i];
        if (!d->has_mac || d->vendor[0]) {
            continue;
        }
        if (lan_oui_lookup(d->mac, d->vendor, sizeof(d->vendor))) {
            known++;
        }
    }
    s->reg.revision++;

    if (lan_oui_have_db()) {
        note(s, "%d vendors from the IEEE list (%d entries)", known,
             lan_oui_db_entries());
    } else {
        /* Said outright rather than left as a gap. Without the file only the
           built-in prefixes resolve, and a mostly empty vendor column looks
           like a network of unknown devices instead of a missing download. */
        note(s, "%d vendors from the built-in table - no oui.bin on the card",
             known);
    }
    enter(s, LAN_STAGE_DNS);
}

/*
 * Every address on the segment, asked outright.
 *
 * This is the stage that decides what exists, and it asks about all of them -
 * not only the ones something else has already turned up. That distinction is
 * the whole difference between finding eight devices and finding twenty.
 *
 * A host may ignore ICMP, close every port, announce nothing over mDNS and
 * refuse NetBIOS, and it still has to answer an ARP request, because
 * otherwise nothing on the segment could reach it at all. There is no such
 * thing as a live host on our own broadcast domain that is silent here. So an
 * ARP request to every address is a complete census, and it is cheap: a
 * 42-byte broadcast frame, no socket, no queue entry, no reply timeout to
 * wait out.
 *
 * It replaces what the desktop version needs a whole-subnet TCP knock for -
 * 21 ports across 254 addresses, five thousand connects - because that tool
 * cannot assume it is one hop from what it is looking at, and this one can.
 *
 * The reading back is continuous rather than at the end. lwIP's table holds
 * 64 and we are about to make 254 entries churn through it, so an answer that
 * is not collected within a moment of arriving is an answer thrown away.
 */
/*
 * Asked more than once, because a request is a single packet and this is
 * Wi-Fi.
 *
 * An ARP request that is lost, or whose reply is lost, is indistinguishable
 * from an address with nothing at it - and on a weak association loss is not
 * rare. One pass over the subnet found eight devices where the same network
 * had twenty; the misses were not hosts that refused to answer, they were
 * hosts that were never successfully asked.
 *
 * Later rounds skip whatever has already answered, so the second pass is a
 * fraction of the first and the third is smaller again. It costs a few
 * hundred 42-byte broadcasts to stop guessing.
 */
#define ARP_BATCH        8
#define ARP_EVERY_MS    20
#define ARP_ROUNDS       3
#define ARP_SETTLE_MS 1200

static void stage_arp(lan_scan_t *s)
{
    if (!s->stage_entered) {
        s->stage_entered = true;
        s->cursor        = 0;
        s->sent          = 0;      /* the round we are on */
        s->sweep_next_ms = lan_now();
        s->deadline      = 0;
    }

    /* Every pass, at whatever rate the loop is turning over: the table is
       small and the entries in it are perishable. */
    harvest_arp(s);

    if (s->cursor < s->net_hosts) {
        if ((int32_t)(lan_now() - s->sweep_next_ms) < 0) {
            /* Waiting for the next batch. One tick of sleep keeps the core
               free without slowing the harvest above to anything like the
               rate at which entries are recycled. */
            neos_sleep_ms(1);
            return;
        }
        s->sweep_next_ms = lan_now() + ARP_EVERY_MS;

        const int stop = s->cursor + ARP_BATCH;
        while (s->cursor < s->net_hosts && s->cursor < stop) {
            const uint32_t ip = s->net_base + (uint32_t)s->cursor;
            s->cursor++;

            /* After the first pass, only the addresses still unaccounted
               for. Re-asking a host that has already answered would be
               spending the round on the devices that did not need it. */
            if (s->sent > 0) {
                const lan_device_t *d = lan_find(&s->reg, ip);
                if (d && d->has_mac) {
                    continue;
                }
            }
            /* Our own address answers from the stack rather than the wire,
               and the gateway is already in the cache; asking anyway costs
               one frame and keeps this a plain sweep with no exceptions. */
            neos_neigh_ask(ip);
        }
        note(s, "round %d of %d, %d of %d addresses",
             s->sent + 1, ARP_ROUNDS, s->cursor, s->net_hosts);
        if (s->cursor >= s->net_hosts) {
            s->deadline = lan_now() + ARP_SETTLE_MS;
        }
        return;
    }

    if (s->deadline && (int32_t)(lan_now() - s->deadline) >= 0) {
        /* Settled. Another round, or done. */
        if (++s->sent < ARP_ROUNDS) {
            s->cursor        = 0;
            s->deadline      = 0;
            s->sweep_next_ms = lan_now();
            return;
        }

        int with_mac = 0;
        for (int i = 0; i < s->reg.count; i++) {
            if (s->reg.dev[i].has_mac) {
                with_mac++;
            }
        }
        note(s, "%d devices answered ARP, %d with a MAC", s->reg.count, with_mac);
        classify_all(s);
        enter(s, LAN_STAGE_KNOCK);
    }
}

void lan_engine_tick(lan_scan_t *s)
{
    /*
     * A scan that was asked for but has not managed to begin keeps trying.
     * Twice a second is often enough that the table comes back on its own the
     * moment an address appears, and rare enough that a tablet with no
     * network is not asking the stack about it in a spin.
     */
    if (!s->running) {
        if (!s->wanted) {
            return;
        }
        if ((int32_t)(lan_now() - s->retry_ms) < 0) {
            neos_sleep_ms(10);
            return;
        }
        s->retry_ms = lan_now() + 500;
        lan_engine_start(s);
        return;
    }

    /* Ambient, on every tick regardless of stage: both are shouts whose
       answers arrive whenever the devices that heard them decide to reply. */
    lan_mdns_poll(s);
    lan_ssdp_poll(s);

    if (!s->first_pass_done) {
        s->elapsed_ms = lan_now() - s->started_ms;
    }

    switch (s->stage) {

    case LAN_STAGE_LOCAL:
        stage_local(s);
        break;

    case LAN_STAGE_ARP_CACHE: {
        const int found = harvest_arp(s);
        note(s, "%d neighbours the stack already knew", found);
        enter(s, LAN_STAGE_ICMP);
        break;
    }

    case LAN_STAGE_ICMP:
        if (!s->stage_entered) {
            s->stage_entered = true;
            if (!lan_icmp_start(s)) {
                lan_copy(s->note, sizeof(s->note), "no raw socket - skipping the sweep");
                enter(s, LAN_STAGE_ARP);
                break;
            }
        }
        lan_icmp_poll(s);
        harvest_arp(s);          /* while the entries are still in the table */
        note(s, "%d of %d addresses probed", s->cursor, s->net_hosts);
        neos_sleep_ms(1);        /* paced by the clock; see lan_icmp.c */
        if (lan_icmp_done(s)) {
            note(s, "%d replied of %d probed", s->hits, s->sent);
            lan_icmp_stop(s);
            enter(s, LAN_STAGE_ARP);
        }
        break;

    case LAN_STAGE_ARP:
        stage_arp(s);
        break;

    case LAN_STAGE_KNOCK:
        if (!s->stage_entered) {
            s->stage_entered = true;
            s->cursor = 0;
            s->sent   = 0;
            s->hits   = 0;
            lan_tcp_begin(on_tcp, s, KNOCK_TIMEOUT_MS);
        }
        {
            const bool fed = knock_feed(s, lan_top_ports, lan_top_ports_count);
            lan_tcp_tick(10);
            note(s, "port %d of %d | %d open", s->cursor + 1,
                 lan_top_ports_count, s->hits);
            if (fed && lan_tcp_idle()) {
                note(s, "%d open ports on %d devices", s->hits, s->reg.count);
                lan_tcp_end();
                classify_all(s);
                enter(s, LAN_STAGE_VENDOR);
            }
        }
        break;

    case LAN_STAGE_VENDOR:
        stage_vendor(s);
        break;

    case LAN_STAGE_DNS:
        if (!s->stage_entered) {
            s->stage_entered = true;
            if (!lan_dnsq_start(s)) {
                lan_copy(s->note, sizeof(s->note), "no resolver in the lease");
                enter(s, LAN_STAGE_NBT);
                break;
            }
        }
        lan_dnsq_poll(s);
        if (lan_dnsq_done(s)) {
            note(s, "%d names from %d lookups", s->hits, s->sent);
            lan_dnsq_stop(s);
            enter(s, LAN_STAGE_NBT);
        }
        break;

    case LAN_STAGE_NBT:
        if (!s->stage_entered) {
            s->stage_entered = true;
            if (!lan_nbt_start(s)) {
                enter(s, LAN_STAGE_SNMP);
                break;
            }
        }
        lan_nbt_poll(s);
        if (lan_nbt_done(s)) {
            note(s, "%d NetBIOS names from %d asked", s->hits, s->sent);
            lan_nbt_stop(s);
            classify_all(s);
            enter(s, LAN_STAGE_SNMP);
        }
        break;

    case LAN_STAGE_SNMP:
        if (!s->stage_entered) {
            s->stage_entered = true;
            if (!lan_snmp_start(s)) {
                enter(s, LAN_STAGE_PORTS);
                break;
            }
        }
        lan_snmp_poll(s);
        if (lan_snmp_done(s)) {
            if (s->hits) {
                note(s, "%d devices answered SNMP", s->hits);
            } else {
                lan_copy(s->note, sizeof(s->note),
                         "no device answered the public community");
            }
            lan_snmp_stop(s);
            enter(s, LAN_STAGE_PORTS);
        }
        break;

    case LAN_STAGE_PORTS: {
        const int count = deep_port_count(s->depth);
        if (count == 0) {
            lan_copy(s->note, sizeof(s->note), "deep pass off in quick mode");
            enter(s, LAN_STAGE_BANNER);
            break;
        }
        if (!s->stage_entered) {
            s->stage_entered = true;
            s->cursor = 0;
            s->sent   = 0;
            s->hits   = 0;
            lan_tcp_begin(on_tcp, s, KNOCK_TIMEOUT_MS);
        }
        {
            const bool fed = knock_feed(s, lan_deep_ports, count);
            lan_tcp_tick(10);
            note(s, "deep port %d of %d | %d found", s->cursor + 1, count, s->hits);
            if (fed && lan_tcp_idle()) {
                note(s, "%d further ports open", s->hits);
                lan_tcp_end();
                classify_all(s);
                enter(s, LAN_STAGE_BANNER);
            }
        }
        break;
    }

    case LAN_STAGE_BANNER:
        if (!s->stage_entered) {
            s->stage_entered = true;
            s->cursor = 0;
            s->sent   = 0;
            s->hits   = 0;
            lan_tcp_begin(on_tcp, s, BANNER_TIMEOUT_MS);
        }
        {
            const bool fed = banner_feed(s);
            lan_tcp_tick(10);
            note(s, "fingerprinting %d of %d devices", s->cursor, s->reg.count);
            if (fed && lan_tcp_idle()) {
                lan_copy(s->note, sizeof(s->note), "services identified");
                lan_tcp_end();
                classify_all(s);
                enter(s, LAN_STAGE_UPNP);
            }
        }
        break;

    case LAN_STAGE_UPNP:
        if (!s->stage_entered) {
            s->stage_entered = true;
            s->cursor = 0;
            s->sent   = 0;
            s->hits   = 0;
            lan_tcp_begin(on_tcp, s, BANNER_TIMEOUT_MS);
        }
        {
            const bool fed = upnp_feed(s);
            lan_tcp_tick(10);
            if (fed && lan_tcp_idle()) {
                note(s, "%d of %d devices described themselves", s->hits, s->sent);
                lan_tcp_end();
                classify_all(s);

                if (!s->first_pass_done) {
                    s->first_pass_done = true;
                    s->elapsed_ms      = lan_now() - s->started_ms;
                }
                s->next_cycle_ms = lan_now() + WATCH_PERIOD_MS;
                enter(s, LAN_STAGE_WATCH);
            }
        }
        break;

    /*
     * The scan is over; the app is not.
     *
     * Everything already learned stays, and every half minute the cheap half
     * of the pipeline runs again from the top. A device that has since joined
     * the network gets the whole treatment because every stage's "have we
     * asked this one" check is a field on the device, and a device that was
     * already profiled is left alone for the same reason.
     */
    case LAN_STAGE_WATCH:
        if (!s->stage_entered) {
            s->stage_entered = true;
            /* The count is in the footer already; this says what the next
               thirty seconds are for. */
            note(s, "re-checking every %us", (unsigned)(WATCH_PERIOD_MS / 1000));
        }
        if ((int32_t)(lan_now() - s->next_cycle_ms) >= 0) {
            s->cycle++;
            enter(s, LAN_STAGE_ARP_CACHE);
        } else {
            /* Nothing to do but let the ambient listeners work. Sleeping is
               what keeps an idle scanner off the core. */
            neos_sleep_ms(10);
        }
        break;

    default:
        enter(s, LAN_STAGE_WATCH);
        break;
    }
}
