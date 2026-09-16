/*
 * radio - internet radio for NeOS.
 *
 * A port of the pi-Q app of the same name, and mostly an exercise in owning
 * the parts that used to be someone else's process. On the Pi the app opened
 * the stream and handed the bytes to mpg123 over a pipe, ran its effects in
 * scsynth over OSC, and let jackd worry about when a block of audio was due.
 * None of those exist here: an app on this tablet is one function on NeOS's
 * own stack, with no threads, no subprocesses and no scheduler entry. So the
 * HTTP, the MP3, the resampler, the equaliser and the pacing are all in this
 * program, and the whole of it has to get back to
 * neos_app_close_requested() every few milliseconds.
 *
 * That constraint is what shapes everything below. Every stage is a state
 * machine asked to make some progress and give the loop back - the same rule
 * lanscan's port arrived at - and the two buffers between the stages are what
 * let the network stall for a second without the speaker hearing about it.
 *
 *      socket  ->  [ byte ring ]  ->  minimp3  ->  resample 44k1 -> 48k mono
 *                                                        |
 *                        speaker  <-  [ PCM ring ]  <-  EQ
 *
 * The one thing NeOS does hand over is the clock. neos_audio_write() blocks
 * until the codec has taken its frames, so the speaker paces the loop exactly
 * as jackd used to - the difference being that here the loop must not let it,
 * which is what neos_audio_lead_us() is for. See rad_audio.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>      /* snprintf, which the syscall table exports */

#include "ngl.h"
#include "neos_api.h"
#include "neos_net.h"
#include "neos_sock.h"
#include "neos_sys.h"

/* ------------------------------------------------------------------ */
/* Stations                                                            */
/* ------------------------------------------------------------------ */

#define RAD_STATIONS_MAX  24
#define RAD_NAME_MAX      64
#define RAD_URL_MAX      192
/* The resolver's limit, not a number of our own: a buffer smaller than what
   neos_resolve() takes would refuse names it would have looked up. */
#define RAD_HOST_MAX     NEOS_HOST_MAX
#define RAD_PATH_MAX     160
#define RAD_TITLE_MAX    192
#define RAD_ERROR_MAX    128

typedef struct {
    char     name[RAD_NAME_MAX];
    char     url[RAD_URL_MAX];
    bool     autoplay;
    /*
     * An https station, which this tablet cannot play: apps get a socket and
     * not a TLS session, so there is nothing here to hand a certificate to.
     * Kept in the list rather than dropped from it - the file is the user's,
     * the station is not wrong, and the row explains itself instead of
     * failing at connect time. See the README for what would have to change.
     */
    bool     secure;
} rad_station_t;

int  rad_stations_load(rad_station_t *out, int max);
bool rad_stations_save(const rad_station_t *list, int count);

/** Split a URL. False if it is not something this app can dial. */
bool rad_url_split(const char *url, char *host, size_t host_sz,
                   uint16_t *port, char *path, size_t path_sz, bool *secure);

/* ------------------------------------------------------------------ */
/* The byte ring between the socket and the decoder                    */
/* ------------------------------------------------------------------ */

/*
 * Sized for the stall this app is actually built to survive, which is a
 * second or two of marginal Wi-Fi and not an outage - an outage is what the
 * reconnect is for. 256 KB is about sixteen seconds of a 128 kbit stream, so
 * the reserve is far more than the network needs and the cost is PSRAM, of
 * which there is 32 MB.
 *
 * It is also the only place the socket's bursts are absorbed. The loop cannot
 * read while neos_audio_write() is inside the codec, so bytes arrive in
 * clumps of whatever lwIP held meanwhile, and the decoder wants a smooth
 * supply. That is a buffer's whole job.
 */
#define RAD_NET_RING   (256 * 1024)

typedef struct {
    uint8_t  *buf;
    uint32_t  size;
    uint32_t  head;      /* where the producer writes */
    uint32_t  tail;      /* where the consumer reads */
} rad_ring_t;

