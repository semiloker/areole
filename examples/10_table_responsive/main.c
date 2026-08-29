/*
 * areole example 10 - the responsive table corpus
 * SPDX-License-Identifier: MIT
 *
 * A table you can scroll, with a header that stays and a column that stays,
 * checked against a browser rather than against my reading of the spec.
 *
 *     example_table_responsive --dump
 *     python tools/compare_layout.py --run ./build/example_table_responsive.exe \
 *            examples/10_table_responsive/responsive.html
 *
 * ------------------------------------------------------------------------
 * What is in here
 *
 *   a sticky header group, and a sticky row inside one
 *   a frozen first column, and both frozen at once
 *   `visibility: collapse` on a row, a column and a column group
 *   `caption-side`, both ways
 *   `empty-cells`, both ways
 *   `col` and `colgroup` stating a width
 *
 * Each of those at four scroll positions, because a sticky box that never
 * moves is indistinguishable from one that was placed correctly by accident.
 * The scroll is set programmatically rather than driven, so both engines are
 * asked about the same offset rather than about the same gesture.
 *
 * ------------------------------------------------------------------------
 * Why the cells state their sizes
 *
 * Everything here is about where a box ends up, not how wide it decided to
 * be -- that is the 09 corpus's question and it is answered there. Fixing the
 * sizes means a disagreement in this file is a disagreement about sticky,
 * about collapse, or about the caption, and not about column widths arriving
 * from somewhere else.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 240
#define CASE_H 160
#define WIN_W  560
#define WIN_H  360

static unsigned char g_memory[AR_MEM(256)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * In pieces: C89 guarantees only 509 characters in a string literal, and
 * adjacent literals are joined before that limit applies.
 */
static const char *SHEET_A = "#root { display:block; background:#ffffff; }"
                             ".port { display:block; width:220px; height:120px;"
                             "        overflow:auto; background:#faf7f0; }"
                             ".t { display:table; width:360px; }"
                             ".g { display:table-row-group; }"
                             ".h { display:table-header-group; }";

static const char *SHEET_B = ".r { display:table-row; }"
                             ".cap { display:table-caption; height:14px; background:#e8dfcb; }"
                             ".lg  { display:table-column-group; }"
                             ".co  { display:table-column; }"
                             ".c { display:table-cell; width:90px; height:22px;"
                             "     background:#cfd8e3; }";

static const char *SHEET_C = ".hd { background:#b9c8dc; }"
                             ".first { background:#c8d6c2; }"
                             ".stick-h { position:sticky; top:0px; }"
                             ".stick-c { position:sticky; left:0px; }"
                             ".gone { visibility:collapse; }"
                             ".under { caption-side:bottom; }"
                             ".hollow { empty-cells:hide; }"
                             ".w60 { width:60px; }";

typedef struct
{
    const char *name;
    const char *css; /* extra rules, scoped to this case by the twin */
    int         head_sticky;
    int         col_sticky;
} rcase;

/*
 * Twelve shapes. The two sticky flags are separate from the extra CSS because
 * both engines have to be told the same thing and a class is the only way to
 * say it that survives being scoped.
 */
static const rcase CASES[] = {
    /* the controls: nothing sticky, nothing collapsed */
    {"plain", "", 0, 0},
    {"caption", "", 0, 0},

    {"head", "", 1, 0},
    {"col", "", 0, 1},
    {"both", "", 1, 1},

    {"row-gone", ".r3 { visibility:collapse; }", 0, 0},
    {"row-gone-sticky", ".r3 { visibility:collapse; }", 1, 0},
    {"col-gone", ".co2 { visibility:collapse; }", 0, 0},
    {"colgroup-gone", ".lg2 { visibility:collapse; }", 0, 0},

    {"caption-under", ".cap { caption-side:bottom; }", 0, 0},
    {"empty-hidden", ".t { empty-cells:hide; }", 0, 0},
    {"col-widths", ".co1 { width:60px; } .co2 { width:150px; }", 0, 0}};

#define CASE_COUNT ((ar_i32)(sizeof CASES / sizeof CASES[0]))

/* Four offsets, both axes at once, so a case cannot pass by being right on
   one of them. */
typedef struct
{
    ar_i32 x, y;
} at;

