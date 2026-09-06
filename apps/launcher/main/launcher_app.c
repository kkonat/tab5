/*
 * The NeOS launcher - an app like any other.
 *
 * It lives on the card, is named by autorun.cfg, and is loaded through the
 * same ELF path as everything else. Nothing here is linked against ngl or the
 * OS: every ngl_* and neos_* symbol below is left undefined at link time and
 * resolved by the syscall table when NeOS loads this image, which is why the
 * whole shell costs a couple of kilobytes on the card instead of carrying its
 * own copy of the UI toolkit.
 *
 * The system bar is not drawn here. NeOS owns it, so it survives this app
 * handing over to another one, and its close button is the same button
 * wherever you are.
 *
 * Being an app rather than part of the firmware is the point: change one line
 * of autorun.cfg, or put a different card in, and a different shell comes up.
 *
 * The screen is a tab strip over a grid of cards. Both scroll, and both scroll
 * by being dragged rather than by any furniture of their own: a card is 120 px
 * tall and a tab is most of a fingertip wide, so there is nothing on this
 * screen small enough to need a scrollbar to grab, and the one drawn down the
 * right of the grid is there to say how far along you are, not to be used.
 *
 * Which tab an app lands in comes from "category" in its manifest, so putting
 * an app on a different shelf is editing its manifest and not this file.
 *
 * A card has one gesture beyond the tap that launches it: held, a quarantined
 * app is let out. That is the only way back from a crash, because the counter
 * that quarantined it lives in the firmware's NVS where nothing on the card
 * can reach it - see card_hold().
 */
#include <stdio.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_sys.h"       /* neos_uptime_ms, for the hold on a card */

#define UI_MARGIN   20
#define UI_CARD_H   120
#define UI_CARD_GAP 12
#define UI_GRID_TOP 12         /* between the tab strip and the first card */

#define TAB_H       72
#define TAB_PAD     36         /* breathing room either side of a label */
#define TAB_INSET   3          /* between one tab box and the next */
#define TAB_TOP     4          /* above the boxes, so they sit on the rule */

/* How far a finger rolls on a press that was meant as a tap. Past this the
   gesture is a drag and the release is not a tap any more. */
#define SLOP        14

/*
 * How long a press has to stand still on a card to be a hold rather than a
 * slow tap.
 *
 * The hold is what lets a quarantined app out - see card_hold(). Deliberately
 * long: the thing it undoes is the safety net that stops a crashing app being
 * launched into over and over, so it has to be past any plausible hesitation
 * over which card to pick.
 */
#define HOLD_MS    700

#define SCROLLBAR_W 4

/* Colours come from the system theme, not from here. */
#define COL_BG       TH_BG
#define COL_EDGE     TH_EDGE
#define COL_TEXT     TH_TEXT
#define COL_DIM      TH_TEXT_DIM
#define COL_ACCENT   TH_ACCENT

/* ------------------------------------------------------------------ */
/* The shelves                                                         */
/* ------------------------------------------------------------------ */

/*
 * Fixed, and in this order. A tab strip whose tabs come and go as apps are
 * uploaded is a strip you cannot learn: the third tab has to stay the third
 * tab whether or not anything is in it today. So an empty shelf is drawn and
 * says it is empty, rather than being left out.
 *
 * "Other" is last and is where anything unclassified goes, which includes
 * every app written before manifests had a category. An app is never hidden
 * for failing to name a shelf.
 */
static const char *const CATS[] = {
    "System", "Tools", "Games", "Sound", "EyeCandy", "Other",
};
#define NCATS   ((int)(sizeof(CATS) / sizeof(CATS[0])))
#define CAT_OTHER (NCATS - 1)

/*
 * ASCII case-insensitive compare, because a manifest is written by hand and
 * "games" is not a different shelf from "Games".
 *
 * Local rather than strcasecmp: apps link -nostdlib against the syscall table,
 * which carries strcmp and strlen and no more of <string.h> than that, so
 * reaching for one more libc function is an ABI change and this is four lines.
 */
