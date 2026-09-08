/*
 * neos_sock.h, over lwIP.
 *
 * Almost all of this is a rename: an lwIP call with a sockaddr built for it
 * and errno folded into one of four results. The rename is the point - what
 * crosses into an app is a surface NeOS can keep promising, not whatever
 * struct the stack underneath happens to use this year - so the interesting
 * code here is only the two functions at the bottom, which reach past the
 * socket layer into the ARP table and therefore have to go and stand in the
 * right thread first.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_netif.h"
#include "esp_netif_net_stack.h"

#include "lwip/etharp.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

#include "neos_sock.h"

/* ------------------------------------------------------------------ */
/* Plumbing                                                            */
/* ------------------------------------------------------------------ */

static void fill_addr(struct sockaddr_in *sa, uint32_t ip, uint16_t port)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_len         = sizeof(*sa);
    sa->sin_family      = AF_INET;
    sa->sin_port        = lwip_htons(port);
    sa->sin_addr.s_addr = lwip_htonl(ip);
}

/*
 * "It would have waited" and "it failed" arrive the same way and mean opposite
 * things, so every call that can block funnels through here rather than
 * testing errno at each site and getting one of them wrong.
 */
static int again_or_err(void)
{
    return (errno == EWOULDBLOCK || errno == EAGAIN) ? NEOS_SOCK_AGAIN
                                                     : NEOS_SOCK_ERR;
}

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

int neos_sock_open(int kind)
{
    int type, proto;
    switch (kind) {
    case NEOS_SOCK_TCP:  type = SOCK_STREAM; proto = IPPROTO_TCP;  break;
    case NEOS_SOCK_UDP:  type = SOCK_DGRAM;  proto = IPPROTO_UDP;  break;
    case NEOS_SOCK_ICMP: type = SOCK_RAW;    proto = IPPROTO_ICMP; break;
    default: return NEOS_SOCK_ERR;
    }

    const int fd = socket(AF_INET, type, proto);
    if (fd < 0) {
        return NEOS_SOCK_ERR;
    }

    /*
     * Non-blocking is not an option an app may decline - see the header. It
     * is set here, once, rather than trusted to every caller: an app that
     * forgot would not get a slow scan, it would get a tablet that has
     * stopped answering its own close button.
     */
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return NEOS_SOCK_ERR;
    }
    return fd;
}

void neos_sock_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

int neos_sock_bind(int fd, uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa;
    fill_addr(&sa, ip, port);
    return bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 ? NEOS_SOCK_OK
                                                             : NEOS_SOCK_ERR;
}

int neos_sock_connect(int fd, uint32_t ip, uint16_t port)
{
    struct sockaddr_in sa;
    fill_addr(&sa, ip, port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
        return NEOS_SOCK_OK;
    }
    if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK) {
        return NEOS_SOCK_PENDING;
    }
    return NEOS_SOCK_ERR;
}

int neos_sock_status(int fd)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return NEOS_SOCK_ERR;
    }
    if (err == 0) {
        return NEOS_SOCK_OK;
    }
    if (err == EINPROGRESS || err == EALREADY) {
        return NEOS_SOCK_PENDING;
    }
    return NEOS_SOCK_ERR;
}

int neos_sock_send(int fd, const void *buf, int len)
{
    if (!buf || len < 0) {
        return NEOS_SOCK_ERR;
    }
    const int n = send(fd, buf, (size_t)len, 0);
    return n >= 0 ? n : again_or_err();
}

int neos_sock_recv(int fd, void *buf, int len)
{
    if (!buf || len < 0) {
        return NEOS_SOCK_ERR;
    }
    const int n = recv(fd, buf, (size_t)len, 0);
    return n >= 0 ? n : again_or_err();
}

int neos_sock_sendto(int fd, const void *buf, int len, uint32_t ip, uint16_t port)
{
    if (!buf || len < 0) {
        return NEOS_SOCK_ERR;
    }
    struct sockaddr_in sa;
    fill_addr(&sa, ip, port);
    const int n = sendto(fd, buf, (size_t)len, 0,
                         (struct sockaddr *)&sa, sizeof(sa));
    return n >= 0 ? n : again_or_err();
}

int neos_sock_recvfrom(int fd, void *buf, int len,
                       uint32_t *from_ip, uint16_t *from_port)
{
    if (!buf || len < 0) {
        return NEOS_SOCK_ERR;
    }
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    memset(&sa, 0, sizeof(sa));

    const int n = recvfrom(fd, buf, (size_t)len, 0,
                           (struct sockaddr *)&sa, &salen);
    if (n < 0) {
        return again_or_err();
    }
    if (from_ip) {
        *from_ip = lwip_ntohl(sa.sin_addr.s_addr);
    }
    if (from_port) {
        *from_port = lwip_ntohs(sa.sin_port);
    }
    return n;
}

