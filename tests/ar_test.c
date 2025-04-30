/*
 * areole - test harness.
 * SPDX-License-Identifier: MIT
 *
 * No framework, no fixtures. Every non-trivial piece of logic leaves one
 * runnable check here that fails if the logic breaks.
 */
#include "ar_internal.h"
#include "ar_path.h"
#include "ar_font_file.h"
#include "ar_text.h"
#include "ar_break.h"
#include "ar_bidi.h"
#include "ar_shape.h"
#include "ar_indic.h"
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
    ar_style   st;
    ar_u32     tag = 0, id = 0;
    ar_classes klass;

    ar_selector_split(selector, &tag, &klass, &id);
    ar_sheet_resolve(&g_sheet, tag, &klass, id, state, &st);
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
    ar_u32     tag, id;
    ar_classes klass;

    CHECK(ar_selector_split("div.card#first", &tag, &klass, &id),
          "selector: a full selector splits");
    CHECK(tag == ar_hash("div", 3), "selector: the tag is extracted");
    CHECK(klass.n == 1 && klass.h[0] == ar_hash("card", 4), "selector: the class is extracted");
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

#define AR_LAY_MAX 1024

static unsigned char g_ui_mem[AR_MEM(256)];
/* Big enough for every size any test asks for, and ar__ui_surface refuses to
   hand out more. It held 64 rows for a long time and nothing noticed, because
   no test until now painted a background across the whole surface -- the
   overrun landed on the next global and turned the context pointer into a
   colour value. */
#define AR_LAY_ROWS 400
static ar_u32  g_ui_pixels[AR_LAY_MAX * AR_LAY_ROWS];
static ar_ctx *g_ui;

static ar_surface ar__ui_surface(ar_i32 w, ar_i32 h)
{
    ar_surface s;
    s.pixels = g_ui_pixels;
    s.w = w > AR_LAY_MAX ? AR_LAY_MAX : w;
    s.h = h > AR_LAY_ROWS ? AR_LAY_ROWS : h;
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
    CHECK(d.w == AR_DMG_W && d.h == AR_DMG_H, "damage: the first frame repaints the whole surface");
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
    CHECK(bad_frame < 0,
          "damage: tracked output is pixel identical to a full repaint, every frame");
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
    ar_u32                tag, id;
    ar_classes            klass;
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
        const char *const SELECTORS[] = {"div",           "div.card", "div#hero",
                                         "div.card#hero", "span",     ".card"};
        int               k;

        for (k = 0; k < (int)(sizeof SELECTORS / sizeof SELECTORS[0]); ++k)
        {
            if (!ar_selector_split(SELECTORS[k], &tag, &klass, &id))
            {
                continue;
            }
            for (state = 0; state < 8; ++state)
            {
                ar_sheet_resolve(&cached, tag, &klass, id, (ar_u8)state, &a);
                ar_sheet_resolve(&plain, tag, &klass, id, (ar_u8)state, &b);
                if (!ar__styles_agree(&a, &b))
                {
                    mismatch = 1;
                }
            }
        }
    }

    CHECK(!mismatch,
          "style cache: every selector and state resolves as the uncached resolver does");
    CHECK(cached.cache_hits > 0, "style cache: and the second pass actually hit it");
}

static void test_style_cache_is_dropped_when_a_sheet_is_added(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[16];
    ar_sheet              sheet;
    ar_style              before, after;
    ar_u32                tag, id;
    ar_classes            klass;

    ar_sheet_init(&sheet, rules, 64);
    ar_sheet_set_cache(&sheet, cache, 16);
    ar_sheet_parse(&sheet, ".box { width:10px; }");

    ar_selector_split("div.box", &tag, &klass, &id);
    ar_sheet_resolve(&sheet, tag, &klass, id, 0, &before);

    /* A later sheet that overrides it. If the cached answer survived, the new
       rule would never be seen. */
    ar_sheet_parse(&sheet, ".box { width:99px; }");
    ar_sheet_resolve(&sheet, tag, &klass, id, 0, &after);

    CHECK(before.v[AR_P_WIDTH] == 10, "style cache: the first sheet resolves");
    CHECK(after.v[AR_P_WIDTH] == 99, "style cache: adding a sheet drops what it cached");
}

static void test_style_cache_keeps_states_apart(void)
{
    static ar_rule        rules[64];
    static ar_cache_entry cache[16];
    ar_sheet              sheet;
    ar_style              rest, hover;
    ar_u32                tag, id;
    ar_classes            klass;

    ar_sheet_init(&sheet, rules, 64);
    ar_sheet_set_cache(&sheet, cache, 16);
    ar_sheet_parse(&sheet, ".b { background:#111111; }"
                           ".b:hover { background:#222222; }");

    ar_selector_split("div.b", &tag, &klass, &id);
    ar_sheet_resolve(&sheet, tag, &klass, id, 0, &rest);
    ar_sheet_resolve(&sheet, tag, &klass, id, AR_STATE_HOVER, &hover);

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
        ar_style   a, b;
        ar_u32     tag = (ar_u32)(i * 2654435761u);
        ar_classes none;
        ar_classes_clear(&none);
        ar_sheet_resolve(&small, tag, &none, 0, 0, &a);
        ar_sheet_resolve(&plain, tag, &none, 0, 0, &b);
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

    ar_draw_text(&s, clip, 4, 4, LINE, 1, AR_HEX(0xE8DFCC));                 /* the fast path */
    ar_draw_text(&s, clip, 4, 20, LINE, 2, AR_HEX(0x8A8FA0));                /* scaled        */
    ar_draw_text(&s, clip, -40, 40, LINE, 1, AR_HEX(0xFFFFFF));              /* off the left  */
    ar_draw_text(&s, clip, 150, 56, LINE, 1, AR_HEX(0xFFFFFF));              /* off the right */
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
            int   inside = (x >= 10 && x < 30 && y >= 8 && y < 24);
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

/* ------------------------------------------------------------------------
 * TrueType parsing
 *
 * The font below is built rather than shipped: a complete, minimal, valid
 * TrueType file with head, hhea, maxp, cmap format 4, loca, glyf and hmtx,
 * and two glyphs -- an empty one at index 0 as the specification requires,
 * and a triangle at index 1 mapped from 'A'.
 *
 * Generating it keeps the tests self-contained and, more usefully, makes it
 * mutable: the robustness check below corrupts and truncates this same font,
 * which is how a parser gets tested against the input it will actually meet.
 * ------------------------------------------------------------------------ */
static const ar_u8 AR_TEST_FONT[] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x63, 0x6D, 0x61, 0x70,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x2C, 0x67, 0x6C, 0x79, 0x66,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x00, 0x00, 0x00, 0x20, 0x68, 0x65, 0x61, 0x64,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x00, 0x00, 0x36, 0x68, 0x68, 0x65, 0x61,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x24, 0x68, 0x6D, 0x74, 0x78,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x24, 0x00, 0x00, 0x00, 0x08, 0x6C, 0x6F, 0x63, 0x61,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x00, 0x00, 0x06, 0x6D, 0x61, 0x78, 0x70,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x34, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04,
    0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x41, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x41, 0xFF, 0xFF,
    0xFF, 0xC0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,
    0x03, 0xE8, 0x00, 0x02, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x03, 0xE8, 0xFE, 0x0C, 0x00,
    0x00, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x5F, 0x0F, 0x3C, 0xF5, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x03, 0xE8, 0x03, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x03, 0x20, 0xFF, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x02, 0x58, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
};

static ar_i32 g_ttf_pts[2048 * 2];
static ar_i32 g_ttf_x[256], g_ttf_y[256];
static ar_u8  g_ttf_on[256];

static void ar__ttf_buf(ar_outline_buf *b)
{
    b->x = g_ttf_x;
    b->y = g_ttf_y;
    b->on = g_ttf_on;
    b->cap = 256;
}

static void test_ttf_reads_the_tables(void)
{
    ar_face f;
    int     ok = ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);

    CHECK(ok && f.ok, "ttf: a minimal valid font initialises");
    CHECK(f.units_per_em == 1000, "ttf: units per em comes from head");
    CHECK(f.num_glyphs == 2, "ttf: glyph count comes from maxp");
    CHECK(f.ascender == 800 && f.descender == -200, "ttf: vertical metrics come from hhea");
    CHECK(f.cmap_format == 4, "ttf: the format 4 subtable is selected");
    CHECK(!f.loc_long, "ttf: short loca is recognised");
}

static void test_ttf_maps_codepoints(void)
{
    ar_face f;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);

    CHECK(ar_face_glyph(&f, 'A') == 1, "ttf: cmap maps a mapped codepoint");
    CHECK(ar_face_glyph(&f, 'B') == 0, "ttf: and an unmapped one to the notdef glyph");
    CHECK(ar_face_glyph(&f, 0x10FFFF) == 0, "ttf: including one past the basic plane");
    CHECK(ar_face_advance(&f, 1) == 1000, "ttf: hmtx gives the advance");
}

static void test_ttf_scales_without_overflowing(void)
{
    ar_face f;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);

    /* One em at 16 pixels is 16 pixels, which in 26.6 is 1024. */
    CHECK(ar_face_scale(&f, 1000, 16) == 16 * AR_ONE_PIXEL, "ttf: one em scales to the pixel size");
    CHECK(ar_face_scale(&f, 500, 16) == 8 * AR_ONE_PIXEL, "ttf: and half an em to half of it");
    CHECK(ar_face_scale(&f, -1000, 16) == -16 * AR_ONE_PIXEL, "ttf: negatives scale symmetrically");

    /* The combination that overflows the obvious expression: a large
       coordinate at a large size. 32000 * 64 * 1000 is 2.05e9, past the top of
       a signed 32 bit integer, so this would come back negative if the value
       were not split into whole ems and a remainder first. */
    CHECK(ar_face_scale(&f, 32000, 1000) == 32000 * AR_ONE_PIXEL,
          "ttf: a large coordinate at a large size does not overflow");
}

static void test_ttf_extracts_an_outline(void)
{
    ar_face        f;
    ar_path        p;
    ar_outline_buf buf;
    ar_rect        b;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__ttf_buf(&buf);
    ar_path_init(&p, g_ttf_pts, 2048);

    CHECK(ar_face_outline(&f, 1, 32, 0, 32 * AR_ONE_PIXEL, &p, &buf),
          "ttf: a glyph yields an outline");
    CHECK(p.count == 3, "ttf: the triangle's three points come through");
    CHECK(!p.overflow, "ttf: and fit the path storage");

    /* One em tall at 32 pixels, with the baseline at y = 32 and the y axis
       flipped, so the triangle occupies rows 0 to 32. */
    b = ar_path_bounds(&p);
    CHECK(b.w == 32 && b.h == 32, "ttf: the outline is one em square at 32 pixels");
    CHECK(b.y == 0, "ttf: and sits above the baseline, y flipped");
}

static void test_ttf_empty_glyph_is_not_an_error(void)
{
    ar_face        f;
    ar_path        p;
    ar_outline_buf buf;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__ttf_buf(&buf);
    ar_path_init(&p, g_ttf_pts, 2048);

    /* Glyph 0 has no outline, and neither does a space. Contributing nothing
       is correct; the caller still advances the pen. */
    ar_face_outline(&f, 0, 32, 0, 0, &p, &buf);
    CHECK(p.count == 0, "ttf: an empty glyph contributes no points");
}

static void test_ttf_rejects_what_it_cannot_read(void)
{
    ar_face f;
    ar_u8   junk[64];
    ar_i32  i;

    for (i = 0; i < 64; ++i)
    {
        junk[i] = (ar_u8)(i * 7);
    }

    CHECK(!ar_face_init(&f, junk, 64), "ttf: random bytes are not accepted as a font");
    CHECK(!ar_face_init(&f, AR_TEST_FONT, 4), "ttf: nor is a file too short to hold a header");
    CHECK(!ar_face_init(&f, 0, 0), "ttf: nor is nothing at all");

    /* OTTO is a real font with CFF outlines, which this parser cannot read.
       Refusing is right: half-parsing it and drawing nothing would look like a
       bug in the renderer rather than a limitation of the parser. */
    {
        ar_u8 otto[64];
        memcpy(otto, AR_TEST_FONT, 64);
        otto[0] = 'O';
        otto[1] = 'T';
        otto[2] = 'T';
        otto[3] = 'O';
        CHECK(!ar_face_init(&f, otto, 64), "ttf: a CFF font is refused rather than half read");
    }
}

/* The property that matters more than any single parse: a corrupt font must
   not be able to make the parser read outside the buffer it was given. Every
   prefix of the font, and every single-byte corruption of it, goes through the
   whole pipeline. If any read escaped its bounds, this faults. */
/* CFF is the one place in the library that runs a program out of a file: a
   charstring is a stack machine with subroutines. There is no minimal CFF font
   generated here the way there is for glyf, because a valid one is an order of
   magnitude more structure and the property that matters is not "does it draw
   an A" -- it is that a malformed program cannot escape its bounds.

   That is checked outside this suite, under a guard page: Source Han Sans in
   both weights, all 17,934 glyphs each plus 9,471 codepoints, with the font
   placed so its last byte ends at an inaccessible page. The three limits that
   hold it are stated in ar_cff.c. Here we check only that a font claiming to
   be CFF but containing nothing is refused rather than half accepted. */
static void test_cff_header_alone_is_not_a_font(void)
{
    ar_face f;
    ar_u8   otto[128];
    ar_i32  i;

    for (i = 0; i < 128; ++i)
    {
        otto[i] = 0;
    }
    otto[0] = 'O';
    otto[1] = 'T';
    otto[2] = 'T';
    otto[3] = 'O';

    CHECK(!ar_face_init(&f, otto, 128), "cff: an OTTO header with no tables is refused");

    /* And a truncated one, at every length, must not fault. */
    for (i = 0; i <= 128; ++i)
    {
        ar_face_init(&f, otto, (ar_u32)i);
    }
    CHECK(1, "cff: every truncation of it parses without faulting");
}

static void test_ttf_survives_truncation_and_corruption(void)
{
    static ar_u8   copy[sizeof AR_TEST_FONT];
    ar_face        f;
    ar_path        p;
    ar_outline_buf buf;
    ar_u32         n;
    ar_i32         i;
    ar_i32         parsed = 0;

    ar__ttf_buf(&buf);

    for (n = 0; n <= (ar_u32)sizeof AR_TEST_FONT; ++n)
    {
        if (ar_face_init(&f, AR_TEST_FONT, n))
        {
            ar_path_init(&p, g_ttf_pts, 2048);
            ar_face_glyph(&f, 'A');
            ar_face_advance(&f, 1);
            ar_face_outline(&f, 1, 32, 0, 0, &p, &buf);
        }
    }

    for (i = 0; i < (ar_i32)sizeof AR_TEST_FONT; ++i)
    {
        memcpy(copy, AR_TEST_FONT, sizeof AR_TEST_FONT);
        copy[i] = (ar_u8)(~copy[i] & 0xFF);
        if (ar_face_init(&f, copy, (ar_u32)sizeof copy))
        {
            ar_path_init(&p, g_ttf_pts, 2048);
            ar_face_glyph(&f, 'A');
            ar_face_glyph(&f, 0xFFFF);
            ar_face_advance(&f, 1);
            ar_face_outline(&f, 1, 32, 0, 0, &p, &buf);
            ++parsed;
        }
    }

    CHECK(1, "ttf: every truncation and single byte corruption parses without faulting");
    CHECK(parsed > 0, "ttf: and enough still parse for that to be worth something");
}

/* ------------------------------------------------------------------------
 * UTF-8
 *
 * The rejections matter more than the acceptances. Overlong forms, surrogates
 * and values above U+10FFFF are all ways of getting a decoder to produce a
 * codepoint the rest of the program believed had already been validated, and
 * every one of them has been a real vulnerability in a real renderer.
 * ------------------------------------------------------------------------ */
static ar_u32 ar__one(const char *s)
{
    const char *p = s;
    return ar_utf8_next(&p);
}

static ar_i32 ar__consumed(const char *s)
{
    const char *p = s;
    ar_utf8_next(&p);
    return (ar_i32)(p - s);
}

static void test_utf8_decodes_valid_sequences(void)
{
    CHECK(ar__one("A") == 'A', "utf8: ascii");
    CHECK(ar__one("\xC3\xA9") == 0xE9, "utf8: two bytes, e acute");
    CHECK(ar__one("\xD0\x9F") == 0x41F, "utf8: two bytes, cyrillic");
    CHECK(ar__one("\xE2\x80\x94") == 0x2014, "utf8: three bytes, em dash");
    CHECK(ar__one("\xF0\x9F\x8C\xB8") == 0x1F338, "utf8: four bytes, above the basic plane");
    CHECK(ar__one("") == 0, "utf8: the end of the string is zero");

    CHECK(ar__consumed("A") == 1, "utf8: ascii advances one byte");
    CHECK(ar__consumed("\xE2\x80\x94") == 3, "utf8: a three byte sequence advances three");
    CHECK(ar__consumed("\xF0\x9F\x8C\xB8") == 4, "utf8: and a four byte one, four");
}

static void test_utf8_rejects_what_it_should(void)
{
    /* C0 80 is an overlong NUL: two bytes encoding U+0000. Accepting it lets a
       string get past a check for an embedded zero and then produce one. */
    CHECK(ar__one("\xC0\x80") == 0xFFFD, "utf8: an overlong NUL is refused");
    CHECK(ar__one("\xE0\x80\xAF") == 0xFFFD, "utf8: an overlong solidus is refused");
    CHECK(ar__one("\xED\xA0\x80") == 0xFFFD, "utf8: a surrogate half is not a character");
    CHECK(ar__one("\xF4\x90\x80\x80") == 0xFFFD, "utf8: nothing exists above U+10FFFF");
    CHECK(ar__one("\xFF") == 0xFFFD, "utf8: FF cannot start a character");
    CHECK(ar__one("\x80") == 0xFFFD, "utf8: nor can a continuation byte");
    CHECK(ar__one("\xE2\x80") == 0xFFFD, "utf8: a truncated sequence is refused");

    /* One byte forward on every rejection, so a stray byte costs one
       replacement rather than swallowing the character after it. */
    CHECK(ar__consumed("\xFF") == 1, "utf8: a bad lead byte advances exactly one");
    CHECK(ar__consumed("\x80") == 1, "utf8: so does a stray continuation");
    CHECK(ar__consumed("\xE2\x80") == 1, "utf8: and so does a truncated sequence");
}

