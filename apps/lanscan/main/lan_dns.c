/*
 * Just enough DNS wire format.
 *
 * Queries, and reading back the four record types that carry device identity:
 * A, PTR, SRV and TXT. It serves both the reverse-lookup stage, which asks a
 * resolver, and the mDNS browser, which asks the whole segment - they are the
 * same bytes with a different destination address, which is the reason there
 * is one of these and not two.
 *
 * Parsing is defensive to the point of paranoia, and deliberately so. Every
 * byte here came from a device nobody chose, over UDP, and a compression
 * pointer is an offset that the sender picks: a name that points at itself is
 * two lines of malformed packet and an infinite loop in anything that follows
 * it without counting. Nothing in this file can read outside the buffer it
 * was given, and nothing in it can fail to return.
 */

#include "lanscan.h"

int lan_dns_name(uint8_t *out, int size, const char *name)
{
    if (!out || !name || size < 2) {
        return 0;
    }

    int at = 0;
    const char *p = name;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') {
            dot++;
        }
        int len = (int)(dot - p);
        if (len > 63) {
            len = 63;
        }
        if (len > 0) {
            if (at + 1 + len + 1 > size) {
                return 0;
            }
            out[at++] = (uint8_t)len;
            memcpy(out + at, p, (size_t)len);
            at += len;
        }
        p = *dot ? dot + 1 : dot;
    }

    if (at + 1 > size) {
        return 0;
    }
    out[at++] = 0;
    return at;
}

int lan_dns_query(uint8_t *out, int size, const char *const *names,
                  const uint16_t *types, int count, uint16_t txid, bool unicast)
{
    if (!out || size < 12 || count <= 0) {
        return 0;
    }

    /*
     * The QU bit - "answer me directly, do not multicast it at everybody".
     * It is what makes a query to one host produce one reply to us rather
     * than a broadcast every device on the segment has to read and discard.
     */
    const uint16_t qclass = unicast ? 0x8001u : 0x0001u;

    out[0] = (uint8_t)(txid >> 8);
    out[1] = (uint8_t)txid;
    out[2] = 0; out[3] = 0;                        /* flags: a plain query */
    out[4] = (uint8_t)(count >> 8); out[5] = (uint8_t)count;
    out[6] = 0; out[7] = 0;
    out[8] = 0; out[9] = 0;
    out[10] = 0; out[11] = 0;

    int at = 12;
    int asked = 0;
    for (int i = 0; i < count; i++) {
        const int n = lan_dns_name(out + at, size - at - 4, names[i]);
        if (n == 0) {
            break;                                 /* out of room; send what fits */
        }
        at += n;
        out[at++] = (uint8_t)(types[i] >> 8);
        out[at++] = (uint8_t)types[i];
        out[at++] = (uint8_t)(qclass >> 8);
        out[at++] = (uint8_t)qclass;
        asked++;
    }
    if (asked == 0) {
        return 0;
    }
    out[4] = (uint8_t)(asked >> 8);
    out[5] = (uint8_t)asked;
    return at;
}

int lan_dns_read_name(const uint8_t *buf, int len, int off, char *out, size_t size)
{
    if (out && size) {
        out[0] = 0;
    }

    size_t wrote  = 0;
    int    end    = off;
    bool   jumped = false;
    int    hops   = 0;

    while (off >= 0 && off < len) {
        const uint8_t label = buf[off];

        if (label == 0) {
            off++;
            if (!jumped) {
                end = off;
            }
            break;
        }

        if ((label & 0xC0) == 0xC0) {
            if (off + 1 >= len) {
                break;
            }
            const int target = ((label & 0x3F) << 8) | buf[off + 1];
            if (!jumped) {
                end = off + 2;
            }
            /*
             * The hop count is the whole defence. A pointer may legitimately
             * chain, so refusing the second one would break ordinary packets;
             * what cannot happen is sixty-four of them, and a name that wants
             * that many is a name being used as a weapon.
             */
            if (++hops > 64 || target >= len) {
                break;
            }
            off    = target;
            jumped = true;
            continue;
        }

        off++;
        int n = label;
        if (off + n > len) {
            n = len - off;
        }
        if (out && wrote + (size_t)n + 2 < size) {
            if (wrote) {
                out[wrote++] = '.';
            }
            for (int i = 0; i < n; i++) {
                const uint8_t c = buf[off + i];
                out[wrote++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
            out[wrote] = 0;
        }
        off += n;
        if (!jumped) {
            end = off;
        }
    }

    if (out && size) {
        out[wrote < size ? wrote : size - 1] = 0;
    }
    return end;
}

void lan_dns_walk(const uint8_t *buf, int len, lan_dns_rr_fn fn, void *ctx)
{
    if (!buf || len < 12 || !fn) {
        return;
    }

    const int qd = (buf[4] << 8) | buf[5];
    const int an = (buf[6] << 8) | buf[7];
    const int ns = (buf[8] << 8) | buf[9];
    const int ar = (buf[10] << 8) | buf[11];

    int off = 12;

    for (int i = 0; i < qd && i < 32; i++) {
        off = lan_dns_read_name(buf, len, off, NULL, 0);
        off += 4;                                  /* qtype, qclass */
        if (off >= len) {
            return;
        }
    }

    int total = an + ns + ar;
    if (total > 128) {
        total = 128;
    }

    for (int i = 0; i < total; i++) {
        if (off >= len) {
            return;
        }
        lan_dns_rr_t rr;
        off = lan_dns_read_name(buf, len, off, rr.name, sizeof(rr.name));
        if (off + 10 > len) {
            return;
        }
        rr.type = (uint16_t)((buf[off] << 8) | buf[off + 1]);
        const int rdlen = (buf[off + 8] << 8) | buf[off + 9];
        off += 10;
        if (off + rdlen > len) {
            return;
        }
        rr.rdata = buf + off;
        rr.rdlen = rdlen;
        fn(&rr, buf, len, ctx);
        off += rdlen;
    }
}

void lan_dns_reverse(uint32_t ip, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u.in-addr.arpa",
             (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF));
}
