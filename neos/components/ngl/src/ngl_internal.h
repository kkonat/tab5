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