int neos_sock_wait(const int *fds, uint8_t *events, int n, uint32_t ms)
{
    if (!fds || !events || n <= 0) {
        return NEOS_SOCK_ERR;
    }

    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);

    int maxfd = -1;
    for (int i = 0; i < n; i++) {
        const int fd = fds[i];
        /*
         * A negative descriptor is a hole and not a mistake: a caller keeping
         * a fixed array of connection slots has closed sockets in it, and
         * making it compact them before every wait would be making it do
         * bookkeeping this call can skip in a branch.
         */
        if (fd < 0 || fd >= FD_SETSIZE) {
            events[i] = 0;
            continue;
        }
        if (events[i] & NEOS_SOCK_READ)  { FD_SET(fd, &rd); }
        if (events[i] & NEOS_SOCK_WRITE) { FD_SET(fd, &wr); }
        FD_SET(fd, &ex);
        if (fd > maxfd) {
            maxfd = fd;
        }
    }
    if (maxfd < 0) {
        return 0;
    }

    /* The ceiling from the header. An app cannot ask to be gone longer. */
    if (ms > 1000) {
        ms = 1000;
    }
    struct timeval tv = {
        .tv_sec  = (time_t)(ms / 1000),
        .tv_usec = (suseconds_t)((ms % 1000) * 1000),
    };

    if (select(maxfd + 1, &rd, &wr, &ex, &tv) < 0) {
        for (int i = 0; i < n; i++) {
            events[i] = 0;
        }
        return NEOS_SOCK_ERR;
    }

    int ready = 0;
    for (int i = 0; i < n; i++) {
        const int fd = fds[i];
        uint8_t got = 0;
        if (fd >= 0 && fd < FD_SETSIZE) {
            if (FD_ISSET(fd, &rd)) { got |= NEOS_SOCK_READ; }
            if (FD_ISSET(fd, &wr)) { got |= NEOS_SOCK_WRITE; }
            if (FD_ISSET(fd, &ex)) { got |= NEOS_SOCK_FAIL; }
        }
        events[i] = got;
        if (got) {
            ready++;
        }
    }
    return ready;
}

int neos_sock_set(int fd, int option, uint32_t value)
{
    int         level = SOL_SOCKET;
    int         name  = 0;
    const void *data  = NULL;
    socklen_t   size  = 0;

    const int      as_int = (int)value;
    const uint8_t  as_u8  = (uint8_t)value;
    struct in_addr as_addr;
    as_addr.s_addr = lwip_htonl(value);

    switch (option) {
    case NEOS_SOPT_BROADCAST:
        name = SO_BROADCAST; data = &as_int; size = sizeof(as_int);
        break;
    case NEOS_SOPT_REUSE:
        name = SO_REUSEADDR; data = &as_int; size = sizeof(as_int);
        break;
    case NEOS_SOPT_TTL:
        level = IPPROTO_IP; name = IP_TTL; data = &as_int; size = sizeof(as_int);
        break;
    /* The three multicast options are bytes and an address, not ints. lwIP
       checks the length, so getting this wrong fails rather than misbehaves. */
    case NEOS_SOPT_MCAST_TTL:
        level = IPPROTO_IP; name = IP_MULTICAST_TTL;
        data = &as_u8; size = sizeof(as_u8);
        break;
    case NEOS_SOPT_MCAST_LOOP:
        level = IPPROTO_IP; name = IP_MULTICAST_LOOP;
        data = &as_u8; size = sizeof(as_u8);
        break;
    case NEOS_SOPT_MCAST_IF:
        level = IPPROTO_IP; name = IP_MULTICAST_IF;
        data = &as_addr; size = sizeof(as_addr);
        break;
    default:
        return NEOS_SOCK_ERR;
    }

    return setsockopt(fd, level, name, data, size) == 0 ? NEOS_SOCK_OK
                                                        : NEOS_SOCK_ERR;
}

int neos_sock_join(int fd, uint32_t group, uint32_t iface_ip)
{
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = lwip_htonl(group);
    mreq.imr_interface.s_addr = lwip_htonl(iface_ip);
    return setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                      &mreq, sizeof(mreq)) == 0 ? NEOS_SOCK_OK : NEOS_SOCK_ERR;
}

/* ------------------------------------------------------------------ */
/* Where we are on it                                                  */
/* ------------------------------------------------------------------ */

