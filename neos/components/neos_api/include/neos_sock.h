/*
 * The wire.
 *
 * neos_net.h is about the connection - which network the tablet is on, and
 * who decides. This file is about what an app may put on it once NeOS has
 * one: a small, IPv4-only, always-non-blocking socket surface, plus the two
 * things below the transport that a program looking at a LAN cannot work
 * without - our own address and mask, and the neighbour cache.
 *
 * Why this is here at all, when neos_weather.h argues the opposite way about
 * HTTP: the weather is one fact about one place, so two apps fetching it
 * separately could disagree about it, and the fetch needs TLS and JSON that
 * nothing else in the ABI wants. Neither is true of a socket. A scan is an
 * app's own transient work - nothing else in the system has an opinion about
 * the result - and what it needs is the layer below all of that, which is
 * small enough to state completely on one screen. The line neos_net.h draws
 * is around association, and this does not cross it: an app can send a packet
 * over whatever network NeOS joined, and it still cannot decide which one
 * that is, or whether there is one.
 *
 * Addresses are uint32_t in host byte order, everywhere, in and out. There is
 * no sockaddr here on purpose: its layout would be compiled into every app on
 * the card, so it would be frozen for the life of the major ABI, and it
 * carries a family, a port and a pad that this API already knows the answer
 * to. Ports are host order too. An app that wants to print an address does
 * the four shifts itself, which is cheaper than a name to remember.
 *
 * Every socket from neos_sock_open() is non-blocking, and there is no way to
 * ask for a blocking one. An app runs on the boot task's own stack - the same
 * stack NeOS returns into - so a blocking recv() is not a slow app, it is a
 * tablet whose close button has stopped working. A call that would have
 * waited returns NEOS_SOCK_AGAIN instead, and neos_sock_wait() is how an app
 * sleeps until there is something to do.
 *
 * How many are available: lwIP's socket table is shared with the firmware,
 * which is holding a few of its own - SNTP, and an HTTP fetch whenever the
 * weather refreshes. NEOS_SOCK_BUDGET is what an app can count on having at
 * once. Asking for more is not a crash, neos_sock_open() returns
 * NEOS_SOCK_ERR, but an app that treats the budget as a suggestion gets its
 * failures at whichever moment the weather happened to refresh, which is the
 * worst kind of bug to be handed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Sockets                                                             */
/* ------------------------------------------------------------------ */

/** What kind of socket to open. */
#define NEOS_SOCK_TCP    1
#define NEOS_SOCK_UDP    2
/**
 * A raw ICMP socket: echo requests out, whole IP datagrams back.
 *
 * The one thing here that is not an ordinary socket call, and it is in the
 * ABI because the alternative was a neos_ping() that swept a subnet on the
 * app's behalf - which is a scanner in the firmware, with its pacing and its
 * timeouts decided by whoever wrote the firmware. What comes out of a read
 * starts at the IP header and not at the ICMP one, so the reply's TTL is
 * readable, and the TTL is half of what a free OS guess is made of. The
 * checksum on the way out is the caller's: a raw socket does not fill one in.
 */
#define NEOS_SOCK_ICMP   3

/**
 * How many sockets an app may hold at once. See the note at the top.
 *
 * A number and not a call, because it is a promise about the firmware's own
 * appetite rather than a reading: an app that asked "how many are free right
 * now" would get an answer that stopped being true between the question and
 * the open.
 */
#define NEOS_SOCK_BUDGET 16

/* Results. Every call here returns a count or one of these, never errno. */
#define NEOS_SOCK_OK        0    /**< done, or connected */
#define NEOS_SOCK_ERR      (-1)  /**< it failed, and will not un-fail */
#define NEOS_SOCK_AGAIN    (-2)  /**< nothing there yet; wait and ask again */
#define NEOS_SOCK_PENDING  (-3)  /**< a connect is under way */

/** Bits for neos_sock_wait(), in and out. */
#define NEOS_SOCK_READ   0x01u
#define NEOS_SOCK_WRITE  0x02u
#define NEOS_SOCK_FAIL   0x04u

