/*
 * mDNS / DNS-SD.
 *
 * Not a stage. mDNS is a shout, and the answers arrive whenever the devices
 * that heard it feel like answering - some immediately, some after a delay
 * they choose themselves to avoid colliding with each other. There is nothing
 * to block on, so this socket is opened before the first stage and read on
 * every tick until the last one, and the queries go out in rounds spread
 * across the whole scan. The desktop version gives this a thread; on one
 * thread, "runs for the whole scan" and "is polled by the loop" are the same
 * sentence.
 *
 * Between this and SSDP come most of the human-readable names on a home
 * network. A television, a printer, a phone and a thermostat will not answer
 * a port scan with anything you would recognise, and all four announce
 * themselves here by name.
 *
 * One simplification against the desktop version, and it is worth being
 * explicit about. That one accumulates every record from every packet into a
 * store and resolves hostnames to addresses at the end, because it has to:
 * records arrive split across packets and out of order. This keys everything
 * on the address the packet came from instead. A device's own announcement
 * comes from the device, so the source address is the answer to "who is this
 * about" in every case that matters, and it costs a table and a resolve pass
 * that would have to live in PSRAM. What it loses is the rare packet that
 * carries records about some other host - a proxy announcing on its behalf -
 * which is not a case worth a kilobyte of state on a tablet.
 */

#include "lanscan.h"

#define MDNS_GROUP  0xE00000FBu     /* 224.0.0.251 */
#define MDNS_PORT   5353

/* Enough service types to cover a home or office network without turning the
   query rounds into a broadcast storm of their own. */
static const char *const s_services[] = {
    "_services._dns-sd._udp.local",
    "_http._tcp.local",        "_https._tcp.local",
    "_workstation._tcp.local", "_device-info._tcp.local",
    "_ssh._tcp.local",         "_smb._tcp.local",
    "_afpovertcp._tcp.local",  "_nfs._tcp.local",
    "_printer._tcp.local",     "_ipp._tcp.local",
    "_ipps._tcp.local",        "_pdl-datastream._tcp.local",
    "_scanner._tcp.local",     "_uscan._tcp.local",
    "_airplay._tcp.local",     "_raop._tcp.local",
    "_companion-link._tcp.local", "_googlecast._tcp.local",
    "_androidtvremote2._tcp.local", "_spotify-connect._tcp.local",
    "_sonos._tcp.local",       "_hap._tcp.local",
    "_matter._tcp.local",      "_esphomelib._tcp.local",
    "_home-assistant._tcp.local", "_octoprint._tcp.local",
    "_plexmediasvr._tcp.local", "_nvstream._tcp.local",
    "_rfb._tcp.local",         "_teamviewer._tcp.local",
};
#define MDNS_SERVICES  ((int)(sizeof(s_services) / sizeof(s_services[0])))

#define MDNS_PER_QUERY   6      /* names per packet */
#define MDNS_ROUND_MS  900      /* between packets */

typedef struct {
    lan_scan_t *scan;
    uint32_t    from;
} walk_ctx_t;

/* "Living Room._googlecast._tcp.local" -> "_googlecast"; "" if it is not
   a service instance. The type is what says what a device is; the instance
   label is what says which one. */
static void service_type(const char *name, char *out, size_t size)
{
    out[0] = 0;
    const char *p = name;
    while (*p) {
        if (p[0] == '_' && (p == name || p[-1] == '.')) {
            size_t n = 0;
            while (p[n] && p[n] != '.' && n + 1 < size) {
                out[n] = p[n];
                n++;
            }
            out[n] = 0;
            /* "_tcp" and "_udp" are the transport, not the service. */
            if (!lan_ieq(out, "_tcp") && !lan_ieq(out, "_udp")) {
                return;
            }
            out[0] = 0;
        }
        p++;
    }
}

