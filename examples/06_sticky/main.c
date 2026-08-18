/*
 * areole example 06 - the sticky corpus
 * SPDX-License-Identifier: MIT
 *
 * Where a `position: sticky` box ends up once its scrollport has moved under
 * it, checked against a browser rather than against my reading of the
 * specification.
 *
 *     example_sticky --dump
 *     python tools/compare_layout.py --run ./build/example_sticky.exe \
 *            examples/06_sticky/sticky.html
 *
 * Run with --dump it prints the rectangles and exits, which is what the
 * comparison drives and what CI runs. Run with no arguments it opens a window
 * and draws one case at a time, stepped with the left and right arrow keys.
 *
 * ------------------------------------------------------------------------
 * The three shapes
 *
 * simple       port > [before, stick, after]
 * constrained  port > [before, group > [stick, filler], after]
 * nested       outer > [pad, inner > [before, stick, after], pad]
 *
 * `constrained` is the rule that separates sticky from fixed: a sticky box may
 * not leave its containing block, so once the group has scrolled far enough
 * the header goes with it instead of staying pinned. `nested` is there because
 * a sticky box pins to its *nearest* scrolling ancestor, and getting that
 * wrong only shows when there are two to choose between -- so both are
 * scrolled, separately.
 *
 * ------------------------------------------------------------------------
 * The control
 *
 * `static-control` is the same tree with `position: static`, and it exists
 * because of a lesson this project has already paid for once: a corpus that
 * passes while the feature does nothing is worth nothing. If sticky stopped
 * working entirely, every other case here would still agree with a browser
 * that was also not sticking anything -- so one case deliberately pins the
 * un-stuck geometry, and the two must differ.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 200
#define CASE_H 200
#define WIN_W  520
#define WIN_H  320

#define SHAPE_SIMPLE      0
#define SHAPE_CONSTRAINED 1
#define SHAPE_NESTED      2

#define AXIS_Y 0
#define AXIS_X 1

static unsigned char g_memory[AR_MEM(128)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * The shared sheet. Heights are stated here so a variant only ever says
 * something about stickiness, and the scrollports are `scroll` rather than
 * `auto` so a bar's presence never depends on the case.
 *
 * In two halves, joined at run time rather than by the compiler. C89
 * guarantees only 509 characters in a string literal, and adjacent literals
 * are concatenated before that limit is applied, so writing this as one
 * initialiser is rejected by -Woverlength-strings under -pedantic-errors. That
 * gate caught this exact thing in the tour already.
 */
static const char *SHEET_BASE_A = "#root   { display:block; background:#ffffff; }"
                                  ".port   { display:block; overflow:scroll; width:200px;"
                                  "          height:120px; background:#f4f2ee; }"
                                  ".before { display:block; height:60px; background:#e6ded0; }"
                                  ".stick  { display:block; height:30px; background:#b8c6d8; }"
                                  ".after  { display:block; height:220px; background:#dfe6ee; }";

static const char *SHEET_BASE_B = ".group  { display:block; height:120px; background:#eee6f0; }"
                                  ".filler { display:block; height:90px; background:#f0e6ee; }"
                                  ".outer  { display:block; overflow:scroll; width:200px;"
                                  "          height:150px; background:#eef2f4; }"
                                  ".inner  { display:block; overflow:scroll; height:110px;"
                                  "          background:#f8f4ec; }"
                                  ".pad    { display:block; height:70px; background:#e2e8ea; }";

typedef struct
{
    const char *name;
    int         shape;
    int         axis;
    ar_i32      scroll_node;
    const char *css;
} sticky_variant;

/*
 * Ten variants. All four inset directions, both nested scrollports, the
 * containing-block clamp, and the control.
 *
 * Sticky in a table header is specified for this release and is not here:
 * there are no tables until 0.7.0. Recorded in the coverage document rather
 * than approximated with a div.
 */
static const sticky_variant VARIANTS[] = {
    {"top", SHAPE_SIMPLE, AXIS_Y, 1, ".stick { position:sticky; top:10px; }"},

    {"top-zero", SHAPE_SIMPLE, AXIS_Y, 1, ".stick { position:sticky; top:0px; }"},

    {"bottom", SHAPE_SIMPLE, AXIS_Y, 1, ".stick { position:sticky; bottom:12px; }"},

    {"top-and-bottom", SHAPE_SIMPLE, AXIS_Y, 1, ".stick { position:sticky; top:8px; bottom:8px; }"},

    /* The inline axis. The rows are wider than the port, so it scrolls
       sideways, and the sticky box pins against the same two edges. */
    {"left", SHAPE_SIMPLE, AXIS_X, 1,
     ".port  { overflow-x:scroll; }"
     ".before, .stick, .after { width:420px; }"
     ".stick { position:sticky; left:10px; }"},

    {"right", SHAPE_SIMPLE, AXIS_X, 1,
     ".port  { overflow-x:scroll; }"
     ".before, .stick, .after { width:420px; }"
     ".stick { position:sticky; right:14px; }"},

    /* The clamp: once the group has gone, the header goes with it. */
    {"constrained", SHAPE_CONSTRAINED, AXIS_Y, 1, ".stick { position:sticky; top:0px; }"},

    {"constrained-inset", SHAPE_CONSTRAINED, AXIS_Y, 1, ".stick { position:sticky; top:20px; }"},

    /* Two scrollports. The sticky box belongs to the inner one, so scrolling
       the outer must carry it along unpinned and scrolling the inner must pin
       it. Both are exercised, because an implementation that picked the
       outermost ancestor would pass one and fail the other. */
    {"nested-inner", SHAPE_NESTED, AXIS_Y, 3, ".stick { position:sticky; top:6px; }"},

    {"nested-outer", SHAPE_NESTED, AXIS_Y, 1, ".stick { position:sticky; top:6px; }"},

    {"static-control", SHAPE_SIMPLE, AXIS_Y, 1, ".stick { position:static; }"}};

