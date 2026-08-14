/*
 * areole benchmark - text scenes.
 * SPDX-License-Identifier: MIT
 *
 * The baseline measured 0.639 microseconds per 8x8 glyph, and the diagnosis
 * was in the second number rather than the first: scale 2 draws four times the
 * pixels for the same money. That means the cost is per bit tested, not per
 * pixel written, and it points straight at the per-set-pixel function call in
 * the blitter.
 *
 * So these scenes measure at two scales deliberately. If a change makes scale 1
 * faster without changing the ratio to scale 2, it did not fix the actual
 * problem. 0.2.0 replaces the whole path and inherits this as its gate.
 */
#include "../bench.h"

#include <string.h>

static ar_rect whole(const ar_surface *s)
{
    return ar_rect_make(0, 0, s->w, s->h);
}

static const char *const LINE =
    "The quick brown fox jumps over the lazy dog 0123456789 -- and again.";

#define LINES 24

static void draw_block(bench_env *e, ar_i32 scale)
{
    ar_rect clip = whole(&e->surface);
    ar_i32  step = ar_text_line_height(scale);
    int     i;

    for (i = 0; i < LINES; ++i)
    {
        ar_draw_text(&e->surface, clip, 8, 8 + (ar_i32)i * step, LINE, scale, AR_HEX(0x2B2B2B));
    }
}

static void frame_para_1(bench_env *e)
{
    draw_block(e, 1);
}

static void frame_para_2(bench_env *e)
{
    draw_block(e, 2);
}

static const bench_scene SC_PARA_1 = {
    "latin_paragraph", "text", "glyph blitting at scale 1: the per-bit-test cost", 1024, 768, 0, 0,
    frame_para_1};

/* Four times the pixels. If this is not roughly four times the cost of scale 1
   once the blitter is fixed, the blitter is still paying per bit tested rather
   than per pixel written. */
static const bench_scene SC_PARA_2 = {"latin_paragraph_2x",
                                      "text",
                                      "the same text at scale 2: four times the pixels",
                                      1024,
                                      768,
                                      0,
                                      0,
                                      frame_para_2};

/* ------------------------------------------------------------------------
 * Many short labels
 *
 * What an interface actually contains: hundreds of short strings, not
 * paragraphs. Per-call overhead matters far more here than throughput.
 * ------------------------------------------------------------------------ */
static const char *const LABELS[8] = {"Home",     "Products", "Customers", "Orders",
                                      "Settings", "$4.20",    "In stock",  "Out of stock"};

static void frame_labels(bench_env *e)
{
    ar_rect clip = whole(&e->surface);
    int     i;

    for (i = 0; i < 240; ++i)
    {
        ar_i32 x = 8 + (ar_i32)((i % 6) * 168);
        ar_i32 y = 8 + (ar_i32)((i / 6) * 18);
        ar_draw_text(&e->surface, clip, x, y, LABELS[i & 7], 1, AR_HEX(0x2B2B2B));
    }
}

static const bench_scene SC_LABELS = {"many_short_labels",
                                      "text",
                                      "240 short strings, where per-call overhead dominates",
                                      1024,
                                      768,
                                      0,
                                      0,
                                      frame_labels};

/* ------------------------------------------------------------------------
 * Mostly clipped text
 *
 * A long document scrolled so that most of it is off screen. The blitter
 * rejects a glyph with one rectangle test before touching any of its
 * sixty-four bits, and this is the scene that proves it.
 * ------------------------------------------------------------------------ */
static void frame_clipped(bench_env *e)
{
    ar_rect clip = ar_rect_make(0, 0, e->surface.w, 40);
    int     i;

    for (i = 0; i < 200; ++i)
    {
        ar_draw_text(&e->surface, clip, 8, 8 + (ar_i32)i * 12, LINE, 1, AR_HEX(0x2B2B2B));
    }
}

static const bench_scene SC_CLIPPED = {
    "text_clipped", "text", "200 lines with 3 visible: whole-glyph rejection", 1024, 768, 0, 0,
    frame_clipped};

/* ------------------------------------------------------------------------
 * Measurement without drawing
 *
 * ar_text_width runs during layout for every text box, every frame, and draws
 * nothing. It is invisible in a rendering profile and is about to become the
 * inner loop of line breaking in 0.5.0.
 * ------------------------------------------------------------------------ */
static void frame_measure(bench_env *e)
{
    ar_i32 acc = 0;
    int    i;

    for (i = 0; i < 2000; ++i)
    {
        acc += ar_text_width(LABELS[i & 7], 1);
        acc += ar_text_width(LINE, 1);
    }
    /* Keep the result observable so the optimiser cannot delete the loop. The
       previous throwaway harness reported infinite copy bandwidth by exactly
       this mistake. */
    e->surface.pixels[0] = (ar_u32)acc | 0xFF000000u;
}

static const bench_scene SC_MEASURE = {
    "text_measure",
    "text",
    "ar_text_width with no drawing: the hidden cost inside layout",
    256,
    256,
    0,
    0,
    frame_measure};

void bench_register_text(void)
{
    bench_register(&SC_PARA_1);
    bench_register(&SC_PARA_2);
    bench_register(&SC_LABELS);
    bench_register(&SC_CLIPPED);
    bench_register(&SC_MEASURE);
}