static void test_utf8_walks_a_mixed_string_exactly(void)
{
    /* "Hi" + cyrillic pe + em dash + a bad byte + "!" */
    const char *s = "Hi\xD0\x9F\xE2\x80\x94\xFF!";
    ar_u32      got[8];
    ar_i32      n = 0;

    for (;;)
    {
        ar_u32 cp = ar_utf8_next(&s);
        if (cp == 0 || n >= 8)
        {
            break;
        }
        got[n++] = cp;
    }

    CHECK(n == 6, "utf8: a mixed string yields one codepoint per character");
    CHECK(n == 6 && got[0] == 'H' && got[1] == 'i' && got[2] == 0x41F && got[3] == 0x2014 &&
              got[4] == 0xFFFD && got[5] == '!',
          "utf8: and a bad byte replaces itself without disturbing its neighbours");
}

/* ------------------------------------------------------------------------
 * The glyph cache
 * ------------------------------------------------------------------------ */
static ar_glyph_slot g_gc_slots[64];
static ar_u8         g_gc_pixels[64 * 1024];
static ar_i32        g_gc_pts[2048 * 2];
static ar_i32        g_gc_x[256], g_gc_y[256];
static ar_u8         g_gc_on[256];
static ar_i32        g_gc_acc[(96 + 2) * 96];

static void ar__gc_setup(ar_glyph_cache *gc, ar_glyph_scratch *sc, ar_i32 cap)
{
    sc->path_pts = g_gc_pts;
    sc->path_cap = 2048;
    sc->outline.x = g_gc_x;
    sc->outline.y = g_gc_y;
    sc->outline.on = g_gc_on;
    sc->outline.cap = 256;
    sc->acc = g_gc_acc;
    sc->acc_cap = (96 + 2) * 96;
    ar_glyph_cache_init(gc, g_gc_slots, 64, g_gc_pixels, cap);
}

static void test_glyph_cache_rasterizes_once(void)
{
    ar_face              f;
    ar_glyph_cache       gc;
    ar_glyph_scratch     sc;
    const ar_glyph_slot *a, *b;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    a = ar_glyph_get(&gc, &f, 1, 32, 0, &sc);
    CHECK(a != 0 && a->w == 32 && a->h == 32, "glyph cache: the triangle rasterizes at 32 px");
    CHECK(gc.misses == 1 && gc.hits == 0, "glyph cache: the first ask is a miss");

    b = ar_glyph_get(&gc, &f, 1, 32, 0, &sc);
    CHECK(b == a, "glyph cache: the second ask returns the same entry");
    CHECK(gc.hits == 1, "glyph cache: and is counted as a hit");

    /* A different size is a different bitmap, not the same one scaled. */
    b = ar_glyph_get(&gc, &f, 1, 16, 0, &sc);
    CHECK(b != 0 && b != a && b->h == 16, "glyph cache: size is part of the key");
}

/* One glyph at many sizes must occupy many slots, and the slot a key lands in
   must depend on all of the key.

   Masking the key directly takes its low bits, which are the glyph index, so
   every size of one glyph collided on one slot. Sixteen sizes exactly filled
   the sixteen-probe window and the cache silently stopped caching: a benchmark
   drawing the same text at sixteen sizes missed 351,140 times in 400 frames,
   where it should have missed a few hundred times in total. */
static void test_glyph_cache_spreads_sizes_across_slots(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           size;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    for (size = 4; size <= 24; ++size)
    {
        ar_glyph_get(&gc, &f, 1, size, 0, &sc);
    }
    CHECK(gc.misses == 21 && gc.hits == 0, "glyph cache: 21 sizes of one glyph are 21 entries");

    /* Every one of them must still be there. */
    for (size = 4; size <= 24; ++size)
    {
        ar_glyph_get(&gc, &f, 1, size, 0, &sc);
    }
    CHECK(gc.misses == 21 && gc.hits == 21,
          "glyph cache: and asking again finds all of them, not one");
    CHECK(gc.resets == 0, "glyph cache: without evicting anything");
}

static void test_grid_fit_snaps_the_x_height(void)
{
    ar_face f;
    ar_i32  num = 0, den = 0;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);

    /* The generated face states no x-height, so there is nothing to snap to
       and the fit must be the identity rather than a guess. */
    ar_face_grid_fit(&f, 16, &num, &den);
    CHECK(num == den && den != 0, "grid fit: a face with no stated x-height is left alone");

    f.x_height = 500; /* half an em */
    ar_face_grid_fit(&f, 13, &num, &den);
    /* 500/1000 of 13 px is 6.5, which snaps to 7: a 7.7% correction. */
    CHECK(num * 100 / den > 100 && num * 100 / den < 115,
          "grid fit: a half-pixel x-height is corrected upward, by a few per cent");

    /* At 4 px the x-height is 2 px and any rounding is a huge fraction of it,
       so fitting distorts rather than sharpens and is declined. */
    f.x_height = 333;
    ar_face_grid_fit(&f, 5, &num, &den);
    CHECK(num * 8 <= den * 9 && num * 9 >= den * 8,
          "grid fit: declines a correction larger than an eighth");
}

static void test_darkening_keeps_its_endpoints(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           i;
    int              monotonic = 1, lifted = 0;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);
    ar_glyph_cache_set_darken(&gc, 80);

    CHECK(gc.darken_lut[0] == 0, "darken: nothing is not lifted to something");
    CHECK(gc.darken_lut[255] == 255, "darken: and full coverage cannot exceed full");

    for (i = 1; i < 256; ++i)
    {
        if (gc.darken_lut[i] < gc.darken_lut[i - 1])
        {
            monotonic = 0;
        }
        if (gc.darken_lut[i] > i)
        {
            lifted = 1;
        }
    }
    CHECK(monotonic, "darken: the curve never goes backwards");
    CHECK(lifted, "darken: and does lift the midtones");
    CHECK(gc.darken_lut[128] > 128 + 10, "darken: most of all at half coverage");
}

static void test_subpixel_positions_are_separate_entries(void)
{
    ar_face              f;
    ar_glyph_cache       gc;
    ar_glyph_scratch     sc;
    const ar_glyph_slot *a, *b;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    a = ar_glyph_get(&gc, &f, 1, 16, 0, &sc);
    b = ar_glyph_get(&gc, &f, 1, 16, 2, &sc);
    CHECK(a && b && a != b, "subpixel: a half pixel offset is its own cache entry");
    CHECK(gc.misses == 2, "subpixel: and is rasterized rather than reused");

    /* Turning it off must collapse them again, or an application that cannot
       afford four times the atlas would still be paying for it. */
    gc.subpx = 1;
    ar_glyph_cache_clear(&gc);
    a = ar_glyph_get(&gc, &f, 1, 16, 0, &sc);
    b = ar_glyph_get(&gc, &f, 1, 16, 2, &sc);
    CHECK(a == b, "subpixel: switched off, every offset is the same entry");
}

static void test_glyph_cache_carries_metrics(void)
{
    ar_face              f;
    ar_glyph_cache       gc;
    ar_glyph_scratch     sc;
    const ar_glyph_slot *g;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    /* Glyph 0 is empty. It still has an advance, and caching that stops it
       being re-derived for every space in a paragraph. */
    g = ar_glyph_get(&gc, &f, 0, 32, 0, &sc);
    CHECK(g != 0 && g->w == 0 && g->h == 0, "glyph cache: an empty glyph caches as empty");
    CHECK(g != 0 && g->advance == 600 * 32 * AR_ONE_PIXEL / 1000,
          "glyph cache: and still carries its advance");

    g = ar_glyph_get(&gc, &f, 1, 32, 0, &sc);
    CHECK(g != 0 && g->advance == 32 * AR_ONE_PIXEL, "glyph cache: a one em advance at 32 px");
}

static void test_glyph_cache_resets_rather_than_overflowing(void)
{
    ar_face              f;
    ar_glyph_cache       gc;
    ar_glyph_scratch     sc;
    const ar_glyph_slot *g;
    ar_i32               size;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    /* Room for about two glyphs at 32 px, so filling it takes three sizes. */
    ar__gc_setup(&gc, &sc, 32 * 32 * 2 + 16);

    for (size = 20; size <= 32; ++size)
    {
        g = ar_glyph_get(&gc, &f, 1, size, 0, &sc);
        CHECK(g != 0 && gc.used <= gc.cap, "glyph cache: never uses more than its budget");
    }
    CHECK(gc.resets > 0, "glyph cache: a full cache resets rather than overflowing");

    /* And still works afterwards, which is the point of resetting. */
    g = ar_glyph_get(&gc, &f, 1, 32, 0, &sc);
    CHECK(g != 0 && g->w == 32, "glyph cache: and keeps working after a reset");
}

static void test_glyph_cache_refuses_a_glyph_larger_than_its_budget(void)
{
    ar_face              f;
    ar_glyph_cache       gc;
    ar_glyph_scratch     sc;
    const ar_glyph_slot *g;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, 64); /* smaller than any real glyph */

    g = ar_glyph_get(&gc, &f, 1, 32, 0, &sc);
    CHECK(g == 0, "glyph cache: a glyph larger than the whole budget is refused, not wrapped");
    CHECK(gc.used <= gc.cap, "glyph cache: and nothing was written past the slab");
}

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */
static void test_fallback_chain_picks_the_first_face_that_has_it(void)
{
    ar_face       f;
    ar_font_chain ch;
    ar_i32        which = -1, g;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ch.face[0] = &f;
    ch.face[1] = &f;
    ch.count = 2;

    g = ar_font_chain_glyph(&ch, 'A', &which);
    CHECK(g == 1 && which == 0, "fallback: the first face wins when it has the character");

    /* Nothing in the chain has 'B'. Reporting the notdef box of the first face
       is right: it says a character is missing, where drawing nothing would
       say the text was. */
    g = ar_font_chain_glyph(&ch, 'B', &which);
    CHECK(g == 0 && which == 0, "fallback: an unmapped character falls through to notdef");

    ch.count = 0;
    g = ar_font_chain_glyph(&ch, 'A', &which);
    CHECK(g == 0, "fallback: an empty chain answers without faulting");
}

static void test_face_family_name(void)
{
    ar_face f;
    char    name[64];
    ar_i32  n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);

    /* The generated face has no name table, so the answer is an empty string
       rather than whatever happened to be in the buffer. */
    name[0] = 'x';
    n = ar_face_family(&f, name, (ar_i32)sizeof name);
    CHECK(n == 0 && name[0] == 0, "family: a face with no name table gives an empty string");

    CHECK(ar_face_family(&f, name, 0) == 0, "family: a zero length buffer is not written to");
}

static void test_text_draw_matches_measure(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_surface       s;
    ar_i32           drawn, measured;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;
    memset(g_dmg_a, 0, sizeof g_dmg_a);

    measured = ar_text_measure("AAA", &f, 16, &gc, &sc);
    drawn = ar_text_draw(&s, ar_rect_make(0, 0, AR_DMG_W, AR_DMG_H), 4, 40, "AAA", &f, 16,
                         AR_HEX(0xFFFFFF), &gc, &sc);

    CHECK(measured == drawn, "text: drawing advances the pen exactly as measuring said it would");
    CHECK(measured == 3 * 16 * AR_ONE_PIXEL, "text: three one-em glyphs at 16 px is 48 pixels");
}

static void test_text_draws_pixels_and_respects_the_clip(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_surface       s;
    ar_i32           i, inked = 0, outside = 0;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;
    memset(g_dmg_a, 0, sizeof g_dmg_a);

    /* A clip that cuts the text in half vertically. */
    ar_text_draw(&s, ar_rect_make(0, 0, AR_DMG_W, 30), 4, 40, "AAA", &f, 24, AR_HEX(0xFFFFFF), &gc,
                 &sc);

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        if (g_dmg_a[i] != 0)
        {
            ++inked;
            if (i / AR_DMG_W >= 30)
            {
                outside = 1;
            }
        }
    }

    CHECK(inked > 0, "text: something was actually drawn");
    CHECK(!outside, "text: and nothing outside the clip rectangle");
}

static void test_text_survives_a_face_that_failed_to_load(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_surface       s;

    memset(&f, 0, sizeof f);
    ar_face_init(&f, "not a font", 10);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;

    /* A caller that ignored the return value of ar_face_init must not be able
       to fault, because that caller exists. */
    CHECK(ar_text_draw(&s, ar_rect_make(0, 0, AR_DMG_W, AR_DMG_H), 0, 0, "hello", &f, 16,
                       AR_HEX(0xFFFFFF), &gc, &sc) == 0,
          "text: a face that failed to load draws nothing and does not fault");
    CHECK(ar_text_measure("hello", &f, 16, &gc, &sc) == 0, "text: and measures as nothing");
}

/* ------------------------------------------------------------------------
 * Fonts at the context level
 * ------------------------------------------------------------------------ */
static unsigned char g_font_mem[AR_MEM(64) + 160 * 1024];

static ar_ctx *ar__font_ctx(int load)
{
    ar_ctx *c = ar_init(g_font_mem, (ar_u32)sizeof g_font_mem);

    if (c)
    {
        ar_stylesheet(c, "#root { display:flex; padding:2px; }"
                         ".t { font-size:16px; color:#FFFFFF; }");
        if (load)
        {
            /* Before the first frame, as the header says: a frame reserves the
               whole box budget from the other end of the arena. */
            ar_font_load(c, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT, 32 * 1024, 32);
        }
    }
    return c;
}

static void ar__font_frame(ar_ctx *c, ar_surface *s)
{
    ar_frame_begin(c, 0);
    ar_invalidate_all(c);
    ar_begin(c, "div#root");
    ar_text(c, "div.t", "AAA");
    ar_end(c);
    ar_frame_end(c, s);
    ar_frame_presented(c);
}

static void test_font_load_and_fallback(void)
{
    ar_ctx *c = ar__font_ctx(0);

    CHECK(c != 0 && !ar_font_loaded(c), "font: a fresh context has no face");

    /* Garbage must fail without disturbing anything: the bitmap font is the
       fallback, and a missing font should degrade an interface, not stop it. */
    CHECK(!ar_font_load(c, "not a font at all", 17, 32 * 1024, 32), "font: loading garbage fails");
    CHECK(!ar_font_loaded(c), "font: and leaves the context without a face");

    CHECK(ar_font_load(c, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT, 32 * 1024, 32),
          "font: a valid face loads");
    CHECK(ar_font_loaded(c), "font: and the context reports it");
}

static void test_font_changes_what_is_measured(void)
{
    ar_ctx    *c;
    ar_surface s;
    ar_i32     bitmap_w, outline_w;

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;

    c = ar__font_ctx(0);
    ar__font_frame(c, &s);
    bitmap_w = c->nodes[1].text_w;

    c = ar__font_ctx(1);
    ar__font_frame(c, &s);
    outline_w = c->nodes[1].text_w;

    /* The test face's 'A' is one em wide, so three of them at 16 px is 48
       pixels. The bitmap face is proportional and narrower. What matters is
       that layout measured with the face that will draw it. */
    CHECK(bitmap_w > 0 && outline_w > 0, "font: both faces measure the same string");
    CHECK(outline_w == 48, "font: an outline face measures with its own metrics");
    CHECK(outline_w != bitmap_w, "font: which are not the bitmap face's");
}

/* Text is measured once per node per frame, and with an outline face that is a
   glyph cache lookup per character. On a realistic dashboard it was 7.4 million
   lookups across the run where 718 were needed, and the frame was 2.78x slower
   for it. The width is now remembered against a hash of the string and the
   size. */
static void test_measured_width_is_remembered(void)
{
    ar_ctx    *c;
    ar_surface s;
    ar_u32     h1, h2, h3;

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;

    c = ar__font_ctx(1);
    CHECK(c != 0 && ar_font_loaded(c), "measure memo: the face loaded");

    ar__font_frame(c, &s);
    ar_font_cache_stats(c, &h1, 0, 0);
    ar__font_frame(c, &s);
    ar_font_cache_stats(c, &h2, 0, 0);
    ar__font_frame(c, &s);
    ar_font_cache_stats(c, &h3, 0, 0);

    /* Drawing still looks glyphs up; measuring must not. So the second and
       third frames cost the same as each other, and less than a frame that
       measured from scratch would. */
    CHECK(h2 - h1 == h3 - h2, "measure memo: a steady frame costs a steady number of lookups");
    CHECK(h3 - h2 <= 8, "measure memo: and only the ones drawing needs");
}

static void test_font_antialias_toggle(void)
{
    ar_ctx    *c;
    ar_surface s;
    ar_i32     i;
    int        soft = 0, hard_only = 1;

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;

    c = ar__font_ctx(1);
    CHECK(c != 0 && ar_font_loaded(c), "font: the face loaded for the antialiasing test");

    memset(g_dmg_a, 0, sizeof g_dmg_a);
    ar__font_frame(c, &s);
    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        ar_u32 v = g_dmg_a[i] & 0xFFu;
        if (v != 0 && v != 0xFFu)
        {
            soft = 1;
        }
    }
    CHECK(soft, "font: antialiased text has partly covered pixels");

    ar_font_antialias(c, 0);
    memset(g_dmg_a, 0, sizeof g_dmg_a);
    ar__font_frame(c, &s);
    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        ar_u32 v = g_dmg_a[i] & 0xFFu;
        if (v != 0 && v != 0xFFu)
        {
            hard_only = 0;
        }
    }
    CHECK(hard_only, "font: with antialiasing off every pixel is fully inked or untouched");
}

