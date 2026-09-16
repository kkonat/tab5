/*
 * The fader and the keyboard. See mg_panel.h for why only these two are here.
 */
#include "mg_panel.h"

#include "ngl_theme.h"

/* ------------------------------------------------------------------ */
/* The fader                                                           */
/* ------------------------------------------------------------------ */

/*
 * Vertical, with a cap that is an object rather than a coloured fraction of a
 * bar.
 *
 * The difference matters for the one control that is on both pages: it is
 * reached for without looking, so it has to be findable by shape. A filled bar
 * tells you the value; a cap tells you where to put your thumb. The lit part
 * below the cap does the first job and the cap does the second, which is how a
 * mixing desk has always drawn this.
 */
void mg_fader(const turn_t *g, ngl_rect_t r, float norm)
{
    const int16_t cap_h  = 44;
    const int16_t pad    = 10;
    const int16_t trk_w  = 10;
    const int16_t trk_x  = (int16_t)(r.x + (r.w - trk_w) / 2);
    const int16_t trk_y  = (int16_t)(r.y + pad);
    const int16_t trk_h  = (int16_t)(r.h - 2 * pad);
    const int16_t travel = (int16_t)(trk_h - cap_h);

    turn_fill(g, r, TH_BG);
    turn_fill(g, ngl_rect(trk_x, trk_y, trk_w, trk_h), TH_PANEL);
    turn_frame(g, ngl_rect(trk_x, trk_y, trk_w, trk_h), TH_EDGE, 1);

    /* Zero at the bottom, which is where a fader's zero is. */
    const int16_t cap_y = (int16_t)((float)trk_y + (float)travel * (1.0f - norm));
    const int16_t mid_y = (int16_t)(cap_y + cap_h / 2);

    if (mid_y < trk_y + trk_h) {
        turn_fill(g, ngl_rect((int16_t)(trk_x + 2), mid_y, (int16_t)(trk_w - 4),
                              (int16_t)(trk_y + trk_h - mid_y)), TH_ACCENT);
    }

    /* Ticks down both sides, every tenth, so a setting can be found again. */
    for (int i = 0; i <= 10; i++) {
        const int16_t y = (int16_t)((float)(trk_y + cap_h / 2) +
                                    (float)travel * (1.0f - (float)i / 10.0f));
        const int16_t len = (i % 5 == 0) ? 11 : 6;
        turn_hline(g, (int16_t)(trk_x - len - 4), y, len, TH_TEXT_FAINT);
        turn_hline(g, (int16_t)(trk_x + trk_w + 4), y, len, TH_TEXT_FAINT);
    }

    const ngl_rect_t c = ngl_rect((int16_t)(r.x + 4), cap_y, (int16_t)(r.w - 8), cap_h);
    turn_round(g, c, 8, TH_KEY_LATCH);
    turn_round_frame(g, c, 8, TH_GLOW, 2);
    turn_hline(g, (int16_t)(c.x + 6), (int16_t)(c.y + cap_h / 2),
               (int16_t)(c.w - 12), TH_GLOW);
}

void mg_lamp(const turn_t *g, ngl_rect_t r, bool on)
{
    const int16_t cx  = (int16_t)(r.x + r.w / 2);
    const int16_t cy  = (int16_t)(r.y + r.h / 2);
    const int16_t rad = (int16_t)((r.w < r.h ? r.w : r.h) / 2 - 2);

    turn_disc(g, cx, cy, rad, on ? TH_GLOW : TH_KEY_FILL);
    turn_ring(g, cx, cy, rad, 2, on ? TH_ACCENT : TH_EDGE);
}

/* ------------------------------------------------------------------ */
/* The keyboard                                                        */
/* ------------------------------------------------------------------ */

/*
 * Eleven white keys and seven black, laid out from the semitone pattern rather
 * than from a table of positions: a black key sits on the boundary between the
 * two white keys either side of it, and where that boundary is follows from
 * counting the white keys before it. Writing the positions down instead is
 * eighteen numbers that have to agree with each other and with the hit test,
 * which is eighteen chances for the picture and the touch to disagree.
 */
