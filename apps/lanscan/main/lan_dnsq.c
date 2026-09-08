/*
 * Reverse lookups.
 *
 * One socket, a PTR query per known address, all of them sent before any
 * answer is waited for. A resolver on a home router answers these in single
 * milliseconds and the whole stage is over in about as long as it takes to
 * ask, so there is no reason to serialise it into one query at a time.
 *
 * Every answer comes from the same place - the resolver - so the sender's
 * address says nothing about which host an answer is about. The question name
 * does: "14.0.168.192.in-addr.arpa" is read back out of the record and turned
 * into an address again. That is also what makes an out-of-order or
 * duplicated reply harmless.
 */

#include "lanscan.h"

#define DNS_PORT     53
#define DNS_BATCH    16
#define DNS_LINGER  1500

typedef struct {
    lan_scan_t *scan;
} walk_ctx_t;

/* "14.0.168.192.in-addr.arpa" -> 192.168.0.14, or 0 if it is not one. */
static uint32_t reverse_to_ip(const char *name)
{
    unsigned octet[4] = { 0, 0, 0, 0 };
    int      at       = 0;
    unsigned value    = 0;
    bool     digits   = false;

    for (const char *p = name; ; p++) {
        if (*p >= '0' && *p <= '9') {
            value  = value * 10 + (unsigned)(*p - '0');
            digits = true;
            if (value > 255) {
                return 0;
            }
            continue;
        }
        if (*p == '.' || *p == 0) {
            if (!digits) {
                break;
            }
            if (at < 4) {
                octet[at++] = value;
            }
            value  = 0;
            digits = false;
            if (*p == 0 || at == 4) {
                break;
            }
            continue;
        }
        break;
    }

    if (at != 4 || !lan_has(name, "in-addr.arpa")) {
        return 0;
    }
    /* The labels are least significant first, which is the whole point of
       the format and the one place it is easy to get backwards. */
    return (octet[3] << 24) | (octet[2] << 16) | (octet[1] << 8) | octet[0];
}

static void on_record(const lan_dns_rr_t *rr, const uint8_t *buf, int len, void *ctx)
{
    walk_ctx_t *w = (walk_ctx_t *)ctx;

    if (rr->type != LAN_DNS_PTR) {
        return;
    }
    const uint32_t ip = reverse_to_ip(rr->name);
    if (!ip || !lan_find(&w->scan->reg, ip)) {
        return;
    }

    char host[LAN_NAME_LEN];
    lan_dns_read_name(buf, len, (int)(rr->rdata - buf), host, sizeof(host));
    if (host[0] == 0) {
        return;
    }

    /*
     * A fully qualified name is mostly domain, and the domain is the same for
     * every row. Keep the first label, which is the part that identifies the
     * machine - unless that leaves nothing, in which case keep what there is.
     */
    char *dot = strchr(host, '.');
    if (dot && dot != host) {
        *dot = 0;
    }

    lan_seen(&w->scan->reg, ip, LAN_SEEN_DNS);
    lan_set_name(&w->scan->reg, ip, LAN_NAME_DNS, host);
    w->scan->hits++;
}

bool lan_dnsq_start(lan_scan_t *s)
{
    lan_dnsq_stop(s);

    if (s->iface.dns == 0) {
        return false;           /* no resolver in the lease; nothing to ask */
    }
    s->udp_fd = neos_sock_open(NEOS_SOCK_UDP);
    if (s->udp_fd < 0) {
        return false;
    }
    s->cursor   = 0;
    s->hits     = 0;
    s->sent     = 0;
    s->deadline = 0;
    return true;
}

void lan_dnsq_stop(lan_scan_t *s)
{
    if (s->udp_fd >= 0) {
        neos_sock_close(s->udp_fd);
        s->udp_fd = -1;
    }
}

void lan_dnsq_poll(lan_scan_t *s)
{
    if (s->udp_fd < 0) {
        return;
    }

    uint8_t    buf[768];
    walk_ctx_t ctx = { .scan = s };
    for (int guard = 0; guard < 8; guard++) {
        const int n = neos_sock_recvfrom(s->udp_fd, buf, (int)sizeof(buf), NULL, NULL);
        if (n <= 0) {
            break;
        }
        lan_dns_walk(buf, n, on_record, &ctx);
    }

    if (s->cursor < s->net_hosts) {
        const int stop = s->cursor + DNS_BATCH;
        while (s->cursor < s->net_hosts && s->cursor < stop) {
            const uint32_t ip = s->net_base + (uint32_t)s->cursor;
            s->cursor++;
            if (!lan_find(&s->reg, ip)) {
                continue;       /* only ask about addresses we know are there */
            }

            char name[48];
            lan_dns_reverse(ip, name, sizeof(name));
            const char    *names[1] = { name };
            const uint16_t types[1] = { LAN_DNS_PTR };

            uint8_t query[96];
            const int qlen = lan_dns_query(query, (int)sizeof(query), names, types,
                                           1, (uint16_t)(ip & 0xFFFF), false);
            if (qlen > 0 && neos_sock_sendto(s->udp_fd, query, qlen,
                                             s->iface.dns, DNS_PORT) > 0) {
                s->sent++;
            }
        }
        if (s->cursor >= s->net_hosts) {
            s->deadline = lan_now() + DNS_LINGER;
        }
    }
}

bool lan_dnsq_done(lan_scan_t *s)
{
    if (s->udp_fd < 0) {
        return true;
    }
    if (s->sent == 0 && s->cursor >= s->net_hosts) {
        return true;
    }
    return s->cursor >= s->net_hosts && s->deadline &&
           (int32_t)(lan_now() - s->deadline) >= 0;
}
