/*
 * The panel: the control table, the layout, and what a frame costs.
 *
 * ---------------------------------------------------------------- repainting
 *
 * Thirty-six controls, a keyboard and a fader, on a canvas of 921,600 pixels.
 * A wholesale repaint of that is tens of milliseconds and the codec's cushion
 * is thirty, so an app that repaints everything per frame is an app whose
 * sound depends on what the drawing loop happens to be doing - which is the
 * one thing mg_audio.c exists to prevent. Even with the voice safely on its
 * own thread it would be a 15 fps panel, which is not what a knob under a
 * finger should feel like.
 *
 * So the unit of repainting is the control. Each owns a cell; a cell is
 * repainted when its own value changed, and nothing else is touched. Dragging
 * a knob costs one cell of about 20,000 pixels. Pressing a key costs one key,
 * and the two or three black keys that overlap it.
 *
 * ------------------------------------------------------------------ the table
 *
 * Thirty-six controls is where a panel stops being a list of paint calls and
 * starts being data. mg_defs[] says what each one *is* - its page, its kind,
 * how many positions, what it is called - and there is then one painter, one
 * hit test and one drag for all of them. Adding the mod wheel this is missing
 * would be a row here, a line in mg_ui_patch() and a line in mg_ui_value().
 *
 * What the table deliberately does not hold is geometry. Four sections with
 * four different grids do not come out of a common row-and-column spec without
 * a spec more complicated than the layout it describes, so mg_ui_layout()
 * places them with explicit loops and writes the rectangles back.
 */
#include <math.h>
#include <stdio.h>

#include "mg_ui.h"

#include "ngl_theme.h"
#include "neos_sys.h"

/* ------------------------------------------------------------------ */
/* What each control is                                                */
/* ------------------------------------------------------------------ */

const mg_def_t mg_defs[MC_COUNT] = {
    /* --- page 1: CONTROLLERS --- */
    [MC_TUNE]      = { 0, CK_KNOB,   0, "TUNE",       NULL,    NULL   },
    [MC_OSC_MOD]   = { 0, CK_SWITCH, 0, "OSC MOD",    "ON",    "OFF"  },
    [MC_GLIDE]     = { 0, CK_KNOB,   0, "GLIDE",      NULL,    NULL   },
    [MC_MOD_MIX]   = { 0, CK_KNOB,   0, "MOD MIX",    NULL,    NULL   },

    /* --- page 1: OSCILLATOR BANK --- */
    [MC_O1_RANGE]  = { 0, CK_SELECT, RANGE_N, "OSC1 RANGE", NULL, NULL },
    [MC_OSC3_CTL]  = { 0, CK_SWITCH, 0, "OSC 3 CONT", "KBD",   "FREE" },
    [MC_O1_WAVE]   = { 0, CK_SELECT, WAVE_N,  "OSC1 WAVE",  NULL, NULL },
    [MC_O2_RANGE]  = { 0, CK_SELECT, RANGE_N, "OSC2 RANGE", NULL, NULL },
    [MC_O2_FREQ]   = { 0, CK_KNOB,   0, "OSC2 FREQ",  NULL,    NULL   },
    [MC_O2_WAVE]   = { 0, CK_SELECT, WAVE_N,  "OSC2 WAVE",  NULL, NULL },
    [MC_O3_RANGE]  = { 0, CK_SELECT, RANGE_N, "OSC3 RANGE", NULL, NULL },
    [MC_O3_FREQ]   = { 0, CK_KNOB,   0, "OSC3 FREQ",  NULL,    NULL   },
    [MC_O3_WAVE]   = { 0, CK_SELECT, WAVE_N,  "OSC3 WAVE",  NULL, NULL },

    /* --- page 2: MIXER --- */
    [MC_V1]        = { 1, CK_KNOB,   0, "OSC 1",      NULL,    NULL   },
    [MC_ON1]       = { 1, CK_SWITCH, 0, NULL,         "ON",    "OFF"  },
    [MC_V2]        = { 1, CK_KNOB,   0, "OSC 2",      NULL,    NULL   },
    [MC_ON2]       = { 1, CK_SWITCH, 0, NULL,         "ON",    "OFF"  },
    [MC_V3]        = { 1, CK_KNOB,   0, "OSC 3",      NULL,    NULL   },
    [MC_ON3]       = { 1, CK_SWITCH, 0, NULL,         "ON",    "OFF"  },
    [MC_VN]        = { 1, CK_KNOB,   0, "NOISE",      NULL,    NULL   },
    [MC_ONN]       = { 1, CK_SWITCH, 0, NULL,         "ON",    "OFF"  },
    [MC_NCOLOUR]   = { 1, CK_SWITCH, 0, NULL,         "WHITE", "PINK" },
    [MC_VI]        = { 1, CK_KNOB,   0, "INPUT",      NULL,    NULL   },
    [MC_ONI]       = { 1, CK_SWITCH, 0, NULL,         "ON",    "OFF"  },
    [MC_IMODE]     = { 1, CK_SWITCH, 0, NULL,         "FDBK",  "OFF"  },

    /* --- page 2: MODIFIERS --- */
    [MC_FILT_MOD]  = { 1, CK_SWITCH, 0, "FILT MOD",   "ON",    "OFF"  },
    [MC_KBD1]      = { 1, CK_SWITCH, 0, "KBD 1",      "ON",    "OFF"  },
    [MC_KBD2]      = { 1, CK_SWITCH, 0, "KBD 2",      "ON",    "OFF"  },
    [MC_CUTOFF]    = { 1, CK_KNOB,   0, "CUTOFF",     NULL,    NULL   },
    [MC_EMPHASIS]  = { 1, CK_KNOB,   0, "EMPHASIS",   NULL,    NULL   },
    [MC_CONTOUR]   = { 1, CK_KNOB,   0, "CONTOUR",    NULL,    NULL   },
    /*
     * Seven characters and not eight. Eight fills 128 pixels of a 132-pixel
     * cell, which centres to a two-pixel margin - so three of them in a row
     * read as one long word with no spaces in it. The instrument says FILTER
     * CONTOUR over its three and LOUDNESS CONTOUR over the other three; there
     * is no room for a group header here, so the group goes in the caption and
     * the caption loses a letter to make room for the gap.
     */
    [MC_FA]        = { 1, CK_KNOB,   0, "FLT ATK",   NULL,    NULL   },
    [MC_FD]        = { 1, CK_KNOB,   0, "FLT DEC",   NULL,    NULL   },
    [MC_FS]        = { 1, CK_KNOB,   0, "FLT SUS",   NULL,    NULL   },
    [MC_LA]        = { 1, CK_KNOB,   0, "AMP ATK",   NULL,    NULL   },
    [MC_LD]        = { 1, CK_KNOB,   0, "AMP DEC",   NULL,    NULL   },
    [MC_LS]        = { 1, CK_KNOB,   0, "AMP SUS",   NULL,    NULL   },

    /* --- on every page --- */
    [MC_VOLUME]    = { MG_BOTH, CK_SLIDER, 0, "VOL",  NULL,    NULL   },
};

