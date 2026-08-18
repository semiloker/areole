/*
 * areole example 07 - the safe-area corpus
 * SPDX-License-Identifier: MIT
 *
 * What a stylesheet laid out with env() comes to, checked against a browser
 * given the same numbers written out.
 *
 *     example_env --dump
 *     python tools/compare_layout.py --run ./build/example_env.exe \
 *            examples/07_env/env.html
 *
 * ------------------------------------------------------------------------
 * What "simulated safe area" has to mean here
 *
 * 0.6.2's fourth criterion asks that a stub backend reporting non-zero insets
 * produce the same layout as a browser with a simulated safe area. A browser
 * cannot be handed safe-area insets through the comparison harness -- there is
 * no CSS that sets env(), and no device to emulate one from -- so the
 * simulation is the other half: areole is given the insets by its backend and
 * writes env(), and the twin writes the same numbers as literal lengths.
 *
 * That is a real test of the thing that could be wrong. env() resolution, the
 * known-versus-silent distinction and the fallback all sit between the backend
 * and the layout, and this is what says the layout on the far side of them
 * matches a browser's.
 *
 * ------------------------------------------------------------------------
 * Why every case here is `viewport-fit: cover`
 *
 * The other half of viewport-fit shrinks the layout viewport, which moves
 * areole's root box to the top-left corner of the safe rectangle rather than
 * to the surface's. The harness reads browser rectangles relative to the case
 * element, so expressing that on the other side would need a wrapper the
 * areole tree does not have, and the two would stop matching on their paths
 * for a reason that has nothing to do with safe areas.
 *
 * So the viewport half is pinned in ar_test.c instead, by
 * test_viewport_fit_moves_the_insets_and_the_viewport_together, which measures
 * the root box and the reported inset in the same breath -- which is what
 * criterion 5 asks for anyway.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 200
#define CASE_H 200
#define WIN_W  520
#define WIN_H  320

/* What the stub backend reports. Deliberately four different numbers, so an
   implementation that read the wrong slot lands somewhere visible. */
#define INSET_TOP    34
#define INSET_RIGHT  8
#define INSET_BOTTOM 12
#define INSET_LEFT   6

#define TITLEBAR_W 96
#define TITLEBAR_H 32

#define REPORT_NONE 0 /* the backend says nothing: fallbacks stand */
#define REPORT_REAL 1 /* the four insets above */
#define REPORT_ZERO 2 /* a windowed desktop: real insets, all of them zero */

static unsigned char g_memory[AR_MEM(128)];
static ar_u32        g_pixels[CASE_W * CASE_H];

static const char *SHEET_BASE = "#root { display:block; background:#ffffff; }"
                                ".bar  { display:block; background:#cfd8e3; }";

typedef struct
{
    const char *name;
    int         report;

    /* Whether this case's stylesheet is expected to be rejected in part. Only
       `unknown-bare` is: `env(nonsense-inset)` names something nothing can
       supply and gives no fallback, so there is no value and the declaration
       goes -- which areole counts as a stylesheet error, correctly. Without
       this the guard below would throw the case out as a broken corpus entry
       rather than run the one thing it exists to check. */
    int expect_errors;

    const char *css;
} env_variant;

static const env_variant VARIANTS[] = {
    {"cover-top", REPORT_REAL, 0, ".bar { padding-top: env(safe-area-inset-top); }"},

    {"cover-four", REPORT_REAL, 0,
     ".bar { padding-top: env(safe-area-inset-top);"
     "       padding-right: env(safe-area-inset-right);"
     "       padding-bottom: env(safe-area-inset-bottom);"
     "       padding-left: env(safe-area-inset-left); }"},

    /* The backend never mentioned them, so the fallback stands. */
    {"fallback", REPORT_NONE, 0, ".bar { padding-top: env(safe-area-inset-top, 20px); }"},

    /* The backend reported zero, which is an answer rather than a silence: a
       windowed desktop really does have insets of zero, and taking the
       fallback here would put a gap at the top of every window. */
    {"reported-zero", REPORT_ZERO, 0, ".bar { padding-top: env(safe-area-inset-top, 20px); }"},

    /* The titlebar rectangle, which is not gated on viewport-fit. */
    {"titlebar", REPORT_REAL, 0, ".bar { padding-left: env(titlebar-area-width, 5px); }"},

    /* An env() nothing can supply, with and without a way out. */
    {"unknown-with-fallback", REPORT_REAL, 0, ".bar { padding-top: env(nonsense-inset, 14px); }"},

    {"unknown-bare", REPORT_REAL, 1, ".bar { padding-top: env(nonsense-inset); }"}};

