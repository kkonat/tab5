/*
 * Enough of NeOS to link the app's own code on a desktop.
 *
 * ngl itself is NOT stubbed. ngl_draw.c, ngl_text.c and ngl_font_data.c are
 * compiled here unmodified, which is the whole point of the exercise: the
 * question these tests exist to answer is whether turn.c's turn puts a
 * pixel where ngl would have put it, and a stubbed ngl could only be asked
 * whether turn.c agrees with itself.
 *
 * What ngl expects the firmware to own is small - the screen surface, the
 * dirty bookkeeping, the overlay lock and the pixel engine - and none of it
 * has anything to say on a PC. ngl_screen() returning NULL is honest rather
 * than lazy: there is no screen here, so every `s == ngl_screen()` in
 * ngl_draw.c takes the "this is an ordinary surface" branch, which is what
 * every surface in these tests is.
 */
#define NGL_INTERNAL 1

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ngl.h"
#include "ngl_internal.h"

#include "syn_audio.h"

/* --- the screen, which there is not one of --- */

ngl_surface_t *ngl_screen(void)                     { return NULL; }
void           ngl_dirty(const ngl_rect_t *r)       { (void)r; }
void           ngl_dirty_all(void)                  { }
bool           ngl_screen_blocked(const ngl_surface_t *s) { (void)s; return false; }

ngl_rect_t ngl_app_area(void) { return ngl_rect(0, 0, 0, 0); }
int16_t    ngl_bar_height(void) { return 0; }

/* ngl_clear() repaints the reserved strip after wiping the screen. There is no
   screen here and therefore no strip, but the call is still made. */
void ngl_bar_paint(void) { }

/*
 * No pixel engine, so ngl_blit_scale() takes its software path - which is the
 * one that is worth exercising anyway, since it is the one whose arithmetic
 * can be wrong.
 */
bool ngl_ppa_scale(ngl_surface_t *dst, ngl_rect_t dr,
                   const ngl_surface_t *src, ngl_rect_t sr)
{
    (void)dst; (void)dr; (void)src; (void)sr;
    return false;
}

/* --- the panel --- */

/* The Tab5's, and the numbers the whole turn is arranged around. */
#define PANEL_W  720
#define PANEL_H 1280

void ngl_panel_size(int16_t *w, int16_t *h)
{
    if (w) { *w = PANEL_W; }
    if (h) { *h = PANEL_H; }
}

/*
 * The last rectangle turn_present() handed over, and whether it would have been
 * refused on the device.
 *
 * ngl_panel_scale() on the tablet clips to nothing rather than clamping - a
 * caller that got the rectangle wrong is told so rather than shown most of its
 * picture - so a rectangle that falls outside the panel is a bug that shows up
 * as a widget which silently stops updating. Recording it here is how a test
 * can see that happen.
 */
ngl_rect_t stub_last_present;
int        stub_present_calls;
int        stub_present_refused;

bool ngl_panel_scale(const ngl_surface_t *src, ngl_rect_t sr, ngl_rect_t dr,
                     bool mirror_x, bool mirror_y)
{
    (void)mirror_x; (void)mirror_y;

    stub_present_calls++;
    stub_last_present = dr;

    const bool ok = src && sr.w > 0 && sr.h > 0 && dr.w > 0 && dr.h > 0 &&
                    dr.x >= 0 && dr.y >= 0 &&
                    dr.x + dr.w <= PANEL_W && dr.y + dr.h <= PANEL_H &&
                    sr.x >= 0 && sr.y >= 0 &&
                    sr.x + sr.w <= ngl_surface_w(src) &&
                    sr.y + sr.h <= ngl_surface_h(src) &&
                    (16 * (int)dr.w) % (int)sr.w == 0 &&
                    (16 * (int)dr.h) % (int)sr.h == 0;
    if (!ok) {
        stub_present_refused++;
    }
    return ok;
}

void stub_present_reset(void)
{
    stub_present_calls   = 0;
    stub_present_refused = 0;
    stub_last_present    = ngl_rect(0, 0, 0, 0);
}

/* --- the clock --- */

static uint64_t g_now_us = 1000000;

void     stub_advance_us(uint64_t us) { g_now_us += us; }
uint64_t neos_uptime_us(void)         { return g_now_us += 17; }
uint64_t neos_uptime_ms(void)         { return g_now_us / 1000; }

/* --- the voice, as far as the panel is concerned --- */

static float        g_lfo;
static syn_meters_t g_meters;
static int16_t      g_scope[SYN_SCOPE_N];
static bool         g_have_scope;

void  stub_set_lfo(float v)  { g_lfo = v; }
float syn_audio_lfo(void)    { return g_lfo; }

void syn_audio_meters(syn_meters_t *out) { *out = g_meters; }
void stub_set_meters(const syn_meters_t *m) { g_meters = *m; }

void stub_set_scope(const int16_t *pcm, bool have)
{
    if (pcm) { memcpy(g_scope, pcm, sizeof g_scope); }
    g_have_scope = have;
}

bool syn_audio_scope(int16_t *dst)
{
    if (!g_have_scope) {
        return false;
    }
    memcpy(dst, g_scope, sizeof g_scope);
    return true;
}

bool syn_audio_start(void)                    { return true; }
void syn_audio_stop(void)                     { }
bool syn_audio_running(void)                  { return true; }
void syn_audio_set(const syn_patch_t *p)      { (void)p; }