#define VARIANT_COUNT ((ar_i32)(sizeof VARIANTS / sizeof VARIANTS[0]))

/* Five offsets: before the box would stick, right at it, well past it, past
   the containing block for the constrained shapes, and clamped to the end. */
static const ar_i32 TARGETS[] = {0, 20, 45, 80, 9999};
#define TARGET_COUNT ((ar_i32)(sizeof TARGETS / sizeof TARGETS[0]))

#define PAGE_COUNT (VARIANT_COUNT * TARGET_COUNT)

static void build(ar_ctx *ui, int shape)
{
    if (shape == SHAPE_SIMPLE)
    {
        ar_begin(ui, "div.port");
        ar_begin(ui, "div.before");
        ar_end(ui);
        ar_begin(ui, "div.stick");
        ar_end(ui);
        ar_begin(ui, "div.after");
        ar_end(ui);
        ar_end(ui);
    }
    else if (shape == SHAPE_CONSTRAINED)
    {
        ar_begin(ui, "div.port");
        ar_begin(ui, "div.before");
        ar_end(ui);
        ar_begin(ui, "div.group");
        ar_begin(ui, "div.stick");
        ar_end(ui);
        ar_begin(ui, "div.filler");
        ar_end(ui);
        ar_end(ui);
        ar_begin(ui, "div.after");
        ar_end(ui);
        ar_end(ui);
    }
    else
    {
        ar_begin(ui, "div.outer");
        ar_begin(ui, "div.pad");
        ar_end(ui);
        ar_begin(ui, "div.inner");
        ar_begin(ui, "div.before");
        ar_end(ui);
        ar_begin(ui, "div.stick");
        ar_end(ui);
        ar_begin(ui, "div.after");
        ar_end(ui);
        ar_end(ui);
        ar_begin(ui, "div.pad");
        ar_end(ui);
        ar_end(ui);
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

/*
 * One page: lay the case out, move its scrollport, lay it out again.
 *
 * The second frame is not optional. A scroll position is applied by the next
 * frame's layout pass, and sticky is resolved inside that pass -- a dump taken
 * straight after the move would report the rectangles from before it.
 */
static ar_ctx *settle(ar_i32 variant, ar_i32 target, ar_surface *s)
{
    char     sheet[1024];
    ar_ctx  *ui;
    ar_input in;
    int      shape = VARIANTS[variant].shape;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    strcpy(sheet, SHEET_BASE_A);
    strcat(sheet, SHEET_BASE_B);
    strcat(sheet, VARIANTS[variant].css);
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
    build(ui, shape);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    if (VARIANTS[variant].axis == AXIS_X)
    {
        ar_node_scroll_to_x(ui, VARIANTS[variant].scroll_node, target);
    }
    else
    {
        ar_node_scroll_to(ui, VARIANTS[variant].scroll_node, target);
    }

    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    build(ui, shape);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    return ar_unbalanced(ui) ? 0 : ui;
}

static void page_name(ar_i32 variant, ar_i32 target, char *out)
{
    sprintf(out, "%s-t%ld", VARIANTS[variant].name, (long)target);
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     v, t;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  sticky corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (v = 0; v < VARIANT_COUNT; ++v)
    {
        for (t = 0; t < TARGET_COUNT; ++t)
        {
            char    name[64];
            ar_ctx *ui = settle(v, TARGETS[t], &s);
            ar_i32  i;

            page_name(v, TARGETS[t], name);
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
                printf("%s %ld %ld %ld %ld |\n", path, (long)r.x, (long)r.y, (long)r.w, (long)r.h);
            }
        }
    }
    return 0;
}

static int run_window(void)
{
    ar_win *win;
    ar_i32  page = 0;

    win = ar_win_open("areole - the sticky corpus", WIN_W, WIN_H);
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
        ar_i32          v, t, i;

        if (in->keys_pressed & AR_KEY_RIGHT)
        {
            page = (page + 1) % PAGE_COUNT;
        }
        if (in->keys_pressed & AR_KEY_LEFT)
        {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
        }

        t = page % TARGET_COUNT;
        v = page / TARGET_COUNT;

        for (i = 0; i < w->w * w->h; ++i)
        {
            w->pixels[i] = 0xFF201C18u;
        }

        view.pixels = w->pixels + 40 * w->stride + 40;
        view.w = CASE_W;
        view.h = CASE_H;
        view.stride = w->stride;

        ui = settle(v, TARGETS[t], &view);
        page_name(v, TARGETS[t], name);
        sprintf(title, "areole - sticky %ld/%ld  %s%s", (long)(page + 1), (long)PAGE_COUNT, name,
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
