/*
 * areole - test harness.
 * SPDX-License-Identifier: MIT
 *
 * No framework, no fixtures. Every non-trivial piece of logic leaves one
 * runnable check here that fails if the logic breaks.
 */
#include "ar_internal.h"

#include <stdio.h>
#include <string.h>

static int ar__checks = 0;
static int ar__failures = 0;

static void ar__check(int ok, const char *what, const char *file, int line)
{
    ar__checks++;
    if (!ok)
    {
        ar__failures++;
        printf("FAIL  %s\n      at %s:%d\n", what, file, line);
    }
}

#define CHECK(expr, what) ar__check((expr) ? 1 : 0, what, __FILE__, __LINE__)

/* ------------------------------------------------------------------------
 * Arena
 * ------------------------------------------------------------------------ */

/* Deliberately over-sized and offset by one below, so the alignment skew in
   ar_arena_init has something real to correct. */
static ar_u8 g_block[4096];

static int ar__is_aligned(const void *p)
{
    return ((size_t)p & (AR_ALIGN - 1u)) == 0;
}

static void test_arena_alignment(void)
{
    ar_arena a;
    void    *p;

    /* Start one byte in, forcing a non-zero skew. */
    ar_arena_init(&a, g_block + 1, (ar_u32)sizeof g_block - 1u);

    CHECK(a.oom == 0, "arena: init of a valid block does not report oom");
    CHECK(ar__is_aligned(a.base), "arena: base is aligned after init");
    CHECK((a.size & (AR_ALIGN - 1u)) == 0, "arena: size is a whole number of alignment units");

    p = ar_arena_persist(&a, 1);
    CHECK(p != 0, "arena: a one byte persistent allocation succeeds");
    CHECK(ar__is_aligned(p), "arena: a one byte persistent allocation is still aligned");

    p = ar_arena_persist(&a, 3);
    CHECK(ar__is_aligned(p), "arena: the allocation after an odd size is aligned");

    p = ar_arena_frame(&a, 5);
    CHECK(p != 0, "arena: a five byte frame allocation succeeds");
    CHECK(ar__is_aligned(p), "arena: a five byte frame allocation is aligned");
}

static void test_arena_regions_do_not_overlap(void)
{
    ar_arena a;
    ar_u8   *persist;
    ar_u8   *frame;

    ar_arena_init(&a, g_block, (ar_u32)sizeof g_block);

    persist = (ar_u8 *)ar_arena_persist(&a, 64);
    frame = (ar_u8 *)ar_arena_frame(&a, 64);

    CHECK(persist != 0 && frame != 0, "arena: both regions allocate");
    CHECK(persist + 64 <= frame, "arena: persistent memory sits below ephemeral memory");

    /* Write distinct patterns and confirm neither region sees the other. */
    memset(persist, 0xAA, 64);
    memset(frame, 0xBB, 64);
    CHECK(persist[0] == 0xAA && persist[63] == 0xAA, "arena: persistent region survives intact");
    CHECK(frame[0] == 0xBB && frame[63] == 0xBB, "arena: ephemeral region survives intact");
}

static void test_arena_frame_reset(void)
{
    ar_arena a;
    void    *first;
    void    *again;

    ar_arena_init(&a, g_block, (ar_u32)sizeof g_block);

    first = ar_arena_frame(&a, 128);
    CHECK(ar_arena_frame_used(&a) == 128, "arena: frame use is reported after allocating");

    ar_arena_frame_reset(&a);
    CHECK(ar_arena_frame_used(&a) == 0, "arena: reset returns the frame region to empty");

    again = ar_arena_frame(&a, 128);
    CHECK(again == first, "arena: the first allocation after a reset reuses the same address");

    /* Peak is measured across the session, so a reset must not clear it. */
    CHECK(ar_arena_frame_peak(&a) == 128, "arena: reset does not clear the peak watermark");
}

static void test_arena_exhaustion(void)
{
    ar_arena a;
    void    *p;
    ar_u32   room;

    ar_arena_init(&a, g_block, (ar_u32)sizeof g_block);
    room = ar_arena_available(&a);

    p = ar_arena_persist(&a, room);
    CHECK(p != 0, "arena: an allocation of exactly the space available succeeds");
    CHECK(ar_arena_available(&a) == 0, "arena: nothing is left afterwards");

    p = ar_arena_persist(&a, 1);
    CHECK(p == 0, "arena: one byte past full is refused");
    CHECK(a.oom != 0, "arena: refusing an allocation sets oom");

    p = ar_arena_frame(&a, 1);
    CHECK(p == 0, "arena: the ephemeral region is exhausted too");
}