bool     rad_ring_init(rad_ring_t *r, uint32_t size);
void     rad_ring_free(rad_ring_t *r);
void     rad_ring_reset(rad_ring_t *r);
uint32_t rad_ring_used(const rad_ring_t *r);
uint32_t rad_ring_room(const rad_ring_t *r);
uint32_t rad_ring_write(rad_ring_t *r, const uint8_t *src, uint32_t n);
uint32_t rad_ring_read(rad_ring_t *r, uint8_t *dst, uint32_t n);
/** Look at up to @p n contiguous bytes without consuming them. */
uint32_t rad_ring_peek(const rad_ring_t *r, const uint8_t **at);
void     rad_ring_skip(rad_ring_t *r, uint32_t n);

/* ------------------------------------------------------------------ */
/* The stream                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    RAD_STOPPED = 0,
    RAD_RESOLVING,
    RAD_CONNECTING,
    RAD_PLAYING,
    RAD_RETRYING,
    RAD_ERROR,          /* retrying would not fix it; say so and stop */
} rad_state_t;

/*
 * Seconds to wait before each reconnection attempt, the last value repeating
 * - straight from the Pi app, and for the same reason. Most drops on a
 * tablet's Wi-Fi are a momentary stumble and come back at once; the cap is
 * low enough that a station returning after a real outage is picked up within
 * a quarter of a minute.
 */
#define RAD_RETRIES   5
extern const uint8_t rad_retry_delay_s[RAD_RETRIES];

/*
 * How much the ring must hold before the speaker is handed anything.
 *
 * Three seconds of a 128 kbit stream. The cost is three seconds between the
 * tap and the sound, which is about what any internet radio costs and less
 * than the connect that precedes it; what it buys is that the first Wi-Fi
 * stumble after the station comes up is inaudible rather than a gap at the
 * worst possible moment. Below about a second this was audibly fragile on
 * this board, whose radio sits at -61 to -71 dBm like every other.
 */
#define RAD_PREBUFFER  (48 * 1024)

/* Sub-states of RAD_CONNECTING. The UI never sees these - to someone holding
   the tablet, a socket that is still connecting and a response head that is
   half here are the same sentence. */
typedef enum {
    PHASE_CONNECT = 0,   /* waiting for the connect to come back */
    PHASE_SEND,          /* pushing the request out */
    PHASE_HEAD,          /* reading the response head */
} rad_phase_t;

typedef struct {
    rad_state_t state;
    char        title[RAD_TITLE_MAX];   /* ICY StreamTitle, "" if none yet */
    char        icy_name[RAD_NAME_MAX]; /* what the server calls itself */
    char        error[RAD_ERROR_MAX];
    int         attempts;               /* consecutive failures */
    int         retry_in;               /* seconds left, while RETRYING */
    int         station;                /* index, -1 when none */
    int         bitrate_kbps;           /* as decoded, 0 until a frame lands */
    int         rate_hz;                /* the stream's own sample rate */
    bool        buffering;              /* connected, filling before it plays */

    /* --- the parts the UI has no business in --- */
    char        url[RAD_URL_MAX];       /* the current one: redirects move it */
    /*
     * One of these carries the stream. A plain http station is an ordinary
     * socket; an https one is a TLS session, which owns a socket of its own
     * that this app never touches. `secure` is which.
     */
    bool        secure;
    int         fd;                     /* http */
    int         tls;                    /* https, a neos_tls handle */
    rad_phase_t phase;
    uint32_t    host_ip;
    uint16_t    port;
    char        host[RAD_HOST_MAX];
    char        path[RAD_PATH_MAX];
    int         redirects;

    char        req[512];
    int         req_len;
    int         req_sent;

    char        hdr[2048];
    int         hdr_len;

    uint32_t    icy_interval;           /* 0 when the server sends no metadata */
    uint32_t    icy_left;               /* bytes of audio until the next block */
    int         icy_want;               /* bytes of a metadata block still due */
    int         icy_at;
    char        icy_buf[512];

    uint32_t    started_at;             /* ms, when this phase began */
    uint32_t    fed_at;                 /* ms, when a byte last arrived */
    uint32_t    retry_at;
    rad_ring_t  ring;
} rad_stream_t;