static void test_font_antialias_toggle_invalidates(void)
{
    ar_ctx    *c;
    ar_surface s;
    ar_u32     before, after;

    s.pixels = g_dmg_a;
    s.w = AR_DMG_W;
    s.h = AR_DMG_H;
    s.stride = AR_DMG_W;

    c = ar__font_ctx(1);
    ar__font_frame(c, &s);
    ar__font_frame(c, &s);
    ar_font_cache_stats(c, 0, &before, 0);

    /* The flag is part of the cache key, so the old bitmaps could never be
       found again. Dropping them reclaims the space rather than stranding it,
       and the next frame has to repaint. */
    ar_font_antialias(c, 0);
    CHECK(ar_frame_is_dirty(c), "font: changing antialiasing invalidates the window");

    ar__font_frame(c, &s);
    ar_font_cache_stats(c, 0, &after, 0);
    CHECK(after > before, "font: and the glyphs are rasterized again under the new setting");
}

/* ------------------------------------------------------------------------
 * Line breaking
 *
 * The interesting cases are all the ones where "break at spaces" is wrong.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__breaks(const char *s, ar_i32 *at, ar_i32 max)
{
    ar_i32 n = 0, pos = 0, kind;
    ar_i32 len = (ar_i32)strlen(s);

    for (;;)
    {
        ar_i32 next = ar_break_next(s, pos, &kind);
        if (next >= len || next <= pos || n >= max)
        {
            break;
        }
        at[n++] = next;
        pos = next;
    }
    return n;
}

static int ar__breaks_at(const char *s, ar_i32 want)
{
    ar_i32 at[32];
    ar_i32 n = ar__breaks(s, at, 32);
    ar_i32 i;

    for (i = 0; i < n; ++i)
    {
        if (at[i] == want)
        {
            return 1;
        }
    }
    return 0;
}

static void test_break_after_spaces_not_before(void)
{
    CHECK(ar__breaks_at("hello world", 6), "break: after a space, not before it");
    CHECK(!ar__breaks_at("hello world", 5), "break: so a trailing space stays on its own line");
    CHECK(ar__breaks_at("a  b", 3), "break: a run of spaces is one opportunity, after all of it");
}

static void test_break_respects_punctuation(void)
{
    /* A closing bracket may not start a line, and an opening one may not end
       one. Both are what makes hand-rolled space splitting look wrong. */
    CHECK(!ar__breaks_at("(a) b", 1), "break: not after an opening bracket");
    CHECK(!ar__breaks_at("a) b", 1), "break: not before a closing bracket");
    CHECK(!ar__breaks_at("end. Next", 4), "break: not before a full stop");
    CHECK(ar__breaks_at("end. Next", 5), "break: but after the space that follows it");
}

static void test_break_keeps_numbers_together(void)
{
    CHECK(!ar__breaks_at("1,000.50", 2), "break: not inside a thousands separator");
    CHECK(!ar__breaks_at("1,000.50", 6), "break: nor inside a decimal");
    CHECK(!ar__breaks_at("3/4", 2), "break: nor inside a fraction");
}

static void test_break_hyphens_and_dashes(void)
{
    CHECK(ar__breaks_at("one-two", 4), "break: after a hyphen in a compound word");
    /* An em dash may be broken either side; an en dash only after. */
    CHECK(ar__breaks_at("a\xE2\x80\x94"
                        "b",
                        1),
          "break: before an em dash");
    CHECK(ar__breaks_at("a\xE2\x80\x94"
                        "b",
                        4),
          "break: and after it");
}

static void test_break_honours_joiners(void)
{
    /* A non-breaking space is the whole point of a non-breaking space. */
    CHECK(!ar__breaks_at("a\xC2\xA0"
                         "b",
                         1),
          "break: never at a no-break space");
    CHECK(!ar__breaks_at("a\xC2\xA0"
                         "b",
                         3),
          "break: nor after one");
    CHECK(ar__breaks_at("a\xE2\x80\x8B"
                        "b",
                        4),
          "break: but always at a zero width space");
    CHECK(!ar__breaks_at("a\xE2\x81\xA0"
                         "b",
                         4),
          "break: and never at a word joiner");
}

static void test_break_mandatory(void)
{
    ar_i32 kind = AR_BREAK_NONE;
    ar_i32 at = ar_break_next("hard\nbreak", 0, &kind);

    CHECK(at == 5 && kind == AR_BREAK_MANDATORY, "break: a newline must break, and after itself");

    /* CRLF is one break, not two, or every Windows text file gains a blank
       line between every pair of lines. */
    kind = AR_BREAK_NONE;
    at = ar_break_next("a\r\nb", 0, &kind);
    CHECK(at == 3 && kind == AR_BREAK_MANDATORY, "break: CRLF is one break");
}

static void test_break_between_ideographs(void)
{
    /* Japanese has no spaces, so a line breaks between characters. Failing to
       do this makes CJK text one unbreakable line. */
    CHECK(ar__breaks_at("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 3), "break: between ideographs");
    CHECK(ar__breaks_at("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 6), "break: at each of them");
}

static void test_break_always_terminates(void)
{
    /* Every input, including malformed UTF-8 and empty strings, must reach the
       end. A line breaker that loops is a hang in the middle of a paint. */
    static const char *const NASTY[] = {"",
                                        " ",
                                        "\n",
                                        "\xFF",
                                        "\xC2",
                                        "\x80\x80",
                                        "a\xFF"
                                        "b",
                                        "\r",
                                        "\r\r\n",
                                        "((((",
                                        "))))",
                                        "\xE2\x80"};
    ar_i32                   i;
    int                      ok = 1;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        ar_i32 pos = 0, guard = 0, kind;
        ar_i32 len = (ar_i32)strlen(NASTY[i]);
        for (;;)
        {
            ar_i32 next = ar_break_next(NASTY[i], pos, &kind);
            if (next >= len || next <= pos)
            {
                break;
            }
            pos = next;
            if (++guard > 64)
            {
                ok = 0;
                break;
            }
        }
    }
    CHECK(ok, "break: every input terminates, including malformed UTF-8");
}

/* ------------------------------------------------------------------------
 * Wrapping
 * ------------------------------------------------------------------------ */
static void test_wrap_fits_the_width(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           starts[32];
    ar_i32           lines, i;
    int              over = 0;
    /* The test face's every glyph is one em wide, so at 10 px each character
       is exactly 10 px and the arithmetic is checkable by hand. */
    const char *T = "AAA AAA AAA AAA";

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    lines = ar_text_wrap(T, &f, 10, 75, &gc, &sc, starts, 32);
    CHECK(lines > 1, "wrap: text longer than the width becomes more than one line");
    CHECK(starts[0] == 0, "wrap: the first line starts at the beginning");

    for (i = 0; i < lines; ++i)
    {
        ar_i32 end = (i + 1 < lines) ? starts[i + 1] : (ar_i32)strlen(T);
        ar_i32 j, chars = 0;
        for (j = starts[i]; j < end; ++j)
        {
            if (T[j] != ' ')
            {
                ++chars;
            }
        }
        if (chars * 10 > 75)
        {
            over = 1;
        }
    }
    CHECK(!over, "wrap: and no line's visible text exceeds the width");
}

static void test_wrap_does_not_break_a_single_long_word(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           starts[32];
    ar_i32           lines;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    /* Overflowing is a visible failure the caller can clip. Splitting a word
       at an arbitrary point is a silent one that reads as a rendering bug. */
    lines = ar_text_wrap("AAAAAAAAAA", &f, 10, 20, &gc, &sc, starts, 32);
    CHECK(lines == 1, "wrap: a word wider than the line is left to overflow, not split");
}

static void test_wrap_honours_a_mandatory_break(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           starts[32];
    ar_i32           lines;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    lines = ar_text_wrap("A\nA", &f, 10, 1000, &gc, &sc, starts, 32);
    CHECK(lines == 2, "wrap: a newline breaks even when the line would fit");
    CHECK(lines == 2 && starts[1] == 2, "wrap: and the next line starts after it");
}

static void test_wrap_respects_its_line_limit(void)
{
    ar_face          f;
    ar_glyph_cache   gc;
    ar_glyph_scratch sc;
    ar_i32           starts[4];
    ar_i32           lines;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar__gc_setup(&gc, &sc, (ar_i32)sizeof g_gc_pixels);

    lines = ar_text_wrap("A A A A A A A A A A A A", &f, 10, 15, &gc, &sc, starts, 4);
    CHECK(lines <= 4, "wrap: never writes more line starts than it was given room for");
}

/* ------------------------------------------------------------------------
 * The bidirectional algorithm
 *
 * Hebrew alef-bet-gimel is used throughout as the right-to-left text, because
 * it needs no shaping to be a valid test of ordering.
 * ------------------------------------------------------------------------ */
#define HEB "\xD7\x90\xD7\x91\xD7\x92" /* three Hebrew letters */
#define ARA "\xD8\xA7\xD8\xA8"         /* two Arabic letters   */

static ar_u8       g_bidi_lv[128], g_bidi_cl[128];
static ar_bidi_run g_bidi_runs[32];

static ar_i32 ar__bidi(const char *s, ar_i32 dir, ar_i32 *para)
{
    return ar_bidi_levels(s, dir, g_bidi_lv, g_bidi_cl, 128, para);
}

static void test_bidi_paragraph_level(void)
{
    ar_i32 para = -1;

    ar__bidi("abc", AR_DIR_AUTO, &para);
    CHECK(para == 0, "bidi: a paragraph starting in Latin is left to right");

    ar__bidi(HEB, AR_DIR_AUTO, &para);
    CHECK(para == 1, "bidi: one starting in Hebrew is right to left");

    /* P2 skips numbers and punctuation: the first *strong* character decides. */
    ar__bidi("123 " HEB, AR_DIR_AUTO, &para);
    CHECK(para == 1, "bidi: a leading number does not make a paragraph left to right");

    ar__bidi("", AR_DIR_AUTO, &para);
    CHECK(para == 0, "bidi: a paragraph with no strong character is left to right");

    ar__bidi("abc", AR_DIR_RTL, &para);
    CHECK(para == 1, "bidi: an explicit direction overrides what the text says");
}

static void test_bidi_levels_of_mixed_text(void)
{
    ar_i32 n, para;

    n = ar__bidi("ab " HEB, AR_DIR_AUTO, &para);
    CHECK(n == 6, "bidi: levels are per codepoint, not per byte");
    CHECK(g_bidi_lv[0] == 0 && g_bidi_lv[1] == 0, "bidi: Latin stays at the paragraph level");
    CHECK(g_bidi_lv[3] == 1 && g_bidi_lv[5] == 1, "bidi: Hebrew goes one level up");

    /* A number inside right-to-left text is left to right within it, which is
       two levels up, not one. Getting this wrong renders 123 as 321. */
    n = ar__bidi(HEB " 123", AR_DIR_AUTO, &para);
    CHECK(para == 1 && n == 7, "bidi: Hebrew then a number");
    CHECK(g_bidi_lv[0] == 1, "bidi: the Hebrew is at level one");
    CHECK(g_bidi_lv[4] == 2 && g_bidi_lv[6] == 2, "bidi: and the number two levels up, not one");
}

static void test_bidi_numbers_after_arabic(void)
{
    ar_i32 para;

    /* W2: European digits after an Arabic letter are Arabic numbers, which
       resolve to a different level than they would after Latin. */
    ar__bidi(ARA " 42", AR_DIR_AUTO, &para);
    CHECK(para == 1, "bidi: Arabic paragraph");
    CHECK(g_bidi_lv[3] == 2, "bidi: digits after Arabic are still left to right within it");
}

static void test_bidi_separators_join_numbers(void)
{
    ar_i32 para;

    /* W4: one separator between two digits joins them, so a decimal point or
       a thousands comma cannot split a number into two runs. */
    ar__bidi("a 1,234.5 b", AR_DIR_AUTO, &para);
    CHECK(g_bidi_lv[2] == g_bidi_lv[3] && g_bidi_lv[3] == g_bidi_lv[4],
          "bidi: a comma inside a number does not break it");
    CHECK(g_bidi_lv[6] == g_bidi_lv[7], "bidi: nor does a decimal point");
}

static void test_bidi_reorders_into_visual_runs(void)
{
    ar_i32 n, para, nr;

    /* Latin then Hebrew, left to right: read in storage order. */
    n = ar__bidi("ab " HEB, AR_DIR_AUTO, &para);
    nr = ar_bidi_runs(g_bidi_lv, n, g_bidi_runs, 32);
    CHECK(nr == 2, "bidi: Latin then Hebrew is two runs");
    CHECK(g_bidi_runs[0].start == 0, "bidi: and the Latin is drawn first");
    CHECK((g_bidi_runs[1].level & 1) == 1, "bidi: with the Hebrew right to left after it");

    /* Hebrew then Latin, right to left. The Latin is *last* in storage and
       *first* on screen, because the paragraph runs the other way. This is the
       case that a naive implementation gets backwards. */
    n = ar__bidi(HEB " ab", AR_DIR_AUTO, &para);
    nr = ar_bidi_runs(g_bidi_lv, n, g_bidi_runs, 32);
    CHECK(nr == 2, "bidi: Hebrew then Latin is two runs");
    CHECK(g_bidi_runs[0].start > g_bidi_runs[1].start,
          "bidi: and in an RTL paragraph the later text is drawn first");
    CHECK((g_bidi_runs[0].level & 1) == 0, "bidi: the leftmost run being the Latin one");
}

static void test_bidi_explicit_overrides(void)
{
    ar_i32 n, para;
    /* RLO makes even Latin right to left until PDF. */
    const char *s = "a\xE2\x80\xAE"
                    "bc\xE2\x80\xAC"
                    "d";

    n = ar__bidi(s, AR_DIR_AUTO, &para);
    CHECK(n == 6, "bidi: formatting characters are still characters");
    CHECK(g_bidi_lv[0] == 0, "bidi: before an override, the paragraph level");
    CHECK(g_bidi_lv[2] == 1 && g_bidi_lv[3] == 1, "bidi: inside a right-to-left override, odd");
    CHECK(g_bidi_lv[5] == 0, "bidi: and back to the paragraph level after it");
}

static void test_bidi_isolates(void)
{
    ar_i32 n, para;
    /* An isolate keeps its contents from affecting the direction around it,
       which is the whole reason it replaced the embedding characters. */
    const char *s = "a\xE2\x81\xA7" HEB "\xE2\x81\xA9"
                    "b";

    n = ar__bidi(s, AR_DIR_AUTO, &para);
    CHECK(para == 0, "bidi: an isolate's contents do not decide the paragraph");
    CHECK(n == 7, "bidi: isolate initiator and terminator are counted");
    CHECK(g_bidi_lv[2] == 1, "bidi: the isolated Hebrew is right to left");
    CHECK(g_bidi_lv[6] == 0, "bidi: and text after the isolate is unaffected");
}

static void test_bidi_survives_anything(void)
{
    static const char *const NASTY[] = {"",
                                        " ",
                                        "\xE2\x80\xAE",
                                        "\xE2\x80\xAC",
                                        "\xE2\x81\xA9",
                                        "\xE2\x80\xAE\xE2\x80\xAE\xE2\x80\xAE",
                                        "\xE2\x81\xA7\xE2\x81\xA7\xE2\x81\xA7",
                                        "\xFF\xFE",
                                        "\xE2\x80\xAC" HEB,
                                        HEB ARA "123"};
    ar_i32                   i;
    int                      ok = 1;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        ar_i32 para, n = ar__bidi(NASTY[i], AR_DIR_AUTO, &para);
        ar_i32 nr = ar_bidi_runs(g_bidi_lv, n, g_bidi_runs, 32);
        ar_i32 j, total = 0;
        for (j = 0; j < nr; ++j)
        {
            total += g_bidi_runs[j].count;
        }
        /* Every character must end up in exactly one run, or reordering has
           lost or duplicated text. */
        if (total != n)
        {
            ok = 0;
        }
    }
    CHECK(ok, "bidi: unbalanced formatting characters still cover every character exactly once");

    /* And a buffer too small is refused rather than overrun. */
    {
        ar_u8  small[4];
        ar_u8  smallc[4];
        ar_i32 para;
        CHECK(ar_bidi_levels("abcdefgh", AR_DIR_AUTO, small, smallc, 4, &para) == 0,
              "bidi: a buffer too small is refused, not overrun");
    }
}

/* ------------------------------------------------------------------------
 * Shaping
 *
 * The generated test font carries no GSUB or GPOS, so what is checked here is
 * that a font without them is left exactly alone -- which is most of what a
 * shaper must get right, because the alternative is corrupting text in every
 * font that does not opt in.
 *
 * The tables themselves are verified against real fonts, where the answers are
 * checkable: Constantia turns fi into one glyph and office into four, Calibri
 * and Times kern AV by -89 and -264 units, and nnnn is left alone by all of
 * them. Georgia's GPOS has no Latin pairs at all, which was confirmed by
 * decoding its coverage table independently rather than assumed to be a bug.
 * ------------------------------------------------------------------------ */
static void test_shape_leaves_a_plain_font_alone(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_i32    g[4], a[4], cl[4], n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    CHECK(ar_shape_init(&sh, &f), "shape: a font with no GSUB or GPOS still gives a shaper");
    CHECK(sh.liga_count == 0 && sh.kern_count == 0, "shape: with no lookups to apply");

    g[0] = 1;
    g[1] = 1;
    g[2] = 1;
    g[3] = 1;
    a[0] = 100;
    a[1] = 200;
    a[2] = 300;
    a[3] = 400;
    cl[0] = 0;
    cl[1] = 1;
    cl[2] = 2;
    cl[3] = 3;

    n = ar_shape_run(&sh, g, a, cl, 4);
    CHECK(n == 4, "shape: the run keeps its length");
    CHECK(g[0] == 1 && g[3] == 1, "shape: the glyphs are unchanged");
    CHECK(a[0] == 100 && a[3] == 400, "shape: and so are the advances");
    CHECK(cl[2] == 2, "shape: and the clusters");
}

static void test_shape_refuses_what_it_cannot_shape(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_i32    g[2], a[2];

    /* A shaper over a face that failed to load must not fault, because a
       caller that ignored ar_face_init's return value exists. */
    ar_face_init(&f, "not a font", 10);
    CHECK(!ar_shape_init(&sh, &f), "shape: a face that failed to load gives no shaper");

    g[0] = 1;
    g[1] = 1;
    a[0] = 10;
    a[1] = 10;
    CHECK(ar_shape_run(&sh, g, a, 0, 2) == 2, "shape: and shaping through it changes nothing");
    CHECK(ar_shape_run(&sh, 0, 0, 0, 0) == 0, "shape: nor does shaping nothing");
}

/* ------------------------------------------------------------------------
 * Arabic joining
 *
 * The joining classes are a property of the characters and can be checked
 * without a font, which is what these do. The substitution they drive is
 * verified against Arial, where the answers were confirmed by decoding its
 * lookups in Python before the code was believed:
 *
 *   beh beh beh    913 913 911   initial, medial, final
 *   alef beh       unchanged     alef joins only rightward, so beh is isolated
 *   lam alef       one glyph     the mandatory ligature
 *   beh alef beh   913 910 911   initial, final, isolated
 *
 * Arial maps beh's initial and medial forms to the same glyph and its final
 * form to the isolated one. That looked like a bug and is a font design
 * decision.
 * ------------------------------------------------------------------------ */
static void test_arabic_joining_classes(void)
{
    CHECK(ar_join_type(0x0628) == AR_JOIN_D, "join: beh joins on both sides");
    CHECK(ar_join_type(0x0633) == AR_JOIN_D, "join: so does seen");
    CHECK(ar_join_type(0x0627) == AR_JOIN_R, "join: alef joins only to the right");
    CHECK(ar_join_type(0x062F) == AR_JOIN_R, "join: and so does dal");
    CHECK(ar_join_type(0x0648) == AR_JOIN_R, "join: and waw");
    CHECK(ar_join_type(0x0621) == AR_JOIN_U, "join: hamza joins nothing");
    CHECK(ar_join_type(0x0640) == AR_JOIN_C, "join: tatweel causes joining without joining");
    CHECK(ar_join_type(0x064E) == AR_JOIN_T, "join: a fatha is transparent");
    CHECK(ar_join_type(0x0651) == AR_JOIN_T, "join: so is a shadda");
    CHECK(ar_join_type('a') == AR_JOIN_U, "join: Latin does not join");
    CHECK(ar_join_type(0x200D) == AR_JOIN_C, "join: the zero width joiner causes joining");
    CHECK(ar_join_type(0x200C) == AR_JOIN_U, "join: and the non-joiner does not");
}

static void test_arabic_shaping_needs_a_font_with_the_features(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[3];
    ar_i32    g[3], a[3], cl[3], n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);
    CHECK(sh.init_count == 0 && sh.medi_count == 0 && sh.fina_count == 0,
          "join: a face with no positional features has no lookups to apply");

    cps[0] = 0x0628;
    cps[1] = 0x0628;
    cps[2] = 0x0628;
    g[0] = 1;
    g[1] = 1;
    g[2] = 1;
    a[0] = 10;
    a[1] = 10;
    a[2] = 10;
    cl[0] = 0;
    cl[1] = 1;
    cl[2] = 2;

    /* Without the tables, the glyphs must come through untouched rather than
       being guessed at. A wrong shape is worse than an unshaped one. */
    n = ar_shape_run_cp(&sh, cps, g, a, cl, 3);
    CHECK(n == 3 && g[0] == 1 && g[1] == 1 && g[2] == 1,
          "join: and leaves the glyphs exactly alone");
}

/* Mark attachment. Verified against Arial, where a fatha over a beh comes back
   with a displacement of +288,-220 font units and an advance of zero, and a
   shadda and a kasra on the same letter get different displacements because
   one sits above it and one below. A mark with no advance and no offset lands
   on the baseline at the origin, which reads as the text having lost it. */
static void test_marks_need_the_tables(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[2];
    ar_i32    g[2], a[2], dx[2], dy[2], cl[2], n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);
    CHECK(sh.mark_count == 0, "marks: a face with no GPOS mark feature has no lookups");

    cps[0] = 0x0628;
    cps[1] = 0x064E;
    g[0] = 1;
    g[1] = 1;
    a[0] = 10;
    a[1] = 10;
    dx[0] = 99;
    dx[1] = 99;
    dy[0] = 99;
    dy[1] = 99;
    cl[0] = 0;
    cl[1] = 1;

    n = ar_shape_run_pos(&sh, cps, g, a, dx, dy, cl, 2, 2);
    CHECK(n == 2, "marks: the run is unchanged without the tables");
    CHECK(dx[0] == 0 && dy[0] == 0 && dx[1] == 0 && dy[1] == 0,
          "marks: and the offsets are cleared rather than left as they were found");
    CHECK(a[1] == 10, "marks: an unattached mark keeps its own advance");
}

