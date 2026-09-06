/*
 * Private to the ngl component. Not on the include path for main/ or for apps,
 * which is the point: anything declared here is free to change without an ABI
 * bump, because nothing outside src/ can be compiled against it.
 */
#pragma once

#include "ngl.h"

#ifndef NGL_INTERNAL
#error "ngl_internal.h is for the ngl component's own sources"
#endif

/**
 * Set up a caller-provided surface in place.
 *
 * The public ngl_surface_wrap() allocates a handle because apps cannot declare
 * an opaque struct; the screen's back buffer is a file-static here and does not
 * need one, so it uses this instead.
 */
void ngl_surface_init(ngl_surface_t *s, ngl_color_t *px, int16_t w, int16_t h, int16_t stride);

/**
 * True when a modal overlay owns the screen and @p s is it, but the calling
 * task is not the one that took it.
 *
 * Every function that writes a pixel asks this first. The check is per task
 * rather than per surface because the overlay and the app draw into the same
 * back buffer through the same handle - what separates them is who is asking,
 * not what they are asking about.
 */
bool ngl_screen_blocked(const ngl_surface_t *s);

/**
 * Scale `sr` of `src` into `dr` of `dst` with the P4's pixel engine.
 *
 * False when it could not be done at all - no engine, a buffer whose rows do
 * not sit on cache lines, a factor outside what the hardware scales - and then
 * nothing has been written and the caller does the loop itself. Both rects are
 * already clipped by the time this is called.
 *
 * Here rather than in ngl_draw.c because the PPA client belongs to the screen:
 * one registration, shared with the rotation in the flush path.
 */
bool ngl_ppa_scale(ngl_surface_t *dst, ngl_rect_t dr,
                   const ngl_surface_t *src, ngl_rect_t sr);
