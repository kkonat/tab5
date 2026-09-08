/*
 * What we have learned so far.
 *
 * One flat array kept in address order, searched by walking it. That is a
 * linear scan per write and there are a few thousand writes in a pass, which
 * on a 360 MHz core with 96 entries is not worth a hash table and its
 * collision policy - and an ordered array is also what the table on screen
 * wants to read, so keeping it sorted here means nothing has to sort it
 * later.
 *
 * The desktop version's Registry is mostly a lock. There is no lock here and
 * that is not an omission: an app is the only thing running, every probe and
 * the drawing are the same thread, and a mutex would be protecting the code
 * from itself.
 */

#include "lanscan.h"

/*
 * malloc, not a static array.
 *
 * The table is about 250 KB and malloc() on this machine is PSRAM for
 * anything over a kilobyte, which is exactly where a quarter of a megabyte of
 * cold, once-per-frame-at-most data belongs. Putting it in .bss would put it
 * there too, but it would also make every copy of the app on a card carry the
 * size in its program headers, and it would still be resident while a scan is
 * not running.
 */
bool lan_model_init(lan_registry_t *reg)
{
    reg->dev = (lan_device_t *)malloc(sizeof(lan_device_t) * LAN_MAX_DEVICES);
    if (!reg->dev) {
        return false;
    }
    lan_model_clear(reg);
    return true;
}

void lan_model_free(lan_registry_t *reg)
{
    if (reg->dev) {
        free(reg->dev);
        reg->dev = NULL;
    }
    reg->count = 0;
}

void lan_model_clear(lan_registry_t *reg)
{
    if (reg->dev) {
        memset(reg->dev, 0, sizeof(lan_device_t) * LAN_MAX_DEVICES);
    }
    reg->count    = 0;
    reg->overflow = 0;
    reg->revision++;
}

lan_device_t *lan_find(lan_registry_t *reg, uint32_t ip)
{
    for (int i = 0; i < reg->count; i++) {
        if (reg->dev[i].ip == ip) {
            return &reg->dev[i];
        }
        /* Sorted, so the first address past the one wanted ends the search. */
        if (reg->dev[i].ip > ip) {
            return NULL;
        }
    }
    return NULL;
}

lan_device_t *lan_seen(lan_registry_t *reg, uint32_t ip, uint16_t how)
{
    if (ip == 0 || ip == 0xFFFFFFFFu) {
        return NULL;
    }

    int at = 0;
    while (at < reg->count && reg->dev[at].ip < ip) {
        at++;
    }
    if (at < reg->count && reg->dev[at].ip == ip) {
        lan_device_t *d = &reg->dev[at];
        if (how && (d->seen & how) != how) {
            d->seen |= how;
            reg->revision++;
        }
        return d;
    }

    if (reg->count >= LAN_MAX_DEVICES) {
        /*
         * Counted rather than dropped silently. The footer shows this number,
         * because a scanner that quietly stopped at 96 devices would look
         * exactly like a network that has 96 devices on it.
         */
        reg->overflow++;
        reg->revision++;
        return NULL;
    }

    if (at < reg->count) {
        memmove(&reg->dev[at + 1], &reg->dev[at],
                sizeof(lan_device_t) * (size_t)(reg->count - at));
    }
    memset(&reg->dev[at], 0, sizeof(lan_device_t));
    reg->dev[at].ip       = ip;
    reg->dev[at].seen     = how;
    reg->dev[at].name_src = LAN_NAME_NONE;
    reg->count++;
    reg->revision++;
    return &reg->dev[at];
}

void lan_set_mac(lan_registry_t *reg, uint32_t ip, const uint8_t mac[6])
{
    static const uint8_t zero[6] = { 0, 0, 0, 0, 0, 0 };

    if (!mac || memcmp(mac, zero, 6) == 0) {
        return;
    }
    /* A multicast or broadcast MAC is never a device's own address, and both
       turn up in an ARP table that has been shouted at. */
    if (mac[0] & 0x01) {
        return;
    }

    lan_device_t *d = lan_seen(reg, ip, LAN_SEEN_ARP);
    if (!d) {
        return;
    }
    if (d->has_mac && memcmp(d->mac, mac, 6) == 0) {
        return;
    }
    memcpy(d->mac, mac, 6);
    d->has_mac = true;

    /*
     * Bit 0x02 of the first octet is the locally-administered bit, which on a
     * LAN in practice means a phone rotating its address for privacy. There
     * is no manufacturer to look up and there never will be, so this is said
     * outright rather than left looking like a lookup that has not finished.
     */
    if ((mac[0] & 0x02) && d->vendor[0] == 0) {
        lan_copy(d->vendor, sizeof(d->vendor), "(random MAC)");
    }
    reg->revision++;
}

