/*
 * The shared widgets. See wg.h for why they are shared.
 */
#include <math.h>

#include "wg.h"

#include "ngl_theme.h"

/* A knob's travel: seven-thirty round to four-thirty, the way a panel knob's
   end stops are drawn. Zero is straight up. */
#define ANG_MIN   (-2.3561945f)     /* -135 degrees */
#define ANG_SWEEP  4.712389f        /*  270 degrees */

#define NTICK 21

static void spoke(const turn_t *g, int16_t cx, int16_t cy, float ang,
                  float r0, float r1, ngl_color_t c)
{
    const float s = sinf(ang), co = cosf(ang);
    turn_line_aa(g, (int16_t)((float)cx + s * r0), (int16_t)((float)cy - co * r0),
                    (int16_t)((float)cx + s * r1), (int16_t)((float)cy - co * r1), c);
}

void wg_knob(const turn_t *g, int16_t cx, int16_t cy, int16_t r, float norm,
             int detents)
{
    const int n = (detents > 1) ? detents : NTICK;

    for (int t = 0; t < n; t++) {
        const float f   = (n > 1) ? (float)t / (float)(n - 1) : 0.0f;
        const float ang = ANG_MIN + ANG_SWEEP * f;
        /* A hair of slack, so the tick under the pointer lights at either end
           rather than falling foul of the rounding that put it there. */
        const bool lit = (f <= norm + 0.001f);
        spoke(g, cx, cy, ang, (float)(r + 6), (float)(r + 14),
              lit ? TH_ACCENT : TH_TEXT_FAINT);
    }

    turn_disc(g, cx, cy, r, TH_PANEL);
    turn_ring(g, cx, cy, r, r > 40 ? 3 : 2, TH_EDGE);

    /*
     * A selector's pointer starts further out, because the middle of its face
     * is not empty: wg_wave() draws the waveform there afterwards, and a
     * pointer swept through the legend would be drawn over rather than read
     * with it. The glyph reaches 0.46 of the radius at its corners, so 0.55 is
     * clear of it at every position.
     */
    const float r0  = (detents > 1) ? 0.55f : 0.22f;
    const float ang = ANG_MIN + ANG_SWEEP * norm;
    spoke(g, cx, cy, ang, (float)r * r0, (float)r * 0.86f, TH_GLOW);

    if (detents <= 1) {
        turn_disc(g, cx, cy, r > 40 ? 9 : 6, TH_EDGE);
    }
}

void wg_wave(const turn_t *g, int16_t cx, int16_t cy, int16_t hw, int16_t hh,
             int wave, int cycles)
{
    if (cycles < 1) { cycles = 1; }

    const int16_t x0  = (int16_t)(cx - hw);
    const int16_t top = (int16_t)(cy - hh), bot = (int16_t)(cy + hh);
    const int16_t cw  = (int16_t)((2 * hw) / cycles);
    const ngl_color_t c = TH_GLOW;

    for (int k = 0; k < cycles; k++) {
        const int16_t xa = (int16_t)(x0 + k * cw);
        const int16_t xb = (int16_t)(xa + cw);
        const int16_t xm = (int16_t)(xa + cw / 2);

        switch (wave) {
        case WG_SAW:
            turn_line(g, xa, bot, (int16_t)(xb - 1), top, c);
            turn_vline(g, (int16_t)(xb - 1), top, (int16_t)(bot - top + 1), c);
            break;

        case WG_TRI:
            turn_line(g, xa, bot, xm, top, c);
            turn_line(g, xm, top, xb, bot, c);
            break;

        case WG_TRISAW: {
            /* The corner knocked off a ramp, which is what the Model D's
               second position looks like on a scope. */
            const int16_t k1 = (int16_t)(xa + cw / 3);
            turn_line(g, xa, bot, k1, (int16_t)(cy - hh / 3), c);
            turn_line(g, k1, (int16_t)(cy - hh / 3), (int16_t)(xb - 1), top, c);
            turn_vline(g, (int16_t)(xb - 1), top, (int16_t)(bot - top + 1), c);
            break;
        }
        case WG_SQUARE:
        case WG_WIDE:
        case WG_NARROW: {
            const int16_t duty = (wave == WG_SQUARE) ? (int16_t)(cw / 2)
                               : (wave == WG_WIDE)   ? (int16_t)(cw / 4)
                                                     : (int16_t)(cw / 8);
            const int16_t xe = (int16_t)(xa + (duty < 2 ? 2 : duty));
            turn_vline(g, xa, top, (int16_t)(bot - top + 1), c);
            turn_hline(g, xa, top, (int16_t)(xe - xa + 1), c);
            turn_vline(g, xe, top, (int16_t)(bot - top + 1), c);
            turn_hline(g, xe, bot, (int16_t)(xb - xe), c);
            break;
        }
        case WG_SH:
        default: {
            /* A fixed staircase, not a fresh random one: a legend that changes
               every repaint reads as a glitch rather than as noise. */
            static const int8_t step[4] = { 2, -1, 3, 0 };
            const int16_t sw = (int16_t)(cw / 4);
            for (int i = 0; i < 4; i++) {
                const int16_t y = (int16_t)(cy - step[i] * hh / 3);
                turn_hline(g, (int16_t)(xa + i * sw), y, sw, c);
                if (i > 0) {
                    const int16_t yp = (int16_t)(cy - step[i - 1] * hh / 3);
                    turn_vline(g, (int16_t)(xa + i * sw),
                               (int16_t)(y < yp ? y : yp),
                               (int16_t)((y < yp ? yp - y : y - yp) + 1), c);
                }
            }
            break;
        }
        }
    }
}

void wg_switch(const turn_t *g, ngl_rect_t r, bool upper,
               const char *a, const char *b)
{
    const int16_t h  = (int16_t)((r.h - 8) / 2);
    const int16_t th = (int16_t)ngl_font_small.height;

    const ngl_rect_t up = ngl_rect(r.x, r.y, r.w, h);
    const ngl_rect_t dn = ngl_rect(r.x, (int16_t)(r.y + r.h - h), r.w, h);

    const ngl_color_t up_fill = upper ? TH_KEY_DOWN : TH_KEY_FILL;
    const ngl_color_t dn_fill = upper ? TH_KEY_FILL : TH_KEY_DOWN;

    turn_round(g, up, 10, up_fill);
    turn_round_frame(g, up, 10, TH_KEY_EDGE, 2);
    turn_text_mid(g, up.x, (int16_t)(up.y + (h - th) / 2), r.w, a ? a : "",
                  &ngl_font_small, upper ? TH_GLOW : TH_TEXT_FAINT, up_fill);

    turn_round(g, dn, 10, dn_fill);
    turn_round_frame(g, dn, 10, TH_KEY_EDGE, 2);
    turn_text_mid(g, dn.x, (int16_t)(dn.y + (h - th) / 2), r.w, b ? b : "",
                  &ngl_font_small, upper ? TH_TEXT_FAINT : TH_GLOW, dn_fill);
}