static bool eq_nocase(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') { ca = (char)(ca + 32); }
        if (cb >= 'A' && cb <= 'Z') { cb = (char)(cb + 32); }
        if (ca != cb) {
            return false;
        }
        if (!ca) {
            return true;
        }
    }
}

static int cat_of(const neos_app_t *app)
{
    if (app->category[0]) {
        for (int i = 0; i < NCATS; i++) {
            if (eq_nocase(app->category, CATS[i])) {
                return i;
            }
        }
    }
    return CAT_OTHER;
}

/* ------------------------------------------------------------------ */
/* What is on the card, filed                                          */
/* ------------------------------------------------------------------ */

/*
 * Registry indices, per shelf. Indices and not pointers: the registry is
 * rebuilt under this app whenever a card changes, and a neos_app_t* taken
 * before that points into the slot of whatever now sits there.
 */
static uint8_t s_shelf[NCATS][NEOS_APPS_MAX];
static uint8_t s_nshelf[NCATS];
static int     s_total;

static void shelves_build(void)
{
    for (int c = 0; c < NCATS; c++) {
        s_nshelf[c] = 0;
    }
    s_total = 0;

    /*
     * The shell leaves itself out. It is an app on the card like any other,
     * so it comes back in the registry, but a card you can tap to launch the
     * thing you are already looking at is just a way to lose your place.
     *
     * Matched on the directory, not the name: the name is display text from
     * the manifest and nothing stops two apps sharing one.
     */
    const char *self = neos_app_self();

    const int n = neos_apps_count();
    for (int i = 0; i < n && i < NEOS_APPS_MAX; i++) {
        const neos_app_t *app = neos_apps_get(i);
        if (!app || (self && strcmp(app->dir, self) == 0)) {
            continue;
        }
        const int c = cat_of(app);
        s_shelf[c][s_nshelf[c]++] = (uint8_t)i;
        s_total++;

        /*
         * Both what the manifest said and where it landed. An app whose card
         * copy of manifest.json predates categories is filed under Other and
         * looks exactly like one that was categorised wrongly; printing the
         * empty string it actually read is the difference between the two.
         */
        printf("[launcher] %-12s category \"%s\" -> %s\n",
               app->dir, app->category, CATS[c]);
    }
}

/* ------------------------------------------------------------------ */
/* Where we were last time                                             */
/* ------------------------------------------------------------------ */

/*
 * The shell is restarted every time an app exits, so "which tab" is not a
 * session's worth of state - it is answered again several times a minute, and
 * answering it with "the first one" each time makes the strip something to be
 * got past rather than something to navigate with.
 *
 * On the card and not in NVS: the card is where an app's settings live, NVS is
 * the firmware's, and a shell that files its preference alongside the clock's
 * is one a different card can disagree with, which is the point of the card.
 */
#define PREFS_PATH "apps/launcher/launcher.cfg"
#define PREFS_MAX  128
#define KEY_TAB    "tab"

static int s_tab;

/** True if the card named a shelf we know. */
static bool prefs_load(void)
{
    char buf[PREFS_MAX + 1];

    const int n = neos_file_read(PREFS_PATH, buf, PREFS_MAX);
    if (n <= 0) {
        return false;       /* -3 is "no such file", which is every first run */
    }
    buf[n] = 0;

    /*
     * "tab = Games". One setting, so the value is whatever follows the first
     * '=' - which is a whole parser's worth of behaviour for a file with one
     * line in it, and leaves the file something a person can edit.
     */
    char *p = buf;
    while (*p && *p != '=' && *p != '\n') {
        p++;
    }
    if (*p != '=') {
        return false;
    }
    for (p++; *p == ' ' || *p == '\t'; p++) { }

    char *e = p;
    while (*e && *e != '\n' && *e != '\r') {
        e++;
    }
    *e = 0;

    /*
     * Stored by name rather than by index. The strip is a list in this file
     * and lists get reordered; an index that outlived a reorder would reopen
     * the shell on a different shelf than the one it saved, which is worse
     * than not having remembered at all.
     */
    for (int c = 0; c < NCATS; c++) {
        if (eq_nocase(p, CATS[c])) {
            s_tab = c;
            return true;
        }
    }
    return false;
}

