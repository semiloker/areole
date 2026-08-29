/*
 * areole example 11 - the flex, grid and subgrid corpus
 * SPDX-License-Identifier: MIT
 *
 * Where grid puts its boxes, checked against a browser rather than against my
 * reading of the specification.
 *
 *     example_grid --dump
 *     python tools/compare_layout.py --run ./build/example_grid.exe \
 *            examples/11_grid/grid.html
 *
 * ------------------------------------------------------------------------
 * Why this exists, later than it should have
 *
 * Every layout release since 0.5.0 shipped with a corpus: 03_block, 05_snap,
 * 06_sticky, 07_env, 08_anchor, 09_table, 10_table_responsive. 0.8.x -- the
 * rest of flexbox, the whole of grid track sizing and placement, the sizing
 * keywords, and subgrid -- shipped with none, so `src/ar_layout_grid.c` was
 * 39 KB of new solver validated only by checks that agreed with it.
 *
 * The acceptance criteria for 0.8.1 and 0.8.2 name Chrome corpora for
 * `display: contents`, `aspect-ratio`, the automatic minimum size, safe
 * centring and the three-card subgrid layout. This is those, late.
 *
 * The 0.8.0 tour page already found one bug the whole suite missed: a grid
 * item that stated a height never got one, because only the stretch path
 * assigned rect.h and every grid check until then let its items stretch. That
 * is the argument for this file in one sentence.
 *
 * ------------------------------------------------------------------------
 * Why almost nothing here contains text
 *
 * A box whose size comes from measuring its own text is reported by
 * compare_layout.py as a spread rather than a verdict, because two independent
 * rasterizers of the same outlines never agree exactly. A corpus of those
 * measures the font, not the solver.
 *
 * So the items have stated sizes, and the cases that are genuinely about
 * intrinsic sizing use *child boxes* of stated width rather than text: the
 * min-content width of a container is its widest child, which is the same
 * question with an answer both engines can agree on to the pixel.
 *
 * ------------------------------------------------------------------------
 * Why the tracks are small
 *
 * Everything fits in 300x200, the same case size the block corpus uses, so the
 * two files can share a window layout and a twin format. A grid that overflows
 * its case would be testing overflow.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 300
#define CASE_H 200
#define WIN_W  900
#define WIN_H  620

static unsigned char g_memory[AR_MEM(256)];
static unsigned char g_chrome_memory[AR_MEM(256)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * The shared vocabulary. Every case draws from it, and no case may redefine a
 * display type -- only sizes, tracks and alignment -- so the C and the HTML
 * stay obviously the same shape.
 *
 * `#root` is a block container filling the surface, exactly as the block
 * corpus does it, so a grid inside it is measured against a known box rather
 * than against whatever the root happened to be.
 */
static const char *SHEET_BASE = "#root { display:block; }"
                                ".g { display:grid; }"
                                ".f { display:flex; }"
                                ".b { display:block; }"
                                ".w { display:contents; }"
                                ".a { display:block; }"
                                ".c { display:block; }"
                                ".d { display:block; }"
                                ".e { display:block; }"
                                ".deck { display:grid; }"
                                ".card { display:grid; }"
                                ".t { display:block; }"
                                ".y { display:block; }"
                                ".z { display:block; }";

/*
 * Paint only. No rule here touches a size, so --dump prints the same numbers
 * with it as without -- the same discipline the block corpus states, and for
 * the same reason: a colour that moved a box would be a corpus that lies.
 */
static const char *SHEET_PAINT = ".a, .c, .d, .e { background:#e8c9ae; }"
                                 ".g, .deck { background:#dfe7dc; }"
                                 ".card { background:#c9d8c4; }"
                                 ".t { background:#c2703d; }"
                                 ".y { background:#9fb4c7; }"
                                 ".z { background:#7a4a2a; }";

struct grid_case
{
    const char *name;
    const char *css;
    /* '(' opens a box, ')' closes it, and the letters between name its class.
       The same notation the block corpus uses, and the same parser. */
    const char *tree;
};

