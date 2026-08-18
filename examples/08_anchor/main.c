/*
 * areole example 08 - the anchor corpus
 * SPDX-License-Identifier: MIT
 *
 * Where an anchored box lands, checked against a browser rather than against
 * my reading of the specification.
 *
 *     example_anchor --dump
 *     python tools/compare_layout.py --run ./build/example_anchor.exe \
 *            examples/08_anchor/anchor.html
 *
 * This one is a real comparison rather than a substitution: Edge implements
 * `anchor-name`, `position-anchor` and `anchor()`, so the twin says the same
 * thing areole does instead of saying the answer.
 *
 * ------------------------------------------------------------------------
 * Fourteen placements against four anchors
 *
 * The anchor moves between cases on purpose. A corpus with the anchor in one
 * place cannot tell a box that resolved `anchor(bottom)` from one that
 * happened to be given the right constant, and that is the whole failure this
 * is guarding against.
 *
 * ------------------------------------------------------------------------
 * Why `position-try` is not in here
 *
 * The flip fires when the box would leave the viewport, and the two engines do
 * not mean the same rectangle by that. areole's viewport is this 200x200
 * surface; the browser's is the window the page is in, which is far larger, so
 * the same case overflows on one side and not on the other. Comparing them
 * would report a difference of framing as a difference of flipping.
 *
 * It is checked in ar_test.c instead, against arithmetic that can be read off
 * the anchor's rectangle, which is the same treatment `proximity` gets in the
 * snap corpus and for the same reason.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 200
#define CASE_H 200
#define WIN_W  520
#define WIN_H  320

static unsigned char g_memory[AR_MEM(128)];
static ar_u32        g_pixels[CASE_W * CASE_H];

/*
 * The popover has no size of its own unless a case gives it one, so a case
 * that says nothing about size is testing placement alone.
 *
 * In two halves: C89 guarantees only 509 characters in a literal and adjacent
 * literals are joined before that limit applies.
 */
static const char *SHEET_A = "#root { display:block; background:#ffffff; }"
                             ".anc  { display:block; position:absolute;"
                             "        background:#cfd8e3; anchor-name: --a; }";

static const char *SHEET_B = ".pop  { display:block; position:absolute; position-anchor: --a;"
                             "        width:30px; height:16px; background:#b8c6d8; }";

typedef struct
{
    const char *name;
    const char *css;
} anchor_variant;

/*
 * Fourteen placements. The four corners, the two centres, both stretches, and
 * the three anchor-size() forms -- including one crossed on purpose, a width
 * taking the anchor's height, because both falling back to one measurement
 * would otherwise pass every other case here.
 */
static const anchor_variant VARIANTS[] = {
    {"below-left", ".pop { top: anchor(bottom); left: anchor(left); }"},
    {"below-right", ".pop { top: anchor(bottom); right: anchor(right); }"},
    {"above-left", ".pop { bottom: anchor(top); left: anchor(left); }"},
    {"above-right", ".pop { bottom: anchor(top); right: anchor(right); }"},
    {"right-top", ".pop { left: anchor(right); top: anchor(top); }"},
    {"right-bottom", ".pop { left: anchor(right); bottom: anchor(bottom); }"},
    {"left-top", ".pop { right: anchor(left); top: anchor(top); }"},
    {"left-bottom", ".pop { right: anchor(left); bottom: anchor(bottom); }"},
    {"center-below", ".pop { top: anchor(bottom); left: anchor(center); }"},
    {"center-right", ".pop { left: anchor(right); top: anchor(center); }"},
    {"cover", ".pop { top: anchor(top); left: anchor(left);"
              "       width: anchor-size(width); height: anchor-size(height); }"},
    {"size-width", ".pop { top: anchor(bottom); left: anchor(left);"
                   "       width: anchor-size(width); }"},
    {"size-crossed", ".pop { top: anchor(bottom); left: anchor(left);"
                     "       width: anchor-size(height); }"},
    {"stretch-h", ".pop { top: anchor(bottom); left: anchor(left);"
                  "       right: anchor(right); }"}};

#define VARIANT_COUNT ((ar_i32)(sizeof VARIANTS / sizeof VARIANTS[0]))

/* Four anchors, deliberately at different sizes as well as different places:
   a case that read the anchor's width where it wanted its height would land
   correctly on a square one. */
typedef struct
{
    ar_i32 x, y, w, h;
} anchor_at;

static const anchor_at PLACES[] = {
    {60, 70, 60, 24}, {12, 12, 40, 40}, {130, 20, 50, 30}, {96, 150, 70, 18}};

#define PLACE_COUNT ((ar_i32)(sizeof PLACES / sizeof PLACES[0]))
#define PAGE_COUNT  (VARIANT_COUNT * PLACE_COUNT)

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

static ar_ctx *settle(ar_i32 variant, ar_i32 place, ar_surface *s)
{
    char      sheet[1100];
    char      anc_css[160];
    ar_ctx   *ui;
    ar_input  in;
    anchor_at p = PLACES[place];

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    sprintf(anc_css, ".anc { top:%ldpx; left:%ldpx; width:%ldpx; height:%ldpx; }", (long)p.y,
            (long)p.x, (long)p.w, (long)p.h);

    strcpy(sheet, SHEET_A);
    strcat(sheet, SHEET_B);
    strcat(sheet, anc_css);
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
    ar_begin(ui, "div.anc");
    ar_end(ui);
    ar_begin(ui, "div.pop");
    ar_end(ui);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    return ar_unbalanced(ui) ? 0 : ui;
}

static void page_name(ar_i32 variant, ar_i32 place, char *out)
{
    sprintf(out, "%s-p%ld", VARIANTS[variant].name, (long)place);
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     v, p;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  anchor corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (v = 0; v < VARIANT_COUNT; ++v)
    {
        for (p = 0; p < PLACE_COUNT; ++p)
        {
            char    name[64];
            ar_ctx *ui = settle(v, p, &s);
            ar_i32  i;

            page_name(v, p, name);
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

    win = ar_win_open("areole - the anchor corpus", WIN_W, WIN_H);
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
        ar_i32          v, p, i;

        if (in->keys_pressed & AR_KEY_RIGHT)
        {
            page = (page + 1) % PAGE_COUNT;
        }
        if (in->keys_pressed & AR_KEY_LEFT)
        {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
        }

        p = page % PLACE_COUNT;
        v = page / PLACE_COUNT;

        for (i = 0; i < w->w * w->h; ++i)
        {
            w->pixels[i] = 0xFF201C18u;
        }

        view.pixels = w->pixels + 40 * w->stride + 40;
        view.w = CASE_W;
        view.h = CASE_H;
        view.stride = w->stride;

        ui = settle(v, p, &view);
        page_name(v, p, name);
        sprintf(title, "areole - anchor %ld/%ld  %s%s", (long)(page + 1), (long)PAGE_COUNT, name,
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
