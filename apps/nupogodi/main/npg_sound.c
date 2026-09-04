/*
 * The piezo.
 *
 * A Game & Watch has no sound chip. It has one pin, R1, and a piezo disc
 * across it, and everything the thing has ever been heard to do is that pin
 * going up and down under program control. So there is nothing to synthesise
 * here and nothing to sample: the emulated pin is read once per 32768 Hz
 * clock tick, which is a one-bit recording of the real thing at the rate the
 * real thing was capable of changing.
 *
 * Which leaves two jobs. Getting it to 48 kHz, and stopping it being a DC
 * offset.
 *
 * The rate works out unusually well. 48000/32768 is exactly 375/256, so a
 * block of 256 ticks becomes 375 codec frames with nothing left over - the
 * resampler restarts from a known phase every block and can never drift,
 * which is the failure that would otherwise show up half an hour into a game
 * as a slow detune.
 *
 * The DC is the part that matters more than it sounds. The pin idles low, and
 * a tone is a square wave whose duty cycle is whatever the program felt like,
 * so mapping the bit straight onto a signed sample would push the speaker
 * cone off centre and hold it there - a thump on every silence and a
 * permanent loss of headroom. One pole of high-pass takes it out and costs
 * three integer operations a sample.
 *
 * Nothing is smoothed beyond the interpolation. The sound of this machine is
 * a hard square through a resonant disc, and filtering it into something
 * pleasant would be filtering out the thing people recognise.
 */
#include <string.h>

#include "nupogodi.h"
#include "neos_sys.h"

_Static_assert(NPG_BLOCK_TICKS * NEOS_AUDIO_RATE ==
               NPG_BLOCK_FRAMES * SM5A_CLOCK_HZ,
               "the block sizes must be an exact rate conversion, or it drifts");

/*
 * Loud, with room left over.
 *
 * A square wave at full scale clips into the codec's own headroom and comes
 * out as a rasp rather than a tone. This is about -11 dBFS, which the ES8388
 * and a tablet speaker turn into something that carries across a room, and
 * the rest is the volume control's business.
 */
#define AMP  9000

/* One pole, cutoff about 40 Hz at 48 kHz. Q15. */
#define DC_R 32440

static uint8_t s_bits[NPG_BLOCK_TICKS];
static int     s_n;

static int16_t s_pcm[NPG_BLOCK_FRAMES];

static int32_t s_dc_x1, s_dc_y1;
static bool    s_open;

/* ------------------------------------------------------------------ */

bool npg_sound_begin(void)
{
    s_n = 0;
    s_dc_x1 = s_dc_y1 = 0;
    memset(s_bits, 0, sizeof(s_bits));

    s_open = neos_audio_open();
    if (s_open) {
        neos_audio_gain(65);
    }
    return s_open;
}

void npg_sound_pin(uint8_t r_out)
{
    /*
     * Called 32768 times per second of emulated time and nowhere else. The
     * bound is not defensive - the caller runs exactly NPG_BLOCK_INSTR
     * instructions between flushes and each is SM5A_CLK_DIV ticks - it is so
     * that a miscount costs a dropped sample rather than the stack.
     */
    if (s_n < NPG_BLOCK_TICKS) {
        s_bits[s_n++] = r_out & 1;
    }
}

bool npg_sound_flush(void)
{
    const int have = s_n;
    s_n = 0;

    if (!s_open) {
        return false;
    }

    for (int n = 0; n < NPG_BLOCK_FRAMES; n++) {
        /*
         * Exact rational resampling: output n sits at input n*256/375, and
         * both the index and the fraction are integers because 256 and 375
         * are what they are. i+1 runs off the end on the last few frames and
         * holds the final bit instead of reaching into the next block, which
         * is a single sample of error at a boundary every 7.8 ms.
         */
        const int t    = n * NPG_BLOCK_TICKS;
        const int i    = t / NPG_BLOCK_FRAMES;
        const int frac = t % NPG_BLOCK_FRAMES;

        const int a = (i     < have) ? s_bits[i]     : 0;
        const int b = (i + 1 < have) ? s_bits[i + 1] : a;

        /* 0..NPG_BLOCK_FRAMES, scaled to 0..AMP. */
        const int32_t x =
            (int32_t)(a * (NPG_BLOCK_FRAMES - frac) + b * frac) * AMP / NPG_BLOCK_FRAMES;

        /* All int32: y is bounded by AMP, so AMP*DC_R stays well inside it,
           and a 64-bit multiply here would be a call into libgcc that an
           app linked -nostdlib has no reason to be making. */
        const int32_t y = x - s_dc_x1 + ((s_dc_y1 * DC_R) >> 15);
        s_dc_x1 = x;
        s_dc_y1 = y;

        s_pcm[n] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
    }

    if (neos_audio_write(s_pcm, NPG_BLOCK_FRAMES) < 0) {
        s_open = false;      /* the stream went away; the caller paces itself */
        return false;
    }
    return true;
}

void npg_sound_end(void)
{
    if (s_open) {
        neos_audio_close();
        s_open = false;
    }
}