static void prefs_save(void)
{
    char out[PREFS_MAX];
    const int n = snprintf(out, sizeof(out), "%s = %s\n", KEY_TAB, CATS[s_tab]);
    if (n <= 0 || n >= (int)sizeof(out)) {
        return;
    }
    /* A card that will not take it is not worth stopping over: the shell works
       either way and the only thing lost is where it opens next time. */
    const int err = neos_file_write(PREFS_PATH, out, (size_t)n);
    if (err != 0) {
        printf("[launcher] could not save the tab (%d) - carrying on\n", err);
    }
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static ngl_rect_t s_area;               /* what the OS is currently giving us */
static ngl_rect_t s_strip, s_grid;

static int  s_cols = 1;
static int16_t s_card_w;

/* Tab geometry in strip content space - x=0 is the left end of the strip,
   which is not where it is on screen once the strip has been dragged. */
static int16_t s_tab_x[NCATS], s_tab_w[NCATS];
static int16_t s_strip_cw;              /* content width of the whole strip */
static int16_t s_tab_scroll;

/* Kept per shelf: switching tabs to check something and coming back to find
   the list scrolled to the top is losing your place for no reason. */
static int16_t s_scroll[NCATS];

static int16_t clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) { v = lo; }
    if (v > hi) { v = hi; }
    return (int16_t)v;
}

static void layout(void)
{
    s_area  = ngl_app_area();
    s_strip = ngl_rect(s_area.x, s_area.y, s_area.w, TAB_H);
    s_grid  = ngl_rect(s_area.x, (int16_t)(s_area.y + TAB_H),
                       s_area.w, (int16_t)(s_area.h - TAB_H));

    /*
     * Two columns when the tablet is on its side and one when it is upright,
     * decided from the shape of the area rather than from the rotation: the
     * app area is what the cards have to fit in, and it is also the only one
     * of the two that a future system bar down one edge would change.
     */
    s_cols = (s_area.w > s_area.h) ? 2 : 1;
    s_card_w = (int16_t)((s_grid.w - 2 * UI_MARGIN - (s_cols - 1) * UI_CARD_GAP)
                         / s_cols);

    /*
     * Tabs are as wide as their labels need. Six of them at their natural
     * width is wider than 720 px of portrait and narrower than 1280 of
     * landscape, so the strip has to do both: when they fit, the slack is
     * shared out so the strip fills the width and reads as a strip rather than
     * as six buttons that stopped early; when they do not, it scrolls.
     */
    int16_t natural = 0;
    for (int i = 0; i < NCATS; i++) {
        s_tab_w[i] = (int16_t)(ngl_text_width(&ngl_font_small, CATS[i])
                               + 2 * TAB_PAD);
        natural = (int16_t)(natural + s_tab_w[i]);
    }
    if (natural < s_strip.w) {
        const int16_t extra = (int16_t)((s_strip.w - natural) / NCATS);
        for (int i = 0; i < NCATS; i++) {
            s_tab_w[i] = (int16_t)(s_tab_w[i] + extra);
        }
        /* The division loses up to NCATS-1 px; the last tab takes them, so
           the strip ends exactly at the edge and not a hairline short. */
        s_tab_w[NCATS - 1] = (int16_t)(s_tab_w[NCATS - 1]
                                       + (s_strip.w - natural) % NCATS);
    }

    int16_t x = 0;
    for (int i = 0; i < NCATS; i++) {
        s_tab_x[i] = x;
        x = (int16_t)(x + s_tab_w[i]);
    }
    s_strip_cw = x;
}

static int16_t tab_scroll_max(void)
{
    return s_strip_cw > s_strip.w ? (int16_t)(s_strip_cw - s_strip.w) : 0;
}

/** How tall the current shelf is, laid out. */
static int16_t grid_content_h(int tab)
{
    const int rows = (s_nshelf[tab] + s_cols - 1) / s_cols;
    return (int16_t)(UI_GRID_TOP + rows * (UI_CARD_H + UI_CARD_GAP));
}

