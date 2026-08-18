/*
 * areole example 05 - the snap corpus
 * SPDX-License-Identifier: MIT
 *
 * Where a scroll container settles, checked against a browser rather than
 * against my reading of the specification.
 *
 *     example_snap --dump
 *     python tools/compare_layout.py --run ./build/example_snap.exe \
 *            examples/05_snap/snap.html
 *
 * Run with --dump it prints the rectangles and exits, which is what the
 * comparison drives and what CI runs. Run with no arguments it opens a window
 * and draws one case at a time, stepped with the left and right arrow keys,
 * because a corpus you can only read as numbers is a corpus nobody looks at.
 *
 * ------------------------------------------------------------------------
 * How a snap position becomes a rectangle
 *
 * ar_scroll_apply shifts a container's descendants by its scroll offset during
 * layout, so a row's rectangle already carries where the container settled.
 * getBoundingClientRect does the same thing on the other side. That means
 * neither dump has to say what the scroll offset is: the rows report it, and
 * the ordinary path-matched geometry comparison is the whole test.
 *
 * ------------------------------------------------------------------------
 * Why every case here is `mandatory`
 *
 * Two parts of scroll snapping are deliberately absent, and their absence is
 * the reason this corpus can claim a one pixel bound at all.
 *
 * `proximity` has no defined threshold. The specification leaves how near is
 * near enough to the user agent, so areole and Chrome are both correct while
 * disagreeing, and a case built on it would report a difference of opinion as
 * a failure. It is covered in ar_test.c against areole's own stated threshold
 * instead.
 *
 * `scroll-snap-stop: always` constrains passing over snap positions during a
 * scrolling movement. An instant programmatic scroll is not obviously such a
 * movement, browsers do not agree that it is one, and the specification does
 * not settle it. Also covered in ar_test.c, and left out of a comparison that
 * would be measuring an ambiguity.
 *
 * What is left -- mandatory snapping, three alignments, scroll-padding and
 * scroll-margin on both edges -- is specified exactly and is where the
 * arithmetic actually lives.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 200
#define CASE_H 200
#define WIN_W  520
#define WIN_H  320

/* Root, the list, and six rows. */
#define LIST_NODE 1
#define ROW_COUNT 6

