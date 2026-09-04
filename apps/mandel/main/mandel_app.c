/*
 * mandel_app.c - MANDEL, a fractal viewer for the Tab5.
 *
 * Three fractals - the Mandelbrot set, a Julia set and a Lyapunov diagram -
 * and a 1280x720 sheet of glass to steer them with.
 *
 *   drag a frame   that rectangle becomes the view
 *   tap            dive in three times, centred where you touched
 *   ZOOM OUT       back to the view you came in from
 *   SAVE           the picture to album/ on the card, greyscale, with the
 *                  view's coordinates in it so it can be drawn again
 *   PALETTE        the same picture, a new green
 *   PLACE          somewhere worth looking in this fractal, found by measuring
 *   PLACE, held    all the way back out to the whole of it
 *   MODE           the next fractal, framed whole
 *
 * Untouched, it runs itself: it opens on somewhere the search found, draws the
 * next one on the other core while you look at this one, and dissolves across
 * when it is ready. The first touch ends that for good and hands over whatever
 * is on the glass. See "the attract loop" below.
 *
 * The greenbox original had two buttons and therefore no navigation at all -
 * it searched for a good view and showed it to you, which was the right
 * answer for that machine. Here the navigation is the program, and everything
 * below follows from making it feel immediate on a panel this size.
 *
 * ---------------------------------------------------------------- the wait
 *
 * 1280x664 pixels at up to 400 iterations is seconds of arithmetic, so the
 * picture is built in passes, each one complete and each one better than the
 * last, and every pass is resumable between rows.
 *
 * A COARSE pass samples an 8x8 grid - 1.5% of the pixels - and is not drawn.
 * It is measured instead: how deep this particular frame actually goes decides
 * what one trip round the palette should span, which is the difference
 * between a deep zoom that is a gradient and one that is two flat colours.
 * Then that grid is painted, so there is a whole picture on the panel about
 * forty milliseconds in.
 *
 * The FINE pass is interlaced: rows 0, 4, 8 ... first, each one replicated
 * over the three below it, then rows 2, 6, 10 ..., then the odd rows. Every
 * sample is taken exactly once - the interlace costs nothing over a straight
 * top-to-bottom raster - but the screen reaches full horizontal resolution
 * after a quarter of the work rather than a quarter of the way down.
 *
 * Every pixel of every pass is ordered-dithered on its way into RGB565, which
 * is where the bands in a gradient this smooth actually come from - see the
 * note over g_bayer8 in palette.c. It costs an add and a table lookup next to
 * a few hundred iterations, and it is the difference between open water that
 * is a gradient and open water that is a contour map.
 *
 * The REFINE pass goes back over the pixels where the picture steps between
 * neighbours - the fringe beside a filament, the edge of the set - and
 * samples those five ways. Left out of the fine pass on purpose: it wants
 * five samples on between a tenth and two thirds of the screen, and folded in
 * it would have quadrupled the wait before anything appeared. Done afterwards
 * it costs nothing that is being waited on, and an interrupted refinement
 * leaves a picture that is merely less smooth than it was going to be.
 *
 * ---------------------------------------------------------------- two cores
 *
 * The P4 has two 400 MHz cores and the main task is pinned to the first, so
 * half of every pass is handed to a pthread - NeOS's loader exports the POSIX
 * calls and IDF's default pthread has no affinity, so the scheduler puts it
 * on the core that is otherwise running the idle task.
 *
 * The split is by row, and by row for a reason: two threads writing
 * alternating pixels of one row would share cache lines, so the buffers are
 * aligned to 64 bytes and a row is 2560 of them - each thread's writes land
 * in lines no other thread touches. Adjacent rows of the same interlace phase
 * cost nearly the same, so the two halves finish together and neither waits
 * long. There are no atomics on this core (RV32IMAFC has no A extension), so
 * the handshake is two counters with a single writer each and a fence between.
 *
 * If the thread will not start, every path falls back to doing both rows
 * itself and the program is simply half as fast.
 *
 * ------------------------------------------------------------- orientation
 *
 * The panel is pinned landscape and only ever flips end over end. A fractal
 * has no up, but a set of controls does, and half a screen of buttons
 * reshuffling because the tablet tipped is worse than useless. So the display
 * is locked - which also stops the OS reallocating the back buffer from its
 * watcher task in the middle of a render - and this reads the accelerometer
 * itself, at a point in its own loop where nothing is half-drawn. A 180
 * degree flip needs no re-render at all: the logical framebuffer is unchanged
 * and only the mapping onto the panel turns over.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngl.h"
#include "ngl_theme.h"

#include "neos_api.h"
#include "neos_orient.h"
#include "neos_status.h"
#include "neos_sys.h"       /* neos_psram_free, for the note about the buffers */

#include "mandel.h"

/* ------------------------------------------------------------------ */
/* Geometry and buffers                                                */
/* ------------------------------------------------------------------ */

#define CO           8      /* the coarse grid's cell, in pixels */
#define FLUSH_ROWS  48      /* rows to let pile up before pushing to the panel */
#define PROG_H       4      /* the render progress line along the bottom edge */

static ngl_rect_t s_area;               /* what the app may draw in */
static int        s_pw, s_ph;           /* the picture, in pixels */
static int        s_cgw, s_cgh;         /* the coarse grid, in cells */

/*
 * The picture is kept, not just drawn.
 *
 * Everything this program puts on top of the fractal - a rubber band, the
 * controls, the progress line - has to be taken off again, and the only way
 * to do that without re-iterating is to own a copy of what was underneath.
 * It is wrapped as an ngl surface so that putting a piece of it back is one
 * ngl_blit, which is a memcpy per row.
 *
 * s_idx is the palette index behind each pixel, and it is what makes PALETTE
 * instant: a new palette is a new lookup table over the same 850 KB, so the
 * screen repaints without iterating anything. It is also what the refine pass
 * reads to find the edges, which is why it is indices and not colours - "do
 * these two pixels differ by more than five palette steps" means the same
 * thing at every zoom, in every mode, under every palette.
 */
typedef struct {
    ngl_color_t   *pic;
    ngl_surface_t *pics;
    uint8_t       *idx;
    void          *pic_raw, *idx_raw;
} pbuf_t;

/*
 * Two of them, because the attract loop draws the next picture while the last
 * one is still on the panel and then dissolves between the two.
 *
 * s_vis is the buffer the panel is showing; s_tgt is the one the passes are
 * writing into. They are the same number for everything a finger does - a
 * zoom draws over what it replaces, as it always did - and differ only while
 * a picture is being made ahead of time. Buffer 1 is optional: without the
 * room for it the program is exactly what it was, minus the attract loop.
 */
static pbuf_t s_buf[2];
static int    s_vis;
static int    s_tgt;

/* Aliases for s_buf[s_tgt], because every pass in this file writes through
   them and threading an index through all of it would earn nothing. */
static ngl_color_t   *s_pic;
static ngl_surface_t *s_pics;
static uint8_t       *s_idx;

static float         *s_cv;             /* the coarse grid's values */

static void target_set(int i)
{
    s_tgt  = i;
    s_pic  = s_buf[i].pic;
    s_pics = s_buf[i].pics;
    s_idx  = s_buf[i].idx;
}

static inline ngl_color_t *pic_row(int y) { return s_pic + (size_t)y * s_pw; }
static inline uint8_t     *idx_row(int y) { return s_idx ? s_idx + (size_t)y * s_pw : NULL; }

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static scene_t g_scene;

/*
 * The scene the panel is showing, which is g_scene except while the next one
 * is being drawn ahead of time. The readout at the bottom is about this one,
 * and so is everything the user gets back if they take over mid-render.
 */
static scene_t s_shown;

#define HIST_MAX 24
static scene_t s_hist[HIST_MAX];
static int     s_nhist;
static int     s_pal_no = 1;

/* The attract loop's state; the section near the bottom is what drives it. */
enum { AT_OFF = 0, AT_SHOW, AT_MAKE, AT_FADE };
static uint8_t s_at;

enum { RP_COARSE = 0, RP_MAP, RP_ROWS, RP_REFINE, RP_DONE };
static uint8_t  s_rp = RP_DONE;
static int      s_sub;                  /* interlace phase, 0..3 */
static int      s_cur;                  /* next step within the pass */
static uint32_t s_t0, s_ms_fine;
static uint32_t s_ss_a, s_ss_b;         /* supersampled pixels, per thread */
static uint32_t s_yield_t;