static int16_t grid_scroll_max(int tab)
{
    const int32_t over = grid_content_h(tab) - s_grid.h;
    return over > 0 ? (int16_t)over : 0;
}

static ngl_rect_t tab_rect(int i)
{
    return ngl_rect((int16_t)(s_strip.x + s_tab_x[i] - s_tab_scroll),
                    s_strip.y, s_tab_w[i], TAB_H);
}

/** Where the k'th card of the current shelf sits, scrolling included. */
static ngl_rect_t card_rect(int k)
{
    const int col = k % s_cols;
    const int row = k / s_cols;
    return ngl_rect((int16_t)(s_grid.x + UI_MARGIN
                              + col * (s_card_w + UI_CARD_GAP)),
                    (int16_t)(s_grid.y + UI_GRID_TOP
                              + row * (UI_CARD_H + UI_CARD_GAP)
                              - s_scroll[s_tab]),
                    s_card_w, UI_CARD_H);
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

static void paint_strip(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    ngl_fill_rect(sc, s_strip, COL_BG);

    /* The rule first, so the selected tab's accent bar sits in it rather than
       under it: the line runs the width of the strip and is broken by the one
       tab that is current. */
    ngl_hline(sc, s_strip.x, (int16_t)(s_strip.y + s_strip.h - 1),
              s_strip.w, TH_RULE);

    /* Clipped, because a scrolled strip has a tab hanging off each end and
       half a tab is exactly the right thing to see there - it is what says
       the strip goes on. */
    ngl_clip_set(sc, &s_strip);
    for (int i = 0; i < NCATS; i++) {
        const ngl_rect_t t = tab_rect(i);
        if (t.x + t.w <= s_strip.x || t.x >= s_strip.x + s_strip.w) {
            continue;
        }
        const bool on = (i == s_tab);

        /*
         * Each tab is a box, and the boxes run down onto the rule: the bottom
         * edge is drawn over that row, so the strip reads as a row of tabs
         * standing on a line rather than as labels floating above one.
         */
        const ngl_rect_t box = ngl_rect((int16_t)(t.x + TAB_INSET),
                                        (int16_t)(t.y + TAB_TOP),
                                        (int16_t)(t.w - 2 * TAB_INSET),
                                        (int16_t)(t.h - TAB_TOP));

        /*
         * Current is filled and outlined in accent, the rest are outlines. In
         * a one-hue palette a fill on its own reads as a rendering artefact,
         * and an outline on its own is what every tab already has - it takes
         * both for which one is current to survive being glanced at.
         */
        if (on) {
            ngl_fill_rect(sc, box, TH_PANEL);
        }
        ngl_draw_rect(sc, box, on ? COL_ACCENT : COL_EDGE, 2);

        const int16_t tw = ngl_text_width(&ngl_font_small, CATS[i]);
        ngl_text(sc, (int16_t)(box.x + (box.w - tw) / 2),
                 (int16_t)(box.y + (box.h - ngl_font_small.height) / 2),
                 CATS[i], &ngl_font_small,
                 on ? TH_GLOW : (s_nshelf[i] ? COL_DIM : TH_TEXT_FAINT));
    }
    ngl_clip_set(sc, NULL);
}

static void paint_card(int k, const neos_app_t *app)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const ngl_rect_t card = card_rect(k);

    /*
     * Outline, not a fill. Square corners on purpose: a rounded outline is
     * emitted as one span per scanline, which merges straight back into the
     * bounding box, whereas four straight edges stay four small dirty rects -
     * and boxy suits the terminal look anyway.
     */
    ngl_draw_rect(sc, card, COL_EDGE, 2);

    /* The glyph carries the status, so every icon can share the theme's one
       icon green - no traffic-light colours in a monochrome UI. */
    const ngl_icon_t *ic;
    const char *note;
    if (app->crashes) {
        ic = ngl_icon_find("bomb", 32);
        note = "quarantined after a crash";
    } else if (!app->ok) {
        ic = ngl_icon_find("dizzy", 32);
        note = "manifest unreadable";
    } else {
        ic = ngl_icon_find("smile", 32);
        note = app->desc;
    }
    if (ic) {
        ngl_icon(sc, (int16_t)(card.x + 22), (int16_t)(card.y + 44), ic, TH_ICON);
    }

    /*
     * Text is clipped to its own card, not just to the grid. Two columns
     * halves the width a name has, and a long one that overflowed would run
     * into the card beside it rather than off the screen - which is the one
     * kind of overflow that looks like a drawing bug instead of a long name.
     *
     * Intersected with the grid by hand, because ngl_clip_set() replaces the
     * clip rather than narrowing it: a card scrolled half under the tab strip
     * would otherwise get its name back, drawn across the tabs.
     */
    const int16_t tx = (int16_t)(card.x + 74);
    ngl_rect_t text = ngl_rect(tx, card.y, (int16_t)(card.w - 74 - 12), card.h);
    if (!ngl_rect_intersect(&text, &s_grid, &text)) {
        return;
    }
    ngl_clip_set(sc, &text);
    ngl_text(sc, tx, (int16_t)(card.y + 16),
             app->name, &ngl_font_large, COL_TEXT);
    if (note && note[0]) {
        ngl_text(sc, tx, (int16_t)(card.y + 72), note,
                 &ngl_font_small, COL_DIM);
    }
    ngl_clip_set(sc, &s_grid);
}

