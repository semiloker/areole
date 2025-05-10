/*
 * areole example 03 - the block corpus
 * SPDX-License-Identifier: MIT
 *
 * Margin collapsing, checked against a browser rather than against my reading
 * of the specification.
 *
 * ar_test.c already covers these cases with expected numbers written from the
 * specification text. That is the right way to build the thing, and it is
 * still only one reading of a document that is famously easy to misread. This
 * corpus renders the same cases and hands them to Chromium for a second
 * opinion:
 *
 *     example_block --dump
 *     python tools/compare_layout.py --run ./build/example_block.exe \
 *            examples/03_block/block.html
 *
 * Run with --dump it prints the rectangles and exits, which is what the
 * comparison drives. Run with no arguments it opens a window and draws them,
 * one case at a time, because a corpus you can only read as numbers is a
 * corpus nobody looks at.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

/*
 * Two contexts, and that is the interesting part of this file.
 *
 * The chrome -- the list of case names down the side -- is one interface, and
 * the case being shown is another, with its own stylesheet full of names like
 * `.a` and `.outer` that would collide with anything else in the same sheet.
 * Keeping them apart is one extra block of memory and no engine support at
 * all: the case renders into a surface whose pixels point into the middle of
 * the window's, with the window's stride. A sub-view, not a copy.
 */
#define CASE_W 300
#define CASE_H 200
#define WIN_W  860
#define WIN_H  560