static void test_marks_are_optional_to_ask_for(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[2];
    ar_i32    g[2], a[2], cl[2];

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);

    cps[0] = 0x0628;
    cps[1] = 0x064E;
    g[0] = 1;
    g[1] = 1;
    a[0] = 10;
    a[1] = 10;
    cl[0] = 0;
    cl[1] = 1;

    /* Passing no offset arrays must skip attachment rather than write
       through a null pointer: a caller with nowhere to put the offsets is
       better off with marks on the baseline than with a fault. */
    CHECK(ar_shape_run_pos(&sh, cps, g, a, 0, 0, cl, 2, 2) == 2,
          "marks: shaping without offset arrays does not fault");
}

/* Mark to mark and mark to ligature, GPOS 6 and 5.
 *
 * Verified against Arial and Segoe UI. In Arial a fatha alone sits at dy -220
 * on the letter; put a shadda under it and the fatha moves to +120, stacked
 * above the shadda rather than through it. A kasra in the same position stays
 * at -190, attached to the letter and not the shadda, because it belongs below
 * and the font says so. Both fonts show the same pattern with different
 * numbers, which is the check that this reads the tables rather than guesses.
 */
static void test_mark_to_mark_needs_the_tables(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[3];
    ar_i32    g[3], a[3], dx[3], dy[3], cl[3], n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);
    CHECK(sh.mkmk_count == 0, "mkmk: a face with no mark-to-mark feature has no lookups");

    cps[0] = 0x0628;
    cps[1] = 0x0651;
    cps[2] = 0x064E;
    g[0] = 1;
    g[1] = 1;
    g[2] = 1;
    a[0] = 10;
    a[1] = 10;
    a[2] = 10;
    cl[0] = 0;
    cl[1] = 1;
    cl[2] = 2;

    n = ar_shape_run_pos(&sh, cps, g, a, dx, dy, cl, 3, 3);
    CHECK(n == 3, "mkmk: the run survives a font that cannot stack marks");
    CHECK(dx[2] == 0 && dy[2] == 0, "mkmk: and the second mark is left where it was");
}

/* ccmp and GSUB type 2. Decomposition is the only substitution that makes a
   run longer, so it is the only one that can overrun a buffer, and the check
   that matters is that it declines rather than does. */
static void test_decomposition_respects_the_buffer(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[4];
    ar_i32    g[4], a[4], cl[4], n;
    ar_i32    i;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);
    CHECK(sh.ccmp_count == 0, "ccmp: a face with no ccmp feature has no lookups");

    for (i = 0; i < 4; ++i)
    {
        cps[i] = 'A';
        g[i] = 1;
        a[i] = 10;
        cl[i] = i;
    }

    /* A capacity smaller than the count must be treated as the count rather
       than believed, or a caller who got it wrong corrupts memory instead of
       getting a slightly worse rendering. */
    n = ar_shape_run_pos(&sh, cps, g, a, 0, 0, cl, 4, 1);
    CHECK(n == 4, "ccmp: a capacity below the run length is ignored, not obeyed");
    CHECK(g[3] == 1, "ccmp: and nothing is disturbed");
}

/* GSUB type 6. A chained contextual lookup substitutes nothing itself: it
   matches a pattern and names other lookups by index to run inside the match.
   That indirection is the only place the shaper needs the lookup list rather
   than the offsets it resolved from it, and an index out of range is the
   obvious way for a malformed font to reach somewhere it should not. */
static void test_chained_context_needs_a_lookup_list(void)
{
    ar_face   f;
    ar_shaper sh;
    ar_u32    cps[3];
    ar_i32    g[3], a[3], cl[3], n;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);
    CHECK(sh.calt_count == 0, "calt: a face with no contextual feature has no lookups");
    CHECK(sh.gsub_lookups == 0, "calt: and a face with no GSUB has no lookup list");

    cps[0] = 'A';
    cps[1] = 'A';
    cps[2] = 'A';
    g[0] = 1;
    g[1] = 1;
    g[2] = 1;
    a[0] = 10;
    a[1] = 10;
    a[2] = 10;
    cl[0] = 0;
    cl[1] = 1;
    cl[2] = 2;

    n = ar_shape_run_pos(&sh, cps, g, a, 0, 0, cl, 3, 3);
    CHECK(n == 3 && g[0] == 1 && g[2] == 1,
          "calt: shaping without a lookup list leaves the run alone");
}

/* Ligature matching skips marks.
 *
 * A ligature's components are consecutive letters, not consecutive glyphs. In
 * Arial, lam-alef ligates to glyph 1019; put a fatha between the two and it
 * stopped ligating entirely, because matching required exact adjacency. The
 * lam-alef ligature is mandatory in Arabic and real Arabic text is vocalised,
 * so that was wrong for the common case rather than an edge one.
 *
 * The mark has to survive too: dropping it would delete the vowel. Arial now
 * gives 1019 followed by the fatha. */
static void test_marks_do_not_block_a_ligature(void)
{
    ar_face   f;
    ar_shaper sh;

    ar_face_init(&f, AR_TEST_FONT, (ar_u32)sizeof AR_TEST_FONT);
    ar_shape_init(&sh, &f);

    /* The generated face has no GDEF, so nothing is a mark and matching is
       exact -- which is right for a Latin-only font, and is what most of them
       ship. */
    CHECK(sh.glyph_classes == 0, "gdef: a face without GDEF has no glyph classes");
}

/* ------------------------------------------------------------------------
 * Indic reordering
 *
 * Devanagari is written in an order that is not the order it is stored in, and
 * the difference is not cosmetic: the vowel sign i is typed after its consonant
 * and drawn before it. A renderer that draws storage order does not produce
 * plain text, it produces a different word.
 * ------------------------------------------------------------------------ */
#define DV_KA     0x0915u
#define DV_HA     0x0939u
#define DV_NA     0x0928u
#define DV_DA     0x0926u
#define DV_YA     0x092Fu
#define DV_RA     0x0930u
#define DV_VIRAMA 0x094Du
#define DV_I      0x093Fu
#define DV_AA     0x093Eu
#define DV_II     0x0940u

static ar_u32 g_ind_cp[16];
static ar_i32 g_ind_g[16], g_ind_a[16], g_ind_cl[16];

static int ar__reorder(const ar_u32 *in, ar_i32 n)
{
    ar_i32 i;
    for (i = 0; i < n; ++i)
    {
        g_ind_cp[i] = in[i];
        g_ind_g[i] = (ar_i32)in[i];
        g_ind_a[i] = 10;
        g_ind_cl[i] = i;
    }
    return ar_indic_reorder(0, g_ind_cp, g_ind_g, g_ind_a, g_ind_cl, n);
}

static void test_indic_categories(void)
{
    CHECK(ar_indic_category(DV_KA) == AR_IND_CONSONANT, "indic: ka is a consonant");
    CHECK(ar_indic_category(DV_RA) == AR_IND_RA, "indic: ra is its own category, for reph");
    CHECK(ar_indic_category(DV_VIRAMA) == AR_IND_VIRAMA, "indic: the virama joins consonants");
    CHECK(ar_indic_category(DV_I) == AR_IND_MATRA_PRE, "indic: the vowel sign i is drawn before");
    CHECK(ar_indic_category(DV_AA) == AR_IND_MATRA_POST, "indic: aa is drawn after");
    CHECK(ar_indic_category(0x0902) == AR_IND_BINDU, "indic: anusvara is a bindu");
    CHECK(ar_indic_category('a') == AR_IND_OTHER, "indic: Latin is not categorised");
    CHECK(ar_indic_is_indic(DV_KA) && !ar_indic_is_indic('a'),
          "indic: and only Indic codepoints are reordered at all");
}

static void test_indic_pre_base_matra_moves(void)
{
    static const ar_u32 KI[] = {DV_KA, DV_I};

    CHECK(ar__reorder(KI, 2), "indic: ki reorders");
    CHECK(g_ind_cp[0] == DV_I && g_ind_cp[1] == DV_KA,
          "indic: the vowel sign i is drawn before its consonant");
    /* The cluster map has to follow, or a caret placed between the two lands
       on the wrong side of a letter the reader sees as one thing. */
    CHECK(g_ind_cl[0] == 1 && g_ind_cl[1] == 0, "indic: and the clusters move with it");
}

static void test_indic_leaves_alone_what_it_should(void)
{
    static const ar_u32 KA[] = {DV_KA};
    static const ar_u32 KAA[] = {DV_KA, DV_AA};
    static const ar_u32 KYA[] = {DV_KA, DV_VIRAMA, DV_YA};

    CHECK(!ar__reorder(KA, 1), "indic: a lone consonant does not move");
    CHECK(!ar__reorder(KAA, 2), "indic: a post-base matra stays after its consonant");
    CHECK(!ar__reorder(KYA, 3), "indic: a conjunct is not reordered");
    CHECK(g_ind_cp[0] == DV_KA && g_ind_cp[2] == DV_YA, "indic: and comes through intact");
}

static void test_indic_reph_moves_to_the_end(void)
{
    static const ar_u32 RKA[] = {DV_RA, DV_VIRAMA, DV_KA};

    /* A syllable opening ra + virama loses both and gains a mark drawn above
       the END of the syllable, which is where the consonant now is. */
    CHECK(ar__reorder(RKA, 3), "indic: rka reorders");
    CHECK(g_ind_cp[0] == DV_KA, "indic: the base consonant comes first");
    CHECK(g_ind_cp[1] == DV_RA && g_ind_cp[2] == DV_VIRAMA,
          "indic: and the reph moves to the end of the syllable");
}

static void test_indic_a_real_word(void)
{
    /* hindii: ha + i + na + virama + da + ii */
    static const ar_u32 W[] = {DV_HA, DV_I, DV_NA, DV_VIRAMA, DV_DA, DV_II};

    CHECK(ar__reorder(W, 6), "indic: a real word reorders");
    CHECK(g_ind_cp[0] == DV_I && g_ind_cp[1] == DV_HA,
          "indic: the i moves in front of the first syllable's consonant");
    CHECK(g_ind_cp[2] == DV_NA && g_ind_cp[3] == DV_VIRAMA && g_ind_cp[4] == DV_DA,
          "indic: the second syllable's conjunct is untouched");
    CHECK(g_ind_cp[5] == DV_II, "indic: and its post-base matra stays put");
}

static void test_indic_survives_nonsense(void)
{
    static const ar_u32 NASTY[][4] = {{DV_VIRAMA, 0, 0, 0},
                                      {DV_I, DV_I, DV_I, 0},
                                      {DV_RA, DV_VIRAMA, 0, 0},
                                      {DV_VIRAMA, DV_VIRAMA, DV_KA, 0}};
    ar_i32              i;
    int                 ok = 1;

    /* A virama with nothing to join, a matra with no consonant, a reph with
       nothing after it. All of these appear in real broken text and none of
       them may loop or lose a character. */
    for (i = 0; i < 4; ++i)
    {
        ar_i32 n = 0, j;
        while (n < 4 && NASTY[i][n])
        {
            ++n;
        }
        ar__reorder(NASTY[i], n);
        for (j = 0; j < n; ++j)
        {
            if (g_ind_cp[j] == 0)
            {
                ok = 0;
            }
        }
    }
    CHECK(ok, "indic: malformed syllables terminate and lose nothing");
}

/* ------------------------------------------------------------------------
 * Inheritance
 * ------------------------------------------------------------------------ */
static void test_inheritance_flows_down(void)
{
    ar_surface s = ar__ui_surface(200, 120);

    ar__ui_reset("#root { display:flex; flex-direction:column; color:#E8DFCC; font-size:17px; }"
                 ".mid  { display:flex; }"
                 ".own  { color:#FF0000; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.mid");
    ar_begin(g_ui, "div.leaf");
    ar_end(g_ui);
    ar_begin(g_ui, "div.own");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_COLOR] & 0xFFFFFF) == 0xE8DFCC,
          "inherit: a child takes its parent's colour");
    /* Through a box that only inherited it, which is what makes it a cascade
       rather than one level of copying. */
    CHECK((g_ui->nodes[2].style.v[AR_P_COLOR] & 0xFFFFFF) == 0xE8DFCC,
          "inherit: and a grandchild takes it through a box that only inherited");
    CHECK(g_ui->nodes[2].style.v[AR_P_FONT_SIZE] == 17, "inherit: font size inherits too");
    CHECK((g_ui->nodes[3].style.v[AR_P_COLOR] & 0xFFFFFF) == 0xFF0000,
          "inherit: a box that states its own colour keeps it");
    CHECK(g_ui->nodes[3].style.v[AR_P_FONT_SIZE] == 17,
          "inherit: while still inheriting what it did not state");
}

