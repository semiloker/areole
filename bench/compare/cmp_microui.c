/*
 * areole against microui.
 * SPDX-License-Identifier: MIT
 *
 * microui is the library areole is most often going to be compared to by
 * anyone building for a constrained machine: about 1,100 lines, immediate
 * mode, fixed pools, no allocation. Same values, different design.
 *
 * The difference that matters for this measurement is that microui does not
 * build a tree. mu_layout_next() advances a row cursor and returns a rectangle,
 * and that is the whole of its layout. There is no second pass, because there
 * is nothing that could need one: no box's position depends on a sibling that
 * has not been placed yet.
 *
 * areole builds a retained tree, resolves a stylesheet against every node, and
 * runs two passes per axis so that grow and shrink can be solved.
 *
 * So this comparison is not "who is faster at the same job". It is the price of
 * the abstraction, and that is a number worth publishing precisely because it
 * is not flattering:
 *
 *   microui        what a stateless row allocator costs
 *   areole layout  what flexbox costs
 *   areole total   what flexbox plus a stylesheet costs
 *
 * A row allocator cannot express a centred column of grow-weighted cards, which
 * is why areole is not one. But the gap is the budget being spent on that, and
 * if it ever stops being worth it, this case is where it will show.
 *
 * Nothing is painted on either side. microui's command list is 256 KB and the
 * large case would overflow it, and overflowing it aborts rather than degrades;
 * layout is what is being compared here in any case.
 */
#include "microui.h"

#include "compare.h"

#include <stdlib.h>
#include <string.h>

static mu_Context   *g_mu;
static ar_ctx       *g_ui;
static unsigned char *g_ui_mem;

/* The rects have to be consumed or -O2 deletes the loop that produces them. */
static volatile int g_sink;

/* microui asks the host to measure text. Nothing here draws any, but the
   callbacks must exist or mu_begin_window trips over a null pointer. */
static int text_width(mu_Font font, const char *str, int len)
{
    (void)font;
    (void)str;
    return len * 6;
}

static int text_height(mu_Font font)
{
    (void)font;
    return 12;
}

/* ------------------------------------------------------------------------
 * The same shape in both: rows of sixteen equal cells, twelve pixels tall.
 * ------------------------------------------------------------------------ */
#define COLS   16
#define CELL_W 64
#define CELL_H 12

/* Both sides must produce the same rectangles or the comparison is between two
   different pieces of work. microui's -1 width means "whatever is left of the
   row", not "an equal share": with sixteen of them the first cell takes the
   entire row and the other fifteen come back four pixels wide. Fixed widths on
   both sides, spacing and padding zeroed to match gap:0, and the grids agree
   cell for cell -- verified by printing them, not by assuming. */

static void microui_tree(int boxes)
{
    static const int widths[COLS] = {CELL_W, CELL_W, CELL_W, CELL_W, CELL_W, CELL_W,
                                     CELL_W, CELL_W, CELL_W, CELL_W, CELL_W, CELL_W,
                                     CELL_W, CELL_W, CELL_W, CELL_W};
    int              i, acc = 0;

    mu_begin(g_mu);
    if (mu_begin_window(g_mu, "bench", mu_rect(0, 0, 1024, 768)))
    {
        for (i = 0; i < boxes; ++i)
        {
            mu_Rect r;
            if (i % COLS == 0)
            {
                mu_layout_row(g_mu, COLS, widths, CELL_H);
            }
            r = mu_layout_next(g_mu);
            acc += r.x + r.y + r.w + r.h;
        }
        mu_end_window(g_mu);
    }
    mu_end(g_mu);
    g_sink = acc;
}

static const char *const SHEET_A = ".row  { display:flex; flex-direction:row; gap:0px; }"
                                   ".leaf { width:64px; height:12px; }";
static const char *const SHEET_B = "#root { display:flex; flex-direction:column; gap:0px; }";

static void areole_tree(int boxes)
{
    ar_input   in;
    ar_surface none;
    int        i;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    none.pixels = 0;
    none.w = 0;
    none.h = 0;
    none.stride = 1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "div#root");
    for (i = 0; i < boxes; ++i)
    {
        if (i % COLS == 0)
        {
            if (i)
            {
                ar_end(g_ui);
            }
            ar_begin(g_ui, "div.row");
        }
        ar_begin(g_ui, "div.leaf");
        ar_end(g_ui);
    }
    if (boxes)
    {
        ar_end(g_ui);
    }
    ar_end(g_ui);
    ar_frame_end(g_ui, &none);
    ar_frame_presented(g_ui);
}

/* ------------------------------------------------------------------------ */
#define BOXES_SMALL 1000
#define BOXES_LARGE 8000

static void a_1k(cmp_ctx *c)
{
    (void)c;
    areole_tree(BOXES_SMALL);
}
static void m_1k(cmp_ctx *c)
{
    (void)c;
    microui_tree(BOXES_SMALL);
}
static void a_8k(cmp_ctx *c)
{
    (void)c;
    areole_tree(BOXES_LARGE);
}
static void m_8k(cmp_ctx *c)
{
    (void)c;
    microui_tree(BOXES_LARGE);
}

static int microui_setup(cmp_ctx *c)
{
    g_mu = (mu_Context *)malloc(sizeof(mu_Context));
    if (!g_mu)
    {
        return 0;
    }
    mu_init(g_mu);
    g_mu->text_width = text_width;
    g_mu->text_height = text_height;
    g_mu->style->spacing = 0;
    g_mu->style->padding = 0;

    g_ui_mem = (unsigned char *)malloc(AR_MEM(16384));
    if (!g_ui_mem)
    {
        return 0;
    }
    g_ui = ar_init(g_ui_mem, AR_MEM(16384));
    if (!g_ui)
    {
        return 0;
    }
    ar_set_clock(g_ui, bench_time_us);
    ar_stylesheet(g_ui, SHEET_A);
    ar_stylesheet(g_ui, SHEET_B);

    c->surface.pixels = 0;
    c->surface.w = 0;
    c->surface.h = 0;
    c->surface.stride = 1;
    c->native = 0;
    return 1;
}

static void microui_teardown(cmp_ctx *c)
{
    (void)c;
    free(g_mu);
    free(g_ui_mem);
    g_mu = 0;
    g_ui_mem = 0;
    g_ui = 0;
}

static double areole_layout_us(void)
{
    return g_ui ? (double)ar_perf_percentile(ar_perf_of(g_ui), AR_PHASE_LAYOUT, 50) : 0.0;
}

static const char *const CAVEAT =
    "microui does not build a tree and does not resolve style: mu_layout_next advances a row "
    "cursor and returns a rectangle. areole runs two passes per axis so grow and shrink can be "
    "solved, over a retained tree, after matching a stylesheet. This measures the price of that "
    "abstraction, not a like-for-like race, and a ratio below 1.00 is expected.";

static const cmp_case CASES[] = {
    {"layout_1k", "1000 cells in rows of sixteen, no painting", CAVEAT, a_1k, m_1k, "ar layout",
     areole_layout_us},
    {"layout_8k", "8000 cells in rows of sixteen, no painting", CAVEAT, a_8k, m_8k, "ar layout",
     areole_layout_us}};

static const cmp_case *microui_cases(int *count)
{
    *count = (int)(sizeof CASES / sizeof CASES[0]);
    return CASES;
}

static const cmp_engine MICROUI = {"microui", "2.02, vendored under third_party/bench/",
                                   microui_setup, microui_teardown, microui_cases};

const cmp_engine *cmp_engine_microui(void)
{
    return &MICROUI;
}
