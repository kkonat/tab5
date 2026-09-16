/*
 * The networks, and what colour each one is.
 *
 * Two jobs, and the second is the one with the design in it. Reading a scan is
 * a call and a copy. Keeping a network the same colour from one scan to the
 * next is what makes the picture legible at all: the whole point of colour
 * here is to say which bell is which where they overlap, and a colour that
 * changed every two seconds - because a neighbour appeared, or because two
 * networks swapped places in a list sorted by a signal that jitters by a few
 * dB - would carry no information at all. So colour is bound to the name and
 * kept for as long as the app runs, not to the index in this scan's results.
 */
#include "wifiscan.h"

/*
 * Ten hues, evenly spread and all bright enough to read on black.
 *
 * This is the one place the app leaves the system's phosphor-green palette,
 * and it is not decoration: the picture is overlapping translucent shapes and
 * hue is the only channel left to tell them apart, since position is the
 * frequency and height is the signal. Everything that is not a network - the
 * grid, the axis, the furniture - stays in TH_*, so the colour reads as data.
 */
static const ngl_color_t PAL[] = {
    NGL_RGB(  0, 255, 120),   /* spring green */
    NGL_RGB(  0, 200, 255),   /* cyan         */
    NGL_RGB(255, 160,  40),   /* amber        */
    NGL_RGB(200, 120, 255),   /* violet       */
    NGL_RGB(160, 255,  60),   /* lime         */
    NGL_RGB(255, 100, 190),   /* pink         */
    NGL_RGB(120, 170, 255),   /* periwinkle   */
    NGL_RGB(255,  95,  80),   /* coral        */
    NGL_RGB( 60, 255, 205),   /* aquamarine   */
    NGL_RGB(235, 230,  60),   /* yellow       */
};
#define NPAL ((int)(sizeof PAL / sizeof PAL[0]))

/*
 * Who has which colour.
 *
 * Bigger than WS_MAX on purpose: a network that drops out of one scan and
 * comes back in the next - which is every weak network, every scan - keeps its
 * colour, because its row is still here. The table only forgets when it is
 * full and something has to go, and then it forgets whatever has been gone
 * longest.
 */
#define REG_MAX 48

static struct {
    char     ssid[33];
    uint8_t  slot;
    uint32_t seen;
} s_reg[REG_MAX];
static int s_nreg;

static int reg_find(const char *ssid)
{
    for (int i = 0; i < s_nreg; i++) {
        if (strcmp(s_reg[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void reg_add(const char *ssid, uint8_t slot, uint32_t now)
{
    int at = s_nreg;
    if (at >= REG_MAX) {
        at = 0;
        for (int i = 1; i < REG_MAX; i++) {
            if ((int32_t)(s_reg[i].seen - s_reg[at].seen) < 0) {
                at = i;
            }
        }
    } else {
        s_nreg++;
    }
    snprintf(s_reg[at].ssid, sizeof s_reg[at].ssid, "%s", ssid);
    s_reg[at].slot = slot;
    s_reg[at].seen = now;
}

/* ------------------------------------------------------------------ */
/* Reading a scan                                                      */
/* ------------------------------------------------------------------ */

static uint32_t sign_of(const ws_model_t *m)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < m->n; i++) {
        for (const char *p = m->ap[i].ssid; *p; p++) {
            h = (h ^ (uint32_t)(unsigned char)*p) * 16777619u;
        }
        h = (h ^ (uint32_t)(uint8_t)m->ap[i].rssi) * 16777619u;
        h = (h ^ m->ap[i].chan) * 16777619u;
    }
    return h;
}

static void rebuild(ws_model_t *m)
{
    neos_net_ap_t raw[WS_MAX];
    const int found = neos_net_scan_results(raw, WS_MAX);

    int n = found;
    if (n > WS_MAX) {
        n = WS_MAX;
    }
    if (n < 0) {
        n = 0;
    }

    const uint32_t now = ws_now();
    int     used[NPAL];
    uint8_t slot[WS_MAX];

    for (int c = 0; c < NPAL; c++) {
        used[c] = 0;
    }

    /* Pass one: everything with a colour already keeps it. Done first, and
       separately, so that a new network cannot take a colour that one of this
       scan's other networks is about to turn out to own. */
    for (int i = 0; i < n; i++) {
        const int at = reg_find((const char *)raw[i].ssid);
        if (at >= 0) {
            slot[i] = s_reg[at].slot;
            s_reg[at].seen = now;
            used[slot[i]]++;
        } else {
            slot[i] = 0xFFu;
        }
    }

    /* Pass two: a new network takes the least crowded colour. With ten hues
       and at most two dozen networks this is a repeat only once the room is
       genuinely full, and then the repeats are as far apart as they can be. */
    for (int i = 0; i < n; i++) {
        if (slot[i] != 0xFFu) {
            continue;
        }
        int best = 0;
        for (int c = 1; c < NPAL; c++) {
            if (used[c] < used[best]) {
                best = c;
            }
        }
        slot[i] = (uint8_t)best;
        used[best]++;
        reg_add((const char *)raw[i].ssid, (uint8_t)best, now);
    }

    for (int i = 0; i < n; i++) {
        snprintf(m->ap[i].ssid, sizeof m->ap[i].ssid, "%s", raw[i].ssid);
        m->ap[i].rssi   = raw[i].rssi;
        m->ap[i].chan   = raw[i].chan;
        m->ap[i].secure = raw[i].secure;
        m->ap[i].colour = PAL[slot[i]];
    }

    m->n     = n;
    m->found = found;
    m->ever  = true;

    /*
     * The revision is a hash of what is on screen, not a counter of scans.
     * RSSI moves by a dB or two between scans of a room nobody is walking
     * about in, so in practice this changes nearly every time and the plot
     * redraws - which is right, that movement is the live part. What it buys
     * is the case where it genuinely has not changed: a tablet on a desk in an
     * empty flat draws nothing at all rather than a full repaint every two
     * seconds.
     */
    m->rev = sign_of(m);
}

/* ------------------------------------------------------------------ */
/* The pump                                                            */
/* ------------------------------------------------------------------ */

/*
 * A gap between scans.
 *
 * Not throttling for its own sake. A scan takes the radio off its home channel
 * for a couple of seconds, and NeOS owns the connection - so an app that
 * scanned back to back would be an app that kept the tablet's own network
 * limping for as long as it was open. A second between passes is slower than
 * the picture can change in a room and costs the connection almost nothing.
 */
#define WS_GAP_MS   1000

/* And a longer one when the answer was no: the radio is off, or something else
   has it. Retrying that every 30 ms is a busy loop with a syscall in it. */
#define WS_RETRY_MS 1500

void ws_model_init(ws_model_t *m)
{
    memset(m, 0, sizeof *m);
    s_nreg = 0;
}

void ws_model_pump(ws_model_t *m)
{
    const uint32_t now = ws_now();

    /*
     * Watch the flag rather than remember what we asked for. NeOS scans on its
     * own as well - the reconnect loop does, and so does the Wi-Fi panel - and
     * results are results: picking up somebody else's scan is a free update
     * and means this app has something to draw the moment it opens rather than
     * two seconds later.
     */
    if (neos_net_scanning()) {
        m->scanning = true;
        return;
    }

    if (m->scanning) {
        m->scanning = false;
        rebuild(m);
        m->next_ms = now + WS_GAP_MS;
        return;
    }

    if ((int32_t)(now - m->next_ms) < 0) {
        return;
    }

    if (neos_net_scan_start()) {
        m->scanning = true;
    } else {
        m->next_ms = now + WS_RETRY_MS;
    }
}
