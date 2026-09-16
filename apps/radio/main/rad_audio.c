/*
 * Bytes in, sound out, and the pacing that keeps the close button working.
 *
 * The chain is short. minimp3 turns the ring's bytes into interleaved frames
 * at whatever the station encodes at; those are folded to mono and resampled
 * to NEOS_AUDIO_RATE, because the codec is clocked once at bring-up and plays
 * whatever it is handed at the rate the channel runs at - hand it 44100 and
 * it plays the right samples nine percent fast. The result waits in a second
 * ring until the speaker has room.
 *
 * Two things about that second ring are worth stating, because they are the
 * whole design.
 *
 * The equaliser runs on the way *out* of it and not on the way in. Filtering
 * at decode time would be filtering three seconds before anything is heard,
 * so a knob would move and the sound would change a breath later; running it
 * on the block being handed over costs the same cycles and makes the EQ page
 * live, which is what an EQ page is for.
 *
 * And the loop must never let neos_audio_write() block for long. It blocks
 * until the codec has taken its frames, which is exactly the frame clock an
 * emulator wants and exactly the wrong thing here: this app has a socket to
 * drain and a screen to draw, and an app asleep in the codec is an app not
 * polling neos_app_close_requested(). So the lead is measured first and
 * frames are handed over only while there is known room for them - which
 * turns a blocking call into a non-blocking one without needing it to be.
 */

#include "radio.h"
#include "rad_math.h"

#include <stdlib.h>
#include <string.h>

/* Declarations only: the implementation is compiled once, in rad_mp3.c, and
   that file says why it is on its own. */
#include "minimp3.h"

/*
 * How far ahead of the speaker to stay, and the number that everything else
 * about this app's pacing follows from.
 *
 * The ceiling is the hardware's, not a preference. neos_audio_write() blocks
 * until the I2S DMA has taken the frames, so the lead can never exceed what
 * the DMA holds - six descriptors of 240 frames, which at NEOS_AUDIO_RATE is
 * exactly 30 ms. Asking for more than that is not a deeper buffer, it is a
 * loop that sits inside the codec waiting for room that is never coming, and
 * a close button that answers a third of a second late.
 *
 * So 24 ms: under the DMA's depth, so a write always has somewhere to go and
 * returns at once, and enough of a cushion that a frame which overruns a
 * little is still inaudible.
 *
 * The other half of that bargain is the loop's: the speaker runs dry 30 ms
 * after the last write, so the whole of radio_app.c has to come round inside
 * that. It is why the pages repaint only what changed, and why the one page
 * that repaints wholesale tops the speaker up in the middle of doing it.
 */
#define LEAD_TARGET_US   24000

/* One handover. 10 ms is small enough that three of them fit in the cushion
   above, and large enough that the per-call overhead is nothing next to the
   work inside. */
#define BLOCK_FRAMES    (NEOS_AUDIO_RATE / 100)

/* Two seconds of mono at the output rate. This is a reservoir and not a
   jitter buffer - the byte ring upstream is the jitter buffer - so it only
   has to be deep enough that a decode which lands late is still early. */
#define PCM_FRAMES      (NEOS_AUDIO_RATE * 2)

/* What must be in the PCM ring before the speaker is fed again after it has
   run dry. Without it, one underrun becomes a stutter: the first block to
   arrive is handed straight over, runs out, and the gap repeats. */
#define PCM_RESTART     (NEOS_AUDIO_RATE / 4)

/* The window minimp3 hunts a frame header in. A layer III frame is at most
   1441 bytes, so this holds four of them plus whatever junk sits between a
   stream's start and its first sync word. */
#define WINDOW_BYTES    8192

typedef struct {
    uint8_t  *win;
    int       win_len;
    mp3dec_t  dec;
} rad_decoder_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool rad_audio_init(rad_audio_t *a)
{
    memset(a, 0, sizeof(*a));

    /*
     * All three of these are PSRAM, which is the right pool for every one of
     * them: none is touched per-pixel or per-sample-with-a-cache-miss-in-the
     * -middle, and neos_alloc_fast()'s internal RAM is scarce and wanted by
     * the framebuffer. The decoder state is about six kilobytes of overlap
     * and QMF history and is the one that would notice, and it does not:
     * minimp3 walks it linearly.
     */
    rad_decoder_t *d = (rad_decoder_t *)malloc(sizeof(rad_decoder_t));
    if (!d) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    d->win = (uint8_t *)malloc(WINDOW_BYTES);
    mp3dec_init(&d->dec);

    a->dec      = d;
    a->frame    = (int16_t *)malloc(sizeof(int16_t) * MINIMP3_MAX_SAMPLES_PER_FRAME);
    a->pcm      = (int16_t *)malloc(sizeof(int16_t) * PCM_FRAMES);
    a->pcm_size = PCM_FRAMES;
    a->volume   = -1;

    if (!d->win || !a->frame || !a->pcm) {
        rad_audio_free(a);
        return false;
    }

    a->open = neos_audio_open();
    rad_eq_reset(&a->eq);
    return a->open;
}

