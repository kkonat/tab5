/*
 * One pool of half-open TCP connections, shared by every stage that speaks it.
 *
 * The desktop version runs a selector loop with three hundred sockets in
 * flight. Here the ceiling is NEOS_SOCK_BUDGET for the whole app, of which
 * three are already held by the mDNS and SSDP listeners for the life of the
 * scan, so this pool gets eight and the scan is shaped around that rather than
 * pretending otherwise: the knock runs against confirmed hosts instead of the
 * whole subnet, which is the change that makes eight enough.
 *
 * A knock and a banner grab are the same machine. Connect without blocking,
 * wait to become writable, ask whether that meant connected or refused, and
 * then - for the jobs that want to hear something - send a request and read
 * until the peer stops or the buffer is full. So there is one implementation
 * and a `want` of zero is what makes a job a knock.
 *
 * The one subtlety worth stating: a refused connection and a completed one
 * both become writable, and telling them apart is the whole answer a port
 * scan came for. That is what neos_sock_status() is asked, and it is asked
 * only after the wait says writable - asking earlier gets NEOS_SOCK_OK from a
 * connect that has not happened yet, which would report every port on the
 * network as open.
 */

#include "lanscan.h"

typedef enum {
    SLOT_FREE = 0,
    SLOT_CONNECTING,
    SLOT_SENDING,
    SLOT_READING,
} slot_state_t;

typedef struct {
    slot_state_t state;
    int          fd;
    uint32_t     ip;
    uint16_t     port;
    uint8_t      tag;
    uint32_t     deadline;

    const uint8_t *req;
    int            req_len;
    int            req_sent;

    int  want;
    int  got;
    char buf[LAN_TCP_BUF];
} slot_t;

static slot_t          s_slot[LAN_TCP_SLOTS];
static lan_tcp_done_fn s_done;
static void           *s_ctx;
static uint32_t        s_timeout_ms = 700;

/*
 * The request bodies handed to lan_tcp_push() are string literals and small
 * composed buffers owned by the caller's stack, so they are copied here
 * rather than pointed at. Sixteen slots' worth of 200-byte requests is
 * cheaper than the rule "keep your request alive until the callback", which
 * is the kind of rule that holds until the one caller that forgets.
 */
#define REQ_MAX 200
static uint8_t s_req[LAN_TCP_SLOTS][REQ_MAX];

static void slot_finish(slot_t *sl, bool open)
{
    if (sl->fd >= 0) {
        neos_sock_close(sl->fd);
        sl->fd = -1;
    }
    if (s_done) {
        sl->buf[sl->got < LAN_TCP_BUF ? sl->got : LAN_TCP_BUF - 1] = 0;
        s_done(sl->ip, sl->port, sl->tag, open, sl->buf, sl->got, s_ctx);
    }
    sl->state = SLOT_FREE;
    sl->got   = 0;
}

void lan_tcp_begin(lan_tcp_done_fn on_done, void *ctx, uint32_t timeout_ms)
{
    lan_tcp_end();
    s_done       = on_done;
    s_ctx        = ctx;
    s_timeout_ms = timeout_ms ? timeout_ms : 700;
}

void lan_tcp_end(void)
{
    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        if (s_slot[i].fd >= 0) {
            neos_sock_close(s_slot[i].fd);
        }
        s_slot[i].fd    = -1;
        s_slot[i].state = SLOT_FREE;
        s_slot[i].got   = 0;
    }
    s_done = NULL;
    s_ctx  = NULL;
}

bool lan_tcp_room(void)
{
    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        if (s_slot[i].state == SLOT_FREE) {
            return true;
        }
    }
    return false;
}

bool lan_tcp_idle(void)
{
    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        if (s_slot[i].state != SLOT_FREE) {
            return false;
        }
    }
    return true;
}

bool lan_tcp_push(uint32_t ip, uint16_t port,
                  const void *req, int req_len, int want, uint8_t tag)
{
    int at = -1;
    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        if (s_slot[i].state == SLOT_FREE) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        return false;
    }

    slot_t *sl = &s_slot[at];
    memset(sl, 0, sizeof(*sl));
    sl->fd = neos_sock_open(NEOS_SOCK_TCP);
    if (sl->fd < 0) {
        sl->state = SLOT_FREE;
        return false;
    }

    sl->ip       = ip;
    sl->port     = port;
    sl->tag      = tag;
    sl->want     = want > LAN_TCP_BUF - 1 ? LAN_TCP_BUF - 1 : want;
    sl->deadline = lan_now() + s_timeout_ms;

    if (req && req_len > 0) {
        sl->req_len = req_len > REQ_MAX ? REQ_MAX : req_len;
        memcpy(s_req[at], req, (size_t)sl->req_len);
        sl->req = s_req[at];
    }

    const int r = neos_sock_connect(sl->fd, ip, port);
    if (r == NEOS_SOCK_ERR) {
        /* Refused before the call even returned - a closed port on a host
           that is right here. Reported now rather than after a select. */
        slot_finish(sl, false);
        return true;
    }
    sl->state = SLOT_CONNECTING;
    return true;
}

