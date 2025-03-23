/*
 * areole - test harness.
 * SPDX-License-Identifier: MIT
 *
 * No framework, no fixtures. Every non-trivial piece of logic leaves one
 * runnable check here that fails if the logic breaks.
 */
#include "ar_internal.h"
#include "ar_path.h"
#include "ar_css.h"
#include "ar_node.h"

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

/* ------------------------------------------------------------------------
 * Font
 * ------------------------------------------------------------------------ */

static void test_text_metrics(void)
{
    ar_i32 one, two;

    CHECK(ar_text_width("", 1) == 0, "text: an empty string is zero wide");
    CHECK(ar_text_width(0, 1) == 0, "text: a null string is zero wide");
    CHECK(ar_text_height(1) == AR_FONT_H, "text: height at scale one is the face height");
    CHECK(ar_text_height(2) == AR_FONT_H * 2, "text: height scales with the scale");

    /* Line height must exceed the face, or two stacked lines touch and become
       unreadable the moment anything wraps. */
    CHECK(ar_text_line_height(1) > ar_text_height(1),
          "text: line height leaves room between lines");

    one = ar_text_width("Products", 1);
    two = ar_text_width("Products", 2);
    CHECK(one > 0, "text: a real string has width");
    CHECK(two == one * 2, "text: width scales exactly with the scale");

    /* Proportional spacing is the whole reason the generator measures ink
       extents. If this ever comes out equal, the advances have been replaced
       by a fixed cell and every label has gone monospaced. */
    CHECK(ar_text_width("i", 1) < ar_text_width("W", 1),
          "text: a narrow glyph advances less than a wide one");

    /* The width of several lines is the width of the widest, which is what a
       caller sizing a box around the text needs. */
    CHECK(ar_text_width("W\ni", 1) == ar_text_width("W", 1),
          "text: multi-line width is the widest line");

    /* An unmapped character is drawn as a question mark rather than dropped,
       so a mangled string looks mangled instead of looking like a layout bug. */
    CHECK(ar_text_width("\x01", 1) == ar_text_width("?", 1),
          "text: an out of range character measures as the fallback glyph");
}

static void test_text_pixels(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    /* The exclamation mark has ink in bits 3 and 4 of its top row, and its
       leftmost ink column is 2, so drawing at x zero must put those two
       pixels at x one and x two, flush against the origin. A glyph drawn from
       its cell instead of its ink would land two columns to the right. */
    ar_draw_text(&s, ar__whole(&s), 0, 0, "!", 1, AR_HEX(0xFFFFFF));
    CHECK(ar__px(0, 0) == AR_HEX(0x000000), "text: the glyph is flush against its own ink");
    CHECK(ar__px(1, 0) == AR_HEX(0xFFFFFF), "text: the first ink pixel is drawn");
    CHECK(ar__px(2, 0) == AR_HEX(0xFFFFFF), "text: the second ink pixel is drawn");
    CHECK(ar__px(3, 0) == AR_HEX(0x000000), "text: nothing is drawn past the ink");

    /* Row 5 of the exclamation mark is blank, and blank rows are skipped
       wholesale. */
    CHECK(ar__px(1, 5) == AR_HEX(0x000000), "text: a blank glyph row stays blank");

    s = ar__test_surface(AR_HEX(0x000000));
    ar_draw_text(&s, ar_rect_make(0, 0, 0, 0), 0, 0, "!", 1, AR_HEX(0xFFFFFF));
    CHECK(ar__px(1, 0) == AR_HEX(0x000000), "text: an empty clip draws nothing");

    s = ar__test_surface(AR_HEX(0x000000));
    ar_draw_text(&s, ar_rect_make(2, 0, 20, 20), 0, 0, "!", 1, AR_HEX(0xFFFFFF));
    CHECK(ar__px(1, 0) == AR_HEX(0x000000), "text: a glyph is clipped column by column");
    CHECK(ar__px(2, 0) == AR_HEX(0xFFFFFF), "text: the part inside the clip still draws");

    /* Drawing far off screen must be harmless, not a write past the buffer
       the operating system owns. */
    s = ar__test_surface(AR_HEX(0x000000));
    ar_draw_text(&s, ar__whole(&s), -400, -400, "areole", 3, AR_HEX(0xFFFFFF));
    ar_draw_text(&s, ar__whole(&s), 4000, 4000, "areole", 3, AR_HEX(0xFFFFFF));
    CHECK(ar__px(0, 0) == AR_HEX(0x000000), "text: drawing off screen touches nothing");
}

static void test_text_scaling(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    /* At scale two every ink pixel becomes a two by two block. */
    ar_draw_text(&s, ar__whole(&s), 0, 0, "!", 2, AR_HEX(0xFFFFFF));
    CHECK(ar__px(2, 0) == AR_HEX(0xFFFFFF), "text: scaled ink fills its block, top left");
    CHECK(ar__px(3, 1) == AR_HEX(0xFFFFFF), "text: scaled ink fills its block, bottom right");
    CHECK(ar__px(1, 0) == AR_HEX(0x000000), "text: scaled ink does not bleed left");

    /* A scale below one is a caller mistake, not a licence to divide by zero
       or draw nothing. */
    s = ar__test_surface(AR_HEX(0x000000));
    ar_draw_text(&s, ar__whole(&s), 0, 0, "!", 0, AR_HEX(0xFFFFFF));
    CHECK(ar__px(1, 0) == AR_HEX(0xFFFFFF), "text: a scale below one is clamped to one");
    CHECK(ar_text_width("!", -5) == ar_text_width("!", 1),
          "text: a negative scale measures as one");
}

/* ------------------------------------------------------------------------
 * Performance counters
 * ------------------------------------------------------------------------ */

static ar_perf g_perf;

/* Records one frame whose four phases take the given microseconds. */
static void ar__record(ar_perf *p, ar_u32 t, ar_u32 style, ar_u32 layout, ar_u32 raster,
                       ar_u32 present)
{
    ar_perf_begin(p, t);
    t += style;
    ar_perf_mark(p, AR_PHASE_STYLE, t);
    t += layout;
    ar_perf_mark(p, AR_PHASE_LAYOUT, t);
    t += raster;
    ar_perf_mark(p, AR_PHASE_RASTER, t);
    t += present;
    ar_perf_mark(p, AR_PHASE_PRESENT, t);
    ar_perf_end(p, t);
}

static void test_perf_phase_accounting(void)
{
    ar_perf_reset(&g_perf);
    ar__record(&g_perf, 1000, 3, 11, 40, 90);

    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_STYLE, 50) == 3, "perf: style time is recorded");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_LAYOUT, 50) == 11, "perf: layout time is recorded");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) == 40, "perf: raster time is recorded");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_PRESENT, 50) == 90,
          "perf: present time is recorded");

    /* The total is measured across the frame rather than summed from the
       phases, so it also catches whatever happens between them. */
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_COUNT, 50) == 3 + 11 + 40 + 90,
          "perf: the total spans the whole frame");
}

static void test_perf_percentiles(void)
{
    ar_u32 i;

    /* One hundred frames whose totals are 0 through 99, fed in an order that
       is neither sorted nor reversed, so a percentile that quietly returned
       the newest or oldest sample would show up here. */
    ar_perf_reset(&g_perf);
    for (i = 0; i < 100; ++i)
    {
        ar_u32 v = (i * 37u) % 100u;
        ar__record(&g_perf, i * 1000u, 0, 0, v, 0);
    }

    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 0) == 0, "perf: p0 is the minimum");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) == 50, "perf: p50 is the median");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 100) == 99, "perf: p100 is the maximum");
    CHECK(ar_perf_max(&g_perf, AR_PHASE_RASTER) == 99, "perf: max agrees with p100");

    /* Nearest rank, pinned deliberately. With a hundred samples the ninety
       ninth percentile is the ninety ninth of them, which is index 98 and
       therefore the value 98. Publishing percentiles is worthless if the
       definition drifts between releases, so it is asserted rather than
       described. */
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 99) == 98,
          "perf: p99 of a hundred samples is the ninety ninth of them");

    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 0) <=
                  ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) &&
              ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) <=
                  ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 90) &&
              ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 90) <=
                  ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 99) &&
              ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 99) <=
                  ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 100),
          "perf: percentiles rise monotonically");

    /* A percentile above 100 is a caller mistake, not a reason to read past
       the end of the ring. */
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 5000) == 99,
          "perf: an out of range percentile is clamped");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) ==
              ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50),
          "perf: reading a percentile does not disturb the ring");
}

static void test_perf_empty(void)
{
    ar_perf_reset(&g_perf);
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 50) == 0,
          "perf: percentiles of nothing are zero, not a read of uninitialised memory");
    CHECK(ar_perf_max(&g_perf, AR_PHASE_COUNT) == 0, "perf: max of nothing is zero");
}

static void test_perf_ring_wraps(void)
{
    ar_u32 i;

    /* Twice the ring plus a bit. Everything before the last AR_PERF_RING
       frames must be gone, or a stall from a minute ago would keep showing up
       in p99 forever and nobody would trust the readout. */
    ar_perf_reset(&g_perf);
    for (i = 0; i < AR_PERF_RING * 2 + 7; ++i)
    {
        ar__record(&g_perf, i * 1000u, 0, 0, i < AR_PERF_RING ? 9999u : 10u, 0);
    }

    CHECK(g_perf.count == AR_PERF_RING, "perf: the ring saturates at its capacity");
    CHECK(g_perf.frames == AR_PERF_RING * 2 + 7, "perf: the lifetime frame count keeps counting");
    CHECK(ar_perf_max(&g_perf, AR_PHASE_RASTER) == 10,
          "perf: samples older than the ring are gone, not merely hidden");
}