void rad_audio_free(rad_audio_t *a)
{
    if (a->dec) {
        free(((rad_decoder_t *)a->dec)->win);
        free(a->dec);
        a->dec = NULL;
    }
    free(a->frame);
    free(a->pcm);
    a->frame = NULL;
    a->pcm   = NULL;

    if (a->open) {
        neos_audio_close();
        a->open = false;
    }
}

/* Everything that is about *this* stream, and nothing that is about the app.
   Called when the station changes: the decoder's overlap belongs to the last
   station's last frame and would be a click at the start of the next. */
void rad_audio_reset(rad_audio_t *a)
{
    rad_decoder_t *d = (rad_decoder_t *)a->dec;
    if (d) {
        mp3dec_init(&d->dec);
        d->win_len = 0;
    }
    a->pcm_head   = a->pcm_tail = 0;
    a->started    = false;
    a->phase      = 0;
    a->have_last  = false;
    a->frame_rate = 0;
    a->frame_ch   = 0;
    rad_eq_reset(&a->eq);
}

void rad_audio_volume(rad_audio_t *a, int percent)
{
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    if (percent == a->volume) {
        return;
    }
    a->volume = percent;
    if (a->open) {
        /*
         * The codec's own level, rather than a multiply on every sample. It
         * is the same knob the rest of the system uses, NeOS puts it back
         * where it found it when the stream closes, and a gain applied in
         * the analogue part of the part is a gain that costs nothing and
         * loses no bits at the quiet end.
         */
        (void)neos_audio_gain((uint8_t)percent);
    }
}

/* ------------------------------------------------------------------ */
/* The PCM ring                                                        */
/* ------------------------------------------------------------------ */

static inline uint32_t pcm_used(const rad_audio_t *a)
{
    return (a->pcm_head >= a->pcm_tail) ? (a->pcm_head - a->pcm_tail)
                                        : (a->pcm_size - a->pcm_tail + a->pcm_head);
}

static inline uint32_t pcm_room(const rad_audio_t *a)
{
    return a->pcm_size - pcm_used(a) - 1;
}

