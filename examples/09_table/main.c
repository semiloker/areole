/*
 * areole example 09 - the table corpus
 * SPDX-License-Identifier: MIT
 *
 * Where the boxes of a table land, checked against a browser rather than
 * against my reading of chapter 17.
 *
 *     example_table --dump
 *     python tools/compare_layout.py --run ./build/example_table.exe \
 *            examples/09_table/table.html
 *
 * A real comparison rather than a substitution: everything here is `display`
 * on plain divs, so the browser runs the same CSS table algorithm on the same
 * tree instead of being told the answer.
 *
 * ------------------------------------------------------------------------
 * Three criteria in one corpus
 *
 *   forty malformed-markup cases    the anonymous boxes, by their effect
 *   thirty border conflicts         which border wins a collapsed grid line
 *   one twenty-column colspan       the constraint solve at width
 *
 * ------------------------------------------------------------------------
 * Generated boxes are not in the dump
 *
 * A cell declared straight inside a table needs a row to live in, and areole
 * makes one -- a real box with a real rectangle. A browser makes one too, and
 * a browser's is not an element and never appears in a walk of the DOM. So the
 * dump below skips them and numbers the paths as the caller's own tree, which
 * is the only way the two can be compared at all.
 *
 * That is not a weaker test. If the generated boxes are wrong, the declared
 * ones land somewhere else, and the declared ones are what is compared.
 *
 * ------------------------------------------------------------------------
 * Why every cell is given a width
 *
 * The malformed cases are about structure. Left to shrink-wrap, each one would
 * also be a test of how surplus width is distributed, and a single disagreement
 * there would fail all forty for a reason that has nothing to do with any of
 * them. Every cell is 40 wide and the table is 240, which divides exactly by
 * every column count these cases produce.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 260
#define CASE_H 200
#define WIN_W  560
#define WIN_H  360

static unsigned char g_memory[AR_MEM(256)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * The element vocabulary, one class per display value.
 *
 * In pieces: C89 guarantees only 509 characters in a string literal, and
 * adjacent literals are joined before that limit applies.
 */
static const char *SHEET_A = "#root { display:block; background:#ffffff; }"
                             ".t { display:table; width:240px; background:#f4f1ec; }"
                             ".g { display:table-row-group; }"
                             ".h { display:table-header-group; }"
                             ".f { display:table-footer-group; }";

static const char *SHEET_B = ".r { display:table-row; }"
                             ".p { display:table-caption; height:14px; background:#e6dfd4; }"
                             ".l { display:table-column-group; }"
                             ".o { display:table-column; }"
                             ".d { display:block; height:12px; background:#dfe6df; }";

static const char *SHEET_C = ".c { display:table-cell; width:40px; height:20px;"
                             "     background:#cfd8e3; }"
                             ".x { colspan:2; }"
                             ".y { rowspan:2; }"
                             ".z { colspan:2; rowspan:2; }";

/*
 * A case is a tree written out as one string.
 *
 *   t table   g tbody   h thead   f tfoot   r row   c cell
 *   p caption l colgroup o col    d block
 *   x cell with colspan 2   y with rowspan 2   z with both
 *   [ ] open and close a box's children
 *
 * Forty of them, and every one is markup somebody has actually written: a cell
 * with no row, a row with no table, a block where a cell should be, a footer
 * declared before the header it follows.
 */
typedef struct
{
    const char *name;
    const char *tree;
} tcase;

