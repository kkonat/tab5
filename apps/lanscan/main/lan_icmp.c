/*
 * The echo sweep.
 *
 * One raw socket, an echo request to every address in the subnet, and
 * whatever comes back. It is the cheapest question there is - one packet out,
 * one in - and it produces two things at once: an RTT, and the TTL the reply
 * arrived with, which is a free and coarse guess at the operating system
 * because the initial values are conventional (255 network gear, 128 Windows,
 * 64 everything else).
 *
 * It is also, on this machine, the discovery stage for hosts that never
 * answer it. The tablet is on the segment it is sweeping, so every one of
 * these packets has to have its destination resolved before it can leave, and
 * a host that drops ICMP still has to answer the ARP request or it would not
 * be reachable at all. So the sweep populates the neighbour cache for the
 * whole subnet whether or not anything replies, and lan_engine.c reads that
 * cache as it goes. The desktop version knocks on 21 ports across every
 * address to cover the same ground; it does not have the luxury of being one
 * hop away from everything it is asking about.
 *
 * The sending is paced. 254 packets handed to the stack in one go would be
 * 254 ARP entries wanted at once against a table of 64, and the entries that
 * got recycled before anyone read them would be devices that silently did not
 * exist. A dozen per tick is about 400 a second, which finishes a /24 inside a
 * second and never has more outstanding than the table can hold.
 */

#include "lanscan.h"

#define ICMP_ECHO_REQUEST  8
#define ICMP_ECHO_REPLY    0

/* Ours, so a reply meant for something else on the tablet is not counted as
   an answer to a question this app asked. */
#define ICMP_ID  0x4E45u    /* 'NE' */

/*
 * Sending is paced against the clock, not against the loop.
 *
 * The loop has no frame rate: during a sweep there is nothing in it that
 * waits, so it turns over as fast as the core allows. A batch "per tick" was
 * therefore not a rate, and the whole subnet went out in a few milliseconds -
 * which overruns lwIP's ARP queue, makes sendto() fail with ENOMEM, and turns
 * the addresses that failed into hosts the scan never heard of.
 *
 * 12 every 20 ms is 600 a second, which finishes a /24 in under half a second
 * and never has more than a dozen resolutions outstanding.
 */
#define SWEEP_BATCH      12
#define SWEEP_EVERY_MS   20
#define SWEEP_RETRIES     3    /* before giving up on one address */
#define SWEEP_LINGER_MS 1400   /* how long to keep listening after the last send */

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t sum;
    uint16_t id;
    uint16_t seq;
    uint8_t  payload[8];
} __attribute__((packed)) echo_t;

bool lan_icmp_start(lan_scan_t *s)
{
    lan_icmp_stop(s);

    s->icmp_fd = neos_sock_open(NEOS_SOCK_ICMP);
    if (s->icmp_fd < 0) {
        return false;
    }
    /* One hop. Everything this asks about is on our own segment by
       definition, and a sweep that leaked into the wider network would be a
       scan of somebody else's. */
    neos_sock_set(s->icmp_fd, NEOS_SOPT_TTL, 1);

    s->cursor        = 0;
    s->sent          = 0;
    s->hits          = 0;
    s->retry         = 0;
    s->deadline      = 0;
    s->sweep_next_ms = lan_now();
    memset(s->icmp_sent_at, 0, sizeof(s->icmp_sent_at));
    return true;
}

void lan_icmp_stop(lan_scan_t *s)
{
    if (s->icmp_fd >= 0) {
        neos_sock_close(s->icmp_fd);
        s->icmp_fd = -1;
    }
}

static void send_one(lan_scan_t *s, int index)
{
    echo_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = ICMP_ECHO_REQUEST;
    pkt.id   = (uint16_t)((ICMP_ID >> 8) | (ICMP_ID << 8));   /* network order */
    pkt.seq  = (uint16_t)((index >> 8) | ((index & 0xFF) << 8));
    memcpy(pkt.payload, "neos-lan", 8);
    /* A raw socket does not fill this in; the kernel is not involved above
       the IP header. */
    pkt.sum = lan_checksum(&pkt, (int)sizeof(pkt));

    const uint32_t ip = s->net_base + (uint32_t)index;
    s->icmp_sent_at[index] = lan_now();

    const int r = neos_sock_sendto(s->icmp_fd, &pkt, (int)sizeof(pkt), ip, 0);
    if (r < 0) {
        /*
         * Out of buffers - on a burst like this, the ARP queue. Hold the
         * address and come back to it rather than counting it as probed: an
         * address skipped because we were busy is a device reported as absent.
         *
         * Not forever, though. An address the stack simply will not accept a
         * packet for would otherwise stall the sweep on its own, so after a
         * few goes it is left behind. That costs this host its RTT and TTL and
         * nothing else - the ARP sweep in lan_engine.c is what decides whether
         * it exists.
         */
        if (++s->retry < SWEEP_RETRIES) {
            return;
        }
    }
    s->retry = 0;
    s->cursor++;
    s->sent++;
}

static void drain(lan_scan_t *s)
{
    uint8_t  buf[128];
    uint32_t from = 0;

    for (int guard = 0; guard < 32; guard++) {
        const int n = neos_sock_recvfrom(s->icmp_fd, buf, (int)sizeof(buf),
                                         &from, NULL);
        if (n <= 0) {
            return;
        }
        /* What arrives on a raw socket starts at the IP header. */
        if (n < 20 || (buf[0] >> 4) != 4) {
            continue;
        }
        const int ihl = (buf[0] & 0x0F) * 4;
        if (ihl < 20 || n < ihl + 8) {
            continue;
        }
        const uint8_t ttl  = buf[8];
        const uint8_t *icmp = buf + ihl;
        if (icmp[0] != ICMP_ECHO_REPLY) {
            continue;              /* unreachable, time exceeded, our own echo */
        }

        const uint16_t id  = (uint16_t)((icmp[4] << 8) | icmp[5]);
        const uint16_t seq = (uint16_t)((icmp[6] << 8) | icmp[7]);
        if (id != ICMP_ID || seq >= (uint16_t)s->net_hosts) {
            continue;
        }

        uint32_t rtt = lan_now() - s->icmp_sent_at[seq];
        if (rtt > 9999) {
            rtt = 9999;
        }
        /* A reply that arrived inside the same millisecond still took some
           time, and a zero here means "unmeasured" everywhere else. */
        lan_set_alive(&s->reg, from, ttl, (uint16_t)(rtt ? rtt : 1));
        s->hits++;
    }
}

void lan_icmp_poll(lan_scan_t *s)
{
    if (s->icmp_fd < 0) {
        return;
    }

    drain(s);

    if (s->cursor >= s->net_hosts) {
        return;
    }
    if ((int32_t)(lan_now() - s->sweep_next_ms) < 0) {
        return;                         /* not time for the next batch yet */
    }
    s->sweep_next_ms = lan_now() + SWEEP_EVERY_MS;

    const int stop = s->cursor + SWEEP_BATCH;
    while (s->cursor < s->net_hosts && s->cursor < stop) {
        const int before = s->cursor;
        send_one(s, s->cursor);
        if (s->cursor == before) {
            break;                      /* held; the next batch retries it */
        }
    }
    if (s->cursor >= s->net_hosts) {
        s->deadline = lan_now() + SWEEP_LINGER_MS;
    }
}

bool lan_icmp_done(lan_scan_t *s)
{
    if (s->icmp_fd < 0) {
        return true;
    }
    return s->cursor >= s->net_hosts && s->deadline &&
           (int32_t)(lan_now() - s->deadline) >= 0;
}
