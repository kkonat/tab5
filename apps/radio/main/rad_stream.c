/*
 * One HTTP connection, ICY split out, audio into the ring.
 *
 * Over TLS when the URL says https, and that is nearly the whole of what the
 * distinction costs: two helpers at the top choose which send and which recv,
 * and everything below them - the phase machine, the response head, the
 * metadata demux, the ring - is the same code reading the same bytes. The
 * session is NeOS's, because mbedTLS and a certificate bundle are larger than
 * this entire app; see neos_sock.h.
 *
 * The Pi app let urllib do this and still opened the stream itself rather
 * than letting the decoder do it, for a reason that survives the port intact:
 * mpg123 handles HTTP fine but never surfaces the ICY StreamTitle on these
 * Shoutcast servers, so the metadata blocks have to come out of the byte
 * stream here, where something is looking for them. There is no urllib on
 * this tablet, so the HTTP is here too, which is about eighty lines and buys
 * the redirect following that NTS needs and the header that asks for the
 * metadata in the first place.
 *
 * Everything is a step of a state machine. An app runs on NeOS's own stack
 * and a blocking recv() is not a slow radio, it is a close button that has
 * stopped working - lanscan's lesson, and the same answer: each call does
 * whatever can be done without waiting and returns.
 *
 * What is deliberately *not* reset between attempts is the ring. The Pi
 * killed its decoder on every reconnect because the decoder was a process
 * holding a JACK port; here the buffer is three seconds of sound that has
 * already arrived, and a stumble that is over within that is a stumble
 * nobody hears. A splice mid-frame costs one frame - minimp3 hunts for the
 * next sync word - which is a far better trade than a gap.
 */

#include "radio.h"

#include <string.h>

/* Connecting over a marginal radio can legitimately take a while; playing
   cannot. Once the headers are in, several seconds of nothing is a drop and
   not a slow server, so the read gets a much shorter fuse than the dial. */
#define CONNECT_TIMEOUT  12000
#define READ_TIMEOUT      5000
#define MAX_REDIRECTS        4

#define USER_AGENT  "NeOS-radio/1"

const uint8_t rad_retry_delay_s[RAD_RETRIES] = { 1, 2, 4, 8, 15 };

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void drop_socket(rad_stream_t *s)
{
    if (s->fd >= 0) {
        neos_sock_close(s->fd);
    }
    s->fd = -1;

    if (s->tls > 0) {
        neos_tls_close(s->tls);     /* takes its socket with it */
    }
    s->tls = 0;
}

/*
 * The only two places the rest of this file has to care which kind of
 * connection it is holding. Everything else - the phase machine, the response
 * head, the ICY demux, the ring - reads the same either way, which is what
 * made adding TLS a small change rather than a second copy of the file.
 */
static int stream_send(rad_stream_t *s, const void *buf, int len)
{
    return s->secure ? neos_tls_send(s->tls, buf, len)
                     : neos_sock_send(s->fd, buf, len);
}

static int stream_recv(rad_stream_t *s, void *buf, int len)
{
    return s->secure ? neos_tls_recv(s->tls, buf, len)
                     : neos_sock_recv(s->fd, buf, len);
}

bool rad_stream_init(rad_stream_t *s)
{
    memset(s, 0, sizeof(*s));
    s->fd       = -1;
    s->station  = -1;
    s->icy_want = -1;
    return rad_ring_init(&s->ring, RAD_NET_RING);
}

void rad_stream_free(rad_stream_t *s)
{
    drop_socket(s);
    rad_ring_free(&s->ring);
}

void rad_stream_stop(rad_stream_t *s)
{
    drop_socket(s);
    s->state     = RAD_STOPPED;
    s->station   = -1;
    s->buffering = false;
    s->attempts  = 0;
    s->title[0]  = 0;
    s->icy_name[0] = 0;
    s->error[0]  = 0;
    s->bitrate_kbps = 0;
    s->rate_hz   = 0;
    rad_ring_reset(&s->ring);
}

