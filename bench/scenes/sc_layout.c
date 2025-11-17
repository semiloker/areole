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
    if (e->full_repaint)
    {
        ar_invalidate_all(e->ui);
    }
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

/* ------------------------------------------------------------------------
 * Block formatting and margin collapsing
 *
 * Every scene above declares display:flex, and so did every other scene in
 * this benchmark: 0.5.0 and 0.6.0 shipped block layout, inline layout,
 * floats, intrinsic sizing, positioning, stacking and scroll containers
 * without one scene touching any of them. The four below are that gap.
 *
 * Collapsing is the expensive half rather than the stacking -- a margin has
 * to consult its siblings, its parent's first and last child, and whether
 * anything between them stops it -- so the tree is shaped to make all of
 * those cases happen rather than to look like a document.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_BLOCK = "#root { display:block; background:#101214; padding:4px; }"
                                       ".sec { display:block; margin:8px 0px; }"
                                       ".p { display:block; height:14px; margin:6px 0px;"
                                       "     background:#2a3442; }";

static void init_block(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_BLOCK);
}

static void frame_block_1k(bench_env *e)
{
    int i, j;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 100; ++i)
    {
        ar_begin(e->ui, "div.sec");
        for (j = 0; j < 9; ++j)
        {
            ar_begin(e->ui, "div.p");
            ar_end(e->ui);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_BLOCK_1K = {
    "block_1k", "layout",      "1000 blocks whose margins collapse on every edge", 800, 600, 1,
    init_block, frame_block_1k};

/* ------------------------------------------------------------------------
 * Inline formatting, wrapping and fragmentation
 *
 * A line box takes its height from the deepest ascent and the deepest descent
 * on the line rather than from the tallest box, so two font sizes on one line
 * is what exercises it. The runs are wider than their box on purpose: a run
 * the breaker has to cut becomes one rectangle per line it touches, and that
 * is the path with the most arithmetic in it.
 *
 * Box count is low and cost is high, which is the point -- this measures text
 * and line breaking, where the scenes above measure the solver.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_INLINE =
    "#root { display:block; width:600px; background:#101214; padding:8px; }"
    ".para { display:block; width:280px; margin:6px 0px; }"
    ".t { display:inline; font-size:8px; color:#c8d0d8; }"
    ".big { display:inline; font-size:16px; color:#ffffff; }";

static void init_inline(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_INLINE);
}

static void frame_inline_wrap(bench_env *e)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 60; ++i)
    {
        ar_begin(e->ui, "div.para");
        ar_text(e->ui, "div.t", "the quick brown fox jumps over the lazy dog and keeps going");
        ar_text(e->ui, "div.big", "headline");
        ar_text(e->ui, "div.t", "and a second run that also has to be broken across lines");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_INLINE_WRAP = {"inline_wrap",
                                           "layout",
                                           "60 paragraphs of mixed sizes, wrapped and fragmented",
                                           800,
                                           600,
                                           1,
                                           init_inline,
                                           frame_inline_wrap};

/* ------------------------------------------------------------------------
 * Floats, clear and intrinsic sizing
 *
 * A float shortens the line boxes beside it and not the block boxes, so the
 * text is what has to notice it. `clear` then has to find the bottom of
 * everything on that side, and fit-content has to ask a box how wide it would
 * like to be before anything has told it how wide it is -- three different
 * questions about the same card.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_FLOAT =
    "#root { display:block; width:600px; background:#101214; padding:8px; }"
    ".card { display:block; margin:4px 0px; }"
    ".thumb { float:left; width:48px; height:48px; margin:0px 6px 6px 0px;"
    "         background:#3a4a5a; }"
    ".cap { display:inline; font-size:8px; color:#c8d0d8; }"
    ".tag { display:block; width:fit-content; font-size:8px; color:#9fb0c0;"
    "       background:#22303c; }"
    ".clear { display:block; clear:both; height:0px; }";

static void init_float(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_FLOAT);
}

static void frame_float_gallery(bench_env *e)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 80; ++i)
    {
        ar_begin(e->ui, "div.card");
        ar_begin(e->ui, "div.thumb");
        ar_end(e->ui);
        ar_text(e->ui, "div.cap", "a caption long enough to wrap beside the thumbnail twice over");
        ar_text(e->ui, "div.tag", "shrink to fit");
        ar_begin(e->ui, "div.clear");
        ar_end(e->ui);
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_FLOAT_GALLERY = {
    "float_gallery",
    "layout",
    "80 cards: a float, text beside it, clear, fit-content",
    800,
    600,
    1,
    init_float,
    frame_float_gallery};

/* ------------------------------------------------------------------------
 * A real scroll container
 *
 * scroll_10k in sc_realistic.c simulates scrolling the way an application
 * does, by rebuilding the rows it wants visible. This one uses 0.6.0's
 * container instead: the whole list is declared, overflow clips it, and the
 * position moves underneath. Both are worth having -- they are different
 * things an application can do, and only one of them existed to measure.
 *
 * This is also the scene 0.6.1's region move had to beat, and now the one that
 * shows it working: the retained pixels are blitted and only the newly exposed
 * strip is rasterized. Read it as fill_px rather than dirty_ratio -- a scroll
 * moves every pixel of the container, so all of them must still be presented
 * and the ratio stays at 1.000 however well the move works.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_SCROLLER =
    "#root { display:block; overflow:scroll; height:600px; background:#ffffff; }"
    ".item { display:block; height:36px; margin:2px 0px; padding:4px 8px;"
    "        background:#fafafa; }"
    ".label { display:inline; font-size:8px; color:#2b2b2b; }";

static void init_scroller(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_SCROLLER);
}

static void frame_scroll_container(bench_env *e)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 300; ++i)
    {
        ar_begin(e->ui, "div.item");
        ar_text(e->ui, "div.label", "a list row with enough text on it to measure");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);

    /* Boxes are numbered in declaration order, so the container is 0. Moving
       it after the frame rather than before is deliberate: the position is
       read by the next layout, which is exactly what a wheel notch does. */
    ar_node_scroll_to(e->ui, 0, (ar_i32)((e->frame * 7u) % 9000u));
    ar_frame_presented(e->ui);
}

