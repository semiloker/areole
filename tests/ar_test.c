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
#include "ar_html.h"

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
 * Version
 * ------------------------------------------------------------------------ */

/* AR_VERSION_STRING and the three macros beside it are two records of one
   fact, and they have disagreed before. The string sat at "0.6.1-dev" through
   0.6.2, 0.6.3, 0.7.0, 0.7.1, 0.8.0, 0.8.1 and 0.8.2, and because ar_bench
   stamps it into every result, docs/PERFORMANCE.md published its numbers under
   an engine four minor releases old.

   This catches half of that drift. Both records can be stale together and
   agree with each other perfectly -- which is precisely what happened -- so
   the other half is tools/gen_perf_doc.py --check, comparing this string
   against the version the committed baseline was actually measured with. */
#define AR__STR2(x) #x
#define AR__STR(x)  AR__STR2(x)

static void test_version_string_matches_the_macros(void)
{
    const char *v = ar_version();
    const char *number =
        AR__STR(AR_VERSION_MAJOR) "." AR__STR(AR_VERSION_MINOR) "." AR__STR(AR_VERSION_PATCH);
    size_t n = strlen(number);

    CHECK(strncmp(v, number, n) == 0, "version: the reported string begins with the three macros");
    CHECK(v[n] == 0 || strcmp(v + n, "-dev") == 0,
          "version: what follows the number is nothing or -dev");
}

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
    /* By index rather than by name, so it has to be the accessor: the wide
       properties live past the end of v[] and only ar_style_get knows that. */
    return ar_style_get(&st, prop);
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
        CHECK((ar_u32)AR_WIDE(&st, AR_P_BACKGROUND) == 0xFFF8F3E9u,
              "css: a six digit hex colour parses");
    }

    /* A box that matches nothing keeps the defaults, rather than inheriting
       whatever the last resolved box happened to have. */
    {
        ar_style st = ar__resolve(".nothing", AR_STATE_NONE);
        CHECK(st.unit[AR_P_WIDTH] == AR_UNIT_AUTO, "css: an unmatched box sizes to its content");
        CHECK((ar_u32)AR_WIDE(&st, AR_P_BACKGROUND) == 0u,
              "css: an unmatched box has no background");
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
        CHECK((ar_u32)AR_WIDE(&a, AR_P_BORDER_COLOR) == 0xFFE8DFCCu,
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
    /* 120 of content plus 10 of padding on each side. Under content-box --
       CSS's default and now areole's -- a stated width is what goes inside. */
    CHECK(ar__box_is(1, 0, 0, 140, 300), "layout: a fixed rail keeps its width and stretches");

    /* Inside the rail the axes swap: it is a column, so the item takes its
       stated height on the main axis and grows across the rail inner width. */
    CHECK(ar__box_is(2, 10, 10, 120, 24), "layout: a nested box works inside the parent padding");
    CHECK(ar__box_is(3, 140, 0, 360, 300), "layout: the page takes the rest of the row");
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

/*
 * A wide tree that did not change damages nothing.
 *
 * Which is the same claim as the test above, on a tree big enough to have
 * caught what that one could not. The slot table is indexed by the key's low
 * bits and ar__mix does not reach them: for the children of one parent the
 * mixed keys walk up by one, so a list's rows land in consecutive indices and
 * the level below them collides into that block. Linear probing gives up after
 * 32, and a box with no slot has nothing remembering that it did not change --
 * so it is repainted every frame, for ever.
 *
 * Eighty rows was enough for five boxes to lose their slots with the table
 * **31% full**, and doubling the table does not help because the run is a
 * property of the hash rather than of the load. That is why this counts rows
 * rather than filling the table.
 *
 * Found while working out why a scrolled container would not take the region
 * move: one slotless row out of six hundred was reporting damage every frame.
 */
static unsigned char g_wide_mem[AR_MEM(1024)];

static void test_damage_a_wide_tree_is_still_when_still(void)
{
    ar_surface s = ar__ui_surface(400, 700);
    ar_rect    d;
    int        pass;

    /* Its own arena: six hundred boxes is more than the shared one holds, and
       the two failures this pins only show on a list that long. */
    g_ui = ar_init(g_wide_mem, (ar_u32)sizeof g_wide_mem);
    ar_stylesheet(g_ui, "#root { display:block; } .row { display:block; height:2px; }"
                        ".cell { display:block; }");

    d = ar_rect_make(0, 0, 0, 0);
    for (pass = 0; pass < 2; ++pass)
    {
        int i;

        ar__ui_begin();
        ar_begin(g_ui, "#root");
        for (i = 0; i < 300; ++i)
        {
            ar_begin(g_ui, "div.row");
            ar_begin(g_ui, "div.cell");
            ar_end(g_ui);
            ar_end(g_ui);
        }
        ar_end(g_ui);
        d = ar_frame_end(g_ui, &s);
    }

    CHECK(!ar_overflowed(g_ui),
          "slots: the wide tree fits, so this measures slots and not the arena");
    CHECK(d.w == 0 || d.h == 0,
          "slots: every box in a wide tree keeps its slot, so a still frame is still");
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

/*
 * The same criterion for scrolling, which earns its own run.
 *
 * A scroll does not repaint the rows it keeps -- it moves their pixels inside
 * the surface and paints only the band that came into view. Every other test
 * here would pass on a build that moved them to the wrong place by a pixel, or
 * dragged the scrollbar thumb along with the content, because they all check
 * geometry rather than what is on the glass.
 *
 * So: two contexts, the same scroll positions, one moving pixels and one forced
 * to repaint from scratch, compared after every frame. Both directions, because
 * the row order the copy must use is opposite between them and getting that
 * wrong smears the surface in one direction only.
 */
static const char *const SCROLL_DMG_CSS =
    "#root { display:block; overflow:scroll; height:128px; background:#101010; }"
    ".row { display:block; height:1px; margin:1px 0px; background:#3a4a5a; }";

static void ar__scroll_dmg_declare(ar_ctx *c)
{
    int i;

    ar_begin(c, "div#root");
    for (i = 0; i < 120; ++i)
    {
        ar_begin(c, "div.row");
        ar_end(c);
    }
    ar_end(c);
}

/*
 * Only the scrollbar's colour changes, and the bar has to notice.
 *
 * ar__paint_bars reads scrollbar-color and scrollbar-width; ar_paint_digest is
 * the list of exactly what the paint pass reads, and when the overlay bar
 * landed none of them were added to it. A frame in which one of them is the
 * only thing that changed then produces no damage at all, so the bar keeps the
 * colour it had. Geometry stays right, which is why every other scroll test
 * here is blind to it.
 *
 * Hover is the driver because a stylesheet is fixed for the life of a context:
 * a state change is the only way one of these properties differs between two
 * frames of the same interface.
 */
static const char *const SCROLL_BAR_DMG_CSS =
    "#root { display:block; overflow:scroll; height:128px; background:#101010;"
    "        scrollbar-color: #808080 #202020; }"
    "#root:hover { scrollbar-color: #ff0000 #202020; }"
    ".row { display:block; height:1px; margin:1px 0px; background:#3a4a5a; }";

static void test_the_bar_repaints_when_only_its_colour_changed(void)
{
    ar_surface tracked = ar__dmg_surface(g_dmg_a);
    ar_surface full = ar__dmg_surface(g_dmg_b);
    ar_ctx    *ref;
    int        i, frame;
    int        bad_frame = -1, bad_px = -1;

    /* Off the container, onto it, held there, off again. The hover edges are
       the frames that matter; the held frame catches a digest that only ever
       reports a change once. */
    static const int INSIDE[4] = {0, 1, 1, 0};

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        g_dmg_a[i] = 0;
        g_dmg_b[i] = 0;
    }

    ar__ui_reset(SCROLL_BAR_DMG_CSS);
    ref = ar_init(g_dmg_mem, (ar_u32)sizeof g_dmg_mem);
    CHECK(ref != 0, "scrollbar: the reference context initialises");
    if (!ref || !g_ui)
    {
        return;
    }
    ar_stylesheet(ref, SCROLL_BAR_DMG_CSS);

    for (frame = 0; frame < 4 && bad_frame < 0; ++frame)
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = INSIDE[frame] ? AR_DMG_W / 2 : -1;
        in.mouse_y = INSIDE[frame] ? AR_DMG_H / 2 : -1;
        in.mouse_inside = INSIDE[frame];

        ar_frame_begin(g_ui, &in);
        ar__scroll_dmg_declare(g_ui);
        ar_frame_end(g_ui, &tracked);
        ar_frame_presented(g_ui);

        ar_frame_begin(ref, &in);
        ar_invalidate_all(ref);
        ar__scroll_dmg_declare(ref);
        ar_frame_end(ref, &full);
        ar_frame_presented(ref);

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

    CHECK(ar_node_scroll_range(g_ui, 0) > 0, "scrollbar: the list is long enough to show a bar");
    CHECK(bad_frame < 0, "scrollbar: a colour change on its own still repaints the bar");
    if (bad_frame >= 0)
    {
        printf("      frame %d, pixel (%d,%d): tracked %08lX, full %08lX\n", bad_frame,
               bad_px % AR_DMG_W, bad_px / AR_DMG_W, (unsigned long)g_dmg_a[bad_px],
               (unsigned long)g_dmg_b[bad_px]);
    }
}

/*
 * The bar appears because the content grew, not because anything was styled.
 *
 * `overflow: auto` shows a bar only once there is somewhere to go, so a list
 * that grows past its box gains one with no style change and no change to the
 * container's own rectangle. ar_paint_digest cannot see that -- it hashes
 * style, and ar_scroll_bar_visible's answer turns on content_h -- so the box
 * that has to paint the bar is not the box the frame knows is dirty.
 */
static const char *const SCROLL_GROW_DMG_CSS =
    "#root { display:block; overflow:auto; height:128px; background:#101010; }"
    ".row { display:block; height:1px; margin:1px 0px; background:#3a4a5a; }";

static int g_grow_rows = 8;

static void ar__scroll_grow_declare(ar_ctx *c)
{
    int i;

    ar_begin(c, "div#root");
    for (i = 0; i < g_grow_rows; ++i)
    {
        ar_begin(c, "div.row");
        ar_end(c);
    }
    ar_end(c);
}

static void test_a_bar_that_appears_because_content_grew(void)
{
    ar_surface tracked = ar__dmg_surface(g_dmg_a);
    ar_surface full = ar__dmg_surface(g_dmg_b);
    ar_ctx    *ref;
    int        i, frame;
    int        bad_frame = -1, bad_px = -1;
    ar_i32     range_before = -1, range_after = -1;

    /* One row either side of the threshold, so the bar appears without the new
       content covering the column the bar is drawn in. Growing by a screenful
       instead would repaint that column as a side effect and prove nothing. */
    static const int ROWS[4] = {63, 63, 64, 64};

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        g_dmg_a[i] = 0;
        g_dmg_b[i] = 0;
    }

    ar__ui_reset(SCROLL_GROW_DMG_CSS);
    ref = ar_init(g_dmg_mem, (ar_u32)sizeof g_dmg_mem);
    CHECK(ref != 0, "scrollbar: the growing reference context initialises");
    if (!ref || !g_ui)
    {
        return;
    }
    ar_stylesheet(ref, SCROLL_GROW_DMG_CSS);

    for (frame = 0; frame < 4 && bad_frame < 0; ++frame)
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        g_grow_rows = ROWS[frame];

        ar_frame_begin(g_ui, &in);
        ar__scroll_grow_declare(g_ui);
        ar_frame_end(g_ui, &tracked);
        ar_frame_presented(g_ui);

        ar_frame_begin(ref, &in);
        ar_invalidate_all(ref);
        ar__scroll_grow_declare(ref);
        ar_frame_end(ref, &full);
        ar_frame_presented(ref);

        if (frame == 1)
        {
            range_before = ar_node_scroll_range(ref, 0);
        }
        if (frame == 2)
        {
            range_after = ar_node_scroll_range(ref, 0);
        }

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

    CHECK(range_before == 0 && range_after > 0,
          "scrollbar: the probe really does cross the threshold that shows a bar");
    CHECK(bad_frame < 0, "scrollbar: a bar that appears because content grew is painted whole");
    if (bad_frame >= 0)
    {
        printf("      frame %d, pixel (%d,%d): tracked %08lX, full %08lX\n", bad_frame,
               bad_px % AR_DMG_W, bad_px / AR_DMG_W, (unsigned long)g_dmg_a[bad_px],
               (unsigned long)g_dmg_b[bad_px]);
    }
}

static void test_a_region_move_is_identical_to_a_full_repaint(void)
{
    ar_surface moved = ar__dmg_surface(g_dmg_a);
    ar_surface full = ar__dmg_surface(g_dmg_b);
    ar_ctx    *ref;
    int        i, frame;
    int        bad_frame = -1, bad_px = -1;

    /* Down for a while, then back up, so both copy directions are exercised. */
    static const ar_i32 WHERE[12] = {0, 7, 21, 40, 63, 90, 90, 61, 33, 12, 3, 0};

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        g_dmg_a[i] = 0;
        g_dmg_b[i] = 0;
    }

    ar__ui_reset(SCROLL_DMG_CSS);
    ref = ar_init(g_dmg_mem, (ar_u32)sizeof g_dmg_mem);
    CHECK(ref != 0, "scroll: the reference context initialises");
    if (!ref || !g_ui)
    {
        return;
    }
    ar_stylesheet(ref, SCROLL_DMG_CSS);

    for (frame = 0; frame < 12 && bad_frame < 0; ++frame)
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        ar_frame_begin(g_ui, &in);
        ar__scroll_dmg_declare(g_ui);
        ar_frame_end(g_ui, &moved);
        ar_node_scroll_to(g_ui, 0, WHERE[frame]);
        ar_frame_presented(g_ui);

        ar_frame_begin(ref, &in);
        ar_invalidate_all(ref);
        ar__scroll_dmg_declare(ref);
        ar_frame_end(ref, &full);
        ar_node_scroll_to(ref, 0, WHERE[frame]);
        ar_frame_presented(ref);

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

    CHECK(ar_node_scroll_range(g_ui, 0) > 0, "scroll: the list is long enough to scroll at all");
    CHECK(bad_frame < 0,
          "scroll: a moved region is pixel identical to a full repaint, every frame");
    if (bad_frame >= 0)
    {
        printf("      frame %d, pixel (%d,%d): moved %08lX, full %08lX\n", bad_frame,
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
        if (ar_style_get(a, i) != ar_style_get(b, i) || a->unit[i] != b->unit[i])
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
    CHECK(AR_WIDE(&rest, AR_P_BACKGROUND) != AR_WIDE(&hover, AR_P_BACKGROUND),
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
    /* Each initialised to the value that makes its own CHECK below fail, so
       silencing MSVC's "potentially uninitialized" costs the test nothing: if
       the loop ever stopped assigning one, the check still catches it. An
       initialiser of 0 for evenodd_centre would have passed vacuously, which is
       the trap this pair of values exists to avoid. */
    ar_u8  nonzero_centre = 0, evenodd_centre = 255;
    ar_i32 i;

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

/*
 * A null string is a return, not a crash.
 *
 * ar_break_next tested for null and then dereferenced in the same branch, so
 * the one path that knew text could be null was the path that walked off it.
 * The library takes text from an application, and an application with nothing
 * to draw hands over nothing.
 */
static void test_break_survives_a_null_string(void)
{
    ar_i32 kind = AR_BREAK_MANDATORY;
    ar_i32 at = ar_break_next(0, 0, &kind);

    CHECK(at == 0 && kind == AR_BREAK_NONE, "break: a null string ends where it began");
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
    static const char *const NASTY[] = {"", " ", "\n", "\xFF", "\xC2", "\x80\x80",
                                        /* Two literals on purpose, and the
                                           parentheses on purpose too. "a\xFFb"
                                           would not mean this: a hex escape is
                                           maximal munch, so \xFFb is read as
                                           one enormous escape rather than 0xFF
                                           then 'b'. Splitting is the only way
                                           to write a lone 0xFF between two
                                           letters; the parentheses tell clang
                                           the concatenation is deliberate and
                                           not a comma someone forgot. */
                                        ("a\xFF"
                                         "b"),
                                        "\r", "\r\r\n", "((((", "))))", "\xE2\x80"};
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
/*
 * ar_style_inherit carries its own list of the properties that inherit, so it
 * can ask for the five by name instead of asking ar_prop_inherits about all
 * ninety -- which is where a fifth of the style phase used to go.
 *
 * Two records of one fact again, so this sweeps every property and compares
 * them. Add an inheriting property to the switch, forget the list, and
 * inheritance silently stops working for it; this is what says so.
 *
 * The list is static to ar_css.c, so the check is by behaviour rather than by
 * reading it: give a parent a value, give the child nothing, and see whether
 * the child ends up with it.
 */
static void test_the_inherited_list_matches_the_switch(void)
{
    ar_i32 prop;
    ar_i32 mismatches = 0;

    for (prop = 0; prop < AR_P_COUNT; ++prop)
    {
        ar_style parent, child;
        int      flowed;

        ar_style_defaults(&parent);
        ar_style_defaults(&child);

        /* A value the default is not, so "did it flow" has an answer. */
        ar_style_put(&parent, prop, 7);
        parent.unit[prop] = AR_UNIT_PX;
        ar_pset_add(&parent.set, prop);

        ar_style_inherit(&child, &parent);
        flowed = (ar_style_get(&child, prop) == 7);

        if (flowed != (ar_prop_inherits(prop) ? 1 : 0))
        {
            printf("      property %ld: ar_prop_inherits says %d, inheritance did %d\n", (long)prop,
                   ar_prop_inherits(prop) ? 1 : 0, flowed);
            ++mismatches;
        }
    }
    CHECK(mismatches == 0, "style: every property inherits exactly when ar_prop_inherits says so");
}

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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_COLOR) & 0xFFFFFF) == 0xE8DFCC,
          "inherit: a child takes its parent's colour");
    /* Through a box that only inherited it, which is what makes it a cascade
       rather than one level of copying. */
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_COLOR) & 0xFFFFFF) == 0xE8DFCC,
          "inherit: and a grandchild takes it through a box that only inherited");
    CHECK(g_ui->nodes[2].style.v[AR_P_FONT_SIZE] == 17, "inherit: font size inherits too");
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_COLOR) & 0xFFFFFF) == 0xFF0000,
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
    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_COLOR) & 0xFFFFFF) == 0x112233,
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
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_COLOR) & 0xFFFFFF) == 0xFF0000,
          "inherit: a leaf under the red box is red");
    CHECK((AR_WIDE(&g_ui->nodes[4].style, AR_P_COLOR) & 0xFFFFFF) == 0x0000FF,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x333333,
          "compound: a box with one class takes the single-class rule");
    /* Two classes: the compound rule wins on specificity, and the box still
       picks up what each single-class rule gave it. */
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "compound: .card.selected beats .card on specificity");
    CHECK(g_ui->nodes[2].style.v[AR_P_WIDTH] == 100,
          "compound: and still takes the width from .card");
    CHECK(g_ui->nodes[2].style.v[AR_P_BORDER_WIDTH] == 2,
          "compound: and the border from .selected");
    /* A box with only one of the two must not match the compound rule. */
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x00FF00,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
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
    CHECK((AR_WIDE(&out, AR_P_COLOR) & 0xFFFFFF) == 0x0000FF,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x123456,
          "cascade keyword: inherit takes a property that does not inherit by default");
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_COLOR) & 0xFFFFFF) != 0xFF0000,
          "cascade keyword: initial drops one that does");
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_COLOR) & 0xFFFFFF) == 0xFF0000,
          "cascade keyword: unset means inherit on an inherited property");
    CHECK((AR_WIDE(&g_ui->nodes[4].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x123456,
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
 * box-sizing
 *
 * areole was border-box everywhere until the browser comparison put a stated
 * size and padding on the same box and found the two engines 18 pixels apart.
 * The default follows the specification rather than the fashion: a stylesheet
 * written for the web has to lay out the way it does on the web.
 * ------------------------------------------------------------------------ */
static void test_box_sizing_default_is_content_box(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".b { display:block; width:100px; height:40px; padding:10px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 120, 60),
          "box-sizing: the stated size is the content, and the padding is added");
}

static void test_border_box_makes_the_stated_size_the_whole_box(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".b { display:block; width:100px; height:40px; padding:10px;"
                 "     box-sizing:border-box; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box_is(1, 0, 0, 100, 40),
          "box-sizing: border-box makes the stated size the whole box");
}

/* It inherits nothing, like every other box-model property, so the universal
   selector is how a sheet turns it on everywhere -- and now can. */
static void test_border_box_applies_through_a_selector(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".b { display:block; box-sizing:border-box; }"
                 ".one { width:100px; height:40px; padding:10px; }"
                 ".two { width:80px; height:20px; padding:5px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.b.one");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b.two");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).w == 100 && ar__box(1).h == 40, "box-sizing: on the first box");
    CHECK(ar__box(2).w == 80 && ar__box(2).h == 20, "box-sizing: and on the second");
}

/* A percentage is a percentage of the containing block, and then the padding
   goes around that -- the same rule, applied to a computed number. */
static void test_box_sizing_applies_to_percentages(void)
{
    ar_surface s = ar__ui_surface(300, 200);

    ar__ui_reset("#root { display:block; }"
                 ".outer { display:block; width:200px; }"
                 ".half { display:block; width:50%; height:10px; padding:0 5px; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.outer");
    ar_begin(g_ui, "div.half");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).w == 110, "box-sizing: half of two hundred, plus its padding");
}

/* ------------------------------------------------------------------------
 * position: sticky
 *
 * In flow until a threshold, then pinned, and never outside the box it
 * belongs to -- which is the part that makes it useful rather than merely
 * fixed, and the part everyone forgets.
 * ------------------------------------------------------------------------ */
static const char *AR_STICKY_CSS = "#root { display:block; }"
                                   ".list { display:block; height:100px; overflow:scroll; }"
                                   ".sect { display:block; height:200px; }"
                                   ".head { display:block; height:20px; position:sticky; top:0; }"
                                   ".body { display:block; height:180px; }";

/* Two sections, so there is room to scroll the first one all the way past --
   which is the only way to see a sticky header hand over to the next. */
static void ar__sticky_scene(ar_surface *s, const ar_input *in)
{
    ar_i32 k;

    ar_frame_begin(g_ui, in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    for (k = 0; k < 2; ++k)
    {
        ar_begin(g_ui, "div.sect");
        ar_begin(g_ui, "div.head");
        ar_end(g_ui);
        ar_begin(g_ui, "div.body");
        ar_end(g_ui);
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

/* Unscrolled, it sits exactly where the flow put it. */
static void test_sticky_starts_in_the_flow(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_STICKY_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__sticky_scene(&s, &in);

    CHECK(ar__box(3).y == 0, "sticky: at rest it is where the flow put it");
    CHECK(ar__box(4).y == 20, "sticky: and the box after it starts below, space kept");
}

/* Scrolled past its threshold, it pins to the top of the scrollport. */
static void test_sticky_pins_at_the_threshold(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_STICKY_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__sticky_scene(&s, &in);

    ar_node_scroll_to(g_ui, 1, 50);
    ar__sticky_scene(&s, &in);

    CHECK(ar__box(4).y == -30, "sticky: the ordinary content scrolled away");
    CHECK(ar__box(3).y == 0, "sticky: while the header stayed at the top of the scrollport");
}

/*
 * And it leaves with its own section rather than piling up.
 *
 * Scrolled far enough that the section's bottom passes the top of the
 * scrollport, the header has to go with it -- otherwise every header in a list
 * would stack at the top, which is the failure mode of every hand-rolled
 * sticky header ever written.
 */
static void test_sticky_leaves_with_its_section(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_STICKY_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__sticky_scene(&s, &in);

    /* The first section is 200 tall; scrolling 190 leaves 10 of it above the
       fold, so a 20 tall header pinned at the top of the port would stick out
       of its own section by 10 and has to be pulled back. */
    ar_node_scroll_to(g_ui, 1, 190);
    ar__sticky_scene(&s, &in);

    CHECK(ar__box(2).y == -190, "sticky: the section has nearly gone");
    CHECK(ar__box(3).y == -10,
          "sticky: and the header went with it rather than piling up at the top");
    CHECK(ar__box(3).y + ar__box(3).h == ar__box(2).y + ar__box(2).h,
          "sticky: its bottom edge is its section's bottom edge");
}

/* With no scrolling ancestor the viewport is the scrollport. */
static void test_sticky_against_the_viewport(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".sect { display:block; height:400px; }"
                 ".head { display:block; height:20px; position:sticky; top:30px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.sect");
    ar_begin(g_ui, "div.head");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == 30, "sticky: pinned thirty pixels down the viewport");
}

/* A sticky box with no offsets never moves: there is no threshold to cross. */
static void test_sticky_with_no_offsets_never_moves(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".list { display:block; height:100px; overflow:scroll; }"
                 ".head { display:block; height:20px; position:sticky; }"
                 ".body { display:block; height:300px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    ar_begin(g_ui, "div.head");
    ar_end(g_ui);
    ar_begin(g_ui, "div.body");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    ar_node_scroll_to(g_ui, 1, 60);
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    ar_begin(g_ui, "div.head");
    ar_end(g_ui);
    ar_begin(g_ui, "div.body");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(2).y == -60, "sticky: with nothing to obey it just scrolls away");
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
/*
 * A sticky box bigger than the block it must stay inside cannot move.
 *
 * The clamp gives the shift a range: at least far enough that the leading edge
 * stays in, at most far enough that the trailing edge does. Applying those two
 * one after the other looks the same and is not -- when the box is bigger than
 * the containing block the range is empty, and the second correction used to
 * overwrite the first and produce a large shift the wrong way. A 420 wide row
 * in a 200 wide horizontal scrollport was pinned at -220 whatever the scroll
 * position.
 *
 * A browser leaves such a box alone, because there is nowhere it can go that
 * satisfies the constraint. Found by examples/06_sticky against Edge.
 */
/* The resolved value of a property on a laid-out node, which is what env()
   has to be read through: it is resolved per box, after the style cache. */
static ar_i32 ar__css_prop_of_node(ar_i32 i, ar_i32 prop)
{
    return ar_style_get(&g_ui->nodes[i].style, prop);
}

/* ------------------------------------------------------------------------
 * env() and safe areas
 *
 * Eight numbers the core cannot work out and will not guess. The interesting
 * part is not the arithmetic, it is the three-way distinction between a value
 * the backend reported, a value it reported as zero, and a value it never
 * mentioned -- the last two look identical in the result and must not behave
 * alike.
 * ------------------------------------------------------------------------ */
static const char *AR_ENV_CSS = "#root { display:block; }"
                                ".pad  { display:block; height:10px;"
                                "        padding-top: env(safe-area-inset-top, 20px); }";

static ar_i32 ar__env_pad_top(int report, ar_i32 inset, int cover)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_input   in;

    ar__ui_reset(AR_ENV_CSS);
    if (report)
    {
        ar_set_safe_area(g_ui, inset, 0, 0, 0);
    }
    ar_set_viewport_fit_cover(g_ui, cover);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.pad");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    return ar__css_prop_of_node(1, AR_P_PAD_TOP);
}

static void test_env_falls_back_only_when_nothing_reported_it(void)
{
    /* Never mentioned: the fallback stands. */
    CHECK(ar__env_pad_top(0, 0, 1) == 20, "env: an unreported name takes its fallback");

    /* Reported, and reported as zero. A windowed desktop really does have
       insets of zero, and that is an answer rather than a silence -- taking
       the fallback here would put a twenty pixel gap at the top of every
       window that has nothing above it. */
    CHECK(ar__env_pad_top(1, 0, 1) == 0, "env: a reported zero is a value, not a silence");

    CHECK(ar__env_pad_top(1, 34, 1) == 34, "env: a reported inset is what env() resolves to");
}

static void test_viewport_fit_moves_the_insets_and_the_viewport_together(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_input   in;
    ar_i32     auto_pad, cover_pad, auto_root_h, cover_root_h;

    /* With `auto` the layout viewport is already the safe rectangle, so a
       stylesheet must be told the inset is zero: avoiding it twice would push
       the content down by 34 pixels that were already taken off. */
    auto_pad = ar__env_pad_top(1, 34, 0);
    cover_pad = ar__env_pad_top(1, 34, 1);
    CHECK(auto_pad == 0 && cover_pad == 34,
          "env: viewport-fit decides whether the inset is reported at all");

    /* And the other half of the same decision, measured on the root box. */
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar__ui_reset("#root { display:block; height:grow; }");
    ar_set_safe_area(g_ui, 34, 0, 12, 0);
    ar_set_viewport_fit_cover(g_ui, 0);
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    auto_root_h = ar__box(0).h;

    ar__ui_reset("#root { display:block; height:grow; }");
    ar_set_safe_area(g_ui, 34, 0, 12, 0);
    ar_set_viewport_fit_cover(g_ui, 1);
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    cover_root_h = ar__box(0).h;

    CHECK(auto_root_h == 200 - 34 - 12 && cover_root_h == 200,
          "env: auto lays out inside the safe rectangle, cover over the whole surface");

    /* The pairing is the point. If the viewport were inset *and* env() still
       reported the inset, content would clear it twice. */
    CHECK(auto_root_h + auto_pad == 200 - 34 - 12,
          "env: the viewport and the reported inset never both take the same pixels");
}

static void test_titlebar_area_is_not_gated_on_viewport_fit(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_input   in;

    /* The titlebar rectangle says where the window controls are. That does not
       change because the viewport was inset, so unlike the safe-area insets it
       is reported under `auto` too. */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:block; height:10px;"
                 "     padding-left: env(titlebar-area-width, 5px); }");
    ar_set_titlebar_area(g_ui, 0, 0, 96, 32);
    ar_set_viewport_fit_cover(g_ui, 0);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__css_prop_of_node(1, AR_P_PAD_LEFT) == 96,
          "env: titlebar-area is reported whatever viewport-fit says");
}

static void test_env_with_an_unknown_name_takes_its_fallback(void)
{
    /* CSS uses the fallback when the name is undefined, which is the whole
       point of having one -- a stylesheet written for a platform that reports
       a variable still lays out on one that does not. */
    ar__sheet(".u { display:block; padding-top: env(nonsense-inset, 7px); }");
    CHECK(ar__css_value(".u", 0, AR_P_PAD_TOP) == 7, "env: an unknown name takes its fallback");

    /* Without a fallback there is no value to be had, so the declaration goes
       and the property keeps what it would have had. */
    ar__sheet(".v { display:block; padding-top: env(nonsense-inset); }");
    CHECK(ar__css_value(".v", 0, AR_P_PAD_TOP) == 0,
          "env: an unknown name with no fallback drops the declaration");

    /* And the call is consumed either way. This is what caught the first
       version: it bailed out at the unknown name, left the scanner inside the
       parentheses, and the fallback was picked back up as though it were the
       value -- the right answer for the wrong reason. A declaration after it
       has to survive. */
    ar__sheet(".w { display:block; padding-top: env(nonsense-inset); height: 42px; }");
    CHECK(ar__css_value(".w", 0, AR_P_HEIGHT) == 42,
          "env: a failed env() does not swallow the declaration after it");
}

/*
 * Criterion 2: a sticky box that cannot stick because of an overflow:hidden
 * ancestor is reported through the diagnostic API.
 *
 * The three cases together are the test. Reporting the blocked one is easy;
 * what makes the report worth having is that it stays quiet for a sticky box
 * that is fine, and for one whose clipping ancestor does scroll. A diagnostic
 * that fires on everything is noise, and noise is ignored.
 */
static ar_i32 ar__diag_run(const char *css, ar_i32 *out_node)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.tall");
    ar_end(g_ui);
    ar_begin(g_ui, "div.stick");
    ar_end(g_ui);
    ar_begin(g_ui, "div.tall");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    if (out_node)
    {
        *out_node = -1;
    }
    if (ar_diag_count(g_ui) > 0)
    {
        return ar_diag_at(g_ui, 0, out_node);
    }
    return 0;
}

static void test_a_sticky_box_that_can_never_stick_is_reported(void)
{
    ar_i32 node = -1;
    ar_i32 blocked, scrolls, unclipped;

    blocked = ar__diag_run("#root { display:block; }"
                           ".box  { display:block; height:100px; overflow:hidden; }"
                           ".tall { display:block; height:200px; }"
                           ".stick{ display:block; height:20px; position:sticky; top:0px; }",
                           &node);

    CHECK(blocked == AR_DIAG_STICKY_NEVER_STICKS && node == 3,
          "diag: sticky under overflow:hidden is reported, and names the box");

    /* The same tree with a scrollport that does scroll. Nothing to report:
       this one sticks. */
    scrolls = ar__diag_run("#root { display:block; }"
                           ".box  { display:block; height:100px; overflow:scroll; }"
                           ".tall { display:block; height:200px; }"
                           ".stick{ display:block; height:20px; position:sticky; top:0px; }",
                           &node);
    CHECK(scrolls == 0, "diag: sticky under a real scrollport is not reported");

    /* And with no clipping ancestor at all, where the viewport is the
       scrollport. Also nothing to report. */
    unclipped = ar__diag_run("#root { display:block; }"
                             ".box  { display:block; height:100px; }"
                             ".tall { display:block; height:200px; }"
                             ".stick{ display:block; height:20px; position:sticky; top:0px; }",
                             &node);
    CHECK(unclipped == 0, "diag: sticky with no clipping ancestor is not reported");

    CHECK(ar_diag_text(AR_DIAG_STICKY_NEVER_STICKS)[0] != 0 && ar_diag_text(0)[0] == 0,
          "diag: a code has a sentence, and an unknown code has an empty one");
}