static const struct grid_case CASES[] = {
    /* ---------------------------------------------------------- tracks -- */
    {"tracks-fixed", ".g { width:280px; height:60px; grid-template-columns:120px 80px 60px; }",
     "(g(a)(c)(d))"},

    {"tracks-fr-equal", ".g { width:270px; height:60px; grid-template-columns:1fr 1fr 1fr; }",
     "(g(a)(c)(d))"},

    /* 1fr is a ratio of the space left, not of the container: the pair share
       what the 120px track did not take, in the ratio they name. */
    {"tracks-fr-ratio", ".g { width:270px; height:60px; grid-template-columns:1fr 2fr; }",
     "(g(a)(c))"},

    {"tracks-fixed-and-fr", ".g { width:280px; height:60px; grid-template-columns:120px 1fr 2fr; }",
     "(g(a)(c)(d))"},

    /* `1fr` is "at least the contents, then a share of what is left", which is
       the whole reason the distribution is a loop. The 90px child is wider
       than an even third of 210, so the other two share the rest. */
    {"tracks-fr-floored-by-content",
     ".g { width:210px; height:60px; grid-template-columns:1fr 1fr 1fr; } .t { width:90px; }",
     "(g(a(t))(c)(d))"},

    {"tracks-minmax-fixed",
     ".g { width:280px; height:60px; grid-template-columns:minmax(60px,100px) 1fr; }", "(g(a)(c))"},

    {"tracks-auto-and-fr",
     ".g { width:280px; height:60px; grid-template-columns:auto 1fr; } .t { width:70px; }",
     "(g(a(t))(c))"},

    {"tracks-repeat", ".g { width:280px; height:60px; grid-template-columns:repeat(4, 1fr); }",
     "(g(a)(c)(d)(e))"},

    {"tracks-rows-fixed",
     ".g { width:200px; height:180px; grid-template-columns:1fr;"
     "     grid-template-rows:40px 60px 30px; }",
     "(g(a)(c)(d))"},

    {"tracks-rows-fr",
     ".g { width:200px; height:180px; grid-template-columns:1fr;"
     "     grid-template-rows:1fr 2fr; }",
     "(g(a)(c))"},

    /* -------------------------------------------------------- placement -- */
    {"place-explicit-line",
     ".g { width:270px; height:60px; grid-template-columns:1fr 1fr 1fr; }"
     " .a { grid-column-start:3; grid-column-end:4; }",
     "(g(a))"},

    {"place-span",
     ".g { width:270px; height:60px; grid-template-columns:1fr 1fr 1fr; }"
     " .a { grid-column:1 / span 2; }",
     "(g(a)(c))"},

    {"place-implicit-rows",
     ".g { width:180px; height:180px; grid-template-columns:1fr 1fr;"
     "     grid-auto-rows:40px; }",
     "(g(a)(c)(d)(e))"},

    {"place-auto-flow-column",
     ".g { width:270px; height:120px; grid-template-rows:1fr 1fr;"
     "     grid-auto-flow:column; grid-auto-columns:80px; }",
     "(g(a)(c)(d)(e))"},

    /* A row and a column stated together pin the box to one cell, and the
       automatic ones flow around what is already taken. */
    {"place-row-and-column",
     ".g { width:180px; height:120px; grid-template-columns:1fr 1fr;"
     "     grid-template-rows:1fr 1fr; } .a { grid-column:2 / 3; grid-row:2 / 3; }",
     "(g(a)(c))"},

    {"place-gap",
     ".g { width:280px; height:120px; grid-template-columns:1fr 1fr;"
     "     grid-template-rows:1fr 1fr; row-gap:10px; column-gap:20px; }",
     "(g(a)(c)(d)(e))"},

    /* ------------------------------------------------------- alignment -- */
    {"align-items-start",
     ".g { width:200px; height:120px; grid-template-columns:1fr 1fr;"
     "     align-items:start; } .a, .c { height:30px; }",
     "(g(a)(c))"},

    {"align-items-center",
     ".g { width:200px; height:120px; grid-template-columns:1fr 1fr;"
     "     align-items:center; } .a, .c { height:30px; }",
     "(g(a)(c))"},

    {"align-items-end",
     ".g { width:200px; height:120px; grid-template-columns:1fr 1fr;"
     "     align-items:end; } .a, .c { height:30px; }",
     "(g(a)(c))"},

    {"justify-items-center",
     ".g { width:200px; height:80px; grid-template-columns:1fr 1fr;"
     "     justify-items:center; } .a, .c { width:40px; height:30px; }",
     "(g(a)(c))"},

    {"align-self-overrides",
     ".g { width:200px; height:120px; grid-template-columns:1fr 1fr;"
     "     align-items:start; } .a, .c { height:30px; } .c { align-self:end; }",
     "(g(a)(c))"},

    {"justify-content-center",
     ".g { width:280px; height:80px; grid-template-columns:60px 60px;"
     "     justify-content:center; }",
     "(g(a)(c))"},

    {"align-content-space-between",
     ".g { width:200px; height:160px; grid-template-columns:1fr;"
     "     grid-template-rows:30px 30px; align-content:space-between; }",
     "(g(a)(c))"},

    /*
     * 0.8.1 acceptance 5. Centring a box larger than its track puts half the
     * overflow before the start edge, where it can be neither scrolled to nor
     * read. `safe` falls back to start alignment exactly then, and only then.
     */
    {"safe-center-overflows",
     ".g { width:200px; height:60px; grid-template-columns:80px;"
     "     justify-items:safe center; } .a { width:140px; height:30px; }",
     "(g(a))"},

    {"unsafe-center-overflows",
     ".g { width:200px; height:60px; grid-template-columns:80px;"
     "     justify-items:unsafe center; } .a { width:140px; height:30px; }",
     "(g(a))"},

    /* ---------------------------------------------------------- sizing -- */
    /* A grid item that states a height gets one. This is the bug the 0.8.0
       tour page found and the reason this corpus exists. */
    {"item-stated-height",
     ".g { width:200px; height:120px; grid-template-columns:1fr 1fr; }"
     " .a, .c { height:24px; }",
     "(g(a)(c))"},

    {"aspect-ratio-from-width",
     ".g { width:200px; height:120px; grid-template-columns:80px; align-items:start; }"
     " .a { width:80px; aspect-ratio:2; }",
     "(g(a))"},

    {"aspect-ratio-four-thirds", ".b { width:60px; aspect-ratio:4/3; }", "(b)"},

    /* Both axes stated keeps both, which is what makes the property safe to
       put in a base stylesheet. */
    {"aspect-ratio-both-stated", ".b { width:60px; height:20px; aspect-ratio:2; }", "(b)"},

    /* min-content of a container is its widest child, so this asks the
       intrinsic question without asking the font one. */
    {"width-min-content", ".b { width:min-content; } .t { width:70px; } .y { width:40px; }",
     "(b(t)(y))"},

    {"width-max-content", ".b { width:max-content; } .t { width:70px; } .y { width:40px; }",
     "(b(t)(y))"},

    {"width-fit-content-function", ".b { width:fit-content(50px); } .t { width:70px; }", "(b(t))"},

    /* ------------------------------------- automatic minimum, 0.8.1 #4 -- */
    /* A flex item cannot shrink below its own min-content size unless it is
       told it may. Three 90px children in 150px of room is the case. */
    {"flex-auto-minimum", ".f { width:150px; height:40px; } .a, .c { width:90px; }", "(f(a)(c))"},

    {"flex-min-width-zero",
     ".f { width:150px; height:40px; } .a, .c { width:90px; min-width:0px; }", "(f(a)(c))"},

    {"flex-overflow-hidden-escape",
     ".f { width:150px; height:40px; } .a, .c { width:90px; overflow:hidden; }", "(f(a)(c))"},

    /*
     * ------------------------------------- display: contents, and why not --
     *
     * 0.8.1 acceptance 2 asks for a `display: contents` corpus and there is
     * none here, because this corpus cannot host one.
     *
     * compare_layout.py matches boxes on their path through the tree, which is
     * the one identifier the two engines genuinely share. A box that generates
     * none breaks exactly that: areole splices the wrapper out of its parent's
     * child list, so the grid's children become `.a`, `.c`, `.d` at indices
     * 0, 1, 2 -- while the DOM still has `.a` and `.w` at 0 and 1, with `.c`
     * and `.d` beneath. The paths line up in count and mean different boxes,
     * so the comparison would report a confident answer to a question nobody
     * asked.
     *
     * It is checked in tests/ar_test.c instead, where the tree is known rather
     * than inferred. Named here so the gap is a decision, the same way the
     * block corpus names its missing border case.
     */

    /* ----------------------------------------------- subgrid, 0.8.2 #2 -- */
    {"subgrid-rows",
     ".deck { width:280px; height:150px; grid-template-columns:1fr 1fr;"
     "        grid-template-rows:30px 60px 20px; }"
     " .card { grid-row:1 / 4; grid-template-rows:subgrid; }",
     "(deck(card(t)(y)(z))(card(t)(y)(z)))"},

    {"subgrid-columns",
     ".deck { width:280px; height:80px; grid-template-columns:60px 1fr 40px;"
     "        grid-template-rows:1fr; }"
     " .card { grid-column:1 / 4; grid-template-columns:subgrid; }",
     "(deck(card(t)(y)(z)))"},

    /*
     * The card case, which is the point of the feature: the bodies differ and
     * the footers still share a row. Three lengths rather than one, because a
     * layout that only works at one content length is the thing subgrid
     * exists to replace.
     */
    {"subgrid-cards-short",
     ".deck { width:280px; height:150px; grid-template-columns:1fr 1fr 1fr;"
     "        grid-template-rows:24px auto 20px; column-gap:8px; }"
     " .card { grid-row:1 / 4; grid-template-rows:subgrid; }"
     " .y { height:20px; }",
     "(deck(card(t)(y)(z))(card(t)(y)(z))(card(t)(y)(z)))"},

    {"subgrid-cards-uneven",
     ".deck { width:280px; height:150px; grid-template-columns:1fr 1fr 1fr;"
     "        grid-template-rows:24px auto 20px; column-gap:8px; }"
     " .card { grid-row:1 / 4; grid-template-rows:subgrid; }"
     " .y { height:20px; } .card2 .y { height:56px; }",
     "(deck(card(t)(y)(z))(card card2(t)(y)(z))(card(t)(y)(z)))"},

    /* A subgrid whose parent is not a grid falls back to an ordinary grid,
       which CSS says outright and which happens here for free -- with no
       template on either axis, every track is implicit. */
    {"subgrid-parent-not-grid",
     ".b { width:200px; height:80px; } .card { grid-template-rows:subgrid; } .t, .y { height:20px; "
     "}",
     "(b(card(t)(y)))"},

    /* ------------------------------------------------------------ flex -- */
    {"flex-wrap",
     ".f { width:200px; height:120px; flex-wrap:wrap; } .a, .c, .d { width:80px; height:30px; }",
     "(f(a)(c)(d))"},

    {"flex-grow-ratio", ".f { width:240px; height:40px; } .a { flex-grow:1; } .c { flex-grow:3; }",
     "(f(a)(c))"},

    /* One division gives 137 each. The middle item hits its maximum at 60 and
       the sixty it could not take goes to the other two, which is the whole
       reason section 9.7 is a loop. */
    {"flex-max-redistributes",
     ".f { width:412px; height:40px; } .a, .c, .d { flex-grow:1; } .c { max-width:60px; }",
     "(f(a)(c)(d))"},

    {"flex-order", ".f { width:240px; height:40px; } .a, .c, .d { width:60px; } .a { order:2; }",
     "(f(a)(c)(d))"},

    {"flex-basis-vs-grow",
     ".f { width:240px; height:40px; } .a { flex-basis:100px; flex-grow:1; }"
     " .c { flex-grow:1; }",
     "(f(a)(c))"}};