static int  s_pend_rows;                /* blitted but not yet flushed */
static bool s_pend_ui;                  /* ... and it covered the controls */

/* The rows of one interlace phase, in the order they are worth having. */
static const int OFF[4] = { 0, 2, 1, 3 };

static int sub_rows(int p)
{
    return OFF[p] < s_ph ? (s_ph - 1 - OFF[p]) / 4 + 1 : 0;
}

static int sub_steps(int p)
{
    return (sub_rows(p) + 1) / 2;       /* two rows go out per step */
}

/* ------------------------------------------------------------------ */
/* The second core                                                     */
/* ------------------------------------------------------------------ */

enum { WK_ROW = 0, WK_REFINE };

/*
 * Two counters, one writer each, and a fence between the data and the counter
 * that publishes it. s_w_seq only moves when main has already seen the last
 * ack, so everything the worker reads out of s_w_row / s_w_kind is stable for
 * as long as it is working on it.
 */
static volatile int32_t s_w_seq;
static volatile int32_t s_w_ack;
static volatile int32_t s_w_row;
static volatile int8_t  s_w_kind;
static volatile int8_t  s_w_res;        /* WK_REFINE: did the row change */
static volatile int8_t  s_w_quit;
static volatile int8_t  s_w_pause;
/*
 * Whether there is a render on at all. Spinning is the right way to wait
 * microseconds for the next row and quite the wrong way to sit out a
 * minute of somebody looking at a finished picture, and only the main loop
 * knows which of the two is happening.
 */
static volatile int8_t  s_w_busy;

static pthread_t s_w_thread;
static bool      s_w_ok;        /* hand it rows */
/*
 * A thread exists and has to be joined before this image is unloaded.
 *
 * Separate from s_w_ok because the two answer different questions - a
 * worker that stopped answering is no longer worth giving rows to but is
 * emphatically still running - and set in exactly one place, next to the
 * pthread_create it describes. This was two flags set in two places once,
 * the second of them never actually set, so nothing was ever joined and
 * the thread outlived the app: the whole machine went sluggish after
 * mandel had been and gone, because a priority 5 spinner was still on a
 * core, in an ELF image that had been freed underneath it.
 */
static bool      s_w_live;

static void row_compute(int y);
static bool row_refine(int y, uint32_t *count);

static void *worker_main(void *arg)
{
    (void)arg;
    int spins = 0;

    for (;;) {
        if (s_w_quit) {
            break;
        }
        const int32_t seq = s_w_seq;
        if (seq != s_w_ack) {
            __sync_synchronize();
            const int y = (int)s_w_row;
            if (s_w_kind == WK_ROW) {
                row_compute(y);
            } else {
                uint32_t n = 0;
                s_w_res = row_refine(y, &n) ? 1 : 0;
                s_ss_b += n;
            }
            __sync_synchronize();
            s_w_ack = seq;
            spins = 0;
            continue;
        }

        /*
         * Spinning is right between rows - the next one is microseconds away
         * and a 10 ms tick would throw the parallelism away - and wrong for
         * any longer than that, because this thread outranks the idle task
         * whose starvation the watchdog is watching for. So: spin about as
         * long as one row takes, then block. s_w_pause is main saying it is
         * about to block too, which is the only moment both cores are free to
         * run anything else.
         */
        /*
         * Ten, not one. The tick is 100 Hz, so pdMS_TO_TICKS(1) rounds to
         * zero ticks and vTaskDelay(0) yields to equals without ever
         * leaving the ready list - which for a priority 5 task means the
         * idle task below it never runs. Asking for a millisecond here was
         * asking for nothing at all, and this thread simply spun.
         */
        if (!s_w_busy || s_w_pause || ++spins > 400000) {
            neos_sleep_ms(10);
            spins = 0;
        }
    }
    return NULL;
}

static void worker_start(void)
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return;
    }
    /* The render path is a handful of floats and no recursion; the default
       3 KB would do, and 8 KB costs nothing worth counting. */
    pthread_attr_setstacksize(&attr, 8192);
    s_w_live = pthread_create(&s_w_thread, &attr, worker_main, NULL) == 0;
    s_w_ok   = s_w_live;
    printf("[mandel] second core: %s\n", s_w_ok ? "yes" : "no, running alone");
}

/*
 * Every path out of main() goes through here, including the ones that fail.
 * The thread is executing code inside an ELF image that NeOS frees the moment
 * main returns, so leaving it running is not a leak, it is a crash a fraction
 * of a second later with nothing to connect it to.
 */
static void worker_stop(void)
{
    if (!s_w_live) {
        return;
    }
    s_w_ok   = false;           /* no more rows, whatever else happens */
    s_w_quit = 1;
    pthread_join(s_w_thread, NULL);
    s_w_live = false;
    printf("[mandel] worker joined\n");
}

static void w_submit(int kind, int y)
{
    s_w_row  = y;
    s_w_kind = (int8_t)kind;
    __sync_synchronize();
    s_w_seq++;
}

/*
 * Wait for the row the worker was given. True if it came back.
 *
 * A plain spin is what this wants to be - the worker is on the other core and
 * the answer is microseconds away - but a plain spin is also a hang with no
 * way out if the worker ever stops answering, on a machine whose only escape
 * is a close button this thread has to reach. So the spin is bounded, then it
 * blocks, and after about a second it writes the worker off and returns false,
 * which every caller answers by doing the row itself. From then on the
 * program is simply the single-core one.
 */
static bool w_wait(void)
{
    static int late;
    uint32_t   spins = 0;

    while (s_w_ack != s_w_seq) {
        if (++spins > 2000000u) {
            spins = 0;
            neos_sleep_ms(10);      /* a real block; see the note in worker_main */
            if (++late > 100) {
                printf("[mandel] worker stopped answering, going it alone\n");
                s_w_ok = false;
                return false;
            }
        }
    }
    late = 0;
    __sync_synchronize();
    return true;
}

/*
 * Both cores off the buffers, and stay off.
 *
 * The dissolve reads the two pictures and writes a band of scratch, all from
 * this thread. Handing half the rows to the worker was worth about a third of
 * a frame and cost the correctness of the whole thing: the band's origin is
 * one shared int, so any row the worker was late finishing landed at an
 * offset measured from the *next* band - off the end of a 32-row buffer, with
 * the row it should have written left holding the last band's colours. That
 * is what the bright bands flashing through the fade were, and the stall in
 * front of them was w_wait spending its full second deciding the worker had
 * stopped answering.
 *
 * So the fade does not submit rows, and this makes sure nothing else is
 * outstanding either: anything already given out is waited for, and then the
 * thread is held in its blocking branch until the picture has changed hands.
 */
static void worker_park(void)
{
    if (s_w_live && s_w_ok && s_w_ack != s_w_seq) {
        w_wait();
    }
    s_w_busy  = 0;
    s_w_pause = 1;
}

static void worker_unpark(void)
{
    s_w_pause = 0;
}

/* ------------------------------------------------------------------ */
/* Painting the picture                                                */
/* ------------------------------------------------------------------ */

static void ui_paint(void);
static void restore(ngl_rect_t r);
static int16_t s_ui_top;                /* picture row where the controls start */

static void present(void)
{
    if (s_pend_ui) {
        ui_paint();
        s_pend_ui = false;
    }
    ngl_flush();
    s_pend_rows = 0;
}

/** Push `h` rows of the kept picture at row `y` to the panel, eventually. */
static void blit_band(int y, int h)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc || h <= 0) {
        return;
    }
    if (s_tgt != s_vis) {
        return;             /* drawing ahead: this picture is not on the panel yet */
    }
    if (y + h > s_ph) {
        h = s_ph - y;
    }
    const ngl_rect_t src = ngl_rect(0, (int16_t)y, (int16_t)s_pw, (int16_t)h);
    ngl_blit(sc, s_area.x, (int16_t)(s_area.y + y), s_pics, &src);

    if (y + h > s_ui_top) {
        s_pend_ui = true;
    }
    s_pend_rows += h;
    if (s_pend_rows >= FLUSH_ROWS) {
        present();
    }
}

static void row_compute(int y)
{
    ngl_color_t *pr = pic_row(y);
    uint8_t     *ir = idx_row(y);

    for (int x = 0; x < s_pw; x++) {
        const uint8_t s = pal_shade(fr_at(x, y));
        pr[x] = pal_px(s, x, y);
        if (ir) {
            ir[x] = s;
        }
    }
}