static void paint_scrollbar(void)
{
    ngl_surface_t *sc = ngl_screen();
    const int16_t max = grid_scroll_max(s_tab);
    if (!sc || max <= 0) {
        return;
    }
    const int16_t content = grid_content_h(s_tab);
    const ngl_rect_t track = ngl_rect((int16_t)(s_grid.x + s_grid.w - SCROLLBAR_W - 4),
                                      (int16_t)(s_grid.y + UI_GRID_TOP),
                                      SCROLLBAR_W,
                                      (int16_t)(s_grid.h - 2 * UI_GRID_TOP));
    if (track.h <= 0) {
        return;
    }
    int16_t th = (int16_t)((int32_t)track.h * s_grid.h / content);
    if (th < 24) { th = 24; }
    if (th > track.h) { th = track.h; }
    const int16_t ty = (int16_t)(track.y
                                 + (int32_t)(track.h - th) * s_scroll[s_tab] / max);

    ngl_fill_rect(sc, track, TH_RULE);
    ngl_fill_rect(sc, ngl_rect(track.x, ty, track.w, th), COL_EDGE);
}

static void paint_grid(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    ngl_fill_rect(sc, s_grid, COL_BG);
    ngl_clip_set(sc, &s_grid);

    for (int k = 0; k < s_nshelf[s_tab]; k++) {
        const ngl_rect_t r = card_rect(k);
        if (r.y + r.h <= s_grid.y || r.y >= s_grid.y + s_grid.h) {
            continue;                   /* scrolled out; nothing to draw */
        }
        const neos_app_t *app = neos_apps_get(s_shelf[s_tab][k]);
        if (app) {
            paint_card(k, app);
        }
    }

    ngl_clip_set(sc, NULL);

    if (s_nshelf[s_tab] == 0) {
        /*
         * An empty shelf and an empty card are different things and are worth
         * different sentences: one is "look on another tab", the other is
         * "there is nothing to look at".
         */
        char msg[64];
        const char *say = "No other apps on this card";
        if (s_total) {
            snprintf(msg, sizeof(msg), "Nothing on the %s shelf", CATS[s_tab]);
            say = msg;
        }
        ngl_text(sc, (int16_t)(s_grid.x + UI_MARGIN),
                 (int16_t)(s_grid.y + UI_GRID_TOP + 20),
                 say, &ngl_font_small, COL_DIM);
    }

    paint_scrollbar();
}

/*
 * Redrawn from the registry, not from a local copy. Rotating invalidates the
 * whole framebuffer, and NeOS already holds the scan result - a second copy
 * here would only be a way to disagree with it.
 *
 * One flush for the whole screen. Flushing per card rotated several
 * overlapping regions instead of one.
 */
static void repaint_all(void)
{
    paint_strip();
    paint_grid();
    ngl_flush();
}