static void test_perf_survives_a_clock_wrap(void)
{
    ar_perf_reset(&g_perf);

    /* A microsecond counter in 32 bits wraps after about 71 minutes. Left
       unhandled, the one frame that straddles the wrap reports roughly four
       thousand seconds and drags every percentile with it for the next 256
       frames. */
    ar_perf_begin(&g_perf, 0xFFFFFF00u);
    ar_perf_mark(&g_perf, AR_PHASE_RASTER, 0x00000040u);
    ar_perf_end(&g_perf, 0x00000080u);

    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_RASTER, 100) == 0,
          "perf: a phase that straddles a clock wrap reports zero, not a spike");
    CHECK(ar_perf_percentile(&g_perf, AR_PHASE_COUNT, 100) == 0,
          "perf: a frame that straddles a clock wrap reports zero, not a spike");
}

static void test_perf_overlay_draws_and_clips(void)
{
    ar_surface s = ar__test_surface(AR_HEX(0x000000));

    ar_perf_reset(&g_perf);
    ar__record(&g_perf, 0, 1, 2, 3, 4);

    /* The overlay is a widget like any other: it must respect the clip and
       must not write outside the surface when placed off the edge. */
    ar_perf_overlay(&g_perf, &s, ar__whole(&s), 0, 0, 1);
    CHECK(ar__px(0, 0) != AR_HEX(0x000000), "perf: the overlay draws its panel");

    s = ar__test_surface(AR_HEX(0x000000));
    ar_perf_overlay(&g_perf, &s, ar__whole(&s), -500, -500, 1);
    ar_perf_overlay(&g_perf, &s, ar__whole(&s), 9000, 9000, 2);
    CHECK(ar__px(0, 0) == AR_HEX(0x000000), "perf: an overlay placed off screen touches nothing");
}

/* ------------------------------------------------------------------------
 * CSS
 * ------------------------------------------------------------------------ */

static ar_rule  g_rules[64];
static ar_sheet g_sheet;

static void ar__sheet(const char *css)
{
    ar_sheet_init(&g_sheet, g_rules, 64);
    ar_sheet_parse(&g_sheet, css);
}

/* Resolves for a box described the way ar_begin will describe one. */
static ar_style ar__resolve(const char *selector, ar_u8 state)
{
    ar_style st;
    ar_u32   tag = 0, klass = 0, id = 0;

    ar_selector_split(selector, &tag, &klass, &id);
    ar_sheet_resolve(&g_sheet, tag, klass, id, state, &st);
    return st;
}

/* C90 forbids subscripting an array inside a value that is not an lvalue, so
   the resolved style cannot be indexed straight off the return. Rather than
   scatter a temporary through every assertion, reading one property gets an
   accessor of its own. The strict gate caught this; a compiler with
   extensions on would have waved it through and broken on the first old
   toolchain that saw it. */
static ar_i32 ar__css_value(const char *selector, ar_u8 state, ar_prop prop)
{
    ar_style st = ar__resolve(selector, state);
    return st.v[prop];
}

static void test_css_basic(void)
{
    ar__sheet(".card { width: 200px; height: 132px; background: #f8f3e9; }");

    CHECK(g_sheet.errors == 0, "css: a well formed rule parses without error");
    CHECK(g_sheet.count == 1, "css: one rule is stored");

    {
        ar_style st = ar__resolve(".card", AR_STATE_NONE);
        CHECK(st.v[AR_P_WIDTH] == 200 && st.unit[AR_P_WIDTH] == AR_UNIT_PX,
              "css: a pixel width parses");
        CHECK(st.v[AR_P_HEIGHT] == 132, "css: a pixel height parses");
        CHECK((ar_u32)st.v[AR_P_BACKGROUND] == 0xFFF8F3E9u, "css: a six digit hex colour parses");
    }

    /* A box that matches nothing keeps the defaults, rather than inheriting
       whatever the last resolved box happened to have. */
    {
        ar_style st = ar__resolve(".nothing", AR_STATE_NONE);
        CHECK(st.unit[AR_P_WIDTH] == AR_UNIT_AUTO, "css: an unmatched box sizes to its content");
        CHECK((ar_u32)st.v[AR_P_BACKGROUND] == 0u, "css: an unmatched box has no background");
    }
}

static void test_css_units(void)
{
    ar__sheet(".a { width: 50%; height: auto; }"
              ".b { width: grow; gap: 8; }"
              ".c { width: 12.75px; }");

    CHECK(g_sheet.errors == 0, "css: units parse without error");

    {
        ar_style a = ar__resolve(".a", AR_STATE_NONE);
        ar_style b = ar__resolve(".b", AR_STATE_NONE);
        ar_style c = ar__resolve(".c", AR_STATE_NONE);

        CHECK(a.unit[AR_P_WIDTH] == AR_UNIT_PCT && a.v[AR_P_WIDTH] == 50, "css: per cent parses");
        CHECK(a.unit[AR_P_HEIGHT] == AR_UNIT_AUTO, "css: auto parses");
        CHECK(b.unit[AR_P_WIDTH] == AR_UNIT_GROW, "css: grow parses");

        /* A bare number is pixels. Requiring the unit everywhere would be
           pedantry in a stylesheet nobody validates. */
        CHECK(b.v[AR_P_GAP] == 8 && b.unit[AR_P_GAP] == AR_UNIT_PX,
              "css: a unitless length is pixels");

        /* The layout is integer end to end, so a fraction is floored at parse
           time rather than carried and rounded inconsistently later. */
        CHECK(c.v[AR_P_WIDTH] == 12, "css: a fractional length is floored");
    }
}

static void test_css_colors(void)
{
    ar__sheet(".s { background: #abc; }"
              ".l { background: #11223344; }"
              ".t { background: transparent; }");

    CHECK((ar_u32)ar__css_value(".s", 0, AR_P_BACKGROUND) == 0xFFAABBCCu,
          "css: three digit hex expands each digit");
    CHECK((ar_u32)ar__css_value(".l", 0, AR_P_BACKGROUND) == 0x44112233u,
          "css: eight digit hex carries alpha");
    CHECK((ar_u32)ar__css_value(".t", 0, AR_P_BACKGROUND) == 0u, "css: transparent is zero alpha");
}

static void test_css_specificity(void)
{
    /* Written deliberately in the wrong order: the id rule comes first, so
       source order alone would give the wrong answer. */
    ar__sheet("#one { color: #111111; }"
              ".btn { color: #222222; }"
              "div  { color: #333333; }");

    CHECK((ar_u32)ar__css_value("div.btn#one", 0, AR_P_COLOR) == 0xFF111111u,
          "css: an id beats a class and a tag whatever the source order");
    CHECK((ar_u32)ar__css_value("div.btn", 0, AR_P_COLOR) == 0xFF222222u,
          "css: a class beats a tag");
    CHECK((ar_u32)ar__css_value("div", 0, AR_P_COLOR) == 0xFF333333u,
          "css: a tag applies when nothing more specific matches");
}

static void test_css_source_order_breaks_ties(void)
{
    ar__sheet(".a { color: #111111; }"
              ".a { color: #222222; }");

    CHECK((ar_u32)ar__css_value(".a", 0, AR_P_COLOR) == 0xFF222222u,
          "css: between rules of equal specificity the later one wins");
}

static void test_css_pseudo_states(void)
{
    ar__sheet(".btn { background: #333333; }"
              ".btn:hover { background: #444444; }"
              ".btn:active { background: #555555; }");

    CHECK((ar_u32)ar__css_value(".btn", AR_STATE_NONE, AR_P_BACKGROUND) == 0xFF333333u,
          "css: the base rule applies when the box is idle");
    CHECK((ar_u32)ar__css_value(".btn", AR_STATE_HOVER, AR_P_BACKGROUND) == 0xFF444444u,
          "css: hover overrides the base rule");
    CHECK((ar_u32)ar__css_value(".btn", AR_STATE_ACTIVE, AR_P_BACKGROUND) == 0xFF555555u,
          "css: active overrides the base rule");

    /* Held down while hovered is both states at once, which is what actually
       happens with a mouse. The later rule of equal specificity wins. */
    CHECK((ar_u32)ar__css_value(".btn", (ar_u8)(AR_STATE_HOVER | AR_STATE_ACTIVE),
                                AR_P_BACKGROUND) == 0xFF555555u,
          "css: a box that is both hovered and active takes the later rule");
}

static void test_css_shorthands(void)
{
    ar__sheet(".one  { padding: 12px; }"
              ".two  { padding: 6px 12px; }"
              ".four { padding: 1px 2px 3px 4px; }"
              ".m    { margin: 5px 10px 15px; }");

    {
        ar_style a = ar__resolve(".one", 0);
        CHECK(a.v[AR_P_PAD_TOP] == 12 && a.v[AR_P_PAD_RIGHT] == 12 && a.v[AR_P_PAD_BOTTOM] == 12 &&
                  a.v[AR_P_PAD_LEFT] == 12,
              "css: one padding value covers every side");
    }
    {
        ar_style a = ar__resolve(".two", 0);
        CHECK(a.v[AR_P_PAD_TOP] == 6 && a.v[AR_P_PAD_BOTTOM] == 6, "css: two values set vertical");
        CHECK(a.v[AR_P_PAD_RIGHT] == 12 && a.v[AR_P_PAD_LEFT] == 12,
              "css: two values set horizontal");
    }
    {
        ar_style a = ar__resolve(".four", 0);
        CHECK(a.v[AR_P_PAD_TOP] == 1 && a.v[AR_P_PAD_RIGHT] == 2 && a.v[AR_P_PAD_BOTTOM] == 3 &&
                  a.v[AR_P_PAD_LEFT] == 4,
              "css: four values run clockwise from the top");
    }
    {
        /* Three values leave the left mirroring the right, as CSS does. */
        ar_style a = ar__resolve(".m", 0);
        CHECK(a.v[AR_P_MARGIN_TOP] == 5 && a.v[AR_P_MARGIN_RIGHT] == 10 &&
                  a.v[AR_P_MARGIN_BOTTOM] == 15 && a.v[AR_P_MARGIN_LEFT] == 10,
              "css: three values mirror the left onto the right");
    }
}