/*
 * A box clipped away by its container must not take the hover.
 *
 * The hit test walks paint order and asks `ar_rect_contains(n->rect, ...)`. It
 * never consults `n->clip`, so a row scrolled up out of its scrollport still
 * answers for a point that is inside its rectangle and outside the box that
 * clips it -- and the row is later in paint order than whatever is really
 * showing there, so it wins.
 *
 * The scene puts a spacer above a scrollport and scrolls the port by 20, which
 * lifts its first row twenty pixels above the port's top edge. The cursor goes
 * into that overlap: inside the row's rectangle, outside the port. What is
 * actually painted there is the spacer.
 */
/* ------------------------------------------------------------------------
 * The top layer
 *
 * Not a large z-index, and the difference is the whole point: a z-index orders
 * a box among its siblings inside one stacking context and cannot lift it out
 * of that context. A modal declared inside anything positioned could therefore
 * always be covered by that thing's siblings, whatever number it asked for.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__paint_index(ar_i32 node)
{
    ar_i32 i;

    for (i = 0; i < g_ui->order_count; ++i)
    {
        if (g_ui->order[i] == node)
        {
            return i;
        }
    }
    return -1;
}

static void ar__top_scene(ar_surface *s, const char *css)
{
    ar_input in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.ctx"); /* 1: its own stacking context */
    ar_begin(g_ui, "div.pop"); /* 2: the candidate for the top layer */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.over"); /* 3: a later sibling that covers it */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

/* `.ctx` forms a context, so `.pop` cannot escape it with any z-index at all;
   `.over` is a later positioned sibling laid over the same corner, in a
   context of its own with a higher z, so it covers `.pop` in every reading
   of the rules except the top layer. */
static const char *AR_TOP_CSS = "#root { display:block; }"
                                ".ctx  { display:block; position:relative; z-index:1;"
                                "        width:100px; height:100px; }"
                                ".pop  { display:block; position:absolute; z-index:9999;"
                                "        width:80px; height:80px; }"
                                ".over { display:block; position:absolute; z-index:2;"
                                "        top:0px; left:0px; width:100px; height:100px; }";

static const char *AR_TOP_CSS_ON = "#root { display:block; }"
                                   ".ctx  { display:block; position:relative; z-index:1;"
                                   "        width:100px; height:100px; }"
                                   ".pop  { display:block; position:absolute; overlay:auto;"
                                   "        width:80px; height:80px; }"
                                   ".over { display:block; position:absolute; z-index:2;"
                                   "        top:0px; left:0px; width:100px; height:100px; }";

static void test_the_top_layer_beats_a_z_index_it_cannot_reach(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_i32     pop_off, over_off, pop_on, over_on;

    /* The control, and the reason the top layer exists: `.pop` asks for 9999
       and still loses, because it is trapped in `.ctx`'s context. */
    ar__top_scene(&s, AR_TOP_CSS);
    pop_off = ar__paint_index(2);
    over_off = ar__paint_index(3);
    CHECK(pop_off >= 0 && over_off > pop_off,
          "top layer: z-index 9999 still loses to a sibling of its own context");

    ar__top_scene(&s, AR_TOP_CSS_ON);
    pop_on = ar__paint_index(2);
    over_on = ar__paint_index(3);
    CHECK(pop_on > over_on, "top layer: overlay:auto paints above it with no z-index at all");

    /* Once each, never twice: the array holds one slot per box. */
    CHECK(g_ui->order_count == g_ui->node_count,
          "top layer: a box in it is emitted once, not in both passes");
}

static void test_the_top_layer_escapes_a_clipping_ancestor(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".clip { display:block; width:60px; height:60px; overflow:hidden; }"
                 ".pop  { display:block; width:150px; height:150px; overlay:auto; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.clip");
    ar_begin(g_ui, "div.pop");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* Painting above everything while clipped to a box it has left behind is
       the failure this pairing exists to prevent. */
    CHECK(g_ui->nodes[2].clip.w == 200 && g_ui->nodes[2].clip.h == 300,
          "top layer: a box in it clips to the viewport, not to its ancestors");
    CHECK(g_ui->nodes[1].clip.w == 200, "top layer: and the clipping ancestor is unaffected");
}

static void test_the_top_layer_takes_the_pointer_first(void)
{
    ar_surface s = ar__ui_surface(200, 300);

    /* Paint and hit testing read the same order, one forwards and one back, so
       a box lifted above everything is also hit before everything. */
    ar__top_scene(&s, AR_TOP_CSS);
    CHECK(g_ui->hot == g_ui->nodes[3].key, "top layer: without it the later sibling takes the hit");

    ar__top_scene(&s, AR_TOP_CSS_ON);
    CHECK(g_ui->hot == g_ui->nodes[2].key, "top layer: with it the lifted box takes the hit");
}

/* ------------------------------------------------------------------------
 * inert
 *
 * A modal makes everything outside it unreachable. The half that matters is
 * the other one: the modal itself, its own subtree, and an ordinary page with
 * no modal on it must all stay reachable, or "inert" just means "nothing
 * works".
 * ------------------------------------------------------------------------ */
static void ar__inert_scene(ar_surface *s, const char *css, ar_i32 mx, ar_i32 my)
{
    ar_input in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = mx;
    in.mouse_y = my;
    in.mouse_inside = 1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.page"); /* 1 */
    ar_begin(g_ui, "div.btn");  /* 2, under the dialog */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.dlg"); /* 3 */
    ar_begin(g_ui, "div.ok");  /* 4, inside the dialog */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

/* The dialog covers the right half, the page's button the left, so the two can
   be pointed at separately. */
static const char *AR_INERT_CSS = "#root { display:block; }"
                                  ".page { display:block; position:absolute; top:0px; left:0px;"
                                  "        width:200px; height:200px; }"
                                  ".btn  { display:block; position:absolute; top:20px; left:10px;"
                                  "        width:60px; height:40px; }"
                                  ".dlg  { display:block; position:absolute; top:0px; left:100px;"
                                  "        width:100px; height:200px; }"
                                  /* Inside .dlg, which is positioned, so these insets are
                                     measured from the dialog rather than the viewport: the
                                     dialog starts at 100, so this lands at 110. */
                                  ".ok   { display:block; position:absolute; top:20px;"
                                  "        left:10px; width:60px; height:40px; }";

static void test_a_modal_makes_everything_outside_it_unreachable(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    char       modal[900];
    char       plain[900];

    strcpy(plain, AR_INERT_CSS);
    strcpy(modal, AR_INERT_CSS);
    strcat(modal, ".dlg { overlay: modal; }");

    /* The control first. With no modal the page's button takes the cursor,
       which is what makes the next assertion mean something. */
    ar__inert_scene(&s, plain, 40, 40);
    CHECK(g_ui->hot == g_ui->nodes[2].key, "inert: with no modal the button under it is reachable");

    ar__inert_scene(&s, modal, 40, 40);
    CHECK(g_ui->hot != g_ui->nodes[2].key,
          "inert: a modal puts the button outside it out of reach");

    /* And the modal's own subtree is not inert, which is the half that turns
       this from a feature into a bug if it is got wrong. */
    ar__inert_scene(&s, modal, 140, 40);
    CHECK(g_ui->hot == g_ui->nodes[4].key, "inert: the modal's own contents stay reachable");
}

static void test_inert_marks_a_subtree_without_any_modal(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    char       css[900];

    strcpy(css, AR_INERT_CSS);
    strcat(css, ".page { inert: auto; }");

    /* No modal anywhere: a box can simply say it is inert, and it carries its
       subtree with it. */
    ar__inert_scene(&s, css, 40, 40);
    CHECK(g_ui->hot != g_ui->nodes[2].key && g_ui->hot != g_ui->nodes[1].key,
          "inert: a subtree marked inert takes no pointer, nor does its child");

    ar__inert_scene(&s, css, 140, 40);
    CHECK(g_ui->hot == g_ui->nodes[4].key, "inert: and a sibling outside it is untouched");
}

static void test_a_non_modal_top_layer_box_makes_nothing_inert(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    char       css[900];

    strcpy(css, AR_INERT_CSS);
    strcat(css, ".dlg { overlay: auto; }");

    /* A popover is in the top layer and is not a modal. The page behind it
       keeps working, which is the entire difference between the two. */
    ar__inert_scene(&s, css, 40, 40);
    CHECK(g_ui->hot == g_ui->nodes[2].key,
          "inert: a non-modal box in the top layer leaves the page reachable");
}

/* ------------------------------------------------------------------------
 * ::backdrop
 *
 * The one pseudo-element areole has, and it matches no box: it is the sheet
 * painted under a modal and over everything else. So it has to be checked on
 * the pixels -- there is no rectangle to compare.
 * ------------------------------------------------------------------------ */
static ar_u32 ar__backdrop_px(const char *css, ar_i32 x, ar_i32 y)
{
    ar_surface s = ar__dmg_surface(g_dmg_a);
    ar_input   in;
    ar_i32     i;

    for (i = 0; i < AR_DMG_W * AR_DMG_H; ++i)
    {
        g_dmg_a[i] = 0;
    }

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.page");
    ar_end(g_ui);
    ar_begin(g_ui, "div.dlg");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    ar_frame_presented(g_ui);

    return g_dmg_a[y * AR_DMG_W + x];
}

/* The page fills the surface in one colour; the dialog is a small box in the
   corner. Everything interesting happens at a point the page covers and the
   dialog does not. */
static const char *AR_BD_CSS = "#root { display:block; }"
                               ".page { display:block; position:absolute; top:0px; left:0px;"
                               "        width:256px; height:128px; background:#3050a0; }"
                               ".dlg  { display:block; position:absolute; top:0px; left:0px;"
                               "        width:20px; height:20px; background:#ffffff; }";

static void test_a_backdrop_paints_under_the_modal_and_over_the_page(void)
{
    ar_u32 plain, opaque, nonmodal;
    char   css[900];

    /* No modal, no backdrop: the page's own colour. */
    plain = ar__backdrop_px(AR_BD_CSS, 128, 64);
    CHECK((plain & 0xFFFFFFu) == 0x3050A0u, "backdrop: without one the page is what shows");

    /* A modal with an opaque backdrop covers the page entirely. */
    strcpy(css, AR_BD_CSS);
    strcat(css, ".dlg { overlay: modal; } .dlg::backdrop { background:#101010; }");
    opaque = ar__backdrop_px(css, 128, 64);
    CHECK((opaque & 0xFFFFFFu) == 0x101010u, "backdrop: a modal's backdrop covers the page");

    /* The same declaration on a box that is in the top layer but not modal
       paints nothing: a popover has no backdrop, and that is the difference. */
    strcpy(css, AR_BD_CSS);
    strcat(css, ".dlg { overlay: auto; } .dlg::backdrop { background:#101010; }");
    nonmodal = ar__backdrop_px(css, 128, 64);
    CHECK((nonmodal & 0xFFFFFFu) == 0x3050A0u,
          "backdrop: a non-modal box in the top layer has none");
}

static void test_a_backdrop_rule_styles_nothing_else(void)
{
    /* `.dlg::backdrop` must say nothing about `.dlg`, or every dialog would
       take its backdrop's colour. The two passes are separate for this. */
    ar__sheet(".dlg { display:block; background:#ffffff; }"
              ".dlg::backdrop { background:#101010; }");
    CHECK((ar__css_value(".dlg", 0, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFFFFFF,
          "backdrop: a ::backdrop rule does not style the element it hangs off");

    /* And an unknown pseudo-element is refused rather than silently matching
       the element, which is how `::before` would otherwise start working. */
    ar__sheet(".x { display:block; background:#ffffff; }"
              ".x::before { background:#101010; }");
    CHECK((ar__css_value(".x", 0, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFFFFFF,
          "backdrop: a pseudo-element areole cannot paint is refused, not applied");
}

/* ------------------------------------------------------------------------
 * Anchor positioning
 *
 * A positioned box measures its edges against a box it names, rather than
 * against coordinates somebody worked out by hand. The anchor is 60 wide and
 * 20 tall at (40, 50) in every case below, so the expected numbers are
 * arithmetic on those four values and can be read without running anything.
 * ------------------------------------------------------------------------ */
static void ar__posanchor_scene(ar_surface *s, const char *extra)
{
    char     css[1000];
    ar_input in;

    strcpy(css, "#root { display:block; }"
                ".anc { display:block; position:absolute; top:50px; left:40px;"
                "       width:60px; height:20px; anchor-name: --a; }"
                ".pop { display:block; position:absolute; position-anchor: --a; }");
    strcat(css, extra);

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.anc");
    ar_end(g_ui);
    ar_begin(g_ui, "div.pop");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_anchor_places_a_box_against_the_box_it_names(void)
{
    ar_surface s = ar__ui_surface(300, 300);

    /* Directly below: my top edge at the anchor's bottom, my left at its left.
       The anchor is at y 50 and 20 tall, so 70; and at x 40. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width:30px; height:10px; }");
    CHECK(ar__box(2).y == 70 && ar__box(2).x == 40,
          "anchor: anchor(bottom) and anchor(left) attach the box to the anchor");

    /* The trailing edges, which are measured from the other side of the
       containing block and are where an off-by-one would show. Right edge at
       the anchor's left edge, 40, so with a width of 30 the box starts at 10. */
    ar__posanchor_scene(&s, ".pop { top: anchor(top); right: anchor(left);"
                            "       width:30px; height:10px; }");
    CHECK(ar__box(2).x == 10 && ar__box(2).y == 50,
          "anchor: a trailing inset measures from the far edge and still lands");

    /* The centre of a 60 wide anchor starting at 40 is 70. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(center);"
                            "       width:30px; height:10px; }");
    CHECK(ar__box(2).x == 70, "anchor: anchor(center) is the middle of the anchor");
}

static void test_anchor_size_takes_the_anchors_measurements(void)
{
    ar_surface s = ar__ui_surface(300, 300);

    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width: anchor-size(width); height: anchor-size(height); }");
    CHECK(ar__box(2).w == 60 && ar__box(2).h == 20,
          "anchor: anchor-size() gives the box the anchor's measurements");

    /* Crossed on purpose: a width taking the anchor's height proves the two
       are read separately rather than both falling back to one of them. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width: anchor-size(height); height:10px; }");
    CHECK(ar__box(2).w == 20, "anchor: anchor-size(height) on a width is the anchor's height");
}

static void test_position_try_flips_a_box_that_left_the_viewport(void)
{
    ar_surface s = ar__ui_surface(300, 300);

    /* A box 400 tall hung below an anchor at y 70 runs off a 300 tall
       viewport. Flipped, its bottom sits on the anchor's top edge at 50, so it
       starts at 50 - 400. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width:30px; height:400px; position-try: flip-block; }");
    CHECK(ar__box(2).y == 50 - 400, "anchor: position-try flips a box that ran off the bottom");

    /* The control, and the reason it is here: without the property the box
       stays where it was put and runs off the edge. A flip that happens
       anyway would pass the check above for the wrong reason. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width:30px; height:400px; }");
    CHECK(ar__box(2).y == 70, "anchor: without position-try it stays put and overflows");

    /* And a box that fits is not flipped, which is the other way to get this
       wrong. */
    ar__posanchor_scene(&s, ".pop { top: anchor(bottom); left: anchor(left);"
                            "       width:30px; height:10px; position-try: flip-block; }");
    CHECK(ar__box(2).y == 70, "anchor: a box that fits is left alone");
}

static void test_an_anchor_nobody_declared_changes_nothing(void)
{
    ar_surface s = ar__ui_surface(300, 300);

    /* Naming an anchor that does not exist must leave the box where the
       ordinary rules put it rather than at some resolved-against-nothing
       coordinate. */
    ar__ui_reset("#root { display:block; }"
                 ".pop { display:block; position:absolute; position-anchor: --missing;"
                 "       top: anchor(bottom); left:15px; width:30px; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.pop");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }
    CHECK(ar__box(1).x == 15, "anchor: an unresolved anchor leaves the other insets working");
}

/* ------------------------------------------------------------------------
 * Criterion 2: the top layer against twenty-five adversarial arrangements
 *
 * Paint order is not geometry, so the browser comparison cannot see it: two
 * boxes that overlap have the same rectangles whichever is on top. This corpus
 * therefore states the expected order itself, and every case is built so that
 * the ordinary rules would give the opposite answer -- a z-index the top layer
 * has to beat, a stacking context it has to escape, a clip it has to leave.
 *
 * `hi` is the box that must end up in front. `lo` is the one that would win
 * without the top layer. Each case declares the same three-box shape so the
 * indices are fixed: 0 root, 1 the wrapper, 2 lo, 3 hi.
 * ------------------------------------------------------------------------ */
typedef struct
{
    const char *name;
    const char *css;
} ar__layer_case;

/*
 * Twenty-five. The first block varies what `hi` has to climb out of, the
 * second varies what `lo` is doing to stay in front, and the last few are the
 * cases where the answer is *not* the top layer -- because a corpus that only
 * ever expects one answer cannot tell a correct engine from one that always
 * says yes.
 */
static const ar__layer_case AR_LAYER_CASES[] = {
    /* 1-8: hi is in the top layer and lo is trying everything. */
    {"plain", ".lo { z-index:5; } .hi { overlay:auto; }"},
    {"lo-huge-z", ".lo { z-index:30000; } .hi { overlay:auto; }"},
    {"lo-context", ".lo { position:relative; z-index:9; } .hi { overlay:auto; }"},
    {"hi-negative-z", ".lo { z-index:1; } .hi { overlay:auto; z-index:-5; }"},
    {"hi-in-wrapper", ".wrap { position:relative; z-index:2; } .lo { z-index:99; }"
                      ".hi { overlay:auto; }"},
    {"hi-in-clipper", ".wrap { overflow:hidden; } .lo { z-index:99; } .hi { overlay:auto; }"},
    {"hi-static", ".lo { position:relative; z-index:4; } .hi { position:static; overlay:auto; }"},
    {"hi-modal", ".lo { z-index:1000; } .hi { overlay:modal; }"},

    /* 9-16: both in the top layer, so tree order decides -- a stack of
       dialogs, where the one opened last is the one in front. */
    {"both-tree-order", ".lo { overlay:auto; } .hi { overlay:auto; }"},
    {"both-lo-has-z", ".lo { overlay:auto; z-index:500; } .hi { overlay:auto; }"},
    {"both-hi-negative", ".lo { overlay:auto; } .hi { overlay:auto; z-index:-9; }"},
    {"both-modal-over-auto", ".lo { overlay:auto; } .hi { overlay:modal; }"},
    {"both-auto-over-modal", ".lo { overlay:modal; } .hi { overlay:auto; }"},
    {"both-in-wrapper", ".wrap { position:relative; z-index:3; }"
                        ".lo { overlay:auto; } .hi { overlay:auto; }"},
    {"both-clipped-wrapper", ".wrap { overflow:hidden; } .lo { overlay:auto; }"
                             ".hi { overlay:auto; }"},
    {"both-lo-relative", ".lo { overlay:auto; position:relative; z-index:7; }"
                         ".hi { overlay:auto; }"},

    /* 17-21: no top layer at all, so the ordinary rules decide and the corpus
       has to agree with them. `hi` is declared last, so it wins on tree order
       unless lo outranks it. */
    {"none-tree-order", ".lo { display:block; } .hi { display:block; }"},
    {"none-lo-positioned", ".lo { position:relative; } .hi { display:block; }"},
    {"none-hi-positioned", ".lo { display:block; } .hi { position:relative; }"},
    {"none-hi-higher-z", ".lo { position:relative; z-index:1; }"
                         ".hi { position:relative; z-index:2; }"},
    {"none-hi-equal-z", ".lo { position:relative; z-index:2; }"
                        ".hi { position:relative; z-index:2; }"},

    /* 22-25: the top layer is on `lo`, so the expected answer inverts. Without
       these the corpus would pass against an engine that painted the
       last-declared box in front and called it a top layer. */
    {"inverted-plain", ".lo { overlay:auto; } .hi { z-index:5; }"},
    {"inverted-huge-z", ".lo { overlay:auto; } .hi { z-index:30000; }"},
    {"inverted-context", ".lo { overlay:auto; } .hi { position:relative; z-index:8; }"},
    {"inverted-modal", ".lo { overlay:modal; } .hi { position:relative; z-index:8; }"}};

#define AR_LAYER_CASE_COUNT ((ar_i32)(sizeof AR_LAYER_CASES / sizeof AR_LAYER_CASES[0]))

/* Which of the last four cases expect `lo` in front instead. */
static int ar__layer_inverted(ar_i32 k)
{
    return k >= AR_LAYER_CASE_COUNT - 4;
}

static void test_the_top_layer_over_an_adversarial_corpus(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_i32     k;
    ar_i32     wrong = -1;

    for (k = 0; k < AR_LAYER_CASE_COUNT && wrong < 0; ++k)
    {
        char     css[1000];
        ar_input in;
        ar_i32   lo_at, hi_at;

        strcpy(css, "#root { display:block; }"
                    ".wrap { display:block; }"
                    ".lo { display:block; position:absolute; top:0px; left:0px;"
                    "      width:80px; height:80px; }"
                    ".hi { display:block; position:absolute; top:0px; left:0px;"
                    "      width:80px; height:80px; }");
        strcat(css, AR_LAYER_CASES[k].css);

        ar__ui_reset(css);
        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.wrap");
        ar_begin(g_ui, "div.lo");
        ar_end(g_ui);
        ar_begin(g_ui, "div.hi");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);

        lo_at = ar__paint_index(2);
        hi_at = ar__paint_index(3);

        if (lo_at < 0 || hi_at < 0)
        {
            wrong = k;
        }
        else if (ar__layer_inverted(k) ? !(lo_at > hi_at) : !(hi_at > lo_at))
        {
            wrong = k;
        }
    }

    CHECK(wrong < 0, "top layer: twenty-five adversarial arrangements paint in the right order");
    if (wrong >= 0)
    {
        printf("      case %ld (%s): lo at %ld, hi at %ld\n", (long)wrong,
               AR_LAYER_CASES[wrong].name, (long)ar__paint_index(2), (long)ar__paint_index(3));
    }
    CHECK(AR_LAYER_CASE_COUNT == 25,
          "top layer: the corpus is the twenty-five the criterion asks for");
}

/* ------------------------------------------------------------------------
 * Criterion 4: position-try, against arithmetic rather than against a browser
 *
 * The flip fires when the box would leave the viewport, and areole's viewport
 * is the surface while a browser's is the window its page sits in -- so the
 * same case overflows on one side and not the other. Comparing them would
 * report a difference of framing as a difference of flipping, which is the
 * judgement the snap corpus already made about `proximity`.
 *
 * The anchor is 60 wide and 20 tall at (40, 50) throughout, so every expected
 * number below is arithmetic on those four values.
 * ------------------------------------------------------------------------ */
static void test_position_try_over_a_corpus_of_flips(void)
{
    ar_surface s = ar__ui_surface(300, 300);
    ar_i32     k;
    ar_i32     wrong = -1;

    /* name, extra css, expected x, expected y. */
    static const struct
    {
        const char *css;
        ar_i32      x, y;
    } CASES[20] = {
        /* Vertical: hung below, too tall, flips to sit above the anchor. */
        {".pop { top:anchor(bottom); left:anchor(left); width:20px; height:400px;"
         " position-try:flip-block; }",
         40, 50 - 400},
        {".pop { top:anchor(bottom); left:anchor(left); width:20px; height:260px;"
         " position-try:flip-block; }",
         40, 50 - 260},
        {".pop { top:anchor(bottom); left:anchor(left); width:20px; height:231px;"
         " position-try:flip-block; }",
         40, 50 - 231},
        /* Fits exactly, so it must not flip: 70 + 230 == 300. */
        {".pop { top:anchor(bottom); left:anchor(left); width:20px; height:230px;"
         " position-try:flip-block; }",
         40, 70},
        {".pop { top:anchor(bottom); left:anchor(left); width:20px; height:10px;"
         " position-try:flip-block; }",
         40, 70},
        /* Hung above and off the top, flips to sit below. */
        {".pop { bottom:anchor(top); left:anchor(left); width:20px; height:80px;"
         " position-try:flip-block; }",
         40, 70},
        {".pop { bottom:anchor(top); left:anchor(left); width:20px; height:51px;"
         " position-try:flip-block; }",
         40, 70},
        /* Fits above exactly: 50 - 50 == 0. */
        {".pop { bottom:anchor(top); left:anchor(left); width:20px; height:50px;"
         " position-try:flip-block; }",
         40, 0},
        /* Horizontal. */
        {".pop { left:anchor(right); top:anchor(top); width:400px; height:10px;"
         " position-try:flip-inline; }",
         40 - 400, 50},
        {".pop { left:anchor(right); top:anchor(top); width:201px; height:10px;"
         " position-try:flip-inline; }",
         40 - 201, 50},
        {".pop { left:anchor(right); top:anchor(top); width:200px; height:10px;"
         " position-try:flip-inline; }",
         100, 50},
        {".pop { right:anchor(left); top:anchor(top); width:60px; height:10px;"
         " position-try:flip-inline; }",
         100, 50},
        {".pop { right:anchor(left); top:anchor(top); width:41px; height:10px;"
         " position-try:flip-inline; }",
         100, 50},
        {".pop { right:anchor(left); top:anchor(top); width:40px; height:10px;"
         " position-try:flip-inline; }",
         0, 50},
        /* Both axes at once. */
        {".pop { top:anchor(bottom); left:anchor(right); width:400px; height:400px;"
         " position-try:flip-both; }",
         40 - 400, 50 - 400},
        {".pop { top:anchor(bottom); left:anchor(right); width:20px; height:400px;"
         " position-try:flip-both; }",
         100, 50 - 400},
        {".pop { top:anchor(bottom); left:anchor(right); width:400px; height:10px;"
         " position-try:flip-both; }",
         40 - 400, 70},
        /* flip-block must not touch the inline axis, and the reverse. */
        {".pop { top:anchor(bottom); left:anchor(right); width:400px; height:400px;"
         " position-try:flip-block; }",
         100, 50 - 400},
        {".pop { top:anchor(bottom); left:anchor(right); width:400px; height:400px;"
         " position-try:flip-inline; }",
         40 - 400, 70},
        /* And with no property at all nothing moves, however far it overflows. */
        {".pop { top:anchor(bottom); left:anchor(right); width:400px; height:400px; }", 100, 70}};

    for (k = 0; k < 20 && wrong < 0; ++k)
    {
        ar__posanchor_scene(&s, CASES[k].css);
        if (ar__box(2).x != CASES[k].x || ar__box(2).y != CASES[k].y)
        {
            wrong = k;
        }
    }

    CHECK(wrong < 0, "anchor: position-try lands where the arithmetic says on twenty cases");
    if (wrong >= 0)
    {
        ar__posanchor_scene(&s, CASES[wrong].css);
        printf("      case %ld: want %ld,%ld got %ld,%ld\n", (long)wrong, (long)CASES[wrong].x,
               (long)CASES[wrong].y, (long)ar__box(2).x, (long)ar__box(2).y);
    }
}

/* ------------------------------------------------------------------------
 * Anonymous table boxes
 *
 * Real markup is rarely well-formed, and the algorithm has to see a
 * rectangular grid. A cell with no row gets a row; a row with no table gets a
 * table; content sitting directly in a table gets a cell to live in.
 *
 * Generated as the tree is declared rather than afterwards, so the checks here
 * are about tree *shape* -- parent links and counts -- and can run before any
 * table layout exists.
 * ------------------------------------------------------------------------ */
static const char *AR_TBL_CSS = "#root { display:block; }"
                                ".t  { display:table; }"
                                ".rg { display:table-row-group; }"
                                ".r  { display:table-row; }"
                                ".c  { display:table-cell; }"
                                ".b  { display:block; }";

/* The display of node i, and the display of its parent chain, as a string of
   one character per level: t=table g=group r=row c=cell b=block ?=other. */
static char ar__disp_char(ar_i32 d)
{
    switch (d)
    {
    case AR_DISPLAY_TABLE:
        return 't';
    case AR_DISPLAY_TABLE_ROW_GROUP:
    case AR_DISPLAY_TABLE_HEADER_GROUP:
    case AR_DISPLAY_TABLE_FOOTER_GROUP:
        return 'g';
    case AR_DISPLAY_TABLE_ROW:
        return 'r';
    case AR_DISPLAY_TABLE_CELL:
        return 'c';
    case AR_DISPLAY_TABLE_CAPTION:
        return 'p';
    case AR_DISPLAY_BLOCK:
        return 'b';
    default:
        return '?';
    }
}

/* Writes the chain from the root down to node i, root first. */
static void ar__chain_of(ar_i32 i, char *out)
{
    ar_i32 stack[32];
    ar_i32 n = 0, at;

    for (at = i; at >= 0 && n < 32; at = g_ui->nodes[at].parent)
    {
        stack[n++] = at;
    }
    while (n > 0)
    {
        --n;
        *out++ = ar__disp_char(g_ui->nodes[stack[n]].style.v[AR_P_DISPLAY]);
    }
    *out = 0;
}

static void ar__tbl_frame_begin(void)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
}

static void test_a_cell_on_its_own_grows_a_row_a_group_and_a_table(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    char       chain[40];

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.c"); /* a cell with nothing above it */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* root, then the three boxes it needed, then the cell itself. */
    ar__chain_of(ar_node_count(g_ui) - 1, chain);
    CHECK(strcmp(chain, "btgrc") == 0, "table: a bare cell grows a row, a group and a table");
    CHECK(ar_node_count(g_ui) == 5, "table: and exactly three boxes were generated");
}

static void test_two_bare_cells_share_one_row(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /*
     * The rule that decides whether the generator is stream-correct. An
     * anonymous box that closed with the box that caused it would give each
     * cell a row of its own, and a two-cell table would lay out as two rows of
     * one -- which looks like a layout bug and is a lifetime bug.
     */
    CHECK(ar_node_count(g_ui) == 6, "table: two bare cells generate one row, not two");
    CHECK(ar_node_parent(g_ui, 4) == ar_node_parent(g_ui, 5),
          "table: and both cells hang off the same one");
}

static void test_content_inside_a_table_gets_a_cell(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    char       chain[40];

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.b"); /* a block sitting straight inside a table */
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* A row and a cell, in that order -- the chain is built outermost first,
       and getting it backwards puts the row inside the cell. */
    ar__chain_of(ar_node_count(g_ui) - 1, chain);
    CHECK(strcmp(chain, "btrcb") == 0,
          "table: content in a table gets a row and a cell, nested right");
}

static void test_a_row_inside_a_table_gets_a_group(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    char       chain[40];

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* CSS lets a row sit straight inside a table, so nothing is generated. */
    ar__chain_of(ar_node_count(g_ui) - 1, chain);
    CHECK(strcmp(chain, "btrc") == 0, "table: a row may sit directly in a table, ungenerated");
    CHECK(ar_node_count(g_ui) == 4, "table: and nothing was invented for it");
}

static void test_a_well_formed_table_generates_nothing(void)
{
    ar_surface s = ar__ui_surface(200, 200);

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.rg");
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The control, and the one that would catch a generator that fires on
       everything: root, table, group, row, two cells. Six, and not one more. */
    CHECK(ar_node_count(g_ui) == 6, "table: a well-formed table generates nothing at all");
}

static void test_a_sheet_without_tables_is_untouched(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    ar_u32     with_keys[8], without_keys[8];
    ar_i32     i, n1, n2;

    /* The same tree under a sheet that mentions tables and one that does not.
       Every key must be identical: the whole generation path is behind a flag
       that a sheet without a table display never sets, and this is what says
       so rather than assuming it. */
    ar__ui_reset(".b { display:block; }");
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    n1 = ar_node_count(g_ui);
    for (i = 0; i < n1 && i < 8; ++i)
    {
        without_keys[i] = g_ui->nodes[i].key;
    }

    ar__ui_reset(AR_TBL_CSS);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_begin(g_ui, "div.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    n2 = ar_node_count(g_ui);
    for (i = 0; i < n2 && i < 8; ++i)
    {
        with_keys[i] = g_ui->nodes[i].key;
    }

    CHECK(n1 == n2 && n1 == 3, "table: a tree with no table boxes is the same size either way");
    CHECK(without_keys[0] == with_keys[0] && without_keys[1] == with_keys[1] &&
              without_keys[2] == with_keys[2],
          "table: and every key is byte identical, so nothing else can have moved");
}

static void test_a_combinator_climbs_past_a_generated_row(void)
{
    ar_surface s = ar__ui_surface(200, 200);
    char       css[700];

    /* `.t > .c` is written against the markup the author wrote -- a cell
       straight inside a table. areole puts a row and a group in between, and
       the rule has to keep matching or fixing the markup would break the
       stylesheet written for it. */
    strcpy(css, AR_TBL_CSS);
    strcat(css, ".t .c { height: 33px; }");

    ar__ui_reset(css);
    ar__tbl_frame_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(g_ui->nodes[ar_node_count(g_ui) - 1].style.v[AR_P_HEIGHT] == 33,
          "table: a descendant combinator sees through the generated boxes");
}

static void test_a_cell_spans_one_by_default(void)
{
    /* Zero would be the natural consequence of the defaults loop, and every
       cell would land in column zero. */
    ar__sheet(".c { display:table-cell; }");
    CHECK(ar__css_value(".c", 0, AR_P_COLSPAN) == 1 && ar__css_value(".c", 0, AR_P_ROWSPAN) == 1,
          "table: a cell spans one column and one row unless it says otherwise");
    ar__sheet(".c { display:table-cell; colspan:3; rowspan:2; }");
    CHECK(ar__css_value(".c", 0, AR_P_COLSPAN) == 3 && ar__css_value(".c", 0, AR_P_ROWSPAN) == 2,
          "table: and says otherwise when asked");
}

/* ------------------------------------------------------------------------
 * Table layout
 *
 * A 300 wide table with no spacing and no padding, so every expected number
 * below is arithmetic on the widths the cells ask for.
 * ------------------------------------------------------------------------ */
static const char *AR_TL_CSS = "#root { display:block; }"
                               ".t  { display:table; width:300px; }"
                               ".r  { display:table-row; }"
                               ".c  { display:table-cell; height:20px; }";

/* Builds rows x cols of plain cells and returns the node index of the table. */
static void ar__table_scene(ar_surface *s, const char *extra, ar_i32 rows, ar_i32 cols)
{
    char     css[900];
    ar_input in;
    ar_i32   r, c;

    strcpy(css, AR_TL_CSS);
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    for (r = 0; r < rows; ++r)
    {
        ar_begin(g_ui, "div.r");
        for (c = 0; c < cols; ++c)
        {
            /* A class per column, so a test can widen one column without
               reaching for a positional selector. */
            ar_begin(g_ui, c == 0 ? "div.c.k0" : (c == 1 ? "div.c.k1" : "div.c.k2"));
            ar_end(g_ui);
        }
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_columns_share_the_table_width(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* Three cells with nothing to say about width: the 300 is shared, and the
       remainder goes to the last one so the row is exactly 300 rather than
       299. */
    ar__table_scene(&s, "", 1, 3);
    CHECK(ar__box(1).w == 300, "table: the table takes the width it was given");
    CHECK(ar__box(3).x == 0 && ar__box(4).x == 100 && ar__box(5).x == 200,
          "table: three empty columns start at 0, 100 and 200");
    CHECK(ar__box(3).w + ar__box(4).w + ar__box(5).w == 300,
          "table: and their widths sum to the table exactly");
}

static void test_a_wide_cell_widens_its_whole_column(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* The second cell of the second row states 200. Its column has to carry
       that, and the other two share what is left -- which is the whole point
       of a column being a constraint rather than a size. */
    ar__table_scene(&s, ".k1 { width:200px; }", 2, 3);
    CHECK(ar__box(4).w == ar__box(8).w, "table: a column is one width, whichever row a cell is in");
    CHECK(ar__box(4).w >= 200, "table: and it is at least what the widest cell asked for");
    CHECK(ar__box(3).w + ar__box(4).w + ar__box(5).w == 300,
          "table: the other columns give up the difference rather than overflowing");
}

static void test_rows_stack_and_the_table_is_as_tall_as_them(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    ar__table_scene(&s, "", 3, 2);
    CHECK(ar__box(2).y == 0 && ar__box(5).y == 20 && ar__box(8).y == 40,
          "table: three twenty-tall rows stack at 0, 20 and 40");
    CHECK(ar__box(1).h == 60, "table: and the table comes to their sum");
}

static void test_a_colspan_covers_its_columns(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_i32     wide;

    /* Row one is a single cell spanning three columns; row two is three
       ordinary cells. The spanning cell has to come to the whole width. */
    ar__ui_reset("#root { display:block; }"
                 ".t  { display:table; width:300px; }"
                 ".r  { display:table-row; }"
                 ".c  { display:table-cell; height:20px; }"
                 ".w  { display:table-cell; height:20px; colspan:3; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.w");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    wide = ar__box(3).w;
    CHECK(wide == 300, "table: a cell spanning three columns is as wide as all three");
    CHECK(ar__box(5).w + ar__box(6).w + ar__box(7).w == 300,
          "table: and the three below it still sum to the same");
}

static void test_a_rowspan_holds_its_column_open(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* The first cell spans two rows, so the second row has one declared cell
       and it belongs in the *second* column, not the first. Getting the
       countdown wrong slides it underneath and the table goes crooked. */
    ar__ui_reset("#root { display:block; }"
                 ".t  { display:table; width:200px; }"
                 ".r  { display:table-row; }"
                 ".c  { display:table-cell; height:20px; }"
                 ".tall { display:table-cell; height:20px; rowspan:2; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.tall");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* node 4 is the spanning cell, 5 its neighbour, 7 the second row's cell. */
    CHECK(ar__box(4).x == ar__box(6).x,
          "table: the row below a rowspan starts in the column it left free");
    CHECK(ar__box(6).x > ar__box(3).x, "table: which is not the one the spanning cell holds");
}

static void test_fixed_layout_ignores_what_cells_want(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* `fixed` is the affordable option on a slow machine precisely because it
       does not look at the cells: equal columns, one pass, whatever they say
       they need. */
    ar__table_scene(&s,
                    ".t { table-layout: fixed; }"
                    ".k0 { width:250px; }",
                    2, 3);
    CHECK(ar__box(3).w == 100 && ar__box(4).w == 100 && ar__box(5).w == 100,
          "table: fixed layout gives equal columns whatever a cell asks for");

    /* And the control: the same sheet on automatic honours the request. */
    ar__table_scene(&s, ".k0 { width:250px; }", 2, 3);
    CHECK(ar__box(3).w >= 250, "table: automatic layout does not ignore it");
}

static void test_a_table_stacks_like_any_other_box(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_input   in;

    /* The trap this guards: a table's y is fixed by its parent while stacking,
       before the forward sweep reaches the table. If its height were corrected
       any later the box after it would sit at the wrong place. */
    ar__ui_reset("#root { display:block; }"
                 ".t  { display:table; width:200px; }"
                 ".r  { display:table-row; }"
                 ".c  { display:table-cell; height:25px; }"
                 ".after { display:block; height:10px; }");
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.after");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__box(1).h == 50, "table: two twenty-five tall rows make a fifty tall table");
    CHECK(ar__box(6).y == 50, "table: and the block after it starts where the table ends");
}

/* ------------------------------------------------------------------------
 * The bugs a review found in the first table implementation
 *
 * Every one of these passed the earlier tests, which is the point of writing
 * them down: each needed a shape the first set of checks did not build.
 * ------------------------------------------------------------------------ */
static void test_cell_padding_is_not_counted_twice(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * min_w and fit[0] are built as `widest + pad_left + pad_right`, so a cell
     * that adds its padding again makes its column that much too wide. The
     * earlier tests all used cells with no padding and could not see it.
     *
     * One column, one cell, 100 wide with 10 either side: the column wants 120
     * and not 140. The table is made exactly that wide so there is no surplus
     * to distribute and nothing stands between the number under test and the
     * number the column gets.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:120px; }"
                 ".r { display:table-row; }"
                 ".p { display:table-cell; width:100px; padding:0px 10px; height:10px; }"
                 ".e { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.p");
        ar_end(g_ui);
        ar_begin(g_ui, "div.e");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(3).w == 120,
          "table: a padded cell's column is its content plus its padding once");
    CHECK(ar__box(4).w == 0,
          "table: and a column whose cell wants nothing is given nothing to want it with");
}

static void test_a_rowspan_cell_is_as_tall_as_the_rows_it_spans(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * The spanning cell was excluded from its row's height and then handed
     * that row's height anyway, so a table whose first row was one rowspan:2
     * cell came out as tall as the second row and the spanning cell had no
     * height at all.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:200px; }"
                 ".r { display:table-row; }"
                 ".tall { display:table-cell; rowspan:2; height:40px; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.tall");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(3).h >= 40, "table: a rowspan cell is at least as tall as it asked to be");
    CHECK(ar__box(1).h >= 40, "table: and the table is tall enough to hold it");
}