/**
 * Open a socket. Returns a descriptor, or NEOS_SOCK_ERR.
 *
 * @param kind  NEOS_SOCK_TCP, NEOS_SOCK_UDP or NEOS_SOCK_ICMP.
 *
 * The descriptor is lwIP's, so it is a small integer, and it is valid until
 * neos_sock_close(). NeOS does not track which app owns which - an app that
 * returns from main() holding sockets has leaked them for the life of the
 * boot, so close what you open.
 */
int neos_sock_open(int kind);

/** Close a socket. Safe on one already closed, and on a negative descriptor. */
void neos_sock_close(int fd);

/**
 * Bind to a local address and port. 0 for either means "any".
 *
 * Needed for the protocols that are shouted at a well-known port rather than
 * sent to one: mDNS on 5353, and SSDP on 1900 if the unsolicited
 * announcements are wanted as well as the answers.
 */
int neos_sock_bind(int fd, uint32_t ip, uint16_t port);

/**
 * Start connecting. NEOS_SOCK_OK if it completed there and then - which does
 * happen for a host on the same segment that is quick to answer -
 * NEOS_SOCK_PENDING if it is under way, NEOS_SOCK_ERR if it was refused
 * outright.
 *
 * On a UDP socket this only fixes the peer, and always returns NEOS_SOCK_OK.
 *
 * A pending connect completes - either way - by becoming writable. Wait for
 * NEOS_SOCK_WRITE and then ask neos_sock_status(): a refused connection and a
 * completed one are both "writable", and the difference between them is the
 * whole answer a port scan came for.
 */
int neos_sock_connect(int fd, uint32_t ip, uint16_t port);

/**
 * How a pending connect turned out: NEOS_SOCK_OK, NEOS_SOCK_PENDING or
 * NEOS_SOCK_ERR.
 */
int neos_sock_status(int fd);

/** Send on a connected socket. Bytes written, or a negative result. */
int neos_sock_send(int fd, const void *buf, int len);

/** Receive. Bytes read, 0 at end of stream, or a negative result. */
int neos_sock_recv(int fd, void *buf, int len);

/** Send one datagram. */
int neos_sock_sendto(int fd, const void *buf, int len, uint32_t ip, uint16_t port);

/**
 * Receive one datagram, and who it came from. Either pointer may be NULL.
 *
 * On a NEOS_SOCK_ICMP socket what lands in @p buf starts at the IP header -
 * the address is in there twice over, and the TTL only once.
 */
int neos_sock_recvfrom(int fd, void *buf, int len, uint32_t *from_ip, uint16_t *from_port);

/**
 * Wait until one of @p n sockets is ready, or @p ms have gone by.
 *
 * @param fds     the descriptors to watch
 * @param events  in: what to watch each one for; out: what actually happened.
 *                One byte per descriptor, NEOS_SOCK_READ | WRITE | FAIL.
 * @return how many entries came back with something set, 0 on timeout, or
 *         NEOS_SOCK_ERR.
 *
 * Parallel arrays rather than a struct, for the reason sockaddr is missing
 * above: a struct here would be a layout frozen into every app on the card,
 * and this one has nothing in it that needs a name.
 *
 * @p ms of 0 polls, and anything over a second is treated as a second. That
 * ceiling is not a convenience: an app waiting in here is an app that is not
 * polling neos_app_close_requested(), and the close button belongs to whoever
 * is holding the tablet rather than to the app's idea of a reasonable wait.
 */
int neos_sock_wait(const int *fds, uint8_t *events, int n, uint32_t ms);

/* Options worth having, and no others. `value` is a flag, a hop count or an
   address, depending on which. */
#define NEOS_SOPT_BROADCAST   1  /**< let sendto() reach x.x.x.255 */
#define NEOS_SOPT_REUSE       2  /**< share a well-known port. Set before bind */
#define NEOS_SOPT_TTL         3  /**< unicast hop limit */
#define NEOS_SOPT_MCAST_TTL   4  /**< multicast hop limit; 1 keeps it on the LAN */
#define NEOS_SOPT_MCAST_IF    5  /**< value is the local address to send from */
#define NEOS_SOPT_MCAST_LOOP  6  /**< hear our own multicasts back */

