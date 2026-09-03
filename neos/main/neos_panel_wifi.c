/*
 * The Wi-Fi list.
 *
 * A scan, a list of names with live signal, and the two things you can do to
 * one: join it, or forget it. Joining a network that needs a password raises
 * the system keyboard over this panel - which is the reason overlays and touch
 * capture both nest, because this panel has to still be here underneath and
 * still be the thing that gets its screen back.
 *
 * The list is re-scanned every few seconds for as long as the panel is open,
 * so the signal figures move while you decide. Nothing else on the tablet
 * scans continuously; it costs radio time and it is only worth it while
 * somebody is looking at the numbers.
 *
 * Scrolling is by dragging the list, not by buttons. The finger is already
 * being tracked for the keyboard, and a drag is distinguishable from a tap by
 * the same movement tolerance the touch driver already applies - so a list
 * that scrolls costs the state below and nothing in the input path.
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
#include "neos_net.h"
#include "neos_panel.h"
#include "neos_touch.h"
#include "neos_ui.h"

static const char *TAG = "wifipanel";

#define POLL_MS     20
#define TICK_MS     500
#define RESCAN_MS   5000

#define MARGIN      16
#define HEAD_H      52
#define ROW_H       58
#define FOOT_H      64
#define BTN_W       160
#define FORGET_W    110
#define BARS_W      64
#define GAP         10

/* How far a finger travels before the drag stops being a tap. The same number
   the touch driver uses, so the two cannot disagree about which happened. */
#define DRAG_SLOP   24

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static ngl_rect_t s_panel, s_content, s_head, s_list, s_foot;
static ngl_rect_t s_rescan_btn, s_disc_btn;

static neos_net_ap_t s_ap[NEOS_NET_SCAN_MAX];
static int           s_nap;
static int16_t       s_scroll;      /* pixels the list is pushed up by */

/*
 * What is painted, so a tick can tell whether to paint again.
 *
 * The list is re-scanned every few seconds and re-read four times a second,
 * and almost every one of those reads is identical to the last. Without this
 * the panel repaints its whole 1256x637 surface twice a second, which on a
 * software blitter is 40 ms of PPA time each and a list that visibly crawls.
 * With it, a row is redrawn when that row's own signal moves.
 */
static neos_net_ap_t s_shown[NEOS_NET_SCAN_MAX];
static int           s_shown_n;
static int16_t       s_shown_scroll;
static bool          s_shown_valid;
static char          s_shown_head[96];
static char          s_shown_foot[64];
static char          s_shown_on[33];

static void layout(void)
{
    const ngl_rect_t app = ngl_app_area();
    s_panel = ngl_rect((int16_t)(app.x + 12), (int16_t)(app.y + 12),
                       (int16_t)(app.w - 24), (int16_t)(app.h - 24));

    s_content = ngl_rect((int16_t)(s_panel.x + 2),
                         (int16_t)(s_panel.y + NEOS_UI_TITLE_H + 1),
                         (int16_t)(s_panel.w - 4),
                         (int16_t)(s_panel.h - NEOS_UI_TITLE_H - 3));

    const int16_t x = (int16_t)(s_content.x + MARGIN);
    const int16_t w = (int16_t)(s_content.w - 2 * MARGIN);

    s_head = ngl_rect(x, (int16_t)(s_content.y + 4), w, HEAD_H);
    s_foot = ngl_rect(x, (int16_t)(s_content.y + s_content.h - FOOT_H), w, FOOT_H);
    s_list = ngl_rect(x, (int16_t)(s_head.y + s_head.h + 4), w,
                      (int16_t)(s_foot.y - s_head.y - s_head.h - 8));

    const int16_t bh = (int16_t)(FOOT_H - 16);
    s_disc_btn   = ngl_rect((int16_t)(s_foot.x + s_foot.w - BTN_W),
                            (int16_t)(s_foot.y + 8), BTN_W, bh);
    s_rescan_btn = ngl_rect((int16_t)(s_disc_btn.x - GAP - BTN_W),
                            (int16_t)(s_foot.y + 8), BTN_W, bh);
}

/** Where row @p i sits right now, given the scroll. May be off the list. */
static ngl_rect_t row_rect(int i)
{
    return ngl_rect(s_list.x, (int16_t)(s_list.y + i * ROW_H - s_scroll),
                    s_list.w, ROW_H);
}