static const at SCROLLS[] = {{0, 0}, {0, 34}, {40, 0}, {70, 58}};

#define SCROLL_COUNT ((ar_i32)(sizeof SCROLLS / sizeof SCROLLS[0]))
#define PAGE_COUNT   (CASE_COUNT * SCROLL_COUNT)

/* ------------------------------------------------------------------------
 * Building a case
 * ------------------------------------------------------------------------ */

static void build(ar_ctx *ui, const rcase *k)
{
    char sel[64];
    int  r, c;

    ar_begin(ui, "div.port");
    ar_begin(ui, "div.t");

    /* No text and no child. Everything in this file is about where a box ends
       up, and a box sized by its own font is the one thing the two engines
       cannot agree on -- the 02 tour is where that difference is measured. */
    ar_begin(ui, "div.cap");
    ar_end(ui);

    /* Two column groups of two columns each, so a case can close one column
       or a whole group and the two are told apart. */
    ar_begin(ui, "div.lg.lg1");
    ar_begin(ui, "div.co.co1");
    ar_end(ui);
    ar_begin(ui, "div.co.co2");
    ar_end(ui);
    ar_end(ui);
    ar_begin(ui, "div.lg.lg2");
    ar_begin(ui, "div.co.co3");
    ar_end(ui);
    ar_begin(ui, "div.co.co4");
    ar_end(ui);
    ar_end(ui);

    strcpy(sel, k->head_sticky ? "div.h.stick-h" : "div.h");
    ar_begin(ui, sel);
    ar_begin(ui, "div.r");
    for (c = 0; c < 4; ++c)
    {
        strcpy(sel, "div.c.hd");
        if (c == 0 && k->col_sticky)
        {
            strcat(sel, ".first.stick-c");
        }
        else if (c == 0)
        {
            strcat(sel, ".first");
        }
        ar_text(ui, sel, "h");
    }
    ar_end(ui);
    ar_end(ui);

    ar_begin(ui, "div.g");
    for (r = 0; r < 5; ++r)
    {
        sprintf(sel, "div.r.r%d", r);
        ar_begin(ui, sel);
        for (c = 0; c < 4; ++c)
        {
            strcpy(sel, "div.c");
            if (c == 0 && k->col_sticky)
            {
                strcat(sel, ".first.stick-c");
            }
            else if (c == 0)
            {
                strcat(sel, ".first");
            }
            /* The third cell of the third row is left empty on purpose, so
               `empty-cells: hide` has something to hide. */
            if (r == 2 && c == 2)
            {
                ar_begin(ui, sel);
                ar_end(ui);
            }
            else
            {
                ar_text(ui, sel, "x");
            }
        }
        ar_end(ui);
    }
    ar_end(ui);

    ar_end(ui); /* table */
    ar_end(ui); /* port */
}

static ar_ctx *settle(ar_i32 which, ar_i32 scroll, ar_surface *s)
{
    char     sheet[1400];
    ar_ctx  *ui;
    ar_input in;
    ar_i32   port;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    strcpy(sheet, SHEET_A);
    strcat(sheet, SHEET_B);
    strcat(sheet, SHEET_C);
    strcat(sheet, CASES[which].css);
    ar_stylesheet(ui, sheet);
    if (ar_stylesheet_errors(ui))
    {
        return 0;
    }

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    /* One frame to build the tree and settle the content size, so there is
       something to scroll, and a second at the offset. A scroll position is
       state on a box, and the box has to exist before it can be given one. */
    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    build(ui, &CASES[which]);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    port = 1; /* #root is 0, .port is its first child */
    ar_node_scroll_to(ui, port, SCROLLS[scroll].y);
    ar_node_scroll_to_x(ui, port, SCROLLS[scroll].x);

    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    build(ui, &CASES[which]);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    return ar_unbalanced(ui) ? 0 : ui;
}

/* ------------------------------------------------------------------------
 * The dump, in the tree the caller described
 * ------------------------------------------------------------------------ */

#define MAX_NODES 512

static ar_i32 g_dom_index[MAX_NODES];
static ar_i32 g_dom_count[MAX_NODES];