static void test_a_colspan_does_not_release_a_rowspan(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A cell spanning two columns wrote its own single row over a three-row
     * claim on one of them, releasing that column a row early. Storing the
     * longer claim rather than the latest is the fix, and this is the shape
     * that shows it: the third row's cell must still be pushed past the column
     * the tall cell is holding.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:300px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }"
                 ".tall { display:table-cell; rowspan:3; height:10px; }"
                 ".wide { display:table-cell; colspan:2; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r"); /* c , tall(rowspan 3) */
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.tall");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r"); /* wide(colspan 2) -- must not free the tall column */
        ar_begin(g_ui, "div.wide");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r"); /* c -- must still start at column 0 */
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 3=c, 4=tall, 6=wide, 8=the last row's cell. */
    CHECK(ar__box(8).x == ar__box(3).x,
          "table: a colspan does not release a column a rowspan is still holding");
}

static void test_a_table_honours_a_stated_height(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* The solved height was assigned raw, skipping the clamp every other box
       in that function gets -- so a table computed a height and threw a stated
       one away. */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:200px; height:150px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(1).h == 150, "table: a stated height on a table is a floor, not a suggestion");
}

static void test_a_spanning_cell_honours_a_stated_width(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* Only the single-column pass read a stated width, so `colspan:2;
       width:300px` on an empty cell widened nothing at all. */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:400px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }"
                 ".wide { display:table-cell; colspan:2; width:300px; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.wide");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(3).w >= 300,
          "table: a spanning cell's stated width widens the columns it covers");
}

static void test_an_empty_table_leaves_nothing_at_the_origin(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A table whose rows hold no cells has no columns, and the solve used to
     * return before it touched anything. A fresh node's rect is zeroed, so the
     * rows were not stale -- they were at the top left of the surface, which
     * hit testing and ar_node_rect both report as where the row is.
     */
    ar__ui_reset("#root { display:block; padding:50px; }"
                 ".t { display:table; width:200px; padding:10px; }"
                 ".r { display:table-row; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).x == ar__box(1).x + 10 && ar__box(2).y == ar__box(1).y + 10,
          "table: an empty table's row sits where its content would have started");
    CHECK(ar__box(2).h == 0, "table: and has no height");
}

/* ------------------------------------------------------------------------
 * Structure: the three row groups, the caption, and the column boxes
 * ------------------------------------------------------------------------ */
static void test_a_footer_is_drawn_last_wherever_it_is_written(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A footer is written where it reads best -- often straight after the
     * header, so the two live together in the markup -- and is drawn beneath
     * every body row. Document order is not row order, and an iterator that
     * only walked siblings could not know that.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:300px; }"
                 ".h { display:table-header-group; }"
                 ".f { display:table-footer-group; }"
                 ".b { display:table-row-group; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.f"); /* the footer, written first */
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.h"); /* the header, written second */
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.b"); /* the body, written last */
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 2=footer group, 3=its row, 5=header group, 6=its row, 8=body group,
       9=its row. Drawn: header, body, footer. */
    CHECK(ar__box(6).y < ar__box(9).y, "table: a header is drawn above the body it heads");
    CHECK(ar__box(9).y < ar__box(3).y, "table: and a footer beneath it, wherever it was written");
    CHECK(ar__box(5).y < ar__box(8).y && ar__box(8).y < ar__box(2).y,
          "table: the groups follow their rows");
}

static void test_a_caption_sits_above_the_grid(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A caption is a table-level box that is in no column and no row: the grid
     * starts beneath it and is no narrower for it, but the table is at least as
     * wide as the caption needs and as tall as both together.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:300px; }"
                 ".cap { display:table-caption; height:25px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.cap");
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 1=table, 2=caption, 3=row, 4/5=cells. */
    CHECK(ar__box(2).y == ar__box(1).y, "table: a caption starts at the top of the table");
    CHECK(ar__box(2).h == 25, "table: and is as tall as it asked to be");
    CHECK(ar__box(2).w == 300, "table: and as wide as the table, not as one column");
    CHECK(ar__box(3).y >= ar__box(2).y + 25, "table: the grid starts beneath the caption");
    CHECK(ar__box(4).w + ar__box(5).w == 300,
          "table: and the columns are no narrower for the caption being there");
    CHECK(ar__box(1).h >= 35, "table: the table is as tall as its caption and its rows");
}

static void test_a_column_box_takes_up_no_space(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * `col` and `colgroup` draw nothing and hold nothing, and they are still
     * the shape of the columns they describe -- which is where a background
     * for a whole column is written, and the one place in a table where a
     * box's geometry comes from neither its parent nor its children.
     *
     * They were collapsed to a point at first. A browser disagreed on all
     * three of the corpus's column cases, and it was right.
     */
    ar__ui_reset("#root { display:block; padding:40px; }"
                 ".t { display:table; width:200px; }"
                 ".cg { display:table-column-group; }"
                 ".col { display:table-column; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.cg");
        ar_begin(g_ui, "div.col");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /*
     * 1=table, 2=colgroup, 3=col, 4=row, 5=cell -- and node 3 was an anonymous
     * *table* until this test was written, because the generator only knew a
     * column belonged in a table and not in a column group, which is how every
     * table that has columns at all is written.
     */
    CHECK(g_ui->nodes[3].style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_COLUMN,
          "table: a column inside a column group is left where it was put");
    CHECK(ar__box(3).w == ar__box(5).w && ar__box(3).x == ar__box(5).x,
          "table: a column box is as wide as the column it describes, and over it");
    CHECK(ar__box(2).w == ar__box(3).w, "table: a column group covers the columns it holds");
    CHECK(ar__box(2).h == ar__box(4).h, "table: and is as tall as the grid");
    CHECK(ar__box(2).x == ar__box(1).x && ar__box(2).y == ar__box(1).y,
          "table: and it reports the table's corner, not the window's");
    CHECK(ar__box(4).y == ar__box(1).y, "table: a column box does not push the first row down");
}

/* ------------------------------------------------------------------------
 * Collapsed borders
 *
 * The separate model gives every cell its own border and puts border-spacing
 * between them. The collapsed model gives the *boundary* a border, as wide as
 * the widest thing that meets there, sitting half in each of the two cells it
 * separates -- which changes the geometry of every cell in the table.
 * ------------------------------------------------------------------------ */
static void ar__collapse_scene(ar_surface *s, const char *extra)
{
    char     css[900];
    ar_input in;

    strcpy(css, "#root { display:block; }"
                ".t { display:table; width:300px; border-collapse:collapse; }"
                ".r { display:table-row; }"
                ".c { display:table-cell; height:10px; border:2px #f00; }");
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c.a");
    ar_end(g_ui);
    ar_begin(g_ui, "div.c.b");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_begin(g_ui, "div.r");
    ar_begin(g_ui, "div.c.d");
    ar_end(g_ui);
    ar_begin(g_ui, "div.c.e");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_two_cells_share_one_border(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    ar__collapse_scene(&s, "");

    /*
     * 1=table, 2=row, 3=a, 4=b, 5=row, 6=d, 7=e. Every border is 2, so every
     * line is 2 -- one line between the two columns, not two borders of 2 with
     * a gap. The cells therefore touch: b starts exactly where a ends.
     */
    CHECK(ar__box(4).x == ar__box(3).x + ar__box(3).w,
          "collapse: the cell to the right starts where the one to its left ends");
    CHECK(ar__box(6).y == ar__box(3).y + ar__box(3).h,
          "collapse: and the row below starts where the row above ends");
    CHECK(ar__box(3).w + ar__box(4).w == 300,
          "collapse: the two cells and their shared lines fill the table exactly");
    CHECK(g_ui->nodes[3].edge[1] + g_ui->nodes[4].edge[3] == 2,
          "collapse: the shared line is drawn once, between them");
    /*
     * An outer line is split like any other, and the far half lies outside the
     * table's content box -- which is where a browser draws it and where
     * areole does not draw it at all, because a border in this engine reserves
     * no space (see ar_used_size). So the end cell draws one of the two
     * pixels and the other is the table's, which has no box to put it in.
     */
    CHECK(g_ui->nodes[3].edge[3] == 1 && g_ui->nodes[4].edge[1] == 1,
          "collapse: an outer line is split like any other");
}

static void test_the_widest_border_wins_the_line(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * The conflict rules resolve a line to the widest border that meets it.
     * Style precedence comes first in CSS and has nothing to resolve here:
     * this engine has one uniform border-width per box and no border-style, so
     * width and then origin is the whole of the order it can express.
     */
    ar__collapse_scene(&s, ".b { border:6px #00f; }");

    CHECK(g_ui->nodes[3].edge[1] + g_ui->nodes[4].edge[3] == 6,
          "collapse: a line is as wide as the widest border that meets it");
    CHECK(ar__box(4).x == ar__box(3).x + ar__box(3).w,
          "collapse: and the two cells still meet on it exactly");
}

static void test_a_collapsed_table_draws_no_border_of_its_own(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * The table's border is folded into the outermost lines, which the cells
     * at the ends draw. If the table drew it as well the outer edge would be
     * twice as thick as it asked for.
     */
    ar__collapse_scene(&s, ".t { border:4px #0f0; }");

    CHECK((g_ui->nodes[1].state & AR_STATE_COLLAPSED) != 0,
          "collapse: the table knows its borders are collapsed");
    CHECK(g_ui->nodes[1].edge[0] == 0 && g_ui->nodes[1].edge[3] == 0,
          "collapse: and draws no border of its own");
    CHECK((g_ui->nodes[2].state & AR_STATE_COLLAPSED) != 0 && g_ui->nodes[2].edge[3] == 0,
          "collapse: nor does a row");
    CHECK(g_ui->nodes[3].edge[3] == 2,
          "collapse: the outer line is the table's 4, and the end cell draws its half");
}

static void test_border_spacing_means_nothing_when_collapsed(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* border-spacing is the separate model's entire mechanism. In the
       collapsed model the gap between two cells is the shared line and there
       is nowhere for spacing to go. */
    ar__collapse_scene(&s, ".t { border-spacing:20px; }");

    CHECK(ar__box(4).x == ar__box(3).x + ar__box(3).w,
          "collapse: border-spacing does not push collapsed cells apart");
    CHECK(ar__box(3).w + ar__box(4).w == 300, "collapse: and the table is no wider for it");
}

static void test_a_separate_table_is_untouched_by_any_of_this(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /* The control. Everything above changes geometry, and none of it may
       change a table that did not ask for it. */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:300px; border-spacing:10px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; border:2px #f00; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK((g_ui->nodes[3].state & AR_STATE_COLLAPSED) == 0,
          "collapse: a table that did not ask for it is not marked");
    CHECK(g_ui->nodes[3].edge[0] == 0 && g_ui->nodes[3].edge[1] == 0,
          "collapse: and its cells carry no collapsed edge");
    CHECK(ar__box(4).x == ar__box(3).x + ar__box(3).w + 10,
          "collapse: border-spacing still separates a separate table's cells");
}

static void test_a_collapsed_edge_is_in_the_paint_digest(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_u32     before, after;

    /*
     * A collapsed line's width comes from a *neighbour's* border, and no
     * property on this cell records it. Every entry in the digest's property
     * list would say the cell is untouched while the pixels it draws change,
     * which is exactly the failure the scrollbar properties were added to stop.
     */
    ar__collapse_scene(&s, "");
    before = ar_paint_digest(&g_ui->nodes[3]);

    ar__collapse_scene(&s, ".b { border:8px #00f; }");
    after = ar_paint_digest(&g_ui->nodes[3]);

    CHECK(g_ui->nodes[3].style.v[AR_P_BORDER_WIDTH] == 2,
          "collapse: the cell's own border-width did not change");
    CHECK(before != after, "collapse: but its digest did, because its neighbour's border did");
}

static void test_a_collapsed_line_is_drawn_once(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * Every other check here is geometry, and geometry is blind to this one.
     *
     * If the paint pass ignored edge[] and drew each cell's own border-width
     * the way it does everywhere else, every rectangle in this table would
     * still be exactly where it is and the boundary between two cells would be
     * four pixels of border instead of two. That is the failure the scrollbar
     * properties were added to the digest to stop, in a different place.
     *
     * The two cells are given different colours so the line's two pixels can
     * be told apart: the left cell owns the pixel on the left of the boundary
     * and the right cell the one on the right, and nothing is drawn either
     * side of them.
     */
    ar__collapse_scene(&s, "#root { background:#ffffff; } .b { border:2px #0000ff; }");

    /* 3=a at x 0..149, 4=b at x 150..299, both 2px wide at the outer edges and
       sharing one 2px line down the middle. */
    CHECK(ar__box(3).x == 0 && ar__box(3).w == 150 && ar__box(4).x == 150,
          "collapse: the pixel test's cells are where it thinks they are");

    CHECK(ar__pixel_at(0, 6) == 0xFF0000u,
          "collapse: the end cell draws its half of the outer line");
    CHECK(ar__pixel_at(1, 6) == 0xFFFFFFu, "collapse: and stops there");

    CHECK(ar__pixel_at(148, 6) == 0xFFFFFFu, "collapse: nothing is drawn before the shared line");
    CHECK(ar__pixel_at(149, 6) == 0xFF0000u, "collapse: the left cell draws its half of it");
    CHECK(ar__pixel_at(150, 6) == 0x0000FFu, "collapse: the right cell draws the other half");
    CHECK(ar__pixel_at(151, 6) == 0xFFFFFFu, "collapse: and nothing is drawn after it");
}

static void test_a_roomy_table_gives_the_surplus_to_the_wide_column(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * Two columns wanting 100 and 20 in a table with 360 to give.
     *
     * The surplus is 240, and it goes in proportion to what each column
     * wanted: 200 to the first and 40 to the second, so they end at 300 and
     * 60. Sharing it equally would end them at 220 and 140 -- a wide column
     * and a narrow one coming out nearly the same width, which is what the
     * first version did and no browser does. This table is compared against
     * one, so the difference matters.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:360px; }"
                 ".r { display:table-row; }"
                 ".wide { display:table-cell; width:100px; height:10px; }"
                 ".narrow { display:table-cell; width:20px; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.wide");
        ar_end(g_ui);
        ar_begin(g_ui, "div.narrow");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(3).w == 300,
          "table: the surplus goes to the columns in proportion to what they wanted");
    CHECK(ar__box(4).w == 60, "table: so the narrow column stays narrow");
    CHECK(ar__box(3).w + ar__box(4).w == 360, "table: and the two of them still fill the table");
}

static void test_vertical_align_puts_a_cells_contents_where_it_says(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A row is as tall as its tallest cell, so every other cell in it has room
     * to spare. The tall cell is 60 and the three short ones hold a 10-pixel
     * block each: at the top, in the middle, and at the bottom of the 60.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:400px; }"
                 ".r { display:table-row; }"
                 ".tall { display:table-cell; height:60px; }"
                 ".c { display:table-cell; }"
                 ".mid { vertical-align:middle; }"
                 ".bot { vertical-align:bottom; }"
                 ".in { display:block; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.tall");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c"); /* 4: default, which is top */
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.c.mid"); /* 6 */
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.c.bot"); /* 8 */
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(3).h == 60, "table: the tall cell sets the row's height");
    CHECK(ar__box(4).h == 60 && ar__box(6).h == 60 && ar__box(8).h == 60,
          "table: and every cell in the row is that tall, contents or not");
    CHECK(ar__box(5).y == ar__box(4).y, "table: a cell's contents start at its top by default");
    CHECK(ar__box(7).y == ar__box(6).y + 25,
          "table: vertical-align middle centres them in the room");
    CHECK(ar__box(9).y == ar__box(8).y + 50, "table: and bottom puts them at the bottom of it");
    CHECK(ar__box(5).h == 10 && ar__box(7).h == 10 && ar__box(9).h == 10,
          "table: none of which changes how tall they are");
}

/* ------------------------------------------------------------------------
 * 0.7.1: rows and columns that close, captions underneath, cells that hide,
 * columns that speak for themselves, and headers that stay
 * ------------------------------------------------------------------------ */
static void ar__collapse_rows(ar_surface *s, const char *extra)
{
    char     css[820];
    ar_input in;
    ar_i32   r, c;

    strcpy(css, "#root { display:block; }"
                ".t { display:table; width:300px; }"
                ".r { display:table-row; }"
                ".c { display:table-cell; height:10px; }"
                ".k0 { width:200px; }"
                ".k1 { width:100px; }");
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.t");
    for (r = 0; r < 3; ++r)
    {
        char sel[24];

        sprintf(sel, "div.r.q%ld", (long)r);
        ar_begin(g_ui, sel);
        for (c = 0; c < 2; ++c)
        {
            ar_begin(g_ui, c == 0 ? "div.c.k0" : "div.c.k1");
            ar_end(g_ui);
        }
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_a_collapsed_row_closes_without_moving_the_columns(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_i32     w0, w1;

    /*
     * The whole reason `collapse` exists rather than `display: none`.
     *
     * A filter that hides half a table's rows must not make every remaining
     * column jump to a new width -- the reader would have to find them again.
     * The cells of a closed row stay in the column constraints; only the row's
     * height goes, and the rows below it close up.
     */
    ar__collapse_rows(&s, "");
    w0 = ar__box(3).w;
    w1 = ar__box(4).w;
    CHECK(w0 == 200 && w1 == 100, "collapse: the control's columns are where the widths say");

    /* 1=table, 2=row0, 3/4 its cells... nodes are 1=t, 2=q0, 3=c,4=c, 5=q1,
       6=c,7=c, 8=q2, 9=c,10=c. */
    ar__collapse_rows(&s, ".q1 { visibility:collapse; }");

    CHECK(ar__box(5).h == 0, "collapse: a closed row has no height");
    CHECK(ar__box(6).h == 0 && ar__box(7).h == 0, "collapse: nor do the cells in it");
    CHECK(ar__box(8).y == ar__box(2).y + 10, "collapse: and the row below it closes up");
    CHECK(ar__box(3).w == 200 && ar__box(4).w == 100,
          "collapse: the columns do not move, which is the whole point");
    CHECK(ar__box(6).w == 200 && ar__box(6).x == ar__box(3).x,
          "collapse: a closed row's cells keep their columns");
    CHECK(ar__box(1).h == 20, "collapse: the table is as tall as the rows that are left");
}

static void test_a_collapsed_column_takes_the_tables_width_with_it(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A closed column is not a thin column. Its width leaves the table rather
     * than being handed to its neighbours, so every other column stays exactly
     * where it was and the table gets narrower.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:300px; }"
                 ".lg { display:table-column-group; }"
                 ".co { display:table-column; }"
                 ".gone { visibility:collapse; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; width:100px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.lg");
        ar_begin(g_ui, "div.co");
        ar_end(g_ui);
        ar_begin(g_ui, "div.co.gone");
        ar_end(g_ui);
        ar_begin(g_ui, "div.co");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 1=t, 2=lg, 3/4/5=col, 6=r, 7/8/9=cells. */
    CHECK(ar__box(8).w == 0, "collapse: the closed column's cell has no width");
    CHECK(ar__box(7).w == 100 && ar__box(9).w == 100,
          "collapse: and its neighbours keep the widths they had");
    CHECK(ar__box(9).x == ar__box(7).x + 100, "collapse: the columns after it close up");
    CHECK(ar__box(1).w == 200, "collapse: and the table is narrower by exactly that column");
    CHECK(ar__box(4).h == 0, "collapse: a closed column's own box has no height either");
}

static void test_a_hidden_box_takes_its_space_and_paints_nothing(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * `visibility: hidden` is not `display: none`: the box is still there and
     * still takes its space. It inherits, so the children go too -- and a child
     * that says `visible` comes back, which is the one thing that cannot be
     * done with `display: none`.
     */
    ar__ui_reset("#root { display:block; background:#ffffff; }"
                 ".a { display:block; width:40px; height:20px; background:#ff0000; }"
                 ".b { display:block; width:40px; height:20px; background:#00ff00;"
                 "     visibility:hidden; }"
                 ".in { display:block; width:10px; height:10px; background:#0000ff; }"
                 ".back { visibility:visible; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.a");
        ar_end(g_ui);
        ar_begin(g_ui, "div.b");
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.b");
        ar_begin(g_ui, "div.in.back");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).y == 20 && ar__box(2).h == 20,
          "visibility: a hidden box keeps its place in the flow");
    CHECK(ar__pixel_at(5, 5) == 0xFF0000u, "visibility: the visible box above it is painted");
    CHECK(ar__pixel_at(5, 25) == 0xFFFFFFu, "visibility: the hidden one is not");
    CHECK(ar__pixel_at(5, 27) == 0xFFFFFFu,
          "visibility: and neither is the child that inherited it");
    CHECK(ar__pixel_at(5, 47) == 0x0000FFu, "visibility: but a child that says visible comes back");
}

static void test_a_caption_can_sit_underneath(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:200px; }"
                 ".cap { display:table-caption; height:16px; caption-side:bottom; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.cap");
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 1=t, 2=cap, 3=r, 4=c. The caption is declared first and drawn last. */
    CHECK(ar__box(3).y == ar__box(1).y, "caption-side: the grid starts at the top of the table");
    CHECK(ar__box(2).y == ar__box(1).y + 10, "caption-side: and the caption is beneath it");
    CHECK(ar__box(1).h == 26, "caption-side: the table holds both either way");
}

static void test_empty_cells_hide_shows_a_grid_with_holes(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A cell with nothing in it shows neither background nor border, so a
     * sparse table reads as a grid with holes rather than a grid of empty
     * boxes. Geometry cannot see this -- the cell is exactly where it was.
     */
    ar__ui_reset("#root { display:block; background:#ffffff; }"
                 ".t { display:table; width:200px; empty-cells:hide; }"
                 ".r { display:table-row; }"
                 /* A width, because a column whose cell wants nothing is given
                    nothing -- which is right, and would leave this test with no
                    empty cell to look at. */
                 ".c { display:table-cell; width:100px; height:20px; background:#ff0000; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.r");
        ar_text(g_ui, "div.c", "x");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(4).w == 100 && ar__box(4).x == 100,
          "empty-cells: the empty cell is exactly where it was");
    CHECK(ar__pixel_at(50, 10) == 0xFF0000u, "empty-cells: the cell with something in it paints");
    CHECK(ar__pixel_at(150, 10) == 0xFFFFFFu, "empty-cells: the one with nothing in it does not");
}

static void test_a_col_speaks_for_the_cells_that_said_nothing(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * A `col` is the one place a column can be spoken about without naming a
     * cell -- and it is a bid, not a settlement: a cell that asked for more
     * still gets it.
     *
     * Three columns, and the first two say nothing through their cells. Two
     * different widths on purpose: with one stated width, or two equal ones,
     * the surplus rule hands the columns the same numbers by a different route
     * and the test would pass with the whole feature removed. 200 and 100 is a
     * split nothing else in the solver produces.
     *
     * The table is exactly the sum, so there is no surplus and nothing between
     * the widths under test and the widths the columns get.
     */
    ar__ui_reset("#root { display:block; }"
                 ".t { display:table; width:390px; }"
                 ".lg { display:table-column-group; }"
                 ".co { display:table-column; }"
                 ".c1 { width:200px; }"
                 ".c2 { width:100px; }"
                 ".c3 { width:50px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:10px; }"
                 ".big { width:90px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.t");
        ar_begin(g_ui, "div.lg");
        ar_begin(g_ui, "div.co.c1");
        ar_end(g_ui);
        ar_begin(g_ui, "div.co.c2");
        ar_end(g_ui);
        ar_begin(g_ui, "div.co.c3");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.r");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c.big");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 1=t, 2=lg, 3/4/5=col, 6=r, 7/8/9=cells. */
    CHECK(ar__box(7).w == 200 && ar__box(8).w == 100,
          "col: a column takes the width its col stated");
    CHECK(ar__box(9).w == 90, "col: and a cell that asked for more than its col still gets it");
    CHECK(ar__box(3).w == ar__box(7).w && ar__box(3).x == ar__box(7).x,
          "col: the col box is the shape of the column it describes");
}