/* ------------------------------------------------------------------ */
/* Ranges                                                              */
/* ------------------------------------------------------------------ */

/* The master tune, either way, in semitones. */
#define TUNE_SEMIS 2.0f

/* An oscillator's own frequency knob: the Model D's is marked -7 to +7 and
   covers a little over a fifth each way. */
#define DETUNE_SEMIS 7.0f

/* The longest glide, in seconds to cross an octave. A time per octave rather
   than a rate, because a portamento that takes as long to move a semitone as
   an octave is not what the knob is labelled. */
#define GLIDE_MAX 2.0f

/* A contour's attack or decay. The panel is marked from ten milliseconds to
   ten seconds; the bottom goes a little below that so a click is reachable. */
#define ENV_MIN 0.002f
#define ENV_MAX 10.0f

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

#define HEADER_H    52
#define CLOSE_S     48
#define MARGIN      24

#define PAGE_X      44
#define PAGE_Y      56
#define PAGE_W    1084
#define PAGE_H     484

#define ARROW_W     34
#define ARROW_H    180

#define FADER_W     76
#define KEYS_H     152

/*
 * The most a knob may grow, and the most a switch may.
 *
 * Both caps exist for the same reason and it is not tidiness. The four
 * sections have four different cell shapes, so sizing each control to its own
 * cell gives CONTROLLERS knobs half again the size of the OSCILLATOR BANK's
 * across a divider you can see both sides of at once - which reads as two
 * panels bolted together rather than as one instrument. The cap costs the
 * roomy cells some whitespace and buys a panel that looks like it was made in
 * one go.
 */
#define KNOB_MAX     32
#define SWITCH_MAX  152

static struct {
    int16_t    w, h;
    ngl_rect_t header, close, prev, next, keys, rule[2];
    ngl_rect_t cell[MC_COUNT];
    ngl_rect_t lamp, vol_cap, o1_gap;
    int16_t    sec_x[2][2];      /* where each page's two section names start */
} L;

static const char *const SECTION[2][2] = {
    { "CONTROLLERS", "OSCILLATOR BANK" },
    { "MIXER",       "MODIFIERS"       },
};

