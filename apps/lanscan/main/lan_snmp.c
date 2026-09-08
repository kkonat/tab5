/*
 * SNMP v2c GET, for the two strings that identify a box.
 *
 * Routers, switches, printers and NAS units very often still answer the
 * "public" community, and sysDescr is the most informative line you can get
 * out of one without logging in - exact model and firmware, written by the
 * vendor. sysName is what it calls itself.
 *
 * The BER is hand-rolled because only three shapes are ever encoded here - an
 * integer, an octet string and an object identifier - and none of them is
 * longer than a few dozen bytes. A general encoder would be more code than
 * this whole file.
 *
 * Only the public community is tried. The desktop version takes a list;
 * making the tablet ask a device for a community somebody typed in would be
 * making it try a password, which is a different act from reading what a
 * device volunteers to anyone who asks.
 */

#include "lanscan.h"

#define SNMP_PORT     161
#define SNMP_BATCH     16
#define SNMP_LINGER  1600

/* 1.3.6.1.2.1.1.{1,5}.0 - sysDescr and sysName, pre-encoded. The prefix is
   always the same eight bytes and encoding it at runtime would be a parser
   for a string constant. */
static const uint8_t OID_SYSDESCR[] = { 0x2B, 6, 1, 2, 1, 1, 1, 0 };
static const uint8_t OID_SYSNAME[]  = { 0x2B, 6, 1, 2, 1, 1, 5, 0 };

/* Only ever short forms here: nothing this builds exceeds 127 bytes. */
static int tlv(uint8_t *out, int at, uint8_t tag, const void *body, int len)
{
    out[at++] = tag;
    out[at++] = (uint8_t)len;
    if (len) {
        memcpy(out + at, body, (size_t)len);
    }
    return at + len;
}

static int build_get(uint8_t *out, int size, uint16_t request_id)
{
    if (size < 96) {
        return 0;
    }

    /* Two varbinds: an OID and a NULL value each, wrapped in a sequence. */
    uint8_t binds[64];
    int     b = 0;
    for (int i = 0; i < 2; i++) {
        const uint8_t *oid  = i ? OID_SYSNAME : OID_SYSDESCR;
        const int      olen = i ? (int)sizeof(OID_SYSNAME) : (int)sizeof(OID_SYSDESCR);

        uint8_t one[24];
        int     n = 0;
        n = tlv(one, n, 0x06, oid, olen);
        n = tlv(one, n, 0x05, NULL, 0);
        b = tlv(binds, b, 0x30, one, n);
    }

    uint8_t pdu[96];
    int     p = 0;
    const uint8_t id[2] = { (uint8_t)(request_id >> 8), (uint8_t)request_id };
    const uint8_t zero  = 0;
    p = tlv(pdu, p, 0x02, id, 2);                  /* request id */
    p = tlv(pdu, p, 0x02, &zero, 1);               /* error status */
    p = tlv(pdu, p, 0x02, &zero, 1);               /* error index */
    p = tlv(pdu, p, 0x30, binds, b);

    uint8_t body[128];
    int     m = 0;
    const uint8_t version = 1;                     /* 1 means v2c */
    m = tlv(body, m, 0x02, &version, 1);
    m = tlv(body, m, 0x04, "public", 6);
    m = tlv(body, m, 0xA0, pdu, p);                /* GetRequest */

    return tlv(out, 0, 0x30, body, m);
}

/* -> {tag, value offset, value length, offset after}. False if it ran off. */
static bool read_tlv(const uint8_t *d, int len, int off,
                     uint8_t *tag, int *voff, int *vlen, int *next)
{
    if (off + 2 > len) {
        return false;
    }
    *tag = d[off];
    int length = d[off + 1];
    off += 2;

    if (length & 0x80) {
        const int count = length & 0x7F;
        if (count > 4 || off + count > len) {
            return false;
        }
        length = 0;
        for (int i = 0; i < count; i++) {
            length = (length << 8) | d[off + i];
        }
        off += count;
    }
    if (length < 0 || off + length > len) {
        return false;
    }
    *voff = off;
    *vlen = length;
    *next = off + length;
    return true;
}