int neos_sock_set(int fd, int option, uint32_t value);

/**
 * Join a multicast group on the interface holding @p iface_ip.
 *
 * Separate from neos_sock_set() because it takes two addresses where that one
 * takes a value, and folding them together would mean a struct - see the note
 * at the top about why there are none here that do not have to be.
 */
int neos_sock_join(int fd, uint32_t group, uint32_t iface_ip);

/* ------------------------------------------------------------------ */
/* Where we are on it                                                  */
/* ------------------------------------------------------------------ */

/**
 * The address, mask, gateway, resolver and MAC of the interface NeOS is on.
 *
 * neos_net_ip() gives the address as text, which is right for showing and
 * useless for arithmetic. The reason to ask for this one is the mask, and the
 * mask is the whole difference between "the network we are on" and "whatever
 * the user typed".
 */
typedef struct {
    uint32_t ip;           /**< host order, 0 when there is no address */
    uint32_t mask;
    uint32_t gw;           /**< 0 when the lease named none */
    uint32_t dns;          /**< the first resolver, 0 if there is none */
    uint8_t  mac[6];
    uint8_t  reserved[2];
} neos_iface_t;

/** False when there is no address yet, in which case @p out is zeroed. */
bool neos_iface(neos_iface_t *out);

/* ------------------------------------------------------------------ */
/* The neighbour cache                                                 */
/* ------------------------------------------------------------------ */

/*
 * A MAC address is the only identifier on a LAN that survives a DHCP lease
 * change, and the stack is already collecting them: anything the tablet sends
 * to an address on its own segment has to resolve that address first. So an
 * app that has just swept a subnet does not need to send an ARP packet of its
 * own to find out who is there - it needs to read what the sweep already put
 * in here.
 *
 * The table is small and it recycles, which is the one thing to know before
 * building on it. Sweeping 254 addresses makes far more entries than it
 * holds, so this is not a list to read once at the end: read it as you go,
 * keep what you find, and let the stack lose whichever entries it likes.
 *
 * Only resolved neighbours are listed. An address the stack is still asking
 * about occupies a slot and does not appear here, so an entry that is absent
 * means "not answered yet" and never "answered with nothing".
 */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  reserved[2];
} neos_neigh_t;

/**
 * Copy out the cache. Returns how many entries there are, which may exceed
 * @p max - the array fills to max and the count is still the truth, the same
 * convention as neos_net_scan_results(). NULL and 0 just counts.
 */
int neos_neigh_table(neos_neigh_t *out, int max);

/**
 * Ask the wire who holds @p ip, without sending it anything else.
 *
 * For the stragglers. A host that ignores ICMP and closes every port is still
 * obliged to answer an ARP request, so this is the last question there is to
 * ask about an address on our own segment. Nothing comes back through this
 * call - the answer turns up in the table above, a moment later. False if the
 * address is not on our segment, or there is no network.
 */
bool neos_neigh_ask(uint32_t ip);

/*
 * Both structs are filled into storage the app owns, so their layouts are
 * compiled into every app that reads one. Same rule as neos_net_ap_t:
 * appending a field is a minor, moving one is a major, and this is where you
 * find out.
 */
_Static_assert(sizeof(neos_iface_t) == 24, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_iface_t, ip)   ==  0, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_iface_t, mask) ==  4, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_iface_t, gw)   ==  8, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_iface_t, dns)  == 12, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_iface_t, mac)  == 16, "neos_iface_t layout is frozen for ABI v1");
_Static_assert(sizeof(neos_neigh_t) == 12, "neos_neigh_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_neigh_t, ip)  == 0, "neos_neigh_t layout is frozen for ABI v1");
_Static_assert(offsetof(neos_neigh_t, mac) == 4, "neos_neigh_t layout is frozen for ABI v1");

#ifdef __cplusplus
}
#endif