/* Lay @p n controls into a grid, row-major. -1 leaves a cell empty. */
static void grid(int16_t x, int16_t y, int16_t w, int16_t h,
                 int cols, int rows, const int *ids)
{
    const int16_t cw = (int16_t)(w / cols);
    const int16_t ch = (int16_t)(h / rows);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const int id = ids[r * cols + c];
            if (id >= 0) {
                L.cell[id] = ngl_rect((int16_t)(x + c * cw), (int16_t)(y + r * ch),
                                      cw, ch);
            }
        }
    }
}

void mg_ui_layout(const mg_app_t *a)
{
    const int16_t w = a->gfx.w;
    const int16_t h = a->gfx.h;

    L.w = w;
    L.h = h;

    L.header = ngl_rect(0, 0, w, HEADER_H);
    L.close  = ngl_rect((int16_t)(w - MARGIN - CLOSE_S), 2, CLOSE_S, CLOSE_S);

    L.prev = ngl_rect(6, (int16_t)(PAGE_Y + (PAGE_H - ARROW_H) / 2), ARROW_W, ARROW_H);
    L.next = ngl_rect((int16_t)(PAGE_X + PAGE_W + 6),
                      (int16_t)(PAGE_Y + (PAGE_H - ARROW_H) / 2), ARROW_W, ARROW_H);

    /* The caption sits above the cell rather than inside it: mg_fader() fills
       its whole rectangle, so a caption within it would be rubbed out on every
       drag and would have to be presented with the fader to come back. */
    L.vol_cap = ngl_rect((int16_t)(w - MARGIN - FADER_W), PAGE_Y, FADER_W, 32);
    L.cell[MC_VOLUME] = ngl_rect((int16_t)(w - MARGIN - FADER_W),
                                 (int16_t)(PAGE_Y + 34), FADER_W,
                                 (int16_t)(h - PAGE_Y - 34 - MARGIN));

    L.keys = ngl_rect(PAGE_X, (int16_t)(PAGE_Y + PAGE_H + 8), PAGE_W, KEYS_H);

    /* --- page 1 ------------------------------------------------- */
    {
        /*
         * Three rows either side of the divider, so the two sections line up
         * across it. CONTROLLERS has four knobs and a switch where the bank has
         * nine controls, so its last row is one cell wide instead of two - and
         * the switch is the one that belongs there, being the only control on
         * this page that is about routing rather than about a value.
         */
        static const int controllers[] = {
            MC_TUNE,  MC_OSC_MOD,
            MC_GLIDE, MC_MOD_MIX,
            MC_OSC3_CTL, -1,
        };
        grid(PAGE_X, PAGE_Y, 300, PAGE_H, 2, 3, controllers);
        /* ... and it takes the whole width of that row. */
        L.cell[MC_OSC3_CTL].w = 300;

        /*
         * The hole in the middle of the top row is the instrument's own:
         * oscillator 1 has no frequency knob, because it is the reference the
         * other two are tuned against. paint_page() writes that in the gap, so
         * that it reads as a fact about the Minimoog rather than as a control
         * that failed to draw.
         */
        static const int bank[] = {
            MC_O1_RANGE, -1,          MC_O1_WAVE,
            MC_O2_RANGE, MC_O2_FREQ,  MC_O2_WAVE,
            MC_O3_RANGE, MC_O3_FREQ,  MC_O3_WAVE,
        };
        grid(360, PAGE_Y, 768, PAGE_H, 3, 3, bank);
        L.o1_gap = ngl_rect(360 + 768 / 3, PAGE_Y, 768 / 3, PAGE_H / 3);

        L.rule[0] = ngl_rect(352, PAGE_Y, 1, PAGE_H);
        L.sec_x[0][0] = PAGE_X;
        L.sec_x[0][1] = 360;
    }

    /* --- page 2 ------------------------------------------------- */
    {
        static const int mixer[] = {
            MC_V1,  MC_V2,  MC_V3,  MC_VN,      MC_VI,
            MC_ON1, MC_ON2, MC_ON3, MC_ONN,     MC_ONI,
            -1,     -1,     -1,     MC_NCOLOUR, MC_IMODE,
        };
        grid(PAGE_X, PAGE_Y, 540, PAGE_H, 5, 3, mixer);

        /* The overload lamp takes the three cells the mixer's bottom row does
           not use, which is also where the instrument puts it: beside the
           input, watching the sum of everything to its left. */
        const int16_t cw = 540 / 5, ch = PAGE_H / 3;
        L.lamp = ngl_rect(PAGE_X, (int16_t)(PAGE_Y + 2 * ch), (int16_t)(3 * cw), ch);

        static const int switches[] = { MC_FILT_MOD, MC_KBD1, MC_KBD2 };
        grid(594, PAGE_Y, 130, PAGE_H, 1, 3, switches);

        static const int knobs[] = {
            MC_CUTOFF, MC_EMPHASIS, MC_CONTOUR,
            MC_FA,     MC_FD,       MC_FS,
            MC_LA,     MC_LD,       MC_LS,
        };
        grid(732, PAGE_Y, 396, PAGE_H, 3, 3, knobs);

        L.rule[1] = ngl_rect(588, PAGE_Y, 1, PAGE_H);
        L.sec_x[1][0] = PAGE_X;
        L.sec_x[1][1] = 594;
    }
}