void lan_set_name(lan_registry_t *reg, uint32_t ip, lan_name_src_t src, const char *name)
{
    char clean[LAN_NAME_LEN];
    lan_clean(clean, sizeof(clean), name);
    if (clean[0] == 0) {
        return;
    }

    lan_device_t *d = lan_seen(reg, ip, 0);
    if (!d) {
        return;
    }
    /* A worse source never overwrites a better one, so a device that
       announced itself over mDNS keeps that name when the reverse lookup
       later produces 192-168-0-14.static.example.net. */
    if (d->name[0] && d->name_src <= (uint8_t)src) {
        return;
    }
    lan_copy(d->name, sizeof(d->name), clean);
    d->name_src = (uint8_t)src;
    reg->revision++;
}

void lan_set_alive(lan_registry_t *reg, uint32_t ip, uint8_t ttl, uint16_t rtt_ms)
{
    lan_device_t *d = lan_seen(reg, ip, LAN_SEEN_ICMP);
    if (!d) {
        return;
    }
    if (ttl) {
        d->ttl = ttl;
    }
    if (rtt_ms) {
        d->rtt_ms = rtt_ms;
    }
    reg->revision++;
}

void lan_add_service(lan_registry_t *reg, uint32_t ip, const char *label)
{
    char clean[LAN_SERVICE_LEN];
    lan_clean(clean, sizeof(clean), label);
    if (clean[0] == 0) {
        return;
    }

    lan_device_t *d = lan_seen(reg, ip, 0);
    if (!d) {
        return;
    }
    for (int i = 0; i < d->nservices; i++) {
        if (lan_ieq(d->services[i], clean)) {
            return;
        }
    }
    if (d->nservices >= LAN_MAX_SERVICES) {
        return;
    }
    lan_copy(d->services[d->nservices++], LAN_SERVICE_LEN, clean);
    reg->revision++;
}

/*
 * The one line under the name: whatever the device said about itself that is
 * richest. UPnP beats SNMP beats a model string, and the first one to arrive
 * wins within a tier - which is why this only ever fills an empty field. A
 * later, poorer line replacing a good one would make the table change under
 * the reader for no gain.
 */
void lan_set_detail(lan_registry_t *reg, uint32_t ip, const char *text)
{
    char clean[LAN_DETAIL_LEN];
    lan_clean(clean, sizeof(clean), text);
    if (clean[0] == 0) {
        return;
    }

    lan_device_t *d = lan_seen(reg, ip, 0);
    if (!d || d->detail[0]) {
        return;
    }
    lan_copy(d->detail, sizeof(d->detail), clean);
    reg->revision++;
}

lan_port_t *lan_add_port(lan_registry_t *reg, uint32_t ip, uint16_t port)
{
    lan_device_t *d = lan_seen(reg, ip, LAN_SEEN_TCP);
    if (!d) {
        return NULL;
    }

    int at = 0;
    while (at < d->nports && d->ports[at].port < port) {
        at++;
    }
    if (at < d->nports && d->ports[at].port == port) {
        return &d->ports[at];
    }
    if (d->nports >= LAN_MAX_PORTS) {
        return NULL;
    }
    if (at < d->nports) {
        memmove(&d->ports[at + 1], &d->ports[at],
                sizeof(lan_port_t) * (size_t)(d->nports - at));
    }
    memset(&d->ports[at], 0, sizeof(lan_port_t));
    d->ports[at].port = port;
    lan_copy(d->ports[at].service, sizeof(d->ports[at].service),
             lan_service_name(port));
    d->nports++;
    reg->revision++;
    return &d->ports[at];
}