static const tcase SHAPES[] = {
    /* the well-formed controls -- three of them, because a corpus of nothing
       but malformed cases cannot tell a fix-up from a coincidence */
    {"ok-simple", "t[r[c c]]"},
    {"ok-groups", "t[h[r[c c]] g[r[c c]] f[r[c c]]]"},
    {"ok-two-rows", "t[g[r[c c] r[c c]]]"},

    /* a cell with nothing to live in */
    {"bare-cell", "t[c]"},
    {"bare-cells", "t[c c]"},
    {"bare-cells-three", "t[c c c]"},
    {"row-then-bare", "t[r[c] c]"},
    {"bare-then-row", "t[c r[c]]"},
    {"bare-in-group", "t[g[c]]"},
    {"bare-in-group-after-row", "t[g[r[c] c]]"},
    {"bare-cell-no-table", "c"},
    {"bare-cells-no-table", "c c"},

    /* a row with nothing to live in */
    {"row-no-table", "r[c c]"},
    {"group-no-table", "g[r[c c]]"},
    {"row-in-row", "t[r[r[c]]]"},

    /* ordinary content where the model wants a cell */
    {"block-in-table", "t[d]"},
    {"block-then-cell", "t[d c]"},
    {"cell-then-block", "t[c d]"},
    {"block-in-row", "t[r[d]]"},
    {"cell-then-block-in-row", "t[r[c d]]"},
    {"block-then-cell-in-row", "t[r[d c]]"},
    {"block-in-group", "t[g[d]]"},
    {"cell-inside-block", "t[d[c]]"},

    /* the empties */
    {"empty-table", "t[]"},
    {"empty-row", "t[r[] r[c]]"},
    {"empty-group", "t[g[] r[c]]"},
    {"empty-then-cells", "t[r[] r[c c]]"},

    /* order that is not drawing order */
    {"footer-first", "t[f[r[c c]] h[r[c c]] g[r[c c]]]"},
    {"group-before-header", "t[g[r[c c]] h[r[c c]]]"},
    {"footer-only", "t[f[r[c c]]]"},
    {"bare-row-among-groups", "t[h[r[c c]] r[c c] f[r[c c]]]"},

    /* captions and columns */
    {"caption-first", "t[p[d] r[c c]]"},
    {"caption-last", "t[r[c c] p[d]]"},
    {"two-captions", "t[p[d] p[d] r[c c]]"},
    {"colgroup", "t[l[o o] r[c c]]"},
    {"two-colgroups", "t[l[o] l[o] r[c c]]"},
    {"bare-col", "t[o r[c c]]"},

    /* ragged, and spanning */
    {"ragged-short", "t[r[c c] r[c]]"},
    {"ragged-long", "t[r[c] r[c c]]"},

    /*
     * The spanning cases carry their row group in the string.
     *
     * colspan and rowspan are HTML attributes, and a browser ignores them on a
     * div with `display: table-cell` -- so the twin builds these five out of
     * real elements, and a browser's parser puts a real `tbody` in the DOM
     * whether one was written or not. Writing it here makes the two trees the
     * same shape; areole's own generated group is skipped by the dump and this
     * one is not, because it was declared.
     */
    {"colspan-then-cells", "t[g[r[x] r[c c]]]"},
    {"colspan-overflow", "t[g[r[c x]]]"},
    {"rowspan-first", "t[g[r[y c] r[c]]]"},
    {"rowspan-second", "t[g[r[c y] r[c]]]"},
    {"colspan-and-rowspan", "t[g[r[z c] r[c] r[c c]]]"},

    /* tables inside tables */
    {"table-in-table", "t[t[r[c]]]"},
    {"table-in-cell", "t[r[c[t[r[c c]]]]]"},
    {"deep-content", "t[r[c[d[d]]]]"}};

#define SHAPE_COUNT ((ar_i32)(sizeof SHAPES / sizeof SHAPES[0]))

/*
 * Thirty collapsed-border conflicts.
 *
 * CSS resolves a grid line by style first, then width, then the
 * cell/row/group/column/table origin order. areole has one uniform
 * border-width per box and no border-style at all, so style precedence has
 * nothing to resolve here and these thirty are width and origin: every pair of
 * neighbours that can disagree, disagreeing.
 *
 * All of them run on one shape so the only variable is the border.
 */
