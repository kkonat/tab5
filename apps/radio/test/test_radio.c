/*
 * Host tests for the parts of the radio app that are not the panel.
 *
 * The three modules with private state machines are #included rather than
 * linked, so the tests can reach the static functions that are the actual
* subject - the ICY demux, the response-head parser, the resampler. Those
 * files are therefore NOT compiled separately in the build line.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "radio.h"
#include "rad_math.h"
#include "rad_ui.h"

#include "rad_stream.c"
#include "rad_audio.c"

void stub_set_now(uint32_t ms);
void stub_advance(uint32_t ms);
void stub_put_file(const char *rel, const char *text);
const char *stub_get_file(const char *rel);
void stub_clear_files(void);

static int g_fail;
static int g_checks;

#define CHECK(cond, ...) do {                                       \
    g_checks++;                                                     \
    if (!(cond)) {                                                  \
        g_fail++;                                                   \
        printf("  FAIL %s:%d  ", __func__, __LINE__);               \
        printf(__VA_ARGS__);                                        \
        printf("\n");                                               \
    }                                                               \
} while (0)

#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol),      \
        "%s = %.9g, wanted %.9g (tol %g)", #a, (double)(a), (double)(b), (double)(tol))

/* ================================================================== */
/* rad_math                                                           */
/* ================================================================== */
/* The ring                                                           */
/* ================================================================== */