static const bench_scene SC_SCROLL_CONTAINER = {
    "scroll_container",
    "layout",
    "300 rows in an overflow:scroll box, moved 7 px a frame",
    800,
    600,
    1,
    init_scroller,
    frame_scroll_container};

/* ------------------------------------------------------------------------
 * Twenty sticky headers in one scroll container, moved every frame.
 *
 * 0.6.2 budgets sticky at under 0.1 ms on the tier for twenty elements, on the
 * grounds that it costs one comparison and at most one offset each per scroll
 * event. This is the scene that number has to come from, and it is built to be
 * the awkward case rather than the flattering one: every section is short
 * enough that its header is pinned, unpinned or handing over on most frames,
 * so the containing-block clamp is exercised rather than skipped.
 *
 * The comparison against no sticky at all is the useful reading, which is why
 * the same tree without the property is the scene beside it.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_STICKY =
    "#root  { display:block; overflow:scroll; height:600px; background:#ffffff; }"
    ".sect  { display:block; height:120px; background:#f7f7f7; }"
    ".head  { display:block; height:24px; position:sticky; top:0px;"
    "         background:#e8eef4; }"
    ".row   { display:block; height:28px; margin:2px 0px; background:#fafafa; }";

static const char *const SHEET_STICKY_OFF =
    "#root  { display:block; overflow:scroll; height:600px; background:#ffffff; }"
    ".sect  { display:block; height:120px; background:#f7f7f7; }"
    ".head  { display:block; height:24px; background:#e8eef4; }"
    ".row   { display:block; height:28px; margin:2px 0px; background:#fafafa; }";

static void init_sticky(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_STICKY);
}

static void init_sticky_off(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_STICKY_OFF);
}

static void frame_sticky(bench_env *e)
{
    int i, j;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 20; ++i)
    {
        ar_begin(e->ui, "div.sect");
        ar_begin(e->ui, "div.head");
        ar_end(e->ui);
        for (j = 0; j < 3; ++j)
        {
            ar_begin(e->ui, "div.row");
            ar_end(e->ui);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);

    ar_node_scroll_to(e->ui, 0, (ar_i32)((e->frame * 7u) % 1800u));
    ar_frame_presented(e->ui);
}

static const bench_scene SC_STICKY_20 = {
    "sticky_20", "layout",    "20 sticky headers in a scroll container, moved 7 px a frame",
    800,         600,         1,
    init_sticky, frame_sticky};

static const bench_scene SC_STICKY_20_OFF = {
    "sticky_20_off",
    "layout",
    "the same twenty sections with no position:sticky, to subtract",
    800,
    600,
    1,
    init_sticky_off,
    frame_sticky};

/* ------------------------------------------------------------------------
 * 0.6.3's two budgets: a modal over a deep tree, and twenty anchored boxes.
 *
 * The coverage document puts the top layer under 0.1 ms on the tier and anchor
 * resolution under 0.2 ms, and both were assertions until there was a scene to
 * take them from. Each has an *_off twin with the property removed, because a
 * whole-frame figure says nothing about what a feature costs.
 *
 * The modal scene is the one to read carefully. Its backdrop is a full-viewport
 * fill, which is the dominant cost and is *supposed* to be: the coverage
 * document says so up front -- 4.9 ms on the tier at 640x480, paid on the
 * frame the modal opens and never again, because nothing changes afterwards
 * and damage tracking has nothing to present. The scene declares it every
 * frame, so what it measures is that worst frame repeatedly.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_LAYER_BENCH =
    "#root { display:block; background:#ffffff; }"
    ".sect { display:block; height:60px; background:#f7f7f7; }"
    ".cell { display:block; height:14px; margin:1px 0px; background:#fafafa; }"
    ".dlg  { display:block; position:absolute; top:120px; left:160px; width:320px;"
    "        height:200px; background:#ffffff; overlay:modal; }"
    ".dlg::backdrop { background:#00000060; }";

static const char *const SHEET_LAYER_BENCH_OFF =
    "#root { display:block; background:#ffffff; }"
    ".sect { display:block; height:60px; background:#f7f7f7; }"
    ".cell { display:block; height:14px; margin:1px 0px; background:#fafafa; }"
    ".dlg  { display:block; position:absolute; top:120px; left:160px; width:320px;"
    "        height:200px; background:#ffffff; }";

static const char *const SHEET_ANCHOR_BENCH =
    "#root { display:block; background:#ffffff; }"
    ".row  { display:block; height:26px; position:relative; background:#f7f7f7; }"
    ".t    { display:block; position:absolute; top:3px; left:40px; width:80px;"
    "        height:18px; background:#e8eef4; anchor-name: --t; }"
    ".tip  { display:block; position:absolute; position-anchor: --t;"
    "        top: anchor(bottom); left: anchor(center); width:90px; height:14px;"
    "        background:#eef2f4; }";

static const char *const SHEET_ANCHOR_BENCH_OFF =
    "#root { display:block; background:#ffffff; }"
    ".row  { display:block; height:26px; position:relative; background:#f7f7f7; }"
    ".t    { display:block; position:absolute; top:3px; left:40px; width:80px;"
    "        height:18px; background:#e8eef4; }"
    ".tip  { display:block; position:absolute; top:21px; left:80px;"
    "        width:90px; height:14px; background:#eef2f4; }";

static void init_layer(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_LAYER_BENCH);
}

static void init_layer_off(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_LAYER_BENCH_OFF);
}

static void frame_layer(bench_env *e)
{
    int i, j;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 12; ++i)
    {
        ar_begin(e->ui, "div.sect");
        for (j = 0; j < 3; ++j)
        {
            ar_begin(e->ui, "div.cell");
            ar_end(e->ui);
        }
        ar_end(e->ui);
    }
    ar_begin(e->ui, "div.dlg");
    ar_end(e->ui);
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void init_anchored(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_ANCHOR_BENCH);
}

static void init_anchored_off(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_ANCHOR_BENCH_OFF);
}

static void frame_anchored(bench_env *e)
{
    int i;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 20; ++i)
    {
        ar_begin(e->ui, "div.row");
        ar_begin(e->ui, "div.t");
        ar_end(e->ui);
        ar_begin(e->ui, "div.tip");
        ar_end(e->ui);
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_TOP_LAYER = {
    "top_layer", "layout",   "a modal with a full-viewport backdrop over a 48 box tree",
    800,         600,        1,
    init_layer,  frame_layer};

static const bench_scene SC_TOP_LAYER_OFF = {
    "top_layer_off",
    "layout",
    "the same tree and dialog with no top layer, to subtract",
    800,
    600,
    1,
    init_layer_off,
    frame_layer};

static const bench_scene SC_ANCHORED_20 = {
    "anchored_20", "layout",      "20 tooltips placed by anchor() against 20 anchors", 800, 600, 1,
    init_anchored, frame_anchored};

static const bench_scene SC_ANCHORED_20_OFF = {
    "anchored_20_off",
    "layout",
    "the same twenty pairs at literal offsets, to subtract",
    800,
    600,
    1,
    init_anchored_off,
    frame_anchored};

/* ------------------------------------------------------------------------
 * Real tables, at three sizes and in both layout modes.
 *
 * `table_1k_rows` in sc_realistic.c is deliberately left alone: it is a flex
 * approximation of a table, and its comment says it exists to be the "before"
 * number for content-visibility at 0.15.1. Re-pointing it at real tables would
 * have read the criterion literally and destroyed the comparison it was put
 * there to make. These are new scenes beside it.
 *
 * The three sizes are the gate for criterion 5: automatic layout must not be
 * quadratic in row count, and 100 -> 1,000 -> 10,000 rows must grow no worse
 * than 1.4x linear. That is a property no correctness test can see -- a
 * quadratic implementation passes every one of them -- so it has to be a
 * measurement, and it has to exist before there is any temptation to make the
 * column loops ask a cell to measure itself.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_TABLE_AUTO =
    "#root { display:block; background:#ffffff; }"
    ".tbl { display:table; width:760px; }"
    ".trow { display:table-row; }"
    ".cell { display:table-cell; height:16px; padding:1px 4px; background:#fafafa; }";

static const char *const SHEET_TABLE_FIXED =
    "#root { display:block; background:#ffffff; }"
    ".tbl { display:table; width:760px; table-layout:fixed; }"
    ".trow { display:table-row; }"
    ".cell { display:table-cell; height:16px; padding:1px 4px; background:#fafafa; }";

static ar_i32 g_table_rows = 100;

static void init_table_auto(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_TABLE_AUTO);
}

static void init_table_fixed(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_TABLE_FIXED);
}

static void frame_table_grid(bench_env *e)
{
    ar_i32 r, c;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    ar_begin(e->ui, "div.tbl");
    for (r = 0; r < g_table_rows; ++r)
    {
        ar_begin(e->ui, "div.trow");
        for (c = 0; c < 4; ++c)
        {
            ar_begin(e->ui, "div.cell");
            ar_end(e->ui);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void frame_table_100(bench_env *e)
{
    g_table_rows = 100;
    frame_table_grid(e);
}

static void frame_table_1k(bench_env *e)
{
    g_table_rows = 1000;
    frame_table_grid(e);
}

static void frame_table_10k(bench_env *e)
{
    g_table_rows = 10000;
    frame_table_grid(e);
}

static const bench_scene SC_TABLE_AUTO_100 = {
    "table_auto_100", "layout",       "100 rows x 4 cells, automatic table layout", 800, 600, 1,
    init_table_auto,  frame_table_100};

static const bench_scene SC_TABLE_AUTO_1K = {"table_auto_1k",
                                             "layout",
                                             "1000 rows x 4 cells, automatic -- ten times the rows",
                                             800,
                                             600,
                                             1,
                                             init_table_auto,
                                             frame_table_1k};

static const bench_scene SC_TABLE_AUTO_10K = {"table_auto_10k",
                                              "layout",
                                              "10000 rows x 4 cells, automatic -- a hundred times",
                                              800,
                                              600,
                                              1,
                                              init_table_auto,
                                              frame_table_10k};

/*
 * Five hundred flex items, the size the Pentium II budget names.
 *
 * Every item carries a grow factor, a shrink factor and a basis, so the
 * resolution loop has something to resolve rather than freezing everything on
 * its first pass -- a scene of items that cannot move measures the walk and
 * not the algorithm. Two of the five hundred carry a maximum, which is what
 * makes the loop run a second time at all.
 */
