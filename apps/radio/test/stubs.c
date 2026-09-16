/* Enough of NeOS and ngl to link the app's logic on a desktop. */
#include "radio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- clock --- */
static uint32_t g_now_ms = 1000;
void stub_set_now(uint32_t ms) { g_now_ms = ms; }
void stub_advance(uint32_t ms) { g_now_ms += ms; }
uint64_t neos_uptime_ms(void) { return g_now_ms; }

/* --- card --- */
static char  g_files[4][256];
static char  g_data[4][8192];
static int   g_len[4];
static int   g_count;

void stub_put_file(const char *rel, const char *text)
{
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_files[i], rel) == 0) {
            strcpy(g_data[i], text);
            g_len[i] = (int)strlen(text);
            return;
        }
    }
    strcpy(g_files[g_count], rel);
    strcpy(g_data[g_count], text);
    g_len[g_count] = (int)strlen(text);
    g_count++;
}
const char *stub_get_file(const char *rel)
{
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_files[i], rel) == 0) { return g_data[i]; }
    }
    return NULL;
}
void stub_clear_files(void) { g_count = 0; }

int neos_file_read(const char *rel, void *buf, size_t size)
{
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_files[i], rel) == 0) {
            if ((size_t)g_len[i] > size) { return -6; }
            memcpy(buf, g_data[i], (size_t)g_len[i]);
            return g_len[i];
        }
    }
    return -3;
}
int neos_file_write(const char *rel, const void *data, size_t len)
{
    char tmp[8192];
    if (len >= sizeof(tmp)) { return -1; }
    memcpy(tmp, data, len);
    tmp[len] = 0;
    stub_put_file(rel, tmp);
    return 0;
}
int neos_file_size(const char *rel) { (void)rel; return -3; }
int neos_file_read_at(const char *r, void *b, size_t l, uint32_t o)
{ (void)r; (void)b; (void)l; (void)o; return -3; }

/* --- sockets --- */
int  neos_sock_open(int kind) { (void)kind; return 3; }
void neos_sock_close(int fd) { (void)fd; }
int  neos_sock_bind(int f, uint32_t i, uint16_t p) { (void)f; (void)i; (void)p; return 0; }
int  neos_sock_connect(int f, uint32_t i, uint16_t p) { (void)f; (void)i; (void)p; return -3; }
int  neos_sock_status(int f) { (void)f; return -3; }
int  neos_sock_send(int f, const void *b, int l) { (void)f; (void)b; return l; }
int  neos_sock_recv(int f, void *b, int l) { (void)f; (void)b; (void)l; return -2; }
int  neos_sock_sendto(int f, const void *b, int l, uint32_t i, uint16_t p)
{ (void)f; (void)b; (void)i; (void)p; return l; }
int  neos_sock_recvfrom(int f, void *b, int l, uint32_t *ip, uint16_t *po)
{ (void)f; (void)b; (void)l; (void)ip; (void)po; return -2; }
int  neos_sock_wait(const int *f, uint8_t *e, int n, uint32_t ms)
{ (void)f; (void)e; (void)n; (void)ms; return 0; }
int  neos_sock_set(int f, int o, uint32_t v) { (void)f; (void)o; (void)v; return 0; }
int  neos_sock_join(int f, uint32_t g, uint32_t i) { (void)f; (void)g; (void)i; return 0; }
bool neos_iface(neos_iface_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ip = 0xc0a80005u;
    out->mask = 0xffffff00u;
    out->dns = 0xc0a80001u;
    return true;
}
int  neos_neigh_table(neos_neigh_t *o, int m) { (void)o; (void)m; return 0; }

/* The stub iface above always has an address, so the app's "no network yet"
   branch is not what these tests exercise; this only has to link. */
neos_net_state_t neos_net_state(void) { return NEOS_NET_ONLINE; }

/* TLS. The tests drive the stream machine over the plain path - what they are
   about is the ICY demux and the response head, which are the same bytes
   either way - so these only have to link and refuse. */
int  neos_tls_open(const char *host, uint16_t port)
{ (void)host; (void)port; return NEOS_SOCK_ERR; }
int  neos_tls_status(int h) { (void)h; return NEOS_SOCK_ERR; }
int  neos_tls_send(int h, const void *b, int l) { (void)h; (void)b; (void)l; return NEOS_SOCK_ERR; }
int  neos_tls_recv(int h, void *b, int l) { (void)h; (void)b; (void)l; return NEOS_SOCK_ERR; }
int  neos_tls_fd(int h) { (void)h; return NEOS_SOCK_ERR; }
void neos_tls_close(int h) { (void)h; }

/* Resolution answers at once here. The firmware's own is a state machine and
   the app polls it; what the tests care about is the state machine around it,
   which takes the same path either way. */
int neos_resolve(const char *host, uint32_t *ip)
{
    if (!host || !*host) {
        return NEOS_SOCK_ERR;
    }
    if (strcmp(host, "nowhere.invalid") == 0) {
        return NEOS_SOCK_ERR;
    }
    if (ip) {
        *ip = 0x5db8d822u;
    }
    return NEOS_SOCK_OK;
}
bool neos_neigh_ask(uint32_t ip) { (void)ip; return false; }