#define VARIANT_COUNT ((ar_i32)(sizeof VARIANTS / sizeof VARIANTS[0]))

/* Two bar heights, so a padding that leaked into the wrong axis shows up. */
static const ar_i32 HEIGHTS[] = {20, 60};
#define HEIGHT_COUNT ((ar_i32)(sizeof HEIGHTS / sizeof HEIGHTS[0]))

#define PAGE_COUNT (VARIANT_COUNT * HEIGHT_COUNT)

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

static ar_ctx *settle(ar_i32 variant, ar_i32 height, ar_surface *s)
{
    char     sheet[640];
    char     height_css[64];
    ar_ctx  *ui;
    ar_input in;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        return 0;
    }

    sprintf(height_css, ".bar { height: %ldpx; }", (long)height);
    strcpy(sheet, SHEET_BASE);
    strcat(sheet, VARIANTS[variant].css);
    strcat(sheet, height_css);
    ar_stylesheet(ui, sheet);
    if (ar_stylesheet_errors(ui) && !VARIANTS[variant].expect_errors)
    {
        return 0;
    }

    /*
     * The stub backend. Calling ar_set_safe_area at all is what marks the four
     * slots known, so REPORT_ZERO calls it with zeroes rather than skipping
     * it -- that is the whole difference between "no insets" and "no idea".
     */
    if (VARIANTS[variant].report == REPORT_REAL)
    {
        ar_set_safe_area(ui, INSET_TOP, INSET_RIGHT, INSET_BOTTOM, INSET_LEFT);
        ar_set_titlebar_area(ui, 0, 0, TITLEBAR_W, TITLEBAR_H);
    }
    else if (VARIANTS[variant].report == REPORT_ZERO)
    {
        ar_set_safe_area(ui, 0, 0, 0, 0);
    }
    ar_set_viewport_fit_cover(ui, 1);

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#root");
    ar_begin(ui, "div.bar");
    ar_end(ui);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    return ar_unbalanced(ui) ? 0 : ui;
}

static void page_name(ar_i32 variant, ar_i32 height, char *out)
{
    sprintf(out, "%s-h%ld", VARIANTS[variant].name, (long)height);
}

static int run_dump(void)
{
    ar_surface s;
    ar_i32     v, h;

    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = CASE_H;
    s.stride = CASE_W;

    printf("# areole %s  viewport %ld %ld  safe-area corpus\n", ar_version(), (long)s.w, (long)s.h);

    for (v = 0; v < VARIANT_COUNT; ++v)
    {
        for (h = 0; h < HEIGHT_COUNT; ++h)
        {
            char    name[64];
            ar_ctx *ui = settle(v, HEIGHTS[h], &s);
            ar_i32  i;

            page_name(v, HEIGHTS[h], name);
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

    win = ar_win_open("areole - the safe-area corpus", WIN_W, WIN_H);
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
        ar_i32          v, h, i;

        if (in->keys_pressed & AR_KEY_RIGHT)
        {
            page = (page + 1) % PAGE_COUNT;
        }
        if (in->keys_pressed & AR_KEY_LEFT)
        {
            page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
        }

        h = page % HEIGHT_COUNT;
        v = page / HEIGHT_COUNT;

        for (i = 0; i < w->w * w->h; ++i)
        {
            w->pixels[i] = 0xFF201C18u;
        }

        view.pixels = w->pixels + 40 * w->stride + 40;
        view.w = CASE_W;
        view.h = CASE_H;
        view.stride = w->stride;

        ui = settle(v, HEIGHTS[h], &view);
        page_name(v, HEIGHTS[h], name);
        sprintf(title, "areole - safe area %ld/%ld  %s%s", (long)(page + 1), (long)PAGE_COUNT, name,
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