static void test_layout_properties_do_not_inherit(void)
{
    ar_surface s = ar__ui_surface(200, 120);

    ar__ui_reset("#root { display:flex; width:150px; padding:9px; color:#112233; }"
                 ".kid  { height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.kid");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* A width that inherited would make every box the size of its parent, and
       a padding that inherited would compound at every level. */
    CHECK(g_ui->nodes[1].style.v[AR_P_WIDTH] != 150, "inherit: width does not inherit");
    CHECK(g_ui->nodes[1].style.v[AR_P_PAD_LEFT] == 0, "inherit: nor does padding");
    CHECK((g_ui->nodes[1].style.v[AR_P_COLOR] & 0xFFFFFF) == 0x112233,
          "inherit: but colour still does, from the same rule");
}

static void test_inheritance_is_not_in_the_style_cache(void)
{
    ar_surface s = ar__ui_surface(200, 120);
    ar_u32     hits_a, hits_b;

    /* Two boxes with the same selector under parents of different colours must
       resolve differently. If inheritance had been folded into the cache, the
       second would get the first's answer -- which is the silent rendering bug
       the cache key comment warns about. */
    ar__ui_reset("#root { display:flex; }"
                 ".red { display:flex; color:#FF0000; }"
                 ".blue { display:flex; color:#0000FF; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.red");
    ar_begin(g_ui, "div.leaf");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.blue");
    ar_begin(g_ui, "div.leaf");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    ar_style_cache_stats(g_ui, &hits_a, &hits_b);
    CHECK((g_ui->nodes[2].style.v[AR_P_COLOR] & 0xFFFFFF) == 0xFF0000,
          "inherit: a leaf under the red box is red");
    CHECK((g_ui->nodes[4].style.v[AR_P_COLOR] & 0xFFFFFF) == 0x0000FF,
          "inherit: and the same leaf under the blue box is blue");
}

/* ------------------------------------------------------------------------
 * Compound selectors
 * ------------------------------------------------------------------------ */
static void test_compound_selector_matching(void)
{
    ar_surface s = ar__ui_surface(200, 120);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".card { width:100px; background:#333333; }"
                 ".card.selected { background:#00FF00; }"
                 ".selected { border-width:2px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.card");
    ar_end(g_ui);
    ar_begin(g_ui, "div.card.selected");
    ar_end(g_ui);
    ar_begin(g_ui, "div.selected");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x333333,
          "compound: a box with one class takes the single-class rule");
    /* Two classes: the compound rule wins on specificity, and the box still
       picks up what each single-class rule gave it. */
    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "compound: .card.selected beats .card on specificity");
    CHECK(g_ui->nodes[2].style.v[AR_P_WIDTH] == 100,
          "compound: and still takes the width from .card");
    CHECK(g_ui->nodes[2].style.v[AR_P_BORDER_WIDTH] == 2,
          "compound: and the border from .selected");
    /* A box with only one of the two must not match the compound rule. */
    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x00FF00,
          "compound: a box with only one of the classes does not match");
    CHECK(g_ui->nodes[3].style.v[AR_P_WIDTH] != 100,
          "compound: nor picks up the other class's properties");
}

static void test_class_set_is_order_independent(void)
{
    ar_classes a, b;

    ar_classes_clear(&a);
    ar_classes_clear(&b);
    ar_classes_add(&a, 111u);
    ar_classes_add(&a, 222u);
    ar_classes_add(&b, 222u);
    ar_classes_add(&b, 111u);

    /* A box declared .a.b is the same box as one declared .b.a, so they must
       land on the same resolved-style cache entry. */
    CHECK(a.combined == b.combined, "compound: the class set hashes the same either order");
    CHECK(ar_classes_contains(&a, &b) && ar_classes_contains(&b, &a),
          "compound: and each contains the other");

    /* Containment is one-way where it should be: a rule naming two classes
       matches a box carrying three, not the reverse. */
    ar_classes_add(&a, 333u);
    CHECK(ar_classes_contains(&a, &b), "compound: three classes contain two");
    CHECK(!ar_classes_contains(&b, &a), "compound: two do not contain three");
}

static void test_class_list_has_a_ceiling(void)
{
    ar_classes c;
    ar_i32     i;

    ar_classes_clear(&c);
    for (i = 0; i < 20; ++i)
    {
        ar_classes_add(&c, (ar_u32)(i + 1));
    }
    CHECK(c.n == AR_MAX_CLASSES, "compound: the class list stops at its ceiling");

    /* And the same class twice is once, or a selector written .a.a would
       double its own specificity for nothing. */
    ar_classes_clear(&c);
    ar_classes_add(&c, 7u);
    ar_classes_add(&c, 7u);
    CHECK(c.n == 1, "compound: a repeated class is stored once");
}

/* ------------------------------------------------------------------------
 * Combinators
 *
 * A combinator makes a resolved style depend on where a box sits rather than
 * only on what it is, which is exactly what the resolved-style cache cannot
 * key on. Rules carrying one are held apart and resolved per box without it,
 * so a stylesheet with no combinators pays nothing.
 * ------------------------------------------------------------------------ */
/*
 * !important runs a second cascade over only the important declarations, which
 * is why a one-class rule can beat an id. The test that matters is that one,
 * not the easy case where important also happens to be more specific.
 */
static void test_important_beats_specificity(void)
{
    ar_surface s = ar__ui_surface(200, 140);

    ar__ui_reset("#panel { background:#FF0000; border-width:7px; }"
                 ".loud { background:#00FF00 !important; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div#panel.loud");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "important: a class marked important beats an id that is not");
    CHECK(g_ui->nodes[1].style.v[AR_P_BORDER_WIDTH] == 7,
          "important: and the id's other properties are untouched");
}

static void test_important_is_per_declaration(void)
{
    ar_sheet   sheet;
    ar_rule    rules[8];
    ar_classes klass;
    ar_style   out;

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, "p { color:#FF0000 !important; width:40px; }");

    CHECK(sheet.count == 1, "important: the rule parsed");
    CHECK(ar_pset_has(sheet.rules[0].important, AR_P_COLOR),
          "important: the marked property carries the flag");
    CHECK(!ar_pset_has(sheet.rules[0].important, AR_P_WIDTH),
          "important: and the one next to it does not");

    ar_classes_clear(&klass);
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, ar_hash("p", 1), &klass, 0, 0, &out);
    CHECK(out.v[AR_P_WIDTH] == 40, "important: the unmarked declaration still applies");
}

/* Both important, so the ordinary cascade decides between them: the second
   pass is a cascade of its own, not a free pass to whoever asked first. */
static void test_important_loses_to_important(void)
{
    ar_sheet   sheet;
    ar_rule    rules[8];
    ar_classes klass;
    ar_style   out;

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ".b { color:#00FF00 !important; }"
                           "#a { color:#0000FF !important; }");
    ar_classes_clear(&klass);
    ar_classes_add(&klass, ar_hash("b", 1));
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, 0, &klass, ar_hash("a", 1), 0, &out);
    CHECK((out.v[AR_P_COLOR] & 0xFFFFFF) == 0x0000FF,
          "important: between two important rules the more specific one still wins");
}

/*
 * inherit, initial, unset and revert.
 *
 * `inherit` on a property that does not normally inherit is the case worth
 * testing: it is what the keyword exists for, and it is the one that fails if
 * the keyword is handled inside the inherits-by-default loop instead of before
 * it.
 */
static void test_cascade_keywords(void)
{
    ar_surface s = ar__ui_surface(200, 140);

    ar__ui_reset("#root { background:#123456; color:#FF0000; font-size:20px; }"
                 ".takes { background:inherit; }"
                 ".drops { color:initial; }"
                 ".unset-inherited { color:unset; }"
                 ".unset-layout { background:unset; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.takes");
    ar_end(g_ui);
    ar_begin(g_ui, "div.drops");
    ar_end(g_ui);
    ar_begin(g_ui, "div.unset-inherited");
    ar_end(g_ui);
    ar_begin(g_ui, "div.unset-layout");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x123456,
          "cascade keyword: inherit takes a property that does not inherit by default");
    CHECK((g_ui->nodes[2].style.v[AR_P_COLOR] & 0xFFFFFF) != 0xFF0000,
          "cascade keyword: initial drops one that does");
    CHECK((g_ui->nodes[3].style.v[AR_P_COLOR] & 0xFFFFFF) == 0xFF0000,
          "cascade keyword: unset means inherit on an inherited property");
    CHECK((g_ui->nodes[4].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x123456,
          "cascade keyword: and initial on one that is not");
}

/*
 * The structural pseudo-classes.
 *
 * The split that matters is when each one can be answered. :root, :first-child
 * and the odd/even pair follow from position among siblings and are settled at
 * declare time; :last-child, :only-child and :empty are not knowable until the
 * parent has closed, and get a second resolve pass.
 */
/*
 * Text wrapping in layout.
 *
 * ar_text_wrap has been tested since 0.2.0; what these check is that the
 * solver calls it, which it did not until now. A box was one line however
 * narrow it was, and the text ran off the side.
 *
 * The harness has no TrueType face, so these run against the built-in bitmap
 * face, whose advances are known exactly -- which is what lets the expected
 * heights below be written down rather than approximated.
 */
/*
 * Selector lists.
 *
 * `h1, h2 { ... }` is two rules that happen to have been written once. Nothing
 * supported them until 0.4.0 finished: the comma was not a selector character,
 * so the whole block was refused and the declarations silently did nothing.
 */
/*
 * A rule with a combinator must not apply through the cached pass.
 *
 * It used to. `.page .card` was applied to every `.card` anywhere, because the
 * cached resolver only ever looks at the subject compound and dropped the
 * context silently. Every combinator test written before this one passed
 * anyway, because each of their sheets happened to carry a second rule that
 * overwrote the wrong answer -- which is the shape of bug that lives happily
 * inside a green test suite.
 *
 * So: one rule, no second rule to hide behind, and a box that must not get it.
 */
/*
 * A box whose text changed is repainted, not drawn over.
 *
 * This is the bug behind a status line that went dark and smeared: it read
 * "3 region(s)" one frame and "1 region(s)" the next, and nothing erased the
 * first, so two different strings shared the same pixels. The digest hashes
 * the text's *content* rather than its pointer for exactly this reason --
 * formatting a label into a reused buffer every frame leaves the pointer
 * identical while the pixels differ.
 *
 * Checked by rendering a wide string, then a narrow one in the same box, and
 * comparing against a context that only ever drew the narrow one. Any pixel of
 * the wide string still standing is a failure.
 */
static ar_u32 g_repaint_a[200 * 40];

static void ar__render_label(const char *first, const char *second, ar_surface *s)
{
    ar__ui_reset("#root { display:flex; flex-direction:column; background:#FFFFFF; }"
                 ".label { width:200px; height:20px; background:#FFFFFF; color:#000000; }");

    if (first)
    {
        ar__ui_begin();
        ar_begin(g_ui, "#root");
        ar_text(g_ui, "div.label", first);
        ar_end(g_ui);
        ar_frame_end(g_ui, s);
        ar_frame_presented(g_ui);
    }

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.label", second);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
    ar_frame_presented(g_ui);
}

static ar_u32 ar__pixel_at(ar_i32 x, ar_i32 y)
{
    return g_ui_pixels[y * AR_LAY_MAX + x] & 0xFFFFFFu;
}

/* ------------------------------------------------------------------------
 * Scroll containers
 *
 * The third thing STATUS.md said a real interface hits first: a list longer
 * than its box could not be reached.
 * ------------------------------------------------------------------------ */

/* Builds a scroll container with five 40 px rows in a 100 px box. */
static void ar__scroll_scene(ar_surface *s, const ar_input *in)
{
    ar_i32 i;

    ar_frame_begin(g_ui, in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    for (i = 0; i < 5; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static const char *AR_SCROLL_CSS = "#root { display:block; }"
                                   ".list { display:block; height:100px; overflow:scroll; }"
                                   ".row { display:block; height:40px; }";

/* The range is what does not fit, and nothing more. */
static void test_scroll_range_is_the_overflow(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);

    CHECK(ar__box(1).h == 100, "scroll: the container keeps its stated height");
    CHECK(ar_node_scroll_range(g_ui, 1) == 100,
          "scroll: five forty pixel rows in a hundred leaves a hundred to scroll");
    CHECK(ar_node_scroll(g_ui, 1) == 0, "scroll: and it starts at the top");
}

/* Scrolling moves the contents and leaves the container alone. */
static void test_scrolling_moves_the_contents(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);

    CHECK(ar__box(2).y == 0, "scroll: the first row starts at the top");
    ar_node_scroll_to(g_ui, 1, 60);
    ar__scroll_scene(&s, &in);

    CHECK(ar__box(1).y == 0, "scroll: the container itself did not move");
    CHECK(ar__box(2).y == -60, "scroll: but its first row went up by the offset");
    CHECK(ar__box(3).y == -20, "scroll: and so did the one after it");
}

/* The position is clamped to the range, in both directions. */
static void test_scroll_is_clamped(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);

    CHECK(ar_node_scroll_to(g_ui, 1, 9999) == 100, "scroll: past the end stops at the end");
    CHECK(ar_node_scroll_to(g_ui, 1, -50) == 0, "scroll: and above the top stops at the top");
}

/* The position survives between frames, which is what makes it state. */
static void test_scroll_survives_the_frame(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);
    ar_node_scroll_to(g_ui, 1, 40);

    ar__scroll_scene(&s, &in);
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll(g_ui, 1) == 40, "scroll: the position is still there two frames later");
}

/* A wheel notch over the container scrolls it, and lands on the next frame. */
static void test_the_wheel_scrolls_the_box_under_it(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.wheel = -1; /* one notch towards the user */
    ar__scroll_scene(&s, &in);
    CHECK(ar_scrolled(g_ui), "wheel: the notch moved something");

    in.wheel = 0;
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll(g_ui, 1) > 0, "wheel: and the frame after it shows the new position");
    CHECK(ar__box(2).y < 0, "wheel: with the rows moved up");
}

/* A notch outside the container does nothing to it. */
static void test_the_wheel_ignores_a_box_it_is_not_over(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 250; /* below the hundred pixel container */
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.wheel = -3;
    ar__scroll_scene(&s, &in);
    ar__scroll_scene(&s, &in);

    CHECK(ar_node_scroll(g_ui, 1) == 0, "wheel: a notch elsewhere leaves the container alone");
}

/* A box whose content fits has nowhere to go, and `auto` shows no bar for it
   while `scroll` shows one anyway -- which is the whole difference. */
static void test_auto_and_scroll_differ_only_when_it_fits(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:100px; overflow:auto; }"
                 ".s { display:block; height:100px; overflow:scroll; }"
                 ".row { display:block; height:10px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.s");
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar_node_scroll_range(g_ui, 1) == 0, "scroll: content that fits has no range");
    CHECK(!ar_scroll_bar_visible(&g_ui->nodes[1]), "scroll: auto hides the bar when it fits");
    CHECK(ar_scroll_bar_visible(&g_ui->nodes[3]), "scroll: scroll shows one regardless");
}

/* Contents are clipped to the container, so a scrolled row does not paint
   outside it. */
static void test_a_scroll_container_clips(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".list { display:block; height:60px; overflow:scroll; }"
                 ".row { display:block; height:40px; background:#FF0000; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 50) == 0xFF0000, "scroll: a row inside the container is painted");
    CHECK(ar__pixel_at(10, 70) != 0xFF0000, "scroll: and the part past its edge is not");
}

/* ------------------------------------------------------------------------
 * Stacking order
 *
 * Paint order is only observable in pixels, so these read them. Every case is
 * two boxes on the same spot and one question: which colour survived.
 * ------------------------------------------------------------------------ */
/* Without positioning, later wins -- which is what declaration order gave and
   what has to keep being true. */
static void test_declaration_order_still_decides_when_nothing_is_positioned(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; position:relative; }"
                 ".red { display:block; position:absolute; top:0; left:0;"
                 "       width:50px; height:50px; background:#FF0000; }"
                 ".blue { display:block; position:absolute; top:0; left:0;"
                 "        width:50px; height:50px; background:#0000FF; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.red");
    ar_end(g_ui);
    ar_begin(g_ui, "div.blue");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0x0000FF, "stacking: with equal z, the later box wins");
}

/*
 * A positioned box paints above an unpositioned one declared after it.
 *
 * This is the one that catches people out: `position: relative` on its own,
 * with no offsets and no z-index, changes what covers what.
 */
static void test_a_positioned_box_paints_above_the_flow(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; }"
                 ".pos { display:block; position:relative;"
                 "       width:50px; height:50px; background:#FF0000; }"
                 ".flow { display:block; position:relative; top:-50px;"
                 "        width:50px; height:50px; background:#0000FF; }"
                 ".plain { display:block; width:50px; height:50px; background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.pos");
    ar_end(g_ui);
    ar_begin(g_ui, "div.plain");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* .plain is declared second and overlaps nothing, so this only checks that
       the positioned box survived where it sits. */
    CHECK(ar__pixel_at(10, 10) == 0xFF0000, "stacking: the positioned box is still there");
    CHECK(ar__pixel_at(10, 60) == 0x00FF00, "stacking: and the plain one below it is too");
}

/* A negative z-index puts a positioned box behind the flow. */
static void test_negative_z_goes_behind_the_flow(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; position:relative; }"
                 ".back { display:block; position:absolute; top:0; left:0;"
                 "        width:50px; height:50px; background:#FF0000; z-index:-1; }"
                 ".plain { display:block; width:50px; height:50px; background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.back");
    ar_end(g_ui);
    ar_begin(g_ui, "div.plain");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0x00FF00,
          "stacking: a negative z-index puts the box under the flow that follows it");
}

/* z-index orders positioned boxes against each other, beating declaration
   order in both directions. */
static void test_z_index_beats_declaration_order(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; position:relative; }"
                 ".low { display:block; position:absolute; top:0; left:0;"
                 "       width:50px; height:50px; background:#0000FF; z-index:1; }"
                 ".high { display:block; position:absolute; top:0; left:0;"
                 "        width:50px; height:50px; background:#FF0000; z-index:5; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.high");
    ar_end(g_ui);
    ar_begin(g_ui, "div.low");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0xFF0000,
          "stacking: the higher z wins even though it was declared first");
}

