/*
 * The on-screen keyboard.
 *
 * A US QWERTY layout with real modifiers: shift and ctrl are held, with a
 * second finger, the way they are on a keyboard with keys. That is why the
 * touch driver reads more than one point - a modifier that could only be
 * tapped is a modifier you cannot combine with anything, and a keyboard whose
 * capitals need two separate taps is a keyboard nobody types a password on
 * twice.
 *
 * Held is not the only way. A modifier tapped and released without anything
 * being typed under it latches for exactly one key, which is how the same
 * layout works one-handed. Caps lock is a plain toggle and applies to letters
 * only, which is what a caps lock is.
 *
 * The layout is a table, in half-key units, and every row sums to the same
 * number of them - so the columns line up, the whole thing scales to whatever
 * width the screen currently is, and adding a key is an entry rather than a
 * calculation. Portrait is tighter than landscape and that is simply what a
 * 720-pixel-wide QWERTY is; nothing is rotated, because rotating the panel
 * would rotate the app underneath it and lose the frame it is sitting on.
 *
 * Ctrl does what it does in a line editor rather than what it does in a text
 * editor, because a line editor is what this is: see CTRL BINDINGS below.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_kbd.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "kbd";

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

enum {
    K_CHAR = 0,
    K_BKSP, K_TAB, K_CAPS, K_ENTER, K_SHIFT, K_CTRL,
    K_SPACE, K_DEL, K_LEFT, K_RIGHT,
};

typedef struct {
    uint8_t     code;
    uint8_t     units;      /* width, in half-key units */
    const char *lower;      /* legend, and for K_CHAR the character it types */
    const char *upper;      /* what shift makes of it */
} key_t;

/*
 * Widths are in half-key units, and every row below adds up to the same total
 * - which is what makes the columns line up. The total itself is measured at
 * layout time rather than written down here, so a row that does not add up
 * still fills the width and misaligns visibly, instead of leaving a gap at
 * one end that looks like a rendering fault.
 */

#define C(lo, up) { K_CHAR, 2, lo, up }

static const key_t ROW0[] = {
    C("`", "~"), C("1", "!"), C("2", "@"), C("3", "#"), C("4", "$"),
    C("5", "%"), C("6", "^"), C("7", "&"), C("8", "*"), C("9", "("),
    C("0", ")"), C("-", "_"), C("=", "+"),
    { K_BKSP, 4, "Bksp", "Bksp" },
};

static const key_t ROW1[] = {
    { K_TAB, 4, "Tab", "Tab" },
    C("q", "Q"), C("w", "W"), C("e", "E"), C("r", "R"), C("t", "T"),
    C("y", "Y"), C("u", "U"), C("i", "I"), C("o", "O"), C("p", "P"),
    C("[", "{"), C("]", "}"), C("\\", "|"),
};

static const key_t ROW2[] = {
    { K_CAPS, 4, "Caps", "Caps" },
    C("a", "A"), C("s", "S"), C("d", "D"), C("f", "F"), C("g", "G"),
    C("h", "H"), C("j", "J"), C("k", "K"), C("l", "L"),
    C(";", ":"), C("'", "\""),
    { K_ENTER, 4, "Enter", "Enter" },
};

static const key_t ROW3[] = {
    { K_SHIFT, 5, "Shift", "Shift" },
    C("z", "Z"), C("x", "X"), C("c", "C"), C("v", "V"), C("b", "B"),
    C("n", "N"), C("m", "M"), C(",", "<"), C(".", ">"), C("/", "?"),
    { K_SHIFT, 5, "Shift", "Shift" },
};

static const key_t ROW4[] = {
    { K_CTRL,  4, "Ctrl",  "Ctrl" },
    { K_BKSP,  3, "BS",    "BS" },
    { K_DEL,   3, "Del",   "Del" },
    { K_LEFT,  3, "<-",    "<-" },
    { K_SPACE, 10, "Space", "Space" },
    { K_RIGHT, 3, "->",    "->" },
    { K_CTRL,  4, "Ctrl",  "Ctrl" },
};