static unsigned char g_memory[AR_MEM(128)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * Every case shares this. The list is 200 wide and its height is supplied per
 * size below, so a variant only ever says something about snapping.
 */
static const char *SHEET_BASE = "#root { display:block; background:#ffffff; }"
                                ".list { display:block; overflow:scroll; width:200px;"
                                "        background:#f4f2ee; }"
                                ".row  { display:block; height:40px; background:#cfd8e3; }";

typedef struct
{
    const char *name;
    const char *css;
} snap_variant;

/*
 * Eight variants. Each states mandatory snapping on the container and one
 * arrangement of alignment, scroll-padding and scroll-margin.
 */
static const snap_variant VARIANTS[] = {
    {"start", ".list { scroll-snap-type: y mandatory; }"
              ".row  { scroll-snap-align: start; }"},

    {"center", ".list { scroll-snap-type: y mandatory; }"
               ".row  { scroll-snap-align: center; }"},

    {"end", ".list { scroll-snap-type: y mandatory; }"
            ".row  { scroll-snap-align: end; }"},

    {"pad-start", ".list { scroll-snap-type: y mandatory; scroll-padding-top: 10px; }"
                  ".row  { scroll-snap-align: start; }"},

    {"pad-end", ".list { scroll-snap-type: y mandatory; scroll-padding-bottom: 12px; }"
                ".row  { scroll-snap-align: end; }"},

    {"margin-start", ".list { scroll-snap-type: y mandatory; }"
                     ".row  { scroll-snap-align: start; scroll-margin-top: 8px; }"},

    {"margin-end", ".list { scroll-snap-type: y mandatory; }"
                   ".row  { scroll-snap-align: end; scroll-margin-bottom: 6px; }"},

    {"pad-and-margin", ".list { scroll-snap-type: y mandatory; scroll-padding-top: 10px; }"
                       ".row  { scroll-snap-align: start; scroll-margin-top: 8px; }"}};

#define VARIANT_COUNT ((ar_i32)(sizeof VARIANTS / sizeof VARIANTS[0]))

/*
 * Five targets. 10 and 50 sit between snap points on purpose -- a target that
 * happened to be one would pass whether or not any snapping ran. 9999 is past
 * the end, which is the case a naive snap strands short of the bottom.
 */
static const ar_i32 TARGETS[] = {10, 50, 90, 130, 9999};
#define TARGET_COUNT ((ar_i32)(sizeof TARGETS / sizeof TARGETS[0]))

/* Three container sizes. 100 and 140 divide the 40 pixel rows differently, and
   130 divides none of them evenly, which is where an off-by-one in the
   alignment arithmetic shows up. */
static const ar_i32 HEIGHTS[] = {100, 130, 140};
#define HEIGHT_COUNT ((ar_i32)(sizeof HEIGHTS / sizeof HEIGHTS[0]))

#define CASE_COUNT (VARIANT_COUNT * TARGET_COUNT)
#define PAGE_COUNT (CASE_COUNT * HEIGHT_COUNT)

static void build(ar_ctx *ui)
{
    int i;

    ar_begin(ui, "div.list");
    for (i = 0; i < ROW_COUNT; ++i)
    {
        ar_begin(ui, "div.row");
        ar_end(ui);
    }
    ar_end(ui);
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

/*
 * One page: lay the case out, move it, lay it out again.
 *
 * The second frame is not optional. A scroll position is applied by the next
 * frame's layout pass, so a dump taken straight after ar_node_scroll_to would
 * report the rectangles from before the move.
 */
static ar_ctx *settle(ar_i32 variant, ar_i32 target, ar_i32 height, ar_surface *s)
{
    char     height_css[64];
    char     sheet[1024];
    ar_ctx  *ui;
    ar_input in;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    sprintf(height_css, ".list { height: %ldpx; }", (long)height);
    strcpy(sheet, SHEET_BASE);
    strcat(sheet, VARIANTS[variant].css);
    strcat(sheet, height_css);
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
    build(ui);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    ar_node_scroll_to(ui, LIST_NODE, target);

    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    build(ui);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    return ar_unbalanced(ui) ? 0 : ui;
}

static void page_name(ar_i32 variant, ar_i32 target, ar_i32 height, char *out)
{
    sprintf(out, "%s-t%ld-h%ld", VARIANTS[variant].name, (long)target, (long)height);
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     v, t, h;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  snap corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (v = 0; v < VARIANT_COUNT; ++v)
    {
        for (t = 0; t < TARGET_COUNT; ++t)
        {
            for (h = 0; h < HEIGHT_COUNT; ++h)
            {
                char    name[64];
                ar_ctx *ui = settle(v, TARGETS[t], HEIGHTS[h], &s);
                ar_i32  i;

                page_name(v, TARGETS[t], HEIGHTS[h], name);
                if (!ui)
                {
                    printf("# %s: did not lay out\n", name);
                    return 1;
                }

                printf("# page %s\n", name);
                for (i = 0; i < ar_node_count(ui); ++i)
                {
                    char    path[128];
                    ar_rect r = ar_node_rect(ui, i);

                    dump_path(ui, i, path);
                    printf("%s %ld %ld %ld %ld |\n", path, (long)r.x, (long)r.y, (long)r.w,
                           (long)r.h);
                }
            }
        }
    }
    return 0;
}

/*
 * The window. One case at a time, stepped with the arrow keys, drawn into a
 * sub-view of the window's own pixels so the case cannot be confused with the
 * frame around it.
 */
static int run_window(void)
{
    ar_win *win;
    ar_i32  page = 0;

    win = ar_win_open("areole - the snap corpus", WIN_W, WIN_H);
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
        char            title[128];
        char            name[64];
        ar_i32          v, t, h, i;

        if (in->keys_pressed & AR_KEY_RIGHT)
        {
            page = (page + 1) % PAGE_COUNT;
        }
        if (in->keys_pressed & AR_KEY_LEFT)
        {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
        }

        h = page % HEIGHT_COUNT;
        t = (page / HEIGHT_COUNT) % TARGET_COUNT;
        v = page / (HEIGHT_COUNT * TARGET_COUNT);

        for (i = 0; i < w->w * w->h; ++i)
        {
            w->pixels[i] = 0xFF201C18u;
        }

        /* A sub-view, not a copy: the case's pixels point into the middle of
           the window's, with the window's stride. */
        view.pixels = w->pixels + 40 * w->stride + 40;
        view.w = CASE_W;
        view.h = CASE_H;
        view.stride = w->stride;

        ui = settle(v, TARGETS[t], HEIGHTS[h], &view);
        page_name(v, TARGETS[t], HEIGHTS[h], name);
        if (ui)
        {
            sprintf(title, "areole - snap %ld/%ld  %s  settled at %ld", (long)(page + 1),
                    (long)PAGE_COUNT, name, (long)ar_node_scroll(ui, LIST_NODE));
        }
        else
        {
            sprintf(title, "areole - snap %s: did not lay out", name);
        }
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
