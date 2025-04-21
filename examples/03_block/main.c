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
 * No window. Every case is a fixed 300x200 surface and there is nothing to
 * look at -- the output is the box rectangles.
 */
#include "areole.h"

#include <stdio.h>
#include <string.h>

static unsigned char g_memory[AR_MEM(128)];
static ar_u32        g_pixels[300 * 200];

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
                                ".inner { display:block; }";

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

    {"root-first-child-margin", ".a { height:20px; margin-top:20px; }", "(a)"}};

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

int main(void)
{
    ar_surface s;
    ar_i32     k;

    s.pixels = g_pixels;
    s.w = 300;
    s.h = 200;
    s.stride = 300;

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
