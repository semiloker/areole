/*
 * areole example 01 - hello
 * SPDX-License-Identifier: MIT
 *
 * The whole library in one screen: a stylesheet parsed once at startup, a box
 * tree declared fresh every frame, and no coordinates anywhere in this file.
 *
 * Worth comparing against what it replaced. The first version of this example
 * positioned every rectangle by hand and had a nav_rect() and a card_rect()
 * doing arithmetic on constants. All of that is now a few lines of CSS.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>

#define WIN_W 1024
#define WIN_H 640

/* Enough for a few hundred boxes. The block is static, so this process makes
   exactly one allocation and the operating system did it before main ran. */
static unsigned char g_memory[AR_MEM(512)];

/* Split into three, because C90 only guarantees 509 characters in a string
   literal and adjacent literals count as one. ar_stylesheet appends, so
   splitting a sheet costs nothing but a second call. */
static const char *SHEET_APP =
    "#app     { display:flex; flex-direction:row; background:#fefbf2; }"
    ".rail    { width:220px; display:flex; flex-direction:column; padding:16px;"
    "           gap:2px; background:#faf6ed; }"
    ".brand   { font-size:24px; color:#2b2b2b; }"
    ".tagline { font-size:8px; color:#8a8175; padding-bottom:20px; }";

static const char *SHEET_NAV =
    ".nav     { padding:9px 12px; font-size:16px; color:#8a8175; }"
    ".nav:hover  { background:#f0e9db; color:#2b2b2b; }"
    ".nav:active { background:#e6dcc8; }"
    ".nav-on  { padding:9px 12px; font-size:16px; color:#2b2b2b; background:#e6dcc8; }"
    ".page    { width:grow; display:flex; flex-direction:column;"
    "           padding:28px; gap:4px; }"
    ".h1      { font-size:32px; color:#2b2b2b; }"
    ".sub     { font-size:8px; color:#8a8175; padding-bottom:18px; }";

static const char *SHEET_CARD =
    ".row     { display:flex; flex-direction:row; gap:16px; padding-bottom:16px;"
    "           align-items:flex-start; }"
    ".card    { width:grow; display:flex; flex-direction:column;"
    "           background:#f8f3e9; border:1px solid #e8dfcc; }"
    ".card:hover { background:#f2ebdd; }"
    ".accent  { height:4px; background:#c2703d; }";

static const char *SHEET_TEXT =
    ".body    { display:flex; flex-direction:column; padding:14px; gap:8px; }"
    ".name    { font-size:16px; color:#2b2b2b; }"
    ".price   { font-size:16px; color:#c2703d; }"
    ".in      { font-size:8px; color:#4f7a4a; }"
    ".out     { font-size:8px; color:#8a8175; }";

#define NAV_COUNT  5
#define CARD_COLS  3
#define CARD_COUNT 6

static const char *NAV[NAV_COUNT] = {"Home", "Products", "Customers", "Orders", "Settings"};

struct flower
{
    const char *name;
    const char *price;
    int         in_stock;
};

static const struct flower CARDS[CARD_COUNT] = {{"Tulip", "$4.20", 1}, {"Rose", "$6.00", 1},
                                                {"Peony", "$9.50", 0}, {"Lily", "$5.75", 1},
                                                {"Iris", "$3.90", 1},  {"Dahlia", "$7.25", 0}};

static void card(ar_ctx *ui, const struct flower *f)
{
    ar_begin(ui, "div.card");
    ar_begin(ui, "div.accent");
    ar_end(ui);
    ar_begin(ui, "div.body");
    ar_text(ui, "div.name", f->name);
    ar_text(ui, "div.price", f->price);
    ar_text(ui, f->in_stock ? "div.in" : "div.out", f->in_stock ? "In stock" : "Out of stock");
    ar_end(ui);
    ar_end(ui);
}

int main(void)
{
    ar_ctx     *ui;
    ar_win     *win;
    ar_surface *s;
    ar_i32      selected = 1;
    ar_i32      i, row;
    char        status[160];

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        printf("not enough memory for a context\n");
        return 1;
    }

    ar_stylesheet(ui, SHEET_APP);
    ar_stylesheet(ui, SHEET_NAV);
    ar_stylesheet(ui, SHEET_CARD);
    ar_stylesheet(ui, SHEET_TEXT);
    if (ar_stylesheet_errors(ui))
    {
        printf("stylesheet has %lu problem(s)\n", (unsigned long)ar_stylesheet_errors(ui));
        return 1;
    }

    win = ar_win_open("areole - hello", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }

    /* The core has no clock of its own. Lending it one is what makes the phase
       breakdown in the overlay real rather than decorative. */
    ar_set_clock(ui, ar_time_us);

    printf("areole %s, clock: %s\n", ar_version(), ar_time_source());

    while (ar_win_pump(win))
    {
        s = ar_win_surface(win);

        ar_frame_begin(ui, ar_win_input(win));

        ar_begin(ui, "div#app");

        ar_begin(ui, "div.rail");
        ar_text(ui, "div.brand", "areole");
        ar_text(ui, "div.tagline", "no graphics API");
        for (i = 0; i < NAV_COUNT; ++i)
        {
            if (ar_button(ui, i == selected ? "div.nav-on" : "div.nav", NAV[i]))
            {
                selected = i;
            }
        }
        ar_end(ui);

        ar_begin(ui, "div.page");
        ar_text(ui, "div.h1", NAV[selected]);
        ar_text(ui, "div.sub", "Laid out from a stylesheet. Not one coordinate in the C file.");

        /* ponytail: two explicit rows rather than flex-wrap, which does not
           exist yet. It is the next real gap in the layout engine. */
        for (row = 0; row < CARD_COUNT / CARD_COLS; ++row)
        {
            ar_begin(ui, "div.row");
            for (i = 0; i < CARD_COLS; ++i)
            {
                card(ui, &CARDS[row * CARD_COLS + i]);
            }
            ar_end(ui);
        }
        ar_end(ui);

        ar_end(ui);

        ar_frame_end(ui, s);

        /* Drawn over the tree rather than inside it, because it reports on the
           frame that has just been laid out. */
        ar_perf_overlay(ar_perf_of(ui), s, ar_rect_make(0, 0, s->w, s->h), s->w - 296, 16, 1);

        sprintf(status, "areole %s   %ld boxes   %ldx%ld   %s", ar_version(),
                (long)ar_perf_of(ui)->cur.nodes, (long)s->w, (long)s->h, ar_time_source());
        ar_draw_text(s, ar_rect_make(0, 0, s->w, s->h), 12, s->h - 18, status, 1, AR_HEX(0x8A8175));

        ar_win_present(win, ar_rect_make(0, 0, s->w, s->h));
        ar_frame_presented(ui);

        /* Hover is resolved from the previous frame and this pump blocks when
           nothing is happening, so the frame that discovers a new box under
           the cursor has to ask for one more in order to show it. */
        if (ar_needs_redraw(ui))
        {
            ar_win_wake(win);
        }
    }

    ar_win_close(win);
    return 0;
}