static const char *SHEET_FLEX_500 = "#root { display:block; background:#ffffff; }"
                                    ".frow { display:flex; flex-direction:row; width:900px;"
                                    "        height:40px; gap:2px; }"
                                    ".fwrap { flex-wrap:wrap; height:400px; }"
                                    ".fi { flex-grow:1; flex-shrink:1; flex-basis:40px;"
                                    "      height:18px; }"
                                    ".cap { max-width:24px; }";

static void init_flex_500(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_FLEX_500);
}

static ar_i32 g_flex_wrap = 0;

static void frame_flex_500(bench_env *e)
{
    ar_i32 k;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    ar_begin(e->ui, g_flex_wrap ? "div.frow.fwrap" : "div.frow");
    for (k = 0; k < 500; ++k)
    {
        ar_begin(e->ui, (k % 250) == 0 ? "div.fi.cap" : "div.fi");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void frame_flex_line(bench_env *e)
{
    g_flex_wrap = 0;
    frame_flex_500(e);
}

static void frame_flex_wrapped(bench_env *e)
{
    g_flex_wrap = 1;
    frame_flex_500(e);
}

static const bench_scene SC_FLEX_500 = {"flex_500",
                                        "layout",
                                        "500 flex items on one line, with factors and a maximum",
                                        1000,
                                        600,
                                        1,
                                        init_flex_500,
                                        frame_flex_line};

static const bench_scene SC_FLEX_500_WRAP = {
    "flex_500_wrap", "layout",          "the same 500 wrapped over many lines", 1000, 600, 1,
    init_flex_500,   frame_flex_wrapped};

/*
 * Grid, at two sizes, for the track-sizing gate.
 *
 * The criterion asks for 100 x 100 against 50 x 50, and 100 columns is past
 * AR_GRID_MAX -- the track arrays are bounded and on the stack, the same
 * bargain the table's columns and the float list make. 20 x 20 against 40 x 40
 * is the same question inside the bound: four times the items and four times
 * the cells, so anything quadratic in track count shows up as sixteen.
 *
 * Every row is `1fr` so the flexible distribution runs rather than being
 * skipped, which is the part of the algorithm worth timing.
 */
static const char *SHEET_GRID = "#root { display:block; background:#ffffff; }"
                                ".gr { display:grid; width:800px;"
                                "      grid-template-columns: repeat(20, 1fr); }"
                                ".gr40 { grid-template-columns: repeat(40, 1fr); }"
                                ".gc { height:12px; }";

static ar_i32 g_grid_side = 20;

static void init_grid(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_GRID);
}

static void frame_grid(bench_env *e)
{
    ar_i32 k, n = g_grid_side * g_grid_side;

    begin_frame(e);
    ar_begin(e->ui, "div#root");
    ar_begin(e->ui, g_grid_side == 20 ? "div.gr" : "div.gr.gr40");
    for (k = 0; k < n; ++k)
    {
        ar_begin(e->ui, "div.gc");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void frame_grid_20(bench_env *e)
{
    g_grid_side = 20;
    frame_grid(e);
}

static void frame_grid_40(bench_env *e)
{
    g_grid_side = 40;
    frame_grid(e);
}

static const bench_scene SC_GRID_20 = {
    "grid_20x20", "layout",     "400 items in a 20 column grid of fr tracks", 900, 700, 1,
    init_grid,    frame_grid_20};

static const bench_scene SC_GRID_40 = {
    "grid_40x40", "layout",     "1600 items in a 40 column grid -- four times the cells",
    900,          700,          1,
    init_grid,    frame_grid_40};

static const bench_scene SC_TABLE_FIXED_1K = {"table_fixed_1k",
                                              "layout",
                                              "1000 rows x 4 cells, table-layout:fixed, to compare",
                                              800,
                                              600,
                                              1,
                                              init_table_fixed,
                                              frame_table_1k};

void bench_register_layout(void)
{
    bench_register(&SC_FLAT_100);
    bench_register(&SC_FLAT_1K);
    bench_register(&SC_FLAT_8K);
    bench_register(&SC_DEEP_60);
    bench_register(&SC_MIXED);
    bench_register(&SC_BLOCK_1K);
    bench_register(&SC_INLINE_WRAP);
    bench_register(&SC_FLOAT_GALLERY);
    bench_register(&SC_SCROLL_CONTAINER);
    bench_register(&SC_STICKY_20);
    bench_register(&SC_STICKY_20_OFF);
    bench_register(&SC_TOP_LAYER);
    bench_register(&SC_TOP_LAYER_OFF);
    bench_register(&SC_ANCHORED_20);
    bench_register(&SC_ANCHORED_20_OFF);
    bench_register(&SC_TABLE_AUTO_100);
    bench_register(&SC_TABLE_AUTO_1K);
    bench_register(&SC_TABLE_AUTO_10K);
    bench_register(&SC_TABLE_FIXED_1K);
    bench_register(&SC_FLEX_500);
    bench_register(&SC_FLEX_500_WRAP);
    bench_register(&SC_GRID_20);
    bench_register(&SC_GRID_40);
}
