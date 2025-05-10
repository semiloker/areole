/*
 * areole benchmark - primitive scenes.
 * SPDX-License-Identifier: MIT
 *
 * The bottom of the stack: what a pixel costs. A regression here means an
 * inner loop got worse, and nothing above it can be interpreted until this is
 * understood.
 *
 * The pair that matters most is clear_cached against clear_uncached. Identical
 * code, and the baseline measured 0.112 against 0.234 nanoseconds per pixel
 * purely because one target fits in L3 and the other does not. The target
 * hardware can never satisfy the first condition, so a fill benchmark that
 * only measures the cached case measures a machine areole does not run on.
 */
#include "../bench.h"

static ar_rect whole(const ar_surface *s)
{
    return ar_rect_make(0, 0, s->w, s->h);
}

/* ------------------------------------------------------------------------
 * Clearing
 * ------------------------------------------------------------------------ */
static void frame_clear(bench_env *e)
{
    ar_surface_clear(&e->surface, AR_HEX(0x336699));
}

static const bench_scene SC_CLEAR_CACHED = {
    "clear_cached",
    "primitive",
    "opaque span fill into a surface small enough to stay in cache",
    800,
    600,
    0,
    0,
    frame_clear};

/* Four thousand by four thousand is 64 MB, far beyond any cache on any machine
   this is likely to run on. On a Pentium II the same condition is met by an
   800x600 window, because 512 KB of L2 cannot hold 1.8 MB of framebuffer. */
static const bench_scene SC_CLEAR_UNCACHED = {
    "clear_uncached",
    "primitive",
    "the same fill against main memory rather than cache; the honest fill rate",
    4096,
    4096,
    0,
    0,
    frame_clear};

/* ------------------------------------------------------------------------
 * Rectangles
 * ------------------------------------------------------------------------ */
#define RECTS 500

static ar_i32 g_rx[RECTS], g_ry[RECTS];

static void init_rects(bench_env *e)
{
    int i;

    bench_srand(0x1234ABCDu);
    for (i = 0; i < RECTS; ++i)
    {
        g_rx[i] = (ar_i32)(bench_rand() % (ar_u32)(e->surface.w - 64));
        g_ry[i] = (ar_i32)(bench_rand() % (ar_u32)(e->surface.h - 64));
    }
}

static void fill_many(bench_env *e, ar_color c)
{
    ar_rect clip = whole(&e->surface);
    int     i;

    for (i = 0; i < RECTS; ++i)
    {
        ar_fill_rect(&e->surface, ar_rect_make(g_rx[i], g_ry[i], 64, 64), clip, c);
    }
}

static void frame_fill_opaque(bench_env *e)
{
    fill_many(e, AR_RGBA(0x33, 0x66, 0x99, 0xFF));
}

static void frame_fill_blend(bench_env *e)
{
    fill_many(e, AR_RGBA(0x33, 0x66, 0x99, 0x80));
}

static const bench_scene SC_FILL_OPAQUE = {
    "fill_opaque", "primitive",      "the opaque span fill: one word store per pixel", 1024, 768, 0,
    init_rects,    frame_fill_opaque};

/* The baseline measured blending at 5.6x the cost of opaque filling. That
   ratio is the entire case for SIMD, and it is the number this scene exists to
   keep honest. */
static const bench_scene SC_FILL_BLEND = {
    "fill_blend",
    "primitive",
    "source-over blending: read, blend, write, four multiplies",
    1024,
    768,
    0,
    init_rects,
    frame_fill_blend};

/* ------------------------------------------------------------------------
 * Clipping
 *
 * Nine tenths of the drawing is rejected. A rasterizer that rejects cheaply
 * costs almost nothing here; one that clips per pixel costs nearly full price,
 * and the difference is invisible in every other scene.
 * ------------------------------------------------------------------------ */
static void frame_offscreen(bench_env *e)
{
    ar_rect  clip = ar_rect_make(0, 0, e->surface.w / 10, e->surface.h);
    ar_color c = AR_HEX(0x336699);
    int      i;

    for (i = 0; i < RECTS; ++i)
    {
        ar_fill_rect(&e->surface, ar_rect_make(g_rx[i], g_ry[i], 64, 64), clip, c);
    }
}

static const bench_scene SC_OFFSCREEN = {"offscreen_90pc",
                                         "primitive",
                                         "nine tenths of the drawing clipped away before any pixel",
                                         1024,
                                         768,
                                         0,
                                         init_rects,
                                         frame_offscreen};

/* ------------------------------------------------------------------------
 * Thin fills
 *
 * Borders, rules and separators are one pixel wide, so the per-call overhead
 * dominates and the per-pixel cost is almost irrelevant. A real interface
 * draws hundreds of these.
 * ------------------------------------------------------------------------ */
static void frame_hairlines(bench_env *e)
{
    ar_rect  clip = whole(&e->surface);
    ar_color c = AR_HEX(0xE8DFCC);
    ar_i32   y;

    for (y = 0; y < e->surface.h; y += 4)
    {
        ar_fill_rect(&e->surface, ar_rect_make(0, y, e->surface.w, 1), clip, c);
    }
}

static const bench_scene SC_HAIRLINES = {"hairlines",
                                         "primitive",
                                         "one pixel high fills, where per-call overhead dominates",
                                         1024,
                                         768,
                                         0,
                                         0,
                                         frame_hairlines};

void bench_register_primitive(void)
{
    bench_register(&SC_CLEAR_CACHED);
    bench_register(&SC_CLEAR_UNCACHED);
    bench_register(&SC_FILL_OPAQUE);
    bench_register(&SC_FILL_BLEND);
    bench_register(&SC_OFFSCREEN);
    bench_register(&SC_HAIRLINES);
}
