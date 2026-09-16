/*
 * neos_sock.h's TLS half, over esp-tls.
 *
 * Two sessions, a table, and a small state machine in front of somebody
 * else's. What is interesting here is only why the machine is in front at
 * all, because esp-tls has one of its own and this could have been six lines
 * around it.
 *
 * Two things in that machine are unusable from here, and both are about not
 * blocking - which on this machine is not a performance question but the
 * difference between a tablet and a brick, since an app runs on NeOS's own
 * stack and a call that waits is a close button that has stopped working.
 *
 * The first is the name. esp-tls resolves it with getaddrinfo(), which
 * blocks for as long as a resolver takes. So the lookup is done here first
 * through neos_resolve(), which does not.
 *
 * The second is the connect. esp_tls_conn_new_async() fills the fd_sets it
 * selects on once, in its INIT step, and select() clears them when it times
 * out - so from the second call onwards it is selecting on nothing, and a
 * connect that did not complete inside the first slice never completes at
 * all. Passing a long timeout hides it by blocking, which is the thing being
 * avoided.
 *
 * So the socket is dialled here, with the ordinary non-blocking dance from
 * neos_sock.h, and handed over already connected. esp-tls is then entered at
 * ESP_TLS_CONNECTING with non_block off, which is the one path that skips the
 * select entirely and goes straight to building the mbedTLS context and
 * stepping the handshake. The handshake step is honest either way: it returns
 * "want read" as 0 whatever the config says, because the socket under it is
 * non-blocking and mbedTLS can tell.
 *
 * Entering at ESP_TLS_HANDSHAKE instead would look tidier and is wrong: the
 * mbedTLS context is built on the CONNECTING edge, so a session joined at the
 * handshake has nothing to hand the handshake.
 */

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_tls.h"

#include "neos_sock.h"

static const char *TAG = "tls";

typedef enum {
    SLOT_FREE = 0,
    SLOT_RESOLVING,     /* waiting on neos_resolve() */
    SLOT_CONNECTING,    /* our own socket, our own non-blocking connect */
    SLOT_HANDSHAKE,     /* esp-tls, one step per call */
    SLOT_READY,
    SLOT_FAILED,
} slot_state_t;

typedef struct {
    slot_state_t   state;
    esp_tls_t     *tls;
    esp_tls_cfg_t  cfg;
    char           host[NEOS_HOST_MAX];
    uint16_t       port;
    uint32_t       ip;
    int            fd;      /* ours until esp-tls takes it, then -1 */
} tls_slot_t;

static tls_slot_t s_slots[NEOS_TLS_BUDGET];

/* Handles are one-based so that zero is never a live session: an app that
   kept a zeroed struct around would otherwise be holding a valid handle to
   whatever slot happened to be first. */
static tls_slot_t *slot_of(int handle)
{
    if (handle < 1 || handle > NEOS_TLS_BUDGET) {
        return NULL;
    }
    tls_slot_t *s = &s_slots[handle - 1];
    return s->state == SLOT_FREE ? NULL : s;
}

static void slot_release(tls_slot_t *s)
{
    if (s->tls) {
        esp_tls_conn_destroy(s->tls);   /* closes the socket it was given */
        s->tls = NULL;
    } else if (s->fd >= 0) {
        neos_sock_close(s->fd);         /* never got that far */
    }
    s->fd    = -1;
    s->state = SLOT_FREE;
}

static int fail_slot(tls_slot_t *s, const char *why)
{
    ESP_LOGW(TAG, "%s:%u %s", s->host, (unsigned)s->port, why);
    if (s->tls) {
        esp_tls_conn_destroy(s->tls);
        s->tls = NULL;
    } else if (s->fd >= 0) {
        neos_sock_close(s->fd);
    }
    s->fd    = -1;
    s->state = SLOT_FAILED;
    return NEOS_SOCK_ERR;
}

/* ------------------------------------------------------------------ */

int neos_tls_open(const char *host, uint16_t port)
{
    if (!host || !*host || strlen(host) >= NEOS_HOST_MAX) {
        return NEOS_SOCK_ERR;
    }

    tls_slot_t *s = NULL;
    int handle = 0;
    for (int i = 0; i < NEOS_TLS_BUDGET; i++) {
        if (s_slots[i].state == SLOT_FREE) {
            s = &s_slots[i];
            handle = i + 1;
            break;
        }
    }
    if (!s) {
        ESP_LOGW(TAG, "no free session - the budget is %d", NEOS_TLS_BUDGET);
        return NEOS_SOCK_ERR;
    }

    memset(s, 0, sizeof(*s));
    strlcpy(s->host, host, sizeof(s->host));
    s->port = port;
    s->fd   = -1;

    /*
     * The bundle is the IDF's, already in this image because the weather
     * fetch uses it. non_block stays false on purpose - see the note at the
     * top: it is what makes esp-tls skip the select it cannot do twice.
     */
    s->cfg.crt_bundle_attach = esp_crt_bundle_attach;
    s->cfg.non_block         = false;
    s->cfg.timeout_ms        = 0;

    s->tls = esp_tls_init();
    if (!s->tls) {
        ESP_LOGE(TAG, "no memory for a session");
        s->state = SLOT_FREE;
        return NEOS_SOCK_ERR;
    }

    s->state = SLOT_RESOLVING;
    return handle;
}

