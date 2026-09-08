/*
 * The table.
 *
 * One line per device, filling in as the stages land, over a footer that says
 * what is happening. The desktop version drops columns as the terminal
 * narrows so that the identifying ones - address and MAC - are never squeezed
 * into uselessness; the same rule is here, against pixels rather than
 * characters, which is what makes the app readable in portrait as well even
 * though it asks to be held landscape.
 *
 * Repainting is the interesting part. ngl marks the bounds of every draw
 * call dirty, so a frame that draws one row costs one row of panel bandwidth
 * and a frame that draws nothing costs nothing at all - and a scan spends
 * most of its time waiting for packets, during which the table does not
 * change. So each row's rendered content is hashed and compared against what
 * was drawn there last, and a row is repainted only when its own text is
 * genuinely different. An idle table draws nothing, a table where one device
 * just answered draws one line, and neither of those is a full-screen
 * repaint at thirty frames a second.
 */

#include "lanscan.h"
#include "ngl_theme.h"
#include "neos_net.h"

#define ROW_H       38
#define HEAD_H      36
#define FOOT_H      52
#define PAD          8

/*
 * Columns.
 *
 * Widths are pixels against the 16-pixel cell of ngl_font_small, so a number
 * here divided by sixteen is how many characters the column holds: an address
 * is fifteen and a MAC is seventeen, and neither is ever allowed to be
 * narrower than that, because a half-printed MAC is worse than no MAC. GUTTER
 * is what keeps two full-width cells from touching.
 *
 * Everything else earns its place by panel width. The tablet is 1280 across
 * in landscape and 720 the other way up, and this is what makes the app
 * readable in both without a second layout: in portrait the columns simply
 * run out after the two that identify a device.
 */
/* Each width is its content plus the gutter, so the usable cell is exactly
   what the longest value needs: 15 characters for an address, 17 for a MAC. */
#define W_FLAG      26
#define W_IP       252
#define W_MAC      284
#define W_NAME     216
#define W_VENDOR   216
#define GUTTER      12

#define NEED_MAC    600
#define NEED_NAME   850
#define NEED_VENDOR 1080
#define NEED_PORTS  1240

typedef enum {
    COL_IP = 0, COL_MAC, COL_NAME, COL_VENDOR, COL_PORTS, COL_COUNT
} col_t;

typedef struct {
    ngl_rect_t area;      /* what the app may draw in */
    ngl_rect_t list;      /* the rows */
    int        rows;      /* how many fit */
    int        scroll;

    int16_t    col_x[COL_COUNT];
    int16_t    col_w[COL_COUNT];
    bool       col_on[COL_COUNT];

    uint32_t   seen_rev;
    int        seen_scroll;
    int        seen_count;
    bool       repaint_all;

    uint32_t   row_sig[64];   /* by screen row, not by device */
    uint32_t   foot_sig;   /* the status line */
    uint32_t   btn_sig;    /* the buttons and the band they sit on */

    /* the detail card */
    bool       card_open;
    uint32_t   card_ip;
    int        card_scroll;
    bool       card_dirty;      /* redraw the card, without a full repaint */
    int        card_rssi;       /* our own link in dBm, re-read on a timer */
    uint32_t   rssi_next_ms;

    /* touch */
    bool       dragging;
    int16_t    drag_last_y;
    int        drag_px;
} ui_t;

static ui_t s_ui;

/* ------------------------------------------------------------------ */
/* Cells                                                               */
/* ------------------------------------------------------------------ */

static uint32_t hash(uint32_t h, const char *s)
{
    if (!s) {
        return h;
    }
    while (*s) {
        h = (h ^ (uint32_t)(unsigned char)*s++) * 16777619u;
    }
    return h;
}