/*
 * One supersampled pixel: the centre colour the caller already has, blended
 * with the colours of the four quarter-points.
 *
 * The blend is of colours and not of values, and that is the whole reason it
 * helps. Halfway along a filament the five values are scattered across the
 * range, and the colour of their average is not the average of their colours -
 * it is just another arbitrary colour, which is what the speckle already was.
 * Blending what is actually shown also softens the edge of the set itself,
 * where the samples that land inside have no value to average at all.
 *
 * A tent, not a box: the centre is worth two of the corners, so the filter
 * takes the jaggedness off without turning a filament into a smear. The
 * divide by six is a multiply - 171/1024 is within a part in five hundred,
 * and this runs on every refined pixel.
 */
static ngl_color_t ss_colour(int x, int y, uint8_t base)
{
    float v[4];
    fr_at_ss4(x, y, v);

    int r = 2 * g_pal8[base][0];
    int g = 2 * g_pal8[base][1];
    int b = 2 * g_pal8[base][2];

    for (int i = 0; i < 4; i++) {
        const uint8_t c = pal_shade(v[i]);
        r += g_pal8[c][0];
        g += g_pal8[c][1];
        b += g_pal8[c][2];
    }
    /* The average is in eight bits, so it lands between two representable
       565 values as often as anything else does and wants the same dither. */
    return pal_mix((r * 171) >> 10, (g * 171) >> 10, (b * 171) >> 10, x, y);
}

/*
 * The third pass, one row: find the pixels where the picture steps between
 * neighbours and sample those five ways.
 *
 * It reads the index buffer and never writes it, so the neighbours it asks
 * about are always the ones the fine pass drew - no lag rows, no half-refined
 * comparisons - and a recolour can still repaint the whole screen from the
 * same bytes. What that costs is that refinement does not survive into the
 * indices, so a recolour comes back unrefined and has to be refined again.
 */
static bool row_refine(int y, uint32_t *count)
{
    const uint8_t *me = idx_row(y);
    if (!me) {
        return false;
    }
    const uint8_t *up = y > 0          ? me - s_pw : NULL;
    const uint8_t *dn = y + 1 < s_ph   ? me + s_pw : NULL;
    ngl_color_t   *pr = pic_row(y);
    bool changed = false;

    for (int x = 0; x < s_pw; x++) {
        const uint8_t s = me[x];
        if ((x > 0        && fr_jump(s, me[x - 1])) ||
            (x + 1 < s_pw && fr_jump(s, me[x + 1])) ||
            (up           && fr_jump(s, up[x]))     ||
            (dn           && fr_jump(s, dn[x]))) {
            pr[x] = ss_colour(x, y, s);
            changed = true;
            (*count)++;
        }
    }
    return changed;
}

/* ------------------------------------------------------------------ */
/* The passes                                                          */
/* ------------------------------------------------------------------ */

static float progress(void)
{
    switch (s_rp) {
    case RP_COARSE:
        return s_cgh ? 0.03f * (float)s_cur / (float)s_cgh : 0.0f;
    case RP_MAP:
        return 0.03f;
    case RP_ROWS: {
        const int n = sub_steps(s_sub);
        const float within = n ? (float)s_cur / (float)n : 1.0f;
        return 0.03f + 0.77f * 0.25f * ((float)s_sub + within);
    }
    case RP_REFINE:
        return 0.80f + 0.20f * (float)s_cur / (float)(s_ph ? s_ph : 1);
    default:
        return 1.0f;
    }
}

/* One block row of the coarse grid. Sampled, not drawn: see step_map. */
static void step_coarse(void)
{
    if (!s_cv || s_cur >= s_cgh) {
        s_rp = RP_MAP;
        return;
    }
    const int gy = s_cur++;
    int y = gy * CO + CO / 2;
    if (y >= s_ph) {
        y = s_ph - 1;
    }
    for (int gx = 0; gx < s_cgw; gx++) {
        int x = gx * CO + CO / 2;
        if (x >= s_pw) {
            x = s_pw - 1;
        }
        s_cv[gy * s_cgw + gx] = fr_at(x, y);
    }
}

/*
 * Fit the palette to what the coarse pass found, then put that grid on the
 * panel. Both at once, because the fit changes every colour and a grid
 * painted before it would have to be repainted straight after - a whole
 * screen of the wrong colours, for one frame, on every zoom.
 */
static void step_map(void)
{
    s_rp  = RP_ROWS;
    s_sub = 0;
    s_cur = 0;

    if (!s_cv) {
        /* No room for the grid: fit the palette to what the mode can produce
           at all, and let the fine pass draw over whatever is on screen. */
        g_scene.vmax = g_scene.mode == MODE_LYAP ? 300.0f : (float)g_scene.maxiter;
        pal_map(&g_scene);
        return;
    }

    g_scene.vmax = fr_vmax_for(g_scene.mode, s_cv, s_cgw * s_cgh, g_scene.maxiter);
    pal_map(&g_scene);

    for (int gy = 0; gy < s_cgh; gy++) {
        const int y0 = gy * CO;
        const int h  = (y0 + CO <= s_ph) ? CO : (s_ph - y0);
        ngl_color_t *pr = pic_row(y0);
        uint8_t     *ir = idx_row(y0);

        for (int gx = 0; gx < s_cgw; gx++) {
            const uint8_t     s = pal_shade(s_cv[gy * s_cgw + gx]);
            const ngl_color_t c = g_pal[s];
            const int x0 = gx * CO;
            const int w  = (x0 + CO <= s_pw) ? CO : (s_pw - x0);
            for (int i = 0; i < w; i++) {
                pr[x0 + i] = c;
                if (ir) {
                    ir[x0 + i] = s;
                }
            }
        }
        for (int k = 1; k < h; k++) {
            memcpy(pic_row(y0 + k), pr, (size_t)s_pw * sizeof(ngl_color_t));
            if (ir) {
                memcpy(idx_row(y0 + k), ir, (size_t)s_pw);
            }
        }
    }

    blit_band(0, s_ph);
    present();
}

/* How many rows one computed row stands for, in this interlace phase. */
static int rep_for_sub(int sub)
{
    return sub == 0 ? 4 : (sub == 1 ? 2 : 1);
}

static void row_publish(int y)
{
    const int rep = rep_for_sub(s_sub);
    ngl_color_t *src = pic_row(y);
    int h = 1;

    for (int k = 1; k < rep && y + k < s_ph; k++) {
        memcpy(pic_row(y + k), src, (size_t)s_pw * sizeof(ngl_color_t));
        h++;
    }
    blit_band(y, h);
}

static void step_rows(void)
{
    if (s_cur >= sub_steps(s_sub)) {
        present();
        if (++s_sub >= 4) {
            s_ms_fine = now_ms() - s_t0;
            s_rp  = s_idx ? RP_REFINE : RP_DONE;
            s_cur = 0;
            if (s_rp == RP_DONE) {
                ui_paint();
                ngl_flush();
            }
        } else {
            s_cur = 0;
        }
        return;
    }

    const int off = OFF[s_sub];
    const int ya  = off + 4 * (s_cur * 2);
    int       yb  = off + 4 * (s_cur * 2 + 1);
    if (yb >= s_ph) {
        yb = -1;
    }
    s_cur++;

    if (s_w_ok && yb >= 0) {
        w_submit(WK_ROW, yb);
        row_compute(ya);
        if (!w_wait()) {
            row_compute(yb);
        }
    } else {
        row_compute(ya);
        if (yb >= 0) {
            row_compute(yb);
        }
    }

    row_publish(ya);
    if (yb >= 0) {
        row_publish(yb);
    }
}

static void step_refine(void)
{
    if (s_cur >= s_ph) {
        present();
        s_rp = RP_DONE;
        ui_paint();
        ngl_flush();
        printf("[mandel] drawn in %u ms, refined %u%% in %u ms\n",
               (unsigned)s_ms_fine,
               (unsigned)((s_ss_a + s_ss_b) * 100u / (unsigned)(s_pw * s_ph)),
               (unsigned)(now_ms() - s_t0 - s_ms_fine));
        return;
    }

    const int ya = s_cur;
    int       yb = s_cur + 1;
    if (yb >= s_ph) {
        yb = -1;
    }
    s_cur += 2;

    bool ca, cb = false;
    if (s_w_ok && yb >= 0) {
        w_submit(WK_REFINE, yb);
        ca = row_refine(ya, &s_ss_a);
        cb = w_wait() ? (s_w_res != 0) : row_refine(yb, &s_ss_a);
    } else {
        ca = row_refine(ya, &s_ss_a);
        if (yb >= 0) {
            cb = row_refine(yb, &s_ss_a);
        }
    }

    /* A row is pushed only if something in it actually moved: on an ordinary
       frame that is a fifth of them, and the flush is what a repaint costs. */
    if (ca) {
        blit_band(ya, 1);
    }
    if (cb) {
        blit_band(yb, 1);
    }
}

