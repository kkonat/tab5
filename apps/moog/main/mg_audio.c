/*
 * The audio thread. See mg_audio.h for why it is a thread and how the patch
 * crosses into it.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "mg_audio.h"

#include "neos_api.h"
#include "neos_sys.h"

/* How loud the codec is asked to be. A ladder at full emphasis is a good deal
   hotter than music, so this sits below where a player would put it. */
#define CODEC_PERCENT 60

/* Six kilobytes. The voice keeps its state in the block below and its working
   set in registers; what this has to cover is the syscall path into the codec
   driver, plus the patch copy the seqlock makes on the stack. */
#define STACK_BYTES 6144

#define BLOCK_US ((uint32_t)((MG_BLOCK * 1000000) / NEOS_AUDIO_RATE))

/* The first writes go into an empty DMA and the lead reads zero because it
   genuinely is zero. Counting those would put a number on screen saying the
   app failed at the one thing it measures, every time it starts. */
#define WARMUP_BLOCKS 8

/* How long the overload lamp is held after the last block that clipped. Short
   enough to follow playing, long enough that a single block is visible. */
#define OVERLOAD_HOLD_MS 220

static bool      s_open;
static bool      s_running;
static pthread_t s_thread;
static bool      s_have_thread;

static volatile bool s_quit;

/* --- the patch, under a seqlock --- */
static mg_patch_t        s_shadow;
static volatile uint32_t s_seq;

/* --- what a key is doing: two words, one writer --- */
static volatile float s_note = 60.0f;
static volatile bool  s_gate;

/* --- what comes back --- */
static volatile float    s_mod;
static volatile uint32_t s_overload_at;
static volatile uint32_t s_blocks, s_underruns;
static volatile uint32_t s_gen_us, s_gen_us_max, s_lead_us, s_load_pm;
static uint32_t          s_load_acc;

static int16_t    s_pcm[MG_BLOCK];
static mg_voice_t s_voice;

/* ------------------------------------------------------------------ */

void mg_audio_patch(const mg_patch_t *p)
{
    s_seq++;                 /* odd: a write is in progress */
    s_shadow = *p;
    s_seq++;                 /* even: it is whole again     */
}

void mg_audio_perf(float note, bool gate)
{
    s_note = note;
    s_gate = gate;
}

/*
 * Take a consistent copy, or say that this block should reuse the last one.
 *
 * Four attempts and then give up: the writer is a drawing loop that holds the
 * lock for the length of one struct copy, so two attempts is already generous,
 * and an audio thread that could spin here waiting for a UI is exactly the
 * dependency this app is built not to have.
 */
static bool patch_read(mg_patch_t *out)
{
    for (int try = 0; try < 4; try++) {
        const uint32_t a = s_seq;
        if (a & 1u) {
            continue;
        }
        *out = s_shadow;
        if (s_seq == a) {
            return true;
        }
    }
    return false;
}

static void *audio_thread(void *arg)
{
    (void)arg;

    mg_voice_init(&s_voice, (float)NEOS_AUDIO_RATE);

    mg_patch_t patch;
    memset(&patch, 0, sizeof patch);
    bool have_patch = patch_read(&patch);

    while (!s_quit) {
        if (patch_read(&patch)) {
            have_patch = true;
        }
        if (!have_patch) {
            /* Nothing has been published yet, which only happens between the
               thread starting and the panel's first send. Silence is the only
               honest thing to play. */
            memset(s_pcm, 0, sizeof s_pcm);
            if (neos_audio_write(s_pcm, MG_BLOCK) < 0) {
                break;
            }
            continue;
        }

        mg_perf_t perf;
        perf.note = s_note;
        perf.gate = s_gate;

        const uint32_t t0 = (uint32_t)neos_uptime_us();
        mg_voice_render(&s_voice, &patch, &perf, s_pcm, MG_BLOCK);
        const uint32_t gen = (uint32_t)neos_uptime_us() - t0;

        s_mod = s_voice.mod_out;
        if (s_voice.overload) {
            s_overload_at = (uint32_t)neos_uptime_ms();
        }

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
        /* The mean times sixteen, kept that way rather than divided down each
           round: at a load of three per cent a per-round divide truncates the
           whole increment to zero, and the average of a series of zeroes is a
           load figure that reads nought however long the app runs. */
        s_load_acc = s_load_acc - (s_load_acc / 16u) + (gen * 1000u / BLOCK_US);
        s_load_pm  = s_load_acc / 16u;

        if (lead == 0 && n >= WARMUP_BLOCKS) {
            s_underruns = s_underruns + 1;
        }

        /* Blocking, and that is the clock: the voice runs far faster than real
           time, so this is where the thread spends almost all of its life -
           asleep, with the core handed back to whatever is drawing. */
        if (neos_audio_write(s_pcm, MG_BLOCK) < 0) {
            break;
        }
        s_blocks = n + 1;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */

bool mg_audio_start(void)
{
    if (s_running) {
        return true;
    }
    if (!neos_audio_open()) {
        printf("[moog] no codec - the panel works, the speaker does not\n");
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
        printf("[moog] no thread for the voice (%d)\n", rc);
        neos_audio_close();
        s_open = false;
        return false;
    }
    s_have_thread = true;
    s_running     = true;
    printf("[moog] voice up: %d Hz, %d-frame blocks (%u us of sound each)\n",
           NEOS_AUDIO_RATE, MG_BLOCK, (unsigned)BLOCK_US);
    return true;
}

void mg_audio_stop(void)
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

bool mg_audio_running(void) { return s_running; }

float mg_audio_mod(void) { return s_mod; }

bool mg_audio_overload(void)
{
    const uint32_t at = s_overload_at;
    if (at == 0) {
        return false;
    }
    return ((uint32_t)neos_uptime_ms() - at) < OVERLOAD_HOLD_MS;
}

void mg_audio_meters(mg_meters_t *out)
{
    out->blocks     = s_blocks;
    out->underruns  = s_underruns;
    out->gen_us     = s_gen_us;
    out->gen_us_max = s_gen_us_max;
    out->lead_us    = s_lead_us;
    out->load_pm    = s_load_pm;
}
