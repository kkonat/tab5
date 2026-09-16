/*
 * Enough of esp_heap_caps.h to build ngl on a PC.
 *
 * ngl_draw.c is portable apart from this one header - the only thing it wants
 * from ESP-IDF is an allocator that can be asked for PSRAM. On a desktop there
 * is one kind of memory, so the capability bits are accepted and ignored.
 *
 * The alignment is honoured for real, though, and not faked with plain malloc:
 * ngl_surface_new() asks for 64-byte alignment so its surfaces can be handed
 * to the PPA, and a simulator that quietly returned unaligned memory would
 * hide the one class of bug that alignment requirement exists to prevent.
 */
#ifndef SIM_ESP_HEAP_CAPS_H
#define SIM_ESP_HEAP_CAPS_H

#include <stdlib.h>
#include <string.h>

#define MALLOC_CAP_SPIRAM   (1 << 0)
#define MALLOC_CAP_DMA      (1 << 1)
#define MALLOC_CAP_INTERNAL (1 << 2)
#define MALLOC_CAP_8BIT     (1 << 3)

static inline void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    return malloc(n);
}

static inline void *heap_caps_calloc(size_t n, size_t sz, unsigned caps)
{
    (void)caps;
    return calloc(n, sz);
}

static inline void *heap_caps_aligned_alloc(size_t align, size_t n, unsigned caps)
{
    (void)caps;
    return _aligned_malloc(n, align);
}

static inline void *heap_caps_aligned_calloc(size_t align, size_t n, size_t sz,
                                             unsigned caps)
{
    (void)caps;
    void *p = _aligned_malloc(n * sz, align);
    if (p) {
        memset(p, 0, n * sz);
    }
    return p;
}

/*
 * On the device heap_caps_free() takes both plain and aligned blocks. Here the
 * CRT keeps them in separate heaps, and ngl only ever frees what it allocated
 * with the aligned call, so this matches what it does rather than what the
 * name suggests in general.
 */
static inline void heap_caps_free(void *p)
{
    _aligned_free(p);
}

#endif /* SIM_ESP_HEAP_CAPS_H */