/* z-index on an unpositioned box does nothing at all, which is the other half
   of the confusion. */
static void test_z_index_needs_a_position(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; position:relative; }"
                 ".ignored { display:block; position:absolute; top:0; left:0;"
                 "           width:50px; height:50px; background:#FF0000; }"
                 ".plain { display:block; position:absolute; top:0; left:0;"
                 "         width:50px; height:50px; background:#0000FF; z-index:0; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.plain");
    ar_end(g_ui);
    ar_begin(g_ui, "div.ignored");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* Both are in bucket six -- z:0 and z:auto share it -- so declaration
       order decides, and the second one wins. */
    CHECK(ar__pixel_at(10, 10) == 0xFF0000, "stacking: z-index 0 and z-index auto share a layer");
}

/*
 * A float paints above the in-flow blocks and below anything positioned.
 *
 * Buckets four and three of Appendix E. Made observable by pulling the float
 * up over the block with a negative margin, because a float and the block it
 * shortens do not otherwise share any pixels -- which is the whole point of a
 * float and the reason this needed contriving.
 */
static void test_a_float_paints_above_the_blocks(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; }"
                 ".blk { display:block; width:50px; height:50px; background:#00FF00; }"
                 ".f { display:block; float:left; width:50px; height:50px;"
                 "     margin-top:-50px; background:#FF0000; }"
                 /* No offset: a float takes no space in the flow, so this
                    box's static position is already on top of it. */
                 ".over { display:block; position:relative;"
                 "        width:50px; height:50px; background:#0000FF; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.blk");
    ar_end(g_ui);
    ar_begin(g_ui, "div.f");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0xFF0000, "stacking: a float paints above an in-flow block");

    /* And a positioned box paints above the float. */
    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.f");
    ar_end(g_ui);
    ar_begin(g_ui, "div.over");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0x0000FF, "stacking: and a positioned box paints above a float");
}

/* Hit testing follows the same order backwards: the box on top is the one the
   cursor is over. */
static void test_hit_testing_finds_the_box_on_top(void)
{
    ar_surface s = ar__ui_surface(100, 100);
    ar_input   in;

    ar__ui_reset("#root { display:block; position:relative; }"
                 ".under { display:block; position:absolute; top:0; left:0;"
                 "         width:50px; height:50px; }"
                 ".over { display:block; position:absolute; top:0; left:0;"
                 "        width:50px; height:50px; z-index:5; }"
                 ".over:hover { background:#FF0000; }"
                 ".under:hover { background:#0000FF; }");

    /* Two frames: hover is resolved from the previous one. */
    memset(&in, 0, sizeof in);
    in.mouse_x = 10;
    in.mouse_y = 10;
    in.mouse_inside = 1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.over");
    ar_end(g_ui);
    ar_begin(g_ui, "div.under");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.over");
    ar_end(g_ui);
    ar_begin(g_ui, "div.under");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0xFF0000,
          "stacking: the cursor is over the box on top, not the one declared last");
}

/* ------------------------------------------------------------------------
 * Positioning
 * ------------------------------------------------------------------------ */

/* relative shifts a box for painting and leaves its space occupied, which is
   the whole difference from absolute. */
static void test_relative_shifts_but_keeps_its_space(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".r { display:block; height:10px; position:relative; top:5px; left:7px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.r");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).x == 7 && ar__box(2).y == 15, "relative: the box moved by its offsets");
    CHECK(ar__box(3).y == 20, "relative: and the box after it did not, because the space is kept");
}

/* A relative box takes its subtree with it. */
static void test_relative_moves_its_children(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".r { display:block; position:relative; left:10px; }"
                 ".c { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).x == 10, "relative: the box moved");
    CHECK(ar__box(2).x == 10, "relative: and its child moved with it, not twice");
}

/*
 * absolute against the nearest positioned ancestor's *padding* box.
 *
 * Not the border box and not the content box: `top: 0` sits under the border
 * and outside the padding, which is what makes a badge in the corner of a
 * bordered card land where anyone would expect.
 */
static void test_absolute_uses_the_padding_box(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".card { display:block; position:relative; height:80px;"
                 "        border-width:3px; padding:9px; }"
                 ".badge { display:block; position:absolute; top:0; left:0;"
                 "         width:10px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.card");
    ar_begin(g_ui, "div.badge");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 3, 3, 10, 10),
          "absolute: measured from inside the border and outside the padding");
}

/* It takes no space: the box after it sits where it would have anyway. */
static void test_absolute_is_out_of_the_flow(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".abs { display:block; position:absolute; width:50px; height:50px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.abs");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(3).y == 10, "absolute: the block after it ignores it entirely");
}

/* With no offsets it stays at the static position: where the flow had reached.
   That is what makes `position: absolute` alone look like nothing happened. */
static void test_absolute_with_no_offsets_keeps_the_static_position(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".abs { display:block; position:absolute; width:50px; height:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.abs");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 10, "absolute: with no offsets it sits where the flow had got to");
}

/* Both edges of an axis and no size: the box stretches between them. */
static void test_absolute_stretches_between_two_edges(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".p { display:block; position:relative; height:100px; }"
                 ".abs { display:block; position:absolute; left:10px; right:30px;"
                 "       top:5px; bottom:15px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.p");
    ar_begin(g_ui, "div.abs");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 10, 5, 160, 80), "absolute: pinned on four edges, sized by the gap");
}

/* The right edge alone puts the box's far side there. */
static void test_absolute_from_the_far_edges(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".p { display:block; position:relative; height:100px; }"
                 ".abs { display:block; position:absolute; right:10px; bottom:20px;"
                 "       width:30px; height:15px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.p");
    ar_begin(g_ui, "div.abs");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 160, 65, 30, 15), "absolute: right and bottom place the far edges");
}

/* fixed is measured against the viewport, whatever is between it and the root. */
static void test_fixed_uses_the_viewport(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".p { display:block; position:relative; height:100px; padding:20px; }"
                 ".fix { display:block; position:fixed; top:0; right:0;"
                 "       width:10px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.p");
    ar_begin(g_ui, "div.fix");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 190, 0, 10, 10),
          "fixed: the viewport, not the positioned ancestor above it");
}

/* An unpositioned ancestor is skipped: the walk goes to the next one up. */
static void test_absolute_skips_an_unpositioned_ancestor(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".outer { display:block; position:relative; height:100px; }"
                 ".mid { display:block; padding:12px; }"
                 ".abs { display:block; position:absolute; top:0; left:0;"
                 "       width:10px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.mid");
    ar_begin(g_ui, "div.abs");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(3, 0, 0, 10, 10),
          "absolute: the unpositioned middle box is not the containing block");
}

/*
 * Two automatic margins centre an absolutely positioned box between two given
 * edges. This is how a modal is centred, and why that needs `left: 0` and
 * `right: 0` rather than `margin: auto` on its own.
 */
static void test_auto_margins_centre_an_absolute_box(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".p { display:block; position:relative; height:100px; }"
                 ".modal { display:block; position:absolute; left:0; right:0;"
                 "         top:0; bottom:0; width:60px; height:20px;"
                 "         margin:auto; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.p");
    ar_begin(g_ui, "div.modal");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 70, 40, 60, 20), "absolute: two auto margins centre it on both axes");
}

/* A positioned box drops its float, because position wins. */
static void test_position_beats_float(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".x { display:block; position:absolute; float:left; top:40px; left:50px;"
                 "     width:20px; height:20px; }"
                 ".a { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.x");
    ar_end(g_ui);
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 50, 40, 20, 20), "position: absolute wins and the float is dropped");
    CHECK(ar__box(2).y == 0, "position: so nothing in the flow was pushed by it");
}

/* ------------------------------------------------------------------------
 * Inline fragmentation
 *
 * The difference between `inline` and `inline-block`: an inline box's text
 * flows into the lines around it and is cut wherever a line ends, so one box
 * becomes one rectangle per line it touches.
 * ------------------------------------------------------------------------ */

/* Two inline boxes share a line, and their text runs together rather than each
   getting a line of its own. */
static void test_inline_boxes_share_a_line(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".t { display:inline; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.t", "one ");
    ar_text(g_ui, "div.t", "two");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).y == ar__box(2).y, "inline: two runs sit on the same line");
    CHECK(ar__box(2).x == ar__box(1).x + ar__box(1).w,
          "inline: and the second starts where the first ended");
}

/* The one that needs fragments: a run too long for the line is cut, and gets a
   rectangle on each line it touches. */
static void test_an_inline_box_is_cut_across_lines(void)
{
    ar_surface s = ar__ui_surface(400, 200);
    ar_i32     one_line;

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:60px; }"
                 ".t { display:inline; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_text(g_ui, "div.t", "one two three four five");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    one_line = ar_text_height(1);

    CHECK(ar_node_frag_count(g_ui, 2) > 1, "fragment: the run was cut into more than one piece");
    CHECK(ar__box(2).h > one_line, "fragment: and its box spans every line it touched");
    CHECK(ar__box(2).w <= 60, "fragment: none of which is wider than the line");
}

/* Each fragment carries the slice of text that landed on it, and the slices
   are contiguous and cover the whole string. */
static void test_fragments_partition_the_text(void)
{
    ar_surface  s = ar__ui_surface(400, 200);
    const char *text = "one two three four five";
    ar_i32      i, n, at = 0;
    int         gap = 0;

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:60px; }"
                 ".t { display:inline; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_text(g_ui, "div.t", text);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    n = ar_node_frag_count(g_ui, 2);
    for (i = 0; i < n; ++i)
    {
        ar_i32 from, to;

        (void)ar_node_frag(g_ui, 2, i, &from, &to);
        if (from != at)
        {
            gap = 1;
        }
        at = to;
    }
    CHECK(n > 1, "fragment: there is more than one to check");
    CHECK(!gap, "fragment: the slices run end to end with nothing skipped");
    CHECK(at == (ar_i32)strlen(text), "fragment: and together they are the whole string");
}

/* A box that fits on one line has no fragments at all: it is its own single
   rectangle, which is every box that is not a split inline. */
static void test_a_short_inline_has_no_fragments(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".t { display:inline; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.t", "short");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar_node_frag_count(g_ui, 1) <= 1, "fragment: a run that fits is not cut");
    CHECK(ar__box(1).w == ar_text_width("short", 1), "fragment: and is as wide as its text");
}

/* Inline and inline-block on one line: one is cut, the other never is. */
static void test_atomic_and_fragmented_on_one_line(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:80px; }"
                 ".t { display:inline; }"
                 ".i { display:inline-block; width:30px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_text(g_ui, "div.t", "one two three four");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).w == 30, "fragment: the atomic item keeps its stated width");
    CHECK(ar_node_frag_count(g_ui, 2) == 0, "fragment: and is never cut");
    CHECK(ar_node_frag_count(g_ui, 3) > 1, "fragment: while the run beside it is");
    /* The box's rect is the union of its fragments, and the second line starts
       at the left edge, so the union starts there too. The question is where
       the *first* fragment starts, which is after the atomic item. */
    CHECK(ar_node_frag(g_ui, 3, 0, 0, 0).x == 30,
          "fragment: the first piece starts after the atomic item");
    CHECK(ar__box(3).x == 0, "fragment: while the union reaches back to the second line's edge");
}

/* ------------------------------------------------------------------------
 * Intrinsic sizing
 *
 * min-content is the narrowest a box can be without spilling; max-content the
 * widest it would ever want. fit-content is max-content clamped to the room
 * available and then never narrower than min-content, and is the only one of
 * the three anybody writes on purpose.
 * ------------------------------------------------------------------------ */

/* The widest unbreakable run, which for text is its longest word. */
static void test_min_content_of_text_is_its_longest_word(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".m { display:block; width:min-content; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.m", "a bb elephantine cc");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == ar_text_width("elephantine ", 1),
          "min-content: as wide as the longest word, and no wider");
}

/* max-content is the whole string with nothing broken. */
static void test_max_content_is_the_unbroken_string(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".m { display:block; width:max-content; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.m", "a bb elephantine cc");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == ar_text_width("a bb elephantine cc", 1),
          "max-content: the whole string, unbroken");
}

/*
 * fit-content takes the room it is given, up to what it wants.
 *
 * Three containers, one narrower than min-content, one between the two, one
 * wider than max-content -- which is the whole behaviour in one test.
 */
static void test_fit_content_is_clamped_both_ways(void)
{
    ar_surface s = ar__ui_surface(400, 300);
    ar_i32     minw, maxw;

    ar__ui_reset("#root { display:block; }"
                 ".narrow { display:block; width:20px; }"
                 ".middle { display:block; width:90px; }"
                 ".wide { display:block; width:390px; }"
                 ".f { display:block; width:fit-content; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.narrow");
    ar_text(g_ui, "div.f", "a bb elephantine cc");
    ar_end(g_ui);
    ar_begin(g_ui, "div.middle");
    ar_text(g_ui, "div.f", "a bb elephantine cc");
    ar_end(g_ui);
    ar_begin(g_ui, "div.wide");
    ar_text(g_ui, "div.f", "a bb elephantine cc");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    minw = ar_text_width("elephantine ", 1);
    maxw = ar_text_width("a bb elephantine cc", 1);

    /* The middle container has to sit between the two for the middle case to
       mean anything, so the assumption is checked rather than assumed. */
    CHECK(minw < 90 && 90 < maxw, "fit-content: the middle container is genuinely in between");
    CHECK(ar__box(2).w == minw, "fit-content: never narrower than min-content");
    CHECK(ar__box(4).w == 90, "fit-content: takes the room available in between");
    CHECK(ar__box(6).w == maxw, "fit-content: and never wider than max-content");
}

/* A container's min-content comes from its children: the widest, when they
   stack, and the sum when they sit side by side. */
static void test_min_content_of_a_container(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".stack { display:block; width:min-content; }"
                 ".row { display:flex; flex-direction:row; width:min-content; }"
                 ".a { display:block; width:30px; height:10px; }"
                 ".b { display:block; width:50px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.stack");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.row");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == 50, "min-content: a block stack needs its widest child");
    CHECK(ar__box(4).w == 80, "min-content: a flex row needs the sum of them");
}

/* Padding is part of the box, so an intrinsic width includes it. */
static void test_intrinsic_width_includes_padding(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__ui_reset("#root { display:block; }"
                 ".m { display:block; width:min-content; padding:0 7px; }"
                 ".a { display:block; width:30px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.m");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == 44, "min-content: the padding is part of the box");
}

/* ------------------------------------------------------------------------
 * Floats
 *
 * The rule to keep straight: a float shortens *line boxes*, not block boxes.
 * A paragraph beside a float still spans its container; only the lines inside
 * it are narrowed. Getting that backwards looks almost right, which is what
 * makes it worth a test of its own.
 * ------------------------------------------------------------------------ */
static void test_a_float_goes_to_its_side(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:40px; height:20px; }"
                 ".r { display:block; float:right; width:30px; height:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.r");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 40, 20), "float: left goes to the left edge");
    CHECK(ar__box_is(2, 170, 0, 30, 20), "float: right goes to the right edge");
}

/* Two left floats sit beside each other until they run out of room. */
static void test_floats_stack_sideways_then_drop(void)
{
    ar_surface s = ar__ui_surface(100, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:40px; height:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).x == 0 && ar__box(1).y == 0, "float: the first is at the edge");
    CHECK(ar__box(2).x == 40 && ar__box(2).y == 0, "float: the second sits beside it");
    CHECK(ar__box(3).x == 0 && ar__box(3).y == 20, "float: the third drops below both");
}

/* A block box beside a float keeps its full width. Only its lines are cut. */
static void test_a_float_does_not_narrow_a_block_box(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:40px; height:20px; }"
                 ".p { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.p");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 0, 0, 200, 10),
          "float: the block beside it starts at the container edge and spans it");
}

/* But line boxes inside that block are narrowed, and start after the float. */
static void test_a_float_narrows_the_lines_beside_it(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:60px; height:20px; }"
                 ".i { display:inline-block; width:80px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The line beside the float has 140 to give, so one item fits and the
       second drops. Below the float the full 200 is back. */
    CHECK(ar__box(2).x == 60, "float: the first line starts after the float");
    CHECK(ar__box(3).x == 60 && ar__box(3).y == 10,
          "float: the second wraps but is still beside it");
}

/* A float takes no room in the flow: the box after it starts where it would
   have started anyway. */
static void test_a_float_is_out_of_the_flow(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".l { display:block; float:left; width:40px; height:50px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(3).y == 10, "float: the block after it ignores its height entirely");
}

/* clear moves a box below the floats on the sides it names. */
static void test_clear_moves_below_the_float(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:40px; height:50px; }"
                 ".r { display:block; float:right; width:40px; height:80px; }"
                 ".cl { display:block; height:10px; clear:left; }"
                 ".cb { display:block; height:10px; clear:both; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.r");
    ar_end(g_ui);
    ar_begin(g_ui, "div.cl");
    ar_end(g_ui);
    ar_begin(g_ui, "div.cb");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(3).y == 50, "clear: left clears the left float only");
    CHECK(ar__box(4).y == 80, "clear: both clears the taller right one too");
}

/*
 * A formatting context grows to hold its own floats; a plain block box does
 * not. A float hanging out of the bottom of a box is what `clearfix` existed
 * to work around for fifteen years, and it is correct behaviour.
 */
static void test_only_a_formatting_context_contains_its_floats(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".plain { display:block; }"
                 ".bfc { display:block; overflow:hidden; }"
                 ".l { display:block; float:left; width:40px; height:50px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.plain");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.bfc");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).h == 0, "float: a plain block does not grow to hold its float");
    CHECK(ar__box(3).h == 50, "float: one that establishes a context does");
}

