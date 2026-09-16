/*
 * minimp3, compiled.
 *
 * The decoder is a single public-domain header (CC0; see its own banner) and
 * it lives in its own translation unit for one reason: everything else in
 * this app is built -Werror=double-promotion, and that flag is about code we
 * wrote. minimp3 is fixed in its habits, carries its own float discipline,
 * and calls nothing from libm - which is what made it the right decoder for
 * an app that links -nostdlib against a syscall table. Holding somebody
 * else's source to our warning is how a vendored file ends up quietly edited.
 *
 * What it does have to satisfy is the loader: the only symbols it leaves
 * undefined are memcpy and memset, both of which the ELF loader's table
 * exports. There is no malloc in the decode path at all - the caller owns the
 * state - which is why the one allocation for it is in rad_audio.c.
 */

/*
 * The one change made to the vendored file, and it is not a preference.
 *
 * mp3dec_decode_frame() declares its scratch space as an ordinary local:
 * 3.4 KB of bit reservoir, 4.6 KB of granule buffers and 8.4 KB of synthesis
 * state, about seventeen kilobytes in one stack frame. On a desktop that is
 * nothing. Here an app runs on NeOS's own main task, which is eight
 * kilobytes - so the first frame this decoder ever touched took the tablet
 * down with a stack protection fault, and NeOS quarantined the app, which is
 * exactly what that machinery is for.
 *
 * MINIMP3_SCRATCH_STATIC moves it into .bss, which for a loaded app is PSRAM
 * and there is 32 MB of it. Safe because the decoder is entered from one task
 * and one place - rad_audio.c's decode_some() - and it stays safe if that
 * ever moves to a service, since it would still be one task.
 *
 * The proper fix is the other one: neos_service_start() takes a stack size,
 * so a decode loop that asked for 24 KB would not need this. That is a change
 * worth making for its own reasons and this is not a substitute for it - but
 * it is one line, and it is what makes the difference between an app that
 * plays and an app that panics.
 */
#define MINIMP3_SCRATCH_STATIC

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include "minimp3.h"