static void prog_paint(void);

/*
 * Both cores off the CPU for one tick, a few times a second.
 *
 * The idle task is what the task watchdog watches, and a render is a minute
 * of arithmetic at a higher priority than it. Ten milliseconds in every
 * hundred and fifty is under seven per cent of the render and enough for
 * everything else on the tablet to keep running.
 */
static void maybe_yield(void)
{
    const uint32_t t = now_ms();
    if (t - s_yield_t < 150u) {
        return;
    }
    s_yield_t = t;

    prog_paint();
    ngl_flush();

    s_w_pause = 1;
    neos_sleep_ms(10);
    s_w_pause = 0;
}

static void render_step(void)
{
    switch (s_rp) {
    case RP_COARSE: step_coarse(); break;
    case RP_MAP:    step_map();    break;
    case RP_ROWS:   step_rows();   break;
    case RP_REFINE: step_refine(); break;
    default: return;
    }
    maybe_yield();
}

/* ------------------------------------------------------------------ */
/* The controls                                                        */
/* ------------------------------------------------------------------ */

#define UI_H       52
#define UI_MARGIN  14
#define UI_GAP     12
#define UI_PAD     15
#define UI_RADIUS  10
#define INFO_CHARS 30

/*
 * BTN_OUT sits in the bottom left corner on its own, where a thumb is and
 * where the way back belongs. The rest are a cluster at the other end, laid
 * out right to left in reverse of this order.
 */
enum { BTN_OUT = 0, BTN_SAVE, BTN_PAL, BTN_PLACE, BTN_MODE, BTN_N };

static const char *const BTN_TXT[BTN_N] = {
    "ZOOM OUT", "SAVE", "PALETTE", "PLACE", "MODE"
};

static ngl_rect_t s_btn[BTN_N];
static ngl_rect_t s_info;
static int8_t     s_hot = -1;

static int16_t chip_w(const char *txt)
{
    return (int16_t)(ngl_text_width(&ngl_font_small, txt) + 2 * UI_PAD);
}

static void ui_layout(void)
{
    const int16_t y = (int16_t)(s_area.y + s_ph - UI_MARGIN - UI_H);

    /* The way back out lives in the bottom left corner, where a thumb is. */
    s_btn[BTN_OUT] = ngl_rect((int16_t)(s_area.x + UI_MARGIN), y,
                              chip_w(BTN_TXT[BTN_OUT]), UI_H);

    int16_t x = (int16_t)(s_area.x + s_pw - UI_MARGIN);
    for (int i = BTN_N - 1; i > BTN_OUT; i--) {
        const int16_t w = chip_w(BTN_TXT[i]);
        x = (int16_t)(x - w);
        s_btn[i] = ngl_rect(x, y, w, UI_H);
        x = (int16_t)(x - UI_GAP);
    }

    const int16_t ix = (int16_t)(s_btn[BTN_OUT].x + s_btn[BTN_OUT].w + UI_GAP);
    int16_t iw = (int16_t)(INFO_CHARS * ngl_font_small.width + 2 * UI_PAD);
    const int16_t room = (int16_t)(s_btn[BTN_OUT + 1].x - UI_GAP - ix);
    if (iw > room) {
        iw = room;
    }
    s_info = ngl_rect(ix, y, iw > 60 ? iw : 60, UI_H);

    s_ui_top = (int16_t)(s_ph - UI_MARGIN - UI_H);
}

static void chip(ngl_rect_t r, const char *txt, bool hot)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    ngl_fill_round_rect(sc, r, UI_RADIUS, hot ? TH_CLOSE_FILL : TH_PANEL);
    ngl_draw_round_rect(sc, r, UI_RADIUS, hot ? TH_ACCENT : TH_EDGE, 2);

    const int16_t tw = ngl_text_width(&ngl_font_small, txt);
    ngl_text(sc, (int16_t)(r.x + (r.w - tw) / 2),
             (int16_t)(r.y + (r.h - ngl_font_small.height) / 2),
             txt, &ngl_font_small, hot ? TH_GLOW : TH_TEXT);
}

/* 1.6 / hw: how far in this view is, against the one the whole thing fits in. */
void zoom_text(char *buf, size_t n, uint8_t mode, float hw)
{
    const float home = fr_home_hw(mode);
    const uint32_t z = (uint32_t)(home / (hw > 0.0f ? hw : home) * 100.0f + 0.5f);

    if (z < 100000u) {
        snprintf(buf, n, "x%u.%u", (unsigned)(z / 100u), (unsigned)((z / 10u) % 10u));
    } else {
        snprintf(buf, n, "x%u", (unsigned)(z / 100u));
    }
}

static void status_str(char *buf, size_t n)
{
    /* s_shown, not g_scene: while the next picture is being drawn ahead of
       time g_scene is already that one, and a readout describing a view
       nobody can see yet is just wrong. */
    const scene_t *sc = &s_shown;
    char z[16];
    zoom_text(z, sizeof z, sc->mode, sc->hw);

    switch (sc->mode) {
    case MODE_LYAP: {
        char w[SEQ_MAX + 1];
        int len = sc->seq_len > SEQ_MAX ? SEQ_MAX : sc->seq_len;
        for (int i = 0; i < len; i++) {
            w[i] = (char)('a' + (sc->seq[i] & 1));
        }
        w[len] = 0;
        snprintf(buf, n, "lyapunov %s  %s", w, z);
        break;
    }
    default:
        snprintf(buf, n, "%s  %s  %u it", mode_name(sc->mode), z,
                 (unsigned)sc->maxiter);
        break;
    }
}

static void info_paint(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    char txt[64];
    status_str(txt, sizeof txt);

    /* The plate is a fixed size, so cut the text rather than let it run out
       over the picture. */
    const int fit = (s_info.w - 2 * UI_PAD) / ngl_font_small.width;
    if (fit > 0 && (int)strlen(txt) > fit) {
        txt[fit] = 0;
    }

    ngl_fill_round_rect(sc, s_info, UI_RADIUS, TH_PANEL);
    ngl_draw_round_rect(sc, s_info, UI_RADIUS, TH_RULE, 2);
    ngl_text(sc, (int16_t)(s_info.x + UI_PAD),
             (int16_t)(s_info.y + (s_info.h - ngl_font_small.height) / 2),
             txt, &ngl_font_small, TH_TEXT_DIM);
}

/*
 * A line along the very bottom edge while a render is running, and the
 * picture put back behind it when there is not. Three seconds of arithmetic
 * with nothing moving reads as a hang; three seconds with a line crossing the
 * screen reads as work.
 */
static void prog_paint(void)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    const int16_t y = (int16_t)(s_area.y + s_ph - PROG_H);

    if (s_rp == RP_DONE) {
        /* Mid-dissolve there is no buffer this strip can be put back from -
           the panel is showing a blend of two - so it is left as the fade
           drew it, which is the only thing there that is not stale. */
        if (s_at != AT_FADE) {
            restore(ngl_rect(s_area.x, y, (int16_t)s_pw, PROG_H));
        }
        return;
    }
    int16_t w = (int16_t)((float)s_pw * progress());
    if (w < 0)     { w = 0; }
    if (w > s_pw)  { w = (int16_t)s_pw; }

    if (w > 0) {
        ngl_fill_rect(sc, ngl_rect(s_area.x, y, w, PROG_H), TH_ACCENT);
    }
    if (w < s_pw) {
        restore(ngl_rect((int16_t)(s_area.x + w), y, (int16_t)(s_pw - w), PROG_H));
    }
}

static void ui_paint(void)
{
    for (int i = 0; i < BTN_N; i++) {
        chip(s_btn[i], BTN_TXT[i], s_hot == i);
    }
    info_paint();
    prog_paint();
}

