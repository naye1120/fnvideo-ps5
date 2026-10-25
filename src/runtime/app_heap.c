// ps5-homebrew-ui - Process-lifetime heap for the OpenGL runtime.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-opengl native-app/app_heap.c. tools/build.sh links with
// --wrap for the malloc family whenever this file exists, routing allocations
// into a fixed 128 MiB sceLibcMspace that is never unmapped.

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *address, size_t size);
void __real_free(void *address);
int __real_posix_memalign(void **address, size_t alignment, size_t size);
size_t __real_malloc_usable_size(const void *address);

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags);
void *sceLibcMspaceMalloc(void *mspace, size_t size);
void *sceLibcMspaceCalloc(void *mspace, size_t count, size_t size);
void *sceLibcMspaceRealloc(void *mspace, void *address, size_t size);
void sceLibcMspaceFree(void *mspace, void *address);
int sceLibcMspacePosixMemalign(void *mspace, void **address, size_t alignment, size_t size);
size_t sceLibcMspaceMallocUsableSize(const void *address);

int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t search_start, int64_t search_end, size_t alignment,
                                       int64_t *start_out, size_t *size_out);
int sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                  size_t alignment, int memory_type, int64_t *start);
int sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                             int64_t start, size_t alignment);
int sceKernelReleaseDirectMemory(int64_t start, size_t length);

/* Fixed, process-lifetime heap: do not unmap underneath late C++ destructors.
 *
 * Where the memory comes from matters more than how much is asked for. This
 * used to mmap anonymous pages, which the console serves out of *flexible*
 * memory - a few hundred MB for the whole title, most of it taken by the
 * program image before main() runs. That is why 512 MB failed and the chain
 * settled on 256 MB, and 256 MB is the reason a 4K film died: the software
 * decoder's own frame pool is about 100 MB before the app asks for anything,
 * and three 4K pictures on top of that is the end of it.
 *
 * Direct memory is the other pool, and on this hardware it is about 2.3 GB
 * (klog: "DMEM size: 0x90000000"). The reference implementation this port
 * follows runs its malloc out of it and gets a 1.3 GB heap (stremio-ps5
 * native/heap.c, which in turn credits the Kodi PS5 port). So does this now.
 *
 * What is *not* taken is as important: the hardware video decoder maps its
 * buffers from the same pool, and it needs far more than a few. Kodi's PS5
 * port allocates its decoder's CPU workspace from direct memory too, precisely
 * because flexible memory is only about 448 MB and every thread stack comes out
 * of it. So the reserve stays generous: a heap that ate the pool would leave
 * the decoder short, send every film down the software path, and the software
 * decoder's own frame pool is what ran this title out of memory in the first
 * place.
 */
#define HUI_RESERVE_MB 1024

/* Largest first. A mapping is lazy - untouched pages cost nothing - but it
 * still has to fit in what is left, so the first size that maps wins. */
static const size_t kHeapSizes[] = {
    2048u * 1024u * 1024u, 1792u * 1024u * 1024u, 1536u * 1024u * 1024u, 1280u * 1024u * 1024u,
    1024u * 1024u * 1024u, 768u * 1024u * 1024u,  512u * 1024u * 1024u,  256u * 1024u * 1024u,
};
/* The size that was actually mapped; zero until hui_heap_ready() has run. */
static size_t hui_heap_size;
/* Where it came from, for the log line and for the unmap on failure. */
static int64_t hui_heap_direct_start = -1;

/* One block of direct memory, CPU read/write. The two memory types and the
 * two protections are the pairs the reference implementation and the Kodi
 * port use on hardware; the first that works wins. */
static void *hui_map_direct(size_t size)
{
    static const int types[] = {12, 0};
    static const int protections[] = {0x03, 0x33};
    int64_t limit = sceKernelGetDirectMemorySize();
    for (unsigned t = 0; t < 2; t++)
    {
        int64_t start = -1;
        if (sceKernelAllocateDirectMemory(0, limit, size, 0x4000, types[t], &start) != 0)
            continue;
        for (unsigned p = 0; p < 2; p++)
        {
            void *address = NULL;
            if (sceKernelMapDirectMemory(&address, size, protections[p], 0, start, 0x4000) == 0 &&
                address != NULL)
            {
                hui_heap_direct_start = start;
                return address;
            }
        }
        sceKernelReleaseDirectMemory(start, size);
    }
    return NULL;
}