static void relayout(void)
{
    layout();
    s_tab_scroll = clampi(s_tab_scroll, 0, tab_scroll_max());
    for (int c = 0; c < NCATS; c++) {
        s_scroll[c] = clampi(s_scroll[c], 0, grid_scroll_max(c));
    }
    repaint_all();
}

/* ------------------------------------------------------------------ */
/* Gestures                                                            */
/* ------------------------------------------------------------------ */

/*
 * NeOS delivers taps but not drags, and this screen needs both from the same
 * press, so all of it is worked out here from the raw finger position and the
 * OS's taps are drained and thrown away. Doing it the other way round - taking
 * the OS's taps and watching for drags alongside - means a drag that ends
 * where it started still arrives as a tap, and a slide back to the tab you
 * were on would launch whatever ended up under your finger.
 */
enum { G_IDLE = 0, G_TABS, G_GRID, G_DEAD };

static int8_t  s_g;
static bool    s_down_prev;
static int16_t s_gx0, s_gy0;            /* where the press landed */
static int16_t s_g0;                    /* what the scroll was when it did */
static bool    s_moved;
static uint32_t s_down_at;              /* when the press landed */
static bool    s_hold_tried;            /* one hold attempt per press */
static bool    s_held;                  /* ... and it did something */

/* Set when this shell is why the registry moved, so the redraw that follows
   is not announced as a change somebody else made to the card. */
static bool    s_self_change;

/* Set once neos_exec() has been asked for. A flag and not a gesture state,
   because the release that launched an app goes on to finish the gesture and
   would clear any state set inside it. */
static bool    s_launching;

static int16_t iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

static void tab_select(int i)
{
    if (i == s_tab || i < 0 || i >= NCATS) {
        return;
    }
    s_tab = i;
    /* Both halves: the strip because the current box moved, the grid because
       this shelf has its own contents and its own scroll position. */
    repaint_all();

    /*
     * Written on the change and not on the way out. The shell does not get a
     * tidy exit to save in: tapping a card returns from main() immediately,
     * and the close button is a flag this loop notices whenever it next looks.
     */
    prefs_save();
}

static void tap(int16_t x, int16_t y)
{
    if (ngl_rect_contains(&s_strip, x, y)) {
        for (int i = 0; i < NCATS; i++) {
            const ngl_rect_t t = tab_rect(i);
            if (ngl_rect_contains(&t, x, y)) {
                tab_select(i);
                return;
            }
        }
        return;
    }

    if (!ngl_rect_contains(&s_grid, x, y)) {
        return;
    }
    for (int k = 0; k < s_nshelf[s_tab]; k++) {
        const ngl_rect_t r = card_rect(k);
        if (!ngl_rect_contains(&r, x, y)) {
            continue;
        }
        const neos_app_t *app = neos_apps_get(s_shelf[s_tab][k]);
        if (!app) {
            return;
        }
        /*
         * A quarantined app is not launched by tapping it. NeOS refuses to run
         * it anyway, so the tap would be a handover that immediately came back
         * here; saying so is more use than a flicker.
         */
        if (app->crashes) {
            neos_status_for("quarantined - hold it to let it run again", 3000);
            return;
        }
        printf("[launcher] launching %s\n", app->dir);
        neos_exec(app->dir);
        s_launching = true;
        return;
    }
}

/*
 * A press that stood still on a card for HOLD_MS: the card's second action.
 *
 * There is exactly one, and only quarantined cards have it. An app that
 * crashed is refused by NeOS from then on, which is right for an app that
 * crashes every time and wrong for the usual case - one that crashed once and
 * has since been rebuilt and uploaded over the top of itself. Without a way
 * back the only cure is a rebuild of the *firmware*, because the counter lives
 * in NVS and nothing on the card can reach it.
 *
 * Returns whether it did anything, and that is what decides whether the
 * release still counts as a tap: a slow press on a healthy card has always
 * launched it and still does, so nothing changes for a card with no second
 * action to offer.
 */