static void test_css_border_shorthand(void)
{
    ar__sheet(".b { border: 1px solid #e8dfcc; border-radius: 8px; }");

    {
        ar_style a = ar__resolve(".b", 0);
        /* The word "solid" carries no information here, but writing it is
           reflex. Rejecting the declaration over it would be obnoxious. */
        CHECK(g_sheet.errors == 0, "css: an unstyleable border keyword is tolerated");
        CHECK(a.v[AR_P_BORDER_WIDTH] == 1, "css: the border width is taken from the shorthand");
        CHECK((ar_u32)a.v[AR_P_BORDER_COLOR] == 0xFFE8DFCCu,
              "css: the border colour is taken from the shorthand");
        CHECK(a.v[AR_P_BORDER_RADIUS] == 8, "css: the radius parses");
    }
}

static void test_css_keywords(void)
{
    ar__sheet(".f { display: flex; flex-direction: column; "
              "     justify-content: space-between; align-items: center; overflow: hidden; }");

    {
        ar_style a = ar__resolve(".f", 0);
        CHECK(a.v[AR_P_DISPLAY] == AR_DISPLAY_FLEX, "css: display parses");
        CHECK(a.v[AR_P_DIRECTION] == AR_DIR_COLUMN, "css: flex-direction parses");
        CHECK(a.v[AR_P_JUSTIFY] == AR_JUSTIFY_BETWEEN, "css: justify-content parses");
        CHECK(a.v[AR_P_ALIGN] == AR_ALIGN_CENTER, "css: align-items parses");
        CHECK(a.v[AR_P_OVERFLOW] == AR_OVERFLOW_HIDDEN, "css: overflow parses");
    }

    /* The same word means different things to different properties, so the
       keyword table is looked up per property rather than globally. */
    ar__sheet(".g { justify-content: center; align-items: end; }");
    CHECK(ar__css_value(".g", 0, AR_P_JUSTIFY) == AR_JUSTIFY_CENTER,
          "css: center resolves against justify-content");
    CHECK(ar__css_value(".g", 0, AR_P_ALIGN) == AR_ALIGN_END,
          "css: end resolves against align-items");
}

static void test_css_comments_and_whitespace(void)
{
    ar__sheet("/* a leading comment */\n"
              ".a /* between */ {\n"
              "    width : 10px ; /* trailing */\n"
              "}\n"
              "/* unterminated at the end of input");

    CHECK(ar__css_value(".a", 0, AR_P_WIDTH) == 10,
          "css: comments and stray whitespace are ignored");
}

static void test_css_survives_malformed_input(void)
{
    /* The point of this test: a stylesheet is written by hand and will have
       mistakes in it. One bad rule must cost one rule, not the remainder of
       the file. The good rule is last on purpose. */
    ar__sheet(".broken { width: }"
              "@media screen { }"
              ".unknown-prop { flibbertigibbet: 3px; }"
              ".good { width: 42px; }");

    CHECK(g_sheet.errors > 0, "css: malformed input is reported rather than ignored");
    CHECK(ar__css_value(".good", 0, AR_P_WIDTH) == 42,
          "css: a rule after a broken one still parses");

    ar__sheet("");
    CHECK(g_sheet.count == 0 && g_sheet.errors == 0, "css: an empty stylesheet is not an error");

    ar__sheet(0);
    CHECK(g_sheet.count == 0, "css: a null stylesheet is not a crash");

    ar__sheet(".a { }");
    CHECK(g_sheet.count == 0 && g_sheet.errors == 0,
          "css: an empty block is legal and has no effect");
}

/* The parser must terminate on any input at all.
 *
 * This is here because it did not. An unknown property left the cursor
 * sitting on the semicolon it had scanned up to, the block loop called the
 * declaration parser again, that parser could not consume a semicolon either,
 * and the two spun forever. A wrong colour is a bug; a hang on a stylesheet
 * with a typo in it is a different category of bug, and the only durable
 * defence is a rule that every path consumes at least one character.
 *
 * If one of these ever hangs, the harness never prints a result, which is a
 * loud enough failure. */
static void test_css_always_terminates(void)
{
    static const char *const NASTY[] = {";",
                                        ".a { ; }",
                                        ".a { ;;;;;;; }",
                                        ".a { : }",
                                        ".a { unknown: 1px; width: 5px; }",
                                        ".a { unknown: 1px }",
                                        ".a { width",
                                        ".a {",
                                        ".a",
                                        "{ }",
                                        "}",
                                        "}}}}{{{{",
                                        "@media screen { .a { width: 1px; } }",
                                        ".a { width: ; height: 3px; }",
                                        ".a { width: !!! ; }",
                                        ".a { background: #zz; }",
                                        ".a { background: #; }",
                                        ".a { padding: 1px 2px 3px 4px 5px 6px; }",
                                        "/*",
                                        "/* .a { width: 1px; }",
                                        "....",
                                        "###",
                                        ":::",
                                        ".a:nonsense { width: 1px; }",
                                        "ÿþ"};
    ar_u32                   i;

    for (i = 0; i < sizeof NASTY / sizeof NASTY[0]; ++i)
    {
        ar__sheet(NASTY[i]);
    }
    CHECK(1, "css: the parser terminates on every malformed input in the list");

    /* And the recovery is not merely survival: a good rule after a bad one is
       still picked up. */
    ar__sheet(".a { unknown: 1px; } .b { width: 7px; }");
    CHECK(ar__css_value(".b", 0, AR_P_WIDTH) == 7,
          "css: a rule following an unknown property still parses");

    ar__sheet(".a { ; width: 9px; }");
    CHECK(ar__css_value(".a", 0, AR_P_WIDTH) == 9,
          "css: a stray semicolon does not swallow the declaration after it");
}

static void test_css_capacity(void)
{
    ar_rule  few[2];
    ar_sheet s;

    ar_sheet_init(&s, few, 2);
    ar_sheet_parse(&s, ".a{width:1px;} .b{width:2px;} .c{width:3px;}");

    /* Running out of room is reported, not silently truncated, and above all
       is not a write past the end of the array. */
    CHECK(s.count == 2, "css: rules stop at capacity");
    CHECK(s.errors > 0, "css: exceeding capacity is reported");
}

static void test_selector_split(void)
{
    ar_u32 tag, klass, id;

    CHECK(ar_selector_split("div.card#first", &tag, &klass, &id),
          "selector: a full selector splits");
    CHECK(tag == ar_hash("div", 3), "selector: the tag is extracted");
    CHECK(klass == ar_hash("card", 4), "selector: the class is extracted");
    CHECK(id == ar_hash("first", 5), "selector: the id is extracted");

    CHECK(ar_selector_split(".card", &tag, &klass, &id), "selector: a bare class splits");
    CHECK(tag == 0 && id == 0, "selector: absent parts come back as zero");

    CHECK(!ar_selector_split("", &tag, &klass, &id), "selector: an empty selector is rejected");
    CHECK(!ar_selector_split(".", &tag, &klass, &id), "selector: a lone dot is rejected");
    CHECK(!ar_selector_split(0, &tag, &klass, &id), "selector: a null selector is rejected");

    /* Zero is the wildcard in a rule, so no real identifier may hash to it. */
    CHECK(ar_hash("", 0) != 0, "selector: no identifier hashes to the wildcard");
}

/* ------------------------------------------------------------------------
 * Layout
 *
 * Every expectation here is a rectangle worked out by hand from the
 * stylesheet. That is deliberate: a layout engine checked against its own
 * output is checked against nothing.
 * ------------------------------------------------------------------------ */

#define AR_LAY_MAX 512

static unsigned char g_ui_mem[AR_MEM(256)];
static ar_u32        g_ui_pixels[AR_LAY_MAX * 64];
static ar_ctx       *g_ui;

static ar_surface ar__ui_surface(ar_i32 w, ar_i32 h)
{
    ar_surface s;
    s.pixels = g_ui_pixels;
    s.w = w;
    s.h = h;
    s.stride = AR_LAY_MAX;
    return s;
}

/* Fresh context, so one test cannot leave state behind for the next. */
static void ar__ui_reset(const char *css)
{
    g_ui = ar_init(g_ui_mem, (ar_u32)sizeof g_ui_mem);
    if (g_ui)
    {
        ar_stylesheet(g_ui, css);
    }
}

static void ar__ui_begin(void)
{
    ar_input in;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
}

static ar_rect ar__box(ar_i32 index)
{
    if (!g_ui || index < 0 || index >= g_ui->node_count)
    {
        return ar_rect_make(-1, -1, -1, -1);
    }
    return g_ui->nodes[index].rect;
}

