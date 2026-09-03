/*
 * Tap sounds, core side. The switch that turns them on is in neos_sys.h.
 *
 * The only thing NeOS plays. There is no audio API for apps yet and this is
 * deliberately not the start of one - it is one waveform, generated at
 * bring-up, played from one task, and the whole module exists so that a touch
 * on the glass makes the noise a touch should make.
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
