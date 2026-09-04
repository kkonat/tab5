/*
 * The speaker, core side. What apps see of it is in neos_sys.h.
 *
 * Two things want this one codec: the tap click, which is NeOS's own and is
 * one waveform played from one task, and an app's stream, which is whatever
 * the app is generating. Either can bring the part up and it stays up while
 * either still wants it; while an app holds the stream the click is dropped,
 * because two writers into one codec interleave into each other's buffers.
 *
 * The ES8388 and the I2S plumbing come from the BSP - nothing here talks to
 * the part directly.
 */
#pragma once

#include <stdbool.h>

/**
 * Restore the saved setting and, if it was on, bring the codec up.
 *
 * Called once during bring-up, after I2C and after the settings store. Does
 * nothing at all when tap sounds have never been switched on, which is the
 * usual case - the ES8388 stays unconfigured and the speaker rail stays down.
 */
void neos_audio_init(void);

/**
 * Play one click.
 *
 * Called from the touch task on every press edge, so it must not block and
 * must not care that it is called again before the last one finished. Both are
 * true: it posts to the player task and returns, and a click that arrives
 * while one is playing is dropped rather than queued - a queue would turn fast
 * typing into a burst of clicks arriving after the fingers had stopped.
 *
 * A no-op when tap sounds are off, which is the state that costs nothing.
 */
void neos_audio_click(void);

/**
 * Close an app's stream if it returned from main() still holding one.
 *
 * Called by the boot chain after every app, because there is no other moment
 * at which it could be noticed: an app exits by returning, so nothing runs on
 * its behalf afterwards, and a forgotten neos_audio_close() would otherwise
 * leave the amplifier powered and the launcher's clicks suppressed for the
 * rest of the session. Silent when no app held the stream, which is the
 * ordinary case.
 */
void neos_audio_app_release(void);
