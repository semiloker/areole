/*
 * areole - the arena. The only allocator in the library.
 * SPDX-License-Identifier: MIT
 */
#include "ar_internal.h"

#include <stddef.h>

/* Rounds up to the next AR_ALIGN boundary.
 *
 * This wraps for any n above 0xFFFFFFF8, and wrapping is worse than useless
 * here: it turns an absurd request into a request for zero bytes, which then
 * passes every bounds check and hands back a live pointer. Callers must
 * therefore reject n against the space remaining BEFORE rounding. Since the
 * arena masks its own size down to a whole number of alignment units, any n
 * that survives that check is small enough to round safely. */
static ar_u32 ar__align_up(ar_u32 n)
{
    return (n + (AR_ALIGN - 1u)) & ~(AR_ALIGN - 1u);
}

void ar_arena_init(ar_arena *a, void *mem, ar_u32 size)
{
    size_t addr;
    ar_u32 skew;

    a->base = (ar_u8 *)mem;
    a->size = size;
    a->lo = 0;
    a->hi = size;
    a->hi_min = size;
    a->oom = 0;

    if (mem == 0 || size == 0)
    {
        a->size = 0;
        a->hi = 0;
        a->hi_min = 0;
        a->oom = 1;
        return;
    }

    /* The caller's block may start anywhere, including a byte-aligned static
       array. Converting a pointer to an integer to inspect its alignment is
       implementation defined in C89, but it is defined on every target areole
       supports and there is no conforming alternative. Skew the base forward
       so that an aligned offset always yields an aligned pointer. */
    addr = (size_t)a->base;
    skew =
        (ar_u32)(((size_t)AR_ALIGN - (addr & ((size_t)AR_ALIGN - 1u))) & ((size_t)AR_ALIGN - 1u));

    if (skew >= size)
    {
        a->size = 0;
        a->hi = 0;
        a->hi_min = 0;
        a->oom = 1;
        return;
    }

    a->base += skew;
    a->size = size - skew;
    /* Keep the end aligned too, so ephemeral allocations land aligned without
       a second correction on every call. */
    a->size &= ~(AR_ALIGN - 1u);
    a->hi = a->size;
    a->hi_min = a->size;
}

void *ar_arena_persist(ar_arena *a, ar_u32 bytes)
{
    ar_u32 need;

    /* Unrounded first: this is what makes the rounding below safe to perform.
       Compare against the gap rather than adding to lo, so the offset cannot
       wrap past hi and yield memory overlapping the ephemeral region. */
    if (bytes > a->hi - a->lo)
    {
        a->oom = 1;
        return 0;
    }

    need = ar__align_up(bytes);
    if (need > a->hi - a->lo)
    {
        a->oom = 1;
        return 0;
    }

    a->lo += need;
    return a->base + (a->lo - need);
}

void *ar_arena_frame(ar_arena *a, ar_u32 bytes)
{
    ar_u32 need;

    if (bytes > a->hi - a->lo)
    {
        a->oom = 1;
        return 0;
    }

    need = ar__align_up(bytes);
    if (need > a->hi - a->lo)
    {
        a->oom = 1;
        return 0;
    }

    a->hi -= need;
    if (a->hi < a->hi_min)
    {
        a->hi_min = a->hi;
    }
    return a->base + a->hi;
}

void ar_arena_frame_reset(ar_arena *a)
{
    /* The whole point of the design: freeing a frame is one store. hi_min is
       deliberately not reset, so peak use is measured across the session. */
    a->hi = a->size;
}

ar_u32 ar_arena_persist_used(const ar_arena *a)
{
    return a->lo;
}

ar_u32 ar_arena_frame_used(const ar_arena *a)
{
    return a->size - a->hi;
}

ar_u32 ar_arena_frame_peak(const ar_arena *a)
{
    return a->size - a->hi_min;
}

ar_u32 ar_arena_available(const ar_arena *a)
{
    return a->hi - a->lo;
}