static const tcase CONFLICTS[] = {
    {"sep-none", ""},
    {"sep-cells", ".c { border:2px #b03030; }"},
    {"sep-spacing", ".c { border:2px #b03030; } .t { border-spacing:6px; }"},

    {"col-plain", ".t { border-collapse:collapse; }"},
    {"col-cells1", ".t { border-collapse:collapse; } .c { border:1px #b03030; }"},
    {"col-cells2", ".t { border-collapse:collapse; } .c { border:2px #b03030; }"},
    {"col-cells3", ".t { border-collapse:collapse; } .c { border:3px #b03030; }"},
    {"col-cells6", ".t { border-collapse:collapse; } .c { border:6px #b03030; }"},

    {"col-one-wider", ".t { border-collapse:collapse; } .c { border:1px #b03030; }"
                      ".k0 { border:5px #3060b0; }"},
    {"col-one-thinner", ".t { border-collapse:collapse; } .c { border:4px #b03030; }"
                        ".k0 { border:1px #3060b0; }"},
    {"col-last-wider", ".t { border-collapse:collapse; } .c { border:1px #b03030; }"
                       ".k1 { border:5px #3060b0; }"},
    {"col-both-wide", ".t { border-collapse:collapse; } .c { border:1px #b03030; }"
                      ".k0 { border:5px #3060b0; } .k1 { border:5px #30b060; }"},
    {"col-tie", ".t { border-collapse:collapse; } .c { border:3px #b03030; }"
                ".k0 { border:3px #3060b0; }"},

    {"col-table1", ".t { border-collapse:collapse; border:1px #806040; }"
                   ".c { border:1px #b03030; }"},
    {"col-table4", ".t { border-collapse:collapse; border:4px #806040; }"
                   ".c { border:1px #b03030; }"},
    {"col-table-loses", ".t { border-collapse:collapse; border:1px #806040; }"
                        ".c { border:5px #b03030; }"},
    {"col-table-alone", ".t { border-collapse:collapse; border:4px #806040; }"},

    {"col-row2", ".t { border-collapse:collapse; } .r { border:2px #40806a; }"
                 ".c { border:1px #b03030; }"},
    {"col-row6", ".t { border-collapse:collapse; } .r { border:6px #40806a; }"
                 ".c { border:1px #b03030; }"},
    {"col-row-loses", ".t { border-collapse:collapse; } .r { border:1px #40806a; }"
                      ".c { border:4px #b03030; }"},
    {"col-row-alone", ".t { border-collapse:collapse; } .r { border:3px #40806a; }"},

    {"col-group3", ".t { border-collapse:collapse; } .g { border:3px #6a4080; }"
                   ".c { border:1px #b03030; }"},
    {"col-group-loses", ".t { border-collapse:collapse; } .g { border:1px #6a4080; }"
                        ".c { border:4px #b03030; }"},
    {"col-group-alone", ".t { border-collapse:collapse; } .g { border:5px #6a4080; }"},

    {"col-all-one", ".t { border-collapse:collapse; border:1px #806040; }"
                    ".g { border:1px #6a4080; } .r { border:1px #40806a; }"
                    ".c { border:1px #b03030; }"},
    {"col-table-widest", ".t { border-collapse:collapse; border:7px #806040; }"
                         ".g { border:2px #6a4080; } .r { border:3px #40806a; }"
                         ".c { border:4px #b03030; }"},
    {"col-cell-widest", ".t { border-collapse:collapse; border:2px #806040; }"
                        ".g { border:3px #6a4080; } .r { border:4px #40806a; }"
                        ".c { border:7px #b03030; }"},
    {"col-row-widest", ".t { border-collapse:collapse; border:2px #806040; }"
                       ".g { border:3px #6a4080; } .r { border:7px #40806a; }"
                       ".c { border:4px #b03030; }"},
    {"col-odd-widths", ".t { border-collapse:collapse; } .c { border:3px #b03030; }"
                       ".k1 { border:5px #3060b0; }"},
    {"col-spacing-ignored", ".t { border-collapse:collapse; border-spacing:9px; }"
                            ".c { border:2px #b03030; }"}};

#define CONFLICT_COUNT ((ar_i32)(sizeof CONFLICTS / sizeof CONFLICTS[0]))

/* Two rows of two, in a row group, with a class per column so a case can give
   one column a different border without a positional selector. */
static const char *CONFLICT_TREE = "t[g[r[c c] r[c c]]]";

/* ------------------------------------------------------------------------
 * Building a case from its string
 * ------------------------------------------------------------------------ */

static const char *class_of(char ch)
{
    switch (ch)
    {
    case 't':
        return "div.t";
    case 'g':
        return "div.g";
    case 'h':
        return "div.h";
    case 'f':
        return "div.f";
    case 'r':
        return "div.r";
    case 'c':
        return "div.c";
    case 'p':
        return "div.p";
    case 'l':
        return "div.l";
    case 'o':
        return "div.o";
    case 'd':
        return "div.d";
    case 'x':
        return "div.c.x";
    case 'y':
        return "div.c.y";
    case 'z':
        return "div.c.z";
    default:
        return 0;
    }
}

/*
 * Walk the string, opening a box per letter and closing it at its `]` -- or
 * immediately, when nothing follows it.
 *
 * `open[]` remembers whether the box at each depth is still waiting for its
 * bracket, so `c c` closes each cell as the next letter arrives while `r[...]`
 * waits.
 */
static void build(ar_ctx *ui, const char *tree, int number_columns)
{
    int         open[24];
    int         depth = 0;
    int         col = 0;
    char        sel[24];
    const char *at;

    for (at = tree; *at; ++at)
    {
        if (*at == ' ')
        {
            continue;
        }
        if (*at == '[')
        {
            if (depth > 0)
            {
                open[depth - 1] = 1;
            }
            continue;
        }
        if (*at == ']')
        {
            /* Everything above the box whose bracket this is has ended, and so
               has that box. Closing only one was the first version and quietly
               nested every group inside the one before it. */
            while (depth > 0 && !open[depth - 1])
            {
                --depth;
                ar_end(ui);
            }
            if (depth > 0)
            {
                --depth;
                ar_end(ui);
            }
            continue;
        }

        /* A letter. Anything on top of the stack that is not waiting for a
           bracket has ended. */
        while (depth > 0 && !open[depth - 1])
        {
            --depth;
            ar_end(ui);
        }

        {
            const char *base = class_of(*at);

            if (!base)
            {
                continue;
            }
            strcpy(sel, base);
            if (number_columns && (*at == 'c' || *at == 'x' || *at == 'y' || *at == 'z'))
            {
                /* .k0, .k1 ... so a conflict case can single out one column. */
                sprintf(sel + strlen(sel), ".k%d", col % 4);
                ++col;
            }
            if (*at == 'r')
            {
                col = 0;
            }
            ar_begin(ui, sel);
            if (depth < 24)
            {
                open[depth] = 0;
                ++depth;
            }
        }
    }
    while (depth > 0)
    {
        --depth;
        ar_end(ui);
    }
}