/* One pass of the ports cell: as many as fit, then how many did not. */
static int ports_pass(const lan_device_t *d, char *out, size_t size, int room,
                      bool with_service)
{
    int at    = 0;
    int shown = 0;
    out[0] = 0;

    for (int i = 0; i < d->nports; i++) {
        char one[24];
        const int n = (with_service && d->ports[i].service[0])
            ? snprintf(one, sizeof(one), "%s%u/%s", at ? " " : "",
                       (unsigned)d->ports[i].port, d->ports[i].service)
            : snprintf(one, sizeof(one), "%s%u", at ? " " : "",
                       (unsigned)d->ports[i].port);
        if (n < 0 || at + n > room - 4 || (size_t)(at + n) >= size) {
            break;
        }
        memcpy(out + at, one, (size_t)n);
        at += n;
        out[at] = 0;
        shown++;
    }
    if (shown < d->nports && (size_t)at + 6 < size) {
        snprintf(out + at, size - (size_t)at, " +%d", d->nports - shown);
    }
    return shown;
}

/*
 * "22/ssh 80/http +3", or "22 80 443 3389 +7" when the names would not fit.
 *
 * The service name is the nicety and the number is the data, so a host with
 * eleven ports open gets to show them rather than showing one with a label on
 * it and "+10". Two passes rather than a width calculation because "does this
 * fit" is exactly what the first pass answers.
 */
static void ports_cell(const lan_device_t *d, char *out, size_t size, int room)
{
    out[0] = 0;
    if (d->nports == 0) {
        return;
    }
    const int shown = ports_pass(d, out, size, room, true);
    if (shown < d->nports && shown < 3) {
        ports_pass(d, out, size, room, false);
    }
}

/* What a device's row says, in the order it is drawn. */
typedef struct {
    char ip[16];
    char mac[20];
    char name[LAN_NAME_LEN];
    char vendor[LAN_VENDOR_LEN];
    char ports[48];
    bool gateway;
    bool self;
} row_t;

static void build_row(const ui_t *ui, const lan_device_t *d, row_t *r)
{
    lan_ip_str(d->ip, r->ip, sizeof(r->ip));

    if (d->has_mac) {
        lan_mac_str(d->mac, r->mac, sizeof(r->mac));
    } else {
        lan_copy(r->mac, sizeof(r->mac), "");
    }

    /* The name column shows the kind when nothing named the device, because
       "printer" is a better answer than an empty cell and the row already
       says, through the MAC and the vendor, that nobody announced a name. */
    lan_copy(r->name, sizeof(r->name), d->name[0] ? d->name : d->kind);
    lan_copy(r->vendor, sizeof(r->vendor), d->vendor);
    ports_cell(d, r->ports, sizeof(r->ports), ui->col_w[COL_PORTS] / 16);

    r->gateway = (d->seen & LAN_SEEN_GW) != 0;
    r->self    = (d->seen & LAN_SEEN_SELF) != 0;
}