#define CASE_COUNT ((ar_i32)(sizeof CASES / sizeof CASES[0]))

static void build(ar_ctx *ui, const char *tree)
{
    char sel[32];

    while (*tree)
    {
        if (*tree == '(')
        {
            const char *start;
            ar_u32      n = 0;

            tree++;
            start = tree;
            while (*tree && *tree != '(' && *tree != ')')
            {
                tree++;
            }
            n = (ar_u32)(tree - start);
            if (n > 0 && n < sizeof sel - 5)
            {
                ar_u32 k;

                strcpy(sel, "div.");
                memcpy(sel + 4, start, n);
                sel[4 + n] = 0;
                /* A space between class names in the tree means a second class
                   on the same box, which the selector syntax spells with a
                   dot. `card card2` is the one case that needs it. */
                for (k = 4; k < 4 + n; ++k)
                {
                    if (sel[k] == ' ')
                    {
                        sel[k] = '.';
                    }
                }
                ar_begin(ui, sel);
            }
            else
            {
                ar_begin(ui, "div");
            }
            continue;
        }
        if (*tree == ')')
        {
            ar_end(ui);
            tree++;
            continue;
        }
        tree++;
    }
}

static void dump_path(const ar_ctx *c, ar_i32 i, char *out)
{
    ar_i32 stack[32];
    ar_i32 depth = 0;
    ar_i32 at = i;
    char  *w = out;

    while (at >= 0 && depth < 32)
    {
        stack[depth++] = at;
        at = ar_node_parent(c, at);
    }
    while (depth > 0)
    {
        --depth;
        if (w != out)
        {
            *w++ = '/';
        }
        sprintf(w, "%ld", (long)ar_node_child_index(c, stack[depth]));
        while (*w)
        {
            ++w;
        }
    }
    *w = 0;
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     k;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  grid corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (k = 0; k < CASE_COUNT; ++k)
    {
        ar_ctx  *ui;
        ar_input in;
        ar_i32   i;

        /* A fresh context per case, so one sheet cannot leak into the next. */
        ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
        if (!ui)
        {
            printf("# out of memory\n");
            return 1;
        }
        ar_stylesheet(ui, SHEET_BASE);
        ar_stylesheet(ui, CASES[k].css);
        if (ar_stylesheet_errors(ui))
        {
            printf("# %s: stylesheet has %lu problem(s)\n", CASES[k].name,
                   (unsigned long)ar_stylesheet_errors(ui));
            return 1;
        }

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        ar_frame_begin(ui, &in);
        ar_begin(ui, "div#root");
        build(ui, CASES[k].tree);
        ar_end(ui);
        ar_frame_end(ui, &s);
        ar_frame_presented(ui);

        if (ar_unbalanced(ui))
        {
            printf("# %s: unbalanced\n", CASES[k].name);
            return 1;
        }

        printf("# page %s\n", CASES[k].name);
        for (i = 0; i < ar_node_count(ui); ++i)
        {
            char    path[128];
            ar_rect r = ar_node_rect(ui, i);

            dump_path(ui, i, path);
            printf("%s %ld %ld %ld %ld |\n", path, (long)r.x, (long)r.y, (long)r.w, (long)r.h);
        }
    }
    return 0;
}

