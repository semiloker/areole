/*
 * areole benchmark - realistic scenes.
 * SPDX-License-Identifier: MIT
 *
 * What a user will actually feel. The primitive scenes say what a pixel costs;
 * these say whether an interface is usable, and they are the ones the Pentium
 * II budgets in hardware-tiers.md are stated against.
 *
 * Deliberately built the way an application would build them, not the way that
 * benchmarks fastest.
 */
#include "../bench.h"

#include <stdio.h>
#include <string.h>

static const char *const SHEET_APP =
    "#app { display:flex; flex-direction:row; background:#fefbf2; }"
    ".rail { width:220px; display:flex; flex-direction:column; padding:16px;"
    "        gap:2px; background:#faf6ed; }"
    ".brand { font-size:24px; color:#2b2b2b; }"
    ".nav { padding:9px 12px; font-size:16px; color:#8a8175; }";

static const char *const SHEET_PAGE =
    ".nav:hover { background:#f0e9db; color:#2b2b2b; }"
    ".page { width:grow; display:flex; flex-direction:column; padding:24px; gap:4px; }"
    ".h1 { font-size:32px; color:#2b2b2b; }"
    ".sub { font-size:8px; color:#8a8175; padding-bottom:16px; }";

static const char *const SHEET_CARD =
    ".row { display:flex; flex-direction:row; gap:16px; padding-bottom:16px;"
    "       align-items:flex-start; }"
    ".card { width:grow; display:flex; flex-direction:column;"
    "        background:#f8f3e9; border:1px solid #e8dfcc; }"
    ".card:hover { background:#f2ebdd; }"
    ".accent { height:4px; background:#c2703d; }";

static const char *const SHEET_TEXT =
    ".body { display:flex; flex-direction:column; padding:14px; gap:8px; }"
    ".name { font-size:16px; color:#2b2b2b; }"
    ".price { font-size:16px; color:#c2703d; }"
    ".in { font-size:8px; color:#4f7a4a; }";

static void app_sheets(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_APP);
    ar_stylesheet(e->ui, SHEET_PAGE);
    ar_stylesheet(e->ui, SHEET_CARD);
    ar_stylesheet(e->ui, SHEET_TEXT);
}

static void begin(bench_env *e, ar_i32 mx, ar_i32 my)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = mx;
    in.mouse_y = my;
    in.mouse_inside = (mx >= 0);
    ar_frame_begin(e->ui, &in);
}

/* ------------------------------------------------------------------------
 * Dashboard
 *
 * The example that ships with the library, which is the point: the number this
 * scene produces is the number a reader can reproduce by running the example.
 * ------------------------------------------------------------------------ */
static const char *const NAV[5] = {"Home", "Products", "Customers", "Orders", "Settings"};
static const char *const NAMES[6] = {"Tulip", "Rose", "Peony", "Lily", "Iris", "Dahlia"};
static const char *const PRICES[6] = {"$4.20", "$6.00", "$9.50", "$5.75", "$3.90", "$7.25"};

static void card(bench_env *e, int i)
{
    ar_begin(e->ui, "div.card");
    ar_begin(e->ui, "div.accent");
    ar_end(e->ui);
    ar_begin(e->ui, "div.body");
    ar_text(e->ui, "div.name", NAMES[i]);
    ar_text(e->ui, "div.price", PRICES[i]);
    ar_text(e->ui, "div.in", "In stock");
    ar_end(e->ui);
    ar_end(e->ui);
}

