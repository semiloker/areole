/*
 * areole example 01 - hello
 * SPDX-License-Identifier: MIT
 *
 * The smallest thing that proves the stack works end to end: a window whose
 * back buffer is GDI memory, a software rasterizer writing into it, hover and
 * click arriving from the message queue, and one blit to the screen.
 *
 * There is no layout engine yet, so every rectangle here is positioned by
 * hand. Removing that arithmetic is the entire point of the next milestone.
 *
 * The status line at the bottom is drawn by the library itself, and splits
 * raster from present because that is the split that matters: on the hardware
 * areole targets, the blit is routinely the slower half.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>

#define WIN_W 1024
#define WIN_H 640

#define RAIL_W    220
#define NAV_H     38
#define NAV_PAD   12
#define CARD_W    212
#define CARD_H    132
#define CARD_GAP  16
#define CARD_COLS 3
#define STATUS_H  26

#define COL_BG        AR_HEX(0xFEFBF2)
#define COL_RAIL      AR_HEX(0xFAF6ED)
#define COL_NAV_HOVER AR_HEX(0xF0E9DB)
#define COL_NAV_SEL   AR_HEX(0xE6DCC8)
#define COL_CARD      AR_HEX(0xF8F3E9)
#define COL_BORDER    AR_HEX(0xE8DFCC)
#define COL_ACCENT    AR_HEX(0xC2703D)
#define COL_INK       AR_HEX(0x2B2B2B)
#define COL_MUTED     AR_HEX(0x8A8175)
#define COL_OK        AR_HEX(0x4F7A4A)

#define NAV_COUNT  5
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

static void draw_border(ar_surface *s, ar_rect clip, ar_rect r, ar_color c)
{
    ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, 1), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x, r.y + r.h - 1, r.w, 1), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x, r.y, 1, r.h), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x + r.w - 1, r.y, 1, r.h), clip, c);
}

/* Vertically centres a line of text inside a rectangle. */
static void draw_label(ar_surface *s, ar_rect clip, ar_rect box, ar_i32 pad, const char *text,
                       ar_i32 scale, ar_color c)
{
    ar_draw_text(s, clip, box.x + pad, box.y + (box.h - ar_text_height(scale)) / 2, text, scale, c);
}

static ar_rect nav_rect(ar_i32 i)
{
    return ar_rect_make(NAV_PAD, 78 + i * (NAV_H + 4), RAIL_W - NAV_PAD * 2, NAV_H);
}

static ar_rect card_rect(ar_i32 i)
{
    ar_i32 col = i % CARD_COLS;
    ar_i32 row = i / CARD_COLS;
    return ar_rect_make(RAIL_W + 28 + col * (CARD_W + CARD_GAP), 116 + row * (CARD_H + CARD_GAP),
                        CARD_W, CARD_H);
}