static int ar__box_is(ar_i32 index, ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h)
{
    ar_rect r = ar__box(index);
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

static void test_layout_row_with_gap_and_padding(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    ar__ui_reset("#root { display:flex; flex-direction:row; gap:10px; padding:5px; }"
                 ".a { width:50px; height:20px; }");
    CHECK(g_ui != 0, "layout: a context is created");

    ar__ui_begin();
    ar_begin(g_ui, "div#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(!ar_unbalanced(g_ui) && !ar_overflowed(g_ui), "layout: the frame is balanced");
    CHECK(ar__box_is(0, 0, 0, 400, 300), "layout: the root takes the viewport");

    /* Padding pushes the first child in, the gap separates the rest, and none
       of it accumulates: the third child sits at 5 + 2 * (50 + 10). */
    CHECK(ar__box_is(1, 5, 5, 50, 20), "layout: the first child clears the padding");
    CHECK(ar__box_is(2, 65, 5, 50, 20), "layout: the gap separates siblings");
    CHECK(ar__box_is(3, 125, 5, 50, 20), "layout: gaps do not accumulate");
}

static void test_layout_column(void)
{
    ar_surface s = ar__ui_surface(200, 400);

    ar__ui_reset("#root { display:flex; flex-direction:column; gap:8px; }"
                 ".a { width:40px; height:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 40, 30), "layout: a column starts at the origin");
    CHECK(ar__box_is(2, 0, 38, 40, 30), "layout: a column stacks downwards with the gap");
}

static void test_layout_grow_splits_the_remainder(void)
{
    ar_surface s = ar__ui_surface(100, 50);

    ar__ui_reset("#root { display:flex; flex-direction:row; }"
                 ".g { width:grow; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".g");
    ar_end(g_ui);
    ar_begin(g_ui, ".g");
    ar_end(g_ui);
    ar_begin(g_ui, ".g");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* A hundred pixels across three boxes is 33 and a third each. Dividing and
       discarding the remainder leaves a one pixel strip at the right hand
       edge that people notice and nobody can explain, so the remainder is
       handed out a pixel at a time to the boxes at the front. */
    CHECK(ar__box_is(1, 0, 0, 34, 10), "layout: the first grower takes the spare pixel");
    CHECK(ar__box_is(2, 34, 0, 33, 10), "layout: the second grower takes its share");
    CHECK(ar__box_is(3, 67, 0, 33, 10), "layout: the third grower takes its share");

    {
        ar_rect last = ar__box(3);
        CHECK(last.x + last.w == 100, "layout: growers meet the far edge exactly");
    }
}

static void test_layout_grow_alongside_fixed(void)
{
    ar_surface s = ar__ui_surface(300, 50);

    ar__ui_reset("#root { display:flex; flex-direction:row; gap:10px; }"
                 ".fixed { width:80px; height:10px; }"
                 ".g { width:grow; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".fixed");
    ar_end(g_ui);
    ar_begin(g_ui, ".g");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* 300 less the fixed 80 less the 10 gap leaves 210. */
    CHECK(ar__box_is(1, 0, 0, 80, 10), "layout: a fixed box keeps its width beside a grower");
    CHECK(ar__box_is(2, 90, 0, 210, 10), "layout: a grower takes what the fixed box left");
}

static void test_layout_percent(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:flex; padding:20px; }"
                 ".half { width:50%; height:25%; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".half");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* Per cent is of the parent inner box, not its border box. 400 less 40 of
       padding is 360, half of which is 180; 200 less 40 is 160, a quarter of
       which is 40. */
    CHECK(ar__box_is(1, 20, 20, 180, 40), "layout: per cent is of the parent inner box");
}

static void test_layout_auto_sizes_to_content(void)
{
    ar_surface s = ar__ui_surface(400, 200);
    ar_i32     tw;

    /* align-items defaults to stretch, as in CSS, so the root is told not to
       stretch here: this test is about intrinsic sizing, not about alignment
       overriding it. */
    ar__ui_reset("#root { display:flex; align-items:flex-start; }"
                 ".box { padding:6px; }"
                 ".label { font-size:8px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".box");
    ar_text(g_ui, ".label", "Products");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    tw = ar_text_width("Products", 1);

    /* A box that states no size is exactly its content plus its own padding.
       This is what lets a stylesheet stay silent about most dimensions. */
    CHECK(ar__box_is(1, 0, 0, tw + 12, ar_text_height(1) + 12),
          "layout: a box with no stated size wraps its content and padding");
    CHECK(ar__box_is(2, 6, 6, tw, ar_text_height(1)),
          "layout: the text sits inside the padding at its own size");
}

static void test_layout_justify_content(void)
{
    ar_surface s = ar__ui_surface(400, 50);

    ar__ui_reset("#c { display:flex; flex-direction:row; justify-content:center; }"
                 "#e { display:flex; flex-direction:row; justify-content:flex-end; }"
                 "#b { display:flex; flex-direction:row; justify-content:space-between; }"
                 ".a { width:100px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#c");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).x == 150, "layout: justify-content center splits the leftover evenly");

    ar__ui_begin();
    ar_begin(g_ui, "#e");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).x == 300, "layout: justify-content flex-end pushes to the far edge");

    ar__ui_begin();
    ar_begin(g_ui, "#b");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).x == 0, "layout: space-between leaves the first box alone");
    CHECK(ar__box(2).x == 300, "layout: space-between pushes the last box to the edge");
}

static void test_layout_align_items(void)
{
    ar_surface s = ar__ui_surface(400, 100);

    ar__ui_reset("#c { display:flex; flex-direction:row; align-items:center; }"
                 "#e { display:flex; flex-direction:row; align-items:flex-end; }"
                 "#s { display:flex; flex-direction:row; align-items:stretch; }"
                 ".a { width:50px; height:20px; }"
                 ".noheight { width:50px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#c");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).y == 40, "layout: align-items center centres on the cross axis");

    ar__ui_begin();
    ar_begin(g_ui, "#e");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).y == 80, "layout: align-items flex-end sits on the far edge");

    /* Stretch only applies to boxes that have not been given a size of their
       own, which is what makes it safe as a container default. */
    ar__ui_begin();
    ar_begin(g_ui, "#s");
    ar_begin(g_ui, ".noheight");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar__box(1).h == 100, "layout: stretch fills the cross axis when no size is stated");
    CHECK(ar__box(2).h == 20, "layout: stretch leaves a stated size alone");
}

static void test_layout_min_and_max(void)
{
    ar_surface s = ar__ui_surface(1000, 100);

    ar__ui_reset("#root { display:flex; flex-direction:row; }"
                 ".capped { width:grow; max-width:120px; height:10px; }"
                 ".floored { width:10px; min-width:60px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".capped");
    ar_end(g_ui);
    ar_begin(g_ui, ".floored");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == 120, "layout: max-width caps a grower");
    CHECK(ar__box(2).w == 60, "layout: min-width raises a stated size");
}

static void test_layout_display_none(void)
{
    ar_surface s = ar__ui_surface(400, 50);

    ar__ui_reset("#root { display:flex; flex-direction:row; gap:10px; }"
                 ".a { width:50px; height:10px; }"
                 ".gone { display:none; width:50px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_begin(g_ui, ".gone");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* A hidden box takes no room and, just as importantly, leaves no gap
       behind it. Getting the second part wrong is the classic way a toggled
       panel leaves a hole where it used to be. */
    CHECK(ar__box(1).x == 0, "layout: the box before a hidden one is unaffected");
    CHECK(ar__box(2).w == 0, "layout: a hidden box has no size");
    CHECK(ar__box(3).x == 60, "layout: a hidden box leaves no gap behind it");
}

static void test_layout_nesting(void)
{
    ar_surface s = ar__ui_surface(500, 300);

    ar__ui_reset(".app { display:flex; flex-direction:row; }"
                 ".rail { width:120px; padding:10px; display:flex; flex-direction:column; }"
                 ".page { width:grow; padding:20px; }"
                 ".item { width:grow; height:24px; }");

    ar__ui_begin();
    ar_begin(g_ui, "div.app");
    ar_begin(g_ui, "div.rail");
    ar_begin(g_ui, "div.item");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.page");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The rail states a width and no height, so it keeps the one and takes the
       full column from align-items: stretch. */
    CHECK(ar__box_is(1, 0, 0, 120, 300), "layout: a fixed rail keeps its width and stretches");

    /* Inside the rail the axes swap: it is a column, so the item takes its
       stated height on the main axis and grows across the rail inner width. */
    CHECK(ar__box_is(2, 10, 10, 100, 24), "layout: a nested box works inside the parent padding");
    CHECK(ar__box_is(3, 120, 0, 380, 300), "layout: the page takes the rest of the row");
}

static void test_layout_survives_abuse(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; }");

    /* An unbalanced tree is a caller bug. It must be reported, and it must not
       be a crash or a corrupted tree. */
    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".a");
    ar_frame_end(g_ui, &s);
    CHECK(ar_unbalanced(g_ui), "layout: a missing ar_end is reported");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    CHECK(ar_unbalanced(g_ui), "layout: a surplus ar_end is reported");

    /* An empty frame draws nothing and reports nothing wrong. */
    ar__ui_begin();
    ar_frame_end(g_ui, &s);
    CHECK(!ar_unbalanced(g_ui), "layout: an empty frame is not an error");

    /* A tree far deeper than the stack must stop rather than run off it. */
    ar__ui_begin();
    {
        ar_i32 i;
        for (i = 0; i < AR_MAX_DEPTH + 50; ++i)
        {
            ar_begin(g_ui, ".a");
        }
        for (i = 0; i < AR_MAX_DEPTH + 50; ++i)
        {
            ar_end(g_ui);
        }
    }
    ar_frame_end(g_ui, &s);
    CHECK(ar_unbalanced(g_ui), "layout: exceeding the depth limit is reported, not scribbled");
}

