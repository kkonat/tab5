/*
 * Enough of NeOS to link the panel and the voice on a desktop.
 *
 * ngl itself is not stubbed - ngl_draw.c, ngl_text.c and ngl_font_data.c are
 * compiled unmodified, the same sources that go on the tablet - so the
 * pictures these tests compare are the pixels the panel would get. See
 * apps/synth1/test/stubs.c, which this follows; the turn is tested there and
 * is the same code.
 */
#define NGL_INTERNAL 1

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ngl.h"
#include "ngl_internal.h"

#include "mg_audio.h"

/* --- the screen, which there is not one of --- */

ngl_surface_t *ngl_screen(void)                           { return NULL; }
void           ngl_dirty(const ngl_rect_t *r)             { (void)r; }
void           ngl_dirty_all(void)                        { }
bool           ngl_screen_blocked(const ngl_surface_t *s) { (void)s; return false; }
void           ngl_bar_paint(void)                        { }
ngl_rect_t     ngl_app_area(void)   { return ngl_rect(0, 0, 0, 0); }
int16_t        ngl_bar_height(void) { return 0; }

bool ngl_ppa_scale(ngl_surface_t *dst, ngl_rect_t dr,
                   const ngl_surface_t *src, ngl_rect_t sr)
{
    (void)dst; (void)dr; (void)src; (void)sr;
    return false;
}

/* --- the panel --- */

#define PANEL_W  720
#define PANEL_H 1280

void ngl_panel_size(int16_t *w, int16_t *h)
{
    if (w) { *w = PANEL_W; }
    if (h) { *h = PANEL_H; }
}

/*
 * Every rectangle the app offers the glass, and whether the device would take
 * it. ngl_panel_scale() clips to nothing rather than clamping - a caller that
 * got the rectangle wrong is told so rather than shown most of its picture -
 * so one that falls outside the panel is a widget that silently stops
 * updating, which is exactly the bug a test can see and an eye cannot.
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

uint64_t neos_uptime_us(void) { return g_now_us += 17; }
uint64_t neos_uptime_ms(void) { return g_now_us / 1000; }

/* --- the voice, as far as the panel is concerned --- */

static mg_meters_t g_meters;
static bool        g_overload;

void stub_set_meters(const mg_meters_t *m) { g_meters = *m; }
void stub_set_overload(bool on)            { g_overload = on; }

void  mg_audio_meters(mg_meters_t *out) { *out = g_meters; }
bool  mg_audio_overload(void)           { return g_overload; }
float mg_audio_mod(void)                { return 0.0f; }

bool mg_audio_start(void)                  { return true; }
void mg_audio_stop(void)                   { }
bool mg_audio_running(void)                { return true; }
void mg_audio_patch(const mg_patch_t *p)   { (void)p; }
void mg_audio_perf(float note, bool gate)  { (void)note; (void)gate; }