/* Say why, and decide whether saying it is the end of it. */
static void fail(rad_stream_t *s, const char *why, bool fatal)
{
    char line[RAD_ERROR_MAX + 64];
    snprintf(line, sizeof(line), "radio: %s (%s)", why, fatal ? "fatal" : "retrying");
    neos_log(line);

    rad_copy(s->error, sizeof(s->error), why);
    drop_socket(s);
    s->buffering = false;

    if (fatal) {
        s->state = RAD_ERROR;
        return;
    }

    /*
     * Not an error page: the station is still selected and the app is still
     * trying. The backoff is quick at first because most drops here are a
     * momentary stumble and come straight back.
     */
    const int at = s->attempts < RAD_RETRIES - 1 ? s->attempts : RAD_RETRIES - 1;
    s->attempts++;
    s->title[0] = 0;
    s->retry_at = rad_now() + (uint32_t)rad_retry_delay_s[at] * 1000u;
    s->retry_in = rad_retry_delay_s[at];
    s->state    = RAD_RETRYING;
}

/* ------------------------------------------------------------------ */
/* Dialling                                                            */
/* ------------------------------------------------------------------ */

/* Take the current url apart and start looking the host up. Shared by a
   fresh play, a retry and a redirect, which differ only in what set the url
   and whether the ring was emptied first. */
static void begin(rad_stream_t *s)
{
    drop_socket(s);

    if (!rad_url_split(s->url, s->host, sizeof(s->host), &s->port,
                       s->path, sizeof(s->path), &s->secure)) {
        fail(s, "that is not a URL this app can dial", true);
        return;
    }

    s->hdr_len    = 0;
    s->req_len    = 0;
    s->req_sent   = 0;
    s->icy_want   = -1;
    s->icy_at     = 0;
    s->started_at = rad_now();
    s->state      = RAD_RESOLVING;
}

void rad_stream_play(rad_stream_t *s, const rad_station_t *st, int index)
{
    drop_socket(s);
    rad_ring_reset(&s->ring);

    rad_copy(s->url, sizeof(s->url), st->url);
    s->station      = index;
    s->attempts     = 0;
    s->redirects    = 0;
    s->title[0]     = 0;
    s->icy_name[0]  = 0;
    s->error[0]     = 0;
    s->bitrate_kbps = 0;
    s->rate_hz      = 0;
    s->buffering    = true;
    s->icy_interval = 0;
    s->icy_left     = 0;

    begin(s);
}