bool rad_stream_init(rad_stream_t *s);
void rad_stream_free(rad_stream_t *s);
/** Start playing @p st. Any previous connection is dropped. */
void rad_stream_play(rad_stream_t *s, const rad_station_t *st, int index);
void rad_stream_stop(rad_stream_t *s);
/** Make some progress. Called every time round the app's loop. */
void rad_stream_poll(rad_stream_t *s);
/** True while the app considers itself playing - connecting and retrying
    included, because the station is still selected and still the answer. */
static inline bool rad_stream_live(const rad_stream_t *s)
{
    return s->state == RAD_RESOLVING || s->state == RAD_CONNECTING ||
           s->state == RAD_PLAYING   || s->state == RAD_RETRYING;
}

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

/*
 * Every EQ parameter is an integer in its own smallest unit - hertz, tenths
 * of a decibel, hundredths of a Q - which is the convention the whole NeOS
 * ABI trades in, and here it earns its place twice over. Formatting a value
 * is integer division rather than a "%f" that would promote to double and
 * take the app's load with it; and a settings file of integers cannot come
 * back from the card as a number that is nearly what was saved.
 *
 * `decimals` is where the point goes when one of these is printed, so it is
 * also the scale: 0 means the value is hertz, 1 tenths, 2 hundredths.
 */
typedef enum {
    P_HPF_FREQ = 0,
    P_LOW_FREQ,   P_LOW_GAIN,   P_LOW_RS,
    P_LOMID_FREQ, P_LOMID_GAIN, P_LOMID_RQ,
    P_MID_FREQ,   P_MID_GAIN,   P_MID_RQ,
    P_HIMID_FREQ, P_HIMID_GAIN, P_HIMID_RQ,
    P_HIGH_FREQ,  P_HIGH_GAIN,  P_HIGH_RS,
    P_COUNT
} rad_param_t;

typedef struct {
    const char *key;        /* what it is called in the settings file */
    int         lo, hi;     /* scaled by 10^decimals, like the value */
    int         decimals;
    const char *unit;
    int         def;
} rad_range_t;

extern const rad_range_t rad_ranges[P_COUNT];

int  rad_clamp_param(rad_param_t p, int value);
/** One press of a stepper. Frequencies step by a sixth of an octave, so a
    held finger sweeps rather than crawling at one end and leaping at the
    other; everything else steps by a fixed amount. */
int  rad_step_param(rad_param_t p, int value, bool up);
/** "+3.5dB", "1000Hz", "0.70" - into @p out, never promoting to double. */
void rad_format_param(rad_param_t p, int value, char *out, size_t size);

/* ------------------------------------------------------------------ */
/* The equaliser                                                       */
/* ------------------------------------------------------------------ */

/*
 * Seven biquads: a four-pole high-pass as two sections, a low shelf, three
 * peaking bands and a high shelf, in the order the synth applied them. The
 * coefficients are designed at NEOS_AUDIO_RATE and not at the stream's rate,
 * because this runs after the resampler - which is also what makes the curve
 * on the EQ page the filter that is actually running, whatever the station
 * happens to be encoded at.
 */
#define RAD_SECTIONS  7

typedef struct {
    float b0, b1, b2, a1, a2;    /* a0 normalised out */
    float z1, z2;                /* transposed direct form II state */
} rad_biquad_t;

typedef struct {
    rad_biquad_t section[RAD_SECTIONS];
    bool         ready;
} rad_eq_t;

/** Redesign every section from @p params. Cheap enough to call on each move
    of a slider; it does not disturb the filter state, so the sound does not
    click as a knob turns. */
