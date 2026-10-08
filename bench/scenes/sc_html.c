/*
 * areole benchmark - HTML parsing.
 * SPDX-License-Identifier: MIT
 *
 * 0.9.0's budget asks for a parse throughput and a hundred-kilobyte document
 * in under seventy milliseconds on the tier. Neither number can be argued
 * about without a scene, and the parser shipped without one -- five commits of
 * tokenizer, tree construction, encoding and a user-agent stylesheet, and not
 * a measurement between them.
 *
 * ------------------------------------------------------------------------
 * Why a parse is a whole "frame" here
 *
 * Every other scene measures a frame, because every other subsystem runs once
 * a frame. Parsing runs once a *document*, so a frame in these scenes is one
 * complete parse -- which is the unit the budget is written in and the unit an
 * application actually pays.
 *
 * That is why 70 ms is acceptable here and would be absurd anywhere else in
 * this benchmark.
 *
 * ------------------------------------------------------------------------
 * Reading the throughput
 *
 * The harness reports ns/node, and the node count is the DOM's. Bytes per
 * second is the number the budget is written in, so each scene's description
 * carries its own document size and the commit message does the division. A
 * rate the harness cannot compute is better stated once than approximated
 * every run.
 */
#include "areole.h"
#include "bench.h"

#include <string.h>

/* Caller storage rather than the context's arena, so these scenes measure the
   parser and not ar_arena_persist. A real application uses ar_init_ex and
   ar_html_parse_into; the difference is a handful of allocations at startup. */
static ar_dom_node g_nodes[8192];
static ar_attr     g_attrs[2048];
static char        g_text[65536];
static char        g_scratch[8192];
static ar_doc      g_doc;

/* Built once at init so the timed region is the parse and nothing else. */
static char   g_page[24576];
static ar_u32 g_page_len;

static void parse(const char *src, ar_u32 len)
{
    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_nodes;
    g_doc.node_cap = (ar_i32)(sizeof g_nodes / sizeof g_nodes[0]);
    g_doc.attrs = g_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_attrs / sizeof g_attrs[0]);
    g_doc.text = g_text;
    g_doc.text_cap = (ar_u32)sizeof g_text;
    ar_html_parse(&g_doc, src, len, g_scratch, (ar_u32)sizeof g_scratch);
}

/* ------------------------------------------------------------------------
 * A small document: the fixed cost of a parse
 * ------------------------------------------------------------------------ */
static const char *const SMALL =
    "<!DOCTYPE html><html><head><title>t</title></head>"
    "<body><h1>Heading</h1><p>One paragraph of ordinary prose.</p></body></html>";

static void frame_small(bench_env *e)
{
    (void)e;
    parse(SMALL, (ar_u32)strlen(SMALL));
}

/* ------------------------------------------------------------------------
 * A realistic page
 *
 * Built rather than pasted, because a literal this size would run past the 509
 * characters C89 guarantees and would have to be chopped into fifty pieces.
 * The shape is what a documentation page looks like: a heading, prose with
 * inline markup, a list, a table, and enough of it to leave the fixed cost
 * behind.
 * ------------------------------------------------------------------------ */