#define WHITE_KEYS 11

bool mg_key_black(int key)
{
    const int s = key % 12;
    return s == 1 || s == 3 || s == 6 || s == 8 || s == 10;
}

static int whites_before(int key)
{
    int n = 0;
    for (int i = 0; i < key; i++) {
        if (!mg_key_black(i)) { n++; }
    }
    return n;
}

ngl_rect_t mg_key_rect(ngl_rect_t r, int key)
{
    const int16_t w = (int16_t)(r.w / WHITE_KEYS);

    if (!mg_key_black(key)) {
        return ngl_rect((int16_t)(r.x + whites_before(key) * w), r.y, w, r.h);
    }
    const int16_t bw = (int16_t)(w * 3 / 5);
    const int16_t bh = (int16_t)(r.h * 3 / 5);
    return ngl_rect((int16_t)(r.x + whites_before(key) * w - bw / 2), r.y, bw, bh);
}

int mg_key_at(ngl_rect_t r, int16_t x, int16_t y)
{
    if (!ngl_rect_contains(&r, x, y)) {
        return -1;
    }
    /* Black first: they are on top, which is also the order they are drawn. */
    for (int k = 0; k < MG_KEYS; k++) {
        if (mg_key_black(k)) {
            const ngl_rect_t kr = mg_key_rect(r, k);
            if (ngl_rect_contains(&kr, x, y)) {
                return k;
            }
        }
    }
    for (int k = 0; k < MG_KEYS; k++) {
        if (!mg_key_black(k)) {
            const ngl_rect_t kr = mg_key_rect(r, k);
            if (ngl_rect_contains(&kr, x, y)) {
                return k;
            }
        }
    }
    return -1;
}

static void white_face(const turn_t *g, ngl_rect_t kr, bool down)
{
    turn_fill(g, ngl_rect((int16_t)(kr.x + 1), kr.y, (int16_t)(kr.w - 2), kr.h),
              down ? TH_KEY_DOWN : TH_KEY_TEXT);
    turn_frame(g, kr, TH_EDGE, 1);
}

static void black_face(const turn_t *g, ngl_rect_t kr, bool down)
{
    turn_fill(g, kr, down ? TH_ACCENT : TH_BG);
    turn_frame(g, kr, TH_EDGE, 2);
}

ngl_rect_t mg_key_paint(const turn_t *g, ngl_rect_t r, int key, uint32_t held)
{
    const ngl_rect_t kr = mg_key_rect(r, key);
    const bool down = (held & (1u << key)) != 0;

    if (mg_key_black(key)) {
        black_face(g, kr, down);
        return kr;
    }
    white_face(g, kr, down);
    ngl_rect_t touched = kr;

    /*
     * A black key sits over the top of the two white keys either side of it,
     * so repainting a white key rubs out whichever black keys overlap it.
     * Putting them back is cheaper than working out which ones they were, and
     * a great deal cheaper than repainting the whole keyboard - which at 152
     * rows is what a press would otherwise cost.
     */
    for (int k = 0; k < MG_KEYS; k++) {
        if (!mg_key_black(k)) {
            continue;
        }
        const ngl_rect_t br = mg_key_rect(r, k);
        ngl_rect_t hit;
        if (ngl_rect_intersect(&br, &kr, &hit)) {
            black_face(g, br, (held & (1u << k)) != 0);
            touched = ngl_rect_union(&touched, &br);
        }
    }
    return touched;
}

void mg_keys(const turn_t *g, ngl_rect_t r, uint32_t held)
{
    turn_fill(g, r, TH_BG);

    for (int k = 0; k < MG_KEYS; k++) {
        if (!mg_key_black(k)) {
            white_face(g, mg_key_rect(r, k), (held & (1u << k)) != 0);
        }
    }
    for (int k = 0; k < MG_KEYS; k++) {
        if (mg_key_black(k)) {
            black_face(g, mg_key_rect(r, k), (held & (1u << k)) != 0);
        }
    }
}
