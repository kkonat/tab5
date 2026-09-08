/*
 * SSDP, and the UPnP description it points at.
 *
 * The other shout, and the other half of where readable names come from. One
 * M-SEARCH gets routers, televisions, printers, media servers, smart plugs
 * and games consoles to announce themselves with a SERVER string and a
 * LOCATION - a URL to an XML document that names the manufacturer, the model
 * and whatever the owner called the thing. On most home networks that XML is
 * the only place a device's actual product name is written down.
 *
 * Like mDNS this is ambient rather than a stage: the searches go out in
 * rounds and the replies are read on every tick for the life of the scan.
 * What is a stage is fetching the XML, because that is TCP and it queues into
 * the shared pool with everything else - see LAN_STAGE_UPNP in lan_engine.c.
 * All this does is remember where to go.
 */

#include "lanscan.h"

#define SSDP_GROUP  0xEFFFFFFAu     /* 239.255.255.250 */
#define SSDP_PORT   1900
#define SSDP_ROUND_MS  1100

static const char *const s_targets[] = {
    "ssdp:all",
    "upnp:rootdevice",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:device:MediaServer:1",
    "urn:dial-multiscreen-org:service:dial:1",
};
#define SSDP_TARGETS  ((int)(sizeof(s_targets) / sizeof(s_targets[0])))

bool lan_ssdp_open(lan_scan_t *s)
{
    lan_ssdp_close(s);

    s->ssdp_fd = neos_sock_open(NEOS_SOCK_UDP);
    if (s->ssdp_fd < 0) {
        return false;
    }
    /* An ephemeral port on our own address: the replies to an M-SEARCH are
       unicast back to wherever it came from, so nothing needs to be bound to
       1900 unless the unsolicited announcements are wanted too. */
    neos_sock_bind(s->ssdp_fd, s->iface.ip, 0);
    neos_sock_set(s->ssdp_fd, NEOS_SOPT_MCAST_IF, s->iface.ip);
    neos_sock_set(s->ssdp_fd, NEOS_SOPT_MCAST_TTL, 2);
    neos_sock_set(s->ssdp_fd, NEOS_SOPT_MCAST_LOOP, 0);

    s->ssdp_round   = 0;
    s->ssdp_next_ms = lan_now();
    return true;
}

void lan_ssdp_close(lan_scan_t *s)
{
    if (s->ssdp_fd >= 0) {
        neos_sock_close(s->ssdp_fd);
        s->ssdp_fd = -1;
    }
}

/* One header value out of an HTTP-shaped response, by name. */
static bool header(const char *msg, const char *name, char *out, size_t size)
{
    out[0] = 0;
    const char *p = lan_after(msg, name);
    if (!p) {
        return false;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ') {
        p++;
    }

    size_t at = 0;
    while (*p && *p != '\r' && *p != '\n' && at + 1 < size) {
        out[at++] = *p++;
    }
    out[at] = 0;
    return at > 0;
}

/*
 * "http://192.168.0.1:49152/rootDesc.xml" -> port and path.
 *
 * The host is deliberately not taken from the URL. A device is free to write
 * anything there, including a name that would need resolving or an address
 * that is not its own, and the packet already told us where it came from -
 * which is the address we are going to connect to either way.
 */
static bool split_location(const char *url, uint16_t *port, char *path, size_t size)
{
    const char *p = lan_after(url, "http://");
    if (!p) {
        return false;
    }
    *port = 80;

    /* Step over the host, noticing a port if there is one. */
    while (*p && *p != '/' && *p != ':') {
        p++;
    }
    if (*p == ':') {
        p++;
        int value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            p++;
        }
        if (value <= 0 || value > 65535) {
            return false;
        }
        *port = (uint16_t)value;
    }

    if (*p != '/') {
        lan_copy(path, size, "/");
        return true;
    }
    lan_copy(path, size, p);
    return true;
}

/* The device type out of an ST or NT header: the ...:device:<kind>:1 part. */
static void device_kind(const char *st, char *out, size_t size)
{
    out[0] = 0;
    const char *p = lan_after(st, "device:");
    if (!p) {
        return;
    }
    size_t at = 0;
    while (*p && *p != ':' && at + 1 < size) {
        out[at++] = *p++;
    }
    out[at] = 0;
}

static void handle(lan_scan_t *s, uint32_t from, char *msg)
{
    lan_device_t *d = lan_seen(&s->reg, from, LAN_SEEN_SSDP);
    if (!d) {
        return;
    }

    char value[192];

    /* The SERVER string is usually "OS/version UPnP/1.1 product/version",
       which is a real answer even when the description fetch fails. */
    if (header(msg, "SERVER", value, sizeof(value))) {
        lan_set_detail(&s->reg, from, value);
    }

    if (header(msg, "LOCATION", value, sizeof(value))) {
        uint16_t port = 0;
        char     path[64];
        if (!d->upnp_port && split_location(value, &port, path, sizeof(path))) {
            d->upnp_ip   = from;
            d->upnp_port = port;
            lan_copy(d->upnp_path, sizeof(d->upnp_path), path);
        }
    }

    if (header(msg, "ST", value, sizeof(value)) ||
        header(msg, "NT", value, sizeof(value))) {
        char kind[LAN_SERVICE_LEN];
        device_kind(value, kind, sizeof(kind));
        if (kind[0]) {
            /* The prefix is what makes "MediaRenderer" read as a UPnP
               device type and not as something mDNS said, so it is the
               device type that gives up characters if the label is long. */
            char label[LAN_SERVICE_LEN];
            snprintf(label, sizeof(label), "upnp:%.18s", kind);
            lan_add_service(&s->reg, from, label);
        }
    }
}

void lan_ssdp_poll(lan_scan_t *s)
{
    if (s->ssdp_fd < 0) {
        return;
    }

    char     buf[1024];
    uint32_t from = 0;
    for (int guard = 0; guard < 8; guard++) {
        const int n = neos_sock_recvfrom(s->ssdp_fd, buf, (int)sizeof(buf) - 1,
                                         &from, NULL);
        if (n <= 0) {
            break;
        }
        buf[n] = 0;
        if (from != s->iface.ip) {
            handle(s, from, buf);
        }
    }

    if ((int32_t)(lan_now() - s->ssdp_next_ms) < 0) {
        return;
    }
    s->ssdp_next_ms = lan_now() + SSDP_ROUND_MS;

    if (s->ssdp_round >= SSDP_TARGETS) {
        return;
    }
    const char *target = s_targets[s->ssdp_round++];

    /*
     * MX is how long a device may wait before answering, and it exists so
     * that a hundred devices do not all reply in the same millisecond. Two
     * seconds is short enough that the answers land inside the scan and long
     * enough to do the job it is there for.
     */
    char msg[320];
    const int n = snprintf(msg, sizeof(msg),
                           "M-SEARCH * HTTP/1.1\r\n"
                           "HOST: 239.255.255.250:1900\r\n"
                           "MAN: \"ssdp:discover\"\r\n"
                           "MX: 2\r\n"
                           "ST: %s\r\n"
                           "USER-AGENT: NeOS/1.0 UPnP/1.1 lanscan/1.0\r\n\r\n",
                           target);
    if (n > 0) {
        neos_sock_sendto(s->ssdp_fd, msg, n, SSDP_GROUP, SSDP_PORT);
    }
}