/*
 * ------------------------------------------------------------------------
 * The browser twin, printed by the corpus rather than kept beside it
 *
 *     example_grid --html > examples/11_grid/grid.html
 *
 * Every other corpus here keeps its twin as a hand-written file, and every one
 * of them has drifted: the tour's twin stopped at 0.6.0 while its C side grew
 * to fourteen pages, and the comparison has been reporting boxes whose text no
 * longer matches ever since.
 *
 * There is nothing weaker about generating it. The twin's job is to declare
 * the *same* thing; the independence that makes the comparison worth anything
 * is in the two layout engines, not in who typed the markup. Generating it
 * removes the one failure mode that has actually bitten this project, and the
 * generated file is committed so a reviewer can read it.
 */
static void emit_scoped_css(const char *name, const char *css)
{
    ar_i32 depth = 0;
    int    at_selector = 1;

    while (*css)
    {
        if (at_selector && *css != ' ' && *css != '\t' && *css != '\n')
        {
            /* A case's rules are scoped to its own case box, so `.a` in one
               case cannot reach into another. Applied at the start, after
               every `}`, and after every comma in a selector list -- that last
               one because `.a, .c` would otherwise leave `.c` global. */
            printf("#%s ", name);
            at_selector = 0;
        }
        if (*css == '{')
        {
            depth++;
        }
        else if (*css == '}')
        {
            depth--;
            at_selector = 1;
        }
        else if (*css == ',' && depth == 0)
        {
            at_selector = 1;
        }
        putchar(*css);
        css++;
    }
}