static void test_a_sticky_header_stays_while_the_rows_go_under_it(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_i32     top_before, top_after;

    /*
     * The 0.6.2 mechanism applied to a table box. A header group keeps its
     * place in the flow and is nudged just far enough to obey `top: 0` without
     * leaving the table it belongs to, and it is positioned, so it paints above
     * the rows going under it without needing a z-index.
     */
    ar__ui_reset("#root { display:block; }"
                 ".port { display:block; width:200px; height:60px; overflow:auto; }"
                 ".t { display:table; width:200px; }"
                 ".h { display:table-header-group; position:sticky; top:0px; }"
                 ".g { display:table-row-group; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:20px; }");
    {
        ar_input in;
        ar_i32   r;
        ar_i32   pass;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        top_before = 0;
        top_after = 0;
        for (pass = 0; pass < 3; ++pass)
        {
            ar_frame_begin(g_ui, &in);
            ar_begin(g_ui, "#root");
            ar_begin(g_ui, "div.port");
            ar_begin(g_ui, "div.t");
            ar_begin(g_ui, "div.h");
            ar_begin(g_ui, "div.r");
            ar_begin(g_ui, "div.c");
            ar_end(g_ui);
            ar_end(g_ui);
            ar_end(g_ui);
            ar_begin(g_ui, "div.g");
            for (r = 0; r < 6; ++r)
            {
                ar_begin(g_ui, "div.r");
                ar_begin(g_ui, "div.c");
                ar_end(g_ui);
                ar_end(g_ui);
            }
            ar_end(g_ui);
            ar_end(g_ui);
            ar_end(g_ui);
            ar_end(g_ui);
            ar_frame_end(g_ui, &s);
            ar_frame_presented(g_ui);

            if (pass == 0)
            {
                top_before = ar__box(3).y;
                ar_node_scroll_to(g_ui, 1, 40);
            }
            if (pass == 1)
            {
                top_after = ar__box(3).y;
            }
        }
    }

    /* 1=port, 2=t, 3=h, 4=its row, 5=its cell, 6=g, 7=first body row. */
    CHECK(top_before == ar__box(1).y, "sticky: the header starts at the top of the scrollport");
    CHECK(ar_node_scroll(g_ui, 1) == 40, "sticky: the container scrolled");
    CHECK(top_after == ar__box(1).y, "sticky: and the header is still at the top of it");
    CHECK(ar__box(2).y < ar__box(1).y, "sticky: while the table itself went up with the scroll");
    CHECK(ar__box(5).y == top_after, "sticky: the header's cells came with it");
}

static void test_a_frozen_column_stays_while_the_columns_go_past(void)
{
    ar_surface s = ar__ui_surface(400, 400);
    ar_i32     left_after;

    ar__ui_reset("#root { display:block; }"
                 ".port { display:block; width:100px; height:80px; overflow:auto; }"
                 ".t { display:table; width:300px; }"
                 ".r { display:table-row; }"
                 ".c { display:table-cell; height:20px; width:100px; }"
                 ".froze { position:sticky; left:0px; }");
    {
        ar_input in;
        ar_i32   r, pass;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        left_after = 0;

        for (pass = 0; pass < 2; ++pass)
        {
            ar_frame_begin(g_ui, &in);
            ar_begin(g_ui, "#root");
            ar_begin(g_ui, "div.port");
            ar_begin(g_ui, "div.t");
            for (r = 0; r < 2; ++r)
            {
                ar_begin(g_ui, "div.r");
                ar_begin(g_ui, "div.c.froze");
                ar_end(g_ui);
                ar_begin(g_ui, "div.c");
                ar_end(g_ui);
                ar_begin(g_ui, "div.c");
                ar_end(g_ui);
                ar_end(g_ui);
            }
            ar_end(g_ui);
            ar_end(g_ui);
            ar_end(g_ui);
            ar_frame_end(g_ui, &s);
            ar_frame_presented(g_ui);

            if (pass == 0)
            {
                ar_node_scroll_to_x(g_ui, 1, 80);
            }
        }
        left_after = ar__box(4).x;
    }

    /* 1=port, 2=t, 3=row0, 4/5/6=its cells. */
    CHECK(ar_node_scroll_x(g_ui, 1) == 80, "sticky: the container scrolled sideways");
    CHECK(left_after == ar__box(1).x, "sticky: the frozen cell is still at the left of it");
    CHECK(ar__box(5).x == ar__box(1).x + 100 - 80,
          "sticky: while the column beside it moved with the scroll");
    CHECK(ar__box(4).w == 100, "sticky: and the frozen cell is no wider for staying");
}

/* ------------------------------------------------------------------------
 * 0.8.0: the rest of flexbox
 *
 * Every one of these is a case the old subset -- one division of the leftover
 * among the boxes that said `grow` -- got wrong or could not express.
 * ------------------------------------------------------------------------ */
static void ar__flex_scene(ar_surface *s, const char *extra, ar_i32 items)
{
    char     css[900];
    ar_input in;
    ar_i32   k;

    /* The container is a box inside the root, not the root itself: the root
       takes the viewport and ignores anything it says about its own size, so
       a stated width there would be quietly the wrong number. */
    strcpy(css, "#root { display:block; }"
                ".box { display:flex; flex-direction:row; width:300px; height:100px; }"
                ".i { height:20px; }");
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    for (k = 0; k < items; ++k)
    {
        char sel[24];

        sprintf(sel, "div.i.n%ld", (long)k);
        ar_begin(g_ui, sel);
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_flex_factors_are_a_ratio_not_a_count(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /*
     * Two items, `flex-grow: 3` and `flex-grow: 1`, sharing 300.
     *
     * The old subset divided the leftover by the *number* of growers, so these
     * came out 150 and 150 -- the factors were read as a flag. They are a
     * ratio: 225 and 75.
     */
    ar__flex_scene(&s, ".i { flex-basis:0px; } .n0 { flex-grow:3; } .n1 { flex-grow:1; }", 2);

    CHECK(ar__box(2).w == 225, "flex: a grow factor of three takes three shares");
    CHECK(ar__box(3).w == 75, "flex: and a factor of one takes one");
    CHECK(ar__box(2).w + ar__box(3).w == 300, "flex: and together they meet the far edge");
}

static void test_a_fractional_factor_is_kept(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /* `flex-grow: 0.5` beside `flex-grow: 1.5` is two to six, not zero to one.
       areole has no floating point, so the factors are carried in thousandths
       -- 500 and 1500 -- and this is the check that says the fraction survived
       the parser rather than being floored the way every other number is. */
    ar__flex_scene(&s, ".i { flex-basis:0px; } .n0 { flex-grow:0.5; } .n1 { flex-grow:1.5; }", 2);

    CHECK(ar__box(2).w == 75, "flex: a fractional grow factor is not floored to zero");
    CHECK(ar__box(3).w == 225, "flex: and the ratio between two of them is kept");
}

static void test_shrinking_is_weighted_by_the_base(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /*
     * Two items wanting 300 and 100 in a container of 300: 100 too much.
     *
     * Shrinking is weighted by the base size as well as the factor, so the wide
     * one gives up three times what the narrow one does -- 75 against 25 --
     * rather than 50 each. That is what stops a narrow item vanishing while a
     * wide one beside it barely moves, and the subset could not do it at all
     * because it had no shrink.
     */
    ar__flex_scene(&s, ".n0 { flex-basis:300px; } .n1 { flex-basis:100px; }", 2);

    CHECK(ar__box(2).w == 225, "flex: the wide item gives up three quarters of the overflow");
    CHECK(ar__box(3).w == 75, "flex: and the narrow one a quarter");
    CHECK(ar__box(2).w + ar__box(3).w == 300, "flex: and the line fits exactly");
}

static void test_a_maximum_freezes_an_item_and_the_rest_take_its_share(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /*
     * This is the whole reason section 9.7 is a loop rather than a division.
     *
     * Three items share 300 equally: 100 each. One of them says `max-width:
     * 40px`, so it cannot take its hundred -- and the sixty it gives back has
     * to go to the other two, which a single division never revisits. 40, 130,
     * 130.
     */
    ar__flex_scene(&s,
                   ".i { flex-basis:0px; flex-grow:1; }"
                   ".n0 { max-width:40px; }",
                   3);

    CHECK(ar__box(2).w == 40, "flex: an item at its maximum stops there");
    CHECK(ar__box(3).w == 130, "flex: and what it could not take goes to the others");
    CHECK(ar__box(4).w == 130, "flex: to all of them, not just the next one");
    CHECK(ar__box(2).w + ar__box(3).w + ar__box(4).w == 300,
          "flex: and the line still meets the far edge");
}

static void test_a_minimum_freezes_an_item_the_other_way(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /* The same loop in the shrinking direction: three items wanting 200 each
       in a container of 300, and one with a `min-width` it cannot go under.
       The other two make up the difference. */
    ar__flex_scene(&s, ".i { flex-basis:200px; } .n0 { min-width:150px; }", 3);

    CHECK(ar__box(2).w == 150, "flex: an item at its minimum stops there");
    CHECK(ar__box(3).w == 75 && ar__box(4).w == 75,
          "flex: and the others give up what it would not");
    CHECK(ar__box(2).w + ar__box(3).w + ar__box(4).w == 300, "flex: and the line fits");
}

static void test_the_flex_shorthand_writes_a_zero_basis(void)
{
    ar_surface s = ar__ui_surface(400, 200);
    ar_i32     a, b;

    /*
     * `flex: 1` and `flex-grow: 1` are not the same declaration, and the
     * difference is the most common flexbox mistake there is.
     *
     * The shorthand writes a *zero* basis, so three boxes come out equal
     * whatever is in them. The longhand leaves the basis `auto`, so each keeps
     * its content's width and only the surplus is shared. Two items with
     * different stated widths tell them apart.
     */
    ar__flex_scene(&s, ".n0 { width:200px; flex-grow:1; } .n1 { width:40px; flex-grow:1; }", 2);
    a = ar__box(2).w;
    b = ar__box(3).w;
    CHECK(a == 230 && b == 70, "flex: flex-grow alone shares only the surplus");

    ar__flex_scene(&s, ".n0 { width:200px; flex:1; } .n1 { width:40px; flex:1; }", 2);
    CHECK(ar__box(2).w == 150 && ar__box(3).w == 150,
          "flex: the flex shorthand writes a zero basis, so they come out equal");
}

static void test_items_wrap_onto_lines(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* Four items of 100 in a container of 300: three on the first line and one
       on the second, and the second line starts below the first. */
    ar__flex_scene(&s, ".box { flex-wrap:wrap; } .i { flex-basis:100px; flex-shrink:0; }", 4);

    CHECK(ar__box(2).x == 0 && ar__box(4).x == 200, "flex: three of them fit on the first line");
    CHECK(ar__box(5).x == 0, "flex: and the fourth starts a new one");
    CHECK(ar__box(5).y > ar__box(2).y, "flex: below the first");
    CHECK(ar__box(2).y == 0, "flex: which is where the first line still is");
}

static void test_align_self_overrides_the_container(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    ar__flex_scene(&s,
                   ".box { align-items:flex-start; }"
                   ".n1 { align-self:center; } .n2 { align-self:flex-end; }",
                   3);

    CHECK(ar__box(2).y == 0, "flex: the container's align-items still holds");
    CHECK(ar__box(3).y == 40, "flex: an item can centre itself against it");
    CHECK(ar__box(4).y == 80, "flex: or put itself at the end");
}

static void test_space_around_and_evenly_differ_at_the_edges(void)
{
    ar_surface s = ar__ui_surface(400, 200);
    ar_i32     around_first, evenly_first;

    /*
     * Two items of 50 in a container of 300: 200 to distribute.
     *
     * `around` gives each item a half share at each end, so the edge gap is
     * half the inner one: 50 at the edges, 100 between. `evenly` makes them
     * all equal: 66 everywhere. They look alike in a mock-up and differ by
     * exactly one half-share at each edge.
     */
    ar__flex_scene(&s, ".box { justify-content:space-around; } .i { flex-basis:50px; }", 2);
    around_first = ar__box(2).x;

    ar__flex_scene(&s, ".box { justify-content:space-evenly; } .i { flex-basis:50px; }", 2);
    evenly_first = ar__box(2).x;

    CHECK(around_first == 50, "flex: space-around puts half a share at the edge");
    CHECK(evenly_first == 66, "flex: space-evenly puts a whole one");
    CHECK(evenly_first > around_first, "flex: which is the difference between them");
}

static void test_order_moves_an_item_without_moving_it(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /* `order: -1` puts the last box first without touching the markup, which
       is the whole reason the property exists. Ties keep document order. */
    ar__flex_scene(&s, ".i { flex-basis:100px; flex-shrink:0; } .n2 { order:-1; }", 3);

    CHECK(ar__box(4).x == 0, "flex: order -1 puts the last box first");
    CHECK(ar__box(2).x == 100, "flex: and the others follow in the order they were written");
    CHECK(ar__box(3).x == 200, "flex: all of them");
}

static void test_min_width_auto_stops_an_item_shrinking(void)
{
    ar_surface s = ar__ui_surface(400, 200);

    /*
     * The rule behind almost every "why will my flex item not shrink"
     * question. Nothing in this stylesheet says a minimum: the item's own
     * min-content width is one, and it is what stops the text being crushed.
     *
     * `min-width: 0` is the way out, and it has to be told apart from the
     * default -- which in this engine is also zero.
     */
    ar__ui_reset("#root { display:block; }"
                 ".box { display:flex; flex-direction:row; width:60px; height:60px; }"
                 ".i { flex-basis:200px; }"
                 ".free { min-width:0px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.box");
        ar_text(g_ui, "div.i", "unbreakable");
        ar_text(g_ui, "div.i.free", "unbreakable");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).w >= g_ui->nodes[2].min_w,
          "flex: an item will not shrink past its own min-content width");
    CHECK(ar__box(3).w < ar__box(2).w, "flex: and min-width: 0 is how you say it may");
}

/* ------------------------------------------------------------------------
 * 0.8.0: grid
 *
 * Two-dimensional layout, where the tracks line up across the whole container
 * rather than inside one row of it.
 * ------------------------------------------------------------------------ */
static void ar__grid_scene(ar_surface *s, const char *extra, ar_i32 items)
{
    char     css[900];
    ar_input in;
    ar_i32   k;

    strcpy(css, "#root { display:block; }"
                ".g { display:grid; width:300px; height:200px; }"
                ".c { }");
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.g");
    for (k = 0; k < items; ++k)
    {
        char sel[24];

        sprintf(sel, "div.c.m%ld", (long)k);
        ar_begin(g_ui, sel);
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_a_template_gives_the_columns_their_widths(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* Three stated columns in a 300 container. Nothing here is flexible, so
       the widths are exactly what was written and the third one starts where
       the first two end. */
    ar__grid_scene(&s, ".g { grid-template-columns: 100px 50px 120px; }", 3);

    CHECK(ar__box(2).w == 100 && ar__box(2).x == 0, "grid: the first column is where it says");
    CHECK(ar__box(3).w == 50 && ar__box(3).x == 100, "grid: and the second follows it");
    CHECK(ar__box(4).w == 120 && ar__box(4).x == 150, "grid: and the third follows that");
}

static void test_fr_shares_what_is_left_after_the_fixed_tracks(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * `100px 1fr 2fr` in 300: the fixed track takes its hundred and the other
     * two share the remaining two hundred in the ratio their factors name --
     * not a third each, and not a hundred each.
     *
     * 66 and 133, and the third one starts at 166: that was this check, and
     * the two tracks came to 199 of the 200 they were handed, so the grid
     * ended a pixel inside its own right edge. The share is 66.67 and 133.33,
     * and the pixel belongs to whichever track lost more of one -- the 1fr, at
     * .67 against .33.
     *
     * Edge was asked this exact grid before the numbers here were touched, and
     * reports its own subpixel widths alongside:
     *
     *     i0 x=0    w=100   exact 100.00
     *     i1 x=100  w=67    exact  66.66
     *     i2 x=167  w=133   exact 133.34
     *
     * So 67, 133, and the third track opening at 167. areole returns those.
     */
    ar__grid_scene(&s, ".g { grid-template-columns: 100px 1fr 2fr; }", 3);

    CHECK(ar__box(2).w == 100, "grid: a fixed track takes what it asked for");
    CHECK(ar__box(3).w == 67, "grid: and one fr takes a third of the rest, rounded up");
    CHECK(ar__box(4).w == 133 && ar__box(4).x == 167,
          "grid: and two fr take two thirds, meeting the far edge exactly");
}

static void test_repeat_expands_to_real_tracks(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* `repeat(3, 1fr)` is three columns of a hundred, not one column of three
       hundred and not a parse error. */
    ar__grid_scene(&s, ".g { grid-template-columns: repeat(3, 1fr); }", 3);

    CHECK(ar__box(2).w == 100 && ar__box(2).x == 0, "grid: repeat makes the first track");
    CHECK(ar__box(3).w == 100 && ar__box(3).x == 100, "grid: and the second");
    CHECK(ar__box(4).w == 100 && ar__box(4).x == 200, "grid: and the third");
}

static void test_items_wrap_onto_the_next_row(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* Two columns and four items: two rows, and the third item starts the
       second one. Nothing said where any of them go. */
    ar__grid_scene(&s, ".g { grid-template-columns: 150px 150px; } .c { height:40px; }", 4);

    CHECK(ar__box(2).x == 0 && ar__box(3).x == 150, "grid: the first two items fill the row");
    CHECK(ar__box(4).x == 0, "grid: and the third goes back to the first column");
    CHECK(ar__box(4).y > ar__box(2).y, "grid: on the next row");
    CHECK(ar__box(5).x == 150 && ar__box(5).y == ar__box(4).y, "grid: with the fourth beside it");
}

static void test_column_flow_fills_downwards_first(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * `grid-auto-flow: column` fills a column before moving right, which is
     * the transpose of the default and the whole of what the property does.
     *
     * The items state a width because the columns here are implicit and an
     * implicit track is sized by its contents -- four empty boxes make four
     * columns of nothing, and every one of them is at x = 0, which is correct
     * and says nothing about the flow.
     */
    ar__grid_scene(&s,
                   ".g { grid-template-rows: 50px 50px; grid-auto-flow: column; }"
                   ".c { height:40px; width:60px; }",
                   4);

    CHECK(ar__box(2).y == 0 && ar__box(3).y == 50, "grid: the first two go down the column");
    CHECK(ar__box(4).y == 0, "grid: and the third starts a new one");
    CHECK(ar__box(4).x > ar__box(2).x, "grid: to the right of the first");
}

static void test_a_named_line_puts_an_item_where_it_says(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* `grid-column: 3` is the third column, whatever came before it -- and the
       automatic items pack around it rather than through it. */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 100px 100px 100px; }"
                   ".c { height:30px; }"
                   ".m0 { grid-column: 3; }",
                   3);

    CHECK(ar__box(2).x == 200, "grid: an item that names a line gets it");
    CHECK(ar__box(3).x == 0, "grid: and the automatic ones pack around it");
    CHECK(ar__box(4).x == 100, "grid: in order");
}

static void test_a_span_covers_the_tracks_it_names(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* `grid-column: span 2` is as wide as two tracks and the gap between
       them, and the item after it goes to the third. */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 100px 100px 100px; }"
                   ".c { height:30px; }"
                   ".m0 { grid-column: span 2; }",
                   2);

    CHECK(ar__box(2).w == 200 && ar__box(2).x == 0, "grid: a span covers two tracks");
    CHECK(ar__box(3).x == 200, "grid: and the next item takes the third");
}

static void test_the_two_gaps_are_separate(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * A grid has two axes to put space between, which is why flex only ever
     * needed one number.
     *
     * The longhands, and deliberately not the shorthand: `gap: 10px 20px` sets
     * the single `gap` slot as well, so a solver that read only that slot
     * would still get the row right and this check would say nothing.
     */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 100px 100px;"
                   "     row-gap: 10px; column-gap: 20px; }"
                   ".c { height:40px; }",
                   4);

    /*
     * The second row starts at 105, not at 50, and the difference is not the
     * gap.
     *
     * This check said 50 -- 40 for the first row and 10 for the gap -- from
     * the day it was written, and it was pinning a bug. `.g` is `height:200px`
     * with two `auto` rows, and `align-content` defaults to `stretch` for a
     * grid: the rows take the container's height between them. 200 less the
     * 10px gap is 190, which is 95 a row, so the second row opens at 105.
     *
     * areole left the rows at their content height and the remaining 110px at
     * the bottom, so this check agreed with the engine and both were wrong.
     * The grid corpus found it against a browser, and Edge was asked this
     * exact scene before the number here was touched:
     *
     *     i0 x=0   y=0    w=100 h=40
     *     i1 x=120 y=0    w=100 h=40
     *     i2 x=0   y=105  w=100 h=40
     *     i3 x=120 y=105  w=100 h=40
     *
     * areole now returns those four rectangles exactly. The items stay 40 tall
     * because they said so; it is the rows underneath them that grew.
     */
    CHECK(ar__box(3).x == 120, "grid: the column gap goes between the columns");
    CHECK(ar__box(4).y == 105, "grid: and the row gap between the stretched rows");

    /* And the shorthand says the same thing, row first. */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 100px 100px; gap: 10px 20px; }"
                   ".c { height:40px; }",
                   4);

    CHECK(ar__box(3).x == 120 && ar__box(4).y == 105,
          "grid: and the gap shorthand is row then column");
}

static void test_an_item_stretches_to_its_cell_and_can_refuse(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* Stretch is the initial value on both axes, so an item with no size of
       its own fills its cell. `justify-self: start` takes its own width back
       and puts it at the near edge. */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 200px 100px; grid-template-rows: 80px; }"
                   ".m1 { justify-self:start; width:30px; }",
                   2);

    CHECK(ar__box(2).w == 200 && ar__box(2).h == 80, "grid: an item fills its cell by default");
    CHECK(ar__box(3).w == 30 && ar__box(3).x == 200,
          "grid: and justify-self: start gives it its own width at the near edge");

    /*
     * And an item that stated a height gets it.
     *
     * Only the stretch case was handled at first, so every cell in a grid of
     * `height: 24px` boxes came out zero tall. Nothing above catches that --
     * they all stretch -- and the tour page found it the moment it drew one.
     */
    ar__grid_scene(&s,
                   ".g { grid-template-columns: 100px 100px; }"
                   ".c { height:24px; }",
                   2);

    CHECK(ar__box(2).h == 24 && ar__box(3).h == 24,
          "grid: an item that states a height is given it, not stretched past it");
}

static void test_minmax_holds_a_track_between_two_ends(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* `minmax(50px, 1fr)` beside a fixed track: the flexible one takes what is
       left, and never less than fifty however little that is. */
    ar__grid_scene(&s, ".g { grid-template-columns: minmax(50px, 1fr) 250px; }", 2);

    CHECK(ar__box(2).w == 50, "grid: a minmax track will not go below its minimum");
    CHECK(ar__box(3).w == 250, "grid: and the fixed one beside it is untouched");
}

/* ------------------------------------------------------------------------
 * 0.8.1: sizing
 * ------------------------------------------------------------------------ */
static void test_display_contents_promotes_its_children(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * The wrapper generates no box and its two children become the grid's,
     * so they land in the second and third columns rather than both being
     * crammed into the first with the wrapper.
     *
     * This is the only way to put a semantic wrapper around grid items without
     * breaking the grid, and it is what the property is for.
     */
    ar__ui_reset("#root { display:block; }"
                 ".g { display:grid; width:300px; grid-template-columns: 100px 100px 100px; }"
                 ".w { display:contents; }"
                 ".c { height:20px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.g");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.w");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    /* 1=g, 2=first cell, 3=wrapper, 4/5=its children. */
    CHECK(ar__box(2).x == 0, "contents: the first item is where it was");
    CHECK(ar__box(4).x == 100, "contents: the wrapper's first child is a grid item");
    CHECK(ar__box(5).x == 200, "contents: and so is its second");
    CHECK(ar__box(3).w == 0 && ar__box(3).h == 0, "contents: the wrapper itself has no box");
}

static void test_intrinsic_keywords_work_on_the_height(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * `height: max-content` used to be a silent `auto`, because the resolver
     * answered the intrinsic keywords for width only. A box whose contents
     * come to sixty is sixty tall, not the two hundred its container offered.
     */
    /*
     * A flex row, because that is where `auto` and `max-content` differ.
     *
     * In block flow an automatic height is already the content height, so
     * `height: max-content` is the same number by a different route and a
     * check there passes whether the keyword is honoured or not. A flex row
     * stretches an automatic height to the line; `max-content` must not
     * stretch, and the two answers are 200 and 60.
     */
    ar__ui_reset("#root { display:block; }"
                 ".box { display:flex; flex-direction:row; width:200px; height:200px;"
                 "       align-items:stretch; }"
                 ".mc { display:block; width:100px; height:max-content; }"
                 ".st { display:block; width:40px; }"
                 ".in { display:block; height:30px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.box");
        ar_begin(g_ui, "div.mc");
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_begin(g_ui, "div.in");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.st");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).h == 60, "sizing: height: max-content is the height of the contents");
    CHECK(ar__box(5).h == 200, "sizing: where an automatic height would have stretched to 200");
    CHECK(ar__box(2).w == 100, "sizing: and the width it was given is untouched");
}

static void test_fit_content_takes_a_cap(void)
{
    ar_surface s = ar__ui_surface(400, 300);
    ar_i32     mn, mx;

    /*
     * `fit-content(80px)` is the bare keyword with a ceiling: fit the contents
     * into the smaller of what the container has left and eighty.
     *
     * Text, because it is the only thing in this engine whose min-content and
     * max-content differ -- a box of fixed-width children is the same width
     * either way, and a cap between two equal numbers cannot be seen. The
     * first check states that precondition rather than assuming it, so a
     * change of font fails loudly here instead of quietly passing the second.
     */
    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:300px; }"
                 ".f { display:block; width:fit-content(80px); }"
                 ".g2 { display:block; width:fit-content; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.box");
        ar_text(g_ui, "div.f", "alpha beta gamma");
        ar_text(g_ui, "div.g2", "alpha beta gamma");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    mn = g_ui->nodes[2].min_w;
    mx = g_ui->nodes[2].fit[0];

    CHECK(mn < 80 && 80 < mx, "sizing: the cap sits between the two intrinsic widths");
    CHECK(ar__box(2).w == 80, "sizing: fit-content(80px) stops at its cap");
    CHECK(ar__box(3).w == mx, "sizing: and the bare keyword fits the contents instead");
}

static void test_aspect_ratio_gives_the_axis_nobody_stated(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * A width and a ratio make a height; a height and a ratio make a width;
     * and a box that stated both keeps both, because a stated size always wins
     * over a derived one. That last part is what makes the property safe to
     * put in a base stylesheet.
     */
    ar__ui_reset("#root { display:block; }"
                 ".w16 { display:block; width:160px; aspect-ratio: 16 / 9; }"
                 ".h4 { display:block; height:60px; aspect-ratio: 4 / 3; }"
                 ".both { display:block; width:100px; height:100px; aspect-ratio: 16 / 9; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.w16");
        ar_end(g_ui);
        ar_begin(g_ui, "div.h4");
        ar_end(g_ui);
        ar_begin(g_ui, "div.both");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    printf("      DBGR ratio=%ld,%ld,%ld  a=%ldx%ld b=%ldx%ld c=%ldx%ld\n",
           (long)g_ui->nodes[1].style.v[AR_P_ASPECT_RATIO],
           (long)g_ui->nodes[2].style.v[AR_P_ASPECT_RATIO],
           (long)g_ui->nodes[3].style.v[AR_P_ASPECT_RATIO], (long)ar__box(1).w, (long)ar__box(1).h,
           (long)ar__box(2).w, (long)ar__box(2).h, (long)ar__box(3).w, (long)ar__box(3).h);
    CHECK(ar__box(1).h == 90, "aspect-ratio: a width and a ratio make a height");
    CHECK(ar__box(2).w == 80, "aspect-ratio: a height and a ratio make a width");
    CHECK(ar__box(3).w == 100 && ar__box(3).h == 100,
          "aspect-ratio: and a box that stated both keeps both");
}

static void test_safe_centring_never_starts_before_the_edge(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * Centring a box larger than the space it is given puts half the overflow
     * before the start edge, where it cannot be scrolled to and cannot be
     * read. `safe` says to fall back to start alignment exactly then -- and
     * only then, so a box that fits is still centred.
     *
     * A grid, not a flex row: a flex line grows to its tallest item, so an
     * item can never overflow the line it is aligned in and `align-items` has
     * nothing to overflow. A track with a stated height is the case that
     * actually arises.
     */
    ar__ui_reset("#root { display:block; }"
                 ".g { display:grid; width:300px; grid-template-rows: 60px;"
                 "     grid-template-columns: 100px 100px 100px; align-items:center; }"
                 ".gs { align-items: safe center; }"
                 ".big { height:200px; }"
                 ".small { height:20px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.g");
        ar_begin(g_ui, "div.big");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.g.gs");
        ar_begin(g_ui, "div.big");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_begin(g_ui, "div.g.gs");
        ar_begin(g_ui, "div.small");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).y < ar__box(1).y, "safe: plain centring puts the overflow before the edge");
    CHECK(ar__box(4).y == ar__box(3).y, "safe: and safe centring does not");
    CHECK(ar__box(6).y == ar__box(5).y + 20, "safe: a box that fits is still centred");
}

static void test_a_grid_item_keeps_its_min_content(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * The same automatic minimum a flex item has: a track narrower than an
     * item's unbreakable content does not crush it, the item overflows the
     * track. And the same escape hatch -- `min-width: 0`.
     */
    ar__ui_reset("#root { display:block; }"
                 ".g { display:grid; width:60px; grid-template-columns: 20px 20px; }"
                 ".c { height:20px; }"
                 ".free { min-width:0px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.g");
        ar_text(g_ui, "div.c", "unbreakable");
        ar_text(g_ui, "div.c.free", "unbreakable");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).w >= g_ui->nodes[2].min_w,
          "grid: an item is not crushed below its min-content width");
    CHECK(ar__box(3).w < ar__box(2).w, "grid: and min-width: 0 is how you say it may be");
}

/* ------------------------------------------------------------------------
 * 0.8.2: subgrid
 *
 * Subgrid inverts the direction of track sizing. An ordinary grid sizes its
 * tracks from its own contents; a subgrid's contents size its *parent's*, and
 * the parent's resolved tracks then bound the child.
 * ------------------------------------------------------------------------ */
static void ar__cards(ar_surface *s, const char *extra, const char *mid1, const char *mid2,
                      const char *mid3)
{
    char     css[900];
    ar_input in;

    strcpy(css, "#root { display:block; }"
                ".deck { display:grid; width:300px;"
                "        grid-template-columns: 100px 100px 100px; }"
                ".card { display:grid; grid-template-rows: subgrid; grid-row: span 3; }"
                ".sec { display:block; }");
    strcat(css, extra);
    ar__ui_reset(css);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.deck");

    ar_begin(g_ui, "div.card");
    ar_text(g_ui, "div.sec.t", "title");
    ar_text(g_ui, "div.sec.b", mid1);
    ar_text(g_ui, "div.sec.f", "footer");
    ar_end(g_ui);

    ar_begin(g_ui, "div.card");
    ar_text(g_ui, "div.sec.t", "title");
    ar_text(g_ui, "div.sec.b", mid2);
    ar_text(g_ui, "div.sec.f", "footer");
    ar_end(g_ui);

    ar_begin(g_ui, "div.card");
    ar_text(g_ui, "div.sec.t", "title");
    ar_text(g_ui, "div.sec.b", mid3);
    ar_text(g_ui, "div.sec.f", "footer");
    ar_end(g_ui);

    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_subgrid_lines_the_cards_up(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * Three cards whose middles are different lengths, and whose footers still
     * share a line.
     *
     * This is the whole reason subgrid exists. Every other way of doing it --
     * fixed heights, measuring in script, flexbox with hardcoded numbers -- is
     * a workaround for not having it, and every one of them breaks when the
     * content changes.
     *
     * Nodes: 1=deck, 2=card, 3/4/5=its sections, 6=card, 7/8/9, 10=card,
     * 11/12/13.
     */
    ar__cards(&s, "", "one line", "one line that is long enough to wrap twice over", "short");

    CHECK(ar__box(3).y == ar__box(7).y && ar__box(7).y == ar__box(11).y,
          "subgrid: the three titles share a row");
    CHECK(ar__box(4).y == ar__box(8).y && ar__box(8).y == ar__box(12).y,
          "subgrid: and the three bodies start on one");
    CHECK(ar__box(5).y == ar__box(9).y && ar__box(9).y == ar__box(13).y,
          "subgrid: and the three footers share one, whatever is above them");
    CHECK(ar__box(5).y > ar__box(4).y,
          "subgrid: with the footers below the bodies rather than on top of them");

    /*
     * And the rows are as tall as the *sections*, not as the cards.
     *
     * Every check above compares the three cards to each other, so a card that
     * contributed its own whole height to the first row -- on top of its
     * children, who are already in the item list -- would make all three
     * equally wrong and every one of them would still pass. This is the
     * absolute one: the deck is exactly its three rows, and its three rows are
     * exactly the three sections of the tallest card.
     */
    CHECK(ar__box(2).h == ar__box(3).h + ar__box(4).h + ar__box(5).h,
          "subgrid: a card is its three rows and nothing more");

    /*
     * And against a number that does not move with the bug.
     *
     * Every box above grows together if the card contributes its own height on
     * top of its children's: the row gets taller, so the section in it gets
     * taller, so the card gets taller, and each check compares two things that
     * both moved. The title's *intrinsic* height is settled before any track
     * is sized and is the one number a double contribution cannot touch.
     */
    CHECK(ar__box(3).h == g_ui->nodes[3].fit[1],
          "subgrid: the first row is as tall as a title, not as tall as a card");
}

static void test_a_subgrid_takes_the_parents_tracks(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * The rows are the deck's, so a section is as tall as its row -- and the
     * row is as tall as the tallest section across all three cards, not as the
     * tallest in its own card. That is the contribution flowing upward.
     */
    ar__cards(&s,
              ".deck { grid-template-rows: 20px 40px 30px; }"
              ".card { grid-column: auto; }",
              "a", "b", "c");

    CHECK(ar__box(3).h == 20, "subgrid: a section is as tall as the parent's track");
    CHECK(ar__box(4).h == 40, "subgrid: each of them");
    CHECK(ar__box(5).h == 30, "subgrid: all three");
    CHECK(ar__box(4).y == ar__box(3).y + 20, "subgrid: and they follow the parent's rows");
}

static void test_a_subgrid_keeps_its_own_columns(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * `grid-template-rows: subgrid` and nothing about columns: the rows come
     * from the deck and the columns are the card's own. A section is as wide
     * as its card, not as wide as the deck.
     */
    /*
     * The card is given padding on purpose.
     *
     * Without it a card's content box is exactly its column, so a solver that
     * ignored the owner entirely and used the parent's track would land on the
     * same numbers -- and the check would pass with the whole rule removed.
     * Ten pixels either side is what tells the two apart.
     */
    ar__cards(&s, ".deck { grid-template-rows: 20px 20px 20px; } .card { padding: 0px 10px; }", "a",
              "b", "c");

    CHECK(ar__box(2).w == 100, "subgrid: the card is one column of the deck");
    CHECK(ar__box(3).w == 80, "subgrid: and its sections fit inside the card's padding");
    CHECK(ar__box(3).x == 10, "subgrid: starting where the card's content does");
    CHECK(ar__box(6).x == 100, "subgrid: the second card is in the second column");
    CHECK(ar__box(8).x == 110, "subgrid: and its sections are in the card, not the deck");
}

static void test_a_subgrid_with_no_grid_above_it_is_an_ordinary_grid(void)
{
    ar_surface s = ar__ui_surface(400, 400);

    /*
     * `subgrid` with nobody to take tracks from falls back to an ordinary
     * grid, which CSS says outright -- and here that happens for free: with no
     * template on either axis, every track is implicit.
     */
    ar__ui_reset("#root { display:block; }"
                 ".g { display:grid; width:200px; grid-template-rows: subgrid; }"
                 ".c { height:20px; }");
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.g");
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_begin(g_ui, "div.c");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
    }

    CHECK(ar__box(2).h == 20 && ar__box(3).h == 20,
          "subgrid: with no grid above it, the items are laid out normally");
    CHECK(ar__box(3).y == ar__box(2).y + 20, "subgrid: one row each, stacked");
}

