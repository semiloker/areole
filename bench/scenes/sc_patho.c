/*
 * areole benchmark - pathological scenes.
 * SPDX-License-Identifier: MIT
 *
 * The cases that break a clever optimisation. Every one of these exists
 * because some future release will make an assumption, and this is where the
 * assumption gets tested rather than shipped.
 *
 * None of them is a realistic interface. That is the point: an optimisation
 * that only holds on realistic input is a bug waiting for an unusual user.
 */
#include "../bench.h"

#include <string.h>

static ar_rect whole(const ar_surface *s)
{
    return ar_rect_make(0, 0, s->w, s->h);
}

/* ------------------------------------------------------------------------
 * Overdraw
 *
 * Ten full-surface fills per frame. Overdraw is the multiplier on the
 * bandwidth budget that decides whether the Pentium II tier is reachable at
 * all, and hardware-tiers.md assumes a factor of two to three. This scene is
 * where that assumption gets a number instead of a guess.
 * ------------------------------------------------------------------------ */
static void frame_overdraw(bench_env *e)
{
    ar_rect clip = whole(&e->surface);
    int     i;

    for (i = 0; i < 10; ++i)
    {
        ar_fill_rect(&e->surface, clip, clip, AR_HEX(0x101010 + (ar_u32)i * 0x101010u));
    }
}

static const bench_scene SC_OVERDRAW = {
    "overdraw_10x",
    "pathological",
    "ten full-surface fills: pure bandwidth, no cleverness helps",
    800,
    600,
    0,
    0,
    frame_overdraw};

/* ------------------------------------------------------------------------
 * Ten thousand tiny boxes
 *
 * Per-call overhead with almost no pixels. The hash grid in 0.1.2 hashes per
 * command, so this is the scene where hashing could plausibly cost more than
 * the drawing it avoids, and 0.1.2 has an acceptance criterion pointing here.
 * ------------------------------------------------------------------------ */
static void frame_tiny(bench_env *e)
{
    ar_rect clip = whole(&e->surface);
    int     i;

    for (i = 0; i < 10000; ++i)
    {
        ar_i32 x = (ar_i32)((ar_u32)(i * 37) % 796u);
        ar_i32 y = (ar_i32)((ar_u32)(i * 53) % 596u);
        ar_fill_rect(&e->surface, ar_rect_make(x, y, 3, 3), clip, AR_HEX(0x336699));
    }
}

static const bench_scene SC_TINY = {"tiny_boxes_10k",
                                    "pathological",
                                    "10000 three pixel fills: all overhead, no pixels",
                                    800,
                                    600,
                                    0,
                                    0,
                                    frame_tiny};

/* ------------------------------------------------------------------------
 * Opposite corners
 *
 * Two single-pixel changes as far apart as the surface allows. A merged
 * bounding rectangle covers the whole window and pays full price; a hash grid
 * touches two cells. 0.1.2 ships both and this scene is how the difference
 * between them is published rather than claimed.
 *
 * Until damage tracking exists it simply measures two pixel writes, which is
 * the correct baseline for the comparison.
 * ------------------------------------------------------------------------ */
static void frame_corners(bench_env *e)
{
    ar_rect clip = whole(&e->surface);
    ar_u32  tint = e->frame & 0xFFu;

    ar_fill_rect(&e->surface, ar_rect_make(0, 0, 1, 1), clip, AR_HEX(0x000000) | (tint << 8));
    ar_fill_rect(&e->surface, ar_rect_make(e->surface.w - 1, e->surface.h - 1, 1, 1), clip,
                 AR_HEX(0x000000) | tint);
}

static const bench_scene SC_CORNERS = {
    "opposite_corners",
    "pathological",
    "two single pixel changes at opposite corners: where merged-bounds damage degenerates",
    800,
    600,
    0,
    0,
    frame_corners};

/* ------------------------------------------------------------------------
 * Arena churn
 *
 * A tree whose size changes wildly between frames, so the frame arena's high
 * water mark moves and nothing about the allocation pattern is stable. The
 * zero-allocation invariant has to survive this, not just a steady scene.
 * ------------------------------------------------------------------------ */
static const char *const SHEET = ".leaf { width:grow; height:8px; background:#3a4a5a; }"
                                 "#root { display:flex; flex-direction:column; }";

static void init_churn(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET);
}

static void frame_arena_churn(bench_env *e)
{
    ar_input in;
    /* Between 10 and 4000 boxes, varying every frame in a pattern that does
       not settle. */
    int n = 10 + (int)((e->frame * 977u) % 3990u);
    int i;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(e->ui, &in);
    if (e->full_repaint)
    {
        ar_invalidate_all(e->ui);
    }
    ar_begin(e->ui, "div#root");
    for (i = 0; i < n; ++i)
    {
        ar_begin(e->ui, "div.leaf");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_ARENA = {"arena_churn",
                                     "pathological",
                                     "tree size swinging between 10 and 4000 boxes every frame",
                                     800,
                                     600,
                                     1,
                                     init_churn,
                                     frame_arena_churn};


/* ------------------------------------------------------------------------
 * Opposite corners, through the tree
 *
 * The scene above draws two pixels directly and never touches the box tree, so
 * it says nothing about damage tracking. It was written before damage tracking
 * existed, as a prediction of where merged bounds would degenerate.
 *
 * This is that prediction made testable. Two boxes sit in opposite corners of a
 * window and one of them changes colour every frame. Individually they are 24
 * by 16 pixels; merged, their bounding rectangle is the entire window, so a
 * single merged region repaints 800 by 600 to update 768 pixels.
 *
 * That is exactly the case the hash grid exists to fix, and whether it is worth
 * building depends on how close real interfaces come to this. Measuring the
 * worst case is how that question gets an answer instead of an opinion.
 * ------------------------------------------------------------------------ */
static const char *const CORNER_SHEET =
    "#root { display:flex; flex-direction:column; justify-content:space-between; }"
    ".bar  { display:flex; flex-direction:row; justify-content:space-between; }"
    ".dot  { width:24px; height:16px; background:#3a4a5a; }"
    ".lit  { width:24px; height:16px; background:#e8c39e; }"
    ".gap  { width:24px; height:16px; }";

static void init_corners_tree(bench_env *e)
{
    ar_stylesheet(e->ui, CORNER_SHEET);
}

static void frame_corners_tree(bench_env *e)
{
    ar_input in;
    int      lit = (int)(e->frame & 1u);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(e->ui, &in);
    if (e->full_repaint)
    {
        ar_invalidate_all(e->ui);
    }

    ar_begin(e->ui, "div#root");

    ar_begin(e->ui, "div.bar");
    ar_begin(e->ui, lit ? "div.lit" : "div.dot");
    ar_end(e->ui);
    ar_begin(e->ui, "div.gap");
    ar_end(e->ui);
    ar_end(e->ui);

    ar_begin(e->ui, "div.bar");
    ar_begin(e->ui, "div.gap");
    ar_end(e->ui);
    ar_begin(e->ui, lit ? "div.dot" : "div.lit");
    ar_end(e->ui);
    ar_end(e->ui);

    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_CORNERS_TREE = {
    "corners_tree",
    "pathological",
    "two boxes in opposite corners, one changing: merged damage degenerates to the whole window",
    800,
    600,
    1,
    init_corners_tree,
    frame_corners_tree};

void bench_register_patho(void)
{
    bench_register(&SC_OVERDRAW);
    bench_register(&SC_TINY);
    bench_register(&SC_CORNERS);
    bench_register(&SC_CORNERS_TREE);
    bench_register(&SC_ARENA);
}