/* --- audio --- */
static int g_written;
bool    neos_audio_open(void) { return true; }
int     neos_audio_write(const int16_t *f, int n) { (void)f; g_written += n; return n; }
int32_t neos_audio_lead_us(void) { return 0; }
bool    neos_audio_gain(uint8_t p) { (void)p; return true; }
void    neos_audio_close(void) {}
int     stub_frames_written(void) { return g_written; }
void    stub_reset_written(void) { g_written = 0; }

/* --- the rest of the surface the app references --- */
/* The app logs its network transitions here. Swallowed by default so a
   passing run stays readable; set RADIO_TEST_VERBOSE to watch them. */
int neos_log(const char *msg)
{
    if (getenv("RADIO_TEST_VERBOSE")) {
        fputs("    [log] ", stdout);
        puts(msg ? msg : "(null)");
    }
    return 0;
}

void neos_sleep_ms(uint32_t ms) { (void)ms; }
bool neos_app_close_requested(void) { return true; }
const char *neos_app_self(void) { return "radio"; }
bool neos_touch(neos_touch_t *o) { (void)o; return false; }
bool neos_imu_accel_mg(int16_t *x, int16_t *y, int16_t *z)
{ (void)x; (void)y; (void)z; return false; }

/* --- ngl: enough to be called, not to draw --- */
static ngl_font_t g_small = { NULL, 16, 32, 2, 32, 126 };
static ngl_font_t g_large = { NULL, 24, 48, 3, 32, 126 };
const ngl_font_t ngl_font_small = { NULL, 16, 32, 2, 32, 126 };
const ngl_font_t ngl_font_large = { NULL, 24, 48, 3, 32, 126 };

ngl_surface_t *ngl_screen(void) { return NULL; }
void ngl_flush(void) {}
void ngl_dirty_all(void) {}
ngl_rect_t ngl_app_area(void) { return ngl_rect(0, 56, 1280, 664); }
int16_t ngl_bar_height(void) { return 56; }
int16_t ngl_text_width(const ngl_font_t *f, const char *s)
{ return (int16_t)(f->width * (int)strlen(s)); }
int16_t ngl_text(ngl_surface_t *s, int16_t x, int16_t y, const char *t,
                 const ngl_font_t *f, ngl_color_t c)
{ (void)s; (void)x; (void)y; (void)f; (void)c; return (int16_t)strlen(t); }
void ngl_clear(ngl_surface_t *s, ngl_color_t c) { (void)s; (void)c; }
void ngl_fill_rect(ngl_surface_t *s, ngl_rect_t r, ngl_color_t c) { (void)s; (void)r; (void)c; }
void ngl_draw_rect(ngl_surface_t *s, ngl_rect_t r, ngl_color_t c, int16_t t)
{ (void)s; (void)r; (void)c; (void)t; }
void ngl_fill_round_rect(ngl_surface_t *s, ngl_rect_t r, int16_t rad, ngl_color_t c)
{ (void)s; (void)r; (void)rad; (void)c; }
void ngl_draw_round_rect(ngl_surface_t *s, ngl_rect_t r, int16_t rad, ngl_color_t c, int16_t t)
{ (void)s; (void)r; (void)rad; (void)c; (void)t; }
void ngl_hline(ngl_surface_t *s, int16_t x, int16_t y, int16_t w, ngl_color_t c)
{ (void)s; (void)x; (void)y; (void)w; (void)c; }
void ngl_vline(ngl_surface_t *s, int16_t x, int16_t y, int16_t h, ngl_color_t c)
{ (void)s; (void)x; (void)y; (void)h; (void)c; }
void ngl_line(ngl_surface_t *s, int16_t a, int16_t b, int16_t c2, int16_t d, ngl_color_t c)
{ (void)s; (void)a; (void)b; (void)c2; (void)d; (void)c; }
void ngl_clip_set(ngl_surface_t *s, const ngl_rect_t *r) { (void)s; (void)r; }
ngl_rect_t ngl_surface_clip(const ngl_surface_t *s) { (void)s; return ngl_rect(0, 0, 1280, 720); }
bool ngl_rect_contains(const ngl_rect_t *r, int16_t x, int16_t y)
{ return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h; }
bool ngl_rect_intersect(const ngl_rect_t *a, const ngl_rect_t *b, ngl_rect_t *o)
{ (void)a; (void)b; (void)o; return false; }
ngl_rect_t ngl_rect_union(const ngl_rect_t *a, const ngl_rect_t *b) { (void)b; return *a; }
ngl_rotation_t ngl_rotation(void) { return NGL_ROT_90; }
void neos_orient_lock(ngl_rotation_t r) { (void)r; }
void neos_orient_unlock(void) {}
ngl_rotation_t neos_orient_get(void) { return NGL_ROT_90; }

void stub_unused(void) { (void)g_small; (void)g_large; }

/* The ABI guard the API header emits a reference to. On the target this is
   the firmware's own object and the loader's name lookup is the version
   check; here it only has to exist.

   Spelled through the macro rather than by name, so a minor bump does not
   break the tests with a link error about a symbol nobody typed. */
const uint32_t NEOS_ABI_SYMBOL = ((uint32_t)NEOS_ABI_MAJOR << 16) | NEOS_ABI_MINOR;