static inline void pcm_push(rad_audio_t *a, int16_t sample)
{
    a->pcm[a->pcm_head] = sample;
    a->pcm_head = (a->pcm_head + 1) % a->pcm_size;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/*
 * Fold to mono and resample, in one pass.
 *
 * Linear interpolation, with the position held as 16.16 in source frames.
 * That is a choice worth defending: a proper polyphase filter would be
 * better, and for 44100 to 48000 - a ratio of 160 to 147 - linear
 * interpolation's error lands as a gentle roll-off at the very top of the
 * band plus images far above anything a 128 kbit stream carries any energy
 * at. On a tablet speaker, against a lossy stream, it is inaudible, and the
 * whole thing is two multiplies a sample.
 *
 * Mono because the codec takes mono: NEOS_AUDIO_RATE frames are single
 * channel, so the fold is not a loss of anything the speaker could have
 * played.
 */
static void resample_in(rad_audio_t *a, const int16_t *pcm, int frames, int channels)
{
    /*
     * Deliberately 32-bit throughout. The widest this gets is 48000 << 16,
     * which is 3.1 billion and fits; doing it in 64 would pull in __udivdi3,
     * and the loader's table carries the double helpers it carries and no
     * long-long ones - so a 64-bit divide here is not a slower resampler, it
     * is an app that will not load. abi_check.py is what says so.
     */
    const uint32_t step = ((uint32_t)a->frame_rate << 16) /
                          (uint32_t)NEOS_AUDIO_RATE;

    for (int i = 0; i < frames; i++) {
        const int32_t l = pcm[i * channels];
        const int32_t r = (channels > 1) ? pcm[i * channels + 1] : l;
        const int16_t mono = (int16_t)((l + r) / 2);

        if (!a->have_last) {
            /*
             * Prime, and emit nothing yet. An interpolator needs two samples
             * to stand between, so the first input only becomes `last` - and
             * the phase is put a whole frame ahead so the loop below skips
             * this one. Starting at zero instead would emit the first sample
             * twice, which is inaudible but makes a matched rate not quite a
             * pass-through, and "48 kHz in is the same 48 kHz out" is the
             * kind of property worth being able to state without an asterisk.
             */
            a->last[0]   = mono;
            a->have_last = true;
            a->phase     = 0x10000u;
        }

        /*
         * Emit every output sample whose position falls inside this input
         * step. phase is measured from the previous input sample, so while it
         * is under one whole source frame there is another output due between
         * the two we are holding.
         */
        while (a->phase < 0x10000u) {
            if (pcm_room(a) == 0) {
                /* Nowhere to put it. The reservoir is full, which means the
                   speaker is a long way behind the decoder - so stop here and
                   let the pump catch up; the byte ring holds the rest. */
                return;
            }
            const int32_t frac = (int32_t)a->phase;
            const int32_t out  = ((int32_t)a->last[0] * (0x10000 - frac) +
                                  (int32_t)mono * frac) >> 16;
            pcm_push(a, (int16_t)out);
            a->phase += step;
        }
        a->phase -= 0x10000u;
        a->last[0] = mono;
    }
}

/* Decode what is in the byte ring, until the reservoir is full or the ring
   runs dry. Returns true if anything came out. */
static bool decode_some(rad_audio_t *a, rad_stream_t *s)
{
    rad_decoder_t *d = (rad_decoder_t *)a->dec;
    bool made = false;

    /*
     * A bound, so that a ring full of a stream minimp3 cannot make sense of
     * does not spin here for a whole frame's worth of wall clock. Eight
     * frames is about 200 ms of audio - an order more than one turn of the
     * loop hands over - so this never throttles the supply, it only caps what
     * one call can cost.
     */
    int budget = 8;

    while (budget-- > 0 && pcm_room(a) > MINIMP3_MAX_SAMPLES_PER_FRAME * 2) {
        if (d->win_len < WINDOW_BYTES) {
            d->win_len += (int)rad_ring_read(&s->ring, d->win + d->win_len,
                                             (uint32_t)(WINDOW_BYTES - d->win_len));
        }
        if (d->win_len < 1024 && rad_ring_used(&s->ring) == 0) {
            break;                      /* nothing to work with yet */
        }

        mp3dec_frame_info_t info;
        const int samples = mp3dec_decode_frame(&d->dec, d->win, d->win_len,
                                                a->frame, &info);

        if (info.frame_bytes == 0) {
            /*
             * Not enough bytes to be sure of a frame. minimp3 says so by
             * consuming nothing, and the answer is to wait for more rather
             * than to drop what is held - the next read will complete it.
             */
            break;
        }

        d->win_len -= info.frame_bytes;
        if (d->win_len > 0) {
            memmove(d->win, d->win + info.frame_bytes, (size_t)d->win_len);
        }

        if (samples <= 0) {
            continue;                   /* junk, or a tag, stepped over */
        }

        a->frame_rate = info.hz;
        a->frame_ch   = info.channels;
        s->rate_hz    = info.hz;
        s->bitrate_kbps = info.bitrate_kbps;

        resample_in(a, a->frame, samples, info.channels);
        made = true;
    }
    return made;
}

/* ------------------------------------------------------------------ */
/* Handing it over                                                     */
/* ------------------------------------------------------------------ */

int rad_audio_pump(rad_audio_t *a, rad_stream_t *s)
{
    if (!a->open) {
        return 0;
    }

    /*
     * Nothing plays while the stream is still filling its first buffer, and
     * nothing plays when there is no stream: a stopped radio hands the speaker
     * silence by handing it nothing at all.
     *
     * But a reconnect is not a stop. rad_stream.c deliberately keeps the ring
     * across an attempt, so the seconds of sound that already arrived are
     * still there while the socket is being redialled - and playing them is
     * the whole reason they were kept. So resolving and connecting feed too,
     * and only `buffering`, which is true just once per station, holds the
     * speaker back.
     */
    const bool live = s->state == RAD_PLAYING   || s->state == RAD_RETRYING ||
                      s->state == RAD_RESOLVING || s->state == RAD_CONNECTING;
    const bool feed = live && !s->buffering;

    /* And the bytes already in the ring are still worth decoding, for the
       same reason. */
    if (live) {
        (void)decode_some(a, s);
    }

    if (!feed) {
        return (int)(pcm_used(a) * 1000u / (uint32_t)NEOS_AUDIO_RATE);
    }

    if (!a->started) {
        if (pcm_used(a) < PCM_RESTART) {
            return (int)(pcm_used(a) * 1000u / (uint32_t)NEOS_AUDIO_RATE);
        }
        a->started = true;
    }

    static int16_t block[BLOCK_FRAMES];

    /*
     * While there is room ahead of the speaker, and frames to put in it. The
     * lead is asked *before* every block rather than once, because a block
     * that took longer to prepare than it plays would otherwise let the
     * loop hand over several in a row and sit in the codec for all of them.
     */
    /*
     * Three blocks is the whole cushion, so this can never be inside the
     * codec for longer than the cushion is deep - and in the steady state it
     * hands over one block and finds the lead satisfied.
     */
    int guard = 4;
    while (guard-- > 0 && neos_audio_lead_us() < LEAD_TARGET_US) {
        if (pcm_used(a) < BLOCK_FRAMES) {
            /*
             * Run dry. Nothing is written - a short block would be a click,
             * and the codec playing out its own tail is a cleaner silence -
             * and the restart threshold above makes the refill wait until
             * there is enough to be going on with.
             */
            a->started = false;
            a->underruns++;
            break;
        }

        for (int i = 0; i < BLOCK_FRAMES; i++) {
            block[i] = a->pcm[a->pcm_tail];
            a->pcm_tail = (a->pcm_tail + 1) % a->pcm_size;
        }

        rad_eq_run(&a->eq, block, BLOCK_FRAMES);

        if (neos_audio_write(block, BLOCK_FRAMES) < 0) {
            a->open = false;
            break;
        }
    }

    return (int)(pcm_used(a) * 1000u / (uint32_t)NEOS_AUDIO_RATE);
}