void  rad_eq_design(rad_eq_t *eq, const int *params);
void  rad_eq_reset(rad_eq_t *eq);
void  rad_eq_run(rad_eq_t *eq, int16_t *pcm, int n);
/** Combined response at @p hz, in dB - the same cascade, for drawing. */
float rad_eq_response(const rad_eq_t *eq, float hz);

/* ------------------------------------------------------------------ */
/* Audio out                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    bool      open;
    bool      started;       /* past the prebuffer, handing frames over */
    int16_t  *pcm;           /* decoded, resampled, mono at NEOS_AUDIO_RATE */
    uint32_t  pcm_size;      /* in frames */
    uint32_t  pcm_head;
    uint32_t  pcm_tail;
    uint32_t  underruns;

    void     *dec;           /* mp3dec_t, off the heap: it is 6 KB */
    int16_t  *frame;         /* one decoded MP3 frame, interleaved */
    int       frame_rate;    /* the stream's rate, for the resampler */
    int       frame_ch;
    uint32_t  phase;         /* resampler position, 16.16 in source frames */
    int16_t   last[2];       /* the sample the next interpolation starts from */
    bool      have_last;

    int       volume;        /* 0-100, what the codec is set to */
    rad_eq_t  eq;
} rad_audio_t;

bool rad_audio_init(rad_audio_t *a);
void rad_audio_free(rad_audio_t *a);
void rad_audio_reset(rad_audio_t *a);
void rad_audio_volume(rad_audio_t *a, int percent);
/** Decode what the ring holds, then hand the speaker what it has room for.
    Returns the milliseconds of sound now queued, for the UI to show. */
int  rad_audio_pump(rad_audio_t *a, rad_stream_t *s);

/* ------------------------------------------------------------------ */
/* The app                                                             */
/* ------------------------------------------------------------------ */

typedef enum { TAB_NOW = 0, TAB_EQ, TAB_COUNT } rad_tab_t;

typedef struct {
    rad_station_t   station[RAD_STATIONS_MAX];
    int             stations;
    int             selected;
    int             page;

    int             params[P_COUNT];
    int             volume;

    rad_tab_t       tab;
    rad_stream_t    stream;
    rad_audio_t     audio;

    char            pressed[24];    /* what a finger is on, "" for nothing */
    char            slider[24];     /* what it is dragging */
    char            repeat[24];     /* the stepper it is holding down */
    uint32_t        repeat_at;
    bool            dirty;          /* settings want writing */
    uint32_t        saved_at;
    bool            repaint;        /* the page changed under us */
} rad_app_t;

void rad_settings_load(rad_app_t *app);
void rad_settings_save(rad_app_t *app);


/* UI. The page owns its own layout inside the area it is handed, so the app
   loop never has a pixel in it. */
void rad_ui_init(rad_app_t *app);
void rad_ui_draw(rad_app_t *app);
/** What is under a touch, as a key like "row3" or "step:midFreq+". NULL for
    nothing. The string lives in static storage until the next call. */
const char *rad_ui_hit(rad_app_t *app, int16_t x, int16_t y);
void rad_ui_repaint(rad_app_t *app);

void rad_eqpage_draw(rad_app_t *app, ngl_rect_t area, bool full);
const char *rad_eqpage_hit(ngl_rect_t area, int16_t x, int16_t y);
int  rad_eqpage_slider_value(rad_param_t p, ngl_rect_t area, int16_t x);

/* Small shared helpers. */
uint32_t rad_now(void);
bool     rad_streq(const char *a, const char *b);
/** Case-insensitive "does @p hay start with @p needle". */
bool     rad_starts(const char *hay, const char *needle);
/** strstr, which the loader's table does not carry. NULL if it is not there. */
const char *rad_find(const char *hay, const char *needle);
void     rad_copy(char *dst, size_t size, const char *src);