/* Once connected, a job with nothing to say and nothing to hear is done. */
static void slot_connected(slot_t *sl)
{
    if (sl->want == 0 && sl->req_len == 0) {
        slot_finish(sl, true);
        return;
    }
    sl->state = sl->req_len ? SLOT_SENDING : SLOT_READING;
}

static void slot_send(slot_t *sl)
{
    while (sl->req_sent < sl->req_len) {
        const int n = neos_sock_send(sl->fd, sl->req + sl->req_sent,
                                     sl->req_len - sl->req_sent);
        if (n == NEOS_SOCK_AGAIN) {
            return;
        }
        if (n < 0) {
            /* It connected, so the port is open; we just never got to ask.
               That is still the answer the scan wanted. */
            slot_finish(sl, true);
            return;
        }
        sl->req_sent += n;
    }
    if (sl->want == 0) {
        slot_finish(sl, true);
        return;
    }
    sl->state = SLOT_READING;
}

static void slot_read(slot_t *sl)
{
    while (sl->got < sl->want) {
        const int n = neos_sock_recv(sl->fd, sl->buf + sl->got,
                                     sl->want - sl->got);
        if (n == NEOS_SOCK_AGAIN) {
            return;
        }
        if (n <= 0) {            /* end of stream, or the peer hung up */
            slot_finish(sl, true);
            return;
        }
        sl->got += n;
    }
    slot_finish(sl, true);
}

void lan_tcp_tick(uint32_t ms)
{
    int     fds[LAN_TCP_SLOTS];
    uint8_t ev[LAN_TCP_SLOTS];
    int     n = 0;
    int     map[LAN_TCP_SLOTS];

    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        slot_t *sl = &s_slot[i];
        if (sl->state == SLOT_FREE) {
            continue;
        }
        map[n]  = i;
        fds[n]  = sl->fd;
        /* A connecting or sending socket is waited on for writability, a
           reading one for something to read. Both also want the error bit,
           which neos_sock_wait() sets regardless of what was asked. */
        ev[n]   = (sl->state == SLOT_READING) ? NEOS_SOCK_READ : NEOS_SOCK_WRITE;
        n++;
    }

    if (n > 0) {
        neos_sock_wait(fds, ev, n, ms);
        for (int k = 0; k < n; k++) {
            slot_t *sl = &s_slot[map[k]];
            if (sl->state == SLOT_FREE || ev[k] == 0) {
                continue;
            }
            if (ev[k] & NEOS_SOCK_FAIL) {
                slot_finish(sl, false);
                continue;
            }
            switch (sl->state) {
            case SLOT_CONNECTING:
                if (neos_sock_status(sl->fd) == NEOS_SOCK_OK) {
                    slot_connected(sl);
                    /* Fall straight into sending: the socket is writable
                       right now, and waiting a whole tick to use that would
                       double the cost of every HTTP probe. */
                    if (sl->state == SLOT_SENDING) {
                        slot_send(sl);
                    }
                } else {
                    slot_finish(sl, false);
                }
                break;
            case SLOT_SENDING:
                slot_send(sl);
                break;
            case SLOT_READING:
                slot_read(sl);
                break;
            default:
                break;
            }
        }
    } else if (ms) {
        /* Nothing in flight but the caller asked to wait. Sleeping is the
           honest thing to do with the time - it is what keeps a stage that is
           between batches from spinning the core at 300 Hz. */
        neos_sleep_ms(ms);
    }

    /*
     * Reap anything past its deadline. A filtered port is a socket that never
     * becomes anything: no reply, no reset, no error, and the only thing that
     * ends it is the clock.
     */
    const uint32_t now = lan_now();
    for (int i = 0; i < LAN_TCP_SLOTS; i++) {
        slot_t *sl = &s_slot[i];
        if (sl->state == SLOT_FREE) {
            continue;
        }
        if ((int32_t)(now - sl->deadline) >= 0) {
            /* Reading is the one state where a deadline is not a failure: the
               connection is open and the peer has simply said all it intends
               to. Whatever arrived is the banner. */
            slot_finish(sl, sl->state == SLOT_READING);
        }
    }
}