/*
 * The interface NeOS is on.
 *
 * Asked for by key first, and only then by "whichever is default". The two
 * are usually the same handle and once were assumed to be, which cost an
 * afternoon: esp_netif_get_default_netif() picks by route priority among the
 * interfaces that are up, and on this machine the radio is a second chip on
 * the far side of SDIO whose netif is not always the one that answers. An app
 * that asked where it was and was told nowhere, while the system bar was
 * showing a Wi-Fi icon, is the shape that bug takes.
 *
 * WIFI_STA_DEF is the key esp_netif_create_default_wifi_sta() registers under,
 * which is the call neos_net.c makes.
 */
static esp_netif_t *wifi_netif(void)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return nif ? nif : esp_netif_get_default_netif();
}

bool neos_iface(neos_iface_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    esp_netif_t *nif = wifi_netif();
    if (!nif) {
        return false;
    }

    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(nif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    out->ip   = lwip_ntohl(info.ip.addr);
    out->mask = lwip_ntohl(info.netmask.addr);
    out->gw   = lwip_ntohl(info.gw.addr);

    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        out->dns = lwip_ntohl(dns.ip.u_addr.ip4.addr);
    }
    esp_netif_get_mac(nif, out->mac);
    return true;
}

/* ------------------------------------------------------------------ */
/* The neighbour cache                                                 */
/* ------------------------------------------------------------------ */

/*
 * The ARP table is lwIP's, and lwIP's core is not locked in this build
 * (CONFIG_LWIP_TCPIP_CORE_LOCKING is off), so it belongs to the tcpip task
 * and reading it from an app's task is a data race - the interesting kind,
 * where the entry being copied out is the one being recycled underneath.
 *
 * esp_netif_tcpip_exec() posts the work to that task and waits for it, which
 * is a context switch each way and is exactly right here: an app doing this
 * has just sent 254 packets and is asking who answered.
 */

typedef struct {
    neos_neigh_t *out;
    int           max;
    int           count;
} neigh_read_t;

static esp_err_t neigh_read(void *ctx)
{
    neigh_read_t *job = (neigh_read_t *)ctx;

    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t      *ip  = NULL;
        struct netif    *nif = NULL;
        struct eth_addr *eth = NULL;

        /* Returns 0 for an empty slot and for one still being asked about,
           which is what makes an absent entry mean "no answer yet". */
        if (!etharp_get_entry(i, &ip, &nif, &eth) || !ip || !eth) {
            continue;
        }
        if (job->out && job->count < job->max) {
            job->out[job->count].ip = lwip_ntohl(ip4_addr_get_u32(ip));
            memcpy(job->out[job->count].mac, eth->addr, 6);
            job->out[job->count].reserved[0] = 0;
            job->out[job->count].reserved[1] = 0;
        }
        job->count++;
    }
    return ESP_OK;
}

int neos_neigh_table(neos_neigh_t *out, int max)
{
    neigh_read_t job = { .out = out, .max = max > 0 ? max : 0, .count = 0 };
    if (esp_netif_tcpip_exec(neigh_read, &job) != ESP_OK) {
        return 0;
    }
    return job.count;
}

typedef struct {
    esp_netif_t *nif;
    uint32_t     ip;
    bool         asked;
} neigh_ask_t;

static esp_err_t neigh_ask(void *ctx)
{
    neigh_ask_t *job = (neigh_ask_t *)ctx;

    struct netif *nif = esp_netif_get_netif_impl(job->nif);
    if (!nif || !netif_is_up(nif)) {
        return ESP_OK;
    }

    ip4_addr_t addr;
    ip4_addr_set_u32(&addr, lwip_htonl(job->ip));

    /*
     * Off-segment is refused rather than attempted. An ARP request for an
     * address the router owns would be answered by nobody, and the app would
     * read that silence as "there is no such host" - which is a wrong answer
     * arrived at confidently, the sort worth not offering.
     */
    if (!ip4_addr_netcmp(&addr, netif_ip4_addr(nif), netif_ip4_netmask(nif))) {
        return ESP_OK;
    }

    job->asked = (etharp_request(nif, &addr) == ERR_OK);
    return ESP_OK;
}

bool neos_neigh_ask(uint32_t ip)
{
    neigh_ask_t job = {
        .nif   = wifi_netif(),
        .ip    = ip,
        .asked = false,
    };
    if (!job.nif) {
        return false;
    }
    if (esp_netif_tcpip_exec(neigh_ask, &job) != ESP_OK) {
        return false;
    }
    return job.asked;
}