/* The mirror file the runtime writes, so the size the heap got is in the log
 * even when the app dies before it can log anything itself. Weak: the host
 * build and the unit tests have no runtime shims. */
extern int hui_step_write(const char *text, int length) __attribute__((weak));

static void heap_note(const char *format, ...)
{
    if (hui_step_write == NULL)
        return;
    char line[128];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (length > 0)
        hui_step_write(line, length < (int)sizeof line ? length : (int)sizeof line - 1);
}

static atomic_int hui_heap_state;
static void *hui_heap_base;
static void *hui_heap_mspace;
/* Owned-heap usable bytes only; relaxed counters are observations, not fences. */
static atomic_size_t hui_heap_live_bytes, hui_heap_peak_bytes, hui_heap_blocks;
static atomic_size_t hui_heap_failures;

static void hui_heap_resize_stats(size_t before, size_t after)
{
    size_t live = after >= before ? atomic_fetch_add_explicit(&hui_heap_live_bytes, after - before,
                                                              memory_order_relaxed) +
                                        after - before
                                  : atomic_fetch_sub_explicit(&hui_heap_live_bytes, before - after,
                                                              memory_order_relaxed) -
                                        (before - after);
    size_t peak = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    while (live > peak &&
           !atomic_compare_exchange_weak_explicit(&hui_heap_peak_bytes, &peak, live,
                                                  memory_order_relaxed, memory_order_relaxed))
    {
    }
}

static void *hui_heap_record_allocation(void *address, int nonzero)
{
    if (address)
    {
        hui_heap_resize_stats(0, sceLibcMspaceMallocUsableSize(address));
        atomic_fetch_add_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
    }
    else if (nonzero)
    {
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    }
    return address;
}

static int hui_heap_ready(void)
{
    int state = atomic_load_explicit(&hui_heap_state, memory_order_acquire);
    if (state == 2)
        return 1;
    if (state != 0)
        return 0;

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&hui_heap_state, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire))
        return expected == 2;

    void *base = MAP_FAILED;
    size_t size = 0;
    /* Size the request from what the pool actually is, not from a constant:
     * the console reports the total, and the decoder's share comes off the
     * top. On hardware that is 2.3 GB - 800 MB, so the chain starts at
     * 1.3 GB rather than trying and failing two sizes first. */
    const int64_t direct_total = sceKernelGetDirectMemorySize();
    size_t cap = SIZE_MAX;
    if (direct_total > 0)
    {
        const int64_t reserve = (int64_t)HUI_RESERVE_MB << 20;
        cap = direct_total > reserve ? (size_t)(direct_total - reserve) : (size_t)256 << 20;
    }
    for (size_t i = 0; i < sizeof kHeapSizes / sizeof kHeapSizes[0]; i++)
    {
        size = kHeapSizes[i];
        if (size > cap)
            continue;
        base = hui_map_direct(size);
        if (base == NULL)
            continue;
        void *mspace = sceLibcMspaceCreate("ps5-homebrew-ui", base, size, 0);
        if (mspace != NULL)
        {
            hui_heap_base = base;
            hui_heap_mspace = mspace;
            hui_heap_size = size;
            heap_note("[HUI] heap %zu MB of direct memory mapped (total %lld MB)\n", size >> 20,
                      (long long)(direct_total >> 20));
            atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
            return 1;
        }
        sceKernelReleaseDirectMemory(hui_heap_direct_start, size);
        hui_heap_direct_start = -1;
    }

    /* No direct memory: fall back to anonymous pages, which the console serves
     * out of flexible memory. Small, but a small heap that works beats no app
     * at all - and the size chain says how small it had to go. */
    heap_note("[HUI] heap: no direct memory; trying flexible\n");
    for (size_t i = 0; i < sizeof kHeapSizes / sizeof kHeapSizes[0]; i++)
    {
        size = kHeapSizes[i];
        base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (base == MAP_FAILED)
            continue;
        void *mspace = sceLibcMspaceCreate("ps5-homebrew-ui", base, size, 0);
        if (mspace != NULL)
        {
            hui_heap_base = base;
            hui_heap_mspace = mspace;
            hui_heap_size = size;
            heap_note("[HUI] heap %zu MB mapped (flexible)\n", size >> 20);
            atomic_store_explicit(&hui_heap_state, 2, memory_order_release);
            return 1;
        }
        munmap(base, size);
    }

    /* Nothing mapped: every allocation now goes to the default libc heap, which
     * is small enough that the app will die during start-up. Say so. */
    heap_note("[HUI] heap NOT mapped; falling back to the libc heap\n");
    atomic_store_explicit(&hui_heap_state, -1, memory_order_release);
    return 0;
}