static void emit_tree(const char *tree)
{
    while (*tree)
    {
        if (*tree == '(')
        {
            const char *start;

            tree++;
            start = tree;
            while (*tree && *tree != '(' && *tree != ')')
            {
                tree++;
            }
            printf("<div class=\"%.*s\">", (int)(tree - start), start);
            continue;
        }
        if (*tree == ')')
        {
            printf("</div>");
            tree++;
            continue;
        }
        tree++;
    }
}

static int run_html(void)
{
    ar_i32 k;

    printf("<!DOCTYPE html>\n<meta charset=\"utf-8\">\n");
    printf("<title>areole grid corpus %s the browser twin</title>\n\n", "\xe2\x80\x94");
    printf("<!--\n");
    printf("  GENERATED by examples/11_grid/main.c. Do not edit.\n\n");
    printf("      ./build/example_grid --html > examples/11_grid/grid.html\n\n");
    printf("  The same flex, grid, sizing and subgrid cases as the C corpus, in HTML, so a\n");
    printf("  browser can be asked the same questions:\n\n");
    printf("      python tools/compare_layout.py --run ./build/example_grid.exe \\\n");
    printf("             examples/11_grid/grid.html\n\n");
    printf("  It is generated because every hand-written twin in this repository has drifted\n");
    printf("  from its C side, and the comparison then reports the drift as a divergence.\n");
    printf("-->\n\n");

    printf("<style>\n");
    printf("* { margin: 0; padding: 0; }\n\n");
    printf("/*\n");
    printf(" * No box-sizing rule. areole defaults to content-box, as CSS says, and a reset\n");
    printf(" * that set border-box here would change every stated width in the corpus.\n");
    printf(" */\n");
    printf("body { background:#fff; font:13px/1.4 \"Segoe UI\", system-ui, sans-serif;\n");
    printf("       padding:20px; }\n");
    printf("h2 { font:600 12px/1.4 \"Segoe UI\", system-ui, sans-serif; color:#8d8578;\n");
    printf("     margin:18px 0 4px; }\n\n");
    printf("/*\n");
    printf(" * `display: flow-root` says \"be a block formatting context and nothing else\",\n");
    printf(" * which is what areole's #root is.\n");
    printf(" */\n");
    printf(".app { display:flow-root; width:%dpx; height:%dpx;\n", CASE_W, CASE_H);
    printf("       outline:1px solid #e6dcc6; background:#fcfaf6; }\n");
    printf(".app div { background:rgba(194,112,61,0.25); }\n\n");
    printf("#out { width:100%%; height:200px; font:12px/1.4 Consolas, monospace;\n");
    printf("       white-space:pre; overflow:auto; border:1px solid #ddd; padding:8px;\n");
    printf("       margin-top:16px; }\n");
    printf("button.dump { font:13px \"Segoe UI\", sans-serif; padding:8px 14px; cursor:pointer; "
           "}\n\n");
    /*
     * The shared vocabulary, unscoped, exactly as SHEET_BASE gives it to
     * areole. Leaving it out is not a subtle failure: without it `.g` is an
     * ordinary div, every item takes the full width, and the browser reports
     * 280x0 for every box in the corpus. Which is what it did.
     *
     * `#root` is not emitted -- `.app` above is what plays that part here.
     */
    printf("/* -------------------------------------------------- the vocabulary -- */\n");
    printf(".g { display:grid; }\n");
    printf(".f { display:flex; }\n");
    printf(".b { display:block; }\n");
    printf(".w { display:contents; }\n");
    printf(".a, .c, .d, .e { display:block; }\n");
    printf(".deck { display:grid; }\n");
    printf(".card { display:grid; }\n");
    printf(".t, .y, .z { display:block; }\n\n");

    printf("/* ------------------------------------------------------------- cases -- */\n");
    for (k = 0; k < CASE_COUNT; ++k)
    {
        emit_scoped_css(CASES[k].name, CASES[k].css);
        printf("\n");
    }
    printf("</style>\n\n");

    for (k = 0; k < CASE_COUNT; ++k)
    {
        printf("<h2>%s</h2>\n", CASES[k].name);
        printf("<div class=\"app\" id=\"%s\" data-page=\"%s\">", CASES[k].name, CASES[k].name);
        emit_tree(CASES[k].tree);
        printf("</div>\n\n");
    }

    printf("<button class=\"dump\" onclick=\"dump()\">Dump</button>\n");
    /* A textarea, and the dump written as child text as well as value:
       compare_layout.py recovers it from --dump-dom, where a property set by
       script never appears in the markup. Every twin here does both, and
       writing only textContent produced an empty dump that the tool reports as
       "the page produced no dump" -- which is how this line got written. */
    printf("<textarea id=\"out\" spellcheck=\"false\"></textarea>\n\n");
    printf("<script>\n");
    printf("/* The same walk and output format as every other twin here:\n");
    printf("   document order is declaration order, coordinates are relative to the\n");
    printf("   .app origin, and everything is rounded because areole is integer. */\n");
    printf("function childIndex(el) {\n");
    printf("  let n = 0;\n");
    printf("  for (let p = el.previousElementSibling; p; p = p.previousElementSibling) n++;\n");
    printf("  return n;\n");
    printf("}\n\n");
    printf("function walk(el, path, origin, out) {\n");
    printf("  const r = el.getBoundingClientRect();\n");
    printf("  out.push(\n");
    printf("    path + ' ' +\n");
    printf("    Math.round(r.left - origin.left) + ' ' +\n");
    printf("    Math.round(r.top - origin.top) + ' ' +\n");
    printf("    Math.round(r.width) + ' ' +\n");
    printf("    Math.round(r.height) + ' |'\n");
    printf("  );\n");
    printf("  for (const c of el.children) walk(c, path + '/' + childIndex(c), origin, out);\n");
    printf("}\n\n");
    printf("function dump() {\n");
    printf("  const out = ['# browser ' + navigator.userAgent.replace(/\\s+/g, ' ')];\n");
    printf("  for (const app of document.querySelectorAll('.app')) {\n");
    printf("    out.push('# page ' + app.dataset.page);\n");
    printf("    walk(app, '0', app.getBoundingClientRect(), out);\n");
    printf("  }\n");
    printf("  const box = document.getElementById('out');\n");
    printf("  box.value = out.join('\\n');\n");
    printf("  box.textContent = out.join('\\n');   /* so --dump-dom sees it */\n");
    printf("}\n");
    printf("dump();\n");
    printf("</script>\n");
    return 0;
}