static void test_arena_oversized_request_cannot_wrap(void)
{
    ar_arena a;
    void    *p;

    ar_arena_init(&a, g_block, (ar_u32)sizeof g_block);

    /* Rounding this size up to the alignment boundary overflows a 32 bit
       offset and lands on zero. A zero sized request passes every bounds
       check, so the arena would hand back a live pointer for a request it
       cannot possibly satisfy. The size is therefore checked before it is
       rounded, not after. */
    p = ar_arena_persist(&a, 0xFFFFFFF9u);
    CHECK(p == 0, "arena: a request that overflows when aligned is refused");
    CHECK(a.lo == 0, "arena: the refused request consumed nothing");

    ar_arena_init(&a, g_block, (ar_u32)sizeof g_block);
    p = ar_arena_frame(&a, 0xFFFFFFF9u);
    CHECK(p == 0, "arena: the same overflow is refused in the ephemeral region");
    CHECK(ar_arena_frame_used(&a) == 0, "arena: the refused frame request consumed nothing");
}

static void test_arena_rejects_nothing(void)
{
    ar_arena a;

    ar_arena_init(&a, 0, 1024);
    CHECK(a.oom != 0, "arena: a null block is rejected at init");
    CHECK(ar_arena_persist(&a, 1) == 0, "arena: nothing can be allocated from a rejected arena");

    ar_arena_init(&a, g_block, 0);
    CHECK(a.oom != 0, "arena: a zero sized block is rejected at init");

    /* A block too small to even absorb the alignment skew. */
    ar_arena_init(&a, g_block + 1, 2);
    CHECK(a.oom != 0, "arena: a block smaller than the alignment skew is rejected");
}

/* ------------------------------------------------------------------------
 * Rectangles
 * ------------------------------------------------------------------------ */