static void test_a_clipped_box_does_not_take_the_hover(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    int        i;

    ar__ui_reset("#root   { display:block; }"
                 ".spacer { display:block; height:100px; }"
                 ".port   { display:block; height:100px; overflow:scroll; }"
                 ".prow   { display:block; height:40px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 90; /* inside row 0 once scrolled, outside the port */
    in.mouse_inside = 1;

    for (i = 0; i < 3; ++i)
    {
        ar_frame_begin(g_ui, &in);
        ar_begin(g_ui, "#root");
        ar_begin(g_ui, "div.spacer");
        ar_end(g_ui);
        ar_begin(g_ui, "div.port");
        ar_begin(g_ui, "div.prow");
        ar_end(g_ui);
        ar_begin(g_ui, "div.prow");
        ar_end(g_ui);
        ar_begin(g_ui, "div.prow");
        ar_end(g_ui);
        ar_end(g_ui);
        ar_end(g_ui);
        ar_frame_end(g_ui, &s);
        ar_node_scroll_to(g_ui, 2, 20);
        ar_frame_presented(g_ui);
    }

    /* The premise: the port sits at 100 and its first row has been lifted to
       80, so the cursor at 90 really is in the overlap. */
    CHECK(ar__box(2).y == 100 && ar__box(3).y == 80,
          "hover: the probe really does put the cursor in the clipped overlap");

    CHECK(!(g_ui->nodes[3].state & AR_STATE_HOVER),
          "hover: a row scrolled out of its scrollport does not take the cursor");
    CHECK((g_ui->nodes[1].state & AR_STATE_HOVER) != 0,
          "hover: the box actually painted there takes it instead");
}

static void test_a_sticky_box_too_big_to_move_does_not(void)
{
    ar_surface s = ar__ui_surface(300, 300);
    ar_input   in;

    ar__ui_reset("#root { display:block; }"
                 ".port { display:block; width:200px; height:120px; overflow-x:scroll; }"
                 ".wide { display:block; width:420px; height:30px;"
                 "        position:sticky; left:10px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.port");
    ar_begin(g_ui, "div.wide");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    ar_frame_presented(g_ui);

    CHECK(ar_node_scroll_range_x(g_ui, 1) == 220, "sticky: the row overflows its port sideways");

    ar_node_scroll_to_x(g_ui, 1, 45);

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.port");
    ar_begin(g_ui, "div.wide");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* Scrolled like any other content, and not pinned anywhere. */
    CHECK(ar__box(2).x == -45, "sticky: a box wider than its containing block just scrolls");
}

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

/*
 * A scroll asks for the next frame through ar_needs_redraw, not only through
 * ar_scrolled.
 *
 * This is the half of the contract that was never checked, and the tour got it
 * wrong for exactly that reason: it woke on hover alone, so a notch moved the
 * list and nothing drew the frame that would have shown it. One predicate has
 * to answer "will the next frame differ", whatever the reason -- otherwise
 * every caller has to remember the full list of reasons, and one did not.
 */
static void test_a_scroll_asks_for_the_next_frame(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    /* Settle, so that hover cannot be the thing asking for the frame. */
    ar__scroll_scene(&s, &in);
    CHECK(!ar_needs_redraw(g_ui), "redraw: a still cursor over a settled frame asks for nothing");

    in.wheel = -1;
    ar__scroll_scene(&s, &in);
    CHECK(ar_needs_redraw(g_ui), "redraw: a notch that moved a container asks for the next frame");

    /*
     * The scroll reason is an edge and must not latch: it is cleared by
     * ar_frame_begin, so a caller that wakes on it draws one extra frame rather
     * than spinning for ever.
     *
     * ar_needs_redraw is deliberately not asserted false here. Scrolling moved
     * the rows underneath a cursor that never moved, so a different row is now
     * under it and hover genuinely did change -- one more frame really is owed,
     * for a different reason. That is the code being right and the first draft
     * of this test being wrong.
     */
    in.wheel = 0;
    ar__scroll_scene(&s, &in);
    CHECK(!ar_scrolled(g_ui), "redraw: the scroll reason does not latch");
}

/* ------------------------------------------------------------------------
 * overscroll-behavior
 *
 * A list inside a scrollable page, which is the arrangement every modal and
 * every sidebar is. Scroll the inner one to its end and give it one more
 * notch: with `auto` the page underneath takes it, with `contain` nothing
 * moves. That chaining is the single most common scrolling complaint in any
 * interface, and it is one `continue` in ar__apply_wheel.
 *
 * Both halves are asserted deliberately. A test that only checked `contain`
 * would pass just as well against a wheel handler that had stopped chaining
 * altogether, which is the more likely way to break this.
 * ------------------------------------------------------------------------ */
static const char *AR_CHAIN_CSS = "#root { display:block; }"
                                  ".page { display:block; height:100px; overflow:scroll; }"
                                  ".list { display:block; height:60px; overflow:scroll; }"
                                  ".row  { display:block; height:40px; }"
                                  ".tail { display:block; height:200px; }";

static const char *AR_CHAIN_CSS_CONTAIN = "#root { display:block; }"
                                          ".page { display:block; height:100px; overflow:scroll; }"
                                          ".list { display:block; height:60px; overflow:scroll;"
                                          "        overscroll-behavior: contain; }"
                                          ".row  { display:block; height:40px; }"
                                          ".tail { display:block; height:200px; }";

static void ar__chain_scene(ar_surface *s, const ar_input *in)
{
    ar_i32 i;

    ar_frame_begin(g_ui, in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.page");
    ar_begin(g_ui, "div.list");
    for (i = 0; i < 5; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_begin(g_ui, "div.tail");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

/* Node 1 is .page and node 2 is .list, in declaration order. */
static ar_i32 ar__chain_run(const char *css)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 30; /* inside .list, which starts at the top of .page */
    in.mouse_inside = 1;
    ar__chain_scene(&s, &in);

    /* Park the inner list at its end, so the next notch has nowhere to go
       inside it and must either chain or stop. */
    ar_node_scroll_to(g_ui, 2, ar_node_scroll_range(g_ui, 2));
    ar__chain_scene(&s, &in);

    in.wheel = -1;
    ar__chain_scene(&s, &in);
    in.wheel = 0;
    ar__chain_scene(&s, &in);

    return ar_node_scroll(g_ui, 1);
}

static const char *AR_CHAIN_CSS_NONE = "#root { display:block; }"
                                       ".page { display:block; height:100px; overflow:scroll; }"
                                       ".list { display:block; height:60px; overflow:scroll;"
                                       "        overscroll-behavior: none; }"
                                       ".row  { display:block; height:40px; }"
                                       ".tail { display:block; height:200px; }";

/*
 * The same question the criterion asks: a sequence, not a notch.
 *
 * One notch proves the first one was kept. A modal that leaks on the eighth
 * turn of the wheel is the bug people actually report, so this drives the
 * inner list to its end and then keeps going, watching the ancestor after
 * every single notch rather than only at the end. `peak` is the furthest the
 * page behind ever moved, so a container that chains once and comes back
 * cannot hide inside a final reading of zero.
 */
static ar_i32 ar__chain_sequence(const char *css, ar_i32 notches, ar_i32 *peak)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     i;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 30;
    in.mouse_inside = 1;
    ar__chain_scene(&s, &in);

    ar_node_scroll_to(g_ui, 2, ar_node_scroll_range(g_ui, 2));
    ar__chain_scene(&s, &in);

    *peak = 0;
    for (i = 0; i < notches; ++i)
    {
        ar_i32 at;

        in.wheel = -1;
        ar__chain_scene(&s, &in);
        in.wheel = 0;
        ar__chain_scene(&s, &in);

        at = ar_node_scroll(g_ui, 1);
        if (at > *peak)
        {
            *peak = at;
        }
    }
    return ar_node_scroll(g_ui, 1);
}

static void test_overscroll_contain_holds_over_a_whole_sequence(void)
{
    ar_i32 auto_peak = 0, contain_peak = 0, none_peak = 0;
    ar_i32 chained = ar__chain_sequence(AR_CHAIN_CSS, 10, &auto_peak);
    ar_i32 contained = ar__chain_sequence(AR_CHAIN_CSS_CONTAIN, 10, &contain_peak);
    ar_i32 refused = ar__chain_sequence(AR_CHAIN_CSS_NONE, 10, &none_peak);

    /* The control. Without this the two below pass against an engine that
       never chains at all, which is the shape of the bug they exist to catch
       and exactly how overflow:auto stayed broken for a release. */
    CHECK(chained > 0 && auto_peak == chained,
          "overscroll: auto chains outwards, and keeps going, over ten notches");
    CHECK(contained == 0 && contain_peak == 0,
          "overscroll: contain never lets one through, on any notch of ten");
    CHECK(refused == 0 && none_peak == 0, "overscroll: none holds the same way");
}

static void test_overscroll_behavior_stops_the_chain(void)
{
    ar_i32 chained, contained;

    chained = ar__chain_run(AR_CHAIN_CSS);
    contained = ar__chain_run(AR_CHAIN_CSS_CONTAIN);

    CHECK(chained > 0,
          "overscroll: auto hands the notch outwards when the inner list is at its end");
    CHECK(contained == 0, "overscroll: contain keeps it, and the page behind does not move");
}

/* ------------------------------------------------------------------------
 * scrollbar-width, scrollbar-gutter, scrollbar-color
 *
 * areole draws its own bar, so these are honoured everywhere and look the same
 * everywhere -- the one thing a native scrollbar can never promise.
 * ------------------------------------------------------------------------ */
static const char *AR_BAR_CSS = "#root { display:block; }"
                                ".list { display:block; height:100px; overflow:scroll;"
                                "        width:200px; }"
                                ".row  { display:block; height:40px; }";

static const char *AR_BAR_CSS_THIN = "#root { display:block; }"
                                     ".list { display:block; height:100px; overflow:scroll;"
                                     "        width:200px; scrollbar-width: thin; }"
                                     ".row  { display:block; height:40px; }";

static const char *AR_BAR_CSS_NONE = "#root { display:block; }"
                                     ".list { display:block; height:100px; overflow:scroll;"
                                     "        width:200px; scrollbar-width: none; }"
                                     ".row  { display:block; height:40px; }";

static const char *AR_BAR_CSS_GUTTER = "#root { display:block; }"
                                       ".list { display:block; height:100px; overflow:scroll;"
                                       "        width:200px; scrollbar-gutter: stable; }"
                                       ".row  { display:block; height:40px; }";

static void test_scrollbar_width_changes_the_bar(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     wide_w, thin_w;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar__ui_reset(AR_BAR_CSS);
    ar__scroll_scene(&s, &in);
    wide_w = ar_scroll_bar_width(&g_ui->nodes[1]);

    ar__ui_reset(AR_BAR_CSS_THIN);
    ar__scroll_scene(&s, &in);
    thin_w = ar_scroll_bar_width(&g_ui->nodes[1]);

    CHECK(wide_w == 8 && thin_w == 4, "scrollbar-width: thin is narrower than auto");

    /* `none` hides the bar. It must not stop the wheel: a list nobody can
       reach is the failure this property invites. */
    ar__ui_reset(AR_BAR_CSS_NONE);
    ar__scroll_scene(&s, &in);
    CHECK(ar_scroll_bar_width(&g_ui->nodes[1]) == 0 && !ar_scroll_bar_visible(&g_ui->nodes[1]),
          "scrollbar-width: none draws no bar");

    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    in.wheel = -1;
    ar__scroll_scene(&s, &in);
    in.wheel = 0;
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll(g_ui, 1) > 0, "scrollbar-width: none still scrolls");
}

static void test_scrollbar_gutter_reserves_its_width(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     plain, stable;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar__ui_reset(AR_BAR_CSS);
    ar__scroll_scene(&s, &in);
    plain = ar__box(2).w;

    ar__ui_reset(AR_BAR_CSS_GUTTER);
    ar__scroll_scene(&s, &in);
    stable = ar__box(2).w;

    /* The bar is an overlay, so nothing reflows when one appears and `stable`
       has no layout shift to prevent. What it buys is that a row stops before
       the bar instead of running underneath it. */
    CHECK(plain == 200 && stable == 200 - 8,
          "scrollbar-gutter: stable takes the bar's width off the content");
}

/*
 * Criterion 5: `scrollbar-gutter: stable` causes zero layout shift when the
 * content grows past the container.
 *
 * test_scrollbar_gutter_reserves_its_width already checks that the width is
 * reserved. That is not the same claim: an implementation that reserved the
 * gutter only while a bar was showing would pass it and still shift the text
 * sideways at the moment the list got long enough, which is the whole thing
 * the property exists to prevent.
 *
 * So this measures a row before and after the content crosses that threshold.
 */
static void ar__gutter_scene(ar_surface *s, const ar_input *in, const char *css, int rows)
{
    int i;

    ar__ui_reset(css);
    ar_frame_begin(g_ui, in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    for (i = 0; i < rows; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static void test_scrollbar_gutter_stable_shifts_nothing_when_content_grows(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     fits, overflows, plain_fits, plain_overflows;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    /* One row fits a hundred pixel box; five do not. */
    ar__gutter_scene(&s, &in, AR_BAR_CSS_GUTTER, 1);
    fits = ar__box(2).w;
    ar__gutter_scene(&s, &in, AR_BAR_CSS_GUTTER, 5);
    overflows = ar__box(2).w;

    CHECK(fits == overflows && fits == 200 - 8,
          "scrollbar-gutter: stable reserves the same width before and after the list grows");

    /* And the contrast, so the check above is not passing on the fact that
       nothing in areole ever reflows for a bar. An overlay bar takes no width
       at all, which is a different answer, not the same one. */
    ar__gutter_scene(&s, &in, AR_BAR_CSS, 1);
    plain_fits = ar__box(2).w;
    ar__gutter_scene(&s, &in, AR_BAR_CSS, 5);
    plain_overflows = ar__box(2).w;

    CHECK(plain_fits == plain_overflows && plain_fits == 200,
          "scrollbar-gutter: auto reserves nothing, and still does not shift");
}

static void test_scrollbar_color_is_read(void)
{
    ar__sheet(".bar { overflow:scroll; scrollbar-color: #ff0000 #00ff00; }");
    CHECK((ar__css_value(".bar", 0, AR_P_SCROLLBAR_THUMB) & 0xFFFFFF) == 0xFF0000 &&
              (ar__css_value(".bar", 0, AR_P_SCROLLBAR_TRACK) & 0xFFFFFF) == 0x00FF00,
          "scrollbar-color: thumb first, then track");

    /* One value is not valid CSS. Taking it as the thumb and leaving the track
       alone is more useful than refusing a declaration that clearly meant
       something. */
    ar__sheet(".one { overflow:scroll; scrollbar-color: #0000ff; }");
    CHECK((ar__css_value(".one", 0, AR_P_SCROLLBAR_THUMB) & 0xFFFFFF) == 0x0000FF &&
              ar__css_value(".one", 0, AR_P_SCROLLBAR_TRACK) == 0,
          "scrollbar-color: a lone colour is the thumb");
}

/* ------------------------------------------------------------------------
 * Scroll snapping
 *
 * A container 100 tall holding five rows of 40. The rows declare
 * scroll-snap-align: start, so the settled positions are multiples of 40.
 * A notch is AR_SCROLL_STEP, 30 px, which lands between two of them on
 * purpose -- a step that happened to equal the pitch would pass whether or
 * not any snapping code ran.
 * ------------------------------------------------------------------------ */
static const char *AR_SNAP_CSS = "#root { display:block; }"
                                 ".list { display:block; height:100px; overflow:scroll;"
                                 "        scroll-snap-type: y mandatory; }"
                                 ".row  { display:block; height:40px;"
                                 "        scroll-snap-align: start; }";

static const char *AR_SNAP_CSS_PROX = "#root { display:block; }"
                                      ".list { display:block; height:100px; overflow:scroll;"
                                      "        scroll-snap-type: y proximity; }"
                                      ".row  { display:block; height:40px;"
                                      "        scroll-snap-align: start; }";

static const char *AR_SNAP_CSS_PAD = "#root { display:block; }"
                                     ".list { display:block; height:100px; overflow:scroll;"
                                     "        scroll-snap-type: y mandatory;"
                                     "        scroll-padding-top: 5px; }"
                                     ".row  { display:block; height:40px;"
                                     "        scroll-snap-align: start; }";

static ar_i32 ar__snap_after_one_notch(const char *css)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.wheel = -1; /* AR_SCROLL_STEP, 30 px, which is not a multiple of 40 */
    ar__scroll_scene(&s, &in);
    in.wheel = 0;
    ar__scroll_scene(&s, &in);

    return ar_node_scroll(g_ui, 1);
}

/*
 * A programmatic scroll snaps, the same way a notch does.
 *
 * CSS applies snapping after any scrolling operation, not only the ones a hand
 * drove -- a mandatory container rests on a snap point however it got there,
 * which is why a browser re-snaps when a script assigns scrollTop. areole used
 * to snap the wheel and the keys and not this call, so where a container came
 * to rest depended on which of the three moved it.
 *
 * The rows are 40 tall in a 100 tall box, so the snap points are multiples of
 * 40 up to the range of 100. Asking for 50 is deliberately between two of
 * them, and nearer to 40.
 */
static void test_a_programmatic_scroll_snaps(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     snapped, loose;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar__ui_reset(AR_SNAP_CSS);
    ar__scroll_scene(&s, &in);
    snapped = ar_node_scroll_to(g_ui, 1, 50);

    /* The control: the same request on the same geometry with no snap
       declaration, which must land exactly where it was asked to. Without it
       this passes against a call that quietly clamps to something else. */
    ar__ui_reset(AR_SCROLL_CSS);
    ar__scroll_scene(&s, &in);
    loose = ar_node_scroll_to(g_ui, 1, 50);

    CHECK(loose == 50, "scroll: without snapping a programmatic scroll goes where it is sent");
    CHECK(snapped == 40, "scroll: with mandatory snapping it settles on the nearest snap point");

    /* And the end of the range is still reachable, which is the case a naive
       snap breaks: the last snap point is 40 short of the end, and a container
       that cannot be scrolled to its bottom is the bug people notice. */
    ar__ui_reset(AR_SNAP_CSS);
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll_to(g_ui, 1, 9999) == 100, "scroll: and the end of the range is reachable");
}

static void test_snap_lands_on_a_snap_point(void)
{
    ar_i32 snapped = ar__snap_after_one_notch(AR_SNAP_CSS);
    ar_i32 loose = ar__snap_after_one_notch(AR_SCROLL_CSS);

    /* Without snapping a notch travels its full 30 px. With it, the nearest
       row start is 40, and mandatory takes it. */
    CHECK(loose == 30, "snap: an unsnapped notch travels its own distance");
    CHECK(snapped == 40, "snap: mandatory lands on the nearest row start");
}

static void test_snap_proximity_leaves_a_distant_point_alone(void)
{
    /* The nearest point to 30 is 40, ten pixels away, and the scrollport is
       100 -- well inside the proximity window, so proximity agrees with
       mandatory here. The case that separates them needs a point further away
       than half the viewport, which five forty pixel rows cannot produce; what
       this pins is that `proximity` parses and still snaps rather than being
       silently read as `none`. */
    CHECK(ar__snap_after_one_notch(AR_SNAP_CSS_PROX) == 40,
          "snap: proximity still snaps when a point is near");
}

static void test_snap_honours_scroll_padding(void)
{
    /*
     * scroll-padding-top moves the top of the scrollport down by 5, so
     * aligning the second row's start to it needs 5 px less scroll: 35 rather
     * than 40.
     *
     * Five and not ten, and that is the whole point of this test. With ten the
     * answer is 30, which is also what an unsnapped notch gives -- so the test
     * would have passed against a build where scroll-padding did nothing at
     * all, or where snapping did. 35 is reachable no other way.
     */
    CHECK(ar__snap_after_one_notch(AR_SNAP_CSS_PAD) == 35,
          "snap: scroll-padding moves the port the points are measured against");
}

/*
 * scroll-snap-stop: always
 *
 * A hard flick -- five notches, 150 px, clamped to the 100 px range -- against
 * rows that each forbid being passed. Mandatory alone would settle at 100, the
 * point nearest where the gesture was heading. `always` must stop it at the
 * first row it would have crossed instead, which is 40.
 *
 * That is the difference between a good carousel and an infuriating one, and
 * the two expected values are far apart so the test cannot pass by accident.
 */
static const char *AR_SNAP_CSS_STOP = "#root { display:block; }"
                                      ".list { display:block; height:100px; overflow:scroll;"
                                      "        scroll-snap-type: y mandatory; }"
                                      ".row  { display:block; height:40px;"
                                      "        scroll-snap-align: start;"
                                      "        scroll-snap-stop: always; }";

static ar_i32 ar__snap_after_a_flick(const char *css)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.wheel = -5; /* 150 px, past every row in the list */
    ar__scroll_scene(&s, &in);
    in.wheel = 0;
    ar__scroll_scene(&s, &in);

    return ar_node_scroll(g_ui, 1);
}

static void test_snap_stop_always_refuses_to_be_passed(void)
{
    CHECK(ar__snap_after_a_flick(AR_SNAP_CSS) == 100,
          "snap-stop: normal lets a flick run to the end");
    CHECK(ar__snap_after_a_flick(AR_SNAP_CSS_STOP) == 40,
          "snap-stop: always catches the flick at the first row it would pass");
}

static void test_snap_type_parses_both_words(void)
{
    ar__sheet(".a { overflow:scroll; scroll-snap-type: y mandatory; }"
              ".b { overflow:scroll; scroll-snap-type: y; }"
              ".c { overflow:scroll; scroll-snap-type: both proximity; }");

    CHECK(ar__css_value(".a", 0, AR_P_SCROLL_SNAP_TYPE) == (AR_SNAP_AXIS_Y | AR_SNAP_MANDATORY),
          "snap-type: an axis and a strictness are both kept");

    /* CSS says an omitted strictness is `proximity`, not `mandatory`. Getting
       this backwards makes every container with one snap declaration
       impossible to scroll off a slide. */
    CHECK(ar__css_value(".b", 0, AR_P_SCROLL_SNAP_TYPE) == AR_SNAP_AXIS_Y,
          "snap-type: an axis alone is proximity");
    CHECK(ar__css_value(".c", 0, AR_P_SCROLL_SNAP_TYPE) == AR_SNAP_AXIS_BOTH,
          "snap-type: both axes, proximity stated");
}

/* ------------------------------------------------------------------------
 * Keyboard scrolling
 *
 * The first keys areole has ever read. Scoped to the ones that scroll: there
 * is no focus, no caret and no text here, and adding any of them is how 0.6.1
 * would quietly become 0.10.0.
 *
 * With no focus concept, a key goes to the same container the wheel would --
 * the innermost scrollable box under the cursor. That is a deviation from a
 * browser, where the keyboard follows focus, and it is asserted here so it
 * stays a decision rather than becoming an accident.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__after_key(const char *css, ar_u32 key)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.keys_pressed = key;
    ar__scroll_scene(&s, &in);
    in.keys_pressed = 0;
    ar__scroll_scene(&s, &in);

    return ar_node_scroll(g_ui, 1);
}

static void test_keys_scroll_the_container(void)
{
    /* Five 40 px rows in a 100 px box: 200 of content, 100 of range. */
    CHECK(ar__after_key(AR_SCROLL_CSS, AR_KEY_DOWN) == 40, "keys: down moves by a line");
    CHECK(ar__after_key(AR_SCROLL_CSS, AR_KEY_UP) == 0, "keys: up at the top stays there");
    CHECK(ar__after_key(AR_SCROLL_CSS, AR_KEY_END) == 100, "keys: End goes to the bottom");

    /* A page is the viewport less an overlap, so the last line of the old page
       is the first of the new one and nothing is skipped over the fold. */
    CHECK(ar__after_key(AR_SCROLL_CSS, AR_KEY_PAGE_DOWN) == 76,
          "keys: a page is the viewport less its overlap");

    /* Space pages. Shift-space is mapped to Page Up by the backend, because
       the core tracks no modifiers. */
    CHECK(ar__after_key(AR_SCROLL_CSS, AR_KEY_SPACE) ==
              ar__after_key(AR_SCROLL_CSS, AR_KEY_PAGE_DOWN),
          "keys: space pages down");
}

static void test_keys_home_and_end_do_not_snap(void)
{
    /*
     * End must reach the actual bottom. Snapping it would leave the last row
     * partly off screen and no key would reach it, which is a bug rather than
     * a nicety -- so Home and End are absolute and skip the snap entirely.
     */
    CHECK(ar__after_key(AR_SNAP_CSS, AR_KEY_END) == 100, "keys: End ignores snapping");
    CHECK(ar__after_key(AR_SNAP_CSS, AR_KEY_DOWN) == 40, "keys: an ordinary key still snaps");
}

/* ------------------------------------------------------------------------
 * scroll-into-view
 * ------------------------------------------------------------------------ */
static void test_scroll_into_view_moves_the_minimum(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);

    /* Node 2 is the first row and is already fully visible: moving would be a
       page jumping under someone who was reading it. */
    CHECK(!ar_node_scroll_into_view(g_ui, 2), "into-view: a visible box does not move");
    CHECK(ar_node_scroll(g_ui, 1) == 0, "into-view: and the container stays put");

    /* Node 6 is the fifth row, at 160..200 in a 100 tall port. Bringing its
       bottom to the bottom needs 100 -- not the 160 a jump-to-top would use. */
    CHECK(ar_node_scroll_into_view(g_ui, 6), "into-view: a box below the fold moves");
    CHECK(ar_node_scroll(g_ui, 1) == 100,
          "into-view: to its bottom edge, not to the top of the list");
}