static ar_i32 dom_parent(const ar_ctx *c, ar_i32 i)
{
    ar_i32 at2 = ar_node_parent(c, i);

    while (at2 >= 0 && ar_node_generated(c, at2))
    {
        at2 = ar_node_parent(c, at2);
    }
    return at2;
}

static void number_nodes(const ar_ctx *c)
{
    ar_i32 n = ar_node_count(c);
    ar_i32 i;

    for (i = 0; i < n && i < MAX_NODES; ++i)
    {
        g_dom_index[i] = -1;
        g_dom_count[i] = 0;
    }
    for (i = 0; i < n && i < MAX_NODES; ++i)
    {
        ar_i32 p;

        if (ar_node_generated(c, i))
        {
            continue;
        }
        p = dom_parent(c, i);
        if (p < 0)
        {
            g_dom_index[i] = 0;
            continue;
        }
        g_dom_index[i] = g_dom_count[p]++;
    }
}

static void dump_path(const ar_ctx *c, ar_i32 i, char *out)
{
    ar_i32 stack[32];
    ar_i32 depth = 0;
    ar_i32 at2 = i;
    char  *w = out;

    while (at2 >= 0 && depth < 32)
    {
        stack[depth++] = at2;
        at2 = dom_parent(c, at2);
    }
    while (depth > 0)
    {
        --depth;
        if (w != out)
        {
            *w++ = '/';
        }
        sprintf(w, "%ld", (long)g_dom_index[stack[depth]]);
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
    ar_i32     k, v;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  responsive table corpus\n", ar_version(), (long)s.w,
           (long)s.h);

    for (k = 0; k < CASE_COUNT; ++k)
    {
        for (v = 0; v < SCROLL_COUNT; ++v)
        {
            char    name[64];
            ar_ctx *ui = settle(k, v, &s);
            ar_rect origin;
            ar_i32  i;

            sprintf(name, "%s-s%ld", CASES[k].name, (long)v);
            if (!ui)
            {
                printf("# %s: did not lay out\n", name);
                return 1;
            }

            printf("# page %s\n", name);
            number_nodes(ui);
            origin = ar_node_rect(ui, 0);

            for (i = 0; i < ar_node_count(ui) && i < MAX_NODES; ++i)
            {
                char    path[160];
                ar_rect r;

                if (ar_node_generated(ui, i))
                {
                    continue;
                }
                r = ar_node_rect(ui, i);
                dump_path(ui, i, path);
                printf("%s %ld %ld %ld %ld |\n", path, (long)(r.x - origin.x),
                       (long)(r.y - origin.y), (long)r.w, (long)r.h);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The window, for looking at a case rather than diffing it
 * ------------------------------------------------------------------------ */

static int run_window(void)
{
    ar_win *win;
    ar_i32  page = 0;

    win = ar_win_open("areole - the responsive table corpus", WIN_W, WIN_H);
    if (!win)
    {
        return 1;
    }

    while (ar_win_pump(win))
    {
        const ar_input *in = ar_win_input(win);
        ar_surface     *w = ar_win_surface(win);
        ar_surface      view;
        ar_ctx         *ui;
        char            title[160];
        ar_i32          i, k, v;

        if (in->keys_pressed & AR_KEY_RIGHT)
        {
            page = (page + 1) % PAGE_COUNT;
        }
        if (in->keys_pressed & AR_KEY_LEFT)
        {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
        }

        for (i = 0; i < w->w * w->h; ++i)
        {
            w->pixels[i] = 0xFF201C18u;
        }

        view.pixels = w->pixels + 40 * w->stride + 40;
        view.w = CASE_W;
        view.h = CASE_H;
        view.stride = w->stride;

        k = page / SCROLL_COUNT;
        v = page % SCROLL_COUNT;
        ui = settle(k, v, &view);
        sprintf(title, "areole - responsive %ld/%ld  %s at %ld,%ld%s", (long)(page + 1),
                (long)PAGE_COUNT, CASES[k].name, (long)SCROLLS[v].x, (long)SCROLLS[v].y,
                ui ? "" : "  (did not lay out)");
        ar_win_set_title(win, title);

        ar_win_present(win, ar_rect_make(0, 0, w->w, w->h));
    }

    ar_win_close(win);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0)
    {
        return run_dump();
    }
    return run_window();
}
