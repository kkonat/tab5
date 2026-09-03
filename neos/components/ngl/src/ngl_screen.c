/*
 * ngl screen binding - panel, rotation, and the reserved system bar.
 *
 * The Tab5 panel is a MIPI-DSI DPI display: it scans a linear 720x1280
 * framebuffer and has no hardware rotation. Since NeOS has to follow the
 * accelerometer, rotation is done in software here, and that forces a back
 * buffer - drawing rotated pixel-by-pixel straight into the framebuffer would
 * turn every horizontal span into a strided write.
 *
 * So: apps draw into a back buffer in *logical* coordinates (already rotated,
 * so a landscape app really is 1280x720), and ngl_flush() maps the dirty region
 * onto the panel. For NGL_ROT_0 that is a row memcpy; for 90/270 it is a
 * transpose, which is why keeping dirty regions small matters.
 *
 * The framebuffer lives in PSRAM behind the CPU cache while the DSI reads it
 * by DMA, so the flush also writes the cache back over the rows it touched.
 *
 * The top of the logical screen is reserved for the system bar. Apps cannot
 * draw there: the clip they are given excludes it, and ngl_clip_set() cannot
 * widen past it. Only ngl itself paints the bar, via a painter the OS registers.
 */
#include <string.h>
#include <stdint.h>

#include "ngl.h"
#include "ngl_internal.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ngl";

static ngl_color_t  *s_fb;              /* panel framebuffer, physical layout */
static int16_t      s_pw, s_ph;        /* physical panel size */

static ngl_surface_t s_back;            /* logical, what apps draw into */
static bool         s_ready;
/*
 * Several dirty rectangles, not one union.
 *
 * With a single bounding box an outlined card costs exactly as much to flush
 * as a filled one - the box covers the hollow middle either way, and the flush
 * is what the time goes on. Tracking the four edges separately is what turns
 * "draw outlines" into a real saving.
 */
#define NGL_DIRTY_MAX 8

static ngl_rect_t s_dirty[NGL_DIRTY_MAX];
static int       s_ndirty;

static ngl_rotation_t s_rot = NGL_ROT_0;
static int16_t       s_bar_h;
static ngl_bar_painter_t s_bar_painter;

/*
 * The dirty rectangle is shared mutable state, and the status bar animates
 * from its own task while apps draw from theirs. Pixels are safe - the regions
 * are disjoint - but the dirty accumulator is not, so it is guarded.
 * Recursive because painters call back into drawing code that flushes.
 */
/*
 * Rotating in software costs ~94 ms for a full 1280x720 screen: the transpose
 * walks down a column of the framebuffer, so every store lands in a different
 * PSRAM burst. The P4's PPA does the same job in hardware over DMA, so that is
 * the primary path and the software loops below are the fallback.
 */
static ppa_client_handle_t s_ppa;

static SemaphoreHandle_t s_lock;
static ngl_rect_t         s_bar_saved_clip;

/*
 * The modal overlay.
 *
 * s_ov_owner is the whole access-control mechanism: while it is set, a draw is
 * allowed only from that task. It is read without the lock on every pixel
 * write, which is why it is a plain handle compared for equality and nothing
 * more - taking a mutex per pixel would cost more than the drawing.
 *
 * Overlays nest, because panels do: the Wi-Fi list puts the keyboard up over
 * itself to take a password, and has to get its own list back afterwards, not
 * the app's. Each level saves the screen as it found it, so leaving unwinds to
 * whatever was underneath that level and nobody has to know how deep they are.
 * Only the owning task may nest - a second task asking for the screen is a
 * second panel, and there is only one screen.
 */
#define NGL_OVERLAY_DEPTH 3

static TaskHandle_t s_ov_owner;
static int          s_ov_depth;
static ngl_rect_t   s_ov_begin_clip;    /* per begin/end pair */