/** Put back whatever an overlay covered, from the kept picture. */
static void restore(ngl_rect_t r)
{
    ngl_surface_t *sc  = ngl_screen();
    ngl_surface_t *vis = s_buf[s_vis].pics;
    ngl_rect_t out;
    /* The visible buffer, not the target: what an overlay covered is what is
       on the panel, which during a background render is the previous picture. */
    if (!sc || !vis || !ngl_rect_intersect(&r, &s_area, &out)) {
        return;
    }
    const ngl_rect_t src = ngl_rect(out.x, (int16_t)(out.y - s_area.y), out.w, out.h);
    ngl_blit(sc, out.x, out.y, vis, &src);
}

/* ------------------------------------------------------------------ */
/* The rubber band                                                     */
/* ------------------------------------------------------------------ */

#define BAND_T 2

static ngl_rect_t s_band;               /* w == 0: nothing drawn */

/* The four strips the band occupies: its own outline plus the dark one just
   outside it, which is what keeps it readable over neon as well as over black. */
static void band_edges(ngl_rect_t r, ngl_rect_t out[4])
{
    const int16_t t = BAND_T * 2;
    const ngl_rect_t e = ngl_rect((int16_t)(r.x - BAND_T), (int16_t)(r.y - BAND_T),
                                  (int16_t)(r.w + 2 * BAND_T), (int16_t)(r.h + 2 * BAND_T));
    out[0] = ngl_rect(e.x, e.y, e.w, t);
    out[1] = ngl_rect(e.x, (int16_t)(e.y + e.h - t), e.w, t);
    out[2] = ngl_rect(e.x, e.y, t, e.h);
    out[3] = ngl_rect((int16_t)(e.x + e.w - t), e.y, t, e.h);
}

static void band_clear(void)
{
    if (!s_band.w) {
        return;
    }
    ngl_rect_t e[4];
    band_edges(s_band, e);
    bool touched_ui = false;
    for (int i = 0; i < 4; i++) {
        restore(e[i]);
        if (e[i].y + e[i].h > s_area.y + s_ui_top) {
            touched_ui = true;
        }
    }
    if (touched_ui) {
        ui_paint();
    }
    s_band = ngl_rect(0, 0, 0, 0);
}

static void band_show(ngl_rect_t r)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc) {
        return;
    }
    /* A finger resting still is the common case at 60 polls a second, and
       redrawing an unchanged band would repaint the controls under it. */
    if (r.x == s_band.x && r.y == s_band.y && r.w == s_band.w && r.h == s_band.h) {
        return;
    }
    band_clear();
    ngl_draw_rect(sc, ngl_rect((int16_t)(r.x - BAND_T), (int16_t)(r.y - BAND_T),
                               (int16_t)(r.w + 2 * BAND_T), (int16_t)(r.h + 2 * BAND_T)),
                  TH_BG, BAND_T);
    ngl_draw_rect(sc, r, TH_GLOW, BAND_T);
    s_band = r;
    ngl_flush();
}

/*
 * The frame the drag describes, corrected to the shape of the screen.
 *
 * A zoom that took the rectangle literally would stretch the plane, and a
 * stretched Mandelbrot is not a Mandelbrot. So whichever side is short is
 * grown to match the panel's aspect, about the middle of the drag, and the
 * result is kept on screen - the band the finger sees is exactly the view it
 * is going to get, which is the only way a drag-to-zoom is honest.
 */
static ngl_rect_t band_rect(int x0, int y0, int x1, int y1)
{
    int lx = x0 < x1 ? x0 : x1, rx = x0 < x1 ? x1 : x0;
    int ty = y0 < y1 ? y0 : y1, by = y0 < y1 ? y1 : y0;
    int w = rx - lx, h = by - ty;

    if (w * s_ph < h * s_pw) { w = (h * s_pw) / s_ph; }
    else                     { h = (w * s_ph) / s_pw; }

    if (w > s_pw) { w = s_pw; h = s_ph; }
    if (h > s_ph) { h = s_ph; w = s_pw; }

    const int cx = (lx + rx) / 2, cy = (ty + by) / 2;
    lx = cx - w / 2;
    ty = cy - h / 2;
    if (lx < s_area.x) { lx = s_area.x; }
    if (ty < s_area.y) { ty = s_area.y; }
    if (lx + w > s_area.x + s_pw) { lx = s_area.x + s_pw - w; }
    if (ty + h > s_area.y + s_ph) { ty = s_area.y + s_ph - h; }

    return ngl_rect((int16_t)lx, (int16_t)ty, (int16_t)w, (int16_t)h);
}

/* ------------------------------------------------------------------ */
/* Gestures                                                            */
/* ------------------------------------------------------------------ */

/*
 * NeOS delivers taps, but not drags, and a viewer needs both from the same
 * press - so all of it is worked out here from the raw finger position and
 * the OS's taps are drained and thrown away. The one thing not decided here
 * is the close button: that press never reaches an app at all.
 */

#define SLOP     18     /* a finger rolls this far on a press meant as a tap */
#define BAND_MIN 40     /* a frame smaller than this was a tap that wandered */
#define TAP_ZOOM 3.0f
/*
 * A press held this long is a different press.
 *
 * Long enough that nobody reaches it by being slow with a tap, short enough
 * that it fires while the finger is still down - which is the point: the
 * action happens under the finger, so the gesture explains itself, and the
 * release afterwards is thrown away rather than doing the short press too.
 */
#define LONG_MS  650

enum { ACT_NONE = 0, ACT_BUSY, ACT_ZOOM_RECT, ACT_ZOOM_PT, ACT_BTN, ACT_BTN_LONG };
enum { G_IDLE = 0, G_PRESS, G_DRAG, G_BTN, G_DEAD };

static int8_t     s_g;
static bool       s_down_prev;
static int16_t    s_gx0, s_gy0;
static uint32_t   s_gt0;            /* when the press went down */
static ngl_rect_t s_act_rect;
static int16_t    s_act_x, s_act_y;
static int        s_act_btn;

static void attract_stop(void);

/* Which buttons mean something different held down. Only one, so far. */
static bool btn_has_long(int b) { return b == BTN_PLACE; }

static int btn_hit(int16_t x, int16_t y)
{
    for (int i = 0; i < BTN_N; i++) {
        if (ngl_rect_contains(&s_btn[i], x, y)) {
            return i;
        }
    }
    return -1;
}

static void btn_light(int i, bool on)
{
    s_hot = on ? (int8_t)i : -1;
    chip(s_btn[i], BTN_TXT[i], on);
    ngl_flush();
}