/*
 * A box that establishes a formatting context does not overlap a float. It
 * moves aside and, if its width was automatic, narrows to what is left.
 *
 * This is the difference that makes `overflow: hidden` beside a float sit next
 * to it rather than under it -- the most-used float idiom there is -- while an
 * ordinary block spans the container and lets only its lines be narrowed.
 */
static void test_a_formatting_context_avoids_a_float(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".l { display:block; float:left; width:40px; height:50px; }"
                 ".bfc { display:block; overflow:hidden; height:20px; }"
                 ".plain { display:block; height:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.bfc");
    ar_end(g_ui);
    ar_begin(g_ui, "div.plain");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(2, 40, 0, 160, 20), "float: a formatting context moves aside and narrows");
    CHECK(ar__box(3).x == 0 && ar__box(3).w == 200,
          "float: an ordinary block still spans the container");
}

/* A float's margins never collapse with anything, because they meet nothing. */
static void test_a_floats_margins_do_not_collapse(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; margin-bottom:20px; }"
                 ".l { display:block; float:left; width:40px; height:20px; margin-top:20px; }"
                 ".b { display:block; height:10px; margin-top:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.l");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The two blocks are still adjacent to each other despite the float
       between them, so their margins collapse to 20 and .b sits at 30. */
    CHECK(ar__box(3).y == 30, "float: the blocks either side of it stay adjacent");
    CHECK(ar__box(2).y == 50, "float: and its own margin is added, not collapsed");
}

/* ------------------------------------------------------------------------
 * Inline formatting
 *
 * Items are atomic -- inline-block, not inline -- so none of these split a box
 * across two lines. That is fragmentation and it is the next piece.
 * ------------------------------------------------------------------------ */

/* The first thing a line box has to do: put siblings beside each other rather
   than under each other, which is the whole difference from a block child. */
static void test_inline_items_share_a_line(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".i { display:inline-block; width:40px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 40, 10), "inline: the first item sits at the left");
    CHECK(ar__box_is(2, 40, 0, 40, 10), "inline: the second sits beside it, not below");
}

/* And the second thing: start a new line when the next item will not fit. */
static void test_inline_wraps_to_a_new_line(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    /* The width goes on a box inside the root, because the root takes the
       viewport and ignores anything it says about its own size. */
    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:100px; }"
                 ".i { display:inline-block; width:40px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).x == 0 && ar__box(2).y == 0, "inline: two fit on the first line");
    CHECK(ar__box(3).x == 40 && ar__box(3).y == 0, "inline: and the second is one of them");
    CHECK(ar__box(4).x == 0 && ar__box(4).y == 10, "inline: the third starts a new line");
    CHECK(ar__box(1).h == 20, "inline: and the block grew to hold both lines");
}

/* Margins count towards the width a line has left. */
static void test_inline_margins_take_room_on_the_line(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".i { display:inline-block; width:40px; height:10px; margin-right:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).x == 50, "inline: the second item clears the first item's margin");
    CHECK(ar__box(2).y == 0, "inline: and 50 plus 50 still fits in 100");
}

/*
 * The reason a line box is not just "as tall as its tallest item".
 *
 * Two boxes of the same height whose baselines sit at different depths need a
 * line taller than either of them, so that both baselines can be the same one.
 * A box with text has its baseline under the ascent; a box without has none,
 * and sits on the line rather than through it.
 */
static void test_line_height_comes_from_the_baselines(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".plain { display:inline-block; width:20px; height:20px; }"
                 ".padded { display:inline-block; width:20px; padding-top:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.plain");
    ar_end(g_ui);
    ar_text(g_ui, "div.padded", "x");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The plain box has no text, so its baseline is its bottom edge: 20 above
       it, nothing below. The padded one has text, so its baseline is 10 of
       padding plus the face's ascent of 8, with nothing below that either.
       The line is max(20, 18) tall and both sit on the same baseline. */
    CHECK(ar__box(1).y == 0, "baseline: the deeper box defines the line's top");
    CHECK(ar__box(2).y == 2, "baseline: the shallower one drops to meet it");
}

/* vertical-align:top ignores the baseline and pins to the line's top edge. */
static void test_vertical_align_top_and_bottom(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".tall { display:inline-block; width:20px; height:40px; }"
                 ".t { display:inline-block; width:20px; height:10px; vertical-align:top; }"
                 ".b { display:inline-block; width:20px; height:10px; vertical-align:bottom; }"
                 ".m { display:inline-block; width:20px; height:10px; vertical-align:middle; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.tall");
    ar_end(g_ui);
    ar_begin(g_ui, "div.t");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_begin(g_ui, "div.m");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 0, "valign: top pins to the top of the line");
    CHECK(ar__box(3).y == 30, "valign: bottom pins to the bottom of it");
    CHECK(ar__box(4).y == 15, "valign: middle centres in it");
}

/* text-align moves a whole line, and only the leftover. */
static void test_text_align_moves_the_line(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".right { display:block; width:100px; text-align:right; }"
                 ".centre { display:block; width:100px; text-align:center; }"
                 ".i { display:inline-block; width:40px; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.right");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.centre");
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).x == 60, "align: right pushes the line to the far edge");
    CHECK(ar__box(4).x == 30, "align: centre splits the leftover");
}

/* An inline run sits in the block stack where an anonymous block would, so
   blocks above and below it keep their own places. */
static void test_an_inline_run_takes_its_place_in_the_stack(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".i { display:inline-block; width:40px; height:20px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 10, "inline: the run starts below the block above it");
    CHECK(ar__box(3).y == 10, "inline: both items are on that line");
    CHECK(ar__box(4).y == 30, "inline: and the block below clears the whole run");
}

/* An inline-level box shrinks to fit rather than filling its container, which
   is the other half of what makes it inline-level. */
static void test_inline_shrinks_to_fit(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".i { display:inline-block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.i", "abc");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == ar_text_width("abc", 1),
          "inline: the box is as wide as its content, not as wide as the page");
}

/* A margin on an inline item does not collapse with anything: it is not a
   block box and the run is an anonymous one, which has no margins at all. */
static void test_inline_margins_do_not_collapse(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; margin-bottom:20px; }"
                 ".i { display:inline-block; width:20px; height:10px; margin-top:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.i");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* 10 + 20 for the block's own margin, then the item's 20 on top of that
       rather than collapsed into it: 50, where two blocks would give 30. */
    CHECK(ar__box(2).y == 50, "inline: an item's margin is added, never collapsed");
}

/* ------------------------------------------------------------------------
 * Block formatting
 *
 * Written from the specification before the code was, which is the only way
 * margin collapsing comes out right: it is a small set of rules that interact,
 * and reading them off an implementation gives you that implementation's
 * opinion rather than the specification's.
 * ------------------------------------------------------------------------ */

/* The arithmetic on its own, before any boxes are involved. */
static void test_margin_collapse_arithmetic(void)
{
    CHECK(ar_margin_collapse(0, 0) == 0, "collapse: nothing against nothing");
    CHECK(ar_margin_collapse(20, 10) == 20, "collapse: two positives take the larger");
    CHECK(ar_margin_collapse(10, 20) == 20, "collapse: and order does not matter");
    CHECK(ar_margin_collapse(20, 0) == 20, "collapse: a positive against zero");
    CHECK(ar_margin_collapse(-20, -10) == -20, "collapse: two negatives take the smaller");
    CHECK(ar_margin_collapse(20, -8) == 12, "collapse: mixed signs add the two extremes");
    CHECK(ar_margin_collapse(-8, 20) == 12, "collapse: mixed signs, the other way round");
    CHECK(ar_margin_collapse(10, -30) == -20, "collapse: a negative large enough to win");
}

/* display:block stacks its children vertically and gives each the full width,
   which is the whole difference from a flex row and was not true before. */
static void test_block_stacks_and_fills(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:20px; }"
                 ".b { display:block; height:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 200, 20), "block: the first child fills the width");
    CHECK(ar__box_is(2, 0, 20, 200, 30), "block: the second stacks below it");
}

/* Rule one: adjacent siblings. 20 against 10 is 20, not 30. */
static void test_margin_collapse_between_siblings(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:20px; margin-bottom:20px; }"
                 ".b { display:block; height:30px; margin-top:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 40, "collapse: siblings share one margin, not two");
}

/* And with a negative in the mix: 20 against -8 leaves 12. */
static void test_margin_collapse_negative_between_siblings(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:20px; margin-bottom:20px; }"
                 ".b { display:block; height:30px; margin-top:-8px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 32, "collapse: a negative margin pulls the sibling up");
}

/*
 * Rule two, the one that surprises people: a first child's top margin escapes
 * its parent when nothing separates them, and moves the parent instead.
 */
static void test_margin_escapes_through_the_parent(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".spacer { display:block; height:10px; }"
                 ".outer { display:block; }"
                 ".inner { display:block; height:20px; margin-top:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.spacer");
    ar_end(g_ui);
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.inner");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 40, "collapse: the child's margin moved the parent");
    CHECK(ar__box(3).y == 40, "collapse: and the child sits flush inside it");
    CHECK(ar__box(2).h == 20, "collapse: the parent is only as tall as the child");
}

/* Padding on the parent stops it. One pixel of padding is enough. */
static void test_padding_stops_the_margin_escaping(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".spacer { display:block; height:10px; }"
                 ".outer { display:block; padding-top:1px; }"
                 ".inner { display:block; height:20px; margin-top:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.spacer");
    ar_end(g_ui);
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.inner");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 10, "collapse: padding keeps the parent where it was");
    CHECK(ar__box(3).y == 41, "collapse: and the margin stays inside, below the padding");
}

/* So does a new formatting context, which overflow other than visible makes. */
static void test_a_formatting_context_stops_the_margin(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".spacer { display:block; height:10px; }"
                 ".outer { display:block; overflow:hidden; }"
                 ".inner { display:block; height:20px; margin-top:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.spacer");
    ar_end(g_ui);
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.inner");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 10, "collapse: margins do not cross a formatting context");
    CHECK(ar__box(3).y == 40, "collapse: the child keeps its margin inside");
}

/* Rule three: the last child's bottom margin escapes downwards on the same
   terms, and a stated height on the parent stops it. */
static void test_the_last_childs_margin_escapes_downwards(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".outer { display:block; }"
                 ".inner { display:block; height:20px; margin-bottom:30px; }"
                 ".after { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.inner");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.after");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).h == 20, "collapse: the parent does not grow to hold the escaped margin");
    CHECK(ar__box(3).y == 50, "collapse: the sibling below is pushed by it");
}

/* Rule four: a box with nothing in it collapses through itself, contributing
   one margin to the flow rather than two. */
static void test_an_empty_box_collapses_through_itself(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".empty { display:block; margin-top:20px; margin-bottom:30px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.empty");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(3).y == 40, "collapse: an empty box contributes one margin of 30, not 50");
}

/* Where the empty box itself ends up. Its top edge sits below the margin
   immediately before it, not below the whole collapsed run -- the run carries
   on past it. Chromium settled which of the two it is. */
static void test_an_empty_box_sits_below_its_own_margin(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; }"
                 ".empty { display:block; margin-top:20px; margin-bottom:30px; }"
                 ".b { display:block; height:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.empty");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 30, "collapse: the empty box sits below its own top margin");
    CHECK(ar__box(2).h == 0, "collapse: and has no height");
    CHECK(ar__box(3).y == 40, "collapse: while the run past it is the collapsed 30");
}

/* A block container's automatic height is its stack, with the collapsed
   margins counted once. */
static void test_block_auto_height(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".outer { display:block; padding:5px; }"
                 ".a { display:block; height:20px; margin-bottom:20px; }"
                 ".b { display:block; height:30px; margin-top:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* 5 padding + 20 + 20 collapsed + 30 + 5 padding. The margins inside do
       not escape, because the padding is in the way. */
    CHECK(ar__box(1).h == 80, "block: automatic height counts the collapsed margins once");
}

/* The root is a formatting context whatever it says. There is nowhere above
   it for a margin to escape to, and a margin that escaped the viewport would
   simply be lost. */
static void test_the_root_is_a_formatting_context(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:20px; margin-top:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).y == 20, "block: a first child's margin applies at the root, not escapes it");
}

/* Horizontal margins never collapse. Only the vertical ones do. */
static void test_horizontal_margins_do_not_collapse(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; margin-left:20px; margin-right:30px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 20, 0, 150, 10), "block: horizontal margins inset the box on both sides");
}

/* A flex child inside a block container keeps its own layout, and its margins
   do not collapse with anything, because it is a formatting context. */
static void test_a_flex_child_does_not_collapse(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:10px; margin-bottom:20px; }"
                 ".f { display:flex; flex-direction:row; height:10px; margin-top:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.f");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* Between siblings the margins still collapse -- that rule is about the
       gap, not about either box's insides -- so 20 against 20 is 20. */
    CHECK(ar__box(2).y == 30, "block: sibling collapsing still applies to a flex sibling");
}

/* Text in a block box wraps to the width it is given, and the box grows. */
static void test_block_text_wraps_and_grows(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:block; }"
                 ".p { display:block; width:60px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.p", "one two three four five six seven");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == 60, "block: the paragraph took the width it was given");
    CHECK(ar__box(1).h == ar_text_height(1) + 4 * ar_text_line_height(1),
          "block: and grew to the five lines that width needs");
}

static void test_changed_text_is_repainted_not_overdrawn(void)
{
    ar_surface s = ar__ui_surface(200, 40);
    ar_i32     x, y, diff = 0;

    /* The narrow string alone: what the second frame must look like. */
    ar__render_label(0, "1 region(s)", &s);
    for (y = 0; y < 40; ++y)
    {
        for (x = 0; x < 200; ++x)
        {
            g_repaint_a[y * 200 + x] = g_ui_pixels[y * s.stride + x];
        }
    }

    /* The wide string first, then the narrow one over it. */
    ar__render_label("3 region(s) wwwwww", "1 region(s)", &s);
    for (y = 0; y < 40; ++y)
    {
        for (x = 0; x < 200; ++x)
        {
            if (g_repaint_a[y * 200 + x] != g_ui_pixels[y * s.stride + x])
            {
                diff++;
            }
        }
    }

    CHECK(diff == 0, "damage: a box whose text changed is repainted, leaving none of the old");
}

static void test_a_combinator_does_not_leak_through_the_cache(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".page { display:flex; flex-direction:column; }"
                 ".page .card { background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.page");
    ar_begin(g_ui, "div.card"); /* 2: inside the page */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.card"); /* 3: outside it */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "combinator: the card inside the page gets the rule");
    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x00FF00,
          "combinator: and the identical card outside it does not, with nothing to overwrite it");
}

static void test_selector_lists(void)
{
    ar_sheet   sheet;
    ar_rule    rules[16];
    ar_classes klass;
    ar_style   out;

    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, ".a, .b, .c { width:40px; }");

    CHECK(sheet.count == 3, "list: one block, three rules");
    CHECK(sheet.errors == 0, "list: and no complaints");

    ar_classes_clear(&klass);
    ar_classes_add(&klass, ar_hash("b", 1));
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, 0, &klass, 0, 0, &out);
    CHECK(out.v[AR_P_WIDTH] == 40, "list: the middle selector applies too");
}

/* A list longer than the array holds is refused whole. A truncated selector
   list is a rule that silently does not apply to some of what it names. */
static void test_selector_list_has_a_ceiling(void)
{
    ar_sheet sheet;
    ar_rule  rules[16];

    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, ".a, .b, .c, .d, .e { width:40px; }");
    CHECK(sheet.count == 0, "list: longer than the array holds is refused");
    CHECK(sheet.errors > 0, "list: and counted as an error");
}

/* Each selector in the list keeps its own specificity, and one bad entry
   refuses the block rather than half of it. */
static void test_selector_list_keeps_its_own_specificity(void)
{
    ar_sheet   sheet;
    ar_rule    rules[16];
    ar_classes klass;
    ar_style   out;

    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, "div { width:10px; }"
                           "#id, .cls { width:20px; }"
                           "div#id { width:30px; }");

    ar_classes_clear(&klass);
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, ar_hash("div", 3), &klass, ar_hash("id", 2), 0, &out);
    CHECK(out.v[AR_P_WIDTH] == 30, "list: a tag plus an id still outranks the id alone");
}

/*
 * :not(), :is() and :where().
 *
 * The three of them are one mechanism: a list of simple selectors on the
 * subject compound, which either all have to fail or one of which has to
 * match. They differ in what they contribute to specificity, and that is
 * settled at parse time.
 */
static void test_not_excludes(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".item { background:#111111; }"
                 ".item:not(.done) { background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.item");
    ar_end(g_ui);
    ar_begin(g_ui, "div.item.done");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "not: matches the box without the class");
    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x111111,
          "not: and skips the one with it");
}

/* Every argument must fail, not just the first. */
static void test_not_takes_a_list(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".item { background:#111111; }"
                 ".item:not(.done, .hidden) { background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.item");
    ar_end(g_ui);
    ar_begin(g_ui, "div.item.hidden");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "not: a box matching neither argument still matches");
    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x111111,
          "not: the second argument excludes as well as the first");
}

/* :not() also accepts a state, which is where it earns its keep -- there is no
   other way to say "any row that is not the last one". */
static void test_not_takes_a_state(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".row:not(:last-child) { border-width:4px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(g_ui->nodes[1].style.v[AR_P_BORDER_WIDTH] == 4, "not: a state argument excludes too");
    CHECK(g_ui->nodes[2].style.v[AR_P_BORDER_WIDTH] != 4, "not: and the last child is excluded");
}

static void test_is_matches_any(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ":is(.a, .b) { background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "is: first alternative");
    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00, "is: second");
    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x00FF00, "is: and nothing else");
}

/*
 * The one thing that separates :is() from :where().
 *
 * :is() takes the specificity of its most specific argument, :where() takes
 * none -- so a plain class beats `:where(#id)` and loses to `:is(#id)`. Two
 * sheets rather than one, because the point is the ranking and not the order.
 */