static unsigned char g_memory[AR_MEM(128)];
static unsigned char g_chrome_memory[AR_MEM(256)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * Every case shares this: the root is a block container filling the surface,
 * with nothing of its own to interfere.
 *
 * There is no border case here, though a border stops a margin escaping just
 * as padding does and ar_test.c could check it. areole paints a border without
 * reserving space for it, and CSS reserves a pixel -- so the two would differ
 * by that pixel for a reason that has nothing to do with margins, and a corpus
 * that reports a known divergence as a failure teaches you to ignore it.
 */
static const char *SHEET_BASE = "#root { display:block; }"
                                ".a { display:block; }"
                                ".b { display:block; }"
                                ".c { display:block; }"
                                ".outer { display:block; }"
                                ".inner { display:block; }"
                                ".i { display:inline-block; }"
                                ".l { display:block; }"
                                ".r { display:block; }"
                                ".bfc { display:block; }"
                                ".narrow { display:block; }"
                                ".abs { display:block; }";

/*
 * Colour, so the window has something to show. Paint only -- no rule here
 * touches a size, so --dump prints exactly what it printed before, and there
 * is a check in the repository history that says so.
 *
 * Written as selector lists, which 0.4.0 added and which this file may as well
 * be the first thing to use.
 */
static const char *SHEET_PAINT = ".a, .b, .c { background:#e8c9ae; }"
                                 ".outer, .narrow { background:#dfe7dc; }"
                                 ".bfc { background:#c9d8c4; }"
                                 ".l, .r { background:#c2703d; }"
                                 ".i, .t { background:#9fb4c7; }"
                                 ".inner { background:#7a4a2a; }";

struct block_case
{
    const char *name;
    const char *css;
    /* The tree, as a string of tokens: '(' opens a box, ')' closes it, and a
       letter names its class. Small enough that a parser is cheaper than a
       function per case, and it keeps the C and the HTML obviously the same
       shape. */
    const char *tree;
};

static const struct block_case CASES[] = {
    {"siblings", ".a { height:20px; margin-bottom:20px; } .b { height:30px; margin-top:10px; }",
     "(a)(b)"},

    {"siblings-negative",
     ".a { height:20px; margin-bottom:20px; } .b { height:30px; margin-top:-8px; }", "(a)(b)"},

    {"siblings-both-negative",
     ".a { height:20px; margin-bottom:-20px; } .b { height:30px; margin-top:-8px; }", "(a)(b)"},

    {"escapes-through-parent",
     ".a { height:10px; } .outer { } .inner { height:20px; margin-top:30px; }",
     "(a)(outer(inner))"},

    {"padding-stops-escape",
     ".a { height:10px; } .outer { padding-top:1px; } .inner { height:20px; margin-top:30px; }",
     "(a)(outer(inner))"},

    {"overflow-stops-escape",
     ".a { height:10px; } .outer { overflow:hidden; } .inner { height:20px; margin-top:30px; }",
     "(a)(outer(inner))"},

    {"last-child-escapes-down",
     ".outer { } .inner { height:20px; margin-bottom:30px; } .c { height:10px; }",
     "(outer(inner))(c)"},

    {"stated-height-stops-escape",
     ".outer { height:40px; } .inner { height:20px; margin-bottom:30px; } .c { height:10px; }",
     "(outer(inner))(c)"},

    {"empty-collapses-through",
     ".a { height:10px; } .b { margin-top:20px; margin-bottom:30px; } .c { height:10px; }",
     "(a)(b)(c)"},

    {"two-empties",
     ".a { height:10px; } .b { margin-top:20px; margin-bottom:5px; }"
     " .c { height:10px; margin-top:40px; }",
     "(a)(b)(c)"},

    {"nested-escape-two-deep",
     ".a { height:10px; } .outer { } .inner { } .b { height:20px; margin-top:30px; }",
     "(a)(outer(inner(b)))"},

    {"auto-height-with-padding",
     ".outer { padding:5px; } .a { height:20px; margin-bottom:20px; }"
     " .b { height:30px; margin-top:10px; }",
     "(outer(a)(b))"},

    {"horizontal-margins", ".a { height:10px; margin-left:20px; margin-right:30px; }", "(a)"},

    {"percentage-width", ".a { height:10px; width:50%; }", "(a)"},

    {"root-first-child-margin", ".a { height:20px; margin-top:20px; }", "(a)"},

    /* ------------------------------------------------------------------
     * Inline formatting. Items are atomic -- inline-block, not inline --
     * so none of these ask a box to split across two lines.
     * ------------------------------------------------------------------ */
    {"inline-share-a-line", ".i { display:inline-block; width:40px; height:10px; }", "(i)(i)"},

    {"inline-wrap", ".outer { width:100px; } .i { display:inline-block; width:40px; height:10px; }",
     "(outer(i)(i)(i))"},

    {"inline-margins",
     ".outer { width:100px; }"
     " .i { display:inline-block; width:40px; height:10px; margin-right:10px; }",
     "(outer(i)(i))"},

    {"inline-baselines",
     ".a { display:inline-block; width:20px; height:20px; }"
     " .b { display:inline-block; width:20px; height:40px; }",
     "(a)(b)"},

    {"inline-valign",
     ".outer { } .a { display:inline-block; width:20px; height:40px; }"
     " .b { display:inline-block; width:20px; height:10px; vertical-align:top; }"
     " .c { display:inline-block; width:20px; height:10px; vertical-align:bottom; }",
     "(a)(b)(c)"},

    {"inline-align-right",
     ".outer { width:100px; text-align:right; }"
     " .i { display:inline-block; width:40px; height:10px; }",
     "(outer(i))"},

    {"inline-align-center",
     ".outer { width:100px; text-align:center; }"
     " .i { display:inline-block; width:40px; height:10px; }",
     "(outer(i))"},

    /* ------------------------------------------------------------------
     * Floats.
     * ------------------------------------------------------------------ */
    {"float-sides",
     ".l { float:left; width:40px; height:20px; } .r { float:right; width:30px; height:20px; }",
     "(l)(r)"},

    {"float-stack-then-drop", ".outer { width:100px; } .l { float:left; width:40px; height:20px; }",
     "(outer(l)(l)(l))"},

    {"float-does-not-narrow-a-block",
     ".l { float:left; width:40px; height:20px; } .a { height:10px; }", "(l)(a)"},

    {"float-narrows-the-lines",
     ".l { float:left; width:60px; height:20px; }"
     " .i { display:inline-block; width:80px; height:10px; }",
     "(l)(i)(i)"},

    {"float-out-of-flow",
     ".a { height:10px; } .l { float:left; width:40px; height:50px; } .c { height:10px; }",
     "(a)(l)(c)"},

    {"float-clear",
     ".l { float:left; width:40px; height:50px; } .r { float:right; width:40px; height:80px; }"
     " .a { height:10px; clear:left; } .c { height:10px; clear:both; }",
     "(l)(r)(a)(c)"},

    {"float-not-contained", ".outer { } .l { float:left; width:40px; height:50px; }", "(outer(l))"},

    {"float-contained", ".bfc { overflow:hidden; } .l { float:left; width:40px; height:50px; }",
     "(bfc(l))"},

    {"bfc-avoids-a-float",
     ".l { float:left; width:40px; height:50px; } .bfc { overflow:hidden; height:20px; }",
     "(l)(bfc)"},

    {"float-margins",
     ".a { height:10px; margin-bottom:20px; }"
     " .l { float:left; width:40px; height:20px; margin-top:20px; }"
     " .c { height:10px; margin-top:20px; }",
     "(a)(l)(c)"},

    /* ------------------------------------------------------------------
     * Intrinsic sizing. No text: the corpus compares geometry, and two
     * rasterizers never agree on a string's width to the pixel.
     * ------------------------------------------------------------------ */
    {"min-content-stack",
     ".outer { width:min-content; } .a { width:30px; height:10px; }"
     " .b { width:50px; height:10px; }",
     "(outer(a)(b))"},

    {"max-content-stack",
     ".outer { width:max-content; } .a { width:30px; height:10px; }"
     " .b { width:50px; height:10px; }",
     "(outer(a)(b))"},

    {"fit-content-clamped-up",
     ".narrow { width:20px; } .outer { width:fit-content; }"
     " .a { width:50px; height:10px; }",
     "(narrow(outer(a)))"},

    {"fit-content-in-between",
     ".narrow { width:80px; } .outer { width:fit-content; }"
     " .a { width:50px; height:10px; }",
     "(narrow(outer(a)))"},

    {"min-content-padding",
     ".outer { width:min-content; padding:0 7px; } .a { width:30px; height:10px; }", "(outer(a))"},

    /* ------------------------------------------------------------------
     * Positioning.
     * ------------------------------------------------------------------ */
    {"relative-keeps-its-space",
     ".a { height:10px; } .r { height:10px; position:relative; top:5px; left:7px; }"
     " .c { height:10px; }",
     "(a)(r)(c)"},

    {"relative-moves-children", ".outer { position:relative; left:10px; } .a { height:10px; }",
     "(outer(a))"},

    {"absolute-padding-box",
     ".outer { position:relative; height:80px; padding:9px; }"
     " .abs { position:absolute; top:0; left:0; width:10px; height:10px; }",
     "(outer(abs))"},

    {"absolute-out-of-flow",
     ".a { height:10px; } .abs { position:absolute; width:50px; height:50px; }"
     " .c { height:10px; }",
     "(a)(abs)(c)"},

    {"absolute-static-position",
     ".a { height:10px; } .abs { position:absolute; width:50px; height:20px; }", "(a)(abs)"},

    {"absolute-four-edges",
     ".outer { position:relative; height:100px; }"
     " .abs { position:absolute; left:10px; right:30px; top:5px; bottom:15px; }",
     "(outer(abs))"},

    {"absolute-far-edges",
     ".outer { position:relative; height:100px; }"
     " .abs { position:absolute; right:10px; bottom:20px; width:30px; height:15px; }",
     "(outer(abs))"},

    {"absolute-skips-unpositioned",
     ".outer { position:relative; height:100px; } .narrow { padding:12px; }"
     " .abs { position:absolute; top:0; left:0; width:10px; height:10px; }",
     "(outer(narrow(abs)))"},

    {"absolute-auto-margins",
     ".outer { position:relative; height:100px; }"
     " .abs { position:absolute; left:0; right:0; top:0; bottom:0;"
     "        width:60px; height:20px; margin:auto; }",
     "(outer(abs))"},

    {"inline-in-the-stack",
     ".a { height:10px; } .i { display:inline-block; width:40px; height:20px; }"
     " .c { height:10px; }",
     "(a)(i)(i)(c)"}};

#define CASE_COUNT ((ar_i32)(sizeof CASES / sizeof CASES[0]))

/* Walks the token string, declaring the tree it describes. */
static void build(ar_ctx *ui, const char *tree)
{
    char sel[16];

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
                strcpy(sel, "div.");
                memcpy(sel + 4, start, n);
                sel[4 + n] = 0;
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

static const char *CHROME_SHEET =
    "#app  { display:flex; flex-direction:row; background:#fcfaf6;"
    "        font-size:14px; color:#3a3733; }"
    ".rail { width:260px; display:flex; flex-direction:column; padding:14px;"
    "        gap:1px; background:#f4efe4; overflow:hidden; }"
    ".title { font-size:20px; color:#20201e; padding-bottom:4px; }"
    ".hint  { font-size:11px; color:#a09789; padding-bottom:10px; }";

static const char *CHROME_SHEET2 =
    ".case    { padding:4px 8px; font-size:12px; color:#6f685d; }"
    ".case:hover { background:#ece4d3; color:#20201e; }"
    ".case-on { padding:4px 8px; font-size:12px; color:#fdfaf3; background:#7a4a2a; }"
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

    win = ar_win_open("areole - the block corpus", WIN_W, WIN_H);
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
        ar_text(chrome, "div.title", "Block corpus");
        ar_text(chrome, "div.hint", "The cases the browser comparison checks.");
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

        /*
         * The case, into a sub-view of the same pixels. Rebuilt only when the
         * selection changes: its own context has no input and nothing in it
         * moves, so laying it out every frame would be work for nothing.
         */
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

        /*
         * Where the frame actually landed, asked of layout rather than
         * predicted. .frame is declared last and boxes are numbered in
         * declaration order, so it is the final node.
         *
         * This was two constants, CASE_X (RAIL_W + 20) and CASE_Y 84, and they
         * stopped agreeing with layout the moment box-sizing began defaulting
         * to content-box: .rail is width:260px with padding:14px, so it became
         * 288 wide and carried .page 28 px to the right. The case pixels stayed
         * where they were and every demo drew 29 px to the left of its own
         * frame, and 23 px below it. An inspector that lies about geometry is
         * worse than no inspector, and the only fix that cannot drift again is
         * to stop keeping a second copy of what layout already knows.
         */
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

static int run_dump(void)
{
    ar_surface s;
    ar_i32     k;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  block corpus\n", ar_version(), (long)s.w, (long)s.h);

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
    return run_window();
}