static ar_ctx *settle(const char *tree, const char *extra, int number_columns, ar_surface *s)
{
    char     sheet[1400];
    ar_ctx  *ui;
    ar_input in;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    strcpy(sheet, SHEET_A);
    strcat(sheet, SHEET_B);
    strcat(sheet, SHEET_C);
    strcat(sheet, extra);
    ar_stylesheet(ui, sheet);
    if (ar_stylesheet_errors(ui))
    {
        return 0;
    }

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    build(ui, tree, number_columns);
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

/* The nearest ancestor that the caller actually declared. A generated box is
   transparent: its children are, to the caller, children of its parent. */
static ar_i32 dom_parent(const ar_ctx *c, ar_i32 i)
{
    ar_i32 at = ar_node_parent(c, i);

    while (at >= 0 && ar_node_generated(c, at))
    {
        at = ar_node_parent(c, at);
    }
    return at;
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
    /* Pre-order, so a parent is always numbered before its children. */
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
    ar_i32 at = i;
    char  *w = out;

    while (at >= 0 && depth < 32)
    {
        stack[depth++] = at;
        at = dom_parent(c, at);
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

static int dump_one(const char *name, const char *tree, const char *extra, int number_columns,
                    ar_surface *s)
{
    ar_ctx *ui = settle(tree, extra, number_columns, s);
    ar_rect origin;
    ar_i32  i;

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
        printf("%s %ld %ld %ld %ld |\n", path, (long)(r.x - origin.x), (long)(r.y - origin.y),
               (long)r.w, (long)r.h);
    }
    return 0;
}

/* The twenty-column table criterion 6 asks for, built rather than written out:
   twenty columns, and a spanning cell every third one so no two rows agree. */
static void wide_tree(char *out)
{
    char *w = out;
    int   row, i;

    strcpy(w, "t[g[");
    w += strlen(w);
    for (row = 0; row < 4; ++row)
    {
        *w++ = 'r';
        *w++ = '[';
        for (i = 0; i < 20;)
        {
            if ((i + row) % 3 == 0 && i + 2 <= 20)
            {
                *w++ = 'x';
                i += 2;
            }
            else
            {
                *w++ = 'c';
                i += 1;
            }
            *w++ = ' ';
        }
        *w++ = ']';
    }
    strcpy(w, "]]");
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     i;
    char       wide[256];

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  table corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (i = 0; i < SHAPE_COUNT; ++i)
    {
        if (dump_one(SHAPES[i].name, SHAPES[i].tree, "", 0, &s))
        {
            return 1;
        }
    }
    for (i = 0; i < CONFLICT_COUNT; ++i)
    {
        if (dump_one(CONFLICTS[i].name, CONFLICT_TREE, CONFLICTS[i].tree, 1, &s))
        {
            return 1;
        }
    }
    wide_tree(wide);
    if (dump_one("wide-20", wide, ".t { width:800px; }", 0, &s))
    {
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The window, for looking at a case rather than diffing it
 * ------------------------------------------------------------------------ */

#define PAGE_COUNT (SHAPE_COUNT + CONFLICT_COUNT)

static void page_of(ar_i32 page, const char **name, const char **tree, const char **extra,
                    int *numbered)
{
    if (page < SHAPE_COUNT)
    {
        *name = SHAPES[page].name;
        *tree = SHAPES[page].tree;
        *extra = "";
        *numbered = 0;
    }
    else
    {
        *name = CONFLICTS[page - SHAPE_COUNT].name;
        *tree = CONFLICT_TREE;
        *extra = CONFLICTS[page - SHAPE_COUNT].tree;
        *numbered = 1;
    }
}

static int run_window(void)
{
    ar_win *win;
    ar_i32  page = 0;

    win = ar_win_open("areole - the table corpus", WIN_W, WIN_H);
    if (!win)
    {
        return 1;
    }

    while (ar_win_pump(win))
    {
        const ar_input *in = ar_win_input(win);
        ar_surface     *w = ar_win_surface(win);
        ar_surface      view;
        const char     *name;
        const char     *tree;
        const char     *extra;
        int             numbered;
        ar_ctx         *ui;
        char            title[160];
        ar_i32          i;

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

        page_of(page, &name, &tree, &extra, &numbered);
        ui = settle(tree, extra, numbered, &view);
        sprintf(title, "areole - table %ld/%ld  %s%s", (long)(page + 1), (long)PAGE_COUNT, name,
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