int main(void)
{
    ar_win         *win;
    ar_surface     *s;
    const ar_input *in;
    ar_rect         clip;
    ar_i32          selected = 1;
    ar_i32          i;
    char            status[192];
    ar_perf         perf;

    win = ar_win_open("areole - hello", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }

    printf("areole %s, clock: %s\n", ar_version(), ar_time_source());

    ar_perf_reset(&perf);

    while (ar_win_pump(win))
    {
        s = ar_win_surface(win);
        in = ar_win_input(win);
        clip = ar_rect_make(0, 0, s->w, s->h);

        /* Hit testing before drawing, so a click lands on what the user saw
           rather than on what is about to be drawn. */
        for (i = 0; i < NAV_COUNT; ++i)
        {
            if ((in->mouse_pressed & AR_MOUSE_LEFT) &&
                ar_rect_contains(nav_rect(i), in->mouse_x, in->mouse_y))
            {
                selected = i;
            }
        }

        ar_perf_begin(&perf, ar_time_us());

        ar_surface_clear(s, COL_BG);

        /* rail */
        ar_fill_rect(s, ar_rect_make(0, 0, RAIL_W, s->h), clip, COL_RAIL);
        ar_fill_rect(s, ar_rect_make(RAIL_W - 1, 0, 1, s->h), clip, COL_BORDER);
        ar_draw_text(s, clip, NAV_PAD + 4, 28, "areole", 3, COL_INK);
        ar_draw_text(s, clip, NAV_PAD + 4, 56, "no graphics API", 1, COL_MUTED);

        for (i = 0; i < NAV_COUNT; ++i)
        {
            ar_rect r = nav_rect(i);
            int     hot = in->mouse_inside && ar_rect_contains(r, in->mouse_x, in->mouse_y);
            int     held = hot && (in->mouse_down & AR_MOUSE_LEFT);

            if (i == selected)
            {
                ar_fill_rect(s, r, clip, COL_NAV_SEL);
                ar_fill_rect(s, ar_rect_make(r.x, r.y, 3, r.h), clip, COL_ACCENT);
            }
            else if (held)
            {
                ar_fill_rect(s, r, clip, COL_NAV_SEL);
            }
            else if (hot)
            {
                ar_fill_rect(s, r, clip, COL_NAV_HOVER);
            }

            draw_label(s, clip, r, 14, NAV[i], 2, i == selected ? COL_INK : COL_MUTED);
        }

        /* page header */
        ar_draw_text(s, clip, RAIL_W + 28, 34, NAV[selected], 4, COL_INK);
        ar_draw_text(s, clip, RAIL_W + 28, 76,
                     "Six items. Every pixel below was written by the CPU.", 1, COL_MUTED);

        /* cards */
        for (i = 0; i < CARD_COUNT; ++i)
        {
            ar_rect r = card_rect(i);
            int     hot = in->mouse_inside && ar_rect_contains(r, in->mouse_x, in->mouse_y);

            ar_fill_rect(s, r, clip, COL_CARD);
            draw_border(s, clip, r, COL_BORDER);
            ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, 4), clip, COL_ACCENT);

            ar_draw_text(s, clip, r.x + 14, r.y + 24, CARDS[i].name, 2, COL_INK);
            ar_draw_text(s, clip, r.x + 14, r.y + 50, CARDS[i].price, 2, COL_ACCENT);
            ar_draw_text(s, clip, r.x + 14, r.y + CARD_H - 26,
                         CARDS[i].in_stock ? "In stock" : "Out of stock", 1,
                         CARDS[i].in_stock ? COL_OK : COL_MUTED);

            /* A translucent wash on hover, which exercises the blend path
               rather than the opaque one. If the two disagreed, this is where
               it would show, as a card that jumps in brightness. */
            if (hot)
            {
                ar_fill_rect(s, r, clip, AR_RGBA(0xC2, 0x70, 0x3D, 0x18));
            }
        }

        /* The overlay reports the previous frames, because timing the frame
           that draws the timings is not a measurement of anything. Style and
           layout read zero until the next milestone gives them work to do,
           which is the honest way to show what is not built yet. */
        ar_perf_overlay(&perf, s, clip, s->w - 300, 16, 1);

        {
            ar_rect bar = ar_rect_make(0, s->h - STATUS_H, s->w, STATUS_H);
            ar_fill_rect(s, bar, clip, COL_RAIL);
            ar_fill_rect(s, ar_rect_make(0, bar.y, s->w, 1), clip, COL_BORDER);

            sprintf(status, "areole %s   %ldx%ld   clock %s   idle CPU is zero: the pump blocks",
                    ar_version(), (long)s->w, (long)s->h, ar_time_source());
            draw_label(s, clip, bar, 12, status, 1, COL_MUTED);
        }

        ar_perf_mark(&perf, AR_PHASE_RASTER, ar_time_us());

        ar_win_present(win, clip);
        ar_perf_mark(&perf, AR_PHASE_PRESENT, ar_time_us());
        ar_perf_end(&perf, ar_time_us());
    }

    ar_win_close(win);
    return 0;
}