static int hui_heap_owns(const void *address)
{
    /* Acquire publication before reading non-atomic heap metadata. */
    if (atomic_load_explicit(&hui_heap_state, memory_order_acquire) != 2)
        return 0;
    uintptr_t value = (uintptr_t)address;
    uintptr_t base = (uintptr_t)hui_heap_base;
    return value >= base && value - base < hui_heap_size;
}

void *__wrap_malloc(size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceMalloc(hui_heap_mspace, size), size != 0)
               : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    return hui_heap_ready()
               ? hui_heap_record_allocation(sceLibcMspaceCalloc(hui_heap_mspace, count, size),
                                            count != 0 && size != 0)
               : __real_calloc(count, size);
}

void *__wrap_realloc(void *address, size_t size)
{
    if (address == NULL)
        return __wrap_malloc(size);
    if (!hui_heap_owns(address))
        return __real_realloc(address, size);
    size_t before = sceLibcMspaceMallocUsableSize(address);
    void *result = sceLibcMspaceRealloc(hui_heap_mspace, address, size);
    if (result)
        hui_heap_resize_stats(before, sceLibcMspaceMallocUsableSize(result));
    else if (size)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

void __wrap_free(void *address)
{
    if (hui_heap_owns(address))
    {
        hui_heap_resize_stats(sceLibcMspaceMallocUsableSize(address), 0);
        atomic_fetch_sub_explicit(&hui_heap_blocks, 1, memory_order_relaxed);
        sceLibcMspaceFree(hui_heap_mspace, address);
    }
    else
    {
        __real_free(address);
    }
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size)
{
    if (!hui_heap_ready())
        return __real_posix_memalign(address, alignment, size);
    int result = sceLibcMspacePosixMemalign(hui_heap_mspace, address, alignment, size);
    if (result == 0)
        hui_heap_record_allocation(*address, size != 0);
    else if (result == ENOMEM)
        atomic_fetch_add_explicit(&hui_heap_failures, 1, memory_order_relaxed);
    return result;
}

size_t __wrap_malloc_usable_size(const void *address)
{
    return hui_heap_owns(address) ? sceLibcMspaceMallocUsableSize(address)
                                  : __real_malloc_usable_size(address);
}

/* Diagnostics: current heap counters for logs and the diagnostics screen. */
void hui_heap_stats(size_t *live_bytes, size_t *peak_bytes, size_t *blocks, size_t *failures)
{
    if (live_bytes)
        *live_bytes = atomic_load_explicit(&hui_heap_live_bytes, memory_order_relaxed);
    if (peak_bytes)
        *peak_bytes = atomic_load_explicit(&hui_heap_peak_bytes, memory_order_relaxed);
    if (blocks)
        *blocks = atomic_load_explicit(&hui_heap_blocks, memory_order_relaxed);
    if (failures)
        *failures = atomic_load_explicit(&hui_heap_failures, memory_order_relaxed);
}

/* How much the heap could ever hold, so a log line can say "live out of
 * capacity" instead of leaving the reader to know the constant. */
size_t hui_heap_capacity(void)
{
    return hui_heap_size;
}
