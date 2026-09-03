/*
 * palette.c - colour, and only ever one kind of it.
 *
 * NeOS is phosphor-green on black, and a fractal viewer that opened in
 * magenta and orange would look like a program someone else wrote and dropped
 * on the card. So the gamut here is closed: deep bottle green and cold petrol
 * at the bottom, jade and sea green through the middle, and neon lime, spring
 * green and cyan at the top, with one near-white mint for the hottest band.
 * A new palette is a different walk through that gamut - a different picture
 * on the same machine, rather than a different machine.
 *
 * The walk is not free-form either. Every palette is a ladder: it starts in
 * the dark tier, climbs to the hot tier and comes back down through the
 * middle before it wraps, and the dark stops are given two or three times the
 * width of the hot ones. That is what makes the neon read as glow - a thin
 * bright band against a lot of dark water - instead of as a rash of colour.
 *
 * The split between pal_new and pal_map is what makes recolouring free.
 * pal_map is the scene's business: how a value becomes a position in the
 * cycle, which depends on how deep this particular view goes. pal_new is the
 * palette's: what colour sits at each position. Only the second changes on a
 * PALETTE tap, so the stored per-pixel indices stay exactly right and the
 * screen can be repainted from a lookup table rather than re-iterated.
 */

#include "mandel.h"

ngl_color_t g_pal[256];
float       g_pal_k     = 1.0f;
float       g_pal_phase = 0.0f;

/* ------------------------------------------------------------------ gamut */

typedef struct { uint8_t r, g, b; } rgb_t;

/* The open water: what most of an exterior is painted in. */
static const rgb_t DARK[] = {
    {  0,   6,   5 },       /* the CRT is off, with a green cast */
    {  0,  14,  11 },
    {  3,  24,  14 },       /* bottle */
    {  0,  18,  26 },       /* cold petrol */
    {  6,  26,  22 },
};

/* The body of the gradient. */
static const rgb_t MID[] = {
    {  0,  64,  42 },       /* sea */
    {  8,  78,  32 },       /* moss */
    {  0,  74,  84 },       /* teal */
    { 24, 112,  62 },       /* jade */
    {  0, 116, 124 },       /* petrol cyan */
    { 46, 140,  80 },
    { 10, 132, 108 },
};

/* The bands close to the boundary, where the picture wants to glow. */
static const rgb_t HOT[] = {
    {   0, 255,  90 },      /* the terminal green, TH_ACCENT */
    {   0, 255, 200 },      /* neon cyan */
    {  96, 255,  60 },      /* neon lime */
    {   0, 224, 164 },      /* spring */
    { 158, 255,  74 },      /* pale lime */
    { 190, 255, 160 },      /* TH_GLOW */
    { 216, 255, 238 },      /* mint white, the hottest thing here */
};

#define NDARK ((int)(sizeof(DARK) / sizeof(DARK[0])))
#define NMID  ((int)(sizeof(MID)  / sizeof(MID[0])))
#define NHOT  ((int)(sizeof(HOT)  / sizeof(HOT[0])))

/*
 * The shapes a palette is allowed to take, as tiers around the cycle.
 *
 * Every one of them starts dark and ends in the middle tier, so the wrap from
 * the last stop back to the first is a short step rather than a hard edge -
 * which matters, because that wrap happens once per palette cycle across the
 * picture and a hard edge there draws a contour line that is not in the
 * fractal.
 */
static const char *const LADDER[] = {
    "DMHM",
    "DMHMM",
    "DMHMHM",
    "DMHMDM",
    "DMHHM",
    "DHMHM",
};
#define NLADDER ((int)(sizeof(LADDER) / sizeof(LADDER[0])))

/* --------------------------------------------------------------- the seed */
/*
 * A generator of its own, so that pal_new(seed) is reproducible whatever the
 * rest of the program has been asking for random numbers in the meantime -
 * and so that asking for a palette does not shift the sequence the location
 * search draws from.
 */
static uint32_t s_prng;
static uint32_t s_seed = 1;