static void init_page(bench_env *e)
{
    ar_u32 n = 0;
    int    i;

    (void)e;

#define PUT(s)                                                                                     \
    do                                                                                             \
    {                                                                                              \
        ar_u32 k = (ar_u32)strlen(s);                                                              \
        if (n + k < sizeof g_page - 1u)                                                            \
        {                                                                                          \
            memcpy(g_page + n, (s), k);                                                            \
            n += k;                                                                                \
        }                                                                                          \
    } while (0)

    PUT("<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
    PUT("<title>A page</title><style>body{margin:8px;}</style></head><body>");
    for (i = 0; i < 40; ++i)
    {
        PUT("<section class=\"block\"><h2>A heading with some words in it</h2>");
        PUT("<p>Ordinary prose with <b>bold</b> and <i>italic</i> and an ");
        PUT("<a href=\"/somewhere/else.html\">anchor</a> in the middle of it, ");
        PUT("long enough that the tokenizer has a run to coalesce.</p>");
        PUT("<ul><li>first item<li>second item<li>third item</ul>");
        PUT("<table><tr><td>a<td>b<td>c<tr><td>d<td>e<td>f</table>");
        PUT("</section>");
    }
    PUT("</body></html>");
#undef PUT

    g_page[n] = 0;
    g_page_len = n;
}

static void frame_page(bench_env *e)
{
    (void)e;
    parse(g_page, g_page_len);
}

/* ------------------------------------------------------------------------
 * The recovery paths
 *
 * Malformed markup is not the exception, it is the input. This is the same
 * page with every optional end tag omitted and a misnested pair in each
 * section, so the adoption agency and the implied-end-tag walk run forty times
 * rather than never.
 *
 * Two sizes, because "this would catch a quadratic recovery" is a claim and
 * not a measurement until there is something to divide by. Twice the sections
 * should cost twice the time, and the pair makes the same argument the table
 * scenes make at 100, 1000 and 10000 rows.
 * ------------------------------------------------------------------------ */
static char   g_bad[49152];
static ar_u32 g_bad_len;

static void build_malformed(int sections)
{
    ar_u32 n = 0;
    int    i;

#define PUT(s)                                                                                     \
    do                                                                                             \
    {                                                                                              \
        ar_u32 k = (ar_u32)strlen(s);                                                              \
        if (n + k < sizeof g_bad - 1u)                                                             \
        {                                                                                          \
            memcpy(g_bad + n, (s), k);                                                             \
            n += k;                                                                                \
        }                                                                                          \
    } while (0)

    PUT("<html><body>");
    for (i = 0; i < sections; ++i)
    {
        PUT("<div><p>unclosed paragraph<p>and another");
        PUT("<b>bold <i>crossing</b> over</i>");
        PUT("<ul><li>one<li>two<li>three");
        PUT("<table><tr><td>a<td>b<tr><td>c<td>d");
        PUT("</div>");
    }
#undef PUT

    g_bad[n] = 0;
    g_bad_len = n;
}

static void init_malformed(bench_env *e)
{
    (void)e;
    build_malformed(40);
}

static void init_malformed_2x(bench_env *e)
{
    (void)e;
    build_malformed(80);
}

static void frame_malformed(bench_env *e)
{
    (void)e;
    parse(g_bad, g_bad_len);
}

/* ------------------------------------------------------------------------
 * Bytes to boxes
 *
 * Parse, then walk the document into the box tree and lay it out. This is what
 * an application actually does with a document, and it is the only scene here
 * that touches the rest of the engine -- so it is the one that would notice if
 * ar_dom_build ever stopped being a thin walk.
 * ------------------------------------------------------------------------ */
static void init_render(bench_env *e)
{
    init_page(e);
    ar_ua_stylesheet(e->ui);
}

static void frame_render(bench_env *e)
{
    ar_input in;

    parse(g_page, g_page_len);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(e->ui, &in);
    ar_dom_build(e->ui, &g_doc);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

static const bench_scene SC_SMALL = {
    "html_small", "html", "a 130 byte document: the fixed cost of a parse", 800, 600, 0, 0,
    frame_small};

static const bench_scene SC_PAGE = {
    "html_page", "html",    "a 13 KB documentation page, well formed", 800, 600, 0,
    init_page,   frame_page};

static const bench_scene SC_MALFORMED = {"html_malformed",
                                         "html",
                                         "the same page with every optional end tag omitted",
                                         800,
                                         600,
                                         0,
                                         init_malformed,
                                         frame_malformed};

static const bench_scene SC_MALFORMED_2X = {
    "html_malformed_2x",
    "html",
    "twice the sections: the ratio is the anti-quadratic check",
    800,
    600,
    0,
    init_malformed_2x,
    frame_malformed};

static const bench_scene SC_RENDER = {
    "html_render", "html",      "the same page parsed, walked into boxes and laid out", 800, 600, 1,
    init_render,   frame_render};

void bench_register_html(void)
{
    bench_register(&SC_SMALL);
    bench_register(&SC_PAGE);
    bench_register(&SC_MALFORMED);
    bench_register(&SC_MALFORMED_2X);
    bench_register(&SC_RENDER);
}
