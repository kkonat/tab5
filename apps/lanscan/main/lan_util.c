/*
 * The pieces libc would have had.
 *
 * Apps link -nostdlib against a syscall table, and between NeOS and the ELF
 * loader that table carries memcpy, memset, memmove, memcmp, strlen, strcmp,
 * strchr and snprintf. Everything else a program that parses network
 * responses reaches for - a bounded copy, a case-insensitive search, an
 * integer parse - is written here rather than wished for.
 *
 * They are deliberately not called strlcpy and strcasestr. A name the
 * compiler knows is a name it may turn back into a call to the libc version
 * that is not there, and that failure arrives as an app which will not load
 * rather than as anything the build says.
 */

#include "lanscan.h"

/* ------------------------------------------------------------------ */
/* Strings                                                             */
/* ------------------------------------------------------------------ */

size_t lan_copy(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = 0;
        return 0;
    }
    size_t i = 0;
    while (src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

size_t lan_copy_n(char *dst, size_t size, const char *src, size_t n)
{
    if (!dst || size == 0) {
        return 0;
    }
    if (!src) {
        dst[0] = 0;
        return 0;
    }
    size_t i = 0;
    while (i < n && src[i] && i + 1 < size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/*
 * Collapse runs of whitespace, drop anything that is not printable ASCII, and
 * stop at the buffer.
 *
 * The reason this exists at all: everything it is applied to came off the
 * wire from a device nobody here chose, and a NetBIOS name field is fifteen
 * bytes of whatever that device felt like putting in it. A control character
 * reaching ngl_text() is not a crash, it is a glyph out of the middle of the
 * font in the middle of a table, which is worse than useless because it looks
 * like data.
 */
void lan_clean(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }

    size_t out = 0;
    bool   gap = false;
    bool   any = false;

    for (const char *p = src; *p && out + 1 < size; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            gap = any;               /* leading whitespace is simply dropped */
            continue;
        }
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        if (gap && out + 2 < size) {
            dst[out++] = ' ';
        }
        gap = false;
        any = true;
        dst[out++] = (char)c;
    }
    dst[out] = 0;
}

bool lan_ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (lower(*a) != lower(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

const char *lan_after(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) {
        return NULL;
    }
    for (const char *p = haystack; *p; p++) {
        const char *h = p;
        const char *n = needle;
        while (*h && *n && lower(*h) == lower(*n)) {
            h++;
            n++;
        }
        if (!*n) {
            return h;
        }
    }
    return NULL;
}

bool lan_has(const char *haystack, const char *needle)
{
    return lan_after(haystack, needle) != NULL;
}

int lan_atoi(const char *s)
{
    if (!s) {
        return 0;
    }
    while (*s == ' ') {
        s++;
    }
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int value = 0;
    while (*s >= '0' && *s <= '9') {
        if (value > 200000000) {          /* whatever this was, it is not a port */
            return sign * value;
        }
        value = value * 10 + (*s - '0');
        s++;
    }
    return sign * value;
}

/* ------------------------------------------------------------------ */
/* Addresses                                                           */
/* ------------------------------------------------------------------ */

void lan_ip_str(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
}

void lan_mac_str(const uint8_t mac[6], char *buf, size_t size)
{
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------------ */
/* Arithmetic                                                          */
/* ------------------------------------------------------------------ */

/*
 * The 16-bit one's complement sum, which ICMP needs on the way out because a
 * raw socket does not fill one in. Sums 16 bits at a time with a 32-bit
 * accumulator and folds the carries once at the end, which is the ordinary
 * trick and the reason this is eight lines rather than a loop with a branch
 * in it.
 */
uint16_t lan_checksum(const void *data, int len)
{
    const uint8_t *p   = (const uint8_t *)data;
    uint32_t       sum = 0;

    while (len > 1) {
        sum += (uint32_t)((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len == 1) {
        sum += (uint32_t)(p[0] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

uint32_t lan_now(void)
{
    return neos_uptime_ms();
}
