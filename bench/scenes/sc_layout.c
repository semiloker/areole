/*
 * areole benchmark - layout scenes.
 * SPDX-License-Identifier: MIT
 *
 * Box count is the variable; the stylesheet is deliberately tiny so that what
 * is measured is the solver rather than the cascade. sc_style.c does the
 * opposite.
 *
 * The three sizes exist so the shape of the curve is visible, not just a
 * point. Layout that is linear in box count is correct; layout that is
 * super-linear has an accidental quadratic in it, and finding that early is
 * worth far more than shaving a constant factor.
 */
#include "../bench.h"

#include <string.h>

static const char *const SHEET_A = ".row  { display:flex; flex-direction:row; gap:2px; }"
                                   ".col  { display:flex; flex-direction:column; gap:2px; }"
                                   ".leaf { width:grow; height:12px; background:#3a4a5a; }";

static const char *const SHEET_B = "#root { display:flex; flex-direction:column;"
                                   "        background:#101214; padding:4px; }";

static void common_init(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_A);
    ar_stylesheet(e->ui, SHEET_B);
}

static void begin_frame(bench_env *e)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(e->ui, &in);
}

/* A wide, shallow tree: one row per sixteen leaves. Depth is constant, so box
   count is the only thing that varies. */
static void flat(bench_env *e, int boxes)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < boxes; ++i)
    {
        if (i % 16 == 0)
        {
            if (i)
            {
                ar_end(e->ui);
            }
            ar_begin(e->ui, "div.row");
        }
        ar_begin(e->ui, "div.leaf");
        ar_end(e->ui);
    }
    if (boxes)
    {
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void frame_flat_100(bench_env *e)
{
    flat(e, 100);
}
static void frame_flat_1k(bench_env *e)
{
    flat(e, 1000);
}
static void frame_flat_8k(bench_env *e)
{
    flat(e, 8000);
}

static const bench_scene SC_FLAT_100 = {
    "flat_100",  "layout",      "100 boxes, shallow: the fixed cost of a frame", 800, 600, 1,
    common_init, frame_flat_100};
static const bench_scene SC_FLAT_1K = {"flat_1k", "layout", "1000 boxes, shallow", 800,
                                       600,       1,        common_init,           frame_flat_1k};
static const bench_scene SC_FLAT_8K = {
    "flat_8k",   "layout",     "8000 boxes: the size Clay publishes, and linearity is the point",
    800,         600,          1,
    common_init, frame_flat_8k};

/* ------------------------------------------------------------------------
 * Depth
 *
 * Both layout passes are plain loops over a flat array rather than
 * traversals, so depth should cost nothing beyond the boxes themselves. This
 * is the scene that would catch a recursion creeping back in.
 * ------------------------------------------------------------------------ */
static void deep(bench_env *e, int depth)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < depth; ++i)
    {
        ar_begin(e->ui, "div.col");
    }
    ar_begin(e->ui, "div.leaf");
    ar_end(e->ui);
    for (i = 0; i < depth; ++i)
    {
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void frame_deep_60(bench_env *e)
{
    /* One below AR_MAX_DEPTH, so this measures depth rather than the overflow
       path. Exceeding the limit is a correctness test, not a benchmark. */
    deep(e, 60);
}

static const bench_scene SC_DEEP_60 = {
    "deep_60",   "layout",     "60 levels of nesting: depth must cost nothing extra", 800, 600, 1,
    common_init, frame_deep_60};

/* ------------------------------------------------------------------------
 * Mixed
 *
 * Alternating rows and columns with varied sizing, which is what a real tree
 * looks like and what defeats any optimisation that assumes uniformity.
 * ------------------------------------------------------------------------ */
static void frame_mixed(bench_env *e)
{
    int i, j;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 40; ++i)
    {
        ar_begin(e->ui, "div.row");
        for (j = 0; j < 12; ++j)
        {
            ar_begin(e->ui, "div.col");
            ar_begin(e->ui, "div.leaf");
            ar_end(e->ui);
            ar_begin(e->ui, "div.leaf");
            ar_end(e->ui);
            ar_end(e->ui);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_MIXED = {
    "mixed_tree", "layout",   "alternating rows and columns, three levels, 1400 boxes", 800, 600, 1,
    common_init,  frame_mixed};

void bench_register_layout(void)
{
    bench_register(&SC_FLAT_100);
    bench_register(&SC_FLAT_1K);
    bench_register(&SC_FLAT_8K);
    bench_register(&SC_DEEP_60);
    bench_register(&SC_MIXED);
}