ngl_rect_t mg_ui_rect(int ctl)
{
    return (ctl >= 0 && ctl < MC_COUNT) ? L.cell[ctl] : ngl_rect(0, 0, 0, 0);
}

ngl_rect_t mg_ui_keys_rect(void) { return L.keys; }

int mg_ui_hit(const mg_app_t *a, int16_t x, int16_t y)
{
    if (ngl_rect_contains(&L.close, x, y)) { return HIT_CLOSE; }
    if (ngl_rect_contains(&L.prev, x, y))  { return HIT_PREV; }
    if (ngl_rect_contains(&L.next, x, y))  { return HIT_NEXT; }
    if (ngl_rect_contains(&L.keys, x, y))  { return HIT_KEYS; }

    for (int i = 0; i < MC_COUNT; i++) {
        if (mg_defs[i].page != a->page && mg_defs[i].page != MG_BOTH) {
            continue;
        }
        if (ngl_rect_contains(&L.cell[i], x, y)) {
            return i;
        }
    }
    /* The instruments are a control too: tapping the header takes the frame
       loop off its 60 Hz pacing, which is the only way to find out what the
       frame can actually do rather than what it was asked to do. */
    if (y < HEADER_H) {
        return HIT_METERS;
    }
    return HIT_NONE;
}

/* ------------------------------------------------------------------ */
/* Positions to values                                                 */
/* ------------------------------------------------------------------ */

/*
 * Three hundred pixels of travel is a control's full range.
 *
 * Not the knob's own diameter, which is sixty: a control you can only set to
 * one of sixty values is a control that cannot be tuned, and reaching for a
 * rotary gesture on a capacitive panel is worse - the finger occludes the
 * thing it is turning. So the knob is a handle and the screen is the track.
 */
#define DRAG_SPAN 300.0f

float mg_ui_drag(int ctl, float v0, int16_t y0, int16_t y)
{
    const mg_def_t *d = &mg_defs[ctl];
    const int steps = (d->kind == CK_SELECT && d->steps > 1) ? d->steps : 0;

    const float span = steps ? (float)(steps - 1) : 1.0f;

    /*
     * A fader travels with the finger and a knob does not. A knob is a handle
     * on a 300-pixel track because it has no travel of its own to speak of; a
     * fader's cap is a real position on a real slot, and one that slid at three
     * times the speed of the thumb pushing it would feel broken.
     */
    const float px = (mg_defs[ctl].kind == CK_SLIDER)
                   ? (float)(L.cell[ctl].h - 64) : DRAG_SPAN;

    float n = v0 / span + (float)(y0 - y) / (px < 1.0f ? 1.0f : px);
    if (n < 0.0f) { n = 0.0f; }
    if (n > 1.0f) { n = 1.0f; }

    return steps ? (float)(int)(n * span + 0.5f) : n;
}

static float logmap(float n, float lo, float hi)
{
    return lo * powf(hi / lo, n);
}

static float logmap_inv(float v, float lo, float hi)
{
    return logf(v / lo) / logf(hi / lo);
}