static ngl_rect_t forget_rect(const ngl_rect_t *row)
{
    return ngl_rect((int16_t)(row->x + row->w - FORGET_W),
                    (int16_t)(row->y + 8), FORGET_W, (int16_t)(ROW_H - 16));
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/*
 * Signal as four bars.
 *
 * The thresholds are the usual ones and they are a display decision, which is
 * why they are here and not in neos_net.h: -55 is "in the room", -67 is "works
 * fine", -75 is "works", below that is "will drop". Nothing else in the system
 * has an opinion about what a dBm is worth.
 */
static int bars_for(int rssi)
{
    if (rssi >= -55) { return 4; }
    if (rssi >= -67) { return 3; }
    if (rssi >= -75) { return 2; }
    return 1;
}

static void paint_bars(ngl_surface_t *s, ngl_rect_t box, int rssi)
{
    const int n = bars_for(rssi);
    const int16_t bw = 10, gap = 4;
    const int16_t base = (int16_t)(box.y + box.h - 10);

    for (int i = 0; i < 4; i++) {
        const int16_t h = (int16_t)(8 + i * 7);
        const ngl_rect_t b = ngl_rect((int16_t)(box.x + i * (bw + gap)),
                                      (int16_t)(base - h), bw, h);
        if (i < n) {
            ngl_fill_rect(s, b, TH_ACCENT);
        } else {
            ngl_draw_rect(s, b, TH_RULE, 1);
        }
    }
}

static void paint_row(ngl_surface_t *s, int i, bool joined)
{
    const ngl_rect_t row = row_rect(i);
    const neos_net_ap_t *a = &s_ap[i];

    ngl_fill_rect(s, row, joined ? TH_PANEL : TH_MODAL_BG);
    ngl_hline(s, row.x, (int16_t)(row.y + ROW_H - 1), row.w, TH_RULE);

    ngl_rect_t bars = ngl_rect(row.x, row.y, BARS_W, ROW_H);
    paint_bars(s, bars, a->rssi);

    /*
     * The name gets whatever is left after the signal, the badges and the
     * forget button, and is clipped to it. SSIDs are 32 bytes and this layout
     * has no idea how long the one in front of it is.
     */
    ngl_rect_t name = ngl_rect((int16_t)(row.x + BARS_W + GAP), row.y,
                               (int16_t)(row.w - BARS_W - GAP -
                                         (a->known ? FORGET_W + GAP : 0) - 150),
                               ROW_H);
    neos_ui_text_left(s, name, a->ssid, &ngl_font_small,
                      joined ? TH_GLOW : TH_TEXT);

    /* The two things about a network that are not its name: whether it will
       ask for a password, and how strong it is as a number, for when four bars
       is not a fine enough distinction to choose between two of them. */
    char badge[32];
    snprintf(badge, sizeof(badge), "%s  %d",
             a->secure ? "lock" : "open", a->rssi);
    ngl_rect_t meta = ngl_rect((int16_t)(name.x + name.w), row.y, 150, ROW_H);
    neos_ui_text_right(s, meta, badge, &ngl_font_small, TH_TEXT_DIM);

    if (a->known) {
        neos_ui_button(s, forget_rect(&row), "forget", false);
    }
}

static void head_text(char *line, size_t n)
{
    switch (neos_net_state()) {
    case NEOS_NET_ABSENT:
        snprintf(line, n, "no radio - the C6 did not answer");
        break;
    case NEOS_NET_OFF:
        snprintf(line, n, "the radio is off");
        break;
    case NEOS_NET_CONNECTING:
        snprintf(line, n, "joining %s...", neos_net_ssid());
        break;
    case NEOS_NET_ONLINE:
        snprintf(line, n, "%s   %s   %d dBm",
                 neos_net_ssid(), neos_net_ip(), neos_net_rssi());
        break;
    default:
        snprintf(line, n, "%s", neos_net_scanning() ? "scanning..."
                                                    : "not connected");
        break;
    }
}

static void paint_head(ngl_surface_t *s)
{
    ngl_fill_rect(s, s_head, TH_MODAL_BG);

    char line[96];
    head_text(line, sizeof(line));

    const ngl_color_t c = neos_net_state() == NEOS_NET_ONLINE ? TH_OK
                        : neos_net_state() == NEOS_NET_ABSENT ? TH_BAD
                                                              : TH_TEXT_DIM;
    neos_ui_text_left(s, s_head, line, &ngl_font_small, c);
    ngl_hline(s, s_head.x, (int16_t)(s_head.y + s_head.h - 1), s_head.w, TH_RULE);
}

static void foot_text(char *count, size_t n)
{
    snprintf(count, n, "%d network%s, %d saved%s",
             s_nap, s_nap == 1 ? "" : "s", neos_net_known_count(),
             neos_net_scanning() ? ", scanning" : "");
}

static void paint_foot(ngl_surface_t *s)
{
    ngl_fill_rect(s, s_foot, TH_MODAL_BG);
    ngl_hline(s, s_foot.x, s_foot.y, s_foot.w, TH_RULE);

    char count[64];
    foot_text(count, sizeof(count));
    ngl_rect_t left = s_foot;
    left.w = (int16_t)(s_rescan_btn.x - left.x - GAP);
    neos_ui_text_left(s, left, count, &ngl_font_small, TH_TEXT_FAINT);

    neos_ui_button(s, s_rescan_btn,
                   neos_net_scanning() ? "scanning" : "Rescan",
                   neos_net_scanning());
    neos_ui_button(s, s_disc_btn, "Disconnect", false);
}

static void paint_list(ngl_surface_t *s)
{
    /* Clipped to the list, so a row that is half scrolled off stops at the
       boundary instead of drawing over the header. */
    const ngl_rect_t saved = ngl_surface_clip(s);
    ngl_rect_t clip;
    if (!ngl_rect_intersect(&s_list, &saved, &clip)) {
        return;
    }
    ngl_clip_set(s, &clip);
    ngl_fill_rect(s, s_list, TH_MODAL_BG);

    const char *on = neos_net_ssid();
    for (int i = 0; i < s_nap; i++) {
        const ngl_rect_t r = row_rect(i);
        if (r.y + ROW_H < s_list.y || r.y > s_list.y + s_list.h) {
            continue;
        }
        paint_row(s, i, on[0] && strcmp(on, s_ap[i].ssid) == 0);
    }

    if (s_nap == 0) {
        neos_ui_text_centred(s, s_list,
                             neos_net_state() == NEOS_NET_ABSENT
                                 ? "nothing to scan with"
                                 : "no networks found",
                             &ngl_font_small, TH_TEXT_FAINT);
    }
    ngl_clip_set(s, &saved);

    /*
     * A scrollbar, only when there is something to scroll.
     *
     * Two dozen networks in a list that shows eight is a list whose top is not
     * where you are, and nothing else on the panel says so.
     */
    const int16_t total = (int16_t)(s_nap * ROW_H);
    if (total > s_list.h) {
        const int16_t track_x = (int16_t)(s_list.x + s_list.w - 4);
        ngl_fill_rect(s, ngl_rect(track_x, s_list.y, 3, s_list.h), TH_RULE);
        const int16_t th = (int16_t)((int32_t)s_list.h * s_list.h / total);
        const int16_t ty = (int16_t)(s_list.y +
                                     (int32_t)(s_list.h - th) * s_scroll /
                                     (total - s_list.h));
        ngl_fill_rect(s, ngl_rect(track_x, ty, 3, th), TH_EDGE);
    }
}

/** Record the frame just painted, so the next tick can diff against it. */
static void remember_shown(void)
{
    for (int i = 0; i < s_nap && i < NEOS_NET_SCAN_MAX; i++) {
        s_shown[i] = s_ap[i];
    }
    s_shown_n = s_nap;
    s_shown_scroll = s_scroll;
    s_shown_valid = true;
    head_text(s_shown_head, sizeof(s_shown_head));
    foot_text(s_shown_foot, sizeof(s_shown_foot));
    strlcpy(s_shown_on, neos_net_ssid(), sizeof(s_shown_on));
}

/*
 * @p full repaints the backdrop as well as the panel.
 *
 * It is the expensive one - a copy of the whole screen back from the save
 * buffer, then a blend over everything outside the panel - and it is only
 * needed when the panel has moved or something has been drawn over it. The
 * signal figures update four times a second and none of that applies to them,
 * so the tick takes the other path and repaints an opaque rectangle.
 */
static void paint_all(bool full)
{
    if (full) {
        ngl_overlay_restore();
    }
    ngl_surface_t *s = ngl_overlay_begin(full ? ngl_app_area() : s_panel);
    if (!s) {
        return;
    }
    if (full) {
        neos_ui_dim(s);
    }
    neos_ui_frame(s, s_panel, "Wi-Fi");
    paint_head(s);
    paint_list(s);
    paint_foot(s);
    ngl_overlay_end();
    ngl_flush();

    remember_shown();
}

/**
 * Repaint only what has moved since the last frame.
 *
 * The rows are compared on the fields that are actually drawn - not on the
 * whole record - so a scan that returns the same networks at the same strength
 * costs one memcmp per row and no drawing at all.
 */
static void refresh(void)
{
    if (!s_shown_valid || s_shown_scroll != s_scroll || s_shown_n != s_nap) {
        paint_all(false);          /* everything moved: the list as a whole */
        return;
    }

    char head[96], foot[64];
    head_text(head, sizeof(head));
    foot_text(foot, sizeof(foot));

    const char *on = neos_net_ssid();
    bool rows = false;
    for (int i = 0; i < s_nap; i++) {
        if (s_ap[i].rssi != s_shown[i].rssi ||
            s_ap[i].known != s_shown[i].known ||
            strcmp(s_ap[i].ssid, s_shown[i].ssid) != 0) {
            rows = true;
            break;
        }
    }
    const bool joined_moved = strcmp(on, s_shown_on) != 0;
    const bool head_moved = strcmp(head, s_shown_head) != 0;
    const bool foot_moved = strcmp(foot, s_shown_foot) != 0;

    if (!rows && !joined_moved && !head_moved && !foot_moved) {
        return;
    }

    ngl_surface_t *s = ngl_overlay_begin(s_panel);
    if (!s) {
        return;
    }
    if (head_moved) {
        paint_head(s);
    }
    if (rows || joined_moved) {
        /* Clipped to the list, so a row that is half scrolled off stops at
           the boundary rather than drawing over the header. */
        const ngl_rect_t saved = ngl_surface_clip(s);
        ngl_rect_t clip;
        if (ngl_rect_intersect(&s_list, &saved, &clip)) {
            ngl_clip_set(s, &clip);
            for (int i = 0; i < s_nap; i++) {
                const ngl_rect_t r = row_rect(i);
                if (r.y + ROW_H < s_list.y || r.y > s_list.y + s_list.h) {
                    continue;
                }
                const bool was_on = s_shown_on[0] &&
                                    strcmp(s_shown_on, s_shown[i].ssid) == 0;
                const bool is_on = on[0] && strcmp(on, s_ap[i].ssid) == 0;
                if (was_on != is_on ||
                    s_ap[i].rssi != s_shown[i].rssi ||
                    s_ap[i].known != s_shown[i].known ||
                    strcmp(s_ap[i].ssid, s_shown[i].ssid) != 0) {
                    paint_row(s, i, is_on);
                }
            }
            ngl_clip_set(s, &saved);
        }
    }
    if (foot_moved) {
        paint_foot(s);
    }
    ngl_overlay_end();
    ngl_flush();

    remember_shown();
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void clamp_scroll(void)
{
    const int16_t total = (int16_t)(s_nap * ROW_H);
    int16_t max = (int16_t)(total - s_list.h);
    if (max < 0) {
        max = 0;
    }
    if (s_scroll > max) { s_scroll = max; }
    if (s_scroll < 0)   { s_scroll = 0; }
}

/**
 * Join the network on row @p i, asking for a password if one is needed.
 *
 * A secure network we have never seen raises the keyboard; a secure one we
 * have is joined with what is stored, because being asked again for a password
 * the tablet already knows is the single most annoying thing a Wi-Fi UI does.
 * An open one is joined straight away.
 */
static void join(int i)
{
    const neos_net_ap_t *a = &s_ap[i];

    if (!a->secure) {
        neos_net_connect(a->ssid, "", true);
        return;
    }
    if (a->known) {
        neos_net_connect(a->ssid, NULL, true);
        return;
    }

    char pass[65] = {0};
    char title[64];
    snprintf(title, sizeof(title), "Password for %s", a->ssid);

    if (!neos_kbd_run(title, pass, sizeof(pass), NEOS_INPUT_SECRET)) {
        return;                     /* cancelled: nothing is stored, nothing tried */
    }
    neos_net_connect(a->ssid, pass, true);

    /* The panel is the only thing that ever held it in the clear, and it is
       going out of scope anyway - but a password left on a stack that the
       keyboard will be run on again is worth one memset. */
    memset(pass, 0, sizeof(pass));
}

/* ------------------------------------------------------------------ */

void neos_panel_wifi(void)
{
    if (!ngl_screen()) {
        return;
    }
    layout();

    /*
     * The reconnect loop must not take this scan. Somebody is standing in
     * front of the tablet choosing a network, and having it join one out from
     * under them mid-list is worse than not being connected.
     */
    neos_net_scan_is_users(true);
    s_shown_valid = false;
    neos_net_scan_start();
    s_nap = neos_net_scan_results(s_ap, NEOS_NET_SCAN_MAX);
    if (s_nap > NEOS_NET_SCAN_MAX) {
        s_nap = NEOS_NET_SCAN_MAX;
    }
    paint_all(true);

    int64_t next_scan = esp_timer_get_time() + (int64_t)RESCAN_MS * 1000;
    uint32_t since = 0;
    ngl_rect_t area = ngl_app_area();

    /* Drag state: where the finger went down, and where the list was then. */
    bool    dragging = false;
    int16_t drag_y0 = 0, drag_scroll0 = 0;
    bool    moved = false;

    while (!neos_ui_should_close()) {
        bool repaint = false;
        bool relaid = false;

        neos_touch_t pts[NEOS_TOUCH_MAX];
        const int np = neos_touch_points_os(pts, NEOS_TOUCH_MAX);

        if (np > 0 && ngl_rect_contains(&s_list, pts[0].x, pts[0].y)) {
            if (!dragging) {
                dragging = true;
                moved = false;
                drag_y0 = pts[0].y;
                drag_scroll0 = s_scroll;
            } else {
                const int16_t dy = (int16_t)(drag_y0 - pts[0].y);
                if (dy > DRAG_SLOP || dy < -DRAG_SLOP) {
                    moved = true;
                }
                if (moved) {
                    s_scroll = (int16_t)(drag_scroll0 + dy);
                    clamp_scroll();
                    repaint = true;
                }
            }
        } else if (np == 0) {
            dragging = false;
        }

        int16_t tx = 0, ty = 0;
        if (neos_touch_tap_os(&tx, &ty) && !moved) {
            const ngl_rect_t close = neos_ui_close_rect(s_panel);
            if (ngl_rect_contains(&close, tx, ty)) {
                break;
            }
            if (ngl_rect_contains(&s_rescan_btn, tx, ty)) {
                neos_net_scan_start();
                next_scan = esp_timer_get_time() + (int64_t)RESCAN_MS * 1000;
                repaint = true;
            } else if (ngl_rect_contains(&s_disc_btn, tx, ty)) {
                neos_net_disconnect();
                repaint = true;
            } else if (ngl_rect_contains(&s_list, tx, ty)) {
                for (int i = 0; i < s_nap; i++) {
                    const ngl_rect_t row = row_rect(i);
                    if (!ngl_rect_contains(&row, tx, ty)) {
                        continue;
                    }
                    const ngl_rect_t fg = forget_rect(&row);
                    if (s_ap[i].known && ngl_rect_contains(&fg, tx, ty)) {
                        neos_net_forget(s_ap[i].ssid);
                        s_ap[i].known = false;
                    } else {
                        join(i);
                        /* The keyboard was over this panel and has just put
                           back what it found, which is this panel - but the
                           app area may have moved underneath both, so the
                           whole thing is laid out and repainted. */
                        layout();
                        s_nap = neos_net_scan_results(s_ap, NEOS_NET_SCAN_MAX);
                        if (s_nap > NEOS_NET_SCAN_MAX) {
                            s_nap = NEOS_NET_SCAN_MAX;
                        }
                        relaid = true;
                    }
                    repaint = true;
                    break;
                }
            }
        }

        const ngl_rect_t a = ngl_app_area();
        if (a.x != area.x || a.y != area.y || a.w != area.w || a.h != area.h) {
            area = a;
            layout();
            clamp_scroll();
            repaint = true;
            relaid = true;
        }

        const int64_t now = esp_timer_get_time();
        if (now >= next_scan && !neos_net_scanning()) {
            next_scan = now + (int64_t)RESCAN_MS * 1000;
            neos_net_scan_start();
        }

        since += POLL_MS;
        bool ticked = false;
        if (since >= TICK_MS) {
            since = 0;
            /* The signal figures move whether or not anything was touched,
               which is the whole reason this list is live. */
            const int n = neos_net_scan_results(s_ap, NEOS_NET_SCAN_MAX);
            s_nap = n > NEOS_NET_SCAN_MAX ? NEOS_NET_SCAN_MAX : n;
            clamp_scroll();
            ticked = true;
        }

        if (relaid) {
            paint_all(true);
        } else if (repaint || ticked) {
            refresh();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }

    neos_net_scan_is_users(false);
    ESP_LOGI(TAG, "wifi panel closed");
}