static void parse(lan_scan_t *s, uint32_t from, const uint8_t *d, int len)
{
    uint8_t tag;
    int     voff, vlen, off;

    if (!read_tlv(d, len, 0, &tag, &voff, &vlen, &off)) {
        return;
    }
    off = voff;                                    /* into the outer sequence */
    if (!read_tlv(d, len, off, &tag, &voff, &vlen, &off)) { return; }  /* version */
    if (!read_tlv(d, len, off, &tag, &voff, &vlen, &off)) { return; }  /* community */
    if (!read_tlv(d, len, off, &tag, &voff, &vlen, &off)) { return; }  /* PDU */
    if (tag != 0xA2) {
        return;                                    /* not a GetResponse */
    }

    off = voff;
    for (int i = 0; i < 3; i++) {                  /* id, error, index */
        if (!read_tlv(d, len, off, &tag, &voff, &vlen, &off)) {
            return;
        }
    }
    if (!read_tlv(d, len, off, &tag, &voff, &vlen, &off)) {
        return;                                    /* the varbind list */
    }

    const int list_end = voff + vlen;
    off = voff;
    bool got = false;

    while (off < list_end) {
        int inner, ilen, after;
        if (!read_tlv(d, len, off, &tag, &inner, &ilen, &after)) {
            return;
        }
        off = after;

        int oid_off, oid_len, next;
        if (!read_tlv(d, len, inner, &tag, &oid_off, &oid_len, &next)) {
            return;
        }
        int val_off, val_len, unused;
        if (!read_tlv(d, len, next, &tag, &val_off, &val_len, &unused)) {
            return;
        }
        if (tag != 0x04 || val_len <= 0) {
            continue;                              /* noSuchObject, or a number */
        }

        /* Which of the two we asked for: the last two bytes of the OID say,
           and both are the same length, so comparing the tail is enough. */
        const bool is_name = (oid_len == (int)sizeof(OID_SYSNAME) &&
                              memcmp(d + oid_off, OID_SYSNAME, (size_t)oid_len) == 0);
        const bool is_desc = (oid_len == (int)sizeof(OID_SYSDESCR) &&
                              memcmp(d + oid_off, OID_SYSDESCR, (size_t)oid_len) == 0);

        char text[LAN_DETAIL_LEN];
        int  n = val_len < (int)sizeof(text) - 1 ? val_len : (int)sizeof(text) - 1;
        memcpy(text, d + val_off, (size_t)n);
        text[n] = 0;

        char clean[LAN_DETAIL_LEN];
        lan_clean(clean, sizeof(clean), text);
        if (clean[0] == 0) {
            continue;
        }

        if (is_name) {
            lan_set_name(&s->reg, from, LAN_NAME_SNMP, clean);
            got = true;
        } else if (is_desc) {
            lan_set_detail(&s->reg, from, clean);
            got = true;
        }
    }

    if (got) {
        lan_seen(&s->reg, from, LAN_SEEN_SNMP);
        lan_add_service(&s->reg, from, "snmp");
        s->hits++;
    }
}

bool lan_snmp_start(lan_scan_t *s)
{
    lan_snmp_stop(s);

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

void lan_snmp_stop(lan_scan_t *s)
{
    if (s->udp_fd >= 0) {
        neos_sock_close(s->udp_fd);
        s->udp_fd = -1;
    }
}

void lan_snmp_poll(lan_scan_t *s)
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
        const int stop = s->cursor + SNMP_BATCH;
        while (s->cursor < s->net_hosts && s->cursor < stop) {
            const uint32_t ip = s->net_base + (uint32_t)s->cursor;
            s->cursor++;
            if (!lan_find(&s->reg, ip)) {
                continue;
            }
            uint8_t   query[160];
            const int qlen = build_get(query, (int)sizeof(query), (uint16_t)(ip & 0xFFFF));
            if (qlen > 0 && neos_sock_sendto(s->udp_fd, query, qlen, ip, SNMP_PORT) > 0) {
                s->sent++;
            }
        }
        if (s->cursor >= s->net_hosts) {
            s->deadline = lan_now() + SNMP_LINGER;
        }
    }
}

bool lan_snmp_done(lan_scan_t *s)
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
