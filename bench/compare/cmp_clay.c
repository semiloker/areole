/*
 * areole against Clay.
 * SPDX-License-Identifier: MIT
 *
 * The layout head to head. Clay publishes microsecond layout times for 8,192
 * elements and is the closest thing to a direct competitor areole has, so this
 * is the comparison the roadmap named as the number to beat.
 *
 * It is also the comparison that needs the most care to keep honest, because
 * the two libraries do not do the same amount of work.
 *
 *   Clay does layout. It has no style engine: an element's configuration is
 *   passed inline at the call site, already resolved.
 *
 *   areole does style resolution and then layout. Every box is matched against
 *   every rule in a stylesheet, its computed style is built, and only then is
 *   it laid out.
 *
 * So three numbers are reported, not two:
 *
 *   areole total   tree building, style resolution and layout
 *   areole layout  the layout phase alone, from the library's own counters
 *   Clay total     tree building and layout
 *
 * The middle column is the honest head to head. The first is what an
 * application actually pays for using a stylesheet instead of inline
 * configuration, which is a real cost and worth seeing.
 *
 * Compiled as gnu99: Clay uses compound literals and designated initializers
 * throughout, which is precisely why areole could not be built on it.
 */
#define CLAY_IMPLEMENTATION
#include "../../third_party/bench/clay.h"

#include "compare.h"

#include <stdio.h>
#include <string.h>

/* Clay wants a text measurement callback even when nothing measures text. */
static Clay_Dimensions measure_text(Clay_StringSlice text, Clay_TextElementConfig *config,
                                    void *userData)
{
    (void)config;
    (void)userData;
    return (Clay_Dimensions){(float)text.length * 6.0f, 12.0f};
}

static void on_clay_error(Clay_ErrorData e)
{
    fprintf(stderr, "clay: %.*s\n", e.errorText.length, e.errorText.chars);
}

static void      *g_clay_mem;
static ar_ctx    *g_ui;
static unsigned char *g_ui_mem;

/* The same tree in both libraries: one root, one row per sixteen leaves, each
   leaf growing on the inline axis and fixed at twelve pixels on the block
   axis. Written twice, side by side, so the shapes can be compared by eye. */
static void clay_tree(int boxes)
{
    int i, row = 0;

    Clay_BeginLayout();
    CLAY(CLAY_ID("root"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                      .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                      .childGap = 2}})
    {
        for (row = 0; row * 16 < boxes; ++row)
        {
            CLAY(CLAY_IDI("row", row),
                 {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                             .layoutDirection = CLAY_LEFT_TO_RIGHT,
                             .childGap = 2}})
            {
                int n = boxes - row * 16;
                if (n > 16)
                {
                    n = 16;
                }
                for (i = 0; i < n; ++i)
                {
                    CLAY(CLAY_IDI("leaf", row * 16 + i),
                         {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(12)}},
                          .backgroundColor = {58, 74, 90, 255}});
                }
            }
        }
    }
    Clay_EndLayout(0.0f);
}

static const char *const SHEET_A = ".row  { display:flex; flex-direction:row; gap:2px; }"
                                   ".leaf { width:grow; height:12px; background:#3a4a5a; }";
static const char *const SHEET_B = "#root { display:flex; flex-direction:column; gap:2px; }";

static void areole_tree(int boxes)
{
    ar_input in;
    ar_surface none;
    int        i;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    /* A zero sized surface, so nothing is painted and what is measured is tree
       building, style and layout -- the same three things Clay's two, plus
       style. */
    none.pixels = 0;
    none.w = 0;
    none.h = 0;
    none.stride = 1;

    ar_frame_begin(g_ui, &in);
    ar_begin(g_ui, "div#root");
    for (i = 0; i < boxes; ++i)
    {
        if (i % 16 == 0)
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
/* Root, one row per sixteen leaves, and the leaves themselves, with headroom. */
#define CLAY_ELEMENTS 16384

static void a_1k(cmp_ctx *c)
{
    (void)c;
    areole_tree(BOXES_SMALL);
}
static void c_1k(cmp_ctx *c)
{
    (void)c;
    clay_tree(BOXES_SMALL);
}
static void a_8k(cmp_ctx *c)
{
    (void)c;
    areole_tree(BOXES_LARGE);
}
static void c_8k(cmp_ctx *c)
{
    (void)c;
    clay_tree(BOXES_LARGE);
}

static int clay_setup(cmp_ctx *c)
{
    uint32_t   need;
    Clay_Arena arena;

    /* Clay defaults to 8192 elements, and the large case is 8000 leaves plus
       500 rows plus a root. Overflowing the cap does not merely truncate: the
       open/close pairing comes apart and the frame stops being a measurement of
       anything. Raise it first, then size the arena. */
    Clay_SetMaxElementCount(CLAY_ELEMENTS);
    need = Clay_MinMemorySize();
    g_clay_mem = malloc(need);
    if (!g_clay_mem)
    {
        return 0;
    }
    arena = Clay_CreateArenaWithCapacityAndMemory(need, g_clay_mem);
    Clay_Initialize(arena, (Clay_Dimensions){(float)c->w, (float)c->h},
                    (Clay_ErrorHandler){on_clay_error, 0});
    Clay_SetMeasureTextFunction(measure_text, 0);

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
    /* Without a clock the library's own phase timings are all zero, and the
       layout-only column silently disappears rather than being wrong -- which
       is the better failure of the two, but still a failure. */
    ar_set_clock(g_ui, bench_time_us);

    ar_stylesheet(g_ui, SHEET_A);
    ar_stylesheet(g_ui, SHEET_B);

    /* No surface: this comparison never paints. */
    c->surface.pixels = 0;
    c->surface.w = 0;
    c->surface.h = 0;
    c->surface.stride = 1;
    c->native = 0;
    return 1;
}

static void clay_teardown(cmp_ctx *c)
{
    (void)c;
    free(g_clay_mem);
    free(g_ui_mem);
    g_clay_mem = 0;
    g_ui_mem = 0;
    g_ui = 0;
}

static const char *const CAVEAT =
    "areole resolves a stylesheet for every box; Clay takes its configuration inline, already "
    "resolved. areole is therefore doing strictly more work. The layout-only column in "
    "PERFORMANCE.md is the fair head to head; this column is what a stylesheet costs.";

static const cmp_case CASES[] = {
    {"flat_1k", "1000 boxes, one row per sixteen, no painting", CAVEAT, a_1k, c_1k,
     "ar layout", cmp_areole_layout_us},
    {"flat_8k", "8000 boxes: the size Clay publishes", CAVEAT, a_8k, c_8k, "ar layout",
     cmp_areole_layout_us}};

static const cmp_case *clay_cases(int *count)
{
    *count = (int)(sizeof CASES / sizeof CASES[0]);
    return CASES;
}

static const cmp_engine CLAY_ENGINE = {"Clay", "0.14, vendored under third_party/bench/",
                                       clay_setup, clay_teardown, clay_cases};

const cmp_engine *cmp_engine_clay(void)
{
    return &CLAY_ENGINE;
}

/* Reported separately: areole's layout phase alone, from its own counters. */
double cmp_areole_layout_us(void)
{
    return g_ui ? (double)ar_perf_percentile(ar_perf_of(g_ui), AR_PHASE_LAYOUT, 50) : 0.0;
}