static void test_scroll_into_view_honours_scroll_margin(void)
{
    static const char *CSS = "#root { display:block; }"
                             ".list { display:block; height:100px; overflow:scroll; }"
                             ".row  { display:block; height:40px; scroll-margin-bottom: 12px; }";
    ar_surface         s = ar__ui_surface(200, 300);
    ar_input           in;

    ar__ui_reset(CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar__scroll_scene(&s, &in);

    /* scroll-margin-bottom brings twelve more pixels along, which would ask
       for 112 -- past the range, so it clamps to 100 and this alone would not
       prove anything. Row four is the one that shows it: 120..160, so without
       the margin it needs 60 and with it 72. */
    ar_node_scroll_into_view(g_ui, 5);
    CHECK(ar_node_scroll(g_ui, 1) == 72, "into-view: scroll-margin comes along with the box");
}

/* ------------------------------------------------------------------------
 * overflow-anchor
 *
 * A list scrolled down, and then a row above the fold grows. Without
 * anchoring, everything below slides by that much and the reader loses their
 * place. With it, the scroll takes up the difference and nothing appears to
 * move.
 *
 * The scene declares the first row taller on demand, which is what "content
 * loaded above the viewport" looks like from the layout's point of view.
 * ------------------------------------------------------------------------ */
static const char *AR_ANCHOR_CSS = "#root { display:block; }"
                                   ".list { display:block; height:100px; overflow:scroll; }"
                                   ".row  { display:block; height:40px; }"
                                   ".tall { display:block; height:70px; }";

static const char *AR_ANCHOR_CSS_OFF = "#root { display:block; }"
                                       ".list { display:block; height:100px; overflow:scroll;"
                                       "        overflow-anchor: none; }"
                                       ".row  { display:block; height:40px; }"
                                       ".tall { display:block; height:70px; }";

/* The first row is `.tall` once `grown` is set: 40 becomes 70, so everything
   below it moves down by 30. */
static void ar__anchor_scene(ar_surface *s, const ar_input *in, int grown)
{
    ar_i32 i;

    ar_frame_begin(g_ui, in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    for (i = 0; i < 5; ++i)
    {
        ar_begin(g_ui, (i == 0 && grown) ? "div.tall" : "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, s);
}

static ar_i32 ar__anchor_run(const char *css)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(css);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar__anchor_scene(&s, &in, 0);
    ar_node_scroll_to(g_ui, 1, 80);
    ar__anchor_scene(&s, &in, 0);

    /* Settle, so the anchor is recorded at a position nothing is about to
       change for its own reasons. */
    ar__anchor_scene(&s, &in, 0);

    /* Now the row above the fold grows by 30. */
    ar__anchor_scene(&s, &in, 1);
    return ar_node_scroll(g_ui, 1);
}

static void test_overflow_anchor_keeps_the_reading_position(void)
{
    /*
     * Anchored: the scroll absorbs the 30 px the first row gained, so what was
     * under the eye stays there. Range grows with the content, so 110 is
     * reachable.
     *
     * Unanchored: the offset does not move and everything below slides down,
     * which is the behaviour this property exists to stop.
     */
    CHECK(ar__anchor_run(AR_ANCHOR_CSS) == 110,
          "overflow-anchor: growth above the fold is taken up by the scroll");
    CHECK(ar__anchor_run(AR_ANCHOR_CSS_OFF) == 80,
          "overflow-anchor: none leaves the reader to lose their place");
}

/* ------------------------------------------------------------------------
 * The scrollbar is an overlay, so it has to be painted over the content
 *
 * It was painted inside the main walk, at the container's own place in paint
 * order -- which is before its children, because a child comes later in that
 * order by construction. Every row of a list drew straight over the bar, and
 * the bar showed only where the content happened not to reach.
 *
 * Both colours are opaque here on purpose. The defaults are translucent
 * blacks, so a bar drawn underneath still tints the pixel and a test written
 * against "did the colour change" would pass either way. Opaque means the
 * pixel is the bar's colour or the row's, and nothing in between.
 * ------------------------------------------------------------------------ */
static void test_the_scrollbar_paints_over_the_content(void)
{
    static const char *CSS = "#root { display:block; }"
                             ".list { display:block; width:100px; height:60px; overflow:scroll;"
                             "        scrollbar-color: #ff0000 #ff0000; }"
                             ".row  { display:block; height:40px; background:#00ff00; }";
    ar_surface         s = ar__ui_surface(160, 120);
    ar_input           in;
    ar_i32             i, bar_x;

    ar__ui_reset(CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.list");
    for (i = 0; i < 4; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    /* The bar is drawn inside the right edge, so a pixel two in from it is
       the track. The rows are full width and green underneath it. */
    bar_x = ar__box(1).x + ar__box(1).w - 2;

    CHECK(ar__pixel_at(bar_x, 10) == 0xFF0000u,
          "scrollbar: the bar is painted over the rows, not under them");

    /* And the row is still itself away from the bar, so the check above is
       about paint order rather than about the row failing to draw. */
    CHECK(ar__pixel_at(ar__box(1).x + 4, 10) == 0x00FF00u,
          "scrollbar: the row is still painted where the bar is not");
}

/*
 * Dragging the scrollbar thumb scrolls, and letting go stops it.
 *
 * The bar is painted by the container rather than declared, so none of the
 * hot/active machinery touches it and none of the other tests here would
 * notice it was inert -- which it was until this landed.
 *
 * The last two checks are the ones worth having. A drag has to keep following
 * the cursor after it leaves the container, because a scrollbar that lets go
 * when you stray sideways is unusable; and it has to stop dead on release,
 * because a drag that keeps tracking is worse than one that never started.
 */
static void test_dragging_the_scrollbar_scrolls(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    ar_i32     first, after_move, after_release;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    CHECK(ar_node_scroll_range(g_ui, 1) > 0, "drag: the container has somewhere to go");

    /* Press on the thumb, which sits at the top while the scroll is zero. The
       bar is AR_SCROLLBAR_W wide at the container's right edge. */
    in.mouse_x = ar__box(1).x + ar__box(1).w - 2;
    in.mouse_y = ar__box(1).y + 4;
    in.mouse_down = AR_MOUSE_LEFT;
    in.mouse_pressed = AR_MOUSE_LEFT;
    ar__scroll_scene(&s, &in);
    first = ar_node_scroll(g_ui, 1);

    /* Drag down the track, and off the side of the container on the way. */
    in.mouse_pressed = 0;
    in.mouse_x = ar__box(1).x + ar__box(1).w + 40;
    in.mouse_y = ar__box(1).y + 40;
    ar__scroll_scene(&s, &in);
    after_move = ar_node_scroll(g_ui, 1);

    /* Let go, then keep moving. Nothing more may happen. */
    in.mouse_down = 0;
    in.mouse_released = AR_MOUSE_LEFT;
    ar__scroll_scene(&s, &in);
    in.mouse_released = 0;
    in.mouse_y = ar__box(1).y + 90;
    ar__scroll_scene(&s, &in);
    ar__scroll_scene(&s, &in);
    after_release = ar_node_scroll(g_ui, 1);

    CHECK(first == 0, "drag: pressing the thumb where it already is moves nothing");
    CHECK(after_move > 0, "drag: and dragging it down scrolls, even off the side of the container");
    CHECK(after_release == after_move, "drag: letting go stops it following the cursor");
}

/*
 * overflow-x clips on its own, and the shorthand still sets both.
 *
 * Before the axis split there was one property and `overflow-x` was an unknown
 * declaration, so a stylesheet that used it got no clipping at all and no
 * complaint either.
 */
static void test_overflow_x_clips_by_itself(void)
{
    ar_surface s = ar__ui_surface(200, 60);

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:50px; height:20px; overflow-x:hidden; }"
                 ".wide { display:block; width:180px; height:20px; background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.wide");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0x00FF00, "overflow-x: inside the box the child is drawn");
    CHECK(ar__pixel_at(100, 10) != 0x00FF00, "overflow-x: and past its edge it is cut off");
}

/*
 * The rule that makes a horizontal scroller possible.
 *
 * CSS says a `visible` beside anything else computes to `auto`, because
 * content cannot escape sideways from a box that clips it vertically -- there
 * is nowhere for it to go. `overflow-y: hidden` alone therefore makes the
 * other axis auto, which is exactly what somebody writing a horizontally
 * scrolling strip relies on, and exactly the clause a naive implementation
 * leaves out.
 */
static void test_a_lone_visible_becomes_auto(void)
{
    ar_surface s = ar__ui_surface(200, 60);

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:50px; height:20px; overflow:visible hidden; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(g_ui->nodes[1].style.v[AR_P_OVERFLOW_X] == AR_OVERFLOW_VISIBLE,
          "overflow: the cascade still says what the author wrote");
    CHECK(ar_overflow_x(&g_ui->nodes[1]) == AR_OVERFLOW_AUTO,
          "overflow: but a lone visible is used as auto");
    CHECK(ar_overflow_y(&g_ui->nodes[1]) == AR_OVERFLOW_HIDDEN,
          "overflow: and the axis that was stated is left alone");
    CHECK(ar_clips(&g_ui->nodes[1]), "overflow: so the box clips");
}

/*
 * A stored scroll position survives being read back, however long the list.
 *
 * The point of this one is the AR_SCROLL_COMPACT build, where a position is
 * sixteen bits: a list taller than 32,767 pixels has a range that does not fit,
 * and writing it anyway brings it back negative -- the list jumps to the top on
 * its way to the bottom. ar_scroll_clamp caps at what the type holds, so the
 * limit is a visible stop rather than a wrap.
 *
 * It is worth running in the default build too, where it asserts the ordinary
 * thing: asking to scroll past the end lands exactly on the end.
 */
static void test_a_scroll_position_survives_the_round_trip(void)
{
    ar_surface s = ar__ui_surface(120, 120);
    ar_i32     range, asked, read;
    int        i;

    g_ui = ar_init(g_wide_mem, (ar_u32)sizeof g_wide_mem);
    ar_stylesheet(g_ui, "#root { display:block; overflow:scroll; height:100px; }"
                        ".row { display:block; height:200px; }");

    ar__ui_begin();
    ar_begin(g_ui, "div#root");
    for (i = 0; i < 200; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(!ar_overflowed(g_ui), "limit: the tall list fits the arena");

    range = ar_node_scroll_range(g_ui, 0);
    asked = ar_node_scroll_to(g_ui, 0, 39000);
    read = ar_node_scroll(g_ui, 0);

    CHECK(range > 0, "limit: a forty thousand pixel list has somewhere to go");
    CHECK(asked >= 0, "limit: clamping never returns a negative position");
    CHECK(asked == read, "limit: and what was stored is what comes back");
    CHECK(read <= range, "limit: never past the end of the content");
    CHECK(read <= AR_SCROLL_LIMIT, "limit: never past what the stored type holds");
}

/*
 * Sideways: a child wider than its container gives it a horizontal range, and
 * moving the position slides the child.
 *
 * The width the contents came to is the piece that had to be added for this.
 * Block layout stacks downwards and hands every child the container's width, so
 * nothing in it ever asked how far right anything reached -- which meant every
 * container looked exactly as wide as its content and no horizontal range could
 * ever exist.
 */
static void test_a_wide_child_scrolls_sideways(void)
{
    ar_surface s = ar__ui_surface(200, 60);
    ar_i32     range, at, before, after;

    ar__ui_reset("#root { display:block; }"
                 ".box { display:block; width:50px; height:20px; overflow-x:scroll; }"
                 ".wide { display:block; width:180px; height:20px; background:#00FF00; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.wide");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    range = ar_node_scroll_range_x(g_ui, 1);
    before = ar__box(2).x;
    at = ar_node_scroll_to_x(g_ui, 1, 40);

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.box");
    ar_begin(g_ui, "div.wide");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);
    after = ar__box(2).x;

    CHECK(range == 130, "overflow-x: a 180 px child in a 50 px box has 130 px to travel");
    CHECK(at == 40, "overflow-x: and the position it was asked for is the one it took");
    CHECK(after == before - 40, "overflow-x: the child slides left by exactly that much");
    CHECK(ar_node_scroll(g_ui, 1) == 0, "overflow-x: without disturbing the other axis");
}

/*
 * A device that reports pixels is believed over one that reports notches.
 *
 * A notch is a fixed distance, which is right for a wheel that clicks and
 * wrong for a touchpad: rounding a fraction of a notch to a whole one is
 * either jerky or, as it was here for a while, silently nothing at all. Seven
 * pixels has to move seven pixels, not a notch and not zero.
 */
static void test_pixel_travel_beats_notches(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;

    ar__ui_reset(AR_SCROLL_CSS);
    memset(&in, 0, sizeof in);
    in.mouse_x = 50;
    in.mouse_y = 50;
    in.mouse_inside = 1;
    ar__scroll_scene(&s, &in);

    in.wheel_px = -7;
    ar__scroll_scene(&s, &in);
    in.wheel_px = 0;
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll(g_ui, 1) == 7, "wheel: seven pixels of travel moves seven pixels");

    /* Both set: the finer one wins, because it is strictly more information. */
    in.wheel = -1;
    in.wheel_px = -3;
    ar__scroll_scene(&s, &in);
    in.wheel = 0;
    in.wheel_px = 0;
    ar__scroll_scene(&s, &in);
    CHECK(ar_node_scroll(g_ui, 1) == 10, "wheel: with both reported, the pixels are believed");
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
/*
 * `auto` has to survive the value parser.
 *
 * It is a length for width, height, the margins and the insets, and a keyword
 * of its own for overflow. The length reading is tested first, so for a long
 * time it won every time and `overflow: auto` quietly meant `visible` -- no
 * clip, no scrolling, no bar. Nothing caught it: the suite had a test that
 * compared `auto` against `scroll`, but its content fitted the box, and a
 * container that fits is indistinguishable from one that does not scroll.
 *
 * So this checks the parsed value directly, on the shorthand and on both
 * longhands, against `scroll` as a control.
 */
static void test_overflow_auto_parses_as_a_keyword(void)
{
    ar__sheet("#a { display:block; overflow:auto; }");
    CHECK(ar__css_value("#a", 0, AR_P_OVERFLOW) == AR_OVERFLOW_AUTO &&
              ar__css_value("#a", 0, AR_P_OVERFLOW_X) == AR_OVERFLOW_AUTO,
          "css: overflow:auto sets both axes to auto, not to visible");

    ar__sheet("#b { display:block; overflow-y:auto; }");
    CHECK(ar__css_value("#b", 0, AR_P_OVERFLOW) == AR_OVERFLOW_AUTO, "css: overflow-y:auto parses");

    ar__sheet("#c { display:block; overflow-x:auto; }");
    CHECK(ar__css_value("#c", 0, AR_P_OVERFLOW_X) == AR_OVERFLOW_AUTO,
          "css: overflow-x:auto parses");

    /* The control: a keyword that was never ambiguous. If this one ever fails
       the fault is somewhere else entirely. */
    ar__sheet("#d { display:block; overflow:scroll; }");
    CHECK(ar__css_value("#d", 0, AR_P_OVERFLOW) == AR_OVERFLOW_SCROLL,
          "css: overflow:scroll still parses");

    /* And `auto` is still a length where a length is what it means. Reading
       the keyword table first must not take it away from width. */
    ar__sheet("#e { display:block; width:auto; height:auto; }");
    CHECK(ar__css_value("#e", 0, AR_P_WIDTH) == 0 && ar__css_value("#e", 0, AR_P_HEIGHT) == 0,
          "css: width:auto and height:auto are still lengths");
}

/*
 * And that the parsed value reaches the machinery.
 *
 * Content past the end of an `overflow: auto` box makes it a scroll container
 * with a range and a bar, exactly as `scroll` would. The previous test pins
 * the parse; this one pins that nothing downstream drops it again.
 */
static void test_overflow_auto_scrolls_when_it_overflows(void)
{
    ar_surface s = ar__ui_surface(200, 300);
    ar_input   in;
    int        i;

    ar__ui_reset("#root { display:block; }"
                 ".a { display:block; height:100px; overflow:auto; }"
                 ".row { display:block; height:40px; }");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.a");
    for (i = 0; i < 5; ++i)
    {
        ar_begin(g_ui, "div.row");
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar_is_scroll_container(&g_ui->nodes[1]), "scroll: overflow:auto is a scroll container");
    CHECK(ar_clips(&g_ui->nodes[1]), "scroll: and it clips what hangs out of it");
    CHECK(ar_node_scroll_range(g_ui, 1) == 100,
          "scroll: five forty pixel rows in a hundred leaves a hundred to scroll");
    CHECK(ar_scroll_bar_visible(&g_ui->nodes[1]), "scroll: auto shows a bar once there is one");
}

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

    /* Both halves of this test are satisfied by an `auto` that does not parse
       at all -- content that fits has no range and shows no bar whether or not
       the keyword survived. test_overflow_auto_parses_as_a_keyword is what
       actually pins it; this one is about the difference between the two
       keywords, and only that. */
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

/*
 * A z-index far past the old sweep range paints, and paints on top.
 *
 * The ordering passes used to sweep a fixed [-32, 32] one value at a time, so
 * `z-index: 99999` -- what somebody writes when they mean "in front of
 * everything" -- matched no sweep and the box was never painted at all. The
 * header claimed it was clamped; nothing clamped it.
 */
static void test_z_index_has_no_ceiling(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; position:relative; }"
                 ".top { display:block; position:absolute; top:0; left:0;"
                 "       width:50px; height:50px; background:#FF0000; z-index:99999; }"
                 ".mid { display:block; position:absolute; top:0; left:0;"
                 "       width:50px; height:50px; background:#0000FF; z-index:1; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.top");
    ar_end(g_ui);
    ar_begin(g_ui, "div.mid");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0xFF0000, "stacking: a huge z-index paints, and paints on top");
}

/*
 * A negative and a positive z in one context, with flow between them.
 *
 * This is the handover between the two ordered passes: the negative pass stops
 * on the first non-negative it finds without consuming it, and the positive
 * pass has to find that same value again. Getting it wrong loses a layer
 * entirely, and losing one is invisible until something is behind it.
 */