static bool card_hold(int16_t x, int16_t y)
{
    if (!ngl_rect_contains(&s_grid, x, y)) {
        return false;
    }
    for (int k = 0; k < s_nshelf[s_tab]; k++) {
        const ngl_rect_t r = card_rect(k);
        if (!ngl_rect_contains(&r, x, y)) {
            continue;
        }
        const neos_app_t *app = neos_apps_get(s_shelf[s_tab][k]);
        if (!app || !app->crashes || !neos_apps_unquarantine(app->dir)) {
            return false;
        }
        char msg[80];
        snprintf(msg, sizeof msg, "%s may run again", app->name);
        neos_status_for(msg, 3000);
        printf("[launcher] %s let out of quarantine\n", app->dir);

        /*
         * The registry moved, so the loop below will redraw the card with its
         * bomb replaced. It must not also announce that the card changed -
         * this shell is the thing that changed it, and it has already said so
         * in words that mean something.
         */
        s_self_change = true;
        return true;
    }
    return false;
}

static void gesture_poll(void)
{
    neos_touch_t t;
    int16_t junk_x = 0, junk_y = 0;

    /* NeOS's own tap detector is still running; collected and discarded so a
       stale one cannot arrive minutes later. */
    while (neos_touch_tap(&junk_x, &junk_y)) { }

    if (!neos_touch(&t)) {
        return;                         /* no touch panel: the shell still draws */
    }

    if (t.down && !s_down_prev) {
        s_gx0 = t.x;
        s_gy0 = t.y;
        s_moved = false;
        s_down_at    = (uint32_t)neos_uptime_ms();
        s_hold_tried = false;
        s_held       = false;
        if (ngl_rect_contains(&s_strip, t.x, t.y)) {
            s_g  = G_TABS;
            s_g0 = s_tab_scroll;
        } else if (ngl_rect_contains(&s_grid, t.x, t.y)) {
            s_g  = G_GRID;
            s_g0 = s_scroll[s_tab];
        } else {
            s_g = G_DEAD;               /* the system bar is not ours */
        }
    } else if (t.down) {
        /*
         * One axis each, chosen by where the press landed rather than by which
         * way the finger went. The strip only has a left and a right and the
         * grid only has an up and a down, so a diagonal drag on either is a
         * drag along the one axis that exists there - which is what makes a
         * sloppy sideways swipe on the tabs still slide the tabs.
         */
        if (s_g == G_TABS) {
            const int16_t dx = (int16_t)(t.x - s_gx0);
            if (iabs16(dx) > SLOP) {
                s_moved = true;
            }
            if (s_moved) {
                const int16_t v = clampi(s_g0 - dx, 0, tab_scroll_max());
                if (v != s_tab_scroll) {
                    s_tab_scroll = v;
                    paint_strip();
                    ngl_flush();
                }
            }
        } else if (s_g == G_GRID) {
            const int16_t dy = (int16_t)(t.y - s_gy0);
            if (iabs16(dy) > SLOP) {
                s_moved = true;
            }
            /*
             * Fired while the finger is still down, which is the point: a hold
             * that only reported itself on release would be indistinguishable
             * from a slow tap until it was too late to let go.
             */
            if (!s_moved && !s_hold_tried &&
                (uint32_t)neos_uptime_ms() - s_down_at >= HOLD_MS) {
                s_hold_tried = true;
                s_held = card_hold(s_gx0, s_gy0);
            }
            if (s_moved) {
                const int16_t v = clampi(s_g0 - dy, 0, grid_scroll_max(s_tab));
                if (v != s_scroll[s_tab]) {
                    s_scroll[s_tab] = v;
                    paint_grid();
                    ngl_flush();
                }
            }
        }
    } else if (s_down_prev) {
        if (!s_moved && !s_held && s_g != G_DEAD) {
            tap(s_gx0, s_gy0);          /* where it started, not where it let go */
        }
        s_g = G_IDLE;
    }

    s_down_prev = t.down;
}

/* ------------------------------------------------------------------ */

/*
 * The watcher runs on its own task, so it says the screen moved and leaves it
 * at that. Repainting from here would have two tasks drawing into the back
 * buffer at once, and the one holding a layout measured from the old app area
 * would be the loop below.
 */
