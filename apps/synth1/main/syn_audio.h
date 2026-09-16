/*
 * The audio loop, and the reason it cannot be starved by the screen.
 *
 * It is a thread of its own, and that is the whole design. An app on NeOS runs
 * on the boot task at priority 1; a pthread comes up at 5, above it and above
 * the services an app can start, so the scheduler takes the core away from the
 * drawing loop the instant the codec has room and gives it back when the block
 * has been handed over. Nothing in syn_ui.c has to be quick for the sound to
 * be continuous, and nothing in here has to yield politely for the glass to
 * stay responsive - which is the property being tested, and it is a property
 * of the priority rather than of anybody's good behaviour.
 *
 * The alternative shape - one loop that draws a frame, then tops the speaker
 * up, the way apps/radio does - is the right one for a player, because a
 * player's sound arrives from a socket on somebody else's schedule and the
 * loop has to be there to receive it. It is the wrong one here: this app
 * generates its own sound, so the only thing that can make it late is the
 * frame it happens to be in the middle of, and a full repaint of a 720x1280
 * canvas is longer than the codec's thirty-millisecond cushion. A synth that
 * clicks when you drag a knob is a synth that has failed at the only thing it
 * does.
 *
 * ------------------------------------------------------- the parameter block
 *
 * One writer, the UI; one reader, the thread. No lock, and the same reasoning
 * lab/defender's sound board gives: a word is written atomically on this core,
 * so no single field can be read half-updated, and the worst a race can do is
 * let one block hear last frame's pitch with this frame's level. A mutex here
 * would be a mutex the drawing loop could block on, which trades an inaudible
 * five-millisecond skew for a visible stall.
 */
#ifndef SYN_AUDIO_H
#define SYN_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "syn_dsp.h"

/*
 * One handover.
 *
 * 240 frames is 5 ms and is exactly one of the I2S DMA's six descriptors, so a
 * block either fits or the write blocks until it does - there is no partial
 * descriptor to reason about. The cushion is therefore 30 ms deep and this is
 * the granularity at which a knob takes effect.
 */
#define SYN_BLOCK 240

/*
 * How many frames the scope holds: 2048, which is 42.7 ms.
 *
 * Sized by the bottom of the pitch range rather than by the screen. The
 * display picks a window of it wide enough to hold about three periods of
 * whatever is playing, the way a scope's timebase follows its trigger, and
 * three periods of a low A is 109 ms - more than this, so the bottom octave
 * still shows a slice rather than a cycle. Every octave up from there is a
 * whole waveform, and 12 KB across the three buffers is not worth arguing
 * with.
 */
#define SYN_SCOPE_N 2048

/** What the loop has been doing. All of it is read by the UI and written by
    the audio thread, one word at a time, for the reason in the header. */
typedef struct {
    uint32_t blocks;      /**< handed to the codec since the app started   */
    uint32_t underruns;   /**< blocks that arrived with the speaker dry    */
    uint32_t gen_us;      /**< the last block: what generating it cost     */
    uint32_t gen_us_max;  /**< the worst since the app started             */
    uint32_t lead_us;     /**< the cushion measured before the last write  */
    uint32_t load_pm;     /**< generation as per mille of real time        */
} syn_meters_t;

/**
 * Take the speaker and start the thread.
 *
 * False if the codec would not come up or the thread could not be created. The
 * app is worth running either way - the knobs still move and the numbers still
 * mean something - so the caller is expected to carry on and say so on screen.
 */
bool syn_audio_start(void);

/** Stop the thread and give the speaker back. Safe if it never started. */
void syn_audio_stop(void);

bool syn_audio_running(void);

/** Hand the thread a new patch. Returns immediately; see the header. */
void syn_audio_set(const syn_patch_t *p);

/** The LFO's current output, -1..1, for the indicator. */
float syn_audio_lfo(void);

void syn_audio_meters(syn_meters_t *out);

/**
 * Copy the most recent SYN_SCOPE_N frames out, or return false if none have
 * been captured yet.
 *
 * Three buffers rotating, and the published one is not written again until two
 * more have filled - 85 ms, against a copy that takes microseconds. That is a
 * race in the strict sense and not one in any sense that matters; a seqlock
 * here would cost the audio thread a retry loop to protect a picture of a
 * waveform.
 */
bool syn_audio_scope(int16_t *dst);

#endif /* SYN_AUDIO_H */