static uint32_t prnd(void)
{
    uint32_t x = s_prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_prng = x;
    return x;
}

static int prnd_range(int lo, int hi)
{
    return hi <= lo ? lo : lo + (int)(prnd() % (uint32_t)(hi - lo + 1));
}

uint32_t pal_seed(void) { return s_seed; }

/* ---------------------------------------------------------------- the walk */

static rgb_t pick(char tier, const rgb_t *last)
{
    const rgb_t *tab;
    int n;

    switch (tier) {
    case 'D': tab = DARK; n = NDARK; break;
    case 'H': tab = HOT;  n = NHOT;  break;
    default:  tab = MID;  n = NMID;  break;
    }

    /* Two tries at not repeating the stop before this one. Two, not a loop:
       a tier with one usable entry left must still return something. */
    rgb_t c = tab[prnd_range(0, n - 1)];
    if (last && c.r == last->r && c.g == last->g && c.b == last->b) {
        c = tab[prnd_range(0, n - 1)];
    }
    return c;
}

void pal_new(uint32_t seed)
{
    if (!seed) {
        seed = rnd() | 1u;
    }
    s_seed = seed;
    s_prng = seed;

    const char *lad = LADDER[prnd_range(0, NLADDER - 1)];
    int n = 0;
    while (lad[n]) {
        n++;
    }

    rgb_t stop[8];
    int   w[8];
    int   tot = 0;

    for (int i = 0; i < n; i++) {
        stop[i] = pick(lad[i], i ? &stop[i - 1] : NULL);
        /* The dark stops get the room. A gradient that spends most of its
           length in the deep end and only flares at the boundary is what a
           fractal looks like from a distance; equal weights make it a rainbow. */
        switch (lad[i]) {
        case 'D': w[i] = prnd_range(3, 6); break;
        case 'H': w[i] = prnd_range(1, 3); break;
        default:  w[i] = prnd_range(2, 4); break;
        }
        tot += w[i];
    }

    /* Segment boundaries over entries 1..255; entry 0 is the interior. */
    int cut[9];
    int cum = 0;
    cut[0] = 1;
    for (int i = 0; i < n; i++) {
        cum += w[i];
        cut[i + 1] = 1 + (255 * cum) / tot;
        if (cut[i + 1] <= cut[i]) {
            cut[i + 1] = cut[i] + 1;
        }
    }
    cut[n] = 256;

    for (int i = 0; i < n; i++) {
        const rgb_t a = stop[i];
        const rgb_t b = stop[(i + 1) % n];
        const int   c0 = cut[i], c1 = cut[i + 1];
        const int   len = c1 - c0;

        for (int j = c0; j < c1 && j < 256; j++) {
            const int t = ((j - c0) * 256) / (len > 0 ? len : 1);
            /* smoothstep, so the joins between stops are not visible as
               creases running through the picture. */
            int s = (t * t * (768 - 2 * t)) >> 16;
            if (s > 255) { s = 255; }

            const int r = a.r + ((int)b.r - (int)a.r) * s / 255;
            const int g = a.g + ((int)b.g - (int)a.g) * s / 255;
            const int bl = a.b + ((int)b.b - (int)a.b) * s / 255;
            g_pal[j] = NGL_RGB(r, g, bl);
        }
    }

    /* The interior is not part of the cycle: it is the one thing on screen
       that is a shape rather than a gradient, and it reads best as the ground
       the rest is drawn on. */
    g_pal[0] = NGL_RGB(0, 5, 4);
}

/* ---------------------------------------------------------------- the fit */

void pal_map(const scene_t *s)
{
    const float vmax = s->vmax > 1.0f ? s->vmax : 1.0f;
    const int   cyc  = s->cycles ? s->cycles : 1;

    /* 255 positions per trip round, walked on sqrt(v) - see pal_shade. */
    g_pal_k     = 255.0f * (float)cyc / fsqrt_(vmax);
    g_pal_phase = (float)s->phase;
}