void mg_ui_defaults(mg_app_t *a)
{
    for (int i = 0; i < MC_COUNT; i++) {
        a->v[i] = 0.0f;
    }

    /*
     * Something that makes a sound the moment a key is touched, rather than a
     * panel at zero that has to be discovered before it does anything. Two
     * sawtooth oscillators a fourteenth of a semitone apart, through a filter
     * a little over an octave above the note, with a contour on both.
     */
    a->v[MC_TUNE]     = 0.5f;                 /* dead centre */
    a->v[MC_GLIDE]    = 0.0f;
    a->v[MC_MOD_MIX]  = 0.0f;                 /* all oscillator 3 */
    a->v[MC_OSC_MOD]  = 0.0f;
    a->v[MC_OSC3_CTL] = 1.0f;                 /* following the keyboard */

    a->v[MC_O1_RANGE] = RANGE_8;
    a->v[MC_O1_WAVE]  = WAVE_SAW;
    a->v[MC_O2_RANGE] = RANGE_8;
    a->v[MC_O2_FREQ]  = 0.505f;               /* a whisker sharp, so they beat */
    a->v[MC_O2_WAVE]  = WAVE_SAW;
    a->v[MC_O3_RANGE] = RANGE_LO;             /* parked as the modulation source */
    a->v[MC_O3_FREQ]  = 0.5f;
    a->v[MC_O3_WAVE]  = WAVE_TRI;

    a->v[MC_V1]  = 0.80f;  a->v[MC_ON1] = 1.0f;
    a->v[MC_V2]  = 0.65f;  a->v[MC_ON2] = 1.0f;
    a->v[MC_V3]  = 0.50f;  a->v[MC_ON3] = 0.0f;
    a->v[MC_VN]  = 0.30f;  a->v[MC_ONN] = 0.0f;  a->v[MC_NCOLOUR] = 1.0f;  /* white */
    a->v[MC_VI]  = 0.30f;  a->v[MC_ONI] = 0.0f;  a->v[MC_IMODE]   = 1.0f;  /* feedback */

    a->v[MC_FILT_MOD] = 0.0f;
    a->v[MC_KBD1]     = 1.0f;                 /* a third of keyboard tracking */
    a->v[MC_KBD2]     = 0.0f;
    a->v[MC_CUTOFF]   = 0.62f;
    a->v[MC_EMPHASIS] = 0.35f;
    a->v[MC_CONTOUR]  = 0.50f;

    a->v[MC_FA] = logmap_inv(0.010f, ENV_MIN, ENV_MAX);
    a->v[MC_FD] = logmap_inv(0.250f, ENV_MIN, ENV_MAX);
    a->v[MC_FS] = 0.30f;
    a->v[MC_LA] = logmap_inv(0.005f, ENV_MIN, ENV_MAX);
    a->v[MC_LD] = logmap_inv(0.400f, ENV_MIN, ENV_MAX);
    a->v[MC_LS] = 0.75f;

    a->v[MC_VOLUME] = 0.70f;
}

void mg_ui_patch(mg_app_t *a)
{
    mg_patch_t *p = &a->patch;
    const float *v = a->v;

    p->tune     = (v[MC_TUNE] - 0.5f) * 2.0f * TUNE_SEMIS;
    p->glide    = v[MC_GLIDE] * v[MC_GLIDE] * GLIDE_MAX;
    p->mod_mix  = v[MC_MOD_MIX];
    p->osc_mod  = v[MC_OSC_MOD]  > 0.5f;
    p->osc3_kbd = v[MC_OSC3_CTL] > 0.5f;

    p->range[0] = (uint8_t)v[MC_O1_RANGE];
    p->range[1] = (uint8_t)v[MC_O2_RANGE];
    p->range[2] = (uint8_t)v[MC_O3_RANGE];
    p->wave[0]  = (uint8_t)v[MC_O1_WAVE];
    p->wave[1]  = (uint8_t)v[MC_O2_WAVE];
    p->wave[2]  = (uint8_t)v[MC_O3_WAVE];
    p->detune[0] = 0.0f;              /* oscillator 1 is the reference */
    p->detune[1] = (v[MC_O2_FREQ] - 0.5f) * 2.0f * DETUNE_SEMIS;
    p->detune[2] = (v[MC_O3_FREQ] - 0.5f) * 2.0f * DETUNE_SEMIS;

    p->vol[0] = v[MC_V1];  p->on[0] = v[MC_ON1] > 0.5f;
    p->vol[1] = v[MC_V2];  p->on[1] = v[MC_ON2] > 0.5f;
    p->vol[2] = v[MC_V3];  p->on[2] = v[MC_ON3] > 0.5f;

    p->noise_vol  = v[MC_VN];
    p->noise_on   = v[MC_ONN] > 0.5f;
    p->noise_pink = v[MC_NCOLOUR] < 0.5f;     /* the upper legend is WHITE */
    p->fb_vol     = v[MC_VI];
    p->fb_on      = v[MC_ONI] > 0.5f;
    p->fb_mode    = v[MC_IMODE] > 0.5f;

    p->filt_mod = v[MC_FILT_MOD] > 0.5f;
    /*
     * One switch is a third of keyboard tracking, the other two thirds, and
     * both together is all of it - not one and a third. That is the Model D's
     * own arrangement and it is why there are two switches rather than a knob.
     */
    p->kbd_track = (uint8_t)((v[MC_KBD1] > 0.5f ? 1 : 0) + (v[MC_KBD2] > 0.5f ? 2 : 0));

    p->cutoff   = (v[MC_CUTOFF] - 0.5f) * 8.0f;
    p->emphasis = v[MC_EMPHASIS];
    p->contour  = v[MC_CONTOUR];

    p->fa = logmap(v[MC_FA], ENV_MIN, ENV_MAX);
    p->fd = logmap(v[MC_FD], ENV_MIN, ENV_MAX);
    p->fs = v[MC_FS];
    p->la = logmap(v[MC_LA], ENV_MIN, ENV_MAX);
    p->ld = logmap(v[MC_LD], ENV_MIN, ENV_MAX);
    p->ls = v[MC_LS];

    p->master = v[MC_VOLUME];
}

