/*
 * areole benchmark - outline text.
 * SPDX-License-Identifier: MIT
 *
 * These scenes need a real font file, so they register only when one is given
 * with --font. That is a deliberate asymmetry: the rest of the library has no
 * external inputs and its benchmarks are reproducible anywhere, while these
 * depend on which face was measured. PERFORMANCE.md therefore names the font
 * beside the numbers, because "text costs 219 ns a glyph" without saying whose
 * text at what size is not a measurement, it is an anecdote.
 */
#include "../bench.h"

#include "ar_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const LINE =
    "The quick brown fox jumps over the lazy dog 0123456789 -- and again.";

#define LINES 16

static unsigned char *g_font;
static ar_u32         g_font_size;

int bench_font_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    long  n;

    if (!fp)
    {
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n <= 0)
    {
        fclose(fp);
        return 0;
    }
    g_font = (unsigned char *)malloc((size_t)n);
    if (!g_font)
    {
        fclose(fp);
        return 0;
    }
    g_font_size = (ar_u32)fread(g_font, 1, (size_t)n, fp);
    fclose(fp);
    return g_font_size > 0;
}

/* ------------------------------------------------------------------------
 * The face, its cache and its scratch, sized once for the whole run.
 * ------------------------------------------------------------------------ */
#define OUT_MAX_PX 48
/* Sixteen sizes of a Latin alphabet is well over a thousand distinct glyphs.
   Sized past that on purpose: an undersized cache is worth measuring, but not
   by accident in a scene that claims to be measuring something else. */
#define OUT_SLOTS 4096

static ar_face          g_face;
static ar_glyph_cache   g_cache;
static ar_glyph_scratch g_scratch;
static ar_glyph_slot    g_slots[OUT_SLOTS];
static ar_u8            g_atlas[4 * 1024 * 1024];
static ar_i32           g_pts[4096 * 2];
static ar_i32           g_ox[512], g_oy[512];
static ar_u8            g_on[512];
static ar_i32           g_acc[(OUT_MAX_PX + 2) * OUT_MAX_PX];
static int              g_ready;

static void ensure_face(void)
{
    if (g_ready)
    {
        return;
    }
    if (!ar_face_init(&g_face, g_font, g_font_size))
    {
        return;
    }
    g_scratch.path_pts = g_pts;
    g_scratch.path_cap = 4096;
    g_scratch.outline.x = g_ox;
    g_scratch.outline.y = g_oy;
    g_scratch.outline.on = g_on;
    g_scratch.outline.cap = 512;
    g_scratch.acc = g_acc;
    g_scratch.acc_cap = (OUT_MAX_PX + 2) * OUT_MAX_PX;
    ar_glyph_cache_init(&g_cache, g_slots, OUT_SLOTS, g_atlas, (ar_i32)sizeof g_atlas);
    g_ready = 1;
}

static void init_aa(bench_env *e)
{
    (void)e;
    ensure_face();
    g_cache.antialias = 1;
    ar_glyph_cache_clear(&g_cache);
}

static void init_aliased(bench_env *e)
{
    (void)e;
    ensure_face();
    g_cache.antialias = 0;
    ar_glyph_cache_clear(&g_cache);
}

static void frame_paragraph(bench_env *e)
{
    ar_rect clip = ar_rect_make(0, 0, e->surface.w, e->surface.h);
    ar_i32  i;

    for (i = 0; i < LINES; ++i)
    {
        ar_text_draw(&e->surface, clip, 8, 20 + i * 18, LINE, &g_face, 14, AR_HEX(0x2B2B2B),
                     &g_cache, &g_scratch);
    }
}

/* Every line at a different size, so no two lines share a cached bitmap. This
   is what an interface with a heading, body text and a caption actually does,
   and it is where an undersized atlas starts thrashing. */
static void frame_sizes(bench_env *e)
{
    ar_rect clip = ar_rect_make(0, 0, e->surface.w, e->surface.h);
    ar_i32  i, y = 20;

    for (i = 0; i < LINES; ++i)
    {
        ar_i32 px = 10 + i * 2;
        ar_text_draw(&e->surface, clip, 8, y, LINE, &g_face, px, AR_HEX(0x2B2B2B), &g_cache,
                     &g_scratch);
        y += px + 4;
    }
}

/* The cost the cache exists to avoid: every glyph rasterized from its outline,
   every frame. Not a workload anyone should have, but it is the number that
   says what the cache is worth. */
static void frame_cold(bench_env *e)
{
    ar_rect clip = ar_rect_make(0, 0, e->surface.w, e->surface.h);

    ar_glyph_cache_clear(&g_cache);
    ar_text_draw(&e->surface, clip, 8, 20, LINE, &g_face, 14, AR_HEX(0x2B2B2B), &g_cache,
                 &g_scratch);
}

static const bench_scene SC_AA = {"outline_aa",
                                  "outline",
                                  "16 lines of antialiased outline text, glyphs cached",
                                  800,
                                  600,
                                  0,
                                  init_aa,
                                  frame_paragraph};

static const bench_scene SC_ALIASED = {
    "outline_aliased",
    "outline",
    "the same text with antialiasing off: an opaque store, not a blend",
    800,
    600,
    0,
    init_aliased,
    frame_paragraph};

static const bench_scene SC_SIZES = {"outline_sizes",
                                     "outline",
                                     "16 lines at 16 different sizes: no two share a cached bitmap",
                                     800,
                                     600,
                                     0,
                                     init_aa,
                                     frame_sizes};

static const bench_scene SC_COLD = {
    "outline_cold",
    "outline",
    "one line with the cache dropped every frame: what caching is worth",
    800,
    600,
    0,
    init_aa,
    frame_cold};

void bench_register_outline(void)
{
    if (!g_font)
    {
        return; /* no --font was given; these scenes simply do not exist */
    }
    bench_register(&SC_AA);
    bench_register(&SC_ALIASED);
    bench_register(&SC_SIZES);
    bench_register(&SC_COLD);
}