static void test_layout_is_stable_across_frames(void)
{
    ar_surface s = ar__ui_surface(400, 200);
    ar_rect    first, second;

    ar__ui_reset("#root { display:flex; gap:4px; padding:7px; }"
                 ".a { width:grow; height:33px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    first = ar__box(1);

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_begin(g_ui, ".a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    second = ar__box(1);

    /* The tree is thrown away and rebuilt every frame. If anything survived
       that should not have, the same declarations would drift. */
    CHECK(first.x == second.x && first.y == second.y && first.w == second.w && first.h == second.h,
          "layout: rebuilding the same tree gives the same rectangles");

    /* And the frame arena is genuinely released, not merely rewound past. */
    CHECK(ar_arena_frame_used(&g_ui->arena) == ar_arena_frame_peak(&g_ui->arena),
          "layout: a frame uses exactly the arena it used last time");
}


/* ------------------------------------------------------------------------
 * Damage tracking
 *
 * The one that matters is the last: what damage tracking draws must be what a
 * full repaint would have drawn, pixel for pixel. Everything else here is a
 * property that makes the saving real; that one is the property that makes it
 * correct, and a stale strip of pixels is invisible until it is embarrassing.
 * ------------------------------------------------------------------------ */
#define AR_DMG_W 256
#define AR_DMG_H 128

static ar_u32 g_dmg_a[AR_DMG_W * AR_DMG_H];
static ar_u32 g_dmg_b[AR_DMG_W * AR_DMG_H];

static ar_surface ar__dmg_surface(ar_u32 *px)
{
    ar_surface s;
    s.pixels = px;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;
    return s;
}

static const char *const DMG_CSS = "#root { display:flex; flex-direction:column; gap:4px;"
                                   "        padding:6px; background:#101014; }"
                                   ".a { width:grow; height:20px; background:#3a4a5a; }"
                                   ".b { width:grow; height:20px; background:#8a2a2a; }";

/* One frame of a two box interface. Which class the second box uses is the
   only thing the caller varies, so a difference in output can only come from
   that. */
static ar_rect ar__dmg_frame(ar_surface *s, int second_is_b, ar_i32 mouse_x, ar_i32 mouse_y)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = mouse_x;
    in.mouse_y = mouse_y;
    in.mouse_inside = (mouse_x >= 0);
    ar_frame_begin(g_ui, &in);

    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, second_is_b ? "div.b" : "div.a");
    ar_end(g_ui);
    ar_end(g_ui);

    return ar_frame_end(g_ui, s);
}

static void test_damage_first_frame_paints_everything(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    ar__ui_reset(DMG_CSS);
    d = ar__dmg_frame(&s, 0, -1, -1);

    /* Nothing is known about the surface on the first frame, so nothing may be
       assumed unchanged. */
    CHECK(d.w == AR_DMG_W && d.h == AR_DMG_H,
          "damage: the first frame repaints the whole surface");
}

static void test_damage_an_unchanged_frame_paints_nothing(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    ar__ui_reset(DMG_CSS);
    ar__dmg_frame(&s, 0, -1, -1);
    d = ar__dmg_frame(&s, 0, -1, -1);

    CHECK(d.w == 0 || d.h == 0, "damage: redeclaring the same tree damages nothing");
}

static void test_damage_a_style_change_repaints_that_box(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    ar__ui_reset(DMG_CSS);
    ar__dmg_frame(&s, 0, -1, -1);
    ar__dmg_frame(&s, 0, -1, -1);
    d = ar__dmg_frame(&s, 1, -1, -1);

    /* Same geometry, different background. Geometry alone cannot see this,
       which is the entire reason a style digest is stored per box. */
    CHECK(d.w > 0 && d.h > 0, "damage: a box that only changed colour is still repainted");
    CHECK(d.h < AR_DMG_H, "damage: and only that box, not the whole surface");
}

static void test_damage_invalidate_is_honoured(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    ar__ui_reset(DMG_CSS);
    ar__dmg_frame(&s, 0, -1, -1);
    ar__dmg_frame(&s, 0, -1, -1);

    /* An application invalidating its own data, which the library cannot see. */
    ar_frame_begin(g_ui, 0);
    ar_invalidate(g_ui, ar_rect_make(10, 10, 30, 30));
    CHECK(ar_frame_is_dirty(g_ui), "damage: ar_invalidate marks the frame dirty at once");
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    d = ar_frame_end(g_ui, &s);

    CHECK(d.w > 0 && d.h > 0, "damage: ar_invalidate forces a repaint the library cannot see");
    CHECK(d.x <= 10 && d.y <= 10 && d.x + d.w >= 40 && d.y + d.h >= 40,
          "damage: and the region returned covers what was invalidated");
}

static void test_damage_invalidate_all_repaints_everything(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    ar__ui_reset(DMG_CSS);
    ar__dmg_frame(&s, 0, -1, -1);

    ar_frame_begin(g_ui, 0);
    ar_invalidate_all(g_ui);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    d = ar_frame_end(g_ui, &s);

    CHECK(d.w == AR_DMG_W && d.h == AR_DMG_H, "damage: ar_invalidate_all repaints the surface");
}

static void test_damage_a_resize_repaints_everything(void)
{
    ar_surface small = ar__dmg_surface(g_dmg_a);
    ar_surface big = ar__dmg_surface(g_dmg_a);
    ar_rect    d;

    small.w = 120;
    small.h = 60;

    ar__ui_reset(DMG_CSS);
    ar__dmg_frame(&small, 0, -1, -1);
    ar__dmg_frame(&small, 0, -1, -1);
    d = ar__dmg_frame(&big, 0, -1, -1);

    /* Every box moved and the memory behind them is new. */
    CHECK(d.w == AR_DMG_W && d.h == AR_DMG_H, "damage: a resize repaints the whole surface");
}


/* Two changes far apart must stay two regions. Merging them into a bounding
   box is correct and costs 625x too much: measured at 800x600, presenting the
   merge is 480,000 pixels to update 768. */
static const char *const SPLIT_CSS =
    "#root { display:flex; flex-direction:column; justify-content:space-between; }"
    ".bar  { display:flex; flex-direction:row; justify-content:space-between; }"
    ".dot  { width:24px; height:16px; background:#3a4a5a; }"
    ".lit  { width:24px; height:16px; background:#e8c39e; }"
    ".gap  { width:24px; height:16px; }";

/* The tree only. The caller opens and closes the frame, so a test can
   invalidate inside the same frame the tree is declared in. */
static void ar__split_tree(int lit)
{
    ar_begin(g_ui, "#root");

    ar_begin(g_ui, "div.bar");
    ar_begin(g_ui, lit ? "div.lit" : "div.dot");
    ar_end(g_ui);
    ar_begin(g_ui, "div.gap");
    ar_end(g_ui);
    ar_end(g_ui);

    ar_begin(g_ui, "div.bar");
    ar_begin(g_ui, "div.gap");
    ar_end(g_ui);
    ar_begin(g_ui, lit ? "div.dot" : "div.lit");
    ar_end(g_ui);
    ar_end(g_ui);

    ar_end(g_ui);
}

static void ar__split_frame(ar_surface *s, int lit)
{
    ar_frame_begin(g_ui, 0);
    ar__split_tree(lit);
    ar_frame_end(g_ui, s);
}

static void test_damage_distant_changes_stay_separate(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_i32     i, presented = 0;

    ar__ui_reset(SPLIT_CSS);
    ar__split_frame(&s, 0);
    ar__split_frame(&s, 0);

    /* Only the two corner boxes swap colour. Nothing between them changes. */
    ar__split_frame(&s, 1);

    CHECK(ar_damage_count(g_ui) == 2, "damage: two changes in opposite corners stay two regions");

    for (i = 0; i < ar_damage_count(g_ui); ++i)
    {
        ar_rect r = ar_damage_rect(g_ui, i);
        presented += r.w * r.h;
    }

    /* Two 24x16 boxes is 768 pixels. A merged bounding box would span the whole
       surface, which is what this test exists to prevent. */
    CHECK(presented <= 768, "damage: and present only those boxes, not the span between them");
    CHECK(presented * 8 < AR_DMG_W * AR_DMG_H, "damage: far less than a merged bounding box");
}

static void test_damage_regions_never_exceed_the_cap(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_i32     i;
    int        over = 0;

    ar__ui_reset(SPLIT_CSS);
    ar__split_frame(&s, 0);
    ar__split_frame(&s, 0);

    /* Far more scattered invalidations than there are slots, inside the frame
       that declares the tree, and deliberately confined to a corner so the
       list is exercised rather than the collapse rule below. The list must
       merge rather than overflow, and every region must stay on the surface. */
    ar_frame_begin(g_ui, 0);
    for (i = 0; i < 64; ++i)
    {
        ar_invalidate(g_ui, ar_rect_make((i * 37) % 100, (i * 53) % 50, 8, 8));
    }
    ar__split_tree(0);
    ar_frame_end(g_ui, &s);

    CHECK(ar_damage_count(g_ui) <= AR_DAMAGE_RECTS,
          "damage: the region list merges rather than overflowing its cap");
    CHECK(ar_damage_count(g_ui) > 1, "damage: and does not collapse straight to one");

    for (i = 0; i < ar_damage_count(g_ui); ++i)
    {
        ar_rect r = ar_damage_rect(g_ui, i);
        if (r.x < 0 || r.y < 0 || r.x + r.w > AR_DMG_W || r.y + r.h > AR_DMG_H)
        {
            over = 1;
        }
    }
    CHECK(!over, "damage: and every region it returns is inside the surface");
}

