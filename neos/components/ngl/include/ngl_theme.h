/*
 * NeOS system theme.
 *
 * Phosphor-green on black. Apps should draw from this palette so the system
 * looks like one machine rather than a pile of unrelated programs.
 *
 * These are compile-time constants, not exported symbols - an app picks them
 * up by including this header, so the palette costs nothing in the syscall
 * table. The flip side is that a theme change needs apps rebuilt; that is the
 * right trade while the palette is this small and this stable.
 */
#pragma once

#include "ngl.h"

/* --- ground ------------------------------------------------------- */
#define TH_BG          NGL_RGB(0,   0,   0)     /* true black, the CRT is off  */
#define TH_PANEL       NGL_RGB(6,   16,  6)     /* card / panel fill           */
#define TH_EDGE        NGL_RGB(34,  92,  34)    /* panel outline               */
#define TH_RULE        NGL_RGB(22,  60,  22)    /* hairline separators         */

/* --- text --------------------------------------------------------- */
#define TH_TEXT        NGL_RGB(140, 255, 90)    /* primary, limey phosphor     */
#define TH_TEXT_DIM    NGL_RGB(70,  150, 55)    /* secondary / captions        */
#define TH_TEXT_FAINT  NGL_RGB(40,  92,  35)    /* disabled                    */

/* --- accents ------------------------------------------------------ */
#define TH_ACCENT      NGL_RGB(0,   255, 65)    /* the classic terminal green  */
#define TH_GLOW        NGL_RGB(190, 255, 160)   /* highlights, hot text        */

/* --- icons -------------------------------------------------------- */
/* Status icons sit a shade below body text: the glyph carries the meaning
   (smile / dizzy / bomb), so colour does not have to, and the palette stays
   monochrome green. */
#define TH_ICON        NGL_RGB(58,  140, 50)

/* --- status ------------------------------------------------------- */
/* Kept green-family where possible so status reads as part of the theme,
   with hue - not just brightness - carrying the meaning. */
#define TH_OK          NGL_RGB(0,   230, 70)
#define TH_WARN        NGL_RGB(210, 220, 40)
#define TH_BAD         NGL_RGB(220, 70,  50)

/* --- system bar --------------------------------------------------- */
#define TH_BAR_BG      NGL_RGB(3,   10,  3)
#define TH_BAR_EDGE    NGL_RGB(28,  76,  28)
#define TH_CLOSE_FILL  NGL_RGB(8,   48,  12)    /* dark green                  */
#define TH_CLOSE_LINE  NGL_RGB(70,  180, 70)    /* rounded-rect outline        */
#define TH_CLOSE_X     NGL_RGB(160, 255, 130)   /* light green cross           */

/* --- modal panels ------------------------------------------------- */
/* A panel sits in front of the app rather than instead of it, so what is
   behind it is dimmed to TH_SCRIM rather than painted over. The panel body is
   a shade lighter than TH_PANEL so the two read as different depths when one
   is on top of the other - the Wi-Fi list over the keyboard, say. */
#define TH_SCRIM       200                      /* how far to darken, 0-255    */
#define TH_MODAL_BG    NGL_RGB(4,   12,  4)
#define TH_MODAL_EDGE  NGL_RGB(44,  118, 44)

/* --- keys --------------------------------------------------------- */
/* Three states, three weights. A plain key is an outline, a held modifier is
   filled, and a key under a finger is filled brighter still - so which keys
   are latched is legible at a glance without reading any of the legends. */
#define TH_KEY_FILL    NGL_RGB(10,  26,  10)
#define TH_KEY_EDGE    NGL_RGB(30,  84,  30)
#define TH_KEY_TEXT    NGL_RGB(150, 255, 110)
#define TH_KEY_DOWN    NGL_RGB(0,   120, 40)    /* under a finger              */
#define TH_KEY_LATCH   NGL_RGB(12,  64,  20)    /* shift/ctrl/caps held or on  */
#define TH_KEY_MOD     NGL_RGB(8,   20,  10)    /* modifiers and space, at rest */