#undef C

typedef struct {
    const key_t *keys;
    int          n;
} row_t;

#define ROW(r) { r, (int)(sizeof(r) / sizeof(key_t)) }

static const row_t ROWS[] = { ROW(ROW0), ROW(ROW1), ROW(ROW2), ROW(ROW3), ROW(ROW4) };

#undef ROW

#define NROWS  (int)(sizeof(ROWS) / sizeof(row_t))
#define MAXKEY 16       /* the widest row; only ROW0 and ROW1 come near it */

/* The caches below are indexed by key, so the widest row sets their size. */
_Static_assert(sizeof(ROW0) / sizeof(key_t) <= MAXKEY, "MAXKEY too small");
_Static_assert(sizeof(ROW1) / sizeof(key_t) <= MAXKEY, "MAXKEY too small");
_Static_assert(sizeof(ROW2) / sizeof(key_t) <= MAXKEY, "MAXKEY too small");
_Static_assert(sizeof(ROW3) / sizeof(key_t) <= MAXKEY, "MAXKEY too small");
_Static_assert(sizeof(ROW4) / sizeof(key_t) <= MAXKEY, "MAXKEY too small");

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

#define PAD       12
#define KEY_H     64
#define KEY_GAP   6
#define HEAD_H    112                 /* title bar plus the field */
#define FIELD_H   48
#define PANEL_H   (HEAD_H + NROWS * (KEY_H + KEY_GAP) + PAD)

#define POLL_MS      20
#define REPEAT_FIRST 480              /* hold this long before it starts over */
#define REPEAT_EVERY 70

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static ngl_rect_t s_panel;            /* the whole keyboard, screen coordinates */
static ngl_rect_t s_field;

/* Layout is recomputed whenever the panel is, so key rectangles are cached
   rather than derived on every hit test - a poll at 50 Hz walks all 63 of
   them. */
static ngl_rect_t s_krect[NROWS][MAXKEY];

static bool  s_down[NROWS][MAXKEY];   /* a finger is on it now */
static bool  s_shown[NROWS][MAXKEY];  /* what is painted, so repaints are rare */
static bool  s_used[NROWS][MAXKEY];   /* a modifier was consumed while held */
static int64_t s_repeat_at[NROWS][MAXKEY];

static bool s_shift_latch, s_ctrl_latch, s_caps;
static bool s_shift_held, s_ctrl_held;