static void test_damage_collapses_when_tracking_stops_paying(void)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_i32     i;

    ar__ui_reset(SPLIT_CSS);
    ar__split_frame(&s, 0);
    ar__split_frame(&s, 0);

    /* Scattered over most of the surface. Once damage passes half the window,
       deciding what to skip costs more than presenting all of it, so the
       region list gives up on purpose rather than thrashing. */
    ar_frame_begin(g_ui, 0);
    for (i = 0; i < 64; ++i)
    {
        ar_invalidate(g_ui, ar_rect_make((i * 37) % AR_DMG_W, (i * 53) % AR_DMG_H, 24, 24));
    }
    ar__split_tree(0);
    ar_frame_end(g_ui, &s);

    CHECK(ar_damage_count(g_ui) == 1, "damage: collapses to one region once it covers half");
    {
        ar_rect r = ar_damage_rect(g_ui, 0);
        CHECK(r.w == AR_DMG_W && r.h == AR_DMG_H, "damage: and that region is the whole surface");
    }
}

/* The acceptance criterion for the whole release.
 *
 * Two contexts run the same frames in lockstep: one tracks damage, the other is
 * forced to repaint everything. The surfaces are compared after EVERY frame,
 * not once at the end, because a stale pixel that a later frame happens to
 * overwrite is still a stale pixel that was on screen -- that is what flicker
 * is, and comparing only the final image would call it correct.
 */
static unsigned char g_dmg_mem[AR_MEM(256)];

static void ar__dmg_declare(ar_ctx *c, int second_is_b)
{
    ar_begin(c, "#root");
    ar_begin(c, "div.a");
    ar_end(c);
    ar_begin(c, second_is_b ? "div.b" : "div.a");
    ar_end(c);
    ar_end(c);
}

static void test_damage_output_is_identical_to_a_full_repaint(void)
{
    ar_surface tracked = ar__dmg_surface(g_dmg_a);
    ar_surface full = ar__dmg_surface(g_dmg_b);
    ar_ctx    *ref;
    int        i, frame, drew_less = 0;
    int        bad_frame = -1, bad_px = -1;

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        g_dmg_a[i] = 0;
        g_dmg_b[i] = 0;
    }

    ar__ui_reset(DMG_CSS);
    ref = ar_init(g_dmg_mem, (ar_u32)sizeof g_dmg_mem);
    CHECK(ref != 0, "damage: the reference context initialises");
    if (!ref || !g_ui)
    {
        return;
    }
    ar_stylesheet(ref, DMG_CSS);

    for (frame = 0; frame < 12 && bad_frame < 0; ++frame)
    {
        ar_input in;
        ar_rect  d;
        int      variant = (frame % 3 == 2);

        memset(&in, 0, sizeof in);
        in.mouse_x = (frame % 4) * 40;
        in.mouse_y = 20;
        in.mouse_inside = 1;

        ar_frame_begin(g_ui, &in);
        ar__dmg_declare(g_ui, variant);
        d = ar_frame_end(g_ui, &tracked);
        if (d.w < AR_DMG_W || d.h < AR_DMG_H)
        {
            drew_less = 1;
        }

        ar_frame_begin(ref, &in);
        ar_invalidate_all(ref);
        ar__dmg_declare(ref, variant);
        ar_frame_end(ref, &full);

        for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
        {
            if (g_dmg_a[i] != g_dmg_b[i])
            {
                bad_frame = frame;
                bad_px = i;
                break;
            }
        }
    }

    CHECK(drew_less, "damage: at least one frame of the run repainted less than the surface");
    CHECK(bad_frame < 0, "damage: tracked output is pixel identical to a full repaint, every frame");
    if (bad_frame >= 0)
    {
        printf("      frame %d, pixel (%d,%d): tracked %08lX, full %08lX\n", bad_frame,
               bad_px % AR_DMG_W, bad_px / AR_DMG_W, (unsigned long)g_dmg_a[bad_px],
               (unsigned long)g_dmg_b[bad_px]);
    }
}


/* ------------------------------------------------------------------------
 * The resolved style cache
 *
 * A cache that returns a wrong style is a silent rendering bug, so these check
 * agreement with the uncached resolver rather than checking that it is fast.
 * ------------------------------------------------------------------------ */
static int ar__styles_agree(const ar_style *a, const ar_style *b)
{
    int i;

    for (i = 0; i < AR_P_COUNT; ++i)
    {
        if (a->v[i] != b->v[i] || a->unit[i] != b->unit[i])
        {
            return 0;
        }
    }
    return 1;
}

static void test_style_cache_agrees_with_the_resolver(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[16];
    ar_sheet              cached, plain;
    ar_style              a, b;
    ar_u32                tag, klass, id;
    int                   mismatch = 0;
    int                   i, state;

    static const char *const CSS = "div { background:#101014; padding:4px; }"
                                   ".card { background:#20242c; width:120px; height:40px; }"
                                   ".card:hover { background:#2b313b; }"
                                   ".card:active { background:#3a4250; }"
                                   "#hero { width:300px; border:2px solid #e8c39e; }"
                                   "#hero:hover { border-color:#ffffff; }";

    ar_sheet_init(&cached, rules, 64);
    ar_sheet_set_cache(&cached, cache, 16);
    ar_sheet_parse(&cached, CSS);

    /* The same rules, with no cache attached, is the answer being checked. */
    {
        static ar_rule rules2[64];
        ar_sheet_init(&plain, rules2, 64);
        ar_sheet_parse(&plain, CSS);
    }

    /* Every selector against every state, twice, so both the storing pass and
       the hitting pass are compared. */
    for (i = 0; i < 2; ++i)
    {
        const char *const SELECTORS[] = {"div", "div.card", "div#hero", "div.card#hero", "span",
                                         ".card"};
        int               k;

        for (k = 0; k < (int)(sizeof SELECTORS / sizeof SELECTORS[0]); ++k)
        {
            if (!ar_selector_split(SELECTORS[k], &tag, &klass, &id))
            {
                continue;
            }
            for (state = 0; state < 8; ++state)
            {
                ar_sheet_resolve(&cached, tag, klass, id, (ar_u8)state, &a);
                ar_sheet_resolve(&plain, tag, klass, id, (ar_u8)state, &b);
                if (!ar__styles_agree(&a, &b))
                {
                    mismatch = 1;
                }
            }
        }
    }

    CHECK(!mismatch, "style cache: every selector and state resolves as the uncached resolver does");
    CHECK(cached.cache_hits > 0, "style cache: and the second pass actually hit it");
}

static void test_style_cache_is_dropped_when_a_sheet_is_added(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[16];
    ar_sheet              sheet;
    ar_style              before, after;
    ar_u32                tag, klass, id;

    ar_sheet_init(&sheet, rules, 64);
    ar_sheet_set_cache(&sheet, cache, 16);
    ar_sheet_parse(&sheet, ".box { width:10px; }");

    ar_selector_split("div.box", &tag, &klass, &id);
    ar_sheet_resolve(&sheet, tag, klass, id, 0, &before);

    /* A later sheet that overrides it. If the cached answer survived, the new
       rule would never be seen. */
    ar_sheet_parse(&sheet, ".box { width:99px; }");
    ar_sheet_resolve(&sheet, tag, klass, id, 0, &after);

    CHECK(before.v[AR_P_WIDTH] == 10, "style cache: the first sheet resolves");
    CHECK(after.v[AR_P_WIDTH] == 99, "style cache: adding a sheet drops what it cached");
}

static void test_style_cache_keeps_states_apart(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[16];
    ar_sheet              sheet;
    ar_style              rest, hover;
    ar_u32                tag, klass, id;

    ar_sheet_init(&sheet, rules, 64);
    ar_sheet_set_cache(&sheet, cache, 16);
    ar_sheet_parse(&sheet, ".b { background:#111111; }"
                           ".b:hover { background:#222222; }");

    ar_selector_split("div.b", &tag, &klass, &id);
    ar_sheet_resolve(&sheet, tag, klass, id, 0, &rest);
    ar_sheet_resolve(&sheet, tag, klass, id, AR_STATE_HOVER, &hover);

    /* The state is part of the key. Dropping it would make every box look
       hovered as soon as one of them was. */
    CHECK(rest.v[AR_P_BACKGROUND] != hover.v[AR_P_BACKGROUND],
          "style cache: hover and rest are separate entries");
}

static void test_style_cache_survives_more_tuples_than_it_holds(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[8];
    static ar_rule        rules2[64];
    ar_sheet              small, plain;
    int                   i, mismatch = 0;

    static const char *const CSS = ".a { width:1px; } .b { width:2px; } .c { width:3px; }";

    ar_sheet_init(&small, rules, 64);
    ar_sheet_set_cache(&small, cache, 8);
    ar_sheet_parse(&small, CSS);
    ar_sheet_init(&plain, rules2, 64);
    ar_sheet_parse(&plain, CSS);

    /* Far more distinct tuples than entries, so the probe window fills and the
       resolver has to fall through. Falling through must still be correct. */
    for (i = 0; i < 200; ++i)
    {
        ar_style a, b;
        ar_u32   tag = (ar_u32)(i * 2654435761u);
        ar_sheet_resolve(&small, tag, 0, 0, 0, &a);
        ar_sheet_resolve(&plain, tag, 0, 0, 0, &b);
        if (!ar__styles_agree(&a, &b))
        {
            mismatch = 1;
        }
    }

    CHECK(!mismatch, "style cache: a full table falls through to the resolver, still correctly");
}


