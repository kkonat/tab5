/*
 * The audio thread. See syn_audio.h for why it is a thread.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "syn_audio.h"

#include "neos_api.h"
#include "neos_sys.h"

/* How loud the codec is asked to be. A raw sawtooth at full level is a good
   deal hotter than music, so this sits below where a player would put it. */
#define CODEC_PERCENT 60

/* Four kilobytes. The voice keeps its state in the block below and its working
   set in registers; what this really has to cover is the syscall path into the
   codec driver. */
#define STACK_BYTES 4096

/* One block of real time, in microseconds - the divisor every load figure
   below is against. */
#define BLOCK_US ((uint32_t)((SYN_BLOCK * 1000000) / NEOS_AUDIO_RATE))

/*
 * How long the loop is given to settle before an empty cushion is believed.
 *
 * The first writes go into an empty DMA and the lead reads zero because it
 * genuinely is zero - the speaker has not been given anything yet. Counting
 * those as underruns would put a number on screen that says the app failed at
 * the one thing it is measuring, every single time it starts.
 */
#define WARMUP_BLOCKS 8

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static bool      s_open;
static bool      s_running;
static pthread_t s_thread;
static bool      s_have_thread;

static volatile bool s_quit;

/* The patch, written field by field by the UI and read field by field here.
   See the header: one word each, one writer, no lock. */
static volatile float s_freq     = 110.0f;
static volatile float s_amp      = 0.0f;
static volatile float s_lfo_rate = 5.0f;
static volatile float s_lfo_amt  = 0.0f;
static volatile int   s_lfo_shape = SYN_TRI;

static volatile float s_lfo_out;

static volatile uint32_t s_blocks, s_underruns;
static volatile uint32_t s_gen_us, s_gen_us_max, s_lead_us, s_load_pm;

/* The mean above, times sixteen, and the audio thread's alone. */
static uint32_t s_load_acc;

static int16_t s_pcm[SYN_BLOCK];

static int16_t       s_cap[3][SYN_SCOPE_N];
static volatile int  s_cap_ready = -1;
static int           s_cap_w;      /* the buffer being filled */
static int           s_cap_n;      /* how much of it is filled */

static syn_voice_t s_voice;

/* ------------------------------------------------------------------ */
/* The loop                                                            */
/* ------------------------------------------------------------------ */

static void capture(const int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) {
        s_cap[s_cap_w][s_cap_n++] = pcm[i];
        if (s_cap_n >= SYN_SCOPE_N) {
            s_cap_ready = s_cap_w;
            s_cap_w     = (s_cap_w + 1) % 3;
            s_cap_n     = 0;
        }
    }
}

static void *audio_thread(void *arg)
{
    (void)arg;

    syn_voice_init(&s_voice, (float)NEOS_AUDIO_RATE);

    while (!s_quit) {
        syn_patch_t p;
        p.freq      = s_freq;
        p.amp       = s_amp;
        p.lfo_rate  = s_lfo_rate;
        p.lfo_amt   = s_lfo_amt;
        p.lfo_shape = s_lfo_shape;

        const uint32_t t0 = (uint32_t)neos_uptime_us();
        syn_voice_render(&s_voice, &p, s_pcm, SYN_BLOCK);
        const uint32_t gen = (uint32_t)neos_uptime_us() - t0;

        capture(s_pcm, SYN_BLOCK);
        s_lfo_out = s_voice.lfo_out;

        /*
         * Asked before the write and not after, because after the write the
         * cushion is always full by definition - the call does not return
         * until the codec has taken the block. What is worth knowing is how
         * close the previous block came to running out, and this is that.
         */
        const uint32_t lead = (uint32_t)neos_audio_lead_us();
        const uint32_t n    = s_blocks;

        s_gen_us  = gen;
        s_lead_us = lead;
        if (gen > s_gen_us_max) {
            s_gen_us_max = gen;
        }
        /*
         * An exponential mean over sixteen blocks, so the figure on screen is
         * readable rather than flickering through every block's rounding.
         *
         * The accumulator is the mean times sixteen and is kept that way
         * rather than divided down each round: at a load of three per cent a
         * per-round divide by sixteen truncates the whole increment to zero,
         * and the average of a series of zeroes is a load figure that reads
         * nought however long the app runs.
         */
        s_load_acc = s_load_acc - (s_load_acc / 16u) + (gen * 1000u / BLOCK_US);
        s_load_pm  = s_load_acc / 16u;

        if (lead == 0 && n >= WARMUP_BLOCKS) {
            s_underruns = s_underruns + 1;
        }

        /*
         * Blocking, and that is the clock. The generator runs far faster than
         * real time, so this is where the thread spends almost all of its
         * life - asleep, with the core handed back to whatever is drawing.
         */
        if (neos_audio_write(s_pcm, SYN_BLOCK) < 0) {
            break;
        }
        s_blocks = n + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* The interface                                                       */
/* ------------------------------------------------------------------ */

bool syn_audio_start(void)
{
    if (s_running) {
        return true;
    }
    if (!neos_audio_open()) {
        printf("[synth1] no codec - the knobs work, the speaker does not\n");
        return false;
    }
    s_open = true;
    neos_audio_gain(CODEC_PERCENT);

    s_quit = false;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, STACK_BYTES);
    const int rc = pthread_create(&s_thread, &attr, audio_thread, NULL);

    if (rc != 0) {
        printf("[synth1] no thread for the voice (%d)\n", rc);
        neos_audio_close();
        s_open = false;
        return false;
    }
    s_have_thread = true;
    s_running     = true;
    printf("[synth1] voice up: %d Hz, %d-frame blocks (%u us of sound each)\n",
           NEOS_AUDIO_RATE, SYN_BLOCK, (unsigned)BLOCK_US);
    return true;
}

void syn_audio_stop(void)
{
    s_quit = true;
    if (s_have_thread) {
        pthread_join(s_thread, NULL);
        s_have_thread = false;
    }
    if (s_open) {
        neos_audio_close();
        s_open = false;
    }
    s_running = false;
}

bool syn_audio_running(void)
{
    return s_running;
}

void syn_audio_set(const syn_patch_t *p)
{
    s_freq      = p->freq;
    s_amp       = p->amp;
    s_lfo_rate  = p->lfo_rate;
    s_lfo_amt   = p->lfo_amt;
    s_lfo_shape = p->lfo_shape;
}

float syn_audio_lfo(void)
{
    return s_lfo_out;
}

void syn_audio_meters(syn_meters_t *out)
{
    out->blocks     = s_blocks;
    out->underruns  = s_underruns;
    out->gen_us     = s_gen_us;
    out->gen_us_max = s_gen_us_max;
    out->lead_us    = s_lead_us;
    out->load_pm    = s_load_pm;
}

bool syn_audio_scope(int16_t *dst)
{
    const int i = s_cap_ready;
    if (i < 0) {
        return false;
    }
    memcpy(dst, s_cap[i], sizeof(int16_t) * SYN_SCOPE_N);
    return true;
}