/* Is `n` a run of exactly 12 hex digits - i.e. a MAC written without separators? */
static bool is_bare_mac(const char *s, size_t n)
{
    if (n != 12) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                         (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

/*
 * The label in front of the service type, which is the name a person gave the
 * device.
 *
 * Two things are undone on the way. mDNS escapes a space as \032, which is
 * worth turning back into a space rather than showing on screen. And AirPlay
 * and RAOP name their instances "AABBCCDDEEFF@Living Room" - the MAC is
 * already the row's second column, so a name column reading 30F9ED14A88A@Sony
 * spends thirteen characters saying nothing new. The part after the @ is the
 * part somebody chose.
 */
static void instance_label(const char *name, char *out, size_t size)
{
    size_t at = 0;
    out[0] = 0;

    const char *at_sign = strchr(name, '@');
    if (at_sign && is_bare_mac(name, (size_t)(at_sign - name))) {
        name = at_sign + 1;
    }

    for (const char *p = name; *p && at + 1 < size; ) {
        if (p[0] == '.' && p[1] == '_') {
            break;
        }
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '9' &&
            p[2] >= '0' && p[2] <= '9' && p[3] >= '0' && p[3] <= '9') {
            const int code = (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
            out[at++] = (code >= 0x20 && code < 0x7F) ? (char)code : ' ';
            p += 4;
            continue;
        }
        if (p[0] == '\\') {
            p++;
            continue;
        }
        out[at++] = *p++;
    }
    out[at] = 0;
}

/* Strip the trailing ".local" a hostname always carries here. */
static void strip_local(char *name)
{
    const size_t n = strlen(name);
    if (n > 6 && lan_ieq(name + n - 6, ".local")) {
        name[n - 6] = 0;
    }
}

static void on_record(const lan_dns_rr_t *rr, const uint8_t *buf, int len, void *ctx)
{
    walk_ctx_t   *w = (walk_ctx_t *)ctx;
    lan_registry_t *reg = &w->scan->reg;

    switch (rr->type) {
    case LAN_DNS_A: {
        if (rr->rdlen != 4) {
            return;
        }
        const uint32_t addr = ((uint32_t)rr->rdata[0] << 24) |
                              ((uint32_t)rr->rdata[1] << 16) |
                              ((uint32_t)rr->rdata[2] << 8) | rr->rdata[3];
        /* Only the sender's own address. A record about somebody else is a
           record we have no way to trust here - see the note at the top. */
        if (addr != w->from) {
            return;
        }
        char host[LAN_NAME_LEN];
        lan_copy(host, sizeof(host), rr->name);
        strip_local(host);
        lan_seen(reg, w->from, LAN_SEEN_MDNS);
        lan_set_name(reg, w->from, LAN_NAME_MDNS, host);
        break;
    }

    case LAN_DNS_PTR: {
        /* The question was a service type and the answer names an instance
           of it. The type is the useful half. */
        char type[LAN_SERVICE_LEN];
        service_type(rr->name, type, sizeof(type));
        if (type[0]) {
            lan_seen(reg, w->from, LAN_SEEN_MDNS);
            lan_add_service(reg, w->from, type);
        }
        break;
    }

    case LAN_DNS_SRV: {
        char type[LAN_SERVICE_LEN];
        service_type(rr->name, type, sizeof(type));
        if (type[0]) {
            lan_seen(reg, w->from, LAN_SEEN_MDNS);
            lan_add_service(reg, w->from, type);
        }
        char label[LAN_NAME_LEN];
        instance_label(rr->name, label, sizeof(label));
        if (label[0]) {
            lan_set_name(reg, w->from, LAN_NAME_MDNS, label);
        }
        break;
    }

    case LAN_DNS_TXT: {
        /*
         * TXT is a run of length-prefixed key=value strings. Only the keys
         * that name the hardware are wanted: "md" is what Apple and Google
         * devices put their model in, and it is usually the exact product.
         */
        int at = 0;
        while (at < rr->rdlen) {
            const int n = rr->rdata[at++];
            if (n <= 0 || at + n > rr->rdlen) {
                break;
            }
            char entry[80];
            const int copy = n < (int)sizeof(entry) - 1 ? n : (int)sizeof(entry) - 1;
            memcpy(entry, rr->rdata + at, (size_t)copy);
            entry[copy] = 0;
            at += n;

            char *eq = strchr(entry, '=');
            if (!eq) {
                continue;
            }
            *eq = 0;
            const char *value = eq + 1;
            if (!value[0]) {
                continue;
            }
            if (lan_ieq(entry, "md") || lan_ieq(entry, "model") ||
                lan_ieq(entry, "am") || lan_ieq(entry, "ty")) {
                lan_set_detail(reg, w->from, value);
            }
        }
        break;
    }

    default:
        break;
    }
    (void)buf;
    (void)len;
}

bool lan_mdns_open(lan_scan_t *s)
{
    lan_mdns_close(s);

    /*
     * Two sockets. The listener is where multicast answers land, and it has
     * to be on 5353 to hear them. The querier is on an ephemeral port because
     * a question asked from 5353 gets a multicast answer everybody has to
     * read, and one asked from anywhere else gets a unicast answer back to
     * us - which is the QU bit in the query, and is politer to a network we
     * are already sweeping.
     */
    s->mdns_fd = neos_sock_open(NEOS_SOCK_UDP);
    if (s->mdns_fd >= 0) {
        neos_sock_set(s->mdns_fd, NEOS_SOPT_REUSE, 1);
        if (neos_sock_bind(s->mdns_fd, 0, MDNS_PORT) != NEOS_SOCK_OK) {
            neos_sock_close(s->mdns_fd);
            s->mdns_fd = -1;
        } else {
            neos_sock_join(s->mdns_fd, MDNS_GROUP, s->iface.ip);
        }
    }

    s->mdns_q_fd = neos_sock_open(NEOS_SOCK_UDP);
    if (s->mdns_q_fd >= 0) {
        neos_sock_bind(s->mdns_q_fd, s->iface.ip, 0);
        neos_sock_set(s->mdns_q_fd, NEOS_SOPT_MCAST_IF, s->iface.ip);
        neos_sock_set(s->mdns_q_fd, NEOS_SOPT_MCAST_TTL, 1);
        neos_sock_set(s->mdns_q_fd, NEOS_SOPT_MCAST_LOOP, 0);
    }

    s->mdns_round   = 0;
    s->mdns_next_ms = lan_now();
    return s->mdns_fd >= 0 || s->mdns_q_fd >= 0;
}

void lan_mdns_close(lan_scan_t *s)
{
    if (s->mdns_fd >= 0) {
        neos_sock_close(s->mdns_fd);
        s->mdns_fd = -1;
    }
    if (s->mdns_q_fd >= 0) {
        neos_sock_close(s->mdns_q_fd);
        s->mdns_q_fd = -1;
    }
}

static void drain(lan_scan_t *s, int fd)
{
    if (fd < 0) {
        return;
    }
    uint8_t  buf[1500];
    uint32_t from = 0;

    for (int guard = 0; guard < 8; guard++) {
        const int n = neos_sock_recvfrom(fd, buf, (int)sizeof(buf), &from, NULL);
        if (n <= 0) {
            return;
        }
        if (from == s->iface.ip || from == 0) {
            continue;                    /* our own question, coming back */
        }
        walk_ctx_t ctx = { .scan = s, .from = from };
        lan_dns_walk(buf, n, on_record, &ctx);
    }
}

void lan_mdns_poll(lan_scan_t *s)
{
    drain(s, s->mdns_fd);
    drain(s, s->mdns_q_fd);

    if (s->mdns_q_fd < 0 || (int32_t)(lan_now() - s->mdns_next_ms) < 0) {
        return;
    }
    s->mdns_next_ms = lan_now() + MDNS_ROUND_MS;

    const int first = s->mdns_round * MDNS_PER_QUERY;
    if (first >= MDNS_SERVICES) {
        return;                          /* every type has been asked about */
    }
    s->mdns_round++;

    const char *names[MDNS_PER_QUERY];
    uint16_t    types[MDNS_PER_QUERY];
    int         count = 0;
    for (int i = first; i < MDNS_SERVICES && count < MDNS_PER_QUERY; i++) {
        names[count] = s_services[i];
        types[count] = LAN_DNS_PTR;
        count++;
    }

    uint8_t   query[512];
    const int qlen = lan_dns_query(query, (int)sizeof(query), names, types,
                                   count, 0, true);
    if (qlen > 0) {
        neos_sock_sendto(s->mdns_q_fd, query, qlen, MDNS_GROUP, MDNS_PORT);
    }
}