static void test_ring(void)
{
    rad_ring_t r;
    CHECK(rad_ring_init(&r, 16), "ring allocates");

    uint8_t in[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    uint8_t out[32];

    CHECK(rad_ring_used(&r) == 0, "starts empty");
    CHECK(rad_ring_room(&r) == 15, "one slot is kept back, room = %u",
          rad_ring_room(&r));

    CHECK(rad_ring_write(&r, in, 10) == 10, "writes 10");
    CHECK(rad_ring_used(&r) == 10, "holds 10");
    CHECK(rad_ring_read(&r, out, 4) == 4, "reads 4");
    CHECK(memcmp(out, in, 4) == 0, "in order");

    /* Now wrap: 6 left, 9 room, write 9 and read it all back. */
    CHECK(rad_ring_write(&r, in, 9) == 9, "writes across the wrap");
    CHECK(rad_ring_used(&r) == 15, "full");
    CHECK(rad_ring_write(&r, in, 1) == 0, "a full ring takes nothing");

    CHECK(rad_ring_read(&r, out, 32) == 15, "drains");
    CHECK(memcmp(out, in + 4, 6) == 0, "the older bytes first");
    CHECK(memcmp(out + 6, in, 9) == 0, "then the newer ones, unwrapped");

    /* peek gives a contiguous run and never more than there is */
    rad_ring_reset(&r);
    rad_ring_write(&r, in, 10);
    const uint8_t *at = NULL;
    uint32_t run = rad_ring_peek(&r, &at);
    CHECK(run == 10 && at[0] == 0, "peek sees the whole run");
    rad_ring_skip(&r, 3);
    run = rad_ring_peek(&r, &at);
    CHECK(run == 7 && at[0] == 3, "skip advances the tail");
    rad_ring_skip(&r, 999);
    CHECK(rad_ring_used(&r) == 0, "over-skipping empties rather than wraps");

    rad_ring_free(&r);
}

/* ================================================================== */
/* Parameters                                                         */
/* ================================================================== */

static void test_params(void)
{
    char buf[32];

    rad_format_param(P_MID_FREQ, 1000, buf, sizeof(buf));
    CHECK(strcmp(buf, "1000Hz") == 0, "freq formats as '%s'", buf);

    rad_format_param(P_MID_GAIN, 35, buf, sizeof(buf));
    CHECK(strcmp(buf, "+3.5dB") == 0, "a boost carries its sign: '%s'", buf);
    rad_format_param(P_MID_GAIN, -35, buf, sizeof(buf));
    CHECK(strcmp(buf, "-3.5dB") == 0, "a cut too: '%s'", buf);
    rad_format_param(P_MID_GAIN, 0, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.0dB") == 0, "zero has no sign: '%s'", buf);
    rad_format_param(P_MID_GAIN, -4, buf, sizeof(buf));
    CHECK(strcmp(buf, "-0.4dB") == 0, "a negative under one unit: '%s'", buf);

    rad_format_param(P_MID_RQ, 100, buf, sizeof(buf));
    CHECK(strcmp(buf, "1.00") == 0, "q keeps two places: '%s'", buf);
    rad_format_param(P_MID_RQ, 70, buf, sizeof(buf));
    CHECK(strcmp(buf, "0.70") == 0, "and pads them: '%s'", buf);

    CHECK(rad_clamp_param(P_MID_FREQ, 1) == 150, "clamps up to the floor");
    CHECK(rad_clamp_param(P_MID_FREQ, 99999) == 8000, "and down to the ceiling");

    /* A frequency stepper must always move, and must reach both ends. */
    int v = rad_ranges[P_HPF_FREQ].def;
    for (int i = 0; i < 200; i++) {
        const int next = rad_step_param(P_HPF_FREQ, v, false);
        CHECK(next < v || next == rad_ranges[P_HPF_FREQ].lo,
              "stepping down always moves: %d -> %d", v, next);
        v = next;
    }
    CHECK(v == rad_ranges[P_HPF_FREQ].lo, "reaches the bottom, got %d", v);
    for (int i = 0; i < 200; i++) {
        const int next = rad_step_param(P_HPF_FREQ, v, true);
        CHECK(next > v || next == rad_ranges[P_HPF_FREQ].hi,
              "stepping up always moves: %d -> %d", v, next);
        v = next;
    }
    CHECK(v == rad_ranges[P_HPF_FREQ].hi, "reaches the top, got %d", v);

    /* A sixth of an octave: six steps up should double it. */
    v = 1000;
    for (int i = 0; i < 6; i++) {
        v = rad_step_param(P_MID_FREQ, v, true);
    }
    CHECK(v >= 1980 && v <= 2020, "six steps is an octave: %d", v);

    /* The gains step by 0.5 dB and stop at the rails. */
    v = 0;
    for (int i = 0; i < 40; i++) {
        v = rad_step_param(P_MID_GAIN, v, true);
    }
    CHECK(v == 150, "gain tops out at +15.0 dB, got %d", v);
}

/* ================================================================== */
/* URLs and stations                                                  */
/* ================================================================== */

static void test_urls(void)
{
    char host[RAD_HOST_MAX], path[RAD_PATH_MAX];
    uint16_t port;
    bool secure;

    CHECK(rad_url_split("http://149.56.155.73:8052", host, sizeof(host), &port,
                        path, sizeof(path), &secure), "plain host and port");
    CHECK(strcmp(host, "149.56.155.73") == 0, "host '%s'", host);
    CHECK(port == 8052, "port %u", port);
    CHECK(strcmp(path, "/") == 0, "an empty path is '/': '%s'", path);
    CHECK(!secure, "http is not secure");

    CHECK(rad_url_split("https://kexp.streamguys1.com/kexp160.aac", host,
                        sizeof(host), &port, path, sizeof(path), &secure),
          "https parses");
    CHECK(port == 443, "https defaults to 443, got %u", port);
    CHECK(secure, "and is flagged");
    CHECK(strcmp(path, "/kexp160.aac") == 0, "path '%s'", path);

    CHECK(rad_url_split("http://stream-relay-geo.ntslive.net/stream?client=direct",
                        host, sizeof(host), &port, path, sizeof(path), &secure),
          "a query string parses");
    CHECK(strcmp(path, "/stream?client=direct") == 0,
          "the query stays on the path: '%s'", path);
    CHECK(port == 80, "default port %u", port);

    CHECK(rad_url_split("http://user:pass@host.example/x", host, sizeof(host),
                        &port, path, sizeof(path), &secure), "userinfo parses");
    CHECK(strcmp(host, "host.example") == 0, "userinfo is skipped: '%s'", host);

    CHECK(!rad_url_split("rtsp://example/x", host, sizeof(host), &port, path,
                         sizeof(path), &secure), "an unknown scheme is refused");
    CHECK(!rad_url_split("just a name", host, sizeof(host), &port, path,
                         sizeof(path), &secure), "so is nonsense");
    CHECK(!rad_url_split("http://:80/x", host, sizeof(host), &port, path,
                         sizeof(path), &secure), "so is an empty host");
}

static void test_stations(void)
{
    stub_clear_files();
    stub_put_file("apps/radio/stations.conf",
        "# a comment\n"
        "\n"
        "The Penthouse | http://149.56.155.73:8052 | auto\n"
        "  Jazz Sakura  |  https://kathy.torontocast.com:3330  \n"
        "broken line with no bar\n"
        "| http://nameless.example/s\n"
        "Le Son Parisien | http://stream.lesonparisien.com:80/hi | nothing\n");

    rad_station_t st[RAD_STATIONS_MAX];
    const int n = rad_stations_load(st, RAD_STATIONS_MAX);
    CHECK(n == 4, "four usable lines, got %d", n);

    CHECK(strcmp(st[0].name, "The Penthouse") == 0, "name '%s'", st[0].name);
    CHECK(st[0].autoplay, "the auto flag is read");
    CHECK(!st[0].secure, "http is not secure");

    CHECK(strcmp(st[1].name, "Jazz Sakura") == 0, "trimmed: '%s'", st[1].name);
    CHECK(st[1].secure, "https is flagged");
    CHECK(!st[1].autoplay, "no flags means no auto");

    CHECK(strcmp(st[2].name, "http://nameless.example/s") == 0,
          "an empty name falls back to the url: '%s'", st[2].name);
    CHECK(!st[3].autoplay, "an unknown flag is not auto");

    /* Round trip: save, load, and the stations survive. */
    st[0].autoplay = false;
    st[3].autoplay = true;
    CHECK(rad_stations_save(st, n), "saves");

    rad_station_t back[RAD_STATIONS_MAX];
    const int m = rad_stations_load(back, RAD_STATIONS_MAX);
    CHECK(m == n, "the same count comes back: %d", m);
    for (int i = 0; i < m && i < n; i++) {
        CHECK(strcmp(back[i].name, st[i].name) == 0, "name %d survives", i);
        CHECK(strcmp(back[i].url, st[i].url) == 0, "url %d survives", i);
        CHECK(back[i].autoplay == st[i].autoplay, "star %d survives", i);
        CHECK(back[i].secure == st[i].secure, "https %d survives", i);
    }

    /* No file at all is the ordinary first run, not an error. */
    stub_clear_files();
    CHECK(rad_stations_load(st, RAD_STATIONS_MAX) == 0, "a missing file is empty");
}

static void test_settings(void)
{
    rad_app_t *app = (rad_app_t *)calloc(1, sizeof(rad_app_t));

    stub_clear_files();
    rad_settings_load(app);
    CHECK(app->volume == 25, "the default volume, got %d", app->volume);
    CHECK(app->params[P_MID_FREQ] == 1000, "a default parameter");

    app->volume = 63;
    app->params[P_MID_GAIN]  = -45;
    app->params[P_HPF_FREQ]  = 220;
    rad_settings_save(app);

    rad_app_t *again = (rad_app_t *)calloc(1, sizeof(rad_app_t));
    rad_settings_load(again);
    CHECK(again->volume == 63, "volume survives: %d", again->volume);
    CHECK(again->params[P_MID_GAIN] == -45, "a negative gain survives: %d",
          again->params[P_MID_GAIN]);
    CHECK(again->params[P_HPF_FREQ] == 220, "a frequency survives");

    /* Out-of-range values in a hand-edited file are clamped, not obeyed. */
    stub_put_file("apps/radio/settings.conf",
                  "volume = 900\nmidGain = 9999\nnonsense = 4\n");
    rad_settings_load(again);
    CHECK(again->volume == 100, "volume clamps: %d", again->volume);
    CHECK(again->params[P_MID_GAIN] == 150, "gain clamps: %d",
          again->params[P_MID_GAIN]);

    free(app);
    free(again);
}

/* ================================================================== */
/* DNS                                                                */
/* ================================================================== */
/* HTTP and ICY                                                       */
/* ================================================================== */

/* Put a whole response head into the stream and parse it, the way
   rad_stream_poll does once it has found the blank line. */
static void feed_head(rad_stream_t *s, const char *head)
{
    const int len = (int)strlen(head);
    memcpy(s->hdr, head, (size_t)len);
    s->hdr_len = len;
    read_head(s, len);
}

static void test_http(void)
{
    rad_stream_t *s = (rad_stream_t *)calloc(1, sizeof(rad_stream_t));
    CHECK(rad_stream_init(s), "stream allocates");

    /* --- an ordinary Shoutcast answer --- */
    rad_copy(s->url, sizeof(s->url), "http://a.example/stream");
    feed_head(s,
        "ICY 200 OK\r\n"
        "icy-name:The Penthouse\r\n"
        "icy-metaint:16000\r\n"
        "content-type:audio/mpeg\r\n"
        "\r\n");
    CHECK(s->state == RAD_PLAYING, "a 200 plays, state %d", s->state);
    CHECK(s->icy_interval == 16000, "metaint read: %u", s->icy_interval);
    CHECK(strcmp(s->icy_name, "The Penthouse") == 0, "icy-name '%s'", s->icy_name);

    /* --- no metadata offered: the demux must degenerate to a copy --- */
    feed_head(s, "HTTP/1.0 200 OK\r\ncontent-type:audio/mpeg\r\n\r\n");
    CHECK(s->state == RAD_PLAYING, "still plays");
    CHECK(s->icy_interval == 0, "no metaint means zero");

    /* --- a content type with no decoder here --- */
    feed_head(s, "HTTP/1.1 200 OK\r\ncontent-type:audio/aac\r\n\r\n");
    CHECK(s->state == RAD_ERROR, "aac is fatal, state %d", s->state);
    CHECK(strstr(s->error, "aac") != NULL, "and says so: '%s'", s->error);

    /* --- 404: refused, and retrying will not help --- */
    s->attempts = 0;
    feed_head(s, "HTTP/1.1 404 Not Found\r\n\r\n");
    CHECK(s->state == RAD_ERROR, "404 is fatal");
    CHECK(strstr(s->error, "404") != NULL, "naming the status: '%s'", s->error);

    /* --- 503: weather, so it retries --- */
    s->attempts = 0;
    feed_head(s, "HTTP/1.1 503 Service Unavailable\r\n\r\n");
    CHECK(s->state == RAD_RETRYING, "5xx retries, state %d", s->state);

    /* --- 429 is the server asking to be asked again --- */
    s->attempts = 0;
    feed_head(s, "HTTP/1.1 429 Too Many Requests\r\n\r\n");
    CHECK(s->state == RAD_RETRYING, "429 retries");

    /* --- a redirect moves the url and starts over --- */
    rad_copy(s->url, sizeof(s->url), "http://a.example/stream");
    s->redirects = 0;
    feed_head(s,
        "HTTP/1.1 302 Found\r\n"
        "Location: http://b.example/real\r\n"
        "\r\n");
    CHECK(strcmp(s->url, "http://b.example/real") == 0,
          "the url follows: '%s'", s->url);
    CHECK(s->redirects == 1, "and is counted");

    /* --- a relative redirect keeps the host --- */
    rad_copy(s->url, sizeof(s->url), "http://a.example:8000/stream");
    rad_copy(s->host, sizeof(s->host), "a.example");
    s->port = 8000;
    s->redirects = 0;
    feed_head(s, "HTTP/1.1 301 Moved\r\nLocation: /other\r\n\r\n");
    CHECK(strcmp(s->url, "http://a.example:8000/other") == 0,
          "a relative Location resolves: '%s'", s->url);

    /* --- a redirect from http to https changes the kind of connection ---
       which is what NTS actually does, twice, before it plays anything. */
    rad_copy(s->url, sizeof(s->url), "http://a.example/stream");
    s->redirects = 0;
    s->secure    = false;
    feed_head(s, "HTTP/1.1 302 Found\r\nLocation: https://a.example/s\r\n\r\n");
    CHECK(strcmp(s->url, "https://a.example/s") == 0,
          "the url follows across schemes: '%s'", s->url);
    CHECK(s->secure, "and the stream knows it is a secure one now");
    CHECK(s->port == 443, "with https's port, got %u", s->port);
    CHECK(s->state == RAD_RESOLVING, "and is dialling again, state %d", s->state);

    /* --- a redirect loop stops --- */
    rad_copy(s->url, sizeof(s->url), "http://a.example/stream");
    s->redirects = 0;
    s->state     = RAD_CONNECTING;   /* the https case above left it in ERROR */
    s->error[0]  = 0;
    for (int i = 0; i < 8 && s->state != RAD_ERROR; i++) {
        feed_head(s, "HTTP/1.1 302 Found\r\nLocation: http://a.example/x\r\n\r\n");
    }
    CHECK(s->state == RAD_ERROR, "a redirect loop ends in an error");
    CHECK(strstr(s->error, "redirect") != NULL, "saying so: '%s'", s->error);

    rad_stream_free(s);
    free(s);
}

/*
 * The demux, driven the way the socket drives it: the same byte stream
 * delivered in every chunk size from 1 upwards, since a metadata block
 * straddling a read is the case the interval counter exists for.
 */
static void test_icy(void)
{
    rad_stream_t *s = (rad_stream_t *)calloc(1, sizeof(rad_stream_t));
    rad_stream_init(s);

    const uint32_t interval = 64;

    /* Build: 64 audio bytes, a block, 64 more, an empty block, 64 more. */
    uint8_t wire[512];
    uint8_t want[512];
    int at = 0, w = 0;

    for (int i = 0; i < 64; i++) { wire[at++] = want[w++] = (uint8_t)(i + 1); }

    const char *meta = "StreamTitle='Miles Davis - So What';StreamUrl='';";
    const int pad = 16 - ((int)strlen(meta) % 16);
    const int metalen = (int)strlen(meta) + (pad == 16 ? 0 : pad);
    wire[at++] = (uint8_t)(metalen / 16);
    memcpy(wire + at, meta, strlen(meta));
    memset(wire + at + strlen(meta), 0, (size_t)(metalen - (int)strlen(meta)));
    at += metalen;

    for (int i = 0; i < 64; i++) { wire[at++] = want[w++] = (uint8_t)(i + 101); }
    wire[at++] = 0;                     /* an empty block: unchanged */
    for (int i = 0; i < 64; i++) { wire[at++] = want[w++] = (uint8_t)(i + 201); }

    for (int chunk = 1; chunk <= 40; chunk++) {
        rad_ring_reset(&s->ring);
        s->icy_interval = interval;
        s->icy_left     = interval;
        s->icy_want     = -1;
        s->icy_at       = 0;
        s->title[0]     = 0;

        for (int off = 0; off < at; off += chunk) {
            int n = chunk;
            if (off + n > at) { n = at - off; }
            demux(s, wire + off, n);
        }

        uint8_t got[512];
        const uint32_t n = rad_ring_read(&s->ring, got, sizeof(got));
        CHECK(n == (uint32_t)w, "chunk %d: %u audio bytes, wanted %d",
              chunk, n, w);
        CHECK(memcmp(got, want, (size_t)w) == 0,
              "chunk %d: the audio is the audio and nothing else", chunk);
        CHECK(strcmp(s->title, "Miles Davis - So What") == 0,
              "chunk %d: title '%s'", chunk, s->title);
    }

    /* A title with an apostrophe in it must not end early. */
    rad_copy(s->icy_buf, sizeof(s->icy_buf),
             "StreamTitle='Guns N' Roses - Sweet Child O' Mine';");
    s->icy_at = (int)strlen(s->icy_buf);
    s->title[0] = 0;
    take_title(s);
    CHECK(strcmp(s->title, "Guns N' Roses - Sweet Child O' Mine") == 0,
          "an apostrophe survives: '%s'", s->title);

    /* A block with no StreamTitle leaves the last one alone. */
    rad_copy(s->icy_buf, sizeof(s->icy_buf), "StreamUrl='http://x';");
    s->icy_at = (int)strlen(s->icy_buf);
    take_title(s);
    CHECK(strcmp(s->title, "Guns N' Roses - Sweet Child O' Mine") == 0,
          "a block without a title changes nothing");

    /* A metadata-free stream is a straight copy. */
    rad_ring_reset(&s->ring);
    s->icy_interval = 0;
    demux(s, wire, at);
    CHECK(rad_ring_used(&s->ring) == (uint32_t)at,
          "with no metaint every byte is audio");

    rad_stream_free(s);
    free(s);
}

/* ================================================================== */
/* The equaliser                                                      */
/* ================================================================== */

/*
 * An independent reference, in double, straight off the RBJ cookbook. The
 * point of writing it twice is that a transcription slip in rad_eq.c shows up
 * here rather than as a filter that sounds nearly right.
 */
typedef struct { double b[3], a[3]; } ref_bq;

static ref_bq ref_highpass(double hz, double q, double fs)
{
    const double w0 = 2.0 * M_PI * hz / fs;
    const double cw = cos(w0), sw = sin(w0);
    const double al = sw / (2.0 * q);
    ref_bq r = { { (1 + cw) / 2, -(1 + cw), (1 + cw) / 2 },
                 { 1 + al, -2 * cw, 1 - al } };
    return r;
}
static ref_bq ref_peaking(double hz, double db, double q, double fs)
{
    const double a  = pow(10.0, db / 40.0);
    const double w0 = 2.0 * M_PI * hz / fs;
    const double cw = cos(w0), sw = sin(w0);
    const double al = sw / (2.0 * q);
    ref_bq r = { { 1 + al * a, -2 * cw, 1 - al * a },
                 { 1 + al / a, -2 * cw, 1 - al / a } };
    return r;
}
static ref_bq ref_shelf(double hz, double db, double s, double fs, int high)
{
    const double a  = pow(10.0, db / 40.0);
    const double w0 = 2.0 * M_PI * hz / fs;
    const double cw = cos(w0), sw = sin(w0);
    const double al = sw / 2.0 * sqrt((a + 1 / a) * (1 / s - 1) + 2);
    const double tr = 2.0 * sqrt(a) * al;
    ref_bq r;
    if (high) {
        r.b[0] = a * ((a + 1) + (a - 1) * cw + tr);
        r.b[1] = -2 * a * ((a - 1) + (a + 1) * cw);
        r.b[2] = a * ((a + 1) + (a - 1) * cw - tr);
        r.a[0] = (a + 1) - (a - 1) * cw + tr;
        r.a[1] = 2 * ((a - 1) - (a + 1) * cw);
        r.a[2] = (a + 1) - (a - 1) * cw - tr;
    } else {
        r.b[0] = a * ((a + 1) - (a - 1) * cw + tr);
        r.b[1] = 2 * a * ((a - 1) - (a + 1) * cw);
        r.b[2] = a * ((a + 1) - (a - 1) * cw - tr);
        r.a[0] = (a + 1) + (a - 1) * cw + tr;
        r.a[1] = -2 * ((a - 1) + (a + 1) * cw);
        r.a[2] = (a + 1) + (a - 1) * cw - tr;
    }
    return r;
}
static double ref_db(ref_bq q, double hz, double fs)
{
    const double w = 2.0 * M_PI * hz / fs;
    const double c1 = cos(-w), s1 = sin(-w), c2 = cos(-2 * w), s2 = sin(-2 * w);
    const double nr = q.b[0] + q.b[1] * c1 + q.b[2] * c2;
    const double ni =          q.b[1] * s1 + q.b[2] * s2;
    const double dr = q.a[0] + q.a[1] * c1 + q.a[2] * c2;
    const double di =          q.a[1] * s1 + q.a[2] * s2;
    return 20.0 * log10(hypot(nr, ni) / hypot(dr, di));
}

static double ref_cascade(const int *p, double hz)
{
    const double fs = 48000.0;
    const double hq = 1.0 / sqrt(2.0);
    double total = 2.0 * ref_db(ref_highpass(p[P_HPF_FREQ], hq, fs), hz, fs);

    total += ref_db(ref_shelf(p[P_LOW_FREQ], p[P_LOW_GAIN] / 10.0,
                              p[P_LOW_RS] / 100.0, fs, 0), hz, fs);
    total += ref_db(ref_peaking(p[P_LOMID_FREQ], p[P_LOMID_GAIN] / 10.0,
                                p[P_LOMID_RQ] / 100.0, fs), hz, fs);
    total += ref_db(ref_peaking(p[P_MID_FREQ], p[P_MID_GAIN] / 10.0,
                                p[P_MID_RQ] / 100.0, fs), hz, fs);
    total += ref_db(ref_peaking(p[P_HIMID_FREQ], p[P_HIMID_GAIN] / 10.0,
                                p[P_HIMID_RQ] / 100.0, fs), hz, fs);
    total += ref_db(ref_shelf(p[P_HIGH_FREQ], p[P_HIGH_GAIN] / 10.0,
                              p[P_HIGH_RS] / 100.0, fs, 1), hz, fs);
    return total;
}

static void test_eq_response(void)
{
    static const int settings[3][P_COUNT] = {
        /* flat, with the high-pass at the bottom of its range */
        { 20, 120, 0, 100, 300, 0, 100, 1000, 0, 100, 3500, 0, 100, 6000, 0, 100 },
        /* the defaults */
        { 120, 120, 0, 100, 300, 0, 100, 1000, 0, 100, 3500, 0, 100, 6000, 0, 100 },
        /* everything moved, and the rails on the gains */
        { 400, 80, 150, 50, 250, -120, 30, 2500, 90, 190, 9000, -150, 20, 14000, 120, 180 },
    };

    for (int k = 0; k < 3; k++) {
        rad_eq_t eq;
        memset(&eq, 0, sizeof(eq));
        rad_eq_design(&eq, settings[k]);

        double worst = 0;
        double worst_hz = 0;
        for (int i = 0; i < 400; i++) {
            const double t  = (double)i / 399.0;
            const double hz = 20.0 * pow(1000.0, t);
            const double got  = (double)rad_eq_response(&eq, (float)hz);
            const double want = ref_cascade(settings[k], hz);
            if (want < -80.0) {
                continue;       /* deep in the stopband, where the float
                                   cascade's product is clamped on purpose */
            }
            const double d = fabs(got - want);
            if (d > worst) { worst = d; worst_hz = hz; }
        }
        printf("  setting %d: worst %.4f dB at %.0f Hz\n", k, worst, worst_hz);
        CHECK(worst < 0.05, "setting %d response differs by %.4f dB at %.0f Hz",
              k, worst, worst_hz);
    }
}

static void test_eq_run(void)
{
    /* Flat, with the high-pass right down: a 1 kHz tone must come out at the
       level it went in, which is the end-to-end check that the coefficients
       and the difference equation agree. */
    static const int flat[P_COUNT] =
        { 20, 120, 0, 100, 300, 0, 100, 1000, 0, 100, 3500, 0, 100, 6000, 0, 100 };

    rad_eq_t eq;
    memset(&eq, 0, sizeof(eq));
    rad_eq_design(&eq, flat);
    rad_eq_reset(&eq);

    int16_t block[4800];
    for (int i = 0; i < 4800; i++) {
        block[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    }
    rad_eq_run(&eq, block, 4800);

    /* Skip the first few hundred samples: the filter starts from rest. */
    double peak = 0;
    for (int i = 1000; i < 4800; i++) {
        if (fabs((double)block[i]) > peak) { peak = fabs((double)block[i]); }
    }
    printf("  flat 1 kHz peak %.0f of 8000\n", peak);
    CHECK(peak > 7800 && peak < 8200, "a flat EQ passes 1 kHz: peak %.0f", peak);

    /* A 15 dB boost at 1 kHz must actually be about 15 dB. */
    int boosted[P_COUNT];
    memcpy(boosted, flat, sizeof(boosted));
    boosted[P_MID_GAIN] = 150;
    boosted[P_MID_RQ]   = 70;
    rad_eq_design(&eq, boosted);
    rad_eq_reset(&eq);

    for (int i = 0; i < 4800; i++) {
        block[i] = (int16_t)(2000.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    }
    rad_eq_run(&eq, block, 4800);
    peak = 0;
    for (int i = 1000; i < 4800; i++) {
        if (fabs((double)block[i]) > peak) { peak = fabs((double)block[i]); }
    }
    const double db = 20.0 * log10(peak / 2000.0);
    printf("  +15 dB band measured %.2f dB\n", db);
    CHECK(fabs(db - 15.0) < 0.4, "the boost is %.2f dB, wanted 15", db);

    /* And it must clip rather than wrap: the same boost on a loud input. */
    rad_eq_reset(&eq);
    for (int i = 0; i < 4800; i++) {
        block[i] = (int16_t)(30000.0 * sin(2.0 * M_PI * 1000.0 * i / 48000.0));
    }
    rad_eq_run(&eq, block, 4800);
    int sign_flips = 0;
    for (int i = 1001; i < 4800; i++) {
        /* A wrap shows up as a sample of the opposite sign next to a rail. */
        if (block[i - 1] > 30000 && block[i] < -30000) { sign_flips++; }
        if (block[i - 1] < -30000 && block[i] > 30000) { sign_flips++; }
    }
    CHECK(sign_flips == 0, "overdriving clips, it does not wrap (%d wraps)",
          sign_flips);

    /* A 200 Hz tone under a 400 Hz four-pole high-pass should be well down. */
    int hp[P_COUNT];
    memcpy(hp, flat, sizeof(hp));
    hp[P_HPF_FREQ] = 400;
    rad_eq_design(&eq, hp);
    rad_eq_reset(&eq);
    for (int i = 0; i < 9600; i++) {
        block[i % 4800] = (int16_t)(8000.0 * sin(2.0 * M_PI * 200.0 * i / 48000.0));
        if (i % 4800 == 4799) { rad_eq_run(&eq, block, 4800); }
    }
    peak = 0;
    for (int i = 1000; i < 4800; i++) {
        if (fabs((double)block[i]) > peak) { peak = fabs((double)block[i]); }
    }
    const double cut = 20.0 * log10(peak / 8000.0);
    printf("  200 Hz under a 400 Hz 24dB/oct high-pass: %.1f dB\n", cut);
    CHECK(cut < -18.0 && cut > -32.0,
          "a four-pole high-pass an octave down should be about -24 dB, got %.1f",
          cut);
}

/* ================================================================== */
/* The resampler                                                      */
/* ================================================================== */

static void test_resample(void)
{
    rad_audio_t *a = (rad_audio_t *)calloc(1, sizeof(rad_audio_t));
    a->pcm      = (int16_t *)malloc(sizeof(int16_t) * NEOS_AUDIO_RATE * 2);
    a->pcm_size = NEOS_AUDIO_RATE * 2;
    a->volume   = -1;

    /* A 1 kHz tone at 44100, stereo, with the two channels in antiphase in
       the second half - the fold to mono has to cancel them. */
    const int frames = 4410;                 /* 100 ms */
    int16_t *in = (int16_t *)malloc(sizeof(int16_t) * (size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const double v = 8000.0 * sin(2.0 * M_PI * 1000.0 * i / 44100.0);
        in[i * 2]     = (int16_t)v;
        in[i * 2 + 1] = (int16_t)v;
    }

    a->frame_rate = 44100;
    a->have_last  = false;
    a->phase      = 0;
    resample_in(a, in, frames, 2);

    const uint32_t out = (a->pcm_head >= a->pcm_tail)
                       ? a->pcm_head - a->pcm_tail
                       : a->pcm_size - a->pcm_tail + a->pcm_head;

    /* 100 ms in is 100 ms out, which at 48 kHz is 4800 frames. A couple
       either side is the interpolator's phase, not a rate error. */
    printf("  44100 -> 48000: %d frames in, %u out\n", frames, out);
    CHECK(out >= 4798 && out <= 4802, "rate conversion produced %u frames", out);

    /* The tone survives: a 1 kHz sine resampled is still a 1 kHz sine, so its
       zero crossings are still 48 samples apart. */
    int crossings = 0;
    for (uint32_t i = a->pcm_tail + 200; i + 1 < a->pcm_tail + out; i++) {
        if (a->pcm[i % a->pcm_size] <= 0 && a->pcm[(i + 1) % a->pcm_size] > 0) {
            crossings++;
        }
    }
    printf("  upward zero crossings in ~96 ms: %d\n", crossings);
    CHECK(crossings >= 95 && crossings <= 97,
          "a 1 kHz tone should cross up ~96 times, got %d", crossings);

    int16_t peak = 0;
    for (uint32_t i = 0; i < out; i++) {
        const int16_t v = a->pcm[(a->pcm_tail + i) % a->pcm_size];
        if (v > peak) { peak = v; }
    }
    CHECK(peak > 7700 && peak <= 8100, "amplitude survives: %d", peak);

    /* Antiphase must fold to silence. */
    a->pcm_head = a->pcm_tail = 0;
    a->have_last = false;
    a->phase = 0;
    for (int i = 0; i < frames; i++) {
        const double v = 8000.0 * sin(2.0 * M_PI * 1000.0 * i / 44100.0);
        in[i * 2]     = (int16_t)v;
        in[i * 2 + 1] = (int16_t)-v;
    }
    resample_in(a, in, frames, 2);
    int16_t loudest = 0;
    for (uint32_t i = a->pcm_tail; i != a->pcm_head; i = (i + 1) % a->pcm_size) {
        const int16_t v = a->pcm[i] < 0 ? (int16_t)-a->pcm[i] : a->pcm[i];
        if (v > loudest) { loudest = v; }
    }
    CHECK(loudest <= 2, "antiphase folds to silence, peak %d", loudest);

    /* 48000 in must be a straight pass-through, sample for sample. */
    a->pcm_head = a->pcm_tail = 0;
    a->have_last = false;
    a->phase = 0;
    a->frame_rate = 48000;
    int16_t mono[480];
    for (int i = 0; i < 480; i++) {
        mono[i] = (int16_t)(i * 37 - 8000);
    }
    resample_in(a, mono, 480, 1);
    const uint32_t n48 = a->pcm_head - a->pcm_tail;
    /* One sample short, and only ever at the very start of a stream: the
       interpolator needs a pair to stand between, so the first input primes
       it. Everything after that is the input, sample for sample. */
    CHECK(n48 == 479, "48 kHz passes 1:1, got %u", n48);
    int same = 1;
    for (uint32_t i = 0; i < n48; i++) {
        if (a->pcm[i] != mono[i]) {
            printf("  [%u] %d != %d\n", i, a->pcm[i], mono[i]);
            same = 0;
            break;
        }
    }
    CHECK(same, "and unchanged, sample for sample");

    free(in);
    free(a->pcm);
    free(a);
}

/* ================================================================== */
/* The tape path                                                      */
/* ================================================================== */
/* The whole audio path                                               */
/* ================================================================== */

/*
 * The one test that is not about a function: a real MP3 file, delivered the
 * way a Shoutcast server delivers one - in socket-sized lumps, with ICY
 * metadata blocks cut into it - and taken all the way out to the frames that
 * would go to the codec. Everything between is under test at once: the demux,
 * the ring, minimp3, the fold to mono, the resampler and the pacing.
 *
 * What it checks at the end is the only thing that matters about an internet
 * radio: two seconds of sound went in, two seconds of sound came out, and it
 * is still the note it was.
 */
static uint8_t *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { return NULL; }
    fseek(f, 0, SEEK_END);
    *len = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)*len);
    if (fread(buf, 1, (size_t)*len, f) != (size_t)*len) { *len = 0; }
    fclose(f);
    return buf;
}

static void run_one_file(const char *path, int want_rate, double want_hz,
                         double want_seconds, uint32_t metaint)
{
    int len = 0;
    uint8_t *mp3 = slurp(path, &len);
    if (!mp3 || len <= 0) {
        printf("  SKIP %s is not there\n", path);
        free(mp3);
        return;
    }

    rad_stream_t *s = (rad_stream_t *)calloc(1, sizeof(rad_stream_t));
    rad_stream_init(s);
    s->state        = RAD_PLAYING;
    s->buffering    = false;
    s->icy_interval = metaint;
    s->icy_left     = metaint;
    s->icy_want     = -1;

    rad_audio_t *a = (rad_audio_t *)calloc(1, sizeof(rad_audio_t));
    CHECK(rad_audio_init(a), "the audio path comes up");
    rad_audio_reset(a);

    static int16_t out[NEOS_AUDIO_RATE * 4];
    int written = 0;

    /*
     * Delivered in 1500-byte lumps - about an Ethernet segment - with an ICY
     * block cut in at every `metaint` bytes of audio, which is what a server
     * that was asked for metadata does.
     */
    const char *meta = "StreamTitle='Test Tone - 1 kHz';";
    uint8_t block[512];
    const int metalen = ((int)strlen(meta) + 15) / 16 * 16;
    block[0] = (uint8_t)(metalen / 16);
    memset(block + 1, 0, (size_t)metalen);
    memcpy(block + 1, meta, strlen(meta));

    int at = 0;
    uint32_t until_meta = metaint;

    while (at < len) {
        /* Fill the ring the way rad_stream_poll would. */
        while (at < len && rad_ring_room(&s->ring) > 4096) {
            int n = 1500;
            if (at + n > len) { n = len - at; }
            if (metaint) {
                if (until_meta == 0) {
                    demux(s, block, metalen + 1);
                    until_meta = metaint;
                    continue;
                }
                if ((uint32_t)n > until_meta) { n = (int)until_meta; }
                until_meta -= (uint32_t)n;
            }
            demux(s, mp3 + at, n);
            at += n;
        }

        /* And drain it the way the loop would. */
        decode_some(a, s);
        while (a->pcm_head != a->pcm_tail &&
               written < (int)(sizeof(out) / sizeof(out[0]))) {
            out[written++] = a->pcm[a->pcm_tail];
            a->pcm_tail = (a->pcm_tail + 1) % a->pcm_size;
        }
    }
    /* Whatever is still in the byte ring at the end. */
    for (int i = 0; i < 64; i++) {
        decode_some(a, s);
        while (a->pcm_head != a->pcm_tail &&
               written < (int)(sizeof(out) / sizeof(out[0]))) {
            out[written++] = a->pcm[a->pcm_tail];
            a->pcm_tail = (a->pcm_tail + 1) % a->pcm_size;
        }
    }

    printf("  %s: %d bytes -> %d frames (%.3f s), stream %d Hz %d kbps\n",
           path, len, written, written / (double)NEOS_AUDIO_RATE,
           s->rate_hz, s->bitrate_kbps);

    CHECK(s->rate_hz == want_rate, "the stream rate is read as %d, wanted %d",
          s->rate_hz, want_rate);
    CHECK(s->bitrate_kbps > 0, "and its bitrate: %d", s->bitrate_kbps);

    if (metaint) {
        CHECK(strcmp(s->title, "Test Tone - 1 kHz") == 0,
              "the title came out of the stream: '%s'", s->title);
    }

    const double secs = written / (double)NEOS_AUDIO_RATE;
    CHECK(fabs(secs - want_seconds) < 0.12,
          "%.3f s out for %.1f s in", secs, want_seconds);

    /*
     * And it is still the note. Counted as upward zero crossings over the
     * middle of the clip, away from the encoder's lead-in and the decoder's
     * first frame.
     */
    const int from = NEOS_AUDIO_RATE / 4;
    const int to   = written - NEOS_AUDIO_RATE / 8;
    CHECK(to > from + 1000, "enough audio to measure: %d frames", written);
    if (to <= from + 1000) {
        rad_stream_free(s); free(s); rad_audio_free(a); free(a); free(mp3);
        return;
    }

    int crossings = 0;
    int peak = 0;
    for (int i = from; i + 1 < to; i++) {
        if (out[i] <= 0 && out[i + 1] > 0) { crossings++; }
        const int v = out[i] < 0 ? -out[i] : out[i];
        if (v > peak) { peak = v; }
    }
    const double span = (double)(to - from) / (double)NEOS_AUDIO_RATE;
    const double hz   = crossings / span;
    printf("  measured %.1f Hz, peak %d\n", hz, peak);
    CHECK(fabs(hz - want_hz) < want_hz * 0.01,
          "the tone comes out at %.1f Hz, wanted %.0f", hz, want_hz);
    /* The fixtures are encoded at about -1.5 dBFS, so anything much under
       three quarters of full scale means the fold or the interpolation is
       losing level rather than the encoder being quiet. */
    CHECK(peak > 24000, "at the level it was encoded at: %d", peak);

    rad_stream_free(s);
    free(s);
    rad_audio_free(a);
    free(a);
    free(mp3);
}

static void test_audio_path(void)
{
    /* 44100 stereo, the ordinary case, with ICY metadata cut into it. */
    run_one_file("tone.mp3", 44100, 1000.0, 2.0, 16000);
    /* Again with the blocks at an awkward interval, so one lands inside
       almost every socket read. */
    run_one_file("tone.mp3", 44100, 1000.0, 2.0, 1013);
    /* A stream already at the output rate, which the resampler passes on. */
    run_one_file("tone48.mp3", 48000, 440.0, 1.0, 0);
}

/* ================================================================== */
/* Layout and hit testing                                             */
/* ================================================================== */

/*
 * The pages lay themselves out inside whatever ngl_app_area() hands over, so
 * what is worth checking is not where a box is but that the drawing and the
 * hit test agree about it - and that nothing runs off the panel, which on a
 * page of six rows and a curve is a real risk after a change of font.
 */
static void test_layout(void)
{
    const ngl_rect_t area = ngl_app_area();
    const ngl_rect_t page = rad_ui_page(area);

    CHECK(page.y == area.y + UI_TAB_H, "the page starts under the strip");
    CHECK(page.h == area.h - UI_TAB_H, "and is the rest of it");

    rad_app_t *app = (rad_app_t *)calloc(1, sizeof(rad_app_t));
    app->selected = -1;
    stub_clear_files();
    rad_settings_load(app);
    stub_put_file("apps/radio/stations.conf",
        "A | http://a.example/1 | auto\nB | http://b.example/2\n"
        "C | http://c.example/3\nD | http://d.example/4\n"
        "E | http://e.example/5\nF | http://f.example/6\n"
        "G | http://g.example/7\nH | http://h.example/8\n"
        "I | http://i.example/9\nJ | http://j.example/10\n");
    app->stations = rad_stations_load(app->station, RAD_STATIONS_MAX);
    CHECK(app->stations == 10, "ten stations, got %d", app->stations);

    const int per = rad_ui_per_page();
    CHECK(per == 9, "nine to a page on this panel, got %d", per);

    /* The tabs: tapping the middle of each one selects it. */
    for (int i = 0; i < TAB_COUNT; i++) {
        const int16_t x = (int16_t)(area.x + UI_MARGIN + 140 +
                                    i * (UI_TAB_W + UI_TAB_GAP) + UI_TAB_W / 2);
        const int16_t y = (int16_t)(area.y + UI_TAB_H / 2);
        char want[16];
        snprintf(want, sizeof(want), "tab:%d", i);
        const char *got = rad_ui_hit(app, x, y);
        CHECK(rad_streq(got, want), "tab %d hits '%s', wanted '%s'", i,
              got ? got : "(none)", want);
    }

    /* The NOW grid: every cell of a full page answers, the star half and the
       name half differently, and nothing lands off the bottom. */
    app->tab  = TAB_NOW;
    app->page = 0;
    int rows_seen = 0;
    for (int slot = 0; slot < per; slot++) {
        char want_row[16], want_star[16];
        snprintf(want_row, sizeof(want_row), "row%d", slot);
        snprintf(want_star, sizeof(want_star), "star%d", slot);

        bool found_row = false, found_star = false;
        for (int16_t y = page.y; y < page.y + page.h; y += 2) {
            for (int16_t x = page.x; x < page.x + page.w; x += 8) {
                const char *got = rad_ui_hit(app, x, y);
                if (rad_streq(got, want_row))  { found_row = true; }
                if (rad_streq(got, want_star)) { found_star = true; }
            }
        }
        CHECK(found_row, "cell %d has a body", slot);
        CHECK(found_star, "cell %d has a star", slot);
        if (found_row) { rows_seen++; }
    }
    CHECK(rows_seen == per, "every cell on the page is reachable");

    /* Page two holds the tenth station and nothing beyond it. */
    app->page = 1;
    bool found_10 = false, found_11 = false;
    for (int16_t y = page.y; y < page.y + page.h; y += 4) {
        for (int16_t x = page.x; x < page.x + page.w; x += 8) {
            const char *got = rad_ui_hit(app, x, y);
            if (rad_streq(got, "row9"))  { found_10 = true; }
            if (rad_streq(got, "row10")) { found_11 = true; }
        }
    }
    CHECK(found_10, "the tenth station is on page two");
    CHECK(!found_11, "and there is no eleventh");

    /* The transport row. */
    app->page = 0;
    bool play = false, up = false, down = false, bar = false;
    for (int16_t y = page.y; y < page.y + page.h; y += 2) {
        for (int16_t x = page.x; x < page.x + page.w; x += 4) {
            const char *got = rad_ui_hit(app, x, y);
            if (rad_streq(got, "play"))    { play = true; }
            if (rad_streq(got, "volup"))   { up = true; }
            if (rad_streq(got, "voldown")) { down = true; }
            if (rad_streq(got, "volbar"))  { bar = true; }
        }
    }
    CHECK(play && up && down && bar, "the transport is all reachable");

    /* The volume bar reads left to right, ends included. */
    CHECK(rad_ui_volume_from_x((int16_t)(page.x - 500)) == 0, "left of the bar is 0");
    CHECK(rad_ui_volume_from_x((int16_t)(page.x + page.w + 500)) == 100,
          "right of it is 100");

    /* The EQ page: every slider and every stepper is reachable. */
    app->tab = TAB_EQ;
    int seen_slider = 0, seen_step = 0;
    for (int p = 0; p < P_COUNT; p++) {
        char s_key[24], minus[24], plus[24];
        snprintf(s_key, sizeof(s_key), "slider:%d", p);
        snprintf(minus, sizeof(minus), "step:%d-", p);
        snprintf(plus,  sizeof(plus),  "step:%d+", p);

        bool hit_s = false, hit_m = false, hit_p = false;
        for (int16_t y = page.y; y < page.y + page.h; y += 2) {
            for (int16_t x = page.x; x < page.x + page.w; x += 4) {
                const char *got = rad_eqpage_hit(page, x, y);
                if (rad_streq(got, s_key)) { hit_s = true; }
                if (rad_streq(got, minus)) { hit_m = true; }
                if (rad_streq(got, plus))  { hit_p = true; }
            }
        }
        if (hit_s) { seen_slider++; }
        if (hit_m && hit_p) { seen_step++; }
    }
    /*
     * Six sliders, one per row. Eleven stepper pairs, not sixteen: the five
     * gains are slider-only - the row's slider *is* the gain - and the
     * high-pass has one stepper because its slider carries its frequency, so
     * drag sweeps and tap trims the same value.
     */
    CHECK(seen_slider == 6, "six rows carry a slider, found %d", seen_slider);
    CHECK(seen_step == 11, "eleven parameters have a - and a +, found %d",
          seen_step);
    for (int p = 0; p < P_COUNT; p++) {
        if (rad_ranges[p].unit[0] != 'd') {
            continue;                       /* not a gain */
        }
        char minus[24];
        snprintf(minus, sizeof(minus), "step:%d-", p);
        bool found = false;
        for (int16_t y = page.y; y < page.y + page.h && !found; y += 2) {
            for (int16_t x = page.x; x < page.x + page.w; x += 4) {
                if (rad_streq(rad_eqpage_hit(page, x, y), minus)) {
                    found = true;
                    break;
                }
            }
        }
        CHECK(!found, "gain %d is the slider's own value, not a stepper", p);
    }

    /* And the EQ sliders reach both ends of their range. */
    for (int p = 0; p < P_COUNT; p++) {
        const int lo = rad_eqpage_slider_value((rad_param_t)p, page,
                                               (int16_t)(page.x - 200));
        const int hi = rad_eqpage_slider_value((rad_param_t)p, page,
                                               (int16_t)(page.x + page.w + 200));
        CHECK(lo == rad_ranges[p].lo, "slider %d bottoms at %d", p, lo);
        CHECK(hi == rad_ranges[p].hi, "slider %d tops at %d", p, hi);
    }

    free(app);
}

/* ================================================================== */

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "ring",       test_ring },
        { "params",     test_params },
        { "urls",       test_urls },
        { "stations",   test_stations },
        { "settings",   test_settings },
        { "http",       test_http },
        { "icy",        test_icy },
        { "eq response",test_eq_response },
        { "eq run",     test_eq_run },
        { "resample",   test_resample },
        { "audio path", test_audio_path },
        { "layout",     test_layout },
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        const int before = g_fail;
        printf("%s\n", tests[i].name);
        tests[i].fn();
        if (g_fail == before) {
            printf("  ok\n");
        }
    }

    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
