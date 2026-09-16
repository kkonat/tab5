/*
 * The audio loop, and why the panel cannot starve it.
 *
 * A thread of its own, at a priority above the drawing loop: an app on NeOS
 * runs on the boot task at priority 1 and a pthread comes up at 5, so the
 * scheduler takes the core away from whatever is repainting the instant the
 * codec has room and gives it back when the block has been handed over.
 * apps/synth1 established that and the reasoning is written out in full in
 * apps/synth1/main/syn_audio.h; nothing about it changes because the voice got
 * twenty times larger, which is rather the point of having settled it first.
 *
 * ---------------------------------------------------- handing over the patch
 *
 * A Minimoog's patch is thirty-odd fields and no longer fits the "every field
 * is one word, so nothing can be read half-written" argument that a handful of
 * knobs got by on. A struct copied while it is being written is a struct that
 * can hold the old oscillator with the new filter.
 *
 * With continuous parameters that would be inaudible - they are smoothed
 * anyway, and both halves are settings the player asked for within five
 * milliseconds of each other. With the discrete ones it is not: a waveform
 * index or a range index read from a torn write is a value that was never
 * chosen at all, and RANGE_N would index off the end of a table.
 *
 * So the patch goes across under a seqlock, which costs the writer two
 * increments and the reader a compare. The reader retries rather than blocks,
 * and after a few retries gives up and renders the block against the patch it
 * already had - a course the writer cannot stall, because the worst case is
 * one more block of the previous sound.
 */
#ifndef MG_AUDIO_H
#define MG_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "mg_dsp.h"

/*
 * One handover: 240 frames is 5 ms and exactly one of the I2S DMA's six
 * descriptors, so the cushion is 30 ms deep and this is the granularity at
 * which a knob takes effect.
 */
#define MG_BLOCK 240

typedef struct {
    uint32_t blocks;
    uint32_t underruns;   /**< blocks that arrived with the speaker dry   */
    uint32_t gen_us;      /**< the last block: what generating it cost    */
    uint32_t gen_us_max;
    uint32_t lead_us;
    uint32_t load_pm;     /**< generation as per mille of real time       */
} mg_meters_t;

bool mg_audio_start(void);
void mg_audio_stop(void);
bool mg_audio_running(void);

/** Hand the thread a new patch. Returns immediately; see the header. */
void mg_audio_patch(const mg_patch_t *p);

/** Which note is held and whether anything is. Two words, written far more
    often than the patch and needing none of its machinery. */
void mg_audio_perf(float note, bool gate);

void mg_audio_meters(mg_meters_t *out);

/** The modulation source as the voice last left it, -1..1, for the panel. */
float mg_audio_mod(void);

/** True while the mixer is running past full scale - the overload lamp.
    Latched for long enough to be seen, since a peak that lasts one block
    would light a lamp for five milliseconds. */
bool mg_audio_overload(void);

#endif /* MG_AUDIO_H */