/*
 * Every number on this panel is formatted out of integers, and that is a hard
 * rule rather than a style: a float handed to snprintf is promoted to double
 * by the language, apps link -nostdlib against a table with only some of the
 * double helpers on it, and the result is an app that fails to load. The build
 * turns that into a compile error with -Werror=double-promotion, and this is
 * the file where it would otherwise keep happening.
 */
static void dec1(char *buf, int size, float x)
{
    const int n = (int)(x * 10.0f + (x < 0.0f ? -0.5f : 0.5f));
    snprintf(buf, size, "%s%d.%d", (n < 0 && n > -10) ? "-" : "",
             n / 10, (n < 0 ? -n : n) % 10);
}

static void seconds(char *buf, int size, float s)
{
    if (s < 1.0f) {
        snprintf(buf, size, "%d ms", (int)(s * 1000.0f + 0.5f));
    } else {
        const int n = (int)(s * 10.0f + 0.5f);
        snprintf(buf, size, "%d.%d s", n / 10, n % 10);
    }
}

void mg_ui_value(const mg_app_t *a, int ctl, char *buf, int size)
{
    const float v = a->v[ctl];

    switch (ctl) {
    case MC_TUNE: {
        const int cents = (int)((v - 0.5f) * 2.0f * TUNE_SEMIS * 100.0f +
                                (v >= 0.5f ? 0.5f : -0.5f));
        snprintf(buf, size, "%+d ct", cents);
        break;
    }
    case MC_O2_FREQ:
    case MC_O3_FREQ:
        dec1(buf, size, (v - 0.5f) * 2.0f * DETUNE_SEMIS);
        break;

    case MC_O1_RANGE: case MC_O2_RANGE: case MC_O3_RANGE:
        snprintf(buf, size, "%s", mg_range_name((int)v));
        break;

    case MC_O1_WAVE: case MC_O2_WAVE: case MC_O3_WAVE:
        snprintf(buf, size, "%s", mg_wave_name((int)v));
        break;

    case MC_CUTOFF:
        dec1(buf, size, (v - 0.5f) * 8.0f);
        break;

    case MC_FA: case MC_FD: case MC_LA: case MC_LD:
        seconds(buf, size, logmap(v, ENV_MIN, ENV_MAX));
        break;

    default:
        /* Everything else reads 0 to 10, which is what the instrument's own
           knobs are marked and what a patch sheet would be written in. */
        dec1(buf, size, v * 10.0f);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Painting                                                            */
/* ------------------------------------------------------------------ */

/*
 * How big a knob fits in a cell.
 *
 * Worked out rather than tabulated because the four sections have four
 * different cell shapes and a table of radii is a table that has to be
 * revisited every time a column moves. The caption and the value take a text
 * line each, the ticks stand fourteen pixels outside the face, and what is
 * left over is the knob.
 */
static int16_t knob_radius(ngl_rect_t cell, bool has_cap)
{
    const int16_t th   = (int16_t)ngl_font_small.height;
    const int16_t used = (int16_t)((has_cap ? th : 0) + th);

    int16_t by_w = (int16_t)((cell.w - 10) / 2 - 14);
    int16_t by_h = (int16_t)((cell.h - used - 10) / 2 - 14);

    int16_t r = by_w < by_h ? by_w : by_h;
    if (r > KNOB_MAX) { r = KNOB_MAX; }
    if (r < 12)       { r = 12; }
    return r;
}

/* The waveform glyphs, in the oscillator's own order. wg.h's numbering is
   nobody's ABI, so the mapping lives here where both ends are visible. */
static const uint8_t WG_OF_WAVE[WAVE_N] = {
    WG_TRI, WG_TRISAW, WG_SAW, WG_SQUARE, WG_WIDE, WG_NARROW
};

static void paint_ctl(mg_app_t *a, int i)
{
    const turn_t   *g = &a->gfx;
    const mg_def_t *d = &mg_defs[i];
    const ngl_rect_t r = L.cell[i];
    const int16_t th = (int16_t)ngl_font_small.height;

    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    if (d->kind == CK_SLIDER) {
        mg_fader(g, r, a->v[i]);
        return;
    }

    turn_fill(g, r, TH_BG);

    if (d->cap) {
        turn_text_mid(g, r.x, r.y, r.w, d->cap, &ngl_font_small, TH_TEXT_DIM, TH_BG);
    }

    if (d->kind == CK_SWITCH) {
        /* A switch has no value line: both its legends are showing and the lit
           one is the value. So it takes the cell under the caption, up to the
           cap - past which two stacked caps stop looking like a switch and
           start looking like two buttons. */
        const int16_t top = (int16_t)(r.y + (d->cap ? th + 4 : 4));
        int16_t h = (int16_t)(r.y + r.h - 6 - top);
        if (h > SWITCH_MAX) { h = SWITCH_MAX; }
        const int16_t w = (int16_t)(r.w > 130 ? 130 : r.w - 12);
        wg_switch(g, ngl_rect((int16_t)(r.x + (r.w - w) / 2), top, w, h),
                  a->v[i] > 0.5f, d->lab_a, d->lab_b);
        return;
    }

    const int16_t rad = knob_radius(r, d->cap != NULL);
    const int16_t top = (int16_t)(r.y + (d->cap ? th : 0));
    const int16_t bot = (int16_t)(r.y + r.h - th);
    const int16_t cx  = (int16_t)(r.x + r.w / 2);
    const int16_t cy  = (int16_t)((top + bot) / 2);

    if (d->kind == CK_SELECT) {
        const float norm = (d->steps > 1) ? a->v[i] / (float)(d->steps - 1) : 0.0f;
        wg_knob(g, cx, cy, rad, norm, d->steps);

        /* A waveform selector shows the shape in its face; a range selector
           has nothing to draw there, and its value line already says "8'". */
        if (i == MC_O1_WAVE || i == MC_O2_WAVE || i == MC_O3_WAVE) {
            const int w = (int)a->v[i];
            wg_wave(g, cx, cy, (int16_t)(rad * 2 / 5), (int16_t)(rad * 4 / 15),
                    WG_OF_WAVE[w % WAVE_N], 1);
        }
    } else {
        wg_knob(g, cx, cy, rad, a->v[i], 0);
    }

    char buf[24];
    mg_ui_value(a, i, buf, sizeof buf);
    turn_text_mid(g, r.x, bot, r.w, buf, &ngl_font_small, TH_TEXT, TH_BG);
}

static void paint_arrow(const turn_t *g, ngl_rect_t r, bool left)
{
    turn_fill(g, r, TH_BG);
    turn_round(g, r, 8, TH_KEY_FILL);
    turn_round_frame(g, r, 8, TH_KEY_EDGE, 2);

    const int16_t cx = (int16_t)(r.x + r.w / 2);
    const int16_t cy = (int16_t)(r.y + r.h / 2);
    const int16_t d  = 9;
    const int16_t x0 = (int16_t)(cx + (left ? d / 2 : -d / 2));
    const int16_t x1 = (int16_t)(cx - (left ? d / 2 : -d / 2));

    turn_line_aa(g, x0, (int16_t)(cy - d), x1, cy, TH_TEXT);
    turn_line_aa(g, x1, cy, x0, (int16_t)(cy + d), TH_TEXT);
}

static void paint_close(const turn_t *g)
{
    const ngl_rect_t r = L.close;
    turn_round(g, r, 10, TH_CLOSE_FILL);
    turn_round_frame(g, r, 10, TH_CLOSE_LINE, 2);

    const int16_t p = 15;
    turn_line_aa(g, (int16_t)(r.x + p), (int16_t)(r.y + p),
                    (int16_t)(r.x + r.w - p), (int16_t)(r.y + r.h - p), TH_CLOSE_X);
    turn_line_aa(g, (int16_t)(r.x + r.w - p), (int16_t)(r.y + p),
                    (int16_t)(r.x + p), (int16_t)(r.y + r.h - p), TH_CLOSE_X);
}

/*
 * The header, which carries the four things that are true whatever page is up:
 * what this is, which sections are showing, how the two loops are doing, and
 * the way out.
 *
 * The instruments are here rather than in a footer because the footer is the
 * keyboard. They are deliberately terse - a frame rate, the audio load, and
 * the underrun count - because the one that matters is the last: any number
 * there at all means the sound broke, and it is drawn in red when it is not
 * zero.
 */
static void paint_header(mg_app_t *a)
{
    const turn_t *g = &a->gfx;
    char buf[64];

    turn_fill(g, L.header, TH_BG);
    turn_text(g, 16, 10, "MOOG", &ngl_font_small, TH_ACCENT, TH_BG);

    snprintf(buf, sizeof buf, "%s / %s",
             SECTION[a->page][0], SECTION[a->page][1]);
    turn_text(g, 110, 10, buf, &ngl_font_small, TH_TEXT_DIM, TH_BG);

    mg_meters_t m;
    mg_audio_meters(&m);

    if (!a->audio) {
        turn_text_right(g, 0, 10, (int16_t)(L.close.x - 16), "no codec",
                        &ngl_font_small, TH_BAD, TH_BG);
    } else {
        snprintf(buf, sizeof buf, "%ufps %u.%u%% u%u",
                 (unsigned)a->fps, (unsigned)(m.load_pm / 10),
                 (unsigned)(m.load_pm % 10), (unsigned)m.underruns);
        turn_text_right(g, 0, 10, (int16_t)(L.close.x - 16), buf, &ngl_font_small,
                        m.underruns ? TH_BAD : TH_TEXT_DIM, TH_BG);
    }

    paint_close(g);
    turn_hline(g, 0, (int16_t)(HEADER_H - 1), L.w, TH_RULE);
}

static void paint_page(mg_app_t *a)
{
    const turn_t *g = &a->gfx;

    turn_fill(g, ngl_rect(PAGE_X, PAGE_Y, PAGE_W, PAGE_H), TH_BG);
    turn_fill(g, L.rule[a->page], TH_RULE);

    for (int i = 0; i < MC_COUNT; i++) {
        if (mg_defs[i].page == a->page) {
            paint_ctl(a, i);
        }
    }
    if (a->page == 0) {
        const int16_t th = (int16_t)ngl_font_small.height;
        turn_text_mid(g, L.o1_gap.x, L.o1_gap.y, L.o1_gap.w, "OSC1 FREQ",
                      &ngl_font_small, TH_TEXT_FAINT, TH_BG);
        turn_text_mid(g, L.o1_gap.x,
                      (int16_t)(L.o1_gap.y + L.o1_gap.h / 2 - th / 2), L.o1_gap.w,
                      "REFERENCE", &ngl_font_small, TH_TEXT_FAINT, TH_BG);
    }
    if (a->page == 1) {
        turn_text_mid(g, L.lamp.x, (int16_t)(L.lamp.y + 10), L.lamp.w, "OVERLOAD",
                      &ngl_font_small, TH_TEXT_DIM, TH_BG);
        mg_lamp(g, ngl_rect((int16_t)(L.lamp.x + L.lamp.w / 2 - 18),
                            (int16_t)(L.lamp.y + 54), 36, 36),
                mg_audio_overload());
    }
}

void mg_ui_repaint(mg_app_t *a)
{
    const turn_t *g = &a->gfx;

    turn_clear(g, TH_BG);
    paint_header(a);
    paint_page(a);
    paint_arrow(g, L.prev, true);
    paint_arrow(g, L.next, false);
    turn_text_mid(g, L.vol_cap.x, L.vol_cap.y, L.vol_cap.w, "VOL",
                  &ngl_font_small, TH_TEXT_DIM, TH_BG);
    paint_ctl(a, MC_VOLUME);
    mg_keys(g, L.keys, a->held);

    turn_present(g, ngl_rect(0, 0, g->w, g->h));

    a->repaint_all  = false;
    a->ctl_dirty[0] = a->ctl_dirty[1] = 0;
    a->keys_changed = 0;
    a->head_dirty   = false;
}

uint32_t mg_ui_paint(mg_app_t *a)
{
    const turn_t *g = &a->gfx;
    uint32_t us = 0;

    if (a->repaint_all) {
        const uint32_t t0 = (uint32_t)neos_uptime_us();
        mg_ui_repaint(a);
        return (uint32_t)neos_uptime_us() - t0;
    }

    for (int i = 0; i < MC_COUNT; i++) {
        if (!mg_is_dirty(a, i)) {
            continue;
        }
        if (mg_defs[i].page != a->page && mg_defs[i].page != MG_BOTH) {
            continue;                    /* changed on a page nobody is looking at */
        }
        paint_ctl(a, i);
        us += turn_present(g, L.cell[i]);
    }
    a->ctl_dirty[0] = a->ctl_dirty[1] = 0;

    /*
     * Only the keys that went down or came up. The whole keyboard is 1084 by
     * 152, which is 165,000 pixels to show that one key of 15,000 has moved -
     * and a key moves on every note, which is the most frequent repaint this
     * panel has.
     */
    if (a->keys_changed) {
        for (int k = 0; k < MG_KEYS; k++) {
            if (a->keys_changed & (1u << k)) {
                const ngl_rect_t touched = mg_key_paint(g, L.keys, k, a->held);
                us += turn_present(g, touched);
            }
        }
        a->keys_changed = 0;
    }

    if (a->head_dirty) {
        paint_header(a);
        us += turn_present(g, L.header);

        /* The lamp is on the same clock as the instruments: it is the only
           other thing on the panel that changes without anybody touching it. */
        if (a->page == 1) {
            const ngl_rect_t lr = ngl_rect((int16_t)(L.lamp.x + L.lamp.w / 2 - 18),
                                           (int16_t)(L.lamp.y + 54), 36, 36);
            mg_lamp(g, lr, mg_audio_overload());
            us += turn_present(g, lr);
        }
        a->head_dirty = false;
    }
    return us;
}