int neos_tls_status(int handle)
{
    tls_slot_t *s = slot_of(handle);
    if (!s) {
        return NEOS_SOCK_ERR;
    }

    switch (s->state) {
    case SLOT_READY:
        return NEOS_SOCK_OK;
    case SLOT_FAILED:
        return NEOS_SOCK_ERR;

    case SLOT_RESOLVING: {
        const int found = neos_resolve(s->host, &s->ip);
        if (found == NEOS_SOCK_PENDING) {
            return NEOS_SOCK_PENDING;
        }
        if (found != NEOS_SOCK_OK) {
            return fail_slot(s, "cannot be looked up");
        }

        s->fd = neos_sock_open(NEOS_SOCK_TCP);
        if (s->fd < 0) {
            return fail_slot(s, "has no socket to dial with");
        }
        const int dial = neos_sock_connect(s->fd, s->ip, s->port);
        if (dial != NEOS_SOCK_OK && dial != NEOS_SOCK_PENDING) {
            return fail_slot(s, "would not dial");
        }
        s->state = SLOT_CONNECTING;
        if (dial != NEOS_SOCK_OK) {
            return NEOS_SOCK_PENDING;
        }
    }
    /* falls through - connected on the spot, which a near host does */

    case SLOT_CONNECTING: {
        if (s->state == SLOT_CONNECTING) {
            /* Writability first and the status second, which is the contract
               neos_sock.h states: SO_ERROR is zero on a socket still in the
               handshake as well as on one that finished it. */
            uint8_t events = NEOS_SOCK_WRITE;
            const int ready = neos_sock_wait(&s->fd, &events, 1, 0);
            if (ready <= 0 || !(events & (NEOS_SOCK_WRITE | NEOS_SOCK_FAIL))) {
                return NEOS_SOCK_PENDING;
            }
            const int up = neos_sock_status(s->fd);
            if (up == NEOS_SOCK_PENDING) {
                return NEOS_SOCK_PENDING;
            }
            if (up != NEOS_SOCK_OK) {
                return fail_slot(s, "refused the connection");
            }
        }

        /*
         * Hand the connected socket over and join esp-tls's machine at the
         * step that builds the mbedTLS context. From here it owns the
         * descriptor, so ours is forgotten rather than closed.
         */
        if (esp_tls_set_conn_sockfd(s->tls, s->fd) != ESP_OK ||
            esp_tls_set_conn_state(s->tls, ESP_TLS_CONNECTING) != ESP_OK) {
            return fail_slot(s, "could not be handed to esp-tls");
        }
        s->fd    = -1;
        s->state = SLOT_HANDSHAKE;
    }
    /* falls through - the first handshake step can go on this call */

    case SLOT_HANDSHAKE: {
        const int r = esp_tls_conn_new_async(s->host, (int)strlen(s->host),
                                             (int)s->port, &s->cfg, s->tls);
        if (r == 0) {
            return NEOS_SOCK_PENDING;
        }
        if (r == 1) {
            s->state = SLOT_READY;
            ESP_LOGI(TAG, "%s:%u up", s->host, (unsigned)s->port);
            return NEOS_SOCK_OK;
        }
        return fail_slot(s, "would not shake hands");
    }

    default:
        return NEOS_SOCK_ERR;
    }
}

/*
 * Both of these map mbedTLS's "I would have waited" onto the one this ABI
 * uses everywhere. Worth being exact about: WANT_WRITE coming back from a
 * read is not a mistake, it is a renegotiation needing to send something
 * first, and the caller's answer to both is the same - come back later.
 */
static int again_or_err(ssize_t r)
{
    if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
        return NEOS_SOCK_AGAIN;
    }
    return NEOS_SOCK_ERR;
}

int neos_tls_send(int handle, const void *buf, int len)
{
    tls_slot_t *s = slot_of(handle);
    if (!s || s->state != SLOT_READY || !buf || len <= 0) {
        return NEOS_SOCK_ERR;
    }

    const ssize_t r = esp_tls_conn_write(s->tls, buf, (size_t)len);
    return r > 0 ? (int)r : again_or_err(r);
}

int neos_tls_recv(int handle, void *buf, int len)
{
    tls_slot_t *s = slot_of(handle);
    if (!s || s->state != SLOT_READY || !buf || len <= 0) {
        return NEOS_SOCK_ERR;
    }

    const ssize_t r = esp_tls_conn_read(s->tls, buf, (size_t)len);
    if (r > 0) {
        return (int)r;
    }
    if (r == 0) {
        return 0;               /* the peer closed it, same as a socket */
    }
    return again_or_err(r);
}

int neos_tls_fd(int handle)
{
    tls_slot_t *s = slot_of(handle);
    if (!s) {
        return NEOS_SOCK_ERR;
    }
    if (s->fd >= 0) {
        return s->fd;           /* still ours: dialling */
    }
    int fd = -1;
    if (!s->tls || esp_tls_get_conn_sockfd(s->tls, &fd) != ESP_OK || fd < 0) {
        return NEOS_SOCK_ERR;
    }
    return fd;
}

void neos_tls_close(int handle)
{
    tls_slot_t *s = slot_of(handle);
    if (s) {
        slot_release(s);
    }
}