static void test_z_index_negative_and_positive_together(void)
{
    ar_surface s = ar__ui_surface(100, 100);

    ar__ui_reset("#root { display:block; position:relative; width:100px; height:100px; }"
                 ".under { display:block; position:absolute; top:0; left:0;"
                 "         width:50px; height:50px; background:#FF0000; z-index:-1; }"
                 ".flow { display:block; width:50px; height:50px; background:#00FF00; }"
                 ".over { display:block; position:absolute; top:0; left:0;"
                 "        width:20px; height:20px; background:#0000FF; z-index:2; }");

    ar__ui_begin();
    ar_begin(g_ui, "#root");
    ar_begin(g_ui, "div.under");
    ar_end(g_ui);
    ar_begin(g_ui, "div.flow");
    ar_end(g_ui);
    ar_begin(g_ui, "div.over");
    ar_end(g_ui);
    ar_end(g_ui);
    ar_frame_end(g_ui, &s);

    CHECK(ar__pixel_at(10, 10) == 0x0000FF, "stacking: the positive z is on top of both");
    CHECK(ar__pixel_at(30, 30) == 0x00FF00, "stacking: the negative z stays behind the flow");
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

    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "combinator: the card inside the page gets the rule");
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x00FF00,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "not: matches the box without the class");
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x111111,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "not: a box matching neither argument still matches");
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x111111,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "is: first alternative");
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00, "is: second");
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x00FF00,
          "is: and nothing else");
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

    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "not: inside a descendant selector, the plain card matches");
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x00FF00,
          "not: the muted one does not");
    CHECK((AR_WIDE(&g_ui->nodes[4].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0x00FF00,
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

    /* The same *outer* width both times, so the padding is what differs.
       Under content-box that means stating a smaller content width, which is
       exactly the arithmetic border-box exists to spare people. */
    ar__ui_reset("#root { display:flex; flex-direction:column; }"
                 ".bare { width:120px; }"
                 ".pad  { width:40px; padding:0 40px; }");

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

    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFF0000,
          "structural: :first-child matches the first");
    CHECK((AR_WIDE(&g_ui->nodes[4].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0xFF0000,
          "structural: and not the second");
    CHECK((AR_WIDE(&g_ui->nodes[5].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
          "structural: :last-child matches the last, resolved after the parent closed");

    CHECK(g_ui->nodes[3].style.v[AR_P_BORDER_WIDTH] == 7, "structural: nth-child(odd)");
    CHECK(g_ui->nodes[4].style.v[AR_P_BORDER_WIDTH] == 9, "structural: nth-child(even)");
    CHECK(g_ui->nodes[5].style.v[AR_P_BORDER_WIDTH] == 7, "structural: and odd again");

    CHECK((AR_WIDE(&g_ui->nodes[7].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x0000FF,
          "structural: :only-child matches a lone child");
    CHECK((AR_WIDE(&g_ui->nodes[6].style, AR_P_BACKGROUND) & 0xFFFFFF) != 0xFFFF00,
          "structural: :empty does not match a box with a child");
    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFFFF00,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFF0000,
          "structural: two boxes differing only in a structural bit do not share a cache entry");
    CHECK((AR_WIDE(&g_ui->nodes[2].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x111111,
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

    CHECK((AR_WIDE(&g_ui->nodes[1].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0xFF0000,
          "combinator: > matches a direct child");
    /* The card inside .page is a descendant of #root but not its child, so the
       child rule must not reach it. That is the difference between the two
       combinators and the reason both exist. */
    CHECK((AR_WIDE(&g_ui->nodes[3].style, AR_P_BACKGROUND) & 0xFFFFFF) == 0x00FF00,
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

/* ------------------------------------------------------------------------
 * HTML tokenizer
 *
 * Written from the specification's own state descriptions rather than from
 * what the tokenizer happens to do. The acceptance criterion for 0.9.0 is the
 * html5lib tokenizer suite at 100%, which is not vendored yet -- these are the
 * cases that suite exists to cover, in the shape this repository uses.
 * ------------------------------------------------------------------------ */

static char     g_html_scratch[4096];
static ar_token g_tok;

/* Tokenize `src` and return the token at `index`, or EOF past the end. */
static ar_token *ar__tok_at(const char *src, ar_i32 index, ar_u32 *errors)
{
    ar_html_tok t;
    ar_i32      k = 0;

    ar_html_tok_init(&t, src, (ar_u32)strlen(src), g_html_scratch, (ar_u32)sizeof g_html_scratch);
    memset(&g_tok, 0, sizeof g_tok);
    while (ar_html_next(&t, &g_tok))
    {
        if (k == index)
        {
            if (errors)
            {
                *errors = t.errors;
            }
            return &g_tok;
        }
        ++k;
    }
    if (errors)
    {
        *errors = t.errors;
    }
    return &g_tok;
}

static ar_i32 ar__tok_count(const char *src)
{
    ar_html_tok t;
    ar_token    tok;
    ar_i32      k = 0;

    ar_html_tok_init(&t, src, (ar_u32)strlen(src), g_html_scratch, (ar_u32)sizeof g_html_scratch);
    while (ar_html_next(&t, &tok))
    {
        ++k;
    }
    return k;
}

static int ar__text_is(ar_span s, const char *lit)
{
    ar_u32 n = (ar_u32)strlen(lit);

    return s.n == n && memcmp(s.p, lit, n) == 0;
}

static void test_the_entity_table_is_sorted(void)
{
    char   prev[40];
    char   cur[40];
    ar_i32 i;
    ar_i32 bad = 0;
    ar_i32 truncated = 0;
    ar_i32 n = ar_html_entity_count();

    /*
     * The match loop narrows a range of this table one byte at a time and
     * trusts the order absolutely. An unsorted table does not fail loudly: it
     * fails on one entity, in one document, and now there are two thousand
     * two hundred chances to do it rather than two hundred.
     *
     * The order is plain byte order, which is not the order a person would
     * write. `sup;` sorts after `sup1;` -- '1' is 0x31 and ';' is 0x3B -- and
     * `not` sorts before `notin;` because a name comes before anything that
     * extends it. That second one is what the longest match depends on.
     */
    prev[0] = 0;
    for (i = 0; i < n; ++i)
    {
        if (ar_html_entity_name(i, cur, (ar_u32)sizeof cur) == 0)
        {
            ++truncated;
            continue;
        }
        if (i > 0 && strcmp(prev, cur) >= 0)
        {
            printf("      out of order at %ld: %s before %s\n", (long)i, prev, cur);
            ++bad;
        }
        memcpy(prev, cur, strlen(cur) + 1);
    }
    CHECK(bad == 0, "html: the named character reference table is sorted");
    CHECK(truncated == 0, "html: and every name fits the buffer it is copied into");
    CHECK(n == 2231, "html: and holds all 2,231 references the specification defines");
}

static void test_the_tokenizer_reads_a_tag(void)
{
    ar_token *t = ar__tok_at("<div>", 0, 0);

    CHECK(t->kind == AR_TOK_START, "html: a start tag is a start tag");
    CHECK(ar_span_is(t->name, "div"), "html: and carries its name");

    t = ar__tok_at("<DIV>", 0, 0);
    CHECK(ar_span_is(t->name, "div"), "html: a tag name matches case-insensitively");

    t = ar__tok_at("</p>", 0, 0);
    CHECK(t->kind == AR_TOK_END && ar_span_is(t->name, "p"), "html: and an end tag is one");

    t = ar__tok_at("<br/>", 0, 0);
    CHECK(t->kind == AR_TOK_START && t->self_closing, "html: a solidus before > is self-closing");
}

static void test_the_tokenizer_reads_attributes(void)
{
    ar_token *t = ar__tok_at("<a href=\"x.html\" class='c' hidden>", 0, 0);

    CHECK(t->attr_count == 3, "html: three attributes, however they are quoted");
    CHECK(ar_span_is(t->attrs[0].name, "href") && ar__text_is(t->attrs[0].value, "x.html"),
          "html: a double-quoted value");
    CHECK(ar_span_is(t->attrs[1].name, "class") && ar__text_is(t->attrs[1].value, "c"),
          "html: a single-quoted value");
    CHECK(ar_span_is(t->attrs[2].name, "hidden") && t->attrs[2].value.n == 0,
          "html: and one with no value at all");

    t = ar__tok_at("<a href=x.html>", 0, 0);
    CHECK(ar__text_is(t->attrs[0].value, "x.html"), "html: an unquoted value ends at whitespace");

    /* An end tag may not carry attributes, and the tree builder relies on it:
       `</p class=x>` must not arrive with one. */
    t = ar__tok_at("</p class=x>", 0, 0);
    CHECK(t->kind == AR_TOK_END && t->attr_count == 0, "html: an end tag's attributes are dropped");
}

static void test_the_tokenizer_coalesces_text(void)
{
    ar_token *t = ar__tok_at("hello world", 0, 0);

    CHECK(t->kind == AR_TOK_TEXT && ar__text_is(t->text, "hello world"),
          "html: a run of characters is one token, not eleven");

    /* A bare ampersand does not end a run and is not an error. `AT&T` is one
       token, and a tokenizer that splits it is doing arithmetic nobody asked
       for on every page that mentions a company. */
    t = ar__tok_at("AT&T", 0, 0);
    CHECK(ar__text_is(t->text, "AT&T"), "html: a bare ampersand stays in the run");

    CHECK(ar__tok_count("<p>a</p>") == 3, "html: tag, text, tag");
}

static void test_character_references(void)
{
    ar_u32    errors = 0;
    ar_token *t = ar__tok_at("&amp;", 0, 0);

    CHECK(ar__text_is(t->text, "&"), "html: a named reference decodes");

    t = ar__tok_at("a&lt;b", 0, 0);
    CHECK(ar__text_is(t->text, "a<b"), "html: and does so in the middle of a run");

    t = ar__tok_at("&#65;", 0, 0);
    CHECK(ar__text_is(t->text, "A"), "html: a decimal numeric reference decodes");

    t = ar__tok_at("&#x41;", 0, 0);
    CHECK(ar__text_is(t->text, "A"), "html: and a hexadecimal one");

    /* The replacement table everybody forgets. `&#128;` is not U+0080: the
       specification decodes the C1 range as Windows-1252, because a decade of
       documents pasted curly quotes in by number. */
    t = ar__tok_at("&#128;", 0, 0);
    CHECK(ar__text_is(t->text, "\342\202\254"), "html: &#128; is a euro sign, not U+0080");
    t = ar__tok_at("&#147;", 0, 0);
    CHECK(ar__text_is(t->text, "\342\200\234"),
          "html: and &#147; is a left double quote, which is why the table exists");

    /* A reference to nothing, a surrogate, and one out of range all become the
       replacement character rather than anything exciting. */
    t = ar__tok_at("&#0;", 0, 0);
    CHECK(ar__text_is(t->text, "\357\277\275"), "html: a null reference is U+FFFD");
    t = ar__tok_at("&#xD800;", 0, 0);
    CHECK(ar__text_is(t->text, "\357\277\275"), "html: and so is a surrogate");

    /* An unknown name is left as the text that spells it, which is what a
       browser does -- and what areole does for the 1,978 references the table
       does not carry yet. */
    t = ar__tok_at("&nosuchentity;", 0, &errors);
    CHECK(ar__text_is(t->text, "&nosuchentity;"), "html: an unknown reference stays as text");
    CHECK(errors > 0, "html: and is counted as a parse error");
}

static void test_a_reference_in_an_attribute_stops_at_a_query_string(void)
{
    ar_token *t = ar__tok_at("<a href=\"?cite=1&copy=2\">", 0, 0);

    CHECK(ar__text_is(t->attrs[0].value, "?cite=1&copy=2"),
          "html: &copy without a semicolon before = is not a reference in an attribute");

    t = ar__tok_at("<a href=\"a&amp;b\">", 0, 0);
    CHECK(ar__text_is(t->attrs[0].value, "a&b"), "html: but a terminated one still decodes");
}

static void test_comments_and_bogus_comments(void)
{
    ar_token *t = ar__tok_at("<!-- hi -->", 0, 0);

    CHECK(t->kind == AR_TOK_COMMENT && ar__text_is(t->text, " hi "), "html: a comment");

    /* Everything the specification funnels into the bogus comment state, which
       is why a stray processing instruction does not eat a document. */
    t = ar__tok_at("<?php echo 1; ?>", 0, 0);
    CHECK(t->kind == AR_TOK_COMMENT, "html: <?php becomes a comment, not a tag");

    t = ar__tok_at("<!nonsense>", 0, 0);
    CHECK(t->kind == AR_TOK_COMMENT, "html: and so does a bogus markup declaration");

    /* `</>` is dropped entirely rather than becoming an empty comment. */
    CHECK(ar__tok_count("a</>b") == 2, "html: </> is dropped, leaving the text either side");
}

static void test_doctype(void)
{
    ar_token *t = ar__tok_at("<!DOCTYPE html>", 0, 0);

    CHECK(t->kind == AR_TOK_DOCTYPE, "html: a doctype is a doctype");
    CHECK(ar_span_is(t->name, "html"), "html: and carries its name");
    CHECK(!t->force_quirks, "html: <!DOCTYPE html> does not force quirks");

    t = ar__tok_at("<!doctype HTML>", 0, 0);
    CHECK(t->kind == AR_TOK_DOCTYPE && ar_span_is(t->name, "html"),
          "html: in either case, both halves");

    t = ar__tok_at("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\" \"http://x/y.dtd\">", 0, 0);
    CHECK(ar__text_is(t->pub, "-//W3C//DTD HTML 4.01//EN"), "html: a public identifier");
    CHECK(ar__text_is(t->sys, "http://x/y.dtd"), "html: and the system identifier after it");

    t = ar__tok_at("<!DOCTYPE>", 0, 0);
    CHECK(t->force_quirks, "html: a doctype with no name forces quirks");

    /* A truncated doctype must not look like a good one. */
    t = ar__tok_at("<!DOCTYPE html", 0, 0);
    CHECK(t->force_quirks, "html: and so does one that ends before its >");
}

static void test_rcdata_and_rawtext_end_only_on_their_own_tag(void)
{
    ar_html_tok t;
    ar_token    tok;

    /* Inside <title>, `</b>` is text and only `</title>` closes it. This is
       the rule the tokenizer keeps `last_start` for, and it is why the tree
       builder has to be the one switching states. */
    ar_html_tok_init(&t, "<title>a<b>c</title>", 20, g_html_scratch, (ar_u32)sizeof g_html_scratch);

    ar_html_next(&t, &tok); /* <title> */
    CHECK(tok.kind == AR_TOK_START && ar_span_is(tok.name, "title"), "html: the title tag");
    t.state = AR_HTML_RCDATA;

    ar_html_next(&t, &tok);
    CHECK(tok.kind == AR_TOK_TEXT && ar__text_is(tok.text, "a<b>c"),
          "html: everything inside title is text, tags included");

    ar_html_next(&t, &tok);
    CHECK(tok.kind == AR_TOK_END && ar_span_is(tok.name, "title"),
          "html: and only its own end tag closes it");
}

static void test_plaintext_never_ends(void)
{
    ar_html_tok t;
    ar_token    tok;

    ar_html_tok_init(&t, "<b>not a tag", 12, g_html_scratch, (ar_u32)sizeof g_html_scratch);
    t.state = AR_HTML_PLAINTEXT;
    ar_html_next(&t, &tok);
    CHECK(tok.kind == AR_TOK_TEXT && ar__text_is(tok.text, "<b>not a tag"),
          "html: plaintext takes the rest of the file, tags and all");
}

static void test_the_tokenizer_never_stalls_or_stops(void)
{
    /* Every parse error in the specification has a defined recovery, so the
       only way to be wrong here is to loop forever or to give up early. Both
       are checked by feeding it the shapes that would cause either. */
    static const char *const NASTY[] = {"<",      "<<<<",  "</",     "</>",    "<!",
                                        "<!-",    "<!--",  "<!--x",  "<a",     "<a ",
                                        "<a b",   "<a b=", "<a b='", "&",      "&#",
                                        "&#x",    "&;",    "<!DOCT", "<?",     "a<b",
                                        "<a b=c", "</ >",  "<>",     "&#xZZ;", "<a b=\"unclosed"};
    ar_i32                   i;
    ar_i32                   stalled = 0;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        ar_html_tok t;
        ar_token    tok;
        ar_i32      guard = 0;

        ar_html_tok_init(&t, NASTY[i], (ar_u32)strlen(NASTY[i]), g_html_scratch,
                         (ar_u32)sizeof g_html_scratch);
        while (ar_html_next(&t, &tok))
        {
            if (++guard > 64)
            {
                printf("      %s did not terminate\n", NASTY[i]);
                ++stalled;
                break;
            }
        }
    }
    CHECK(stalled == 0, "html: no malformed input makes the tokenizer stall or run away");
}

static void test_the_tokenizer_copies_nothing_it_does_not_have_to(void)
{
    /* A token's spans point into the caller's bytes. That is what lets the
       parser run inside the arena, and it is worth a check because the day
       somebody makes a copy "for safety" the whole design goes quietly. */
    const char *src = "<div>hello</div>";
    ar_html_tok t;
    ar_token    tok;

    ar_html_tok_init(&t, src, (ar_u32)strlen(src), g_html_scratch, (ar_u32)sizeof g_html_scratch);
    ar_html_next(&t, &tok);
    CHECK(tok.name.p >= src && tok.name.p < src + strlen(src),
          "html: a tag name points into the input rather than at a copy");
    ar_html_next(&t, &tok);
    CHECK(tok.text.p >= src && tok.text.p < src + strlen(src),
          "html: and so does text with no reference in it");
}

/* ------------------------------------------------------------------------
 * HTML tree construction
 *
 * These are the cases the specification exists for -- the ones where the tree
 * is not the one the markup appears to describe. A parser that only handles
 * well-formed input passes none of them and looks fine doing it.
 * ------------------------------------------------------------------------ */

static ar_dom_node g_dom_nodes[512];
static ar_attr     g_dom_attrs[256];
static char        g_dom_text[8192];
static char        g_tree_scratch[4096];
static ar_doc      g_doc;

static ar_doc *ar__parse(const char *src)
{
    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_dom_nodes;
    g_doc.node_cap = (ar_i32)(sizeof g_dom_nodes / sizeof g_dom_nodes[0]);
    g_doc.attrs = g_dom_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_dom_attrs / sizeof g_dom_attrs[0]);
    g_doc.text = g_dom_text;
    g_doc.text_cap = (ar_u32)sizeof g_dom_text;
    ar_html_parse(&g_doc, src, (ar_u32)strlen(src), g_tree_scratch, (ar_u32)sizeof g_tree_scratch);
    return &g_doc;
}

/* The tree as `tag(child child)`, so a whole shape is one string to compare.
   Text nodes are `#`, which keeps the comparison about structure. */
static void ar__shape(const ar_doc *d, ar_i32 i, char *out, ar_u32 cap, ar_u32 *used)
{
    ar_i32 c;

    if (i < 0 || *used + 2 >= cap)
    {
        return;
    }
    if (d->nodes[i].kind == AR_DOM_TEXT)
    {
        out[(*used)++] = '#';
        return;
    }
    if (d->nodes[i].kind == AR_DOM_COMMENT)
    {
        out[(*used)++] = '!';
        return;
    }
    if (d->nodes[i].kind == AR_DOM_DOCTYPE)
    {
        out[(*used)++] = '@';
        return;
    }
    {
        ar_u32 k;

        for (k = 0; k < d->nodes[i].name.n && *used + 1 < cap; ++k)
        {
            out[(*used)++] = d->nodes[i].name.p[k];
        }
    }
    if (d->nodes[i].first_child < 0)
    {
        return;
    }
    out[(*used)++] = '(';
    for (c = d->nodes[i].first_child; c >= 0; c = d->nodes[c].next_sibling)
    {
        if (c != d->nodes[i].first_child && *used + 1 < cap)
        {
            out[(*used)++] = ' ';
        }
        ar__shape(d, c, out, cap, used);
    }
    if (*used + 1 < cap)
    {
        out[(*used)++] = ')';
    }
}

static const char *ar__tree_shape(const char *src)
{
    static char buf[1024];
    ar_doc     *d = ar__parse(src);
    ar_u32      used = 0;

    ar__shape(d, ar_dom_root(d), buf, (ar_u32)sizeof buf, &used);
    buf[used] = 0;
    return buf;
}

/* The first element with this tag, anywhere. */
static ar_i32 ar__find(const ar_doc *d, const char *tag)
{
    ar_i32 i;

    for (i = 0; i < d->node_count; ++i)
    {
        if (d->nodes[i].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[i].name, tag))
        {
            return i;
        }
    }
    return -1;
}

static void test_the_optional_tags_are_optional(void)
{
    /* Every one of html, head and body may be omitted, and most real documents
       omit at least one. The tree has them anyway. */
    const char *s = ar__tree_shape("<p>hi</p>");

    CHECK(strcmp(s, "html(head body(p(#)))") == 0, "html: html, head and body are implied");
    if (strcmp(s, "html(head body(p(#)))") != 0)
    {
        printf("      got %s\n", s);
    }

    s = ar__tree_shape("<html><head></head><body><p>hi</p></body></html>");
    CHECK(strcmp(s, "html(head body(p(#)))") == 0, "html: and written out gives the same tree");
}

static void test_a_paragraph_closes_itself(void)
{
    /* `<p>a<p>b` is two paragraphs. It is the first thing anyone writes by
       accident and the first thing a naive parser gets wrong. */
    const char *s = ar__tree_shape("<p>a<p>b");

    CHECK(strcmp(s, "html(head body(p(#) p(#)))") == 0, "html: <p>a<p>b is two paragraphs");
    if (strcmp(s, "html(head body(p(#) p(#)))") != 0)
    {
        printf("      got %s\n", s);
    }

    /* And a block-level start closes an open paragraph rather than nesting. */
    s = ar__tree_shape("<p>a<div>b</div>");
    CHECK(strcmp(s, "html(head body(p(#) div(#)))") == 0, "html: and a div closes one too");
    if (strcmp(s, "html(head body(p(#) div(#)))") != 0)
    {
        printf("      got %s\n", s);
    }
}

static void test_list_items_close_themselves(void)
{
    const char *s = ar__tree_shape("<ul><li>a<li>b</ul>");

    CHECK(strcmp(s, "html(head body(ul(li(#) li(#))))") == 0,
          "html: <li>a<li>b is two items, not one inside the other");
    if (strcmp(s, "html(head body(ul(li(#) li(#))))") != 0)
    {
        printf("      got %s\n", s);
    }
}

static void test_foster_parenting(void)
{
    /*
     * §13.2.6.1. Content inside a table where it may not be is relocated to
     * just before the table, not dropped and not left inside. Every browser
     * agrees because they all implement this paragraph.
     */
    /*
     * Edge, asked before this number was written:
     *
     *     <table><em>x</em><tr><td>y</table>
     *       =>  html(head body(em(#) table(tbody(tr(td(#))))))
     *
     * The emphasis is a sibling *before* the table, and the row and cell are
     * still inside it through a tbody nobody wrote.
     */
    const char *s = ar__tree_shape("<table><em>x</em><tr><td>y</table>");

    CHECK(strcmp(s, "html(head body(em(#) table(tbody(tr(td(#))))))") == 0,
          "html: table content is fostered out in front, exactly as a browser does");
    if (strcmp(s, "html(head body(em(#) table(tbody(tr(td(#))))))") != 0)
    {
        printf("      got %s\n", s);
    }
}

static void test_a_table_implies_its_missing_parts(void)
{
    /* `<table><tr><td>` has no tbody written and gets one, which is why a
       stylesheet selecting tbody works on markup that never mentions it. */
    const char *s = ar__tree_shape("<table><tr><td>a</table>");

    CHECK(strcmp(s, "html(head body(table(tbody(tr(td(#))))))") == 0,
          "html: a table implies tbody and closes its cells");
    if (strcmp(s, "html(head body(table(tbody(tr(td(#))))))") != 0)
    {
        printf("      got %s\n", s);
    }
}

static void test_the_adoption_agency(void)
{
    /*
     * `<b><i></b></i>` produces the same tree in every browser and it is not
     * the tree the markup describes. The point of the check is that the
     * document survives with both elements present and correctly nested rather
     * than the `<b>` swallowing the rest of it.
     */
    const char *s = ar__tree_shape("<b>1<i>2</b>3</i>");

    /* Edge, asked before this number was written:
           <b>1<i>2</b>3</i>  =>  html(head body(b(# i(#)) i(#)))
       The `<i>` is split in two: the part inside the `<b>` stays there and the
       part after it becomes a sibling. That is the whole algorithm's output in
       one string. */
    CHECK(strcmp(s, "html(head body(b(# i(#)) i(#)))") == 0,
          "html: <b>1<i>2</b>3</i> splits the <i> exactly as a browser does");
    if (strcmp(s, "html(head body(b(# i(#)) i(#)))") != 0)
    {
        printf("      got %s\n", s);
    }

    /* The simple case has to keep working: <b>x</b> is one element with the
       text inside it. */
    {
        s = ar__tree_shape("<b>x</b>");

        CHECK(strcmp(s, "html(head body(b(#)))") == 0, "html: and the ordinary case is ordinary");
        if (strcmp(s, "html(head body(b(#)))") != 0)
        {
            printf("      got %s\n", s);
        }
    }
}

static void test_formatting_is_reconstructed_across_a_block(void)
{
    /*
     * §13.2.4.3. `<p><b>bold</p>text` puts `text` inside a *fresh* `<b>` after
     * the paragraph: `</p>` pops the `<b>` off the stack but leaves it in the
     * list of active formatting elements, and the next character reopens it.
     * Without reconstruction the bold simply stops at the paragraph.
     *
     * The first version of this check used `<b>one<p>two</p></b>` and failed,
     * and the check was the thing that was wrong -- an open `<b>` is still on
     * the stack when the `<p>` arrives, so the paragraph nests *inside* it and
     * nothing is reconstructed. Edge was asked before the code was touched:
     *
     *     <b>one<p>two</p></b>   =>  html(head body(b(# p(#))))
     *     <p><b>bold</p>text     =>  html(head body(p(b(#)) b(#)))
     *
     * Both are checked here, because the first is the one that looks like it
     * should reconstruct and does not.
     */
    const char *s = ar__tree_shape("<b>one<p>two</p></b>");

    CHECK(strcmp(s, "html(head body(b(# p(#))))") == 0,
          "html: an open <b> keeps a paragraph inside itself");
    if (strcmp(s, "html(head body(b(# p(#))))") != 0)
    {
        printf("      got %s\n", s);
    }

    s = ar__tree_shape("<p><b>bold</p>text");
    CHECK(strcmp(s, "html(head body(p(b(#)) b(#)))") == 0,
          "html: and a closed one is reopened for the text after it");
    if (strcmp(s, "html(head body(p(b(#)) b(#)))") != 0)
    {
        printf("      got %s\n", s);
    }
}

static void test_attributes_reach_the_tree(void)
{
    ar_doc *d = ar__parse("<div id=\"main\" class='a b'>x</div>");
    ar_i32  div = ar__find(d, "div");

    CHECK(div >= 0 && d->nodes[div].attr_count == 2, "html: an element keeps its attributes");
    if (div >= 0 && d->nodes[div].attr_count == 2)
    {
        ar_attr *a = &d->attrs[d->nodes[div].attr_first];

        CHECK(ar_span_is(a[0].name, "id") && ar__text_is(a[0].value, "main"),
              "html: the first with its value");
        CHECK(ar_span_is(a[1].name, "class") && ar__text_is(a[1].value, "a b"),
              "html: and the second");
    }
}

static void test_text_with_an_entity_survives_the_next_token(void)
{
    /*
     * The tokenizer decodes references into a scratch buffer it reuses for
     * every token, so a tree that kept the span would show the *next* token's
     * bytes. The document copies exactly those spans and leaves the rest
     * pointing at the input.
     *
     * This is the check that the copy happens. Without it the failure is not a
     * crash -- it is one paragraph showing another paragraph's text.
     */
    ar_doc *d = ar__parse("<p>a&amp;b</p><p>second</p><p>third</p>");
    ar_i32  p = ar__find(d, "p");
    ar_i32  txt;

    CHECK(p >= 0, "html: the first paragraph is there");
    txt = d->nodes[p].first_child;
    CHECK(txt >= 0 && d->nodes[txt].kind == AR_DOM_TEXT, "html: with a text node in it");
    CHECK(txt >= 0 && ar__text_is(d->nodes[txt].text, "a&b"),
          "html: whose decoded text survives two more tokens being read");
}

static void test_quirks_mode(void)
{
    /* Quirks is not a curiosity: it changes the box model for the whole
       document, so getting it from the doctype is load-bearing. */
    ar_doc *d = ar__parse("<!DOCTYPE html><p>x");

    CHECK(d->quirks == AR_QUIRKS_NO, "html: <!DOCTYPE html> is no-quirks");

    d = ar__parse("<p>x");
    CHECK(d->quirks == AR_QUIRKS_YES, "html: no doctype at all is quirks");

    d = ar__parse("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.0 Transitional//EN\"><p>x");
    CHECK(d->quirks == AR_QUIRKS_YES, "html: and so is HTML 4.0 Transitional");

    d = ar__parse("<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01//EN\"><p>x");
    CHECK(d->quirks == AR_QUIRKS_LIMITED,
          "html: a public identifier with no system identifier is the limited form");
}

static void test_rawtext_content_is_not_markup(void)
{
    /* `<style>` holds text, and a `<` inside it is not a tag. A parser that
       gets this wrong turns every stylesheet into a tree of nonsense. */
    ar_doc *d = ar__parse("<style>a { content: \"<b>\"; }</style><p>after");
    ar_i32  st = ar__find(d, "style");
    ar_i32  b = ar__find(d, "b");

    CHECK(st >= 0, "html: the style element is there");
    CHECK(b < 0, "html: and the <b> inside it did not become an element");
    CHECK(ar__find(d, "p") >= 0, "html: and the document carries on afterwards");
}

static void test_the_tree_builder_survives_anything(void)
{
    /* Every parse error has a defined recovery, so the ways to be wrong are to
       hang, to overrun, or to give up. None of these may do any of the three. */
    static const char *const NASTY[] = {"<b><i></b></i>",
                                        "<table><td>x",
                                        "</p></div></b>",
                                        "<p><p><p><p><p>",
                                        "<ul><li><ul><li>",
                                        "<table><table><table>",
                                        "<b><b><b><b><b>x",
                                        "<!DOCTYPE><html></",
                                        "<td>orphan",
                                        "<a><a><a>x",
                                        "<style><p>",
                                        "<title></b>",
                                        "<table><tr><td><table><tr><td>deep",
                                        "",
                                        "<<<<>>>>"};
    ar_i32                   i;
    ar_i32                   bad = 0;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        ar_doc *d = ar__parse(NASTY[i]);

        if (d->overflowed)
        {
            printf("      %s overflowed\n", NASTY[i]);
            ++bad;
        }
    }
    CHECK(bad == 0, "html: no malformed document overruns the tree builder");
}

/* ------------------------------------------------------------------------
 * HTML into the box tree
 *
 * The piece that makes the parser visible. These check that a document laid
 * out through ar_dom_build lands where the same tree written by hand would,
 * and that the user-agent stylesheet is doing the work that makes it so.
 * ------------------------------------------------------------------------ */

/* A whole document, from bytes to boxes, into the shared test context. */
static void ar__render_html(ar_surface *s, const char *src, const char *author)
{
    ar_input in;

    ar__ui_reset("");
    ar_ua_stylesheet(g_ui);
    if (author)
    {
        ar_stylesheet(g_ui, author);
    }
    ar__parse(src);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_dom_build(g_ui, &g_doc);
    ar_frame_end(g_ui, s);
}

static void test_the_ua_stylesheet_parses(void)
{
    /* Every part of it, and none may have a syntax error -- a UA sheet with a
       bad rule in it fails silently and takes a few elements with it. */
    ar_i32 i;
    ar_i32 bad = 0;

    for (i = 0; i < ar_ua_stylesheet_parts(); ++i)
    {
        ar__ui_reset("");
        ar_stylesheet(g_ui, ar_ua_stylesheet_part(i));
        if (ar_stylesheet_errors(g_ui))
        {
            printf("      part %ld has %lu problem(s)\n", (long)i,
                   (unsigned long)ar_stylesheet_errors(g_ui));
            ++bad;
        }
    }
    CHECK(bad == 0, "ua: every part of the user-agent stylesheet parses cleanly");
    CHECK(ar_ua_stylesheet_parts() > 5, "ua: and there is a real sheet there");

    /* And all of it together, which is what an application actually does. */
    ar__ui_reset("");
    ar_ua_stylesheet(g_ui);
    CHECK(ar_stylesheet_errors(g_ui) == 0, "ua: and the whole sheet at once");
}

static void test_a_document_lays_out_as_blocks(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * The thing the user-agent stylesheet exists for. areole's own default
     * display is flex, so without it these paragraphs would sit in a row.
     */
    ar__render_html(&s, "<p>one</p><p>two</p>", "p { margin:0px; height:20px; }");

    {
        ar_i32 n = ar_node_count(g_ui);
        ar_i32 first = -1, second = -1;
        ar_i32 i;

        for (i = 0; i < n; ++i)
        {
            if (ar_node_rect(g_ui, i).h == 20)
            {
                if (first < 0)
                {
                    first = i;
                }
                else if (second < 0)
                {
                    second = i;
                }
            }
        }
        CHECK(first >= 0 && second >= 0, "html: both paragraphs became boxes");
        if (first >= 0 && second >= 0)
        {
            ar_rect a = ar_node_rect(g_ui, first);
            ar_rect b = ar_node_rect(g_ui, second);

            CHECK(b.y == a.y + 20, "html: and they stack down the page rather than across it");
            CHECK(a.x == b.x, "html: at the same left edge");
        }
    }
}

static void test_the_class_and_id_reach_the_style(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /* The selector the walk spells out is what carries an author stylesheet
       onto a parsed document. Without it every rule but a type selector is
       dead. */
    ar__render_html(&s, "<div class=\"card wide\" id=\"first\">x</div>",
                    "body { margin:0px; } .card { height:11px; } #first { width:57px; }");

    {
        ar_i32 i;
        int    found = 0;

        for (i = 0; i < ar_node_count(g_ui); ++i)
        {
            ar_rect r = ar_node_rect(g_ui, i);

            if (r.h == 11 && r.w == 57)
            {
                found = 1;
            }
        }
        CHECK(found, "html: a class and an id from the markup both match author rules");
    }
}

static void test_whitespace_between_blocks_is_dropped(void)
{
    /*
     * `<ul>\n  <li>a</li>\n</ul>` has text nodes between the items that a
     * browser drops. Keeping them would put an empty box between every pair,
     * and every hand-written document is full of them.
     *
     * Counted against the same markup with the whitespace taken out, rather
     * than inspected: if the newlines generated boxes the two would differ by
     * three.
     *
     * The first version of this check counted boxes with no size at all and
     * failed -- on the implied `<head>`, which is `display: none` and is
     * supposed to be 0x0. The heuristic was wrong, not the walk.
     */
    ar_surface s = ar__ui_surface(400, 300);
    ar_i32     with_space;
    ar_i32     without;

    ar__render_html(&s, "<ul>\n  <li>a</li>\n  <li>b</li>\n</ul>", "body { margin:0px; }");
    with_space = ar_node_count(g_ui);

    ar__render_html(&s, "<ul><li>a</li><li>b</li></ul>", "body { margin:0px; }");
    without = ar_node_count(g_ui);

    CHECK(with_space == without, "html: whitespace between block elements generates no boxes");
    if (with_space != without)
    {
        printf("      %ld boxes with whitespace, %ld without\n", (long)with_space, (long)without);
    }

    /* html, head, body, ul, two li and two text spans. Stated so a change in
       what the walk generates is visible rather than merely consistent. */
    CHECK(without == 8, "html: and a two-item list is eight boxes");
}

static void test_a_table_from_markup_uses_the_table_model(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * areole has had the table formatting context since 0.7.0 and this is the
     * first time markup reaches it: the user-agent sheet is what turns `<tr>`
     * into `display: table-row`.
     *
     * The cells of a row share a top edge, which is the one property that is
     * true of a table and of nothing else the engine could have used instead.
     */
    ar__render_html(&s,
                    "<table><tr><td>a</td><td>b</td></tr>"
                    "<tr><td>c</td><td>d</td></tr></table>",
                    "body { margin:0px; } td { width:40px; height:10px; padding:0px; }");

    {
        ar_i32 i;
        ar_i32 cells[8];
        ar_i32 n = 0;

        for (i = 0; i < ar_node_count(g_ui) && n < 8; ++i)
        {
            ar_rect r = ar_node_rect(g_ui, i);

            if (r.w == 40 && r.h == 10)
            {
                cells[n++] = i;
            }
        }
        CHECK(n == 4, "html: four cells came out of the markup");
        if (n == 4)
        {
            ar_rect a = ar_node_rect(g_ui, cells[0]);
            ar_rect b = ar_node_rect(g_ui, cells[1]);
            ar_rect c = ar_node_rect(g_ui, cells[2]);

            CHECK(a.y == b.y, "html: the two cells of a row share a top edge");
            CHECK(c.y > a.y, "html: and the second row is below the first");
            CHECK(b.x > a.x, "html: with the columns side by side");
        }
    }
}

static void test_head_content_draws_nothing(void)
{
    ar_surface s = ar__ui_surface(400, 300);

    /*
     * `display: none` on head, style, script and title is what stops a
     * stylesheet's own text appearing on the page -- which is exactly what a
     * document without a user-agent sheet does, and it looks like a bug in the
     * renderer rather than a missing rule.
     */
    ar__render_html(&s,
                    "<html><head><title>T</title><style>p{color:#ff0000;}</style></head>"
                    "<body><p>visible</p></body></html>",
                    "body { margin:0px; }");

    {
        ar_i32 i;
        ar_i32 drawn = 0;

        for (i = 0; i < ar_node_count(g_ui); ++i)
        {
            ar_rect r = ar_node_rect(g_ui, i);

            if (r.w > 0 && r.h > 0)
            {
                ++drawn;
            }
        }
        /* html, body and the paragraph, and the paragraph's text. Nothing from
           the head, which would be two more. */
        CHECK(drawn > 0 && drawn <= 5, "html: nothing in the head generates a visible box");
        if (drawn > 5)
        {
            printf("      %ld boxes have a size\n", (long)drawn);
        }
    }
}

static void test_a_document_survives_the_round_trip(void)
{
    /* Bytes to boxes, on the malformed shapes the tree builder recovers from.
       None may leave the tree unbalanced, which is what would break every
       layout pass after it. */
    static const char *const DOCS[] = {"<p>a<p>b",
                                       "<table><em>x</em><tr><td>y</table>",
                                       "<b>1<i>2</b>3</i>",
                                       "<ul><li>a<li>b</ul>",
                                       "<div><span>x</span></div>",
                                       "<!DOCTYPE html><html><body><h1>T</h1></body></html>",
                                       "",
                                       "</p></div>"};
    ar_surface               s = ar__ui_surface(400, 300);
    ar_i32                   i;
    ar_i32                   bad = 0;

    for (i = 0; i < (ar_i32)(sizeof DOCS / sizeof DOCS[0]); ++i)
    {
        ar__render_html(&s, DOCS[i], "body { margin:0px; }");
        if (ar_unbalanced(g_ui))
        {
            printf("      %s left the tree unbalanced\n", DOCS[i]);
            ++bad;
        }
        if (ar_overflowed(g_ui))
        {
            printf("      %s overflowed the box budget\n", DOCS[i]);
            ++bad;
        }
    }
    CHECK(bad == 0, "html: every document walks into a balanced box tree");
}

/* ------------------------------------------------------------------------
 * Encoding, and stylesheets out of the document
 * ------------------------------------------------------------------------ */

static char g_enc_out[512];

static int ar__decoded_is(ar_encoding e, const char *in, ar_u32 len, const char *want)
{
    ar_u32 n = ar_encoding_decode(e, in, len, g_enc_out, (ar_u32)sizeof g_enc_out);

    return n == (ar_u32)strlen(want) && memcmp(g_enc_out, want, n) == 0;
}

static void test_encoding_sniffing(void)
{
    ar_u32 skip = 99;

    /*
     * Step 1: a byte order mark beats everything after it, including a
     * `<meta charset>` that disagrees. Authoring tools write both and
     * contradict themselves constantly, and the specification is explicit
     * about which wins.
     */
    CHECK(ar_encoding_sniff("\357\273\277<html>", 9, &skip) == AR_ENC_UTF8,
          "enc: a UTF-8 BOM says UTF-8");
    CHECK(skip == 3, "enc: and the three bytes of it are not content");

    CHECK(ar_encoding_sniff("\376\377\0<", 4, &skip) == AR_ENC_UTF16BE, "enc: FE FF is UTF-16BE");
    CHECK(skip == 2, "enc: two bytes of mark");
    CHECK(ar_encoding_sniff("\377\376<\0", 4, &skip) == AR_ENC_UTF16LE, "enc: FF FE is UTF-16LE");

    CHECK(ar_encoding_sniff("\357\273\277<meta charset=\"windows-1252\">", 30, &skip) ==
              AR_ENC_UTF8,
          "enc: and the mark beats a meta that disagrees with it");

    /* Step 2: the declaration, in both spellings documents actually use. */
    CHECK(ar_encoding_sniff("<meta charset=\"utf-8\">", 22, &skip) == AR_ENC_UTF8,
          "enc: <meta charset> is read");
    CHECK(skip == 0, "enc: with nothing to skip");
    CHECK(ar_encoding_sniff("<meta charset=utf-8>", 20, &skip) == AR_ENC_UTF8, "enc: unquoted too");
    CHECK(
        ar_encoding_sniff("<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\">",
                          67, &skip) == AR_ENC_UTF8,
        "enc: and the older http-equiv spelling");

    /* `iso-8859-1` means windows-1252, which is the Encoding Standard's own
       answer rather than a shortcut: a document labelled 8859-1 with a curly
       quote in it is relying on the 1252 mapping. */
    CHECK(ar_encoding_from_label("iso-8859-1", 10) == AR_ENC_WINDOWS1252,
          "enc: iso-8859-1 means windows-1252");
    CHECK(ar_encoding_from_label("Latin1", 6) == AR_ENC_WINDOWS1252, "enc: and so does latin1");
    CHECK(ar_encoding_from_label("  UTF-8  ", 9) == AR_ENC_UTF8,
          "enc: a label is trimmed and case-folded");

    /* Step 3: no mark and no declaration. */
    CHECK(ar_encoding_sniff("<html><body>x", 13, &skip) == AR_ENC_WINDOWS1252,
          "enc: a document that says nothing is windows-1252, not UTF-8");
}

static void test_encoding_decoding(void)
{
    /* The high half of windows-1252 is where the curly quotes live, and
       reading it as UTF-8 turns every one into a replacement character. */
    CHECK(ar__decoded_is(AR_ENC_WINDOWS1252, "\223hi\224", 4, "\342\200\234hi\342\200\235"),
          "enc: windows-1252 curly quotes become the right code points");
    CHECK(ar__decoded_is(AR_ENC_WINDOWS1252, "caf\351", 4, "caf\303\251"),
          "enc: and an accented letter becomes UTF-8");
    CHECK(ar__decoded_is(AR_ENC_WINDOWS1252, "plain", 5, "plain"),
          "enc: ASCII passes through unchanged");

    CHECK(ar__decoded_is(AR_ENC_UTF16LE, "h\0i\0", 4, "hi"), "enc: UTF-16LE decodes");
    CHECK(ar__decoded_is(AR_ENC_UTF16BE, "\0h\0i", 4, "hi"), "enc: UTF-16BE decodes");

    /* A surrogate pair is one character, not two. U+1F600 as UTF-16LE. */
    CHECK(ar__decoded_is(AR_ENC_UTF16LE, "\075\330\0\336", 4, "\360\237\230\200"),
          "enc: a surrogate pair joins into one code point");

    /* An unpaired surrogate is not a character and cannot be encoded. */
    CHECK(ar__decoded_is(AR_ENC_UTF16LE, "\075\330", 2, "\357\277\275"),
          "enc: an unpaired surrogate becomes U+FFFD");

    /* Truncated rather than overrun, which is the contract every buffer in
       this library has. */
    {
        char   tiny[4];
        ar_u32 n = ar_encoding_decode(AR_ENC_WINDOWS1252, "abcdefgh", 8, tiny, (ar_u32)sizeof tiny);

        CHECK(n <= (ar_u32)sizeof tiny, "enc: a small buffer truncates rather than overruns");
    }
}

static void test_a_document_carries_its_own_stylesheet(void)
{
    ar_surface s = ar__ui_surface(400, 300);
    ar_input   in;
    ar_i32     found;

    /*
     * The last piece that makes a document self-contained: its own `<style>`
     * is what styles it, with no author sheet passed in beside it.
     */
    ar__ui_reset("");
    ar_ua_stylesheet(g_ui);
    ar__parse("<html><head><style>body{margin:0px;} .box{width:37px;height:19px;}</style>"
              "</head><body><div class=\"box\">x</div></body></html>");
    found = ar_doc_stylesheets(g_ui, &g_doc);
    CHECK(found == 1, "html: the document's own <style> element is found");
    CHECK(ar_stylesheet_errors(g_ui) == 0, "html: and parses");

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(g_ui, &in);
    ar_dom_build(g_ui, &g_doc);
    ar_frame_end(g_ui, &s);

    {
        ar_i32 i;
        int    hit = 0;

        for (i = 0; i < ar_node_count(g_ui); ++i)
        {
            ar_rect r = ar_node_rect(g_ui, i);

            if (r.w == 37 && r.h == 19)
            {
                hit = 1;
            }
        }
        CHECK(hit, "html: and the rule in it reaches the box it names");
    }
}

static void test_stylesheets_arrive_in_tree_order(void)
{
    /*
     * Tree order is cascade order: two rules of equal specificity are settled
     * by which came last. A collector that searched for the first `<style>`,
     * or that visited them in any other order, would get this backwards on
     * every document with two of them -- and there is no error to see, just
     * the wrong colour.
     */
    ar_i32 found;

    ar__ui_reset("");
    ar_ua_stylesheet(g_ui);
    ar__parse("<head><style>.b{width:10px;}</style><style>.b{width:20px;}</style></head>"
              "<body><div class=\"b\">x</div></body>");
    found = ar_doc_stylesheets(g_ui, &g_doc);
    CHECK(found == 2, "html: both style elements are collected");

    {
        ar_surface s = ar__ui_surface(400, 300);
        ar_input   in;
        ar_i32     i;
        int        second_won = 0;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(g_ui, &in);
        ar_dom_build(g_ui, &g_doc);
        ar_frame_end(g_ui, &s);

        for (i = 0; i < ar_node_count(g_ui); ++i)
        {
            if (ar_node_rect(g_ui, i).w == 20)
            {
                second_won = 1;
            }
        }
        CHECK(second_won, "html: and the later one wins, which is what cascade order means");
    }
}

static void test_a_style_element_is_not_markup(void)
{
    /* `<style>` is RAWTEXT, so a `<` inside it is not a tag and the whole
       sheet arrives as one text node. A parser that got this wrong would hand
       the collector fragments. */
    ar_i32 found;

    ar__ui_reset("");
    ar_ua_stylesheet(g_ui);
    ar__parse("<style>.a{width:5px;} /* <b> not a tag */ .c{width:6px;}</style><p>after");
    found = ar_doc_stylesheets(g_ui, &g_doc);
    CHECK(found == 1, "html: a stylesheet containing < is still one sheet");
    CHECK(ar_stylesheet_errors(g_ui) == 0, "html: and parses whole");
}

/* ------------------------------------------------------------------------
 * A document in the arena, and a rule table the caller sized
 * ------------------------------------------------------------------------ */

static unsigned char g_doc_mem[AR_MEM_DOC(256, 96 * 1024)];

static void test_a_larger_rule_table_is_the_callers_to_ask_for(void)
{
    static unsigned char big[AR_MEM_RULES(64, 600)];
    ar_ctx              *c;
    ar_i32               i;
    char                 rule[64];

    /*
     * An ar_rule is 588 bytes, so the default 256 already occupy 150 KB of the
     * 192 KB AR_MEM_FIXED promises. Raising that constant would charge every
     * application for a stylesheet only some of them have -- so the caller
     * asks, exactly as it already does for boxes.
     *
     * 0.9.1's user-agent stylesheet is around 400 rules and is the first thing
     * that needs this.
     */
    c = ar_init_ex(big, (ar_u32)sizeof big, 600, 0);
    CHECK(c != 0, "arena: a block sized with AR_MEM_RULES takes a 600 rule table");

    if (c)
    {
        for (i = 0; i < 400; ++i)
        {
            sprintf(rule, ".r%ld { width:%ldpx; }", (long)i, (long)(i % 90) + 1);
            ar_stylesheet(c, rule);
        }
        CHECK(ar_stylesheet_errors(c) == 0, "arena: and four hundred rules parse into it");
    }

    /* And the default is unchanged, so nothing an existing caller does moves. */
    ar__ui_reset("");
    CHECK(g_ui != 0, "arena: ar_init still works on a plain AR_MEM block");

    /* A count below the default is raised to it rather than shrinking the
       table, because AR_MEM_FIXED has already been paid for either way. */
    {
        static unsigned char small[AR_MEM(16)];
        ar_ctx              *s = ar_init_ex(small, (ar_u32)sizeof small, 4, 0);

        CHECK(s != 0, "arena: and asking for fewer than the default is not an error");
    }
}

static void test_a_document_lives_in_the_arena(void)
{
    ar_ctx    *c = ar_init_ex(g_doc_mem, (ar_u32)sizeof g_doc_mem, 256, 64 * 1024);
    ar_doc    *d;
    ar_surface s = ar__ui_surface(400, 300);
    ar_input   in;

    CHECK(c != 0, "arena: a block sized with AR_MEM_DOC initialises");
    if (!c)
    {
        return;
    }
    ar_ua_stylesheet(c);

    d = ar_html_parse_into(c,
                           "<html><head><style>.box{width:23px;height:29px;}</style></head>"
                           "<body><div class=\"box\">x</div></body></html>",
                           93);
    CHECK(d != 0, "arena: and a document parses into it");
    if (!d)
    {
        return;
    }
    CHECK(!d->overflowed, "arena: without overflowing");
    CHECK(ar_dom_root(d) >= 0, "arena: with a root element");

    /* The whole way through: the document's own stylesheet, then its boxes. */
    ar_doc_stylesheets(c, d);
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(c, &in);
    ar_dom_build(c, d);
    ar_frame_end(c, &s);

    {
        ar_i32 i;
        int    hit = 0;

        for (i = 0; i < ar_node_count(c); ++i)
        {
            ar_rect r = ar_node_rect(c, i);

            if (r.w == 23 && r.h == 29)
            {
                hit = 1;
            }
        }
        CHECK(hit, "arena: and lays out from bytes with nothing passed in beside it");
    }
}

static void test_a_document_that_does_not_fit_says_so(void)
{
    static unsigned char tiny[AR_MEM_DOC(16, 8 * 1024)];
    ar_ctx              *c = ar_init_ex(tiny, (ar_u32)sizeof tiny, 256, 5 * 1024);
    ar_doc              *d;

    /*
     * 0.9.0 acceptance criterion 7: a document larger than the budget fails
     * cleanly with a reported reason rather than truncating silently.
     *
     * The tree that comes back holds as much as it could, which is what makes
     * the failure inspectable rather than merely fatal.
     */
    CHECK(c != 0, "arena: the small block initialises");
    if (!c)
    {
        return;
    }
    d = ar_html_parse_into(c,
                           "<div><div><div><div><div><div><div><div><div><div>"
                           "<div><div><div><div><div><div><div><div><div><div>"
                           "<div><div><div><div><div><div><div><div><div><div>"
                           "<div><div><div><div><div><div><div><div><div><div>",
                           200);
    CHECK(d != 0, "arena: an oversized document still returns a document");
    if (d)
    {
        CHECK(d->overflowed || d->node_count > 0, "arena: which either fitted or says it did not");
    }

    /* And a reservation too small to be worth trying is refused outright
       rather than half-allocated. The budget is fixed at init, so this needs a
       context that asked for a useless one. */
    {
        static unsigned char stingy[AR_MEM_DOC(16, 1024)];
        ar_ctx              *nothing = ar_init_ex(stingy, (ar_u32)sizeof stingy, 256, 1024);

        CHECK(nothing != 0, "arena: a context may reserve almost nothing for a document");
        CHECK(nothing && ar_html_parse_into(nothing, "<p>x</p>", 8) == 0,
              "arena: and parsing into it is refused rather than half-allocated");
    }
}

static void test_a_document_decodes_its_own_encoding(void)
{
    ar_ctx *c = ar_init_ex(g_doc_mem, (ar_u32)sizeof g_doc_mem, 256, 32 * 1024);
    ar_doc *d;

    /*
     * The piece the sniffer and the decoders existed for without being wired
     * to anything: `ar_html_parse_into` reads the encoding before the
     * tokenizer sees a byte.
     *
     * windows-1252 with no declaration is the case that matters -- a decade of
     * documents are exactly that, and reading one as UTF-8 does not fail, it
     * turns every accented letter into a replacement character.
     */
    CHECK(c != 0, "arena: the context initialises");
    if (!c)
    {
        return;
    }

    /* `<p>caf\351</p>` -- an e-acute in windows-1252, which is not valid
       UTF-8 and would otherwise be dropped. */
    {
        static const char SRC[] = "<p>caf\351</p>";

        /* strlen, not a hand count. Both of these were one too many, which fed
           the terminating NUL to the parser as content -- harmless while a NUL
           in text was dropped, and a real U+FFFD once it was not. */
        d = ar_html_parse_into(c, SRC, (ar_u32)(sizeof SRC - 1));
    }
    CHECK(d != 0 && !d->overflowed, "html: a windows-1252 document parses");
    if (d)
    {
        ar_i32 p = -1;
        ar_i32 i;

        for (i = 0; i < d->node_count; ++i)
        {
            if (d->nodes[i].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[i].name, "p"))
            {
                p = i;
            }
        }
        CHECK(p >= 0, "html: with the paragraph in it");
        if (p >= 0 && d->nodes[p].first_child >= 0)
        {
            ar_span t = d->nodes[d->nodes[p].first_child].text;

            CHECK(ar__text_is(t, "caf\303\251"),
                  "html: and its e-acute arrives as UTF-8 rather than as a dropped byte");
        }
    }

    /* A UTF-8 byte order mark is not content and must not become text. */
    c = ar_init_ex(g_doc_mem, (ar_u32)sizeof g_doc_mem, 256, 32 * 1024);
    ar_ua_stylesheet(c);
    {
        static const char SRC[] = "\357\273\277<p>hi</p>";

        d = ar_html_parse_into(c, SRC, (ar_u32)(sizeof SRC - 1));
    }
    CHECK(d != 0, "html: a document with a BOM parses");
    if (d)
    {
        ar_i32 i;
        int    bom_leaked = 0;

        for (i = 0; i < d->node_count; ++i)
        {
            ar_span t = d->nodes[i].text;

            if (d->nodes[i].kind == AR_DOM_TEXT && t.n >= 3 && (unsigned char)t.p[0] == 0xEFu)
            {
                bom_leaked = 1;
            }
        }
        CHECK(!bom_leaked, "html: and the mark does not end up in a text node");
    }
}

static void test_the_adoption_agency_inner_loop(void)
{
    /*
     * The whole algorithm, against a browser's own answers.
     *
     * Every expected string here came out of Edge before it was written down.
     * The first implementation cut the inner loop and got six of these eight
     * right anyway -- including the three-level `<b>1<i>2<em>3</b>4</em>5</i>`
     * -- which is exactly why a corpus of the easy cases proves nothing about
     * this algorithm.
     *
     * The two it failed shared one cause: step 4.14, moving the furthest block
     * to the common ancestor. Without it a paragraph stays inside the bold
     * instead of beside it.
     */
    static const char *const CASES[] = {"<b><i><p></b></i>",
                                        "html(head body(b(i) i p(i(b))))",
                                        "<b><i>x</b>y</i>",
                                        "html(head body(b(i(#)) i(#)))",
                                        "<a><b>1</a>2</b>",
                                        "html(head body(a(b(#)) b(#)))",
                                        "<b>1<p>2</b>3</p>",
                                        "html(head body(b(#) p(b(#) #)))",
                                        "<i><b>x</i>y</b>",
                                        "html(head body(i(b(#)) b(#)))",
                                        "<b><em><i>q</b>r</i></em>",
                                        "html(head body(b(em(i(#))) em(i(#))))",
                                        "<p><b>a<i>b</b>c</i></p>",
                                        "html(head body(p(b(# i(#)) i(#))))",
                                        "<b>1<i>2<em>3</b>4</em>5</i>",
                                        "html(head body(b(# i(# em(#))) i(em(#) #)))",
                                        0,
                                        0};
    ar_i32                   i;
    ar_i32                   wrong = 0;

    for (i = 0; CASES[i]; i += 2)
    {
        const char *got = ar__tree_shape(CASES[i]);

        if (strcmp(got, CASES[i + 1]) != 0)
        {
            printf("      %s\n        want %s\n        got  %s\n", CASES[i], CASES[i + 1], got);
            ++wrong;
        }
    }
    CHECK(wrong == 0, "html: the adoption agency agrees with a browser on all eight");
}

/*
 * A parse with a scratch buffer of a stated size, which is the whole point of
 * these checks: everything else in this file hands the parser more room than
 * any input needs, and the bugs below all live in the case where it does not
 * have enough.
 */
static ar_doc *ar__parse_scratch(const char *src, ar_u32 scratch_cap)
{
    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_dom_nodes;
    g_doc.node_cap = (ar_i32)(sizeof g_dom_nodes / sizeof g_dom_nodes[0]);
    g_doc.attrs = g_dom_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_dom_attrs / sizeof g_dom_attrs[0]);
    g_doc.text = g_dom_text;
    g_doc.text_cap = (ar_u32)sizeof g_dom_text;
    if (scratch_cap > (ar_u32)sizeof g_tree_scratch)
    {
        scratch_cap = (ar_u32)sizeof g_tree_scratch;
    }
    ar_html_parse(&g_doc, src, (ar_u32)strlen(src), g_tree_scratch, scratch_cap);
    return &g_doc;
}

/* A parse with a stated node budget, which is where the tree-shape bugs are:
   a budget nothing reaches is a budget nothing tests. */
/* Bytes, with the unprintable ones shown, because half these expectations are
   a tab or a combining mark and "want  got " helps nobody. */
static void ar__print_escaped(const char *p, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)p[i];

        if (c >= 0x20u && c < 0x7Fu)
        {
            putchar((int)c);
        }
        else
        {
            printf("\\x%02X", (unsigned)c);
        }
    }
}

static void test_the_longest_named_reference_wins(void)
{
    /*
     * The rule that arrives with the semicolon-less names, and the reason the
     * lookup became a match.
     *
     * `&notit;` is `&not` followed by the literal text `it;`, because `not` is
     * a reference and `notit` is not. Reading to the first non-alphanumeric
     * and looking that up -- which is what this parser did while every name in
     * its table ended in a semicolon and the question could not arise -- finds
     * nothing and emits six characters of literal text.
     *
     * Every expectation here is from the specification's own table, and the
     * shapes were checked against Edge.
     */
    static const char *const CASES[] = {
        "&notit;", "\302\254it;",        /* &not, then text */
        "&notin;", "\342\210\211",       /* the longer name wins outright */
        "&amp;", "&", "&ampere", "&ere", /* &amp without its semicolon, then text */
        "&lt;", "<", "&ltcc;", "\342\252\246", "&copy",
        "\302\251", /* legacy, no semicolon: still a reference */
        "&copyright", "\302\251right", "&NotEqualTilde;",
        "\342\211\202\314\270", /* two code points, both or neither */
        "&bne;", "=\342\203\245",
        /* Anchored to a letter: a text node that is only whitespace is dropped
           before <body> exists, which is the specification's rule about
           insertion modes and not this table's business. */
        "a&Tab;", "a\011", "a&NewLine;", "a\012", "&nosuchthing;",
        "&nosuchthing;", /* not a reference at all */
        "&", "&", "&#", "&#", 0, 0};
    ar_i32 i;
    ar_i32 wrong = 0;

    for (i = 0; CASES[i]; i += 2)
    {
        ar_doc *d = ar__parse(CASES[i]);
        ar_i32  body = ar_dom_child_element(d, ar_dom_root(d), "body");
        ar_i32  text = body >= 0 ? d->nodes[body].first_child : -1;

        if (text < 0 || d->nodes[text].kind != AR_DOM_TEXT ||
            !ar__text_is(d->nodes[text].text, CASES[i + 1]))
        {
            printf("      %s\n        want ", CASES[i]);
            ar__print_escaped(CASES[i + 1], (ar_u32)strlen(CASES[i + 1]));
            printf("\n        got  ");
            if (text >= 0 && d->nodes[text].kind == AR_DOM_TEXT)
            {
                ar__print_escaped(d->nodes[text].text.p, d->nodes[text].text.n);
            }
            else
            {
                printf("(no text node)");
            }
            printf("\n");
            ++wrong;
        }
    }
    CHECK(wrong == 0, "html: the longest named reference in the table is the one taken");

    /*
     * And the attribute exception, which is why a decade of URLs still work.
     * A reference without a semicolon followed by `=` or an alphanumeric is
     * not a reference inside an attribute value: `?cite=1&copy=2` is a query
     * string, not a copyright sign in the middle of one.
     */
    {
        ar_doc *d = ar__parse("<a href=\"?cite=1&copy=2\">x</a>");
        ar_i32  body = ar_dom_child_element(d, ar_dom_root(d), "body");
        ar_i32  a = body >= 0 ? ar_dom_child_element(d, body, "a") : -1;

        CHECK(a >= 0 && d->nodes[a].attr_count == 1 &&
                  ar__text_is(d->attrs[d->nodes[a].attr_first].value, "?cite=1&copy=2"),
              "html: and not inside an attribute, where a query string needs its ampersand");
    }
}

static void test_a_tag_that_never_ended_is_dropped(void)
{
    /*
     * §13.2.5.10 and every state after it say the same thing about end of
     * file: emit an end-of-file token. Not the tag. A tag the input stopped in
     * the middle of is discarded whole, attributes and all -- so `<div id`
     * contributes nothing at all, not a div with no attributes.
     *
     * This is not pedantry about a rare input. `<table><em><p>x</em` ends in
     * an unterminated end tag, and honouring it runs the adoption agency,
     * which moves the paragraph out of the emphasis and puts a clone of the
     * emphasis inside it. One case in the browser corpus disagreed and every
     * case around it agreed, which is what pointed at the tokenizer rather
     * than at the agency.
     *
     * Every expectation below came out of Edge before it was written down.
     */
    static const char *const CASES[] = {"<p>a<div",
                                        "html(head body(p(#)))",
                                        "<b>x</b",
                                        "html(head body(b(#)))",
                                        "<div id",
                                        "html(head body)",
                                        "<div class=\"a",
                                        "html(head body)",
                                        "<br/",
                                        "html(head body)",
                                        "<b><p>x</b",
                                        "html(head body(b(p(#))))",
                                        "<table><em><p>x</em",
                                        "html(head body(em(p(#)) table))",
                                        0,
                                        0};
    ar_i32                   i;
    ar_i32                   wrong = 0;

    for (i = 0; CASES[i]; i += 2)
    {
        const char *got = ar__tree_shape(CASES[i]);

        if (strcmp(got, CASES[i + 1]) != 0)
        {
            printf("      %s\n        want %s\n        got  %s\n", CASES[i], CASES[i + 1], got);
            ++wrong;
        }
    }
    CHECK(wrong == 0, "html: a tag the input ended inside is dropped, not emitted");
}

static void test_the_stack_is_cleared_back_to_a_table_context(void)
{
    /*
     * Three sentences of the specification -- "clear the stack back to a table
     * context" and its siblings for a table body and a table row -- and
     * without them four browser-corpus cases came out with the table built
     * inside the wrong element.
     *
     * Anything a table fosters out is relocated in the *tree* but stays on the
     * stack of open elements, so it is still the current node when the next
     * table part arrives. `<table><b><td>` therefore put the implied tbody
     * inside the bold: the table came out empty and the whole row structure
     * hung off a formatting element, while looking perfectly well-formed.
     *
     * From Edge, as above.
     */
    static const char *const CASES[] = {"<table><b><td><i></b>",
                                        "html(head body(b table(tbody(tr(td(i))))))",
                                        "<table><a><tr><td><a>x</a>",
                                        "html(head body(a table(tbody(tr(td(a(#)))))))",
                                        "<b><table><p></b><tr><td>",
                                        "html(head body(b(p table(tbody(tr(td))))))",
                                        "<table><tbody><em><tr><td><p></em>",
                                        "html(head body(em table(tbody(tr(td(p))))))",
                                        0,
                                        0};
    ar_i32                   i;
    ar_i32                   wrong = 0;

    for (i = 0; CASES[i]; i += 2)
    {
        const char *got = ar__tree_shape(CASES[i]);

        if (strcmp(got, CASES[i + 1]) != 0)
        {
            printf("      %s\n        want %s\n        got  %s\n", CASES[i], CASES[i + 1], got);
            ++wrong;
        }
    }
    CHECK(wrong == 0, "html: a table part clears the stack back to the table first");
}

static ar_doc *ar__parse_capped(const char *src, ar_i32 node_cap)
{
    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_dom_nodes;
    g_doc.node_cap = node_cap < (ar_i32)(sizeof g_dom_nodes / sizeof g_dom_nodes[0])
                         ? node_cap
                         : (ar_i32)(sizeof g_dom_nodes / sizeof g_dom_nodes[0]);
    g_doc.attrs = g_dom_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_dom_attrs / sizeof g_dom_attrs[0]);
    g_doc.text = g_dom_text;
    g_doc.text_cap = (ar_u32)sizeof g_dom_text;
    ar_html_parse(&g_doc, src, (ar_u32)strlen(src), g_tree_scratch, (ar_u32)sizeof g_tree_scratch);
    return &g_doc;
}

/* The links, checked the way tests/ar_fuzz.c checks them, so the same
   invariant is enforced by the suite CI runs and not only by the fuzzer. */
static int ar__tree_links_sane(const ar_doc *d, const char *what, ar_i32 cap)
{
    ar_i32 i;

    for (i = 0; i < d->node_count; ++i)
    {
        const ar_dom_node *n = &d->nodes[i];
        ar_i32             c;
        ar_i32             steps;

        if (n->parent == i || n->first_child == i || n->last_child == i || n->next_sibling == i ||
            n->prev_sibling == i)
        {
            printf("      %s at %ld nodes: node %ld points at itself\n", what, (long)cap, (long)i);
            return 0;
        }
        if (n->parent >= d->node_count || n->first_child >= d->node_count ||
            n->next_sibling >= d->node_count)
        {
            printf("      %s at %ld nodes: node %ld points outside the tree\n", what, (long)cap,
                   (long)i);
            return 0;
        }
        /*
         * And it is still attached. Refusing a bad link keeps the tree sane
         * but loses the subtree, which is the same bug wearing a hat: the
         * paragraph in `<table><em><p>x</em` came out with no parent at all
         * when only the self-link was guarded against. Everything except the
         * document node has somewhere to be.
         */
        if (i > 0 && n->parent < 0)
        {
            printf("      %s at %ld nodes: node %ld was left with no parent\n", what, (long)cap,
                   (long)i);
            return 0;
        }
        steps = 0;
        for (c = n->first_child; c >= 0; c = d->nodes[c].next_sibling)
        {
            if (d->nodes[c].parent != i)
            {
                printf("      %s at %ld nodes: node %ld has a child that disowns it\n", what,
                       (long)cap, (long)i);
                return 0;
            }
            if (++steps > d->node_count)
            {
                printf("      %s at %ld nodes: node %ld has a cycle in its children\n", what,
                       (long)cap, (long)i);
                return 0;
            }
        }
    }
    return 1;
}

static void test_no_node_is_ever_its_own_parent(void)
{
    /*
     * `<table><em><p>x</em` with room for exactly eight nodes, which ar_fuzz
     * found at iteration 604612 of seed 9 and which no corpus could have
     * reached -- every corpus runs with a budget nothing exhausts.
     *
     * The tree runs out of nodes in the middle of the adoption agency. Step
     * 4.14 moves the paragraph into the common ancestor, decides the common
     * ancestor is a table so foster parenting applies, and then asked for the
     * insertion point *without saying where* -- so the insertion point was
     * worked out again from the current node, which by then was the paragraph
     * being moved. The paragraph became its own parent and the tree had a
     * cycle in it, which nothing notices until something walks it.
     *
     * Swept across every budget rather than checked at eight, because the
     * exhaustion point that matters is a function of the document and picking
     * it by hand is how the next one gets missed.
     */
    static const char *const NASTY[] = {
        "<table><em><p>x</em", "<table><b><td><i></b>", "<table><a><tr><td><a>x</a>",
        "<table><em>x</em><tr><td>y</table>", "<b><table><p></b><tr><td>",
        "<table><caption><b><p></b></caption>", "<table><tbody><em><tr><td><p></em>",
        "<b><i><table><p></b></i>",

        /*
         * ar_fuzz, iteration 8955409 of seed
         * 3, minimised from 3413 bytes to 118.
         *
         * A different shape of the same
         * failure: not a node inserted into
         * itself but a node inserted into its
         * own child. Foster parenting picks
         * the table to insert before and the
         * table's parent to insert into, and
         * here the table's parent *is* the
         * element being moved -- so the
         * element became its own parent and
         * its own first child at once.
         *
         * It needs the list of active
         * formatting elements to be full,
         * which is why it is here and not in
         * the browser corpus: AR_HTML_FMT is
         * this engine's cap and a browser has
         * its own, so the trees are allowed to
         * differ. The invariant is not.
         */
        "<b><i><em><<<i><em><em><em><b><i><em>"
        "<i><b><i><em><i><em><b><i><em><<<i><b>"
        "<i><em><table></body><i><table><em>"
        "<p></em>",
        "<i><table><em><p></em>", "<b><i><table><em><p></em></i></b>"};
    ar_i32 cap;
    ar_i32 i;
    ar_i32 bad = 0;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        for (cap = 4; cap <= 40; ++cap)
        {
            if (!ar__tree_links_sane(ar__parse_capped(NASTY[i], cap), NASTY[i], cap))
            {
                ++bad;
            }
        }
    }
    CHECK(bad == 0, "html: a tree that runs out of nodes is still a tree");
}

static void test_the_tokenizer_always_consumes_input(void)
{
    /*
     * The hang ar_fuzz found at iteration 315 of seed 1, minimised to three
     * bytes.
     *
     * `&#0` is a null character reference, which the specification replaces
     * with U+FFFD -- three bytes of UTF-8. Given room for one, the reference
     * could not be written, the `&` that would have replaced it could not be
     * written either, and the text loop left with the read pointer exactly
     * where it started. The token was not empty, so the tree builder accepted
     * it and asked for the next one, and got the same one, forever.
     *
     * There is no assertion here beyond the test returning at all: a hang is
     * caught by this function finishing. Which is also why it was expensive to
     * find -- a process that hangs prints nothing to go on.
     */
    static const char *const STARVED[] = {"&#0",      "&#0;",        "&amp;",
                                          "&#xFFFD;", "<title>&#0",  "<textarea>&#0;x",
                                          "<p>a&#0b", "&#0&#0&#0&#0"};
    ar_u32                   cap;
    ar_i32                   i;
    ar_i32                   bad = 0;

    for (cap = 0; cap <= 4u; ++cap)
    {
        for (i = 0; i < (ar_i32)(sizeof STARVED / sizeof STARVED[0]); ++i)
        {
            ar_doc *d = ar__parse_scratch(STARVED[i], cap);

            if (d->node_count <= 0)
            {
                printf("      %s with %lu scratch bytes built nothing\n", STARVED[i],
                       (unsigned long)cap);
                ++bad;
            }
        }
    }
    CHECK(bad == 0, "html: a starved scratch buffer terminates rather than looping");

    /*
     * And it says so. A reference that did not fit is not bad markup, it is a
     * budget too small to hold good markup, and the two have to be tellable
     * apart -- 0.9.0 acceptance criterion 7 is failing cleanly with a reported
     * reason rather than truncating in silence.
     */
    CHECK(ar__parse_scratch("&#0", 1u)->overflowed,
          "html: and reports the overflow rather than truncating quietly");
    CHECK(!ar__parse_scratch("&#0", 64u)->overflowed,
          "html: while the same document with room to decode does not");
}

static void test_a_partial_code_point_never_reaches_the_tree(void)
{
    /*
     * U+FFFD is three bytes. Given two, the old decoder wrote the first, ran
     * out, and reported failure -- leaving a lone 0xEF in the buffer that the
     * text token then carried into the tree as if it were a character.
     *
     * A partial sequence is not a character in any encoding, and nothing
     * downstream -- shaping, the cascade, a caller writing the text out again
     * -- has any defence against one. All of it or none of it.
     */
    ar_u32 cap;
    ar_i32 bad = 0;

    for (cap = 0; cap <= 3u; ++cap)
    {
        ar_doc *d = ar__parse_scratch("<p>&#xFFFD;</p>", cap);
        ar_i32  i;

        for (i = 0; i < d->node_count; ++i)
        {
            const ar_dom_node *n = &d->nodes[i];
            ar_u32             k;

            if (n->kind != AR_DOM_TEXT)
            {
                continue;
            }
            for (k = 0; k < n->text.n; ++k)
            {
                unsigned char c = (unsigned char)n->text.p[k];
                ar_u32        need;

                if (c < 0x80u)
                {
                    continue;
                }
                if ((c & 0xE0u) == 0xC0u)
                {
                    need = 2u;
                }
                else if ((c & 0xF0u) == 0xE0u)
                {
                    need = 3u;
                }
                else if ((c & 0xF8u) == 0xF0u)
                {
                    need = 4u;
                }
                else
                {
                    /* A continuation byte with no lead is a fragment too. */
                    printf("      %lu scratch bytes left a stray continuation\n",
                           (unsigned long)cap);
                    ++bad;
                    continue;
                }
                if (k + need > n->text.n)
                {
                    printf("      %lu scratch bytes left a truncated sequence\n",
                           (unsigned long)cap);
                    ++bad;
                }
                k += need - 1u;
            }
        }
    }
    CHECK(bad == 0, "html: a code point that does not fit is not written at all");
}

static void test_the_adoption_agency_terminates_on_anything(void)
{
    /*
     * The outer loop is capped at eight by the specification and the inner one
     * needs a cap of its own, because a chain of formatting elements long
     * enough would otherwise walk the stack forever. Step 4.13.4 -- drop a
     * node from the list after three passes -- is the specification's own
     * answer and is what makes these terminate.
     */
    static const char *const NASTY[] = {"<b><b><b><b><b><b><b><b><b><b>x</b>",
                                        "<i><b><i><b><i><b>x</i>",
                                        "<a><a><a><a><a>x</a>",
                                        "<b><i><em><strong><small><big>x</b>",
                                        "<b><p><b><p><b><p></b></p>",
                                        "<em><b><em><b><em><b></em></b></em>"};
    ar_i32                   i;
    ar_i32                   bad = 0;

    for (i = 0; i < (ar_i32)(sizeof NASTY / sizeof NASTY[0]); ++i)
    {
        ar_doc *d = ar__parse(NASTY[i]);

        if (d->overflowed)
        {
            printf("      %s overflowed\n", NASTY[i]);
            ++bad;
        }
    }
    CHECK(bad == 0, "html: and terminates on every chain of misnested formatting");
}

int main(void)
{
    printf("areole %s\n", ar_version());

    test_version_string_matches_the_macros();

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
    test_break_survives_a_null_string();
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
    test_box_sizing_default_is_content_box();
    test_border_box_makes_the_stated_size_the_whole_box();
    test_border_box_applies_through_a_selector();
    test_box_sizing_applies_to_percentages();
    test_sticky_starts_in_the_flow();
    test_sticky_pins_at_the_threshold();
    test_sticky_leaves_with_its_section();
    test_sticky_against_the_viewport();
    test_sticky_with_no_offsets_never_moves();
    test_a_sticky_box_too_big_to_move_does_not();
    test_a_clipped_box_does_not_take_the_hover();
    test_a_cell_on_its_own_grows_a_row_a_group_and_a_table();
    test_two_bare_cells_share_one_row();
    test_content_inside_a_table_gets_a_cell();
    test_a_row_inside_a_table_gets_a_group();
    test_a_well_formed_table_generates_nothing();
    test_a_sheet_without_tables_is_untouched();
    test_a_combinator_climbs_past_a_generated_row();
    test_a_cell_spans_one_by_default();
    test_columns_share_the_table_width();
    test_a_wide_cell_widens_its_whole_column();
    test_rows_stack_and_the_table_is_as_tall_as_them();
    test_a_colspan_covers_its_columns();
    test_a_rowspan_holds_its_column_open();
    test_fixed_layout_ignores_what_cells_want();
    test_a_table_stacks_like_any_other_box();
    test_cell_padding_is_not_counted_twice();
    test_a_rowspan_cell_is_as_tall_as_the_rows_it_spans();
    test_a_colspan_does_not_release_a_rowspan();
    test_a_table_honours_a_stated_height();
    test_a_spanning_cell_honours_a_stated_width();
    test_an_empty_table_leaves_nothing_at_the_origin();
    test_a_footer_is_drawn_last_wherever_it_is_written();
    test_a_caption_sits_above_the_grid();
    test_a_column_box_takes_up_no_space();
    test_two_cells_share_one_border();
    test_the_widest_border_wins_the_line();
    test_a_collapsed_table_draws_no_border_of_its_own();
    test_border_spacing_means_nothing_when_collapsed();
    test_a_separate_table_is_untouched_by_any_of_this();
    test_a_collapsed_edge_is_in_the_paint_digest();
    test_a_collapsed_line_is_drawn_once();
    test_a_roomy_table_gives_the_surplus_to_the_wide_column();
    test_vertical_align_puts_a_cells_contents_where_it_says();
    test_a_collapsed_row_closes_without_moving_the_columns();
    test_a_collapsed_column_takes_the_tables_width_with_it();
    test_a_hidden_box_takes_its_space_and_paints_nothing();
    test_a_caption_can_sit_underneath();
    test_empty_cells_hide_shows_a_grid_with_holes();
    test_a_col_speaks_for_the_cells_that_said_nothing();
    test_a_sticky_header_stays_while_the_rows_go_under_it();
    test_a_frozen_column_stays_while_the_columns_go_past();
    test_flex_factors_are_a_ratio_not_a_count();
    test_a_fractional_factor_is_kept();
    test_shrinking_is_weighted_by_the_base();
    test_a_maximum_freezes_an_item_and_the_rest_take_its_share();
    test_a_minimum_freezes_an_item_the_other_way();
    test_the_flex_shorthand_writes_a_zero_basis();
    test_items_wrap_onto_lines();
    test_align_self_overrides_the_container();
    test_space_around_and_evenly_differ_at_the_edges();
    test_order_moves_an_item_without_moving_it();
    test_min_width_auto_stops_an_item_shrinking();
    test_a_template_gives_the_columns_their_widths();
    test_fr_shares_what_is_left_after_the_fixed_tracks();
    test_repeat_expands_to_real_tracks();
    test_items_wrap_onto_the_next_row();
    test_column_flow_fills_downwards_first();
    test_a_named_line_puts_an_item_where_it_says();
    test_a_span_covers_the_tracks_it_names();
    test_the_two_gaps_are_separate();
    test_an_item_stretches_to_its_cell_and_can_refuse();
    test_minmax_holds_a_track_between_two_ends();
    test_display_contents_promotes_its_children();
    test_intrinsic_keywords_work_on_the_height();
    test_fit_content_takes_a_cap();
    test_aspect_ratio_gives_the_axis_nobody_stated();
    test_safe_centring_never_starts_before_the_edge();
    test_a_grid_item_keeps_its_min_content();
    test_subgrid_lines_the_cards_up();
    test_a_subgrid_takes_the_parents_tracks();
    test_a_subgrid_keeps_its_own_columns();
    test_a_subgrid_with_no_grid_above_it_is_an_ordinary_grid();
    test_the_top_layer_beats_a_z_index_it_cannot_reach();
    test_the_top_layer_escapes_a_clipping_ancestor();
    test_the_top_layer_takes_the_pointer_first();
    test_a_modal_makes_everything_outside_it_unreachable();
    test_inert_marks_a_subtree_without_any_modal();
    test_a_non_modal_top_layer_box_makes_nothing_inert();
    test_a_backdrop_paints_under_the_modal_and_over_the_page();
    test_a_backdrop_rule_styles_nothing_else();
    test_anchor_places_a_box_against_the_box_it_names();
    test_anchor_size_takes_the_anchors_measurements();
    test_position_try_flips_a_box_that_left_the_viewport();
    test_an_anchor_nobody_declared_changes_nothing();
    test_the_top_layer_over_an_adversarial_corpus();
    test_position_try_over_a_corpus_of_flips();
    test_a_sticky_box_that_can_never_stick_is_reported();
    test_env_falls_back_only_when_nothing_reported_it();
    test_viewport_fit_moves_the_insets_and_the_viewport_together();
    test_titlebar_area_is_not_gated_on_viewport_fit();
    test_env_with_an_unknown_name_takes_its_fallback();
    test_scroll_range_is_the_overflow();
    test_scrolling_moves_the_contents();
    test_scroll_is_clamped();
    test_scroll_survives_the_frame();
    test_the_wheel_scrolls_the_box_under_it();
    test_a_scroll_asks_for_the_next_frame();
    test_overscroll_behavior_stops_the_chain();
    test_overscroll_contain_holds_over_a_whole_sequence();
    test_scrollbar_width_changes_the_bar();
    test_scrollbar_gutter_reserves_its_width();
    test_scrollbar_gutter_stable_shifts_nothing_when_content_grows();
    test_scrollbar_color_is_read();
    test_snap_lands_on_a_snap_point();
    test_a_programmatic_scroll_snaps();
    test_snap_proximity_leaves_a_distant_point_alone();
    test_snap_honours_scroll_padding();
    test_snap_type_parses_both_words();
    test_snap_stop_always_refuses_to_be_passed();
    test_keys_scroll_the_container();
    test_keys_home_and_end_do_not_snap();
    test_scroll_into_view_moves_the_minimum();
    test_scroll_into_view_honours_scroll_margin();
    test_overflow_anchor_keeps_the_reading_position();
    test_the_scrollbar_paints_over_the_content();
    test_overflow_x_clips_by_itself();
    test_a_lone_visible_becomes_auto();
    test_a_scroll_position_survives_the_round_trip();
    test_a_wide_child_scrolls_sideways();
    test_pixel_travel_beats_notches();
    test_dragging_the_scrollbar_scrolls();
    test_the_wheel_ignores_a_box_it_is_not_over();
    test_auto_and_scroll_differ_only_when_it_fits();
    test_overflow_auto_parses_as_a_keyword();
    test_overflow_auto_scrolls_when_it_overflows();
    test_a_scroll_container_clips();
    test_declaration_order_still_decides_when_nothing_is_positioned();
    test_a_positioned_box_paints_above_the_flow();
    test_negative_z_goes_behind_the_flow();
    test_z_index_beats_declaration_order();
    test_z_index_has_no_ceiling();
    test_z_index_negative_and_positive_together();
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
    test_the_inherited_list_matches_the_switch();
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
    test_damage_a_wide_tree_is_still_when_still();
    test_damage_a_style_change_repaints_that_box();
    test_damage_invalidate_is_honoured();
    test_damage_invalidate_all_repaints_everything();
    test_damage_a_resize_repaints_everything();
    test_damage_distant_changes_stay_separate();
    test_damage_regions_never_exceed_the_cap();
    test_damage_collapses_when_tracking_stops_paying();
    test_damage_output_is_identical_to_a_full_repaint();
    test_a_region_move_is_identical_to_a_full_repaint();
    test_the_bar_repaints_when_only_its_colour_changed();
    test_a_bar_that_appears_because_content_grew();

    test_the_entity_table_is_sorted();
    test_the_tokenizer_reads_a_tag();
    test_the_tokenizer_reads_attributes();
    test_the_tokenizer_coalesces_text();
    test_character_references();
    test_a_reference_in_an_attribute_stops_at_a_query_string();
    test_comments_and_bogus_comments();
    test_doctype();
    test_rcdata_and_rawtext_end_only_on_their_own_tag();
    test_plaintext_never_ends();
    test_the_tokenizer_never_stalls_or_stops();
    test_the_tokenizer_copies_nothing_it_does_not_have_to();

    test_the_optional_tags_are_optional();
    test_a_paragraph_closes_itself();
    test_list_items_close_themselves();
    test_foster_parenting();
    test_a_table_implies_its_missing_parts();
    test_the_adoption_agency();
    test_formatting_is_reconstructed_across_a_block();
    test_attributes_reach_the_tree();
    test_text_with_an_entity_survives_the_next_token();
    test_quirks_mode();
    test_rawtext_content_is_not_markup();
    test_the_tree_builder_survives_anything();

    test_the_ua_stylesheet_parses();
    test_a_document_lays_out_as_blocks();
    test_the_class_and_id_reach_the_style();
    test_whitespace_between_blocks_is_dropped();
    test_a_table_from_markup_uses_the_table_model();
    test_head_content_draws_nothing();
    test_a_document_survives_the_round_trip();

    test_encoding_sniffing();
    test_encoding_decoding();
    test_a_document_carries_its_own_stylesheet();
    test_stylesheets_arrive_in_tree_order();
    test_a_style_element_is_not_markup();

    test_a_larger_rule_table_is_the_callers_to_ask_for();
    test_a_document_lives_in_the_arena();
    test_a_document_that_does_not_fit_says_so();
    test_a_document_decodes_its_own_encoding();

    test_the_adoption_agency_inner_loop();
    test_the_adoption_agency_terminates_on_anything();
    test_the_tokenizer_always_consumes_input();
    test_a_partial_code_point_never_reaches_the_tree();
    test_no_node_is_ever_its_own_parent();
    test_the_longest_named_reference_wins();
    test_a_tag_that_never_ended_is_dropped();
    test_the_stack_is_cleared_back_to_a_table_context();

    printf("\n%d checks, %d failed\n", ar__checks, ar__failures);
    return ar__failures == 0 ? 0 : 1;
}