static struct {
    ngl_color_t *save;                  /* the screen as this level found it */
    ngl_rect_t   clip;                  /* and the clip it found */
    int16_t      w, h;
} s_ov[NGL_OVERLAY_DEPTH];

#define NGL_LOCK()   do { if (s_lock) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); } } while (0)
#define NGL_UNLOCK() do { if (s_lock) { xSemaphoreGiveRecursive(s_lock); } } while (0)

bool ngl_screen_blocked(const ngl_surface_t *s)
{
    return s_ov_depth > 0 && s == &s_back && xTaskGetCurrentTaskHandle() != s_ov_owner;
}

/* ------------------------------------------------------------------ */

static void alloc_back(int16_t w, int16_t h)
{
    if (s_back.px && s_back.owns_px) {
        heap_caps_free(s_back.px);
    }
    /* PPA reads this by DMA: external memory must be cache-line aligned. */
    ngl_color_t *px = heap_caps_aligned_calloc(64, (size_t)w * h, sizeof(ngl_color_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!px) {
        ESP_LOGE(TAG, "no PSRAM for a %dx%d back buffer", w, h);
        s_ready = false;
        return;
    }
    ngl_surface_init(&s_back, px, w, h, w);
    s_back.owns_px = true;
    ngl_clip_set(&s_back, NULL);   /* re-applies the bar reservation */
}

int ngl_screen_init(void *panel, int16_t w, int16_t h)
{
    if (!panel || w <= 0 || h <= 0) {
        return -1;
    }
    void *fb = NULL;
    esp_err_t err = esp_lcd_dpi_panel_get_frame_buffer((esp_lcd_panel_handle_t)panel,
                                                       1, &fb);
    if (err != ESP_OK || !fb) {
        ESP_LOGE(TAG, "no DPI framebuffer: %s", esp_err_to_name(err));
        return -2;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_ppa) {
        const ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&pc, &s_ppa) != ESP_OK) {
            ESP_LOGW(TAG, "no PPA client - rotation falls back to software");
            s_ppa = NULL;
        }
    }

    s_fb = (ngl_color_t *)fb;
    s_pw = w;
    s_ph = h;
    s_rot = NGL_ROT_0;
    s_ready = true;

    alloc_back(w, h);
    s_ndirty = 0;

    ESP_LOGI(TAG, "panel %dx%d, framebuffer %p; back buffer %dx%d (%u KB)",
             w, h, fb, s_back.w, s_back.h,
             (unsigned)((size_t)s_back.w * s_back.h * 2 / 1024));
    return s_ready ? 0 : -3;
}

ngl_surface_t *ngl_screen(void)
{
    return s_ready ? &s_back : NULL;
}

/* ------------------------------------------------------------------ */
/* Rotation                                                            */
/* ------------------------------------------------------------------ */

ngl_rotation_t ngl_rotation(void)
{
    return s_rot;
}

void ngl_set_rotation(ngl_rotation_t r)
{
    if (!s_ready || r == s_rot) {
        return;
    }
    /*
     * Not while a panel is up. Rotating reallocates the back buffer, which
     * would strand both the saved pixels underneath the panel and the layout
     * the panel computed from a screen that is no longer that shape. The
     * watcher samples on a timer and will offer the same rotation again once
     * the panel has closed, so nothing is lost by declining this one.
     */
    if (s_ov_depth > 0) {
        return;
    }
    const bool was_swapped = (s_rot == NGL_ROT_90 || s_rot == NGL_ROT_270);
    const bool now_swapped = (r == NGL_ROT_90 || r == NGL_ROT_270);
    s_rot = r;

    if (was_swapped != now_swapped) {
        alloc_back(now_swapped ? s_ph : s_pw,
                   now_swapped ? s_pw : s_ph);
    }
    ESP_LOGI(TAG, "rotation %d, logical %dx%d", (int)r, s_back.w, s_back.h);
    ngl_dirty_all();
}