/* ------------------------------------------------------------------------
 * The glyph blitter, pinned by checksum
 *
 * The blitter was rewritten to work in spans rather than per bit, which made
 * it 10.7x faster. A rewrite that changes what appears on screen is not an
 * optimisation, so this pins the output of every path through it: scale 1,
 * scale 2, clipped off the left, clipped off the right, alpha blended, cut by
 * a tight clip rectangle, and a newline.
 *
 * The value below was taken from the implementation that preceded the rewrite
 * and verified to be unchanged by it. If a future change moves it, that change
 * altered rendering, and the question is whether it meant to.
 * ------------------------------------------------------------------------ */
#define AR_GLYPH_W 200
#define AR_GLYPH_H 120

static ar_u32 g_glyph_px[AR_GLYPH_W * AR_GLYPH_H];

static void test_glyph_blitter_renders_the_same_pixels(void)
{
    static const char *const LINE = "Quick brown fox 0123456789 !@#$%^&*() {}[]<>|/~`";
    ar_surface               s;
    ar_rect                  clip;
    ar_u32                   h = 2166136261u;
    int                      i;

    s.pixels = g_glyph_px;
    s.w = AR_GLYPH_W;
    s.h = AR_GLYPH_H;
    s.stride = AR_GLYPH_W;

    for (i = 0; i < AR_GLYPH_W * AR_GLYPH_H; ++i)
    {
        g_glyph_px[i] = 0xFF101014u;
    }
    clip = ar_rect_make(0, 0, AR_GLYPH_W, AR_GLYPH_H);

    ar_draw_text(&s, clip, 4, 4, LINE, 1, AR_HEX(0xE8DFCC));            /* the fast path */
    ar_draw_text(&s, clip, 4, 20, LINE, 2, AR_HEX(0x8A8FA0));           /* scaled        */
    ar_draw_text(&s, clip, -40, 40, LINE, 1, AR_HEX(0xFFFFFF));         /* off the left  */
    ar_draw_text(&s, clip, 150, 56, LINE, 1, AR_HEX(0xFFFFFF));         /* off the right */
    ar_draw_text(&s, clip, 4, 72, LINE, 1, AR_RGBA(0xE8, 0xC3, 0x9E, 0x80)); /* blended  */
    ar_draw_text(&s, ar_rect_make(20, 80, 60, 20), 4, 88, LINE, 1, AR_HEX(0xFFFFFF));
    ar_draw_text(&s, clip, 4, 104, "tail\nsecond", 1, AR_HEX(0xFFFFFF));

    for (i = 0; i < AR_GLYPH_W * AR_GLYPH_H; ++i)
    {
        h ^= g_glyph_px[i];
        h *= 16777619u;
    }

    CHECK(h == 0x972896A2u, "font: every blitter path renders the pixels it always has");
    if (h != 0x972896A2u)
    {
        printf("      checksum is %08lX, expected 972896A2\n", (unsigned long)h);
    }
}

static void test_glyph_spans_respect_a_tight_clip(void)
{
    ar_surface s;
    int        i, outside = 0;

    s.pixels = g_glyph_px;
    s.w = AR_GLYPH_W;
    s.h = AR_GLYPH_H;
    s.stride = AR_GLYPH_W;

    for (i = 0; i < AR_GLYPH_W * AR_GLYPH_H; ++i)
    {
        g_glyph_px[i] = 0xFF101014u;
    }

    /* A run of set bits is now written as one span, so a clip that cuts
       through the middle of a run is the case that would go wrong. */
    ar_draw_text(&s, ar_rect_make(50, 10, 9, 5), 40, 8, "MMMMMMMM", 1, AR_HEX(0xFFFFFF));

    for (i = 0; i < AR_GLYPH_W * AR_GLYPH_H; ++i)
    {
        ar_i32 x = i % AR_GLYPH_W, y = i / AR_GLYPH_W;
        int    inside = (x >= 50 && x < 59 && y >= 10 && y < 15);
        if (!inside && g_glyph_px[i] != 0xFF101014u)
        {
            outside = 1;
        }
    }

    CHECK(!outside, "font: a span cut by a clip writes nothing outside it");
}


/* ------------------------------------------------------------------------
 * The coverage rasterizer
 *
 * Most of these check area rather than pixels. Summing the coverage of a shape
 * and comparing it to the shape's true area is the property that separates an
 * analytic rasterizer from a sampling one: a supersampler gets it approximately
 * right and its error depends on where the shape happens to sit relative to the
 * sample grid, while exact area accumulation is off only by the rounding in the
 * fixed point. So the tolerances here are tight on purpose, and loosening one
 * to make a change pass would be throwing away the reason for the design.
 * ------------------------------------------------------------------------ */
#define AR_RAST_W 64
#define AR_RAST_H 64

static ar_i32 g_path_pts[4096 * 2];
static ar_u8  g_cov[AR_RAST_W * AR_RAST_H];
static ar_i32 g_acc[(AR_RAST_W + 2) * AR_RAST_H];

#define PX(v) ((ar_i32)((v) * AR_ONE_PIXEL))

static ar_i32 ar__cov_total(void)
{
    ar_i32 sum = 0, i;
    for (i = 0; i < AR_RAST_W * AR_RAST_H; ++i)
    {
        sum += g_cov[i];
    }
    return sum;
}

/* Area in pixels, times 256, so it can be compared against the coverage sum
   without leaving integers. */
static ar_i32 ar__cov_area_x256(void)
{
    return ar__cov_total() * 256 / 255;
}

static void ar__rast(ar_path *p, ar_i32 rule)
{
    ar_path_rasterize(p, g_cov, AR_RAST_W, AR_RAST_H, AR_RAST_W, 0, 0, rule, g_acc);
}

static void test_path_aligned_rect_is_solid(void)
{
    ar_path p;
    ar_i32  x, y, wrong = 0;

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(10), PX(8));
    ar_path_line_to(&p, PX(30), PX(8));
    ar_path_line_to(&p, PX(30), PX(24));
    ar_path_line_to(&p, PX(10), PX(24));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    for (y = 0; y < AR_RAST_H; ++y)
    {
        for (x = 0; x < AR_RAST_W; ++x)
        {
            int inside = (x >= 10 && x < 30 && y >= 8 && y < 24);
            ar_u8 c = g_cov[y * AR_RAST_W + x];
            if (inside ? (c != 255) : (c != 0))
            {
                wrong = 1;
            }
        }
    }

    CHECK(!p.overflow, "path: a four point rectangle fits its storage");
    CHECK(!wrong, "path: a pixel aligned rectangle is exactly solid, with nothing outside it");
}

static void test_path_half_pixel_edge_is_half_covered(void)
{
    ar_path p;
    ar_u8   half, quarter;

    ar_path_init(&p, g_path_pts, 4096);
    /* Right edge at x = 20.5, so column 20 is half covered. */
    ar_path_move_to(&p, PX(10), PX(8));
    ar_path_line_to(&p, PX(10) + AR_ONE_PIXEL / 2 + PX(10), PX(8));
    ar_path_line_to(&p, PX(10) + AR_ONE_PIXEL / 2 + PX(10), PX(24));
    ar_path_line_to(&p, PX(10), PX(24));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    half = g_cov[12 * AR_RAST_W + 20];

    /* And a quarter, one subpixel step further, to show it is a ramp rather
       than a threshold that happens to be right at one half. */
    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(10), PX(8));
    ar_path_line_to(&p, PX(20) + AR_ONE_PIXEL / 4, PX(8));
    ar_path_line_to(&p, PX(20) + AR_ONE_PIXEL / 4, PX(24));
    ar_path_line_to(&p, PX(10), PX(24));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);
    quarter = g_cov[12 * AR_RAST_W + 20];

    CHECK(half >= 125 && half <= 130, "path: an edge halfway through a pixel covers it half");
    CHECK(quarter >= 61 && quarter <= 68, "path: and a quarter of the way through, a quarter");
}

static void test_path_triangle_conserves_area(void)
{
    ar_path p;
    ar_i32  got, want;

    ar_path_init(&p, g_path_pts, 4096);
    /* Base 40, height 30, so the true area is 600 pixels. Deliberately not on
       pixel boundaries, because a rasterizer can be right on the grid and
       wrong everywhere else. */
    ar_path_move_to(&p, PX(5) + 13, PX(50) - 7);
    ar_path_line_to(&p, PX(45) + 13, PX(50) - 7);
    ar_path_line_to(&p, PX(25) + 13, PX(20) - 7);
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    got = ar__cov_area_x256();
    want = 600 * 256;

    /* Measured error is 0.04%. The tolerance is 0.5% because that is still an
       order of magnitude tighter than anything a sampling rasterizer reaches,
       and loosening it to make a change pass would be discarding the property
       the design exists for. */
    CHECK(got > want - want / 200 && got < want + want / 200,
          "path: a triangle's coverage sums to its area, within 0.5%");
}

static void test_path_winding_direction_does_not_matter(void)
{
    ar_path p;
    ar_i32  clockwise, anticlockwise;

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(10), PX(10));
    ar_path_line_to(&p, PX(30), PX(10));
    ar_path_line_to(&p, PX(30), PX(30));
    ar_path_line_to(&p, PX(10), PX(30));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);
    clockwise = ar__cov_total();

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(10), PX(10));
    ar_path_line_to(&p, PX(10), PX(30));
    ar_path_line_to(&p, PX(30), PX(30));
    ar_path_line_to(&p, PX(30), PX(10));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);
    anticlockwise = ar__cov_total();

    CHECK(clockwise == anticlockwise && clockwise > 0,
          "path: a contour fills the same either way round");
}

