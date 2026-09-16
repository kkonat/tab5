/*
 * Widgets and layout, shared by the three pages.
 *
 * The pi-Q app drew against a fixed 800x480 with every coordinate a literal.
 * This one is handed ngl_app_area() and lays itself out inside it, which is
 * not tidiness for its own sake: the strip along the top of this panel
 * belongs to the firmware and its height is the firmware's business, so an
 * app with the bar's size compiled into it is an app that moves when the bar
 * does.
 *
 * There are two fonts on this machine rather than four scales, and they are
 * 16x32 and 24x48 - much larger, relatively, than the Pi's. That is what
 * drove the relayout: the station grid is three columns of the same nine
 * cells, but a value and its caption now share one box on the EQ page
 * because a caption above the box would have cost a row.
 */
#pragma once

#include "radio.h"
#include "ngl_theme.h"

/* The tab strip, at the top of the app area and above every page. */
#define UI_TAB_H     56
#define UI_TAB_W    180
#define UI_TAB_GAP    8
#define UI_MARGIN    24

#define UI_SLIDER_H  28

/** The area below the tab strip - what a page gets to itself. */
ngl_rect_t rad_ui_page(ngl_rect_t area);

void rad_ui_button(ngl_surface_t *sc, ngl_rect_t r, const char *label,
                   const ngl_font_t *font, bool held, bool on, bool enabled);
void rad_ui_slider(ngl_surface_t *sc, int16_t x, int16_t y, int16_t w,
                   int fraction_permille, bool active);
void rad_ui_text_mid(ngl_surface_t *sc, int16_t x, int16_t y, int16_t w,
                     const char *s, const ngl_font_t *font, ngl_color_t c);
/** Cut @p s down until it fits in @p w, in place. @p size is its buffer. */
void rad_ui_ellipsis(char *s, size_t size, const ngl_font_t *font, int16_t w);

/** 0..1000 of the way along a slider track, from a touch. */
int  rad_ui_slider_permille(int16_t x0, int16_t w, int16_t x);

/** The volume the bar would read at this x. The bar's geometry lives in
    rad_ui.c and nowhere else, so the loop asks rather than measuring. */
int  rad_ui_volume_from_x(int16_t x);

/** How many stations a page of the grid holds, for the loop's paging. */
int  rad_ui_per_page(void);

/** A cheap content hash, so a box is repainted only when its text changed. */
uint32_t rad_ui_hash(uint32_t seed, const void *data, size_t len);
uint32_t rad_ui_hash_str(uint32_t seed, const char *s);
