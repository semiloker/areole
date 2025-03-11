/*
 * areole benchmark - style resolution scenes.
 * SPDX-License-Identifier: MIT
 *
 * The baseline found style resolution costing twice what layout costs, with a
 * stylesheet of six rules. Resolution scans every rule for every box, so the
 * cost is linear in rule count, and these scenes exist to make that curve
 * visible before 0.4.0 tries to flatten it.
 *
 * A note that is itself a finding: the rule table is a fixed 256 entries
 * (AR_MAX_RULES in src/ar_ctx.c). A browser user-agent stylesheet alone is
 * around 400 rules, so the ceiling has to become a function of the caller's
 * arena rather than a constant. That is 0.4.0's problem; 0.1.1 only measures,
 * so the largest scene here stays under the limit and the runner reports any
 * stylesheet that was truncated.
 */
#include "../bench.h"

#include <stdio.h>
#include <string.h>

#define STYLE_BUF 48000

static char g_sheet[STYLE_BUF];

/* Builds a stylesheet of n rules, each a distinct class, so no two boxes share
   a match and the scan cannot be short-circuited by luck. */
static void build_sheet(int n)
{
    char *p = g_sheet;
    char *end = g_sheet + STYLE_BUF - 128;
    int   i;

    p += sprintf(p, ".leaf { width:grow; height:12px; background:#3a4a5a; }"
                    ".row { display:flex; flex-direction:row; gap:2px; }"
                    "#root { display:flex; flex-direction:column; }");

    for (i = 0; i < n && p < end; ++i)
    {
        p += sprintf(p, ".f%d { padding:%dpx; color:#%02x%02x%02x; }", i, i % 7, i & 0xFF,
                     (i * 3) & 0xFF, (i * 7) & 0xFF);
    }
    *p = 0;
}

static void begin_frame(bench_env *e, ar_i32 mx, ar_i32 my)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = mx;
    in.mouse_y = my;
    in.mouse_inside = (mx >= 0);
    ar_frame_begin(e->ui, &in);
}

/* Five hundred boxes, each matching exactly two rules, over a sheet whose size
   is what varies. */
static void tree_500(bench_env *e)
{
    int i;

    ar_begin(e->ui, "div#root");
    for (i = 0; i < 500; ++i)
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
    ar_end(e->ui);
    ar_end(e->ui);
}

static void frame_rules(bench_env *e)
{
    begin_frame(e, -1, -1);
    tree_500(e);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static void init_10(bench_env *e)
{
    build_sheet(10);
    ar_stylesheet(e->ui, g_sheet);
}

static void init_100(bench_env *e)
{
    build_sheet(100);
    ar_stylesheet(e->ui, g_sheet);
}

static void init_250(bench_env *e)
{
    /* Deliberately under the 256 rule ceiling. Raising the ceiling is 0.4.0. */
    build_sheet(250);
    ar_stylesheet(e->ui, g_sheet);
}

static const bench_scene SC_RULES_10 = {
    "rules_10", "style", "500 boxes against a 13 rule sheet", 800, 600, 1, init_10, frame_rules};
static const bench_scene SC_RULES_100 = {
    "rules_100", "style",    "the same boxes against 103 rules: the curve, not a point",
    800,         600,        1,
    init_100,    frame_rules};
static const bench_scene SC_RULES_250 = {
    "rules_250", "style",    "253 rules, just under the fixed table ceiling", 800, 600, 1,
    init_250,    frame_rules};

/* ------------------------------------------------------------------------
 * State churn
 *
 * The cursor moves every frame, so a different box is hovered every frame and
 * its style must be resolved against a different state. This is what a user
 * moving a mouse actually costs, and it is the case a style cache has to get
 * right rather than the static one.
 * ------------------------------------------------------------------------ */
static void init_churn(bench_env *e)
{
    build_sheet(60);
    ar_stylesheet(e->ui, g_sheet);
    ar_stylesheet(e->ui, ".leaf:hover { background:#5a6a7a; }"
                         ".leaf:active { background:#7a8a9a; }");
}

static void frame_churn(bench_env *e)
{
    ar_i32 mx = (ar_i32)((e->frame * 13u) % 780u) + 4;
    ar_i32 my = (ar_i32)((e->frame * 7u) % 580u) + 4;

    begin_frame(e, mx, my);
    tree_500(e);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_CHURN = {
    "state_churn", "style",    "the hovered box changes every frame: what moving a mouse costs",
    800,           600,        1,
    init_churn,    frame_churn};

/* ------------------------------------------------------------------------
 * Identical siblings
 *
 * A thousand rows with the same class and the same state. Today every one is
 * resolved from scratch. After 0.4.0's computed style sharing, one should be
 * resolved and 999 should be a pointer assignment, and the difference between
 * this scene and rules_100 is exactly the size of that win.
 * ------------------------------------------------------------------------ */
static void frame_siblings(bench_env *e)
{
    int i;

    begin_frame(e, -1, -1);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 1000; ++i)
    {
        ar_begin(e->ui, "div.leaf");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_SIBLINGS = {
    "identical_siblings",
    "style",
    "1000 boxes with one class: the case style sharing exists for",
    800,
    600,
    1,
    init_100,
    frame_siblings};

void bench_register_style(void)
{
    bench_register(&SC_RULES_10);
    bench_register(&SC_RULES_100);
    bench_register(&SC_RULES_250);
    bench_register(&SC_CHURN);
    bench_register(&SC_SIBLINGS);
}