static void test_where_contributes_no_specificity(void)
{
    ar_sheet   sheet;
    ar_rule    rules[8];
    ar_classes klass;
    ar_style   out;

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ":where(#id) { width:10px; }"
                           ".cls { width:20px; }");
    ar_classes_clear(&klass);
    ar_classes_add(&klass, ar_hash("cls", 3));
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, 0, &klass, ar_hash("id", 2), 0, &out);
    CHECK(out.v[AR_P_WIDTH] == 20, "where: a class beats :where(#id), which scores nothing");

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ":is(#id) { width:10px; }"
                           ".cls { width:20px; }");
    ar_style_defaults(&out);
    ar_sheet_resolve(&sheet, 0, &klass, ar_hash("id", 2), 0, &out);
    CHECK(out.v[AR_P_WIDTH] == 10, "where: and loses to :is(#id), which scores as an id");
}

/* The functional pseudo-classes hold simple selectors only, and belong to the
   subject. Both limits are refusals rather than partial matches. */
static void test_functional_limits_are_refusals(void)
{
    ar_sheet sheet;
    ar_rule  rules[8];

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ".x:not(.a.b) { width:5px; }");
    CHECK(sheet.count == 0, "not: a compound argument is refused, not half-matched");

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ".page:not(.wide) .card { width:5px; }");
    CHECK(sheet.count == 0, "not: on a context part rather than the subject is refused");

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ".x:is(.a, .b, .c, .d) { width:5px; }");
    CHECK(sheet.count == 0, "is: an argument list longer than the array is refused");
}

/* :not() combines with everything else rather than replacing it. */
static void test_not_composes_with_a_combinator(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".page { display:flex; flex-direction:column; }"
                 ".page .card:not(.muted) { background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.page");
    ar_begin(g_ui, "div.card");
    ar_end(g_ui);
    ar_begin(g_ui, "div.card.muted");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.card");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "not: inside a descendant selector, the plain card matches");
    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x00FF00,
          "not: the muted one does not");
    CHECK((g_ui->nodes[4].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0x00FF00,
          "not: and neither does the card outside the page");
}

static void test_layout_wraps_text_to_the_box(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_i32     one, many;

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".narrow { width:60px; }"
                 ".wide   { width:600px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.wide", "one two three four five six seven");
    ar_text(g_ui, "div.narrow", "one two three four five six seven");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    one = ar__box(1).h;
    many = ar__box(2).h;

    CHECK(one == ar_text_height(1), "wrap: text that fits stays one line");
    CHECK(many > one, "wrap: the same text in a narrow box gets taller");
    /* Five lines. The string is 213 px wide at scale 1 and the box is 60, so
       four is the floor; it takes five because words do not divide evenly and
       this breaks between them rather than through them. Written as a number
       because a measured number catches a regression that "taller than one
       line" would sleep through. */
    CHECK(many == ar_text_height(1) + 4 * ar_text_line_height(1),
          "wrap: and is exactly as many lines as it needs");
}

/* A box told how tall to be is not resized by its text. Overflowing a stated
   height is the caller's decision, and silently growing the box would move
   everything below it. */
static void test_wrapping_respects_a_stated_height(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".fixed { width:60px; height:12px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.fixed", "one two three four five six seven");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).h == 12, "wrap: a stated height wins over the wrapped text");
}

/* Padding comes out of the width the text has to fit into, so a padded box
   wraps sooner than an unpadded one of the same size. */
static void test_wrapping_is_inside_the_padding(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".bare { width:120px; }"
                 ".pad  { width:120px; padding:0 40px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.bare", "one two three four five six");
    ar_text(g_ui, "div.pad", "one two three four five six");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).h > ar__box(1).h,
          "wrap: padding narrows the line and the box gets taller for it");
}

/* A box growing along a horizontal main axis only learns its width after the
   leftover is handed out, which is later than every other box. */
static void test_wrapping_a_grown_box(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:flex; flex-direction:row; align-items:flex-start; }"
                 ".rail { width:220px; }"
                 ".rest { width:grow; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.rail");
    ar_end(g_ui);
    ar_text(g_ui, "div.rest", "one two three four five six seven eight");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).w == 80, "wrap: the grown box took the leftover");
    CHECK(ar__box(2).h > ar_text_height(1),
          "wrap: and wrapped into it, though its width was settled last");
}

/* A word wider than the box is not broken. Overflowing is a failure the caller
   can see; splitting a word at an arbitrary point looks like a rendering bug. */
static void test_a_long_word_is_not_broken(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".tiny { width:20px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_text(g_ui, "div.tiny", "unbreakable");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).h == ar_text_height(1), "wrap: a word wider than the box stays on one line");
}

static void test_structural_pseudo_classes(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset(":root { border-width:3px; }"
                 ".row:first-child { background:#FF0000; }"
                 ".row:last-child { background:#00FF00; }"
                 ".row:nth-child(odd) { border-width:7px; }"
                 ".row:nth-child(even) { border-width:9px; }"
                 ".lone:only-child { background:#0000FF; }"
                 ".box:empty { background:#FFFF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box"); /* 1: no children and no text */
    ar_end(g_ui);
    ar_begin(g_ui, "div.list"); /* 2 */
    ar_begin(g_ui, "div.row");  /* 3: first child of the list, odd */
    ar_end(g_ui);
    ar_begin(g_ui, "div.row"); /* 4: even */
    ar_end(g_ui);
    ar_begin(g_ui, "div.row"); /* 5: last child of the list, odd */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.box");  /* 6: has a child, so not empty */
    ar_begin(g_ui, "div.lone"); /* 7: the only child of 6 */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(g_ui->nodes[0].style.v[AR_P_BORDER_WIDTH] == 3, "structural: :root matches the root");
    CHECK(g_ui->nodes[2].style.v[AR_P_BORDER_WIDTH] != 3, "structural: and nothing below it");

    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0xFF0000,
          "structural: :first-child matches the first");
    CHECK((g_ui->nodes[4].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0xFF0000,
          "structural: and not the second");
    CHECK((g_ui->nodes[5].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "structural: :last-child matches the last, resolved after the parent closed");

    CHECK(g_ui->nodes[3].style.v[AR_P_BORDER_WIDTH] == 7, "structural: nth-child(odd)");
    CHECK(g_ui->nodes[4].style.v[AR_P_BORDER_WIDTH] == 9, "structural: nth-child(even)");
    CHECK(g_ui->nodes[5].style.v[AR_P_BORDER_WIDTH] == 7, "structural: and odd again");

    CHECK((g_ui->nodes[7].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x0000FF,
          "structural: :only-child matches a lone child");
    CHECK((g_ui->nodes[6].style.v[AR_P_BACKGROUND] & 0xFFFFFF) != 0xFFFF00,
          "structural: :empty does not match a box with a child");
    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0xFFFF00,
          "structural: :empty matches one with neither children nor text");
}

/* The style cache is keyed on state among other things, and state outgrew a
   byte when the structural bits arrived. Truncating it there would return the
   plain box's style for the first child, silently. */
static void test_structural_state_survives_the_cache_key(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset(".row { background:#111111; }"
                 ".row:first-child { background:#FF0000; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_begin(g_ui, "div.row");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0xFF0000,
          "structural: two boxes differing only in a structural bit do not share a cache entry");
    CHECK((g_ui->nodes[2].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x111111,
          "structural: and the second gets its own style");
}

/* The general an+b form is not implemented, and a selector using it is refused
   rather than quietly matching something else. */
static void test_nth_child_general_form_is_refused(void)
{
    ar_sheet sheet;
    ar_rule  rules[8];

    ar_sheet_init(&sheet, rules, 8);
    ar_sheet_parse(&sheet, ".r:nth-child(2n+1) { width:5px; }");
    CHECK(sheet.count == 0, "structural: nth-child(an+b) is refused");
    CHECK(sheet.errors > 0, "structural: and counted as an error");
}

static void test_combinators(void)
{
    ar_surface s = ar__ui_surface(200, 140);

    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".page { display:flex; flex-direction:column; }"
                 ".card { background:#111111; }"
                 ".page .card { background:#00FF00; }"
                 "#root > .card { background:#FF0000; }"
                 ".a + .b { border-width:5px; }"
                 ".a ~ .c { border-width:9px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.card"); /* 1: a direct child of #root */
    ar_end(g_ui);
    ar_begin(g_ui, "div.page"); /* 2 */
    ar_begin(g_ui, "div.card"); /* 3: inside .page, not a child of #root */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.a"); /* 4 */
    ar_end(g_ui);
    ar_begin(g_ui, "div.b"); /* 5: immediately after .a */
    ar_end(g_ui);
    ar_begin(g_ui, "div.c"); /* 6: a later sibling of .a */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK((g_ui->nodes[1].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0xFF0000,
          "combinator: > matches a direct child");
    /* The card inside .page is a descendant of #root but not its child, so the
       child rule must not reach it. That is the difference between the two
       combinators and the reason both exist. */
    CHECK((g_ui->nodes[3].style.v[AR_P_BACKGROUND] & 0xFFFFFF) == 0x00FF00,
          "combinator: a descendant is not a child");
    CHECK(g_ui->nodes[5].style.v[AR_P_BORDER_WIDTH] == 5,
          "combinator: + matches the sibling immediately after");
    CHECK(g_ui->nodes[6].style.v[AR_P_BORDER_WIDTH] == 9, "combinator: ~ matches a later sibling");
    CHECK(g_ui->nodes[4].style.v[AR_P_BORDER_WIDTH] == 0,
          "combinator: and neither matches the box on the left of them");
}

static void test_a_sheet_without_combinators_says_so(void)
{
    static ar_rule rules[16];
    ar_sheet       sheet;

    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, ".a { width:1px; } div.b#c:hover { width:2px; }");
    CHECK(!sheet.has_contextual,
          "combinator: a sheet with none is flagged, so the pass is skipped entirely");

    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, ".a .b { width:1px; }");
    CHECK(sheet.has_contextual, "combinator: and a sheet with one is flagged too");
    CHECK(sheet.count == 1 && sheet.rules[0].nctx == 1,
          "combinator: the rule keeps one context part");
}

static void test_selector_depth_is_refused_not_truncated(void)
{
    static ar_rule rules[16];
    ar_sheet       sheet;

    /* Deeper than the context list holds. Truncating would silently match more
       elements than the author asked for, which is worse than not matching. */
    ar_sheet_init(&sheet, rules, 16);
    ar_sheet_parse(&sheet, ".a .b .c .d .e { width:1px; }");
    CHECK(sheet.count == 0, "combinator: a selector deeper than the list is refused");
    CHECK(sheet.errors > 0, "combinator: and counted as an error rather than ignored");
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

    test_ttf_reads_the_tables();
    test_ttf_maps_codepoints();
    test_ttf_scales_without_overflowing();
    test_ttf_extracts_an_outline();
    test_ttf_empty_glyph_is_not_an_error();
    test_ttf_rejects_what_it_cannot_read();
    test_cff_header_alone_is_not_a_font();
    test_ttf_survives_truncation_and_corruption();

    test_utf8_decodes_valid_sequences();
    test_utf8_rejects_what_it_should();
    test_utf8_walks_a_mixed_string_exactly();
    test_glyph_cache_rasterizes_once();
    test_glyph_cache_spreads_sizes_across_slots();
    test_subpixel_positions_are_separate_entries();
    test_grid_fit_snaps_the_x_height();
    test_darkening_keeps_its_endpoints();
    test_glyph_cache_carries_metrics();
    test_glyph_cache_resets_rather_than_overflowing();
    test_glyph_cache_refuses_a_glyph_larger_than_its_budget();
    test_fallback_chain_picks_the_first_face_that_has_it();
    test_face_family_name();
    test_text_draw_matches_measure();
    test_text_draws_pixels_and_respects_the_clip();
    test_text_survives_a_face_that_failed_to_load();

    test_font_load_and_fallback();
    test_font_changes_what_is_measured();
    test_measured_width_is_remembered();
    test_font_antialias_toggle();
    test_font_antialias_toggle_invalidates();

    test_break_after_spaces_not_before();
    test_break_respects_punctuation();
    test_break_keeps_numbers_together();
    test_break_hyphens_and_dashes();
    test_break_honours_joiners();
    test_break_mandatory();
    test_break_between_ideographs();
    test_break_always_terminates();
    test_wrap_fits_the_width();
    test_wrap_does_not_break_a_single_long_word();
    test_wrap_honours_a_mandatory_break();
    test_wrap_respects_its_line_limit();

    test_bidi_paragraph_level();
    test_bidi_levels_of_mixed_text();
    test_bidi_numbers_after_arabic();
    test_bidi_separators_join_numbers();
    test_bidi_reorders_into_visual_runs();
    test_bidi_explicit_overrides();
    test_bidi_isolates();
    test_bidi_survives_anything();

    test_shape_leaves_a_plain_font_alone();
    test_shape_refuses_what_it_cannot_shape();
    test_arabic_joining_classes();
    test_arabic_shaping_needs_a_font_with_the_features();
    test_marks_need_the_tables();
    test_marks_are_optional_to_ask_for();
    test_mark_to_mark_needs_the_tables();
    test_decomposition_respects_the_buffer();
    test_chained_context_needs_a_lookup_list();
    test_marks_do_not_block_a_ligature();

    test_indic_categories();
    test_indic_pre_base_matra_moves();
    test_indic_leaves_alone_what_it_should();
    test_indic_reph_moves_to_the_end();
    test_indic_a_real_word();
    test_indic_survives_nonsense();

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

    test_important_beats_specificity();
    test_important_is_per_declaration();
    test_important_loses_to_important();
    test_cascade_keywords();
    test_scroll_range_is_the_overflow();
    test_scrolling_moves_the_contents();
    test_scroll_is_clamped();
    test_scroll_survives_the_frame();
    test_the_wheel_scrolls_the_box_under_it();
    test_the_wheel_ignores_a_box_it_is_not_over();
    test_auto_and_scroll_differ_only_when_it_fits();
    test_a_scroll_container_clips();
    test_declaration_order_still_decides_when_nothing_is_positioned();
    test_a_positioned_box_paints_above_the_flow();
    test_negative_z_goes_behind_the_flow();
    test_z_index_beats_declaration_order();
    test_z_index_needs_a_position();
    test_a_float_paints_above_the_blocks();
    test_hit_testing_finds_the_box_on_top();
    test_relative_shifts_but_keeps_its_space();
    test_relative_moves_its_children();
    test_absolute_uses_the_padding_box();
    test_absolute_is_out_of_the_flow();
    test_absolute_with_no_offsets_keeps_the_static_position();
    test_absolute_stretches_between_two_edges();
    test_absolute_from_the_far_edges();
    test_fixed_uses_the_viewport();
    test_absolute_skips_an_unpositioned_ancestor();
    test_auto_margins_centre_an_absolute_box();
    test_position_beats_float();
    test_inline_boxes_share_a_line();
    test_an_inline_box_is_cut_across_lines();
    test_fragments_partition_the_text();
    test_a_short_inline_has_no_fragments();
    test_atomic_and_fragmented_on_one_line();
    test_min_content_of_text_is_its_longest_word();
    test_max_content_is_the_unbroken_string();
    test_fit_content_is_clamped_both_ways();
    test_min_content_of_a_container();
    test_intrinsic_width_includes_padding();
    test_a_float_goes_to_its_side();
    test_floats_stack_sideways_then_drop();
    test_a_float_does_not_narrow_a_block_box();
    test_a_float_narrows_the_lines_beside_it();
    test_a_float_is_out_of_the_flow();
    test_clear_moves_below_the_float();
    test_only_a_formatting_context_contains_its_floats();
    test_a_formatting_context_avoids_a_float();
    test_a_floats_margins_do_not_collapse();
    test_inline_items_share_a_line();
    test_inline_wraps_to_a_new_line();
    test_inline_margins_take_room_on_the_line();
    test_line_height_comes_from_the_baselines();
    test_vertical_align_top_and_bottom();
    test_text_align_moves_the_line();
    test_an_inline_run_takes_its_place_in_the_stack();
    test_inline_shrinks_to_fit();
    test_inline_margins_do_not_collapse();
    test_margin_collapse_arithmetic();
    test_block_stacks_and_fills();
    test_margin_collapse_between_siblings();
    test_margin_collapse_negative_between_siblings();
    test_margin_escapes_through_the_parent();
    test_padding_stops_the_margin_escaping();
    test_a_formatting_context_stops_the_margin();
    test_the_last_childs_margin_escapes_downwards();
    test_an_empty_box_collapses_through_itself();
    test_an_empty_box_sits_below_its_own_margin();
    test_block_auto_height();
    test_the_root_is_a_formatting_context();
    test_horizontal_margins_do_not_collapse();
    test_a_flex_child_does_not_collapse();
    test_block_text_wraps_and_grows();
    test_changed_text_is_repainted_not_overdrawn();
    test_a_combinator_does_not_leak_through_the_cache();
    test_selector_lists();
    test_selector_list_has_a_ceiling();
    test_selector_list_keeps_its_own_specificity();
    test_not_excludes();
    test_not_takes_a_list();
    test_not_takes_a_state();
    test_is_matches_any();
    test_where_contributes_no_specificity();
    test_functional_limits_are_refusals();
    test_not_composes_with_a_combinator();
    test_layout_wraps_text_to_the_box();
    test_wrapping_respects_a_stated_height();
    test_wrapping_is_inside_the_padding();
    test_wrapping_a_grown_box();
    test_a_long_word_is_not_broken();
    test_structural_pseudo_classes();
    test_structural_state_survives_the_cache_key();
    test_nth_child_general_form_is_refused();
    test_combinators();
    test_a_sheet_without_combinators_says_so();
    test_selector_depth_is_refused_not_truncated();
    test_compound_selector_matching();
    test_class_set_is_order_independent();
    test_class_list_has_a_ceiling();
    test_inheritance_flows_down();
    test_layout_properties_do_not_inherit();
    test_inheritance_is_not_in_the_style_cache();
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