static char   s_buf[192];
static int    s_len, s_cur;
static bool   s_secret;
static bool   s_field_dirty;

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static void layout(void)
{
    const ngl_rect_t app = ngl_app_area();

    int16_t h = PANEL_H;
    if (h > app.h) {
        h = app.h;
    }
    s_panel = ngl_rect((int16_t)(app.x + 8), (int16_t)(app.y + app.h - h),
                       (int16_t)(app.w - 16), h);

    s_field = ngl_rect((int16_t)(s_panel.x + PAD),
                       (int16_t)(s_panel.y + NEOS_UI_TITLE_H + 4),
                       (int16_t)(s_panel.w - 2 * PAD), FIELD_H);

    /*
     * Widths are laid out by running the unit total along the row and taking
     * the difference, not by rounding each key. Rounding each one leaves the
     * error at the right-hand end, where it shows up as a Backspace a few
     * pixels short of the edge on some rotations and not others.
     */
    const int16_t x0 = (int16_t)(s_panel.x + PAD);
    const int16_t span = (int16_t)(s_panel.w - 2 * PAD);
    const int16_t y0 = (int16_t)(s_panel.y + HEAD_H);

    for (int r = 0; r < NROWS; r++) {
        int total = 0;
        for (int k = 0; k < ROWS[r].n; k++) {
            total += ROWS[r].keys[k].units;
        }
        int used = 0;
        for (int k = 0; k < ROWS[r].n; k++) {
            const int next = used + ROWS[r].keys[k].units;
            const int16_t xa = (int16_t)(x0 + (int32_t)span * used / total);
            const int16_t xb = (int16_t)(x0 + (int32_t)span * next / total);
            s_krect[r][k] = ngl_rect(xa, (int16_t)(y0 + r * (KEY_H + KEY_GAP)),
                                     (int16_t)(xb - xa - KEY_GAP), KEY_H);
            used = next;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/** True when the key should be drawn as active: pressed, or latched on. */
static bool key_lit(int r, int k)
{
    if (s_down[r][k]) {
        return true;
    }
    switch (ROWS[r].keys[k].code) {
    case K_SHIFT: return s_shift_latch;
    case K_CTRL:  return s_ctrl_latch;
    case K_CAPS:  return s_caps;
    default:      return false;
    }
}

/** What this key types right now, given the modifiers. */
static const char *key_legend(int r, int k)
{
    const key_t *key = &ROWS[r].keys[k];
    if (key->code != K_CHAR) {
        return key->lower;
    }
    const bool shifted = s_shift_held || s_shift_latch;
    const bool alpha = key->lower[0] >= 'a' && key->lower[0] <= 'z';
    /* Caps lock is letters only. On a real keyboard caps lock and shift
       cancel on a letter and do nothing to each other on a digit, which is
       exactly what this xor and this test say. */
    const bool up = alpha ? (shifted != s_caps) : shifted;
    return up ? key->upper : key->lower;
}

/*
 * The gap between two keys belongs to both of them.
 *
 * A finger is wider than the six pixels of space between the keys, so a strip
 * that answered to neither would show up as a key that occasionally does not
 * respond - the hardest kind of input bug to believe in. The rectangle that is
 * drawn and the rectangle that is hit are deliberately not the same one.
 */
static ngl_rect_t hit_rect(int r, int k)
{
    const ngl_rect_t b = s_krect[r][k];
    return ngl_rect(b.x, b.y, (int16_t)(b.w + KEY_GAP), (int16_t)(b.h + KEY_GAP));
}

static void paint_key(ngl_surface_t *s, int r, int k)
{
    const key_t *key = &ROWS[r].keys[k];
    const ngl_rect_t b = s_krect[r][k];
    const bool lit = key_lit(r, k);
    const bool special = key->code != K_CHAR;

    ngl_color_t fill = special ? TH_KEY_MOD : TH_KEY_FILL;
    ngl_color_t edge = TH_KEY_EDGE;
    ngl_color_t text = TH_KEY_TEXT;

    if (s_down[r][k]) {
        fill = TH_KEY_DOWN;
        edge = TH_GLOW;
        text = TH_BG;
    } else if (lit) {
        fill = TH_KEY_LATCH;
        edge = TH_ACCENT;
        text = TH_GLOW;
    }

    ngl_fill_round_rect(s, b, 8, fill);
    ngl_draw_round_rect(s, b, 8, edge, 2);
    neos_ui_text_centred(s, b, key_legend(r, k), &ngl_font_small, text);

    s_shown[r][k] = lit;
}

/*
 * The field, with the cursor.
 *
 * Scrolled so the cursor is always on screen rather than wrapped: this is one
 * line of text and a password is the main thing typed into it, so the useful
 * property is that the end of what you are typing is visible, not that all of
 * it is.
 */
static void paint_field(ngl_surface_t *s)
{
    const ngl_font_t *f = &ngl_font_small;

    ngl_fill_round_rect(s, s_field, 6, TH_BG);
    ngl_draw_round_rect(s, s_field, 6, TH_EDGE, 2);

    char shown[sizeof(s_buf)];
    if (s_secret) {
        for (int i = 0; i < s_len; i++) {
            shown[i] = '*';
        }
        shown[s_len] = 0;
    } else {
        memcpy(shown, s_buf, (size_t)s_len + 1);
    }

    const int16_t inner_x = (int16_t)(s_field.x + 10);
    const int16_t inner_w = (int16_t)(s_field.w - 20);
    const int16_t cur_px  = (int16_t)(s_cur * f->width);

    int16_t scroll = 0;
    if (cur_px > inner_w - f->width) {
        scroll = (int16_t)(cur_px - (inner_w - f->width));
    }

    const ngl_rect_t inner = ngl_rect(inner_x, s_field.y, inner_w, s_field.h);
    const ngl_rect_t saved = ngl_surface_clip(s);
    ngl_rect_t clip;
    if (ngl_rect_intersect(&inner, &saved, &clip)) {
        ngl_clip_set(s, &clip);
        const int16_t ty = (int16_t)(s_field.y + (s_field.h - f->height) / 2);
        ngl_text(s, (int16_t)(inner_x - scroll), ty, shown, f, TH_TEXT);
        ngl_fill_rect(s, ngl_rect((int16_t)(inner_x - scroll + cur_px),
                                  (int16_t)(ty - 2), 2, (int16_t)(f->height + 4)),
                      TH_ACCENT);
        ngl_clip_set(s, &saved);
    }
    s_field_dirty = false;
}

static void paint_all(const char *title)
{
    ngl_overlay_restore();
    ngl_surface_t *s = ngl_overlay_begin(ngl_app_area());
    if (!s) {
        return;
    }
    /* From the app's own frame each time, not from the last thing painted -
       the backdrop is a blend, and blending it twice is a black screen. */
    neos_ui_dim(s);
    neos_ui_frame(s, s_panel, title && title[0] ? title : "Input");
    paint_field(s);
    for (int r = 0; r < NROWS; r++) {
        for (int k = 0; k < ROWS[r].n; k++) {
            paint_key(s, r, k);
        }
    }
    ngl_overlay_end();
    ngl_flush();
}

/* ------------------------------------------------------------------ */
/* Editing                                                             */
/* ------------------------------------------------------------------ */

static void insert(char c)
{
    if (s_len + 1 >= (int)sizeof(s_buf)) {
        return;
    }
    memmove(&s_buf[s_cur + 1], &s_buf[s_cur], (size_t)(s_len - s_cur + 1));
    s_buf[s_cur++] = c;
    s_len++;
    s_field_dirty = true;
}

static void erase_before(void)
{
    if (s_cur <= 0) {
        return;
    }
    memmove(&s_buf[s_cur - 1], &s_buf[s_cur], (size_t)(s_len - s_cur + 1));
    s_cur--;
    s_len--;
    s_field_dirty = true;
}

static void erase_after(void)
{
    if (s_cur >= s_len) {
        return;
    }
    memmove(&s_buf[s_cur], &s_buf[s_cur + 1], (size_t)(s_len - s_cur));
    s_len--;
    s_field_dirty = true;
}

/*
 * CTRL BINDINGS
 *
 * The readline set, because this is a one-line field and those are the five
 * things anyone has ever wanted to do to one. Ctrl with anything else does
 * nothing rather than inserting a control character - there is no field on
 * this tablet that a raw 0x01 belongs in, and silently putting one in a Wi-Fi
 * password would be a fault nobody could see.
 */
static bool ctrl_key(char c, bool *cancel)
{
    switch (c) {
    case 'a': s_cur = 0;     s_field_dirty = true; return true;
    case 'e': s_cur = s_len; s_field_dirty = true; return true;
    case 'u':
        s_buf[0] = 0;
        s_len = s_cur = 0;
        s_field_dirty = true;
        return true;
    case 'w':
        while (s_cur > 0 && s_buf[s_cur - 1] == ' ') { erase_before(); }
        while (s_cur > 0 && s_buf[s_cur - 1] != ' ') { erase_before(); }
        return true;
    case 'c':
        *cancel = true;
        return true;
    default:
        return true;      /* swallowed: ctrl never types */
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/**
 * Act on one key going down, or repeating.
 *
 * Returns false when the keyboard should close; @p accept says whether that
 * was Enter or a cancel.
 */
static bool press(int r, int k, bool *accept)
{
    const key_t *key = &ROWS[r].keys[k];
    const bool ctrl = s_ctrl_held || s_ctrl_latch;

    /*
     * A modifier that is held while something else is pressed has done its
     * job as a modifier, so releasing it must not also latch it. That is what
     * s_used records - the difference between "shift, then a" and "shift"
     * on its own.
     */
    if (key->code != K_SHIFT && key->code != K_CTRL) {
        for (int rr = 0; rr < NROWS; rr++) {
            for (int kk = 0; kk < ROWS[rr].n; kk++) {
                if (s_down[rr][kk]) {
                    s_used[rr][kk] = true;
                }
            }
        }
    }

    switch (key->code) {
    case K_CHAR: {
        if (ctrl) {
            bool cancel = false;
            ctrl_key(key->lower[0], &cancel);
            s_ctrl_latch = false;
            if (cancel) {
                *accept = false;
                return false;
            }
            break;
        }
        insert(key_legend(r, k)[0]);
        s_shift_latch = false;     /* one-shot: it applied to this key */
        break;
    }
    case K_SPACE: insert(' ');  s_shift_latch = false; break;
    case K_TAB:   insert('\t'); s_shift_latch = false; break;
    case K_BKSP:  erase_before(); break;
    case K_DEL:   erase_after();  break;
    case K_LEFT:  if (s_cur > 0)     { s_cur--; s_field_dirty = true; } break;
    case K_RIGHT: if (s_cur < s_len) { s_cur++; s_field_dirty = true; } break;
    case K_CAPS:  s_caps = !s_caps; break;
    case K_ENTER:
        *accept = true;
        return false;
    case K_SHIFT:
    case K_CTRL:
        /* Held is handled by the poll; the press itself does nothing. */
        break;
    default:
        break;
    }
    return true;
}

/** Only these repeat. A repeating letter is a typo; a repeating arrow is not. */
static bool repeats(uint8_t code)
{
    return code == K_BKSP || code == K_DEL || code == K_LEFT || code == K_RIGHT;
}

/* ------------------------------------------------------------------ */

bool neos_kbd_run(const char *title, char *buf, size_t size, uint32_t flags)
{
    if (!buf || size < 2 || !ngl_screen() || !neos_touch_present()) {
        return false;
    }
    if (!ngl_overlay_enter()) {
        ESP_LOGW(TAG, "cannot take the screen for the keyboard");
        return false;
    }
    neos_touch_capture(true);

    s_secret = (flags & NEOS_INPUT_SECRET) != 0;
    strlcpy(s_buf, buf, sizeof(s_buf));
    s_len = (int)strlen(s_buf);
    s_cur = s_len;
    s_shift_latch = s_ctrl_latch = s_caps = false;
    s_shift_held = s_ctrl_held = false;
    memset(s_down, 0, sizeof(s_down));
    memset(s_used, 0, sizeof(s_used));
    memset(s_repeat_at, 0, sizeof(s_repeat_at));

    layout();
    paint_all(title);

    bool accept = false;
    bool running = true;
    ngl_rect_t area = ngl_app_area();

    while (running) {
        neos_touch_t pts[NEOS_TOUCH_MAX];
        const int np = neos_touch_points_os(pts, NEOS_TOUCH_MAX);
        const int64_t now = esp_timer_get_time();

        /* Sampled before anything in this pass can change them: the rising
           edge below sets s_shift_held so that press() sees it, which would
           otherwise make the comparison at the end always come out equal. */
        const bool was_upper = s_shift_held || s_shift_latch;
        const bool was_caps  = s_caps;

        bool shift = false, ctrl = false;
        bool repaint[NROWS][MAXKEY];
        memset(repaint, 0, sizeof(repaint));

        for (int r = 0; r < NROWS && running; r++) {
            for (int k = 0; k < ROWS[r].n && running; k++) {
                bool now_down = false;
                for (int p = 0; p < np && p < NEOS_TOUCH_MAX; p++) {
                    const ngl_rect_t hb = hit_rect(r, k);
                    if (ngl_rect_contains(&hb, pts[p].x, pts[p].y)) {
                        now_down = true;
                        break;
                    }
                }
                const uint8_t code = ROWS[r].keys[k].code;

                if (now_down && !s_down[r][k]) {
                    s_down[r][k] = true;
                    s_used[r][k] = false;
                    s_repeat_at[r][k] = now + REPEAT_FIRST * 1000;
                    /* Modifiers take effect before the key that used them,
                       which is only true because they are latched here rather
                       than after the whole row has been walked. */
                    if (code == K_SHIFT) { s_shift_held = true; }
                    if (code == K_CTRL)  { s_ctrl_held  = true; }
                    running = press(r, k, &accept);
                    repaint[r][k] = true;
                } else if (!now_down && s_down[r][k]) {
                    s_down[r][k] = false;
                    if (code == K_SHIFT && !s_used[r][k]) {
                        s_shift_latch = !s_shift_latch;
                    }
                    if (code == K_CTRL && !s_used[r][k]) {
                        s_ctrl_latch = !s_ctrl_latch;
                    }
                    repaint[r][k] = true;
                } else if (now_down && repeats(code) && now >= s_repeat_at[r][k]) {
                    s_repeat_at[r][k] = now + REPEAT_EVERY * 1000;
                    running = press(r, k, &accept);
                }

                if (s_down[r][k]) {
                    if (code == K_SHIFT) { shift = true; }
                    if (code == K_CTRL)  { ctrl  = true; }
                }
            }
        }

        /*
         * Held state is recomputed from the fingers each pass rather than
         * cleared on release. Two fingers on the two Shift keys and one of
         * them lifted must leave shift still held, and only counting what is
         * actually on the glass gets that right.
         */
        s_shift_held = shift;
        s_ctrl_held  = ctrl;

        /* What a character key shows depends on the effective case and on
           nothing else, so that is what decides whether the legends have to be
           redrawn - not which modifier key moved. */
        const bool legends_moved = ((s_shift_held || s_shift_latch) != was_upper) ||
                                   (s_caps != was_caps);

        /*
         * Two ways out that are not the close box: the app underneath is
         * going away, or the panel this keyboard was raised from is. The ui
         * flag is only meaningful while a panel is actually running, or a
         * stale one would close the next keyboard an app puts up.
         */
        if (neos_app_close_requested() ||
            (neos_ui_active() && neos_ui_should_close())) {
            accept = false;
            running = false;
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap_os(&tx, &ty)) {
            const ngl_rect_t cb = neos_ui_close_rect(s_panel);
            if (ngl_rect_contains(&cb, tx, ty)) {
                accept = false;
                running = false;
            }
        }

        /* A rotation cannot happen under a panel, but the app area does move
           when the bar's close button comes and goes. Relaying out is cheaper
           than being wrong about where the keys are. */
        const ngl_rect_t a = ngl_app_area();
        const bool moved = a.x != area.x || a.y != area.y ||
                           a.w != area.w || a.h != area.h;
        if (moved) {
            area = a;
            layout();
            paint_all(title);
            continue;
        }

        ngl_surface_t *s = ngl_overlay_begin(s_panel);
        if (s) {
            for (int r = 0; r < NROWS; r++) {
                for (int k = 0; k < ROWS[r].n; k++) {
                    /* Legends change under a modifier, so a shift going down
                       repaints every character key, not just itself. */
                    const bool legend_moved =
                        legends_moved && ROWS[r].keys[k].code == K_CHAR;
                    if (repaint[r][k] || legend_moved ||
                        s_shown[r][k] != key_lit(r, k)) {
                        paint_key(s, r, k);
                    }
                }
            }
            if (s_field_dirty) {
                paint_field(s);
            }
            ngl_overlay_end();
            ngl_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    if (accept) {
        strlcpy(buf, s_buf, size);
    }
    /* Not left in a static for the next caller to inherit - it may well have
       been a password, and the next field is not entitled to it. */
    memset(s_buf, 0, sizeof(s_buf));
    s_len = s_cur = 0;

    neos_touch_capture(false);
    ngl_overlay_leave();
    neos_touch_drop();

    ESP_LOGI(TAG, "keyboard closed, %s", accept ? "accepted" : "cancelled");
    return accept;
}

bool neos_input_text(const char *title, char *buf, size_t size, uint32_t flags)
{
    return neos_kbd_run(title, buf, size, flags);
}