static const char *CHROME_SHEET =
    "#app  { display:flex; flex-direction:row; background:#fcfaf6;"
    "        font-size:14px; color:#3a3733; }"
    ".rail { width:300px; display:flex; flex-direction:column; padding:14px;"
    "        gap:1px; background:#f4efe4; overflow:hidden; }"
    ".title { font-size:20px; color:#20201e; padding-bottom:4px; }"
    ".hint  { font-size:11px; color:#a09789; padding-bottom:10px; }";

static const char *CHROME_SHEET2 =
    ".case    { padding:3px 8px; font-size:12px; color:#6f685d; }"
    ".case:hover { background:#ece4d3; color:#20201e; }"
    ".case-on { padding:3px 8px; font-size:12px; color:#fdfaf3; background:#7a4a2a; }"
    ".page  { width:grow; display:flex; flex-direction:column; padding:20px; gap:8px; }"
    ".h1    { font-size:22px; color:#20201e; }"
    ".css   { font-size:11px; color:#8d8578; }"
    ".frame { width:300px; height:200px; border:1px solid #d8ccb4; }";

/* The border on .frame, which the case surface sits inside. Has to match the
   border-width in CHROME_SHEET2 -- there is no inspection API for a border. */
#define FRAME_BORDER 1

static int run_window(void)
{
    ar_ctx *chrome;
    ar_ctx *ui = 0;
    ar_win *win;
    ar_rect frame;
    ar_i32  shown = 0;
    ar_i32  built = -1;
    ar_i32  k;

    chrome = ar_init(g_chrome_memory, (ar_u32)sizeof g_chrome_memory);
    if (!chrome)
    {
        printf("not enough memory for the chrome\n");
        return 1;
    }
    ar_stylesheet(chrome, CHROME_SHEET);
    ar_stylesheet(chrome, CHROME_SHEET2);
    if (ar_stylesheet_errors(chrome))
    {
        printf("chrome stylesheet has %lu problem(s)\n",
               (unsigned long)ar_stylesheet_errors(chrome));
        return 1;
    }

    win = ar_win_open("areole - the grid corpus", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }
    ar_set_clock(chrome, ar_time_us);

    while (ar_win_pump(win))
    {
        ar_surface *s = ar_win_surface(win);
        ar_surface  cs;
        ar_input    in;

        ar_frame_begin(chrome, ar_win_input(win));
        ar_begin(chrome, "div#app");

        ar_begin(chrome, "div.rail");
        ar_text(chrome, "div.title", "Grid corpus");
        ar_text(chrome, "div.hint", "Flex, grid, sizing and subgrid, against a browser.");
        for (k = 0; k < CASE_COUNT; ++k)
        {
            if (ar_button(chrome, k == shown ? "div.case-on" : "div.case", CASES[k].name))
            {
                shown = k;
                built = -1;
                ar_invalidate_all(chrome);
            }
        }
        ar_end(chrome);

        ar_begin(chrome, "div.page");
        ar_text(chrome, "div.h1", CASES[shown].name);
        ar_text(chrome, "div.css", CASES[shown].css);
        ar_begin(chrome, "div.frame");
        ar_end(chrome);
        ar_end(chrome);

        ar_end(chrome);
        ar_frame_end(chrome, s);

        /* The case, into a sub-view of the same pixels. Rebuilt only when the
           selection changes: its own context has no input and nothing in it
           moves, so laying it out every frame would be work for nothing. */
        if (built != shown)
        {
            ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
            if (ui)
            {
                ar_stylesheet(ui, SHEET_BASE);
                ar_stylesheet(ui, SHEET_PAINT);
                ar_stylesheet(ui, CASES[shown].css);
            }
            built = shown;
        }

        /* Where the frame actually landed, asked of layout rather than
           predicted. .frame is declared last and boxes are numbered in
           declaration order, so it is the final node. The block corpus keeps
           the long version of why this is not two constants. */
        frame = ar_node_rect(chrome, ar_node_count(chrome) - 1);

        cs.pixels = s->pixels + (frame.y + FRAME_BORDER) * s->stride + frame.x + FRAME_BORDER;
        cs.w = CASE_W;
        cs.h = CASE_H;
        cs.stride = s->stride;

        if (ui)
        {
            memset(&in, 0, sizeof in);
            in.mouse_x = -1;
            in.mouse_y = -1;
            ar_frame_begin(ui, &in);
            ar_begin(ui, "div#root");
            build(ui, CASES[shown].tree);
            ar_end(ui);
            /* Everything, every frame: the chrome may have painted over it,
               and this is an inspector rather than a frame budget. */
            ar_invalidate_all(ui);
            ar_frame_end(ui, &cs);
            ar_frame_presented(ui);
        }

        ar_win_present(win, ar_rect_make(0, 0, s->w, s->h));
        ar_frame_presented(chrome);

        if (ar_needs_redraw(chrome))
        {
            ar_win_wake(win);
        }
    }

    ar_win_close(win);
    return 0;
}

/*
 * No arguments opens the window, which is what happens when somebody
 * double-clicks it. --dump prints the rectangles, which is what the browser
 * comparison drives and what CI runs.
 */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0)
    {
        return run_dump();
    }
    if (argc > 1 && strcmp(argv[1], "--html") == 0)
    {
        return run_html();
    }
    return run_window();
}