static int ar__rect_eq(ar_rect r, ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void test_rect_intersect(void)
{
    ar_rect a, b, r;

    a = ar_rect_make(0, 0, 100, 100);
    b = ar_rect_make(50, 50, 100, 100);
    r = ar_rect_intersect(a, b);
    CHECK(ar__rect_eq(r, 50, 50, 50, 50), "rect: overlapping rects intersect to the overlap");

    b = ar_rect_make(20, 20, 10, 10);
    r = ar_rect_intersect(a, b);
    CHECK(ar__rect_eq(r, 20, 20, 10, 10), "rect: a contained rect intersects to itself");

    b = ar_rect_make(200, 200, 10, 10);
    r = ar_rect_intersect(a, b);
    CHECK(ar_rect_is_empty(r), "rect: disjoint rects intersect to empty");
    CHECK(r.w == 0 && r.h == 0, "rect: an empty intersection is normalised, never negative");

    /* Touching edges share no pixels. Off by one here would double-draw a
       column on every clip boundary in the UI. */
    b = ar_rect_make(100, 0, 10, 100);
    r = ar_rect_intersect(a, b);
    CHECK(ar_rect_is_empty(r), "rect: rects that only touch do not intersect");
}

static void test_rect_union(void)
{
    ar_rect a, b, r;

    a = ar_rect_make(10, 10, 10, 10);
    b = ar_rect_make(50, 50, 10, 10);
    r = ar_rect_union(a, b);
    CHECK(ar__rect_eq(r, 10, 10, 50, 50), "rect: union spans both");

    /* The dirty region starts empty and is merged into every frame. If the
       empty case were not special-cased it would drag the region to the
       origin and the whole window would repaint forever. */
    r = ar_rect_union(ar_rect_make(0, 0, 0, 0), b);
    CHECK(ar__rect_eq(r, 50, 50, 10, 10), "rect: union with empty returns the other rect");
    r = ar_rect_union(a, ar_rect_make(900, 900, 0, 0));
    CHECK(ar__rect_eq(r, 10, 10, 10, 10), "rect: union with empty ignores the empty rect origin");
}

static void test_rect_contains(void)
{
    ar_rect r = ar_rect_make(10, 10, 5, 5);

    CHECK(ar_rect_contains(r, 10, 10), "rect: the top left corner is inside");
    CHECK(ar_rect_contains(r, 14, 14), "rect: the last pixel is inside");
    CHECK(!ar_rect_contains(r, 15, 14), "rect: one past the right edge is outside");
    CHECK(!ar_rect_contains(r, 9, 10), "rect: one before the left edge is outside");
}

/* ------------------------------------------------------------------------
 * Rasterizer
 * ------------------------------------------------------------------------ */

#define AR_TEST_W 16
#define AR_TEST_H 8

static ar_u32 g_pixels[AR_TEST_W * AR_TEST_H];

static ar_surface ar__test_surface(ar_color fill)
{
    ar_surface s;
    int        i;

    s.pixels = g_pixels;
    s.w = AR_TEST_W;
    s.h = AR_TEST_H;
    s.stride = AR_TEST_W;

    for (i = 0; i < AR_TEST_W * AR_TEST_H; ++i)
    {
        g_pixels[i] = fill;
    }
    return s;
}

static ar_u32 ar__px(ar_i32 x, ar_i32 y)
{
    return g_pixels[y * AR_TEST_W + x];
}

static ar_rect ar__whole(const ar_surface *s)
{
    return ar_rect_make(0, 0, s->w, s->h);
}

static void test_fill_opaque(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    ar_fill_rect(&s, ar_rect_make(2, 2, 4, 3), ar__whole(&s), AR_HEX(0x336699));

    CHECK(ar__px(2, 2) == AR_HEX(0x336699), "fill: the first pixel is written");
    CHECK(ar__px(5, 4) == AR_HEX(0x336699), "fill: the last pixel is written");
    CHECK(ar__px(6, 4) == AR_HEX(0x000000), "fill: one past the right edge is untouched");
    CHECK(ar__px(2, 5) == AR_HEX(0x000000), "fill: one past the bottom edge is untouched");
    CHECK(ar__px(1, 2) == AR_HEX(0x000000), "fill: one before the left edge is untouched");
}

static void test_fill_is_clipped(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    ar_fill_rect(&s, ar_rect_make(0, 0, AR_TEST_W, AR_TEST_H), ar_rect_make(4, 2, 3, 2),
                 AR_HEX(0xFFFFFF));

    CHECK(ar__px(4, 2) == AR_HEX(0xFFFFFF), "fill: drawing is confined to the clip");
    CHECK(ar__px(6, 3) == AR_HEX(0xFFFFFF), "fill: the clip last pixel is written");
    CHECK(ar__px(7, 3) == AR_HEX(0x000000), "fill: nothing escapes the clip on the right");
    CHECK(ar__px(4, 1) == AR_HEX(0x000000), "fill: nothing escapes the clip above");
}

static void test_fill_survives_negative_and_oversized_rects(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    /* A rect starting off screen with a size that runs past the far edge is
       what a scrolled container hands down constantly. Getting the clamp
       wrong here writes outside the buffer the OS owns. */
    ar_fill_rect(&s, ar_rect_make(-4, -4, 100, 100), ar__whole(&s), AR_HEX(0x00FF00));

    CHECK(ar__px(0, 0) == AR_HEX(0x00FF00), "fill: a rect starting off screen still fills");
    CHECK(ar__px(AR_TEST_W - 1, AR_TEST_H - 1) == AR_HEX(0x00FF00),
          "fill: a rect running past the far edge fills to the edge");

    s = ar__test_surface(AR_HEX(0x000000));
    ar_fill_rect(&s, ar_rect_make(-50, -50, 10, 10), ar__whole(&s), AR_HEX(0xFF0000));
    CHECK(ar__px(0, 0) == AR_HEX(0x000000), "fill: a rect entirely off screen draws nothing");

    ar_fill_rect(&s, ar_rect_make(2, 2, 0, 5), ar__whole(&s), AR_HEX(0xFF0000));
    CHECK(ar__px(2, 2) == AR_HEX(0x000000), "fill: a zero width rect draws nothing");
}

/* Channelwise distance, so a blend can be checked against the exactly rounded
   value with the one step of slack the fast path is documented to allow. */
static int ar__near(ar_u32 got, ar_u32 want, ar_i32 slack)
{
    ar_i32 shift;
    ar_i32 d;

    for (shift = 0; shift <= 16; shift += 8)
    {
        d = (ar_i32)((got >> shift) & 0xFFu) - (ar_i32)((want >> shift) & 0xFFu);
        if (d < 0)
        {
            d = -d;
        }
        if (d > slack)
        {
            return 0;
        }
    }
    return 1;
}

static void test_blend(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    /* Fully opaque must reproduce the source exactly. This is the whole
       reason the blend factor is stretched from 0..255 to 0..256: with a
       plain divide by 256 an opaque fill would come out one part in 256 too
       dark, and every opaque colour in the UI would be subtly wrong. */
    ar_fill_rect(&s, ar_rect_make(0, 0, 4, 4), ar__whole(&s), AR_RGBA(0x33, 0x66, 0x99, 0xFF));
    CHECK(ar__px(0, 0) == AR_HEX(0x336699), "blend: alpha 255 reproduces the source exactly");

    s = ar__test_surface(AR_HEX(0x000000));
    ar_fill_rect(&s, ar_rect_make(0, 0, 4, 4), ar__whole(&s), AR_RGBA(0x80, 0x40, 0x20, 0x80));
    CHECK(ar__px(0, 0) == AR_HEX(0x402010), "blend: half alpha over black halves each channel");

    s = ar__test_surface(AR_HEX(0xFFFFFF));
    ar_fill_rect(&s, ar_rect_make(0, 0, 4, 4), ar__whole(&s), AR_RGBA(0x00, 0x00, 0x00, 0x80));
    CHECK(ar__near(ar__px(0, 0), AR_HEX(0x7F7F7F), 1),
          "blend: half alpha black over white lands within one step of mid grey");

    s = ar__test_surface(AR_HEX(0x123456));
    ar_fill_rect(&s, ar_rect_make(0, 0, 4, 4), ar__whole(&s), AR_RGBA(0xFF, 0xFF, 0xFF, 0x00));
    CHECK(ar__px(0, 0) == AR_HEX(0x123456), "blend: alpha 0 leaves the destination alone");

    /* The result must stay opaque whatever the source alpha was, because the
       surface is handed to the OS as an opaque window. */
    s = ar__test_surface(AR_HEX(0x000000));
    ar_fill_rect(&s, ar_rect_make(0, 0, 4, 4), ar__whole(&s), AR_RGBA(0xFF, 0xFF, 0xFF, 0x40));
    CHECK(AR_ALPHA_OF(ar__px(0, 0)) == 0xFF, "blend: the result is always opaque");
}

/* The three properties the fast blend actually promises, checked across the
   whole alpha range rather than at one hand-picked value. */
static void test_blend_invariants_across_all_alphas(void)
{
    ar_surface s;
    ar_i32     alpha;
    ar_u32     src, dst, got, want;
    ar_i32     ends_exact = 1, no_drift = 1, within_one = 1;

    src = AR_HEX(0x336699);
    dst = AR_HEX(0xFEFBF2);

    for (alpha = 0; alpha <= 255; ++alpha)
    {
        ar_i32 shift;

        /* Blending a colour over itself must be a no-op, or every repainted
           hover state would creep towards white or black over time. */
        s = ar__test_surface(src);
        ar_fill_rect(&s, ar_rect_make(0, 0, 2, 2), ar__whole(&s),
                     (src & 0x00FFFFFFu) | ((ar_u32)alpha << 24));
        if (ar__px(0, 0) != src)
        {
            no_drift = 0;
        }

        s = ar__test_surface(dst);
        ar_fill_rect(&s, ar_rect_make(0, 0, 2, 2), ar__whole(&s),
                     (src & 0x00FFFFFFu) | ((ar_u32)alpha << 24));
        got = ar__px(0, 0);

        /* The exactly rounded reference, computed the slow honest way. */
        want = 0xFF000000u;
        for (shift = 0; shift <= 16; shift += 8)
        {
            ar_i32 sc = (ar_i32)((src >> shift) & 0xFFu);
            ar_i32 dc = (ar_i32)((dst >> shift) & 0xFFu);
            ar_i32 v = (sc * alpha + dc * (255 - alpha) + 127) / 255;
            want |= (ar_u32)v << shift;
        }

        if (!ar__near(got, want, 1))
        {
            within_one = 0;
        }
        if ((alpha == 0 || alpha == 255) && got != want)
        {
            ends_exact = 0;
        }
    }

    CHECK(ends_exact, "blend: alpha 0 and alpha 255 are exact, not approximate");
    CHECK(no_drift, "blend: a colour blended over itself never drifts, at any alpha");
    CHECK(within_one, "blend: every alpha lands within one step of the exact value");
}

static void test_clear(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    ar_surface_clear(&s, AR_HEX(0xFEFBF2));
    CHECK(ar__px(0, 0) == AR_HEX(0xFEFBF2), "clear: the first pixel is written");
    CHECK(ar__px(AR_TEST_W - 1, AR_TEST_H - 1) == AR_HEX(0xFEFBF2),
          "clear: the last pixel is written");
}

int main(void)
{
    printf("areole %s\n", ar_version());

    test_arena_alignment();
    test_arena_regions_do_not_overlap();
    test_arena_frame_reset();
    test_arena_exhaustion();
    test_arena_oversized_request_cannot_wrap();
    test_arena_rejects_nothing();

    test_rect_intersect();
    test_rect_union();
    test_rect_contains();

    test_fill_opaque();
    test_fill_is_clipped();
    test_fill_survives_negative_and_oversized_rects();
    test_blend();
    test_blend_invariants_across_all_alphas();
    test_clear();

    printf("\n%d checks, %d failed\n", ar__checks, ar__failures);
    return ar__failures == 0 ? 0 : 1;
}