/* Logical (lx,ly) -> physical index. Panel is s_pw x s_ph, native portrait. */
static inline size_t phys_index(int16_t lx, int16_t ly)
{
    switch (s_rot) {
    case NGL_ROT_90:
        /* logical x runs down the panel, logical y runs right-to-left */
        return (size_t)lx * s_pw + (s_pw - 1 - ly);
    case NGL_ROT_180:
        return (size_t)(s_ph - 1 - ly) * s_pw + (s_pw - 1 - lx);
    case NGL_ROT_270:
        return (size_t)(s_ph - 1 - lx) * s_pw + ly;
    default:
        return (size_t)ly * s_pw + lx;
    }
}

/*
 * Physical (panel) coordinates -> logical (screen) coordinates.
 *
 * The exact inverse of phys_index(). The touch controller reports in the
 * panel native frame and never learns about rotation, so something has to
 * undo it - and it belongs here, next to the forward transform, rather than
 * in the touch driver where the two could drift apart.
 */
void ngl_from_panel(int16_t px, int16_t py, int16_t *lx, int16_t *ly)
{
    if (!lx || !ly) {
        return;
    }
    switch (s_rot) {
    case NGL_ROT_90:
        *lx = py;
        *ly = (int16_t)(s_pw - 1 - px);
        break;
    case NGL_ROT_180:
        *lx = (int16_t)(s_pw - 1 - px);
        *ly = (int16_t)(s_ph - 1 - py);
        break;
    case NGL_ROT_270:
        *lx = (int16_t)(s_ph - 1 - py);
        *ly = px;
        break;
    default:
        *lx = px;
        *ly = py;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* System bar                                                          */
/* ------------------------------------------------------------------ */

void ngl_reserve_top(int16_t h, ngl_bar_painter_t painter)
{
    s_bar_h = h < 0 ? 0 : h;
    s_bar_painter = painter;
    if (s_ready) {
        ngl_clip_set(&s_back, NULL);
    }
}

int16_t ngl_bar_height(void)
{
    return s_bar_h;
}

ngl_rect_t ngl_bar_rect(void)
{
    return ngl_rect(0, 0, s_ready ? s_back.w : 0, s_bar_h);
}

ngl_rect_t ngl_app_area(void)
{
    if (!s_ready) {
        return ngl_rect(0, 0, 0, 0);
    }
    return ngl_rect(0, s_bar_h, s_back.w, (int16_t)(s_back.h - s_bar_h));
}

/* Paint the bar with the reservation lifted. Only ngl calls this. */
void ngl_bar_paint(void)
{
    if (!s_ready || s_bar_h <= 0 || !s_bar_painter) {
        return;
    }
    if (ngl_screen_blocked(&s_back)) {
        return;
    }
    const ngl_rect_t bar = ngl_bar_rect();
    const ngl_rect_t saved = s_back.clip;

    s_back.clip = bar;                  /* bypasses ngl_clip_set's reservation */
    s_bar_painter(&s_back, bar);
    s_back.clip = saved;

    ngl_dirty(&bar);
}

/* ------------------------------------------------------------------ */
/* Dirty tracking and flush                                            */
/* ------------------------------------------------------------------ */

static int32_t rect_area(const ngl_rect_t *r)
{
    return (int32_t)r->w * r->h;
}

void ngl_dirty(const ngl_rect_t *r)
{
    if (!s_ready || !r || ngl_rect_empty(r) || ngl_screen_blocked(&s_back)) {
        return;
    }
    NGL_LOCK();

    /* Merge into an existing rect when the union does not waste much, so a
       row of small updates does not immediately fill the list. */
    for (int i = 0; i < s_ndirty; i++) {
        const ngl_rect_t u = ngl_rect_union(&s_dirty[i], r);
        if (rect_area(&u) <= rect_area(&s_dirty[i]) + rect_area(r) + 4096) {
            s_dirty[i] = u;
            NGL_UNLOCK();
            return;
        }
    }

    if (s_ndirty < NGL_DIRTY_MAX) {
        s_dirty[s_ndirty++] = *r;
        NGL_UNLOCK();
        return;
    }

    /* Full: fold into whichever entry grows least. */
    int best = 0;
    int32_t best_growth = INT32_MAX;
    for (int i = 0; i < s_ndirty; i++) {
        const ngl_rect_t u = ngl_rect_union(&s_dirty[i], r);
        const int32_t growth = rect_area(&u) - rect_area(&s_dirty[i]);
        if (growth < best_growth) {
            best_growth = growth;
            best = i;
        }
    }
    s_dirty[best] = ngl_rect_union(&s_dirty[best], r);

    NGL_UNLOCK();
}

void ngl_dirty_all(void)
{
    if (!s_ready || ngl_screen_blocked(&s_back)) {
        return;
    }
    NGL_LOCK();
    s_dirty[0] = ngl_rect(0, 0, s_back.w, s_back.h);
    s_ndirty = 1;
    NGL_UNLOCK();
}

/* Move one rectangle of the back buffer onto the panel. Caller holds the lock. */
static void flush_one(ngl_rect_t d)
{
    const int64_t t0 = esp_timer_get_time();
    bool done = false;

    if (s_ppa && s_rot != NGL_ROT_0) {
        uint32_t ox = 0, oy = 0;
        ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;

        switch (s_rot) {
        case NGL_ROT_90:
            angle = PPA_SRM_ROTATION_ANGLE_270;
            ox = (uint32_t)(s_pw - d.y - d.h);
            oy = (uint32_t)d.x;
            break;
        case NGL_ROT_270:
            angle = PPA_SRM_ROTATION_ANGLE_90;
            ox = (uint32_t)d.y;
            oy = (uint32_t)(s_ph - d.x - d.w);
            break;
        default:
            angle = PPA_SRM_ROTATION_ANGLE_180;
            ox = (uint32_t)(s_pw - d.x - d.w);
            oy = (uint32_t)(s_ph - d.y - d.h);
            break;
        }

        const size_t back_row = (size_t)s_back.stride * sizeof(ngl_color_t);
        esp_cache_msync((uint8_t *)s_back.px + (size_t)d.y * back_row,
                        (size_t)d.h * back_row,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        const ppa_srm_oper_config_t op = {
            .in = {
                .buffer = s_back.px,
                .pic_w = (uint32_t)s_back.w, .pic_h = (uint32_t)s_back.h,
                .block_w = (uint32_t)d.w,    .block_h = (uint32_t)d.h,
                .block_offset_x = (uint32_t)d.x, .block_offset_y = (uint32_t)d.y,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_fb,
                .buffer_size = (uint32_t)((size_t)s_pw * s_ph * sizeof(ngl_color_t)),
                .pic_w = (uint32_t)s_pw, .pic_h = (uint32_t)s_ph,
                .block_offset_x = ox, .block_offset_y = oy,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = angle,
            .scale_x = 1.0f, .scale_y = 1.0f,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        if (ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK) {
            done = true;
        } else {
            ESP_LOGW(TAG, "PPA op failed, using software rotation");
        }
    }

    if (!done) {
        switch (s_rot) {
        case NGL_ROT_0:
            for (int16_t y = d.y; y < d.y + d.h; y++) {
                memcpy(&s_fb[(size_t)y * s_pw + d.x],
                       &s_back.px[(size_t)y * s_back.stride + d.x],
                       (size_t)d.w * sizeof(ngl_color_t));
            }
            break;
        case NGL_ROT_180:
            for (int16_t y = d.y; y < d.y + d.h; y++) {
                const ngl_color_t *src = &s_back.px[(size_t)y * s_back.stride + d.x];
                ngl_color_t *dst = &s_fb[(size_t)(s_ph - 1 - y) * s_pw + (s_pw - 1 - d.x)];
                for (int16_t x = 0; x < d.w; x++) {
                    *dst-- = src[x];
                }
            }
            break;
        default: {
            const int step = (s_rot == NGL_ROT_90) ? s_pw : -s_pw;
            for (int16_t y = d.y; y < d.y + d.h; y++) {
                const ngl_color_t *src = &s_back.px[(size_t)y * s_back.stride + d.x];
                ngl_color_t *dst = &s_fb[phys_index(d.x, y)];
                for (int16_t x = 0; x < d.w; x++) {
                    *dst = src[x];
                    dst += step;
                }
            }
            break;
        }
        }

        const int16_t cx[4] = { d.x, (int16_t)(d.x + d.w - 1), d.x, (int16_t)(d.x + d.w - 1) };
        const int16_t cy[4] = { d.y, d.y, (int16_t)(d.y + d.h - 1), (int16_t)(d.y + d.h - 1) };
        size_t lo = phys_index(cx[0], cy[0]), hi = lo;
        for (int i = 1; i < 4; i++) {
            const size_t p = phys_index(cx[i], cy[i]);
            if (p < lo) { lo = p; }
            if (p > hi) { hi = p; }
        }
        const size_t row_bytes = (size_t)s_pw * sizeof(ngl_color_t);
        size_t y0 = lo / s_pw, y1 = hi / s_pw + 1;
        if (y1 > (size_t)s_ph) { y1 = s_ph; }
        if (y1 > y0) {
            esp_cache_msync((uint8_t *)s_fb + y0 * row_bytes, (y1 - y0) * row_bytes,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }

    if ((int32_t)d.w * d.h > 200000) {
        ESP_LOGI(TAG, "flush %dx%d took %d ms (%s)", d.w, d.h,
                 (int)((esp_timer_get_time() - t0) / 1000), done ? "PPA" : "software");
    }
}

void ngl_flush(void)
{
    if (!s_ready) {
        return;
    }
    NGL_LOCK();

    const ngl_rect_t full = ngl_rect(0, 0, s_back.w, s_back.h);
    for (int i = 0; i < s_ndirty; i++) {
        ngl_rect_t d;
        if (ngl_rect_intersect(&s_dirty[i], &full, &d)) {
            flush_one(d);
        }
    }
    s_ndirty = 0;

    NGL_UNLOCK();
}

ngl_surface_t *ngl_bar_begin(ngl_rect_t region)
{
    if (!s_ready || s_bar_h <= 0) {
        return NULL;
    }
    NGL_LOCK();
    s_bar_saved_clip = s_back.clip;

    const ngl_rect_t bar = ngl_bar_rect();
    ngl_rect_t r;
    if (!ngl_rect_intersect(&region, &bar, &r)) {
        r = ngl_rect(0, 0, 0, 0);
    }
    s_back.clip = r;
    return &s_back;
}

void ngl_bar_end(void)
{
    if (!s_ready) {
        return;
    }
    s_back.clip = s_bar_saved_clip;
    NGL_UNLOCK();
}

/* ------------------------------------------------------------------ */
/* Modal overlays                                                      */
/* ------------------------------------------------------------------ */

bool ngl_overlay_active(void)
{
    return s_ov_depth > 0;
}

bool ngl_overlay_enter(void)
{
    if (!s_ready) {
        return false;
    }
    NGL_LOCK();

    if (s_ov_depth > 0 && xTaskGetCurrentTaskHandle() != s_ov_owner) {
        NGL_UNLOCK();
        return false;          /* somebody else has the screen */
    }
    if (s_ov_depth >= NGL_OVERLAY_DEPTH) {
        ESP_LOGW(TAG, "%d panels deep already", s_ov_depth);
        NGL_UNLOCK();
        return false;
    }

    /*
     * The save buffer is the whole logical screen, allocated per level rather
     * than kept around. A panel is open for seconds at a time and the buffer
     * is 1.8 MB; holding it for the life of the boot would be paying that
     * permanently for something used occasionally.
     */
    const size_t n = (size_t)s_back.w * s_back.h;
    ngl_color_t *save = heap_caps_malloc(n * sizeof(ngl_color_t), MALLOC_CAP_SPIRAM);
    if (!save) {
        ESP_LOGE(TAG, "no PSRAM to save %dx%d behind a panel", s_back.w, s_back.h);
        NGL_UNLOCK();
        return false;
    }
    for (int16_t y = 0; y < s_back.h; y++) {
        memcpy(&save[(size_t)y * s_back.w],
               &s_back.px[(size_t)y * s_back.stride],
               (size_t)s_back.w * sizeof(ngl_color_t));
    }

    s_ov[s_ov_depth].save = save;
    s_ov[s_ov_depth].clip = s_back.clip;
    s_ov[s_ov_depth].w    = s_back.w;
    s_ov[s_ov_depth].h    = s_back.h;
    s_ov_depth++;

    s_ov_owner = xTaskGetCurrentTaskHandle();

    /* Everyone else is now drawing into an empty clip as well as being refused
       at the pixel; belt and braces, and it costs nothing. */
    s_back.clip = ngl_rect(0, 0, 0, 0);

    NGL_UNLOCK();
    return true;
}

void ngl_overlay_leave(void)
{
    if (!s_ready || s_ov_depth <= 0) {
        return;
    }
    NGL_LOCK();

    const int i = --s_ov_depth;

    if (s_ov[i].save) {
        /*
         * Only if the screen is still the shape it was. Rotation is refused
         * while a panel is up, so the sizes should always match - and if they
         * ever do not, restoring the wrong pixels is worse than leaving the
         * panel's own frame up until something draws again.
         */
        if (s_ov[i].w == s_back.w && s_ov[i].h == s_back.h) {
            for (int16_t y = 0; y < s_back.h; y++) {
                memcpy(&s_back.px[(size_t)y * s_back.stride],
                       &s_ov[i].save[(size_t)y * s_ov[i].w],
                       (size_t)s_back.w * sizeof(ngl_color_t));
            }
        }
        heap_caps_free(s_ov[i].save);
        s_ov[i].save = NULL;
    }

    s_back.clip = s_ov[i].clip;
    if (s_ov_depth == 0) {
        s_ov_owner = NULL;     /* cleared before the flush, so it is allowed */
    }

    ngl_dirty_all();
    ngl_flush();

    NGL_UNLOCK();
}

void ngl_overlay_restore(void)
{
    if (!s_ready || s_ov_depth <= 0 || xTaskGetCurrentTaskHandle() != s_ov_owner) {
        return;
    }
    NGL_LOCK();
    const int i = s_ov_depth - 1;
    if (s_ov[i].save && s_ov[i].w == s_back.w && s_ov[i].h == s_back.h) {
        for (int16_t y = 0; y < s_back.h; y++) {
            memcpy(&s_back.px[(size_t)y * s_back.stride],
                   &s_ov[i].save[(size_t)y * s_ov[i].w],
                   (size_t)s_back.w * sizeof(ngl_color_t));
        }
        ngl_dirty_all();
    }
    NGL_UNLOCK();
}

ngl_surface_t *ngl_overlay_begin(ngl_rect_t region)
{
    if (!s_ready || s_ov_depth <= 0 || xTaskGetCurrentTaskHandle() != s_ov_owner) {
        return NULL;
    }
    NGL_LOCK();
    s_ov_begin_clip = s_back.clip;

    const ngl_rect_t whole = ngl_rect(0, 0, s_back.w, s_back.h);
    ngl_rect_t r;
    if (!ngl_rect_intersect(&region, &whole, &r)) {
        r = ngl_rect(0, 0, 0, 0);
    }
    s_back.clip = r;
    return &s_back;
}

void ngl_overlay_end(void)
{
    if (!s_ready) {
        return;
    }
    s_back.clip = s_ov_begin_clip;
    NGL_UNLOCK();
}