static uint32_t row_hash(const ui_t *ui, const row_t *r)
{
    uint32_t h = 2166136261u;
    h = hash(h, r->ip);
    if (ui->col_on[COL_MAC])    { h = hash(h, r->mac); }
    if (ui->col_on[COL_NAME])   { h = hash(h, r->name); }
    if (ui->col_on[COL_VENDOR]) { h = hash(h, r->vendor); }
    if (ui->col_on[COL_PORTS])  { h = hash(h, r->ports); }
    h = (h ^ (r->gateway ? 1u : 0u) ^ (r->self ? 2u : 0u)) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

static void layout(ui_t *ui)
{
    ui->area = ngl_app_area();

    const int16_t w     = ui->area.w;
    const int16_t width[COL_COUNT] = { W_IP, W_MAC, W_NAME, W_VENDOR, 0 };
    const int16_t need[COL_COUNT]  = { 0, NEED_MAC, NEED_NAME, NEED_VENDOR,
                                       NEED_PORTS };

    int16_t x = (int16_t)(ui->area.x + PAD + W_FLAG);
    for (int c = 0; c < COL_COUNT; c++) {
        ui->col_on[c] = (w >= need[c]);
        ui->col_x[c]  = x;
        if (!ui->col_on[c]) {
            ui->col_w[c] = 0;
            continue;
        }
        /* Ports take whatever is left, which is why the column is last: it is
           the only one whose content has no natural width, so it is the one
           that should absorb a panel being wider or narrower than expected. */
        ui->col_w[c] = (c == COL_PORTS)
            ? (int16_t)(ui->area.x + ui->area.w - PAD - x)
            : (int16_t)(width[c] - GUTTER);
        if (ui->col_w[c] < 0) {
            ui->col_w[c] = 0;
        }
        x = (int16_t)(x + (c == COL_PORTS ? ui->col_w[c] : width[c]));
    }

    ui->list.x = ui->area.x;
    ui->list.y = (int16_t)(ui->area.y + HEAD_H);
    ui->list.w = ui->area.w;
    ui->list.h = (int16_t)(ui->area.h - HEAD_H - FOOT_H);
    if (ui->list.h < ROW_H) {
        ui->list.h = ROW_H;
    }

    ui->rows = ui->list.h / ROW_H;
    if (ui->rows > (int)(sizeof(ui->row_sig) / sizeof(ui->row_sig[0]))) {
        ui->rows = (int)(sizeof(ui->row_sig) / sizeof(ui->row_sig[0]));
    }
}

/*
 * One cell, clipped to its own column.
 *
 * The clip is the whole point rather than a nicety: a vendor is whatever the
 * IEEE registry says and a name is whatever a device calls itself, so neither
 * has a length this layout gets to choose. Without this, one long value does
 * not overflow its cell, it overwrites the next one, and the row stops being
 * a table.
 */
static void draw_cell(ui_t *ui, ngl_surface_t *sc, col_t col, int16_t y,
                      const char *text, ngl_color_t colour)
{
    if (!ui->col_on[col] || !text || !text[0]) {
        return;
    }
    const ngl_rect_t was = ngl_surface_clip(sc);
    ngl_rect_t cell = { ui->col_x[col], y, ui->col_w[col], ROW_H };
    ngl_clip_set(sc, &cell);
    ngl_text(sc, ui->col_x[col],
             (int16_t)(y + (ROW_H - ngl_font_small.height) / 2),
             text, &ngl_font_small, colour);
    ngl_clip_set(sc, &was);
}

static void draw_header(ui_t *ui, ngl_surface_t *sc)
{
    static const char *const titles[COL_COUNT] = {
        "ADDRESS", "MAC", "NAME", "VENDOR", "OPEN PORTS",
    };

    ngl_rect_t bar = { ui->area.x, ui->area.y, ui->area.w, HEAD_H };
    ngl_fill_rect(sc, bar, TH_PANEL);

    for (int c = 0; c < COL_COUNT; c++) {
        if (!ui->col_on[c]) {
            continue;
        }
        const ngl_rect_t was = ngl_surface_clip(sc);
        ngl_rect_t cell = { ui->col_x[c], ui->area.y, ui->col_w[c], HEAD_H };
        ngl_clip_set(sc, &cell);
        ngl_text(sc, ui->col_x[c], (int16_t)(ui->area.y + 3), titles[c],
                 &ngl_font_small, TH_TEXT_DIM);
        ngl_clip_set(sc, &was);
    }
    ngl_hline(sc, ui->area.x, (int16_t)(ui->area.y + HEAD_H - 1), ui->area.w, TH_RULE);
}

/*
 * The gateway and this tablet are marked with a shape rather than a glyph.
 * The font is 1-bit ASCII, so a star would have to be an asterisk sitting
 * high in its cell, and two small filled marks read better at a glance than
 * two punctuation characters do.
 */
static void draw_flag(ngl_surface_t *sc, int16_t x, int16_t y, const row_t *r)
{
    const int16_t mid = (int16_t)(y + ROW_H / 2);

    if (r->gateway) {
        for (int i = -4; i <= 4; i++) {
            const int16_t half = (int16_t)(4 - (i < 0 ? -i : i));
            ngl_hline(sc, (int16_t)(x + 5 - half), (int16_t)(mid + i),
                      (int16_t)(1 + half * 2), TH_ACCENT);
        }
    } else if (r->self) {
        ngl_fill_rect(sc, ngl_rect((int16_t)(x + 1), (int16_t)(mid - 4), 9, 9),
                      TH_GLOW);
    }
}

static void draw_row(ui_t *ui, ngl_surface_t *sc, int screen_row, const row_t *r)
{
    const int16_t y = (int16_t)(ui->list.y + screen_row * ROW_H);

    ngl_rect_t band = { ui->area.x, y, ui->area.w, ROW_H };
    ngl_fill_rect(sc, band, TH_BG);

    if (!r) {
        return;
    }
    draw_flag(sc, (int16_t)(ui->area.x + PAD), y, r);

    draw_cell(ui, sc, COL_IP,     y, r->ip,     r->gateway ? TH_GLOW : TH_TEXT);
    draw_cell(ui, sc, COL_MAC,    y, r->mac,    TH_TEXT_DIM);
    draw_cell(ui, sc, COL_NAME,   y, r->name,   TH_TEXT);
    draw_cell(ui, sc, COL_VENDOR, y, r->vendor, TH_TEXT_DIM);
    draw_cell(ui, sc, COL_PORTS,  y, r->ports,  TH_ACCENT);

    ngl_hline(sc, ui->area.x, (int16_t)(y + ROW_H - 1), ui->area.w, TH_RULE);
}

/* ------------------------------------------------------------------ */
/* Footer                                                              */
/* ------------------------------------------------------------------ */

/*
 * The two buttons, right to left. Their widths are given rather than shared,
 * because "DEPTH: normal" is thirteen characters and "RESCAN" is six, and a
 * pair of equal buttons would either clip the one or waste the other.
 */
static ngl_rect_t button_rect(const ui_t *ui, int index)
{
    static const int16_t widths[] = { 140, 240 };

    const int16_t h = FOOT_H - 12;
    const int16_t y = (int16_t)(ui->area.y + ui->area.h - FOOT_H + 6);

    int16_t right = (int16_t)(ui->area.x + ui->area.w - PAD);
    for (int i = 0; i < index; i++) {
        right = (int16_t)(right - widths[i] - 8);
    }
    return ngl_rect((int16_t)(right - widths[index]), y, widths[index], h);
}

static void draw_button(ngl_surface_t *sc, ngl_rect_t r, const char *label)
{
    ngl_fill_round_rect(sc, r, 6, TH_KEY_FILL);
    ngl_draw_round_rect(sc, r, 6, TH_KEY_EDGE, 1);
    const int16_t tw = ngl_text_width(&ngl_font_small, label);
    ngl_text(sc, (int16_t)(r.x + (r.w - tw) / 2),
             (int16_t)(r.y + (r.h - ngl_font_small.height) / 2),
             label, &ngl_font_small, TH_KEY_TEXT);
}

/*
 * The buttons, and the band they sit on.
 *
 * Split from the status line because the two change at completely different
 * rates: the note moves several times a second while a stage runs, and the
 * buttons move when somebody presses one. Repainting the whole band for the
 * note meant clearing the buttons and drawing them again thirty times a
 * second, which is visible as a flicker along the bottom of the screen and is
 * the one thing this app's repaint discipline exists to avoid.
 */
static void draw_footer_frame(ui_t *ui, ngl_surface_t *sc, lan_scan_t *s)
{
    const int16_t y = (int16_t)(ui->area.y + ui->area.h - FOOT_H);

    ngl_rect_t band = { ui->area.x, y, ui->area.w, FOOT_H };
    ngl_fill_rect(sc, band, TH_PANEL);
    ngl_hline(sc, ui->area.x, y, ui->area.w, TH_RULE);

    char depth[24];
    snprintf(depth, sizeof(depth), "DEPTH: %s", lan_depth_name(s->depth));
    draw_button(sc, button_rect(ui, 0), "RESCAN");
    draw_button(sc, button_rect(ui, 1), depth);
}

static void draw_footer(ui_t *ui, ngl_surface_t *sc, lan_scan_t *s)
{
    const int16_t y = (int16_t)(ui->area.y + ui->area.h - FOOT_H);

    /*
     * The count goes first, because it is the answer and everything after it
     * is progress. Putting it at the end meant the one number worth reading
     * was the one a long stage note pushed off the end of the strip.
     *
     * No stage prefix when nothing is running: "watching: no address yet" is
     * two states at once and leaves the reader to pick which to believe.
     */
    char line[140];
    if (!s->running) {
        snprintf(line, sizeof(line), "%s", s->note);
    } else if (s->reg.overflow) {
        snprintf(line, sizeof(line), "%d devices (+%d over) | %s: %s",
                 s->reg.count, s->reg.overflow, lan_stage_name(s->stage), s->note);
    } else {
        snprintf(line, sizeof(line), "%d devices | %s: %s",
                 s->reg.count, lan_stage_name(s->stage), s->note);
    }

    /*
     * Only the strip the text lives in is cleared, and only up to where the
     * buttons begin - so a long note runs out of room rather than running
     * underneath them, and a short one does not wipe them on its way past.
     */
    ngl_rect_t text_area = { (int16_t)(ui->area.x + 1), (int16_t)(y + 1),
                             (int16_t)(button_rect(ui, 1).x - ui->area.x - PAD),
                             (int16_t)(FOOT_H - 1) };
    ngl_fill_rect(sc, text_area, TH_PANEL);

    const ngl_rect_t was = ngl_surface_clip(sc);
    ngl_clip_set(sc, &text_area);
    ngl_text(sc, (int16_t)(ui->area.x + PAD),
             (int16_t)(y + (FOOT_H - ngl_font_small.height) / 2),
             line, &ngl_font_small,
             s->first_pass_done ? TH_TEXT_DIM : TH_TEXT);
    ngl_clip_set(sc, &was);
}

static uint32_t footer_hash(const lan_scan_t *s)
{
    uint32_t h = 2166136261u;
    h = hash(h, s->note);
    h = hash(h, lan_stage_name(s->stage));
    h = (h ^ (uint32_t)s->reg.overflow) * 16777619u;
    h = (h ^ (uint32_t)s->reg.count) * 16777619u;
    h = (h ^ (s->running ? 1u : 0u)) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ */
/* The detail card                                                     */
/* ------------------------------------------------------------------ */

/*
 * Everything about one device, which is everything the table had no column
 * for. It is a plain opaque panel drawn over the list rather than an overlay:
 * ngl's overlay machinery belongs to the OS - an app that could take the
 * screen from NeOS could take it from the close button - so getting back is
 * an ordinary full repaint, which the table has to be able to do anyway.
 */
static void draw_card(ui_t *ui, ngl_surface_t *sc, const lan_device_t *d)
{
    ngl_rect_t card = {
        (int16_t)(ui->area.x + 40), (int16_t)(ui->area.y + 20),
        (int16_t)(ui->area.w - 80), (int16_t)(ui->area.h - 40),
    };
    ngl_fill_round_rect(sc, card, 10, TH_MODAL_BG);
    ngl_draw_round_rect(sc, card, 10, TH_MODAL_EDGE, 2);

    const ngl_rect_t close = ngl_rect((int16_t)(card.x + card.w - 56),
                                      (int16_t)(card.y + 12), 44, 40);
    draw_button(sc, close, "X");

    char line[160];
    int16_t y  = (int16_t)(card.y + 18);
    const int16_t x = (int16_t)(card.x + 20);
    const int16_t step = (int16_t)(ngl_font_small.height + 4);
    const int16_t last = (int16_t)(card.y + card.h - 20);

    lan_ip_str(d->ip, line, sizeof(line));
    ngl_text(sc, x, y, line, &ngl_font_large, TH_GLOW);
    y = (int16_t)(y + ngl_font_large.height + 8);

    int skip = ui->card_scroll;

    /* One closure would be nicer than this macro, and C does not have one.
       Every line goes through it so that scrolling is one counter and not a
       decision at each call site. */
    #define CARD_LINE(colour)                                            \
        do {                                                             \
            if (skip > 0) { skip--; }                                    \
            else if (y < last) {                                         \
                ngl_text(sc, x, y, line, &ngl_font_small, (colour));     \
                y = (int16_t)(y + step);                                 \
            }                                                            \
        } while (0)

    if (d->has_mac) {
        char mac[20];
        lan_mac_str(d->mac, mac, sizeof(mac));
        snprintf(line, sizeof(line), "MAC      %s", mac);
    } else {
        snprintf(line, sizeof(line), "MAC      not resolved");
    }
    CARD_LINE(TH_TEXT);

    if (d->vendor[0]) {
        snprintf(line, sizeof(line), "vendor   %s", d->vendor);
        CARD_LINE(TH_TEXT);
    }
    if (d->name[0]) {
        static const char *const src[] = { "mdns", "netbios", "upnp", "snmp",
                                           "dns", "web page", "" };
        snprintf(line, sizeof(line), "name     %s  (%s)", d->name,
                 src[d->name_src <= LAN_NAME_NONE ? d->name_src : LAN_NAME_NONE]);
        CARD_LINE(TH_TEXT);
    }
    if (d->kind[0]) {
        snprintf(line, sizeof(line), "type     %s", d->kind);
        CARD_LINE(TH_TEXT);
    }
    if (d->os[0]) {
        snprintf(line, sizeof(line), "os       %s (ttl %u)", d->os, (unsigned)d->ttl);
        CARD_LINE(TH_TEXT);
    }
    if (d->rtt_ms) {
        snprintf(line, sizeof(line), "rtt      %u ms", (unsigned)d->rtt_ms);
        CARD_LINE(TH_TEXT);
    }

    /*
     * Signal strength, which on this machine is a fact about exactly one row.
     *
     * The reading a radio can give for another station is the strength of the
     * frames it received from it, and hearing frames not addressed to us is
     * promiscuous mode. The P4 has no radio: esp_wifi here is esp_wifi_remote
     * over ESP-Hosted to the C6, and the whole promiscuous family sits in the
     * block of calls esp_hosted does not implement, so the weak stub answers
     * ESP_ERR_NOT_SUPPORTED. The slave side is the factory C6 image, which
     * cannot be rebuilt from here - so this is a property of the board and
     * not a thing left undone, and no amount of work in this app changes it.
     *
     * What is measurable is our own association, and that is a real number
     * for the tablet's own row. It is labelled as ours rather than left to
     * read as the device's, and the other rows say plainly that there is
     * nothing to show rather than leaving a reader to wonder whether the
     * scan simply had not got there yet.
     */
    if (d->seen & LAN_SEEN_SELF) {
        if (ui->card_rssi) {
            snprintf(line, sizeof(line), "signal   %d dBm  (own link to %s)",
                     ui->card_rssi, neos_net_ssid());
            CARD_LINE(TH_TEXT);
        } else {
            lan_copy(line, sizeof(line), "signal   not associated");
            CARD_LINE(TH_TEXT_DIM);
        }
    } else {
        lan_copy(line, sizeof(line),
                 "signal   n/a - the radio reports only its own link");
        CARD_LINE(TH_TEXT_DIM);
    }
    if (d->workgroup[0]) {
        snprintf(line, sizeof(line), "workgrp  %s", d->workgroup);
        CARD_LINE(TH_TEXT);
    }
    if (d->detail[0]) {
        snprintf(line, sizeof(line), "says     %s", d->detail);
        CARD_LINE(TH_TEXT);
    }

    {
        /* How we know it is there. Worth showing: a device found only by ARP
           is a device that answered nothing else, which is itself a fact
           about it. */
        char how[80];
        how[0] = 0;
        static const struct { uint16_t bit; const char *name; } tags[] = {
            { LAN_SEEN_ARP, "arp" },   { LAN_SEEN_ICMP, "icmp" },
            { LAN_SEEN_TCP, "tcp" },   { LAN_SEEN_MDNS, "mdns" },
            { LAN_SEEN_SSDP, "ssdp" }, { LAN_SEEN_NBT, "netbios" },
            { LAN_SEEN_SNMP, "snmp" }, { LAN_SEEN_DNS, "dns" },
        };
        for (int i = 0; i < (int)(sizeof(tags) / sizeof(tags[0])); i++) {
            if (d->seen & tags[i].bit) {
                const size_t at = strlen(how);
                snprintf(how + at, sizeof(how) - at, "%s%s", at ? " " : "",
                         tags[i].name);
            }
        }
        snprintf(line, sizeof(line), "seen by  %s", how);
        CARD_LINE(TH_TEXT_DIM);
    }

    if (d->nservices) {
        lan_copy(line, sizeof(line), "services");
        CARD_LINE(TH_TEXT_DIM);
        for (int i = 0; i < d->nservices; i++) {
            snprintf(line, sizeof(line), "  %s", d->services[i]);
            CARD_LINE(TH_TEXT);
        }
    }

    if (d->nports) {
        snprintf(line, sizeof(line), "%d open ports", d->nports);
        CARD_LINE(TH_TEXT_DIM);
        for (int i = 0; i < d->nports; i++) {
            const lan_port_t *p = &d->ports[i];
            snprintf(line, sizeof(line), "  %-5u %s", (unsigned)p->port, p->service);
            CARD_LINE(TH_ACCENT);
            if (p->title[0]) {
                snprintf(line, sizeof(line), "        %s", p->title);
                CARD_LINE(TH_TEXT);
            }
            if (p->server[0]) {
                snprintf(line, sizeof(line), "        %s", p->server);
                CARD_LINE(TH_TEXT_DIM);
            }
            if (p->banner[0]) {
                snprintf(line, sizeof(line), "        %s", p->banner);
                CARD_LINE(TH_TEXT_DIM);
            }
        }
    }
    #undef CARD_LINE
}

/*
 * Keep the card's signal reading current.
 *
 * Two reasons this is on a clock rather than read where it is drawn. It is
 * the number on the card that changes while nothing else about the device
 * does - carrying the tablet across a room moves it and moves nothing in the
 * registry - so the repaint discipline, which redraws on the registry's
 * revision, would never notice it. And the reading is an RPC over SDIO to the
 * C6 rather than a variable, so a draw-time call would put one on every frame
 * the card is open.
 *
 * A second is the rate the Wi-Fi panel's bars move at and is far slower than
 * anyone can carry a tablet somewhere different.
 */
#define LINK_MS 1000

static void refresh_link(ui_t *ui, lan_scan_t *s)
{
    if (!ui->card_open) {
        return;
    }
    const lan_device_t *d = lan_find(&s->reg, ui->card_ip);
    if (!d || !(d->seen & LAN_SEEN_SELF)) {
        return;
    }

    const uint32_t now = lan_now();
    if ((int32_t)(now - ui->rssi_next_ms) < 0) {
        return;
    }
    ui->rssi_next_ms = now + LINK_MS;

    const int rssi = neos_net_rssi();
    if (rssi != ui->card_rssi) {
        ui->card_rssi  = rssi;
        ui->card_dirty = true;
    }
}

/* ------------------------------------------------------------------ */
/* Touch                                                               */
/* ------------------------------------------------------------------ */

static void clamp_scroll(ui_t *ui, int count)
{
    const int most = count - ui->rows;
    if (ui->scroll > most) {
        ui->scroll = most;
    }
    if (ui->scroll < 0) {
        ui->scroll = 0;
    }
}

static void handle_touch(ui_t *ui, lan_scan_t *s)
{
    /*
     * Dragging and tapping are read from two places on purpose. NeOS does the
     * edge detection for a tap - a press and release that did not travel far -
     * so a drag never arrives as a tap and neither has to be told about the
     * other.
     */
    neos_touch_t t;
    if (neos_touch(&t) && t.down) {
        if (!ui->dragging) {
            ui->dragging    = true;
            ui->drag_last_y = t.y;
            ui->drag_px     = 0;
        } else {
            ui->drag_px += (ui->drag_last_y - t.y);
            ui->drag_last_y = t.y;
            while (ui->drag_px >= ROW_H) {
                ui->drag_px -= ROW_H;
                if (ui->card_open) { ui->card_scroll++; } else { ui->scroll++; }
            }
            while (ui->drag_px <= -ROW_H) {
                ui->drag_px += ROW_H;
                if (ui->card_open) {
                    if (ui->card_scroll) { ui->card_scroll--; }
                } else {
                    ui->scroll--;
                }
            }
            if (!ui->card_open) {
                clamp_scroll(ui, s->reg.count);
            }
        }
    } else {
        ui->dragging = false;
    }

    int16_t tx, ty;
    if (!neos_touch_tap(&tx, &ty)) {
        return;
    }

    /* Any tap dismisses the card, not only the one on its X. The X is there
       to say the card can be dismissed; making it the only way out would be
       making the affordance into a requirement. */
    if (ui->card_open) {
        ui->card_open   = false;
        ui->card_scroll = 0;
        ui->repaint_all = true;
        return;
    }

    ngl_rect_t rescan = button_rect(ui, 0);
    if (ngl_rect_contains(&rescan, tx, ty)) {
        lan_engine_rescan(s);
        ui->scroll      = 0;
        ui->repaint_all = true;
        return;
    }

    ngl_rect_t depth = button_rect(ui, 1);
    if (ngl_rect_contains(&depth, tx, ty)) {
        /* Takes effect at the next deep pass, which on a scan that is already
           watching is the next cycle. Nothing is torn down to apply it. */
        s->depth = (lan_depth_t)((s->depth + 1) % LAN_DEPTH_COUNT);
        s->reg.revision++;
        return;
    }

    if (ngl_rect_contains(&ui->list, tx, ty)) {
        const int row = (ty - ui->list.y) / ROW_H;
        const int idx = ui->scroll + row;
        if (row >= 0 && row < ui->rows && idx >= 0 && idx < s->reg.count) {
            ui->card_open    = true;
            ui->card_ip      = s->reg.dev[idx].ip;
            ui->card_scroll  = 0;
            ui->card_rssi    = 0;
            ui->rssi_next_ms = lan_now();
            ui->repaint_all  = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* The frame                                                           */
/* ------------------------------------------------------------------ */

void lan_ui_repaint(void)
{
    s_ui.repaint_all = true;
}

void lan_ui_init(lan_scan_t *s)
{
    memset(&s_ui, 0, sizeof(s_ui));
    layout(&s_ui);
    s_ui.repaint_all = true;
    s_ui.seen_rev    = ~s->reg.revision;
}

void lan_ui_tick(lan_scan_t *s)
{
    ui_t          *ui = &s_ui;
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }

    layout(ui);
    handle_touch(ui, s);
    clamp_scroll(ui, s->reg.count);
    refresh_link(ui, s);

    /* Nothing changed, nothing moved, nothing to draw. This is the ordinary
       case while a stage waits for packets, and it costs one comparison. */
    const bool same = (ui->seen_rev == s->reg.revision) &&
                      (ui->seen_scroll == ui->scroll) &&
                      (ui->seen_count == s->reg.count) &&
                      !ui->card_dirty &&
                      !ui->repaint_all;
    if (same) {
        return;
    }

    if (ui->repaint_all) {
        ngl_clear(sc, TH_BG);
        draw_header(ui, sc);
        for (int i = 0; i < ui->rows; i++) {
            ui->row_sig[i] = 0;
        }
        ui->foot_sig = 0;
        ui->btn_sig  = 0;
    }

    if (ui->card_open) {
        const lan_device_t *d = lan_find(&s->reg, ui->card_ip);
        if (d) {
            draw_card(ui, sc, d);
        } else {
            ui->card_open   = false;
            ui->repaint_all = true;
        }
    } else {
        for (int row = 0; row < ui->rows; row++) {
            const int idx = ui->scroll + row;

            uint32_t sig = 0;
            row_t    r;
            if (idx >= 0 && idx < s->reg.count) {
                build_row(ui, &s->reg.dev[idx], &r);
                sig = row_hash(ui, &r);
                if (sig == 0) {
                    sig = 1;      /* 0 is the "nothing drawn here" marker */
                }
            }
            if (sig == ui->row_sig[row]) {
                continue;
            }
            ui->row_sig[row] = sig;
            draw_row(ui, sc, row, sig ? &r : NULL);
        }
    }

    /* The band and its buttons first: the status line draws over the strip
       this leaves, so it has to be the one that lands last. */
    const uint32_t bh = hash(2166136261u, lan_depth_name(s->depth));
    if (bh != ui->btn_sig) {
        ui->btn_sig  = bh;
        ui->foot_sig = 0;
        draw_footer_frame(ui, sc, s);
    }

    const uint32_t fh = footer_hash(s);
    if (fh != ui->foot_sig) {
        ui->foot_sig = fh;
        draw_footer(ui, sc, s);
    }

    ui->seen_rev    = s->reg.revision;
    ui->seen_scroll = ui->scroll;
    ui->seen_count  = s->reg.count;
    ui->repaint_all = false;
    ui->card_dirty  = false;

    ngl_flush();
}