static void send_request(rad_stream_t *s)
{
    /*
     * Icy-MetaData is the whole reason the title band on the NOW page is ever
     * populated: without it a Shoutcast server sends audio and nothing else.
     * Connection: close is not a nicety either - a keep-alive on a stream
     * that never ends means nothing, and saying so keeps proxies from
     * chunking it.
     */
    const int n = snprintf(s->req, sizeof(s->req),
                           "GET %s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "User-Agent: " USER_AGENT "\r\n"
                           "Icy-MetaData: 1\r\n"
                           "Accept: */*\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           s->path, s->host);
    if (n <= 0 || n >= (int)sizeof(s->req)) {
        fail(s, "that URL is too long to ask for", true);
        return;
    }
    s->req_len  = n;
    s->req_sent = 0;
    s->phase    = PHASE_SEND;
}

/* ------------------------------------------------------------------ */
/* The response head                                                   */
/* ------------------------------------------------------------------ */

/* The value of one header, into @p out. The head has already been cut into
   NUL-terminated lines by the caller. */
static const char *header_value(const char *head, int len, const char *name)
{
    const char *p   = head;
    const char *end = head + len;

    while (p < end) {
        if (rad_starts(p, name)) {
            const char *v = p + strlen(name);
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            return v;
        }
        p += strlen(p) + 1;
    }
    return NULL;
}

static int header_int(const char *head, int len, const char *name, int fallback)
{
    const char *v = header_value(head, len, name);
    if (!v) {
        return fallback;
    }
    int value = 0;
    if (*v < '0' || *v > '9') {
        return fallback;
    }
    for (; *v >= '0' && *v <= '9'; v++) {
        value = value * 10 + (*v - '0');
    }
    return value;
}

/*
 * What arrived, and what to do about it. The head is turned into a run of
 * NUL-terminated lines in place first, which is what lets header_value walk
 * it without a second buffer.
 */
static void read_head(rad_stream_t *s, int body_at)
{
    char *head = s->hdr;
    const int len = body_at;

    /* HTTP/1.1 200 OK */
    int status = 0;
    {
        const char *p = head;
        while (*p && *p != ' ') {
            p++;
        }
        while (*p == ' ') {
            p++;
        }
        for (; *p >= '0' && *p <= '9'; p++) {
            status = status * 10 + (*p - '0');
        }
    }
    if (status == 0) {
        fail(s, "the server did not answer with HTTP", false);
        return;
    }

    /* Lines, in place. \r\n becomes \0\0, which leaves each header its own
       string and the walk above a simple one. */
    for (int i = 0; i + 1 < len; i++) {
        if (head[i] == '\r' && head[i + 1] == '\n') {
            head[i] = head[i + 1] = 0;
        }
    }

    if (status >= 300 && status < 400) {
        const char *loc = header_value(head, len, "location:");
        if (!loc || !*loc) {
            fail(s, "the server redirected without saying where", true);
            return;
        }
        if (++s->redirects > MAX_REDIRECTS) {
            fail(s, "too many redirects", true);
            return;
        }
        /* Relative targets are rare on these hosts and the two that redirect
           both send an absolute URL, so a Location without a scheme is
           treated as a path on the same host rather than parsed properly. */
        if (loc[0] == '/') {
            char next[RAD_URL_MAX];
            snprintf(next, sizeof(next), "http://%s:%u%s", s->host,
                     (unsigned)s->port, loc);
            rad_copy(s->url, sizeof(s->url), next);
        } else {
            rad_copy(s->url, sizeof(s->url), loc);
        }
        begin(s);
        return;
    }

    if (status < 200 || status >= 300) {
        char why[RAD_ERROR_MAX];
        snprintf(why, sizeof(why), "the server said %d", status);
        /*
         * 4xx means the server heard the request and refused it - a wrong
         * URL, a stream that has moved. Reconnecting changes nothing. 408 and
         * 429 are the exceptions: both are the server asking to be asked
         * again. Everything else, 5xx included, is weather.
         */
        const bool fatal = status >= 400 && status < 500 &&
                           status != 408 && status != 429;
        fail(s, why, fatal);
        return;
    }

    /*
     * The decoder is MP3 and only MP3, so a content type this app cannot
     * play is worth saying plainly rather than letting minimp3 hunt for a
     * sync word through an AAC stream forever. A server that names no type
     * is assumed to be sending MP3, which on this list is always true.
     */
    const char *type = header_value(head, len, "content-type:");
    if (type && !rad_starts(type, "audio/mpeg") && !rad_starts(type, "audio/mp3") &&
        !rad_starts(type, "application/octet-stream")) {
        char why[RAD_ERROR_MAX];
        snprintf(why, sizeof(why), "no decoder here for %s", type);
        fail(s, why, true);
        return;
    }

    const char *name = header_value(head, len, "icy-name:");
    if (name && *name) {
        rad_copy(s->icy_name, sizeof(s->icy_name), name);
    }

    /* 0 means the server is not sending metadata, and the demux below then
       degenerates to a straight copy - which is exactly right. */
    s->icy_interval = (uint32_t)header_int(head, len, "icy-metaint:", 0);
    s->icy_left     = s->icy_interval;
    s->icy_want     = -1;
    s->icy_at       = 0;

    {
        char line[160];
        snprintf(line, sizeof(line), "radio: playing, metaint %u, type %s",
                 (unsigned)s->icy_interval, type ? type : "(unstated)");
        neos_log(line);
    }
    s->state    = RAD_PLAYING;
    s->attempts = 0;            /* a connection that works clears the backoff */
    s->error[0] = 0;
    s->fed_at   = rad_now();
}

/* ------------------------------------------------------------------ */
/* ICY                                                                 */
/* ------------------------------------------------------------------ */

/* One metadata block: StreamTitle='...'; among whatever else is in there. */
static void take_title(rad_stream_t *s)
{
    s->icy_buf[s->icy_at < (int)sizeof(s->icy_buf) ? s->icy_at
                                                  : (int)sizeof(s->icy_buf) - 1] = 0;

    const char *at = rad_find(s->icy_buf, "StreamTitle='");
    if (!at) {
        return;
    }
    at += 13;

    /* Up to the closing quote-semicolon. A title with an apostrophe in it -
       and there are plenty - would end early on a bare quote, so the pair is
       what ends it. */
    char title[RAD_TITLE_MAX];
    int  n = 0;
    while (at[0] && n + 1 < (int)sizeof(title)) {
        if (at[0] == '\'' && at[1] == ';') {
            break;
        }
        title[n++] = *at++;
    }
    title[n] = 0;

    while (n > 0 && title[n - 1] == ' ') {
        title[--n] = 0;
    }
    rad_copy(s->title, sizeof(s->title), title);
}

/*
 * Split @p n bytes into audio and metadata. The interval counts audio bytes
 * only, so this is a small loop over three cases rather than a byte at a
 * time: a run of audio, the single length byte, and a run of metadata.
 */
static void demux(rad_stream_t *s, const uint8_t *data, int n)
{
    if (s->icy_interval == 0) {
        rad_ring_write(&s->ring, data, (uint32_t)n);
        return;
    }

    while (n > 0) {
        if (s->icy_left > 0) {
            uint32_t run = (uint32_t)n;
            if (run > s->icy_left) {
                run = s->icy_left;
            }
            const uint32_t took = rad_ring_write(&s->ring, data, run);
            /*
             * A ring with no room means the decoder is behind, which on this
             * device means the app is behind - so the bytes are dropped here
             * rather than left to back up into a stall that would be reported
             * as a network fault. The interval must still be advanced past
             * them or every subsequent metadata block lands in the audio.
             */
            data        += run;
            n           -= (int)run;
            s->icy_left -= run;
            (void)took;
            continue;
        }

        if (s->icy_want < 0) {
            s->icy_want = (int)data[0] * 16;
            s->icy_at   = 0;
            data++;
            n--;
            if (s->icy_want == 0) {
                s->icy_want = -1;           /* unchanged since last time */
                s->icy_left = s->icy_interval;
            }
            continue;
        }

        int run = n;
        if (run > s->icy_want) {
            run = s->icy_want;
        }
        if (s->icy_at + run < (int)sizeof(s->icy_buf)) {
            memcpy(s->icy_buf + s->icy_at, data, (size_t)run);
            s->icy_at += run;
        }
        data        += run;
        n           -= run;
        s->icy_want -= run;

        if (s->icy_want == 0) {
            take_title(s);
            s->icy_want = -1;
            s->icy_left = s->icy_interval;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The step                                                            */
/* ------------------------------------------------------------------ */

void rad_stream_poll(rad_stream_t *s)
{
    static uint8_t rx[4096];

    switch (s->state) {

    case RAD_STOPPED:
    case RAD_ERROR:
        return;

    case RAD_RETRYING: {
        const uint32_t now = rad_now();
        if ((int32_t)(s->retry_at - now) > 0) {
            /* Counted down visibly rather than frozen on a static message: on
               a screen with no other feedback, "retrying" that then does
               nothing for fifteen seconds looks like a hang. */
            s->retry_in = (int)((s->retry_at - now) / 1000u) + 1;
            return;
        }
        s->redirects = 0;
        begin(s);
        return;
    }

    case RAD_RESOLVING: {
        /*
         * An address first, and the test is "do we have one" rather than
         * "does neos_net_state() say ONLINE" - lanscan's distinction, and the
         * right one: the enum is a summary meant for a status bar, and an
         * address on the interface is the actual precondition for putting a
         * packet on the wire. The state is only used to word the wait.
         *
         * Without this the dial goes ahead anyway and fails with "could not
         * start the connection", which is true and useless. A tablet that has
         * just woken up, or is between associations, is in this state for a
         * second or two routinely.
         */
        neos_iface_t nif;
        if (!neos_iface(&nif) || nif.ip == 0) {
            const char *why;
            switch (neos_net_state()) {
            case NEOS_NET_ABSENT:     why = "no radio - this tablet cannot see a network"; break;
            case NEOS_NET_OFF:        why = "the radio is off - turn it on from the bar";  break;
            case NEOS_NET_CONNECTING: why = "joining a network";                           break;
            case NEOS_NET_ONLINE:     why = "joined, waiting for an address";              break;
            default:                  why = "no network - join one from the bar";          break;
            }
            fail(s, why, false);
            return;
        }

        /*
         * One call, asked again each turn until it answers. NeOS does the
         * lookup against the stack's own resolver and its cache, so a station
         * that was played a minute ago usually comes back on the first ask
         * with no packet sent - and this app no longer carries three hundred
         * lines of RFC 1035 to find that out for itself.
         */
        const int found = s->secure ? NEOS_SOCK_OK
                                    : neos_resolve(s->host, &s->host_ip);
        if (found == NEOS_SOCK_PENDING) {
            return;
        }
        if (found != NEOS_SOCK_OK) {
            /* Room for the whole hostname, even though rad_copy() will trim
               it to fit the field the card shows: a buffer sized to the
               field would make the compiler right about the truncation. */
            char why[RAD_ERROR_MAX + RAD_HOST_MAX];
            snprintf(why, sizeof(why), "cannot look up %s", s->host);
            fail(s, why, false);
            return;
        }

        if (s->secure) {
            /*
             * neos_tls_open() takes the name rather than the address,
             * because SNI and the certificate both need it - so the lookup
             * happens inside the session and this branch has nothing else to
             * do. From here the two paths rejoin: PHASE_CONNECT asks whether
             * the connection is up, and does not care what kind it is.
             */
            s->tls = neos_tls_open(s->host, s->port);
            if (s->tls <= 0) {
                fail(s, "no TLS session free", false);
                return;
            }
            s->started_at = rad_now();
            s->phase      = PHASE_CONNECT;
            s->state      = RAD_CONNECTING;
            return;
        }

        s->fd = neos_sock_open(NEOS_SOCK_TCP);
        if (s->fd < 0) {
            fail(s, "no socket to dial with", false);
            return;
        }

        {
            neos_iface_t nif;
            const bool have = neos_iface(&nif);
            char line[256];
            snprintf(line, sizeof(line),
                     "radio: dialling %u.%u.%u.%u:%u (%s) from %u.%u.%u.%u",
                     (unsigned)(s->host_ip >> 24) & 0xffu,
                     (unsigned)(s->host_ip >> 16) & 0xffu,
                     (unsigned)(s->host_ip >> 8) & 0xffu,
                     (unsigned)s->host_ip & 0xffu,
                     (unsigned)s->port, s->host,
                     (unsigned)(have ? (nif.ip >> 24) & 0xffu : 0),
                     (unsigned)(have ? (nif.ip >> 16) & 0xffu : 0),
                     (unsigned)(have ? (nif.ip >> 8) & 0xffu : 0),
                     (unsigned)(have ? nif.ip & 0xffu : 0));
            neos_log(line);
        }
        s->started_at = rad_now();
        s->phase      = PHASE_CONNECT;
        s->state      = RAD_CONNECTING;

        /*
         * Three answers, and all three have to be read. A connect that fails
         * on the spot - no route, no network, a port the stack will not dial -
         * was being let through to the poll below, which then waited the full
         * twelve seconds and reported it as a server that did not answer. The
         * message was wrong and the wait was wasted.
         */
        const int dial = neos_sock_connect(s->fd, s->host_ip, s->port);
        if (dial == NEOS_SOCK_OK) {
            send_request(s);
        } else if (dial != NEOS_SOCK_PENDING) {
            fail(s, "could not start the connection", false);
        }
        return;
    }

    case RAD_CONNECTING:
        if (rad_now() - s->started_at > CONNECT_TIMEOUT) {
            fail(s, "the server did not answer in time", false);
            return;
        }

        if (s->phase == PHASE_CONNECT && s->secure) {
            /* The handshake is driven by asking about it. */
            const int up = neos_tls_status(s->tls);
            if (up == NEOS_SOCK_PENDING) {
                return;
            }
            if (up != NEOS_SOCK_OK) {
                fail(s, "the secure connection could not be made", false);
                return;
            }
            send_request(s);
        }

        if (s->phase == PHASE_CONNECT) {
            /*
             * Writability first, and only then the status - which is the
             * contract neos_sock.h states and the thing to get wrong exactly
             * once. SO_ERROR is zero on a socket that is still in the
             * handshake as well as on one that has finished it, so asking
             * the status on its own reports every pending connect as a
             * completed one; the send that follows then lands on a socket
             * with no connection under it and comes back ENOTCONN, which
             * reads as a server that hung up on us.
             *
             * A zero timeout, because this is a poll and not a wait: the
             * loop has a stream to drain and a screen to draw.
             */
            uint8_t events = NEOS_SOCK_WRITE;
            const int ready = neos_sock_wait(&s->fd, &events, 1, 0);
            if (ready <= 0 || !(events & (NEOS_SOCK_WRITE | NEOS_SOCK_FAIL))) {
                return;         /* still dialling */
            }

            const int st = neos_sock_status(s->fd);
            if (st == NEOS_SOCK_PENDING) {
                return;
            }
            if (st != NEOS_SOCK_OK) {
                fail(s, "the connection was refused", false);
                return;
            }
            send_request(s);
        }

        if (s->phase == PHASE_SEND) {
            while (s->req_sent < s->req_len) {
                const int n = stream_send(s, s->req + s->req_sent,
                                          s->req_len - s->req_sent);
                if (n == NEOS_SOCK_AGAIN) {
                    return;
                }
                if (n <= 0) {
                    fail(s, "the connection went away mid-request", false);
                    return;
                }
                s->req_sent += n;
            }
            s->phase   = PHASE_HEAD;
            s->hdr_len = 0;
        }

        if (s->phase == PHASE_HEAD) {
            for (;;) {
                const int room = (int)sizeof(s->hdr) - s->hdr_len;
                if (room <= 0) {
                    fail(s, "the response head is longer than this app allows",
                         true);
                    return;
                }
                const int n = stream_recv(s, s->hdr + s->hdr_len, room);
                if (n == NEOS_SOCK_AGAIN) {
                    return;
                }
                if (n == 0) {
                    fail(s, "the server hung up before answering", false);
                    return;
                }
                if (n < 0) {
                    fail(s, "the connection failed", false);
                    return;
                }
                s->hdr_len += n;

                /*
                 * The blank line, and with it the start of the audio. What
                 * came in the same read goes straight into the ring - there
                 * is no separate "now start reading the body" step, because
                 * on a fast server the first few kilobytes of MP3 arrive in
                 * the same segment as the headers.
                 */
                for (int i = 3; i < s->hdr_len; i++) {
                    if (s->hdr[i - 3] == '\r' && s->hdr[i - 2] == '\n' &&
                        s->hdr[i - 1] == '\r' && s->hdr[i] == '\n') {
                        const int body_at = i + 1;
                        const int spill   = s->hdr_len - body_at;

                        static uint8_t keep[sizeof(s->hdr)];
                        if (spill > 0) {
                            memcpy(keep, s->hdr + body_at, (size_t)spill);
                        }
                        read_head(s, body_at);
                        if (s->state == RAD_PLAYING && spill > 0) {
                            demux(s, keep, spill);
                        }
                        return;
                    }
                }
            }
        }
        return;

    case RAD_PLAYING: {
        int budget = 8;     /* reads per turn: enough to keep up with a fast
                               server, few enough that the loop comes back */
        while (budget-- > 0) {
            uint32_t room = rad_ring_room(&s->ring);
            if (room == 0) {
                break;      /* the decoder is behind; let it catch up */
            }
            int want = (int)sizeof(rx);
            if ((uint32_t)want > room) {
                want = (int)room;
            }

            const int n = stream_recv(s, rx, want);
            if (n == NEOS_SOCK_AGAIN) {
                break;
            }
            if (n == 0) {
                /*
                 * The read ended without an error, which means the server
                 * closed the connection. For live radio that is a drop and
                 * not an end of file, so it is treated as one.
                 */
                fail(s, "the stream ended", false);
                return;
            }
            if (n < 0) {
                fail(s, "the connection dropped", false);
                return;
            }

            s->fed_at = rad_now();
            demux(s, rx, n);
        }

        if (rad_now() - s->fed_at > READ_TIMEOUT) {
            fail(s, "the stream stalled", false);
            return;
        }
        if (s->buffering && rad_ring_used(&s->ring) >= RAD_PREBUFFER) {
            s->buffering = false;
        }
        return;
    }
    }
}