static void frame_dashboard(bench_env *e)
{
    int i, row;

    /* The cursor drifts, so hover state changes and the frame is not the same
       frame every time. A benchmark whose input never changes measures a
       cache, not an interface. */
    begin(e, (ar_i32)((e->frame * 11u) % 900u) + 40, (ar_i32)((e->frame * 5u) % 500u) + 40);

    ar_begin(e->ui, "div#app");
    ar_begin(e->ui, "div.rail");
    ar_text(e->ui, "div.brand", "areole");
    for (i = 0; i < 5; ++i)
    {
        ar_text(e->ui, "div.nav", NAV[i]);
    }
    ar_end(e->ui);

    ar_begin(e->ui, "div.page");
    ar_text(e->ui, "div.h1", "Products");
    ar_text(e->ui, "div.sub", "Laid out from a stylesheet.");
    for (row = 0; row < 2; ++row)
    {
        ar_begin(e->ui, "div.row");
        for (i = 0; i < 3; ++i)
        {
            card(e, row * 3 + i);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_end(e->ui);

    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_DASHBOARD = {
    "dashboard",
    "realistic",
    "the shipped example: rail, nav, six cards, drifting cursor",
    1024,
    640,
    1,
    app_sheets,
    frame_dashboard};

/* ------------------------------------------------------------------------
 * A table of a thousand rows
 *
 * Not a table in the CSS sense, which does not exist until 0.7.0, but the box
 * count and the text volume a real one has. This is the scene that
 * content-visibility exists to rescue in 0.15.1, and having the before number
 * is why it is here now.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_TABLE =
    "#root { display:flex; flex-direction:column; background:#ffffff; }"
    ".tr { display:flex; flex-direction:row; gap:0px; height:18px; }"
    ".td { width:grow; padding:2px 6px; font-size:8px; color:#2b2b2b; }"
    ".tr:hover { background:#f0f4f8; }";

static void init_table(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_TABLE);
}

static void frame_table(bench_env *e)
{
    static char cell[24];
    int         r, c;

    begin(e, (ar_i32)((e->frame * 3u) % 700u) + 20, (ar_i32)((e->frame * 17u) % 560u) + 20);
    ar_begin(e->ui, "div#root");
    for (r = 0; r < 1000; ++r)
    {
        ar_begin(e->ui, "div.tr");
        for (c = 0; c < 5; ++c)
        {
            sprintf(cell, "r%dc%d", r, c);
            ar_text(e->ui, "div.td", cell);
        }
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_TABLE = {"table_1k_rows",
                                     "realistic",
                                     "1000 rows of 5 cells: 6000 boxes, most of them off screen",
                                     800,
                                     600,
                                     1,
                                     init_table,
                                     frame_table};

/* ------------------------------------------------------------------------
 * A long scrolled list
 *
 * The case damage tracking cannot help with, because everything moves. This is
 * the number 0.1.2's region move has to improve, and 0.6.1 after it.
 * ------------------------------------------------------------------------ */
static const char *const SHEET_LIST =
    "#root { display:flex; flex-direction:column; background:#ffffff; padding:0px; }"
    ".item { display:flex; flex-direction:row; height:24px; padding:4px 8px;"
    "        gap:8px; background:#fafafa; }"
    ".label { font-size:8px; color:#2b2b2b; }";

static void init_list(bench_env *e)
{
    ar_stylesheet(e->ui, SHEET_LIST);
}

static void frame_scroll(bench_env *e)
{
    static char label[32];
    /* The scroll offset advances every frame, so no two frames are the same
       and nothing can be cached between them. */
    int base = (int)(e->frame % 9000u);
    int i;

    begin(e, -1, -1);
    ar_begin(e->ui, "div#root");
    for (i = 0; i < 40; ++i)
    {
        ar_begin(e->ui, "div.item");
        sprintf(label, "row %d", base + i);
        ar_text(e->ui, "div.label", label);
        ar_text(e->ui, "div.label", "secondary text for the row");
        ar_end(e->ui);
    }
    ar_end(e->ui);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_SCROLL = {
    "scroll_10k",
    "realistic",
    "a 10000 item list scrolled one row per frame; content never repeats",
    800,
    600,
    1,
    init_list,
    frame_scroll};

void bench_register_realistic(void)
{
    bench_register(&SC_DASHBOARD);
    bench_register(&SC_TABLE);
    bench_register(&SC_SCROLL);
}