static volatile bool s_rotated;

static void on_rotate(ngl_rotation_t r)
{
    printf("[launcher] rotated to %s\n", neos_orient_name(r));
    s_rotated = true;
}

static bool area_moved(void)
{
    const ngl_rect_t a = ngl_app_area();
    return a.x != s_area.x || a.y != s_area.y || a.w != s_area.w || a.h != s_area.h;
}

int main(int argc, char **argv)
{
    printf("[launcher] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[launcher] no screen, nothing to show\n");
        return 0;
    }

    shelves_build();

    /*
     * Open where we were left. Failing that - a fresh card, or a shelf that
     * has since been renamed - the first one with something on it: the strip
     * is fixed and "System" is first whether or not anything has claimed it,
     * so tab zero would usually mean opening on the one screen with no apps.
     *
     * The saved shelf is honoured even when it is now empty. It is still the
     * answer the user gave, and "nothing on the Games shelf" is a truthful
     * screen; quietly moving them somewhere else is how a remembered position
     * stops being worth having.
     */
    if (!prefs_load()) {
        for (int c = 0; c < NCATS; c++) {
            if (s_nshelf[c]) {
                s_tab = c;
                break;
            }
        }
    }

    relayout();

    char msg[64];
    snprintf(msg, sizeof(msg), "%d app%s on this card",
             s_total, s_total == 1 ? "" : "s");
    neos_status_for(msg, 4000);

    /* Follow the tablet from here on. */
    neos_orient_start_watch(3000, on_rotate);

    /*
     * The event loop. A tap on a card asks NeOS to run that app and then
     * returns - one app is resident at a time, so handing over is exactly
     * "say what you want next, then get out of the way". NeOS brings this
     * shell back up when that app closes.
     */
    uint32_t seen = neos_apps_generation();
    bool     was_busy = false;

    for (;;) {
        if (neos_app_close_requested()) {
            printf("[launcher] closing\n");
            return 0;
        }

        /*
         * Sit out a system panel entirely.
         *
         * Not an optimisation. Underneath one, draws are dropped and a captured
         * finger reads as no finger - so a press held on a card when the Wi-Fi
         * list opens over it arrives here as a release, and a release that
         * never travelled is a tap. Without this, opening a panel with a second
         * finger down would launch whatever that finger was resting on.
         *
         * Anything missed while it was up is caught on the way out: the panel
         * restores the pixels it covered, which are the ones from before it
         * opened, so a card uploaded meanwhile would otherwise not be there.
         */
        if (neos_ui_busy()) {
            was_busy   = true;
            s_g        = G_DEAD;
            s_down_prev = false;
            neos_sleep_ms(50);
            continue;
        }
        if (was_busy) {
            was_busy = false;
            shelves_build();
            relayout();
        }

        /* An upload lands on the card while this is running, so the list is
           redrawn whenever NeOS says the registry moved. */
        const uint32_t now = neos_apps_generation();
        if (now != seen) {
            seen = now;
            printf("[launcher] card changed, redrawing\n");
            shelves_build();
            relayout();
            /*
             * And say so. An upload that replaces an app already on the card
             * changes nothing on screen, so a list that correctly redrew
             * identically looks exactly like one that never noticed.
             *
             * Unless this shell is what moved it - a card just let out of
             * quarantine has already said something truer than "card changed".
             */
            if (s_self_change) {
                s_self_change = false;
            } else {
                neos_status_for("card changed", 2000);
            }
        }

        /* The rotation itself is the watcher's; the new shape of the app area
           is what this loop acts on, and it is checked either way because a
           panel closing can move the area without any rotation at all. */
        if (s_rotated || area_moved()) {
            s_rotated = false;
            relayout();
            printf("[launcher] relaid out for %dx%d, %d column%s\n",
                   s_area.w, s_area.h, s_cols, s_cols == 1 ? "" : "s");
        }

        gesture_poll();
        if (s_launching) {
            /* neos_exec() has been asked for; the handover is this return. */
            return 0;
        }

        neos_sleep_ms(15);
    }
}
