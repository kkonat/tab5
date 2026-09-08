/*
 * NetBIOS node status, UDP 137.
 *
 * Fifty bytes out, and back comes a Windows or Samba machine's name, its
 * workgroup, the roles it plays and - in the statistics block nobody reads -
 * its MAC address. No session, no authentication, no SMB. It is the single
 * highest-value question on a mixed network, and printers and NAS boxes
 * answer it too.
 *
 * The reply layout has one trap in it. Most stacks answer with QDCOUNT of
 * zero and a literal 34-byte name in the answer record, but some echo the
 * question back first, so the payload does not start at a fixed offset. The
 * header counts and a real name-walker decide where it starts, which costs
 * ten lines and is the difference between reading half the network's names
 * and reading all of them.
 */

#include "lanscan.h"

#define NBT_PORT     137
#define NBT_BATCH     16
#define NBT_LINGER  1600

/* Suffix byte -> what that name is for. Only the ones worth acting on. */
#define NBT_WORKSTATION  0x00
#define NBT_MESSENGER    0x03
#define NBT_MASTER       0x1D
#define NBT_FILESERVER   0x20

/*
 * First-level encoding: sixteen bytes become thirty-two, each nibble carried
 * as a letter from 'A'. "*" padded with NULs is the wildcard that asks a node
 * for everything it calls itself.
 */
static int encode_wildcard(uint8_t *out)
{
    uint8_t name[16];
    memset(name, 0, sizeof(name));
    name[0] = '*';

    int at = 0;
    out[at++] = 32;
    for (int i = 0; i < 16; i++) {
        out[at++] = (uint8_t)((name[i] >> 4) + 'A');
        out[at++] = (uint8_t)((name[i] & 0x0F) + 'A');
    }
    out[at++] = 0;
    return at;
}

static int build_query(uint8_t *out, int size)
{
    if (size < 50) {
        return 0;
    }
    out[0] = 0x4C; out[1] = 0x53;                  /* a transaction id, 'LS' */
    out[2] = 0; out[3] = 0;
    out[4] = 0; out[5] = 1;                        /* one question */
    out[6] = 0; out[7] = 0;
    out[8] = 0; out[9] = 0;
    out[10] = 0; out[11] = 0;

    int at = 12;
    at += encode_wildcard(out + at);
    out[at++] = 0x00; out[at++] = 0x21;            /* NBSTAT */
    out[at++] = 0x00; out[at++] = 0x01;            /* IN */
    return at;
}

/* Step over one DNS-style name: literal labels, or a compression pointer. */
static int skip_name(const uint8_t *data, int len, int off)
{
    for (int guard = 0; guard < 64 && off < len; guard++) {
        const uint8_t label = data[off];
        if (label == 0) {
            return off + 1;
        }
        if ((label & 0xC0) == 0xC0) {
            return off + 2;
        }
        off += 1 + label;
    }
    return off;
}

static void parse(lan_scan_t *s, uint32_t from, const uint8_t *data, int len)
{
    if (len < 57) {
        return;
    }
    const int qd = (data[4] << 8) | data[5];
    const int an = (data[6] << 8) | data[7];
    if (an < 1) {
        return;
    }

    int off = 12;
    for (int i = 0; i < qd && i < 8; i++) {
        off = skip_name(data, len, off) + 4;
    }
    off = skip_name(data, len, off);
    off += 8;                                      /* type, class, ttl */
    if (off + 2 > len) {
        return;
    }
    const int rdlen = (data[off] << 8) | data[off + 1];
    off += 2;

    int end = off + rdlen;
    if (end > len) {
        end = len;
    }
    if (off >= end) {
        return;
    }

    int count = data[off++];
    if (count > 64) {
        count = 64;
    }

    bool named   = false;
    bool grouped = false;
    bool serves  = false;

    for (int i = 0; i < count; i++) {
        if (off + 18 > end) {
            break;
        }
        char raw[16];
        memcpy(raw, data + off, 15);
        raw[15] = 0;
        const uint8_t  suffix = data[off + 15];
        const uint16_t flags  = (uint16_t)((data[off + 16] << 8) | data[off + 17]);
        off += 18;

        char name[LAN_NAME_LEN];
        lan_clean(name, sizeof(name), raw);
        if (name[0] == 0) {
            continue;
        }
        const bool is_group = (flags & 0x8000u) != 0;

        if (suffix == NBT_WORKSTATION && !is_group && !named) {
            lan_set_name(&s->reg, from, LAN_NAME_NBT, name);
            named = true;
        } else if (suffix == NBT_WORKSTATION && is_group && !grouped) {
            lan_device_t *d = lan_seen(&s->reg, from, LAN_SEEN_NBT);
            if (d) {
                lan_copy(d->workgroup, sizeof(d->workgroup), name);
            }
            grouped = true;
        } else if (suffix == NBT_MASTER && !grouped) {
            lan_device_t *d = lan_seen(&s->reg, from, LAN_SEEN_NBT);
            if (d) {
                lan_copy(d->workgroup, sizeof(d->workgroup), name);
            }
            grouped = true;
        } else if (suffix == NBT_FILESERVER) {
            serves = true;
        }
    }

    if (!named && !grouped && !serves) {
        return;
    }
    lan_seen(&s->reg, from, LAN_SEEN_NBT);
    if (serves) {
        lan_add_service(&s->reg, from, "smb");
    }

    /*
     * The statistics block that follows the name list opens with the
     * adapter's unit id, which is its MAC address. It is the reason this
     * stage is worth running even against a host that has already been named:
     * a device behind a switch that never answered an ARP request of ours
     * hands over its hardware address here for free.
     */
    if (off + 6 <= end) {
        lan_set_mac(&s->reg, from, data + off);
    }
    s->hits++;
}

bool lan_nbt_start(lan_scan_t *s)
{
    lan_nbt_stop(s);

    s->udp_fd = neos_sock_open(NEOS_SOCK_UDP);
    if (s->udp_fd < 0) {
        return false;
    }
    s->cursor   = 0;
    s->sent     = 0;
    s->hits     = 0;
    s->deadline = 0;
    return true;
}

void lan_nbt_stop(lan_scan_t *s)
{
    if (s->udp_fd >= 0) {
        neos_sock_close(s->udp_fd);
        s->udp_fd = -1;
    }
}

void lan_nbt_poll(lan_scan_t *s)
{
    if (s->udp_fd < 0) {
        return;
    }

    uint8_t  buf[1024];
    uint32_t from = 0;
    for (int guard = 0; guard < 8; guard++) {
        const int n = neos_sock_recvfrom(s->udp_fd, buf, (int)sizeof(buf), &from, NULL);
        if (n <= 0) {
            break;
        }
        parse(s, from, buf, n);
    }

    if (s->cursor < s->net_hosts) {
        uint8_t   query[64];
        const int qlen = build_query(query, (int)sizeof(query));
        const int stop = s->cursor + NBT_BATCH;

        while (qlen > 0 && s->cursor < s->net_hosts && s->cursor < stop) {
            const uint32_t ip = s->net_base + (uint32_t)s->cursor;
            s->cursor++;
            if (!lan_find(&s->reg, ip)) {
                continue;
            }
            if (neos_sock_sendto(s->udp_fd, query, qlen, ip, NBT_PORT) > 0) {
                s->sent++;
            }
        }
        if (s->cursor >= s->net_hosts) {
            s->deadline = lan_now() + NBT_LINGER;
        }
    }
}

bool lan_nbt_done(lan_scan_t *s)
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