static int gesture_poll(void)
{
    neos_touch_t t;
    int16_t junk_x = 0, junk_y = 0;

    /* NeOS's own tap detector is still running; collected and discarded so a
       stale one cannot arrive minutes later. */
    while (neos_touch_tap(&junk_x, &junk_y)) { }

    if (!neos_touch(&t)) {
        return ACT_NONE;                /* no touch panel: the app still draws */
    }

    /*
     * The touch that ends the attract loop does nothing else. Waking a
     * screensaver by accidentally zooming into a corner of it is the one
     * thing worse than not being able to wake it at all, so the whole
     * gesture - press, drag and release - is swallowed.
     */
    if (s_at != AT_OFF) {
        if (t.down || s_down_prev) {
            attract_stop();
            s_g = G_DEAD;
            s_down_prev = t.down;
            return t.down ? ACT_BUSY : ACT_NONE;
        }
        return ACT_NONE;
    }

    int act = ACT_NONE;

    if (t.down && !s_down_prev) {
        s_gx0 = t.x;
        s_gy0 = t.y;
        s_gt0 = now_ms();
        if (!ngl_rect_contains(&s_area, t.x, t.y)) {
            s_g = G_DEAD;               /* the system bar is not ours */
        } else {
            const int b = btn_hit(t.x, t.y);
            if (b >= 0) {
                s_act_btn = b;
                s_g = G_BTN;
                btn_light(b, true);
            } else {
                s_g = G_PRESS;
            }
        }
    } else if (t.down) {
        const int moved = iabs_(t.x - s_gx0) > SLOP || iabs_(t.y - s_gy0) > SLOP;
        if (s_g == G_BTN && moved) {
            btn_light(s_act_btn, false);
            s_g = G_DEAD;               /* slid off the button: not a press */
        } else if (s_g == G_BTN && btn_has_long(s_act_btn) &&
                   now_ms() - s_gt0 >= LONG_MS) {
            btn_light(s_act_btn, false);
            s_g = G_DEAD;               /* fired; the release is not a tap */
            act = ACT_BTN_LONG;
        } else if (s_g == G_PRESS && moved) {
            s_g = G_DRAG;
        }
        if (s_g == G_DRAG) {
            band_show(band_rect(s_gx0, s_gy0, t.x, t.y));
        }
    } else if (s_down_prev) {
        switch (s_g) {
        case G_BTN:
            btn_light(s_act_btn, false);
            act = ACT_BTN;
            break;
        case G_DRAG: {
            const ngl_rect_t r = s_band;
            band_clear();
            ngl_flush();
            if (r.w >= BAND_MIN) {
                s_act_rect = r;
                act = ACT_ZOOM_RECT;
            }
            break;
        }
        case G_PRESS:
            s_act_x = s_gx0;
            s_act_y = s_gy0;
            act = ACT_ZOOM_PT;
            break;
        default:
            break;
        }
        s_g = G_IDLE;
    }

    s_down_prev = t.down;
    if (act == ACT_NONE && t.down && s_g != G_DEAD) {
        return ACT_BUSY;                /* hold off rendering under the finger */
    }
    return act;
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

static void restart(void)
{
    /*
     * A scene arrives here from a drag, a tap, the history or the search, and
     * three of those four are arithmetic on numbers a finger chose. If any of
     * it has produced something the kernel cannot draw, this is the last
     * moment anyone can tell - past here it is a screen of noise with no way
     * back, since ZOOM OUT would restore a history built on the same mistake.
     */
    if (!fr_valid(&g_scene)) {
        printf("[mandel] scene came out unusable, going home\n");
        fr_home(g_scene.mode < MODE_COUNT ? g_scene.mode : MODE_MANDEL, &g_scene);
        s_nhist = 0;
    }

    fr_scene(&g_scene);
    fr_view(s_pw, s_ph);

    s_rp    = RP_COARSE;
    s_sub   = 0;
    s_cur   = 0;
    s_t0    = now_ms();
    s_yield_t = s_t0;
    s_ss_a  = 0;
    s_ss_b  = 0;
    s_ms_fine = 0;
    s_pend_rows = 0;
    s_pend_ui   = false;

    /* Unless this render is going into the buffer the panel is not showing,
       the readout is about this view from now on. */
    if (s_tgt == s_vis) {
        s_shown = g_scene;
    }
    ui_paint();
    ngl_flush();
}

static void hist_push(void)
{
    if (s_nhist == HIST_MAX) {
        for (int i = 1; i < HIST_MAX; i++) {
            s_hist[i - 1] = s_hist[i];
        }
        s_nhist--;
    }
    s_hist[s_nhist++] = g_scene;
}

static void zoom_to(float cx, float cy, float hw)
{
    if (hw < HW_MIN) {
        hw = HW_MIN;
        neos_status_for("as deep as single precision goes", 2200);
    }
    hist_push();
    g_scene.cx      = cx;
    g_scene.cy      = cy;
    g_scene.hw      = hw;
    g_scene.maxiter = fr_iters_for(g_scene.mode, hw);
    restart();
}

static void zoom_rect(ngl_rect_t r)
{
    float wx, wy;
    fr_world(r.x - s_area.x + r.w / 2, r.y - s_area.y + r.h / 2, &wx, &wy);
    zoom_to(wx, wy, (float)r.w * fr_pitch() * 0.5f);
}

static void zoom_point(int16_t x, int16_t y)
{
    float wx, wy;
    fr_world(x - s_area.x, y - s_area.y, &wx, &wy);
    zoom_to(wx, wy, g_scene.hw / TAP_ZOOM);
}

/*
 * Out is the way in, backwards. The history is the point - a rectangle drag
 * is not invertible by any amount of arithmetic, and a viewer that zoomed out
 * to somewhere you had never been would be worse than one that could not zoom
 * out at all. Only when there is nothing left to undo does this fall back to
 * widening the view, and then it stops at the whole set.
 */
static void zoom_out(void)
{
    if (s_nhist > 0) {
        g_scene = s_hist[--s_nhist];
        restart();
        return;
    }

    float hcx, hcy, hhw;
    fr_home_view(g_scene.mode, &hcx, &hcy, &hhw);

    if (g_scene.hw >= hhw) {
        neos_status_for("this is the whole of it", 1600);
        return;
    }
    float hw = g_scene.hw * 3.0f;
    if (hw >= hhw) {
        hw = hhw;
        g_scene.cx = hcx;
        g_scene.cy = hcy;
    }
    g_scene.hw      = hw;
    g_scene.maxiter = fr_iters_for(g_scene.mode, hw);
    restart();
}

/*
 * All the way out, in one press, without walking back through the history.
 *
 * PLACE held down rather than tapped, because the two are the same question
 * asked at two scales - "put me somewhere in this fractal" and "put me back
 * where the whole of it is" - and a sixth chip along the bottom to say so
 * would be a sixth chip nobody looks at. The fractal itself is kept: a Julia
 * set framed whole is still that Julia set, which is what fr_home_view is for
 * and what fr_home would have thrown away.
 *
 * It pushes history like any other move, so ZOOM OUT still comes back to the
 * view this was pressed from.
 */
static void zoom_home(void)
{
    float cx, cy, hw;
    fr_home_view(g_scene.mode, &cx, &cy, &hw);

    if (g_scene.hw >= hw) {
        neos_status_for("this is the whole of it", 1600);
        return;
    }
    hist_push();
    g_scene.cx      = cx;
    g_scene.cy      = cy;
    g_scene.hw      = hw;
    g_scene.maxiter = fr_iters_for(g_scene.mode, hw);
    restart();
    neos_status_for("all the way out", 1600);
}

/*
 * A new palette over the same picture, which is a lookup rather than a
 * render - as long as there is a picture. Mid-render the indices below the
 * wavefront are for the old view, so the honest answer is to draw it again.
 */
static void new_palette(void)
{
    pal_new(0);
    s_pal_no++;

    char msg[40];
    snprintf(msg, sizeof msg, "palette %u", (unsigned)s_pal_no);
    neos_status_for(msg, 1400);

    if (s_rp != RP_DONE || !s_idx) {
        restart();
        return;
    }
    for (int y = 0; y < s_ph; y++) {
        ngl_color_t   *pr = pic_row(y);
        const uint8_t *ir = idx_row(y);
        for (int x = 0; x < s_pw; x++) {
            pr[x] = pal_px(ir[x], x, y);
        }
    }
    blit_band(0, s_ph);
    present();

    /* The refinement was colour and did not survive the lookup. */
    s_rp   = RP_REFINE;
    s_cur  = 0;
    s_ss_a = 0;
    s_ss_b = 0;
    s_t0   = now_ms();
    s_ms_fine = 0;
    s_yield_t = s_t0;
}

static void new_place(uint8_t mode)
{
    neos_status("looking for somewhere worth pointing this at");
    s_nhist = 0;
    fr_find(mode, &g_scene);
    neos_status_clear();
    restart();
}

/*
 * The next fractal, framed whole.
 *
 * This used to hand the new mode to the search, which made it indis-
 * tinguishable from PLACE in use: both buttons thought for most of a second
 * and then threw you at a random deep view, and the only thing saying the
 * fractal had changed at all was one word in the readout. Going to the mode's
 * home view instead makes the two buttons answer different questions - MODE is
 * "show me the Julia set", PLACE is "surprise me inside whatever I am looking
 * at" - and it lands somewhere you can actually steer from, which is the whole
 * point of the drag.
 */
static void next_mode(void)
{
    const uint8_t m = (uint8_t)((g_scene.mode + 1) % MODE_COUNT);

    s_nhist = 0;
    fr_home(m, &g_scene);
    restart();
    neos_status_for(mode_name(m), 1800);
}

/* ------------------------------------------------------------------ */
/* The attract loop                                                    */
/* ------------------------------------------------------------------ */

/*
 * What the program does when nobody is steering it.
 *
 * It opens on somewhere the search picked rather than on the whole set,
 * because the whole set is the one view of the Mandelbrot everybody has
 * already seen, and a viewer that starts by showing you something you have
 * not is a better argument for itself. Then, having nothing else to do with
 * the two cores, it draws the next one while you are still looking at this
 * one - the line along the bottom edge is that render, exactly as it is
 * during a zoom - and dissolves across when it is ready and the current
 * picture has had its dwell.
 *
 * The whole of it ends on the first touch, and ends by handing over what is
 * on the panel: mid-dissolve it snaps to the picture that was arriving,
 * mid-render it throws the half-drawn one away and keeps the one you were
 * looking at. Either way the scene the buttons act on is the scene on the
 * glass, which is the only version of this that is not infuriating.
 */

#define AT_DWELL_MS  9000   /* how long a finished picture is left up */
/*
 * A second, and the clock decides how many frames that is.
 *
 * A frame of the dissolve is the whole picture blended into scratch, blitted
 * to the back buffer and flushed through the PPA: measured on the tablet, 175
 * + 40 + 55 ms, so about four of them fit. Counting frames instead and
 * spacing the blend over eight of them is what the first version of this did,
 * and it ran out of second halfway across - four even steps and then a jump
 * from the midpoint to the new picture, which is worse than no dissolve at
 * all because the jump is the thing you notice.
 *
 * So the position comes from the clock, and each frame is drawn for where the
 * dissolve will be when that frame actually reaches the panel - one frame's
 * measured cost ahead of now. Slower hardware draws fewer, larger steps in
 * the same second; faster hardware draws more, smaller ones. Neither jumps.
 */
#define FADE_MS      1000
#define FADE_BAND      32   /* rows blended at a time; see s_fade */

static uint32_t s_at_t;         /* when the picture on the panel finished */
static uint32_t s_fade_t0;      /* when the dissolve started */
static uint32_t s_fade_cost;    /* what the last frame of it took */
static int      s_fade_step;    /* frames drawn, for the log */

/*
 * A band of scratch to blend into, rather than blending in place.
 *
 * In place would need no memory at all and would not be a cross-fade: each
 * step would have to work from the result of the last, and in RGB565 the
 * small differences stop moving - a channel two steps apart never gets past
 * the first rounding, so the picture jumps at the end instead of arriving.
 * Blending both originals into somewhere else keeps every step exact, and a
 * band is enough because the panel is written a band at a time anyway.
 */
static ngl_color_t   *s_fade;
static ngl_surface_t *s_fades;
static void          *s_fade_raw;

/*
 * One band of the blend, from the two pictures into the scratch.
 *
 * m is 0..256, and 256 is exactly the new picture - which is what lets the
 * swap that follows the last frame be a change of bookkeeping rather than of
 * pixels. The arithmetic never leaves the range either endpoint is already
 * in, so there is nothing to clamp.
 */
static void fade_band(int y0, int n, int m)
{
    const ngl_color_t *a = s_buf[s_vis].pic + (size_t)y0 * s_pw;
    const ngl_color_t *b = s_buf[s_tgt].pic + (size_t)y0 * s_pw;
    ngl_color_t       *d = s_fade;
    const size_t       np = (size_t)n * s_pw;

    for (size_t i = 0; i < np; i++) {
        const ngl_color_t ca = a[i], cb = b[i];
        if (ca == cb) {
            d[i] = ca;
            continue;
        }
        const int r0 = (ca >> 11) & 31, g0 = (ca >> 5) & 63, b0 = ca & 31;
        const int r = r0 + ((((int)((cb >> 11) & 31) - r0) * m) >> 8);
        const int g = g0 + ((((int)((cb >>  5) & 63) - g0) * m) >> 8);
        const int bl = b0 + ((((int)( cb        & 31) - b0) * m) >> 8);
        d[i] = (ngl_color_t)((r << 11) | (g << 5) | bl);
    }
}

/* One frame of the dissolve: the whole picture, a band at a time. */
static void fade_paint(int m)
{
    ngl_surface_t *sc = ngl_screen();
    if (!sc || !s_fades) {
        return;
    }
    for (int y0 = 0; y0 < s_ph; y0 += FADE_BAND) {
        const int n = s_ph - y0 < FADE_BAND ? s_ph - y0 : FADE_BAND;
        fade_band(y0, n, m);
        const ngl_rect_t src = ngl_rect(0, 0, (int16_t)s_pw, (int16_t)n);
        ngl_blit(sc, s_area.x, (int16_t)(s_area.y + y0), s_fades, &src);
    }

    ui_paint();         /* the controls sit over the picture, so they go back */
    ngl_flush();

    /* A frame is a tenth of a second with no call that blocks in it, and
       there are eight of them back to back. One tick off the CPU between
       them is what keeps the idle task - and the watchdog watching it -
       alive; see maybe_yield, which pays the same toll during a render. */
    neos_sleep_ms(10);
}

/* The new picture is the picture: from here on it is the one being steered. */
static void attract_swap(void)
{
    s_vis   = s_tgt;
    s_shown = g_scene;
    blit_band(0, s_ph);
    present();
    printf("[mandel] dissolved in %u ms over %d frames\n",
           (unsigned)(now_ms() - s_fade_t0), s_fade_step);
    worker_unpark();            /* the dissolve is over; it can have the other core back */
    s_at_t = now_ms();
    s_at   = AT_SHOW;
}

/* Start drawing the next one into the buffer the panel is not showing. */
static void attract_next(void)
{
    s_at_t = now_ms();
    s_at   = AT_MAKE;

    pal_new(0);                 /* a new green as well as a new place */
    target_set(1 - s_vis);
    s_nhist = 0;
    fr_find((uint8_t)rnd_range(0, MODE_COUNT - 1), &g_scene);
    restart();
}

static void attract_step(void)
{
    switch (s_at) {
    case AT_SHOW:
        attract_next();
        break;
    case AT_MAKE:
        /* Only reached with the render finished, so all that is left is the
           dwell - which is measured from when this picture went up, so the
           time spent drawing the next one is time already served. */
        if (now_ms() - s_at_t < AT_DWELL_MS) {
            neos_sleep_ms(20);
            break;
        }
        worker_park();          /* nothing else touching either picture */
        s_at        = AT_FADE;
        s_fade_step = 0;
        s_fade_cost = 0;
        s_fade_t0   = now_ms();
        break;
    case AT_FADE: {
        /*
         * Where the dissolve should be by the time this frame is on the
         * panel, which is one frame's worth after it is started. The first
         * one has nothing measured yet and guesses a quarter of the second -
         * being wrong about that costs one step's spacing, not the shape of
         * the whole thing.
         */
        const uint32_t dt = now_ms() - s_fade_t0;
        const uint32_t at = dt + (s_fade_cost ? s_fade_cost : FADE_MS / 4);

        /* m == 256 is the new picture and nothing else, so the last frame is
           not a blend at all - it is the swap, which blits that buffer whole. */
        if (at >= FADE_MS) {
            attract_swap();
            break;
        }
        const uint32_t t = now_ms();
        fade_paint((int)((256u * at) / FADE_MS));
        s_fade_cost = now_ms() - t;
        s_fade_step++;
        break;
    }
    default:
        break;
    }
}

static void attract_stop(void)
{
    if (s_at == AT_OFF) {
        return;
    }
    if (s_at == AT_FADE) {
        attract_swap();                 /* land on the one that was arriving */
    } else if (s_tgt != s_vis) {
        /* Throw away the half-drawn one and put the raster back where the
           picture on the panel needs it - a tap is about to ask fr_world
           where a finger landed. */
        target_set(s_vis);
        g_scene = s_shown;
        fr_scene(&g_scene);
        fr_view(s_pw, s_ph);
        s_rp = RP_DONE;
        ui_paint();
        ngl_flush();
    }
    s_at    = AT_OFF;
    s_nhist = 0;
    worker_unpark();            /* whichever way out that was */
    neos_status_for("drag a frame to zoom, tap to dive in", 3000);
    printf("[mandel] attract loop off, you have it\n");
}

static void attract_begin(void)
{
    s_at   = AT_SHOW;
    s_at_t = now_ms();
    fr_find(MODE_MANDEL, &g_scene);
    restart();
}

/*
 * The picture, on the card.
 *
 * Only once the fine pass has finished. Before that the index buffer and the
 * panel disagree - the interlace has replicated rows into the picture that
 * the indices have not caught up with yet - and a file that is not what was
 * on screen when the button was pressed is worse than a moment's wait.
 */
static void save_now(void)
{
    if (!s_idx) {
        neos_status_for("no index buffer - nothing to save", 2000);
        return;
    }
    if (s_rp != RP_DONE && s_rp != RP_REFINE) {
        neos_status_for("still drawing - try again in a moment", 2000);
        return;
    }

    neos_status("saving...");
    char name[80];
    const int rc = save_png(&g_scene, s_idx, s_pw, s_ph, name, sizeof name);

    char msg[112];
    if (rc == 0) {
        snprintf(msg, sizeof msg, "saved %s", name);
    } else {
        snprintf(msg, sizeof msg, "save failed (%d)", rc);
    }
    neos_status_for(msg, 5000);
}

/* ------------------------------------------------------------------ */
/* Orientation                                                         */
/* ------------------------------------------------------------------ */

static ngl_rotation_t s_rot = NGL_ROT_90;
static uint32_t       s_orient_t;

static void orient_pin(ngl_rotation_t r)
{
    s_rot = r;
    neos_orient_lock(r);        /* rotates the panel and keeps the OS in step */
    ngl_bar_paint();
    ngl_flush();
}

/*
 * Only the in-plane x axis has a say, and only when it is unambiguous.
 *
 * Held portrait or laid flat, x is small and there is no landscape answer to
 * give - so this keeps the one it had, which is exactly what "stays
 * horizontal, may flip over" means when the tablet is somewhere in between.
 * The 0.5 g threshold is about a 30 degree tilt: past it the tablet is
 * definitely one way up.
 */
static void orient_poll(void)
{
    const uint32_t t = now_ms();
    if (t - s_orient_t < 400u) {
        return;
    }
    s_orient_t = t;

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (neos_orient_read(&ax, &ay, &az) != ESP_OK || fabs_(ax) < 0.5f) {
        return;
    }
    const ngl_rotation_t want = ax > 0.0f ? NGL_ROT_270 : NGL_ROT_90;
    if (want != s_rot) {
        printf("[mandel] flipped to %s\n", neos_orient_name(want));
        /* Both landscape rotations use the same back buffer, so the picture
           does not have to be drawn again - only mapped the other way up. */
        orient_pin(want);
    }
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

/*
 * 64-byte aligned, and that alignment is load-bearing: the two threads write
 * different rows of these buffers at the same time, and a row that started
 * mid-cache-line would have them writing the same line from two cores.
 */
static void *alloc64(size_t n, void **raw)
{
    void *p = malloc(n + 64);
    *raw = p;
    if (!p) {
        return NULL;
    }
    return (void *)(((uintptr_t)p + 63u) & ~(uintptr_t)63u);
}

/* One picture buffer and its index plane. Only the picture is required. */
static bool pbuf_alloc(pbuf_t *b)
{
    b->pic = alloc64((size_t)s_pw * s_ph * sizeof(ngl_color_t), &b->pic_raw);
    if (!b->pic) {
        return false;
    }
    /* Black, not whatever the heap had: the controls and the progress line
       are put back by copying out of here, and they go up before the first
       pass has written a single pixel into it. */
    memset(b->pic, 0, (size_t)s_pw * s_ph * sizeof(ngl_color_t));

    b->pics = ngl_surface_wrap(b->pic, (int16_t)s_pw, (int16_t)s_ph, (int16_t)s_pw);
    if (!b->pics) {
        return false;
    }
    b->idx = alloc64((size_t)s_pw * s_ph, &b->idx_raw);
    return true;
}

static void pbuf_free(pbuf_t *b)
{
    if (b->pics) {
        ngl_surface_free(b->pics);
    }
    free(b->pic_raw);
    free(b->idx_raw);
    memset(b, 0, sizeof *b);
}

static bool layout(void)
{
    s_area = ngl_app_area();
    s_pw   = s_area.w;
    s_ph   = s_area.h;
    if (s_pw < 64 || s_ph < 64) {
        return false;
    }
    s_cgw = (s_pw + CO - 1) / CO;
    s_cgh = (s_ph + CO - 1) / CO;

    if (!pbuf_alloc(&s_buf[0])) {
        printf("[mandel] no room for a %dx%d picture\n", s_pw, s_ph);
        return false;
    }
    s_vis = 0;
    target_set(0);

    /*
     * The second picture, and the band the dissolve blends into. Both exist
     * for the attract loop and nothing else, so failing to get them costs
     * that and nothing else - which is the right trade on a machine where
     * the first buffer is already most of two megabytes.
     */
    if (pbuf_alloc(&s_buf[1]) && s_buf[1].idx) {
        s_fade = alloc64((size_t)FADE_BAND * s_pw * sizeof(ngl_color_t), &s_fade_raw);
        if (s_fade) {
            s_fades = ngl_surface_wrap(s_fade, (int16_t)s_pw, FADE_BAND, (int16_t)s_pw);
        }
    }
    if (!s_fades) {
        pbuf_free(&s_buf[1]);
        free(s_fade_raw);
        s_fade     = NULL;
        s_fade_raw = NULL;
        printf("[mandel] no second picture: no attract loop\n");
    }

    /* Both of these are optional. Without the index buffer there is no
       refinement and no instant recolour; without the coarse values the
       palette is fitted to the mode's whole range instead of this frame's. */
    s_cv = malloc((size_t)s_cgw * s_cgh * sizeof(float));
    if (!s_idx) {
        printf("[mandel] no index buffer: no supersampling\n");
    }

    ui_layout();
    printf("[mandel] %dx%d picture, coarse %dx%d, %u KB psram left\n",
           s_pw, s_ph, s_cgw, s_cgh, (unsigned)(neos_psram_free() / 1024u));
    return true;
}

static void teardown(void)
{
    worker_stop();          /* first: it is still writing into these buffers */
    if (s_fades) { ngl_surface_free(s_fades); s_fades = NULL; }
    free(s_fade_raw);
    s_fade     = NULL;
    s_fade_raw = NULL;
    pbuf_free(&s_buf[0]);
    pbuf_free(&s_buf[1]);
    free(s_cv);
    s_cv   = NULL;
    s_pic  = NULL;
    s_pics = NULL;
    s_idx  = NULL;
    neos_orient_unlock();
}

static void seed_rng(void)
{
    /* Uptime alone repeats when the app is opened at the same moment twice;
       a stack address moves with whatever the loader placed before us. */
    uint32_t s = now_ms() * 2654435761u;
    s ^= (uint32_t)(uintptr_t)&s;
    rnd_seed(s);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    printf("[mandel] starting as \"%s\"\n", argc > 0 ? argv[0] : "?");

    if (!ngl_screen()) {
        printf("[mandel] no screen, nothing to draw\n");
        return 0;
    }

    /* Landscape first: the app area is a different shape afterwards, and
       everything below is sized from it. */
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    ngl_rotation_t r = NGL_ROT_90;
    if (neos_orient_read(&ax, &ay, &az) == ESP_OK && fabs_(ax) >= 0.35f) {
        r = ax > 0.0f ? NGL_ROT_270 : NGL_ROT_90;
    }
    orient_pin(r);

    seed_rng();
    if (!layout()) {
        neos_orient_unlock();
        return 0;
    }
    worker_start();

    pal_new(0);

    ngl_clear(ngl_screen(), TH_BG);     /* clears all, repaints the system bar */
    ngl_flush();

    /*
     * Somewhere the search found, not the whole set: that view is the one
     * picture of the Mandelbrot everyone has already seen, and it is two
     * taps away from here anyway - PLACE held down. Without the second
     * buffer there is no attract loop to run afterwards, so the opening
     * view is all it is.
     */
    if (s_fades) {
        attract_begin();
        neos_status_for("touch anywhere to take over", 4000);
    } else {
        fr_home(MODE_MANDEL, &g_scene);
        restart();
        neos_status_for("drag a frame to zoom, tap to dive in", 4000);
    }

    while (!neos_app_close_requested()) {
        const int act = gesture_poll();

        switch (act) {
        case ACT_BUSY:
            neos_sleep_ms(15);          /* the finger is down: leave it alone */
            continue;
        case ACT_ZOOM_RECT:
            zoom_rect(s_act_rect);
            continue;
        case ACT_ZOOM_PT:
            zoom_point(s_act_x, s_act_y);
            continue;
        case ACT_BTN:
            switch (s_act_btn) {
            case BTN_OUT:   zoom_out();              break;
            case BTN_SAVE:  save_now();              break;
            case BTN_PAL:   new_palette();           break;
            case BTN_PLACE: new_place(g_scene.mode); break;
            case BTN_MODE:  next_mode();             break;
            default: break;
            }
            continue;
        case ACT_BTN_LONG:
            if (s_act_btn == BTN_PLACE) {
                zoom_home();
            }
            continue;
        default:
            break;
        }

        orient_poll();

        if (s_rp != RP_DONE) {
            s_w_busy = 1;
            render_step();
            continue;
        }
        s_w_busy = 0;
        if (s_at != AT_OFF) {
            attract_step();             /* it sleeps for itself when it waits */
            continue;
        }
        neos_sleep_ms(20);
    }

    printf("[mandel] closing\n");
    teardown();
    return 0;
}