static void test_path_fill_rules_differ_on_a_hole(void)
{
    ar_path p;
    ar_u8   nonzero_centre, evenodd_centre;
    ar_i32  i;

    /* An outer square and an inner square wound the same way. Nonzero fills
       both; even-odd punches the inner one out. This is exactly how a glyph
       like 'o' works, and getting it wrong is how counters fill in. */
    for (i = 0; i < 2; ++i)
    {
        ar_path_init(&p, g_path_pts, 4096);
        ar_path_move_to(&p, PX(8), PX(8));
        ar_path_line_to(&p, PX(40), PX(8));
        ar_path_line_to(&p, PX(40), PX(40));
        ar_path_line_to(&p, PX(8), PX(40));
        ar_path_close(&p);
        ar_path_move_to(&p, PX(16), PX(16));
        ar_path_line_to(&p, PX(32), PX(16));
        ar_path_line_to(&p, PX(32), PX(32));
        ar_path_line_to(&p, PX(16), PX(32));
        ar_path_close(&p);
        ar__rast(&p, i == 0 ? AR_FILL_NONZERO : AR_FILL_EVENODD);
        if (i == 0)
        {
            nonzero_centre = g_cov[24 * AR_RAST_W + 24];
        }
        else
        {
            evenodd_centre = g_cov[24 * AR_RAST_W + 24];
        }
    }

    CHECK(nonzero_centre == 255, "path: nonzero fills a same-wound inner contour");
    CHECK(evenodd_centre == 0, "path: even-odd punches it out");
}

static void test_path_opposite_winding_makes_a_hole(void)
{
    ar_path p;

    /* The way TrueType actually does it: the counter is wound the other way,
       and nonzero cancels it. */
    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(8), PX(8));
    ar_path_line_to(&p, PX(40), PX(8));
    ar_path_line_to(&p, PX(40), PX(40));
    ar_path_line_to(&p, PX(8), PX(40));
    ar_path_close(&p);
    ar_path_move_to(&p, PX(16), PX(16));
    ar_path_line_to(&p, PX(16), PX(32));
    ar_path_line_to(&p, PX(32), PX(32));
    ar_path_line_to(&p, PX(32), PX(16));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    CHECK(g_cov[24 * AR_RAST_W + 24] == 0, "path: an oppositely wound contour is a hole");
    CHECK(g_cov[12 * AR_RAST_W + 24] == 255, "path: and the ring around it stays filled");
}

static void test_path_curves_flatten_to_the_right_area(void)
{
    ar_path p;
    ar_i32  got, want;
    ar_i32  cx = PX(32), cy = PX(32), r = PX(20);
    /* The control point offset that makes a quadratic bezier approximate a
       quarter circle: 4/3 * (sqrt(2) - 1) is the cubic constant; for four
       quadratics the corner control is simply the corner of the square. */

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, cx + r, cy);
    ar_path_quad_to(&p, cx + r, cy + r, cx, cy + r);
    ar_path_quad_to(&p, cx - r, cy + r, cx - r, cy);
    ar_path_quad_to(&p, cx - r, cy - r, cx, cy - r);
    ar_path_quad_to(&p, cx + r, cy - r, cx + r, cy);
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    got = ar__cov_area_x256();

    /* Four quadratics through the corners of the square do not make a circle,
       they make a slightly fatter shape, so the expected area is derived
       rather than assumed. Parametrising one arc from (r,0) to (0,r) with its
       control at (r,r) gives x(t) = r(1 - t^2) and y(t) = r(2t - t^2), and
       Green's theorem then gives

           x y' - y x' = 2r^2 (1 - t + t^2)
           A_quarter   = 1/2 integral_0^1 = 5 r^2 / 6
           A_total     = 4 A_quarter      = 10 r^2 / 3

       which is 1333.3 pixels at r = 20: five sixths of the bounding square,
       against pi/4 for a true circle. What is being tested is that flattening
       conserves whatever area the curve encloses, so the number has to be the
       curve's own area and not a circle's. */
    want = 10 * 20 * 20 * 256 / 3;

    CHECK(!p.overflow, "path: four flattened quadratics fit the point storage");
    /* Measured error is 0.16%, and most of that is the quarter-pixel flattening
       tolerance rather than the rasterizer. */
    CHECK(got > want - want / 100 && got < want + want / 100,
          "path: a curved contour's coverage sums to the area it encloses, within 1%");
}

/* The bug that flooring fixed, kept as a test because it is invisible in any
   single shape: the same triangle drawn either way up must have the same area.
   Truncation towards zero made one 0.52% small and the other 0.52% large. */
static void test_path_area_does_not_depend_on_orientation(void)
{
    ar_path p;
    ar_i32  up, down, diff;

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(5), PX(50));
    ar_path_line_to(&p, PX(45), PX(50));
    ar_path_line_to(&p, PX(25), PX(20));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);
    up = ar__cov_total();

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(5), PX(20));
    ar_path_line_to(&p, PX(45), PX(20));
    ar_path_line_to(&p, PX(25), PX(50));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);
    down = ar__cov_total();

    diff = up > down ? up - down : down - up;
    CHECK(diff * 200 < up, "path: a triangle covers the same area either way up");
}

static void test_path_clips_to_the_bitmap(void)
{
    ar_path p;
    ar_i32  x, y, holes = 0;

    /* Far larger than the bitmap and starting well outside it in both
       directions. Geometry to the left still has to contribute its winding or
       the rows it crosses come out empty. */
    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(-500), PX(-500));
    ar_path_line_to(&p, PX(500), PX(-500));
    ar_path_line_to(&p, PX(500), PX(500));
    ar_path_line_to(&p, PX(-500), PX(500));
    ar_path_close(&p);
    ar__rast(&p, AR_FILL_NONZERO);

    for (y = 0; y < AR_RAST_H; ++y)
    {
        for (x = 0; x < AR_RAST_W; ++x)
        {
            if (g_cov[y * AR_RAST_W + x] != 255)
            {
                holes = 1;
            }
        }
    }
    CHECK(!holes, "path: a shape larger than the bitmap fills all of it");
}

static void test_path_bounds_round_outward(void)
{
    ar_path p;
    ar_rect b;

    ar_path_init(&p, g_path_pts, 4096);
    ar_path_move_to(&p, PX(3) + 1, PX(5) + 63);
    ar_path_line_to(&p, PX(9) - 1, PX(11) + 1);
    ar_path_close(&p);
    b = ar_path_bounds(&p);

    CHECK(b.x == 3 && b.y == 5, "path: bounds floor the minimum to a whole pixel");
    CHECK(b.x + b.w == 9 && b.y + b.h == 12, "path: and ceil the maximum");
}

static void test_path_reports_overflow_rather_than_scribbling(void)
{
    ar_path p;
    ar_i32  small[8 * 2];
    ar_i32  i;

    ar_path_init(&p, small, 8);
    ar_path_move_to(&p, 0, 0);
    for (i = 0; i < 200; ++i)
    {
        ar_path_line_to(&p, PX(i), PX(i));
    }

    CHECK(p.overflow, "path: running out of point storage is reported");
    CHECK(p.count <= 8, "path: and nothing is written past the end of it");
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

    test_text_metrics();
    test_text_pixels();
    test_text_scaling();

    test_perf_phase_accounting();
    test_perf_percentiles();
    test_perf_empty();
    test_perf_ring_wraps();
    test_perf_survives_a_clock_wrap();
    test_perf_overlay_draws_and_clips();

    test_css_basic();
    test_css_units();
    test_css_colors();
    test_css_specificity();
    test_css_source_order_breaks_ties();
    test_css_pseudo_states();
    test_css_shorthands();
    test_css_border_shorthand();
    test_css_keywords();
    test_css_comments_and_whitespace();
    test_css_survives_malformed_input();
    test_css_always_terminates();
    test_css_capacity();
    test_selector_split();

    test_path_aligned_rect_is_solid();
    test_path_half_pixel_edge_is_half_covered();
    test_path_triangle_conserves_area();
    test_path_winding_direction_does_not_matter();
    test_path_fill_rules_differ_on_a_hole();
    test_path_opposite_winding_makes_a_hole();
    test_path_curves_flatten_to_the_right_area();
    test_path_area_does_not_depend_on_orientation();
    test_path_clips_to_the_bitmap();
    test_path_bounds_round_outward();
    test_path_reports_overflow_rather_than_scribbling();

    test_glyph_blitter_renders_the_same_pixels();
    test_glyph_spans_respect_a_tight_clip();

    test_style_cache_agrees_with_the_resolver();
    test_style_cache_is_dropped_when_a_sheet_is_added();
    test_style_cache_keeps_states_apart();
    test_style_cache_survives_more_tuples_than_it_holds();

    test_layout_row_with_gap_and_padding();
    test_layout_column();
    test_layout_grow_splits_the_remainder();
    test_layout_grow_alongside_fixed();
    test_layout_percent();
    test_layout_auto_sizes_to_content();
    test_layout_justify_content();
    test_layout_align_items();
    test_layout_min_and_max();
    test_layout_display_none();
    test_layout_nesting();
    test_layout_survives_abuse();
    test_layout_is_stable_across_frames();

    test_damage_first_frame_paints_everything();
    test_damage_an_unchanged_frame_paints_nothing();
    test_damage_a_style_change_repaints_that_box();
    test_damage_invalidate_is_honoured();
    test_damage_invalidate_all_repaints_everything();
    test_damage_a_resize_repaints_everything();
    test_damage_distant_changes_stay_separate();
    test_damage_regions_never_exceed_the_cap();
    test_damage_collapses_when_tracking_stops_paying();
    test_damage_output_is_identical_to_a_full_repaint();

    printf("\n%d checks, %d failed\n", ar__checks, ar__failures);
    return ar__failures == 0 ? 0 : 1;
}
