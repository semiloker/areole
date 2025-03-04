/*
 * areole example 01 - hello
 * SPDX-License-Identifier: MIT
 *
 * The smallest thing that proves the stack works end to end: a window whose
 * back buffer is GDI memory, a software rasterizer writing into it, hover and
 * click arriving from the message queue, and one blit to the screen.
 *
 * There is no layout engine and no font yet, so every rectangle here is
 * positioned by hand. That is the point of the next two milestones.
 *
 * The title bar is the readout. A library that cannot draw a glyph yet still
 * has numbers worth showing, and splitting raster from present is the split
 * that matters: on old hardware the blit is often the slower half.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>

#define WIN_W 1024
#define WIN_H 640

#define RAIL_W    220
#define NAV_H     42
#define NAV_PAD   12
#define CARD_W    212
#define CARD_H    132
#define CARD_GAP  16
#define CARD_COLS 3

#define COL_BG        AR_HEX(0xFEFBF2)
#define COL_RAIL      AR_HEX(0xFAF6ED)
#define COL_NAV_HOVER AR_HEX(0xF0E9DB)
#define COL_NAV_SEL   AR_HEX(0xE6DCC8)
#define COL_CARD      AR_HEX(0xF8F3E9)
#define COL_BORDER    AR_HEX(0xE8DFCC)
#define COL_ACCENT    AR_HEX(0xC2703D)
#define COL_INK       AR_HEX(0x2B2B2B)

#define NAV_COUNT  5
#define CARD_COUNT 6

/* Stands in for text until the font lands: a bar whose width encodes how long
   the label would have been. */
static void stub_text(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h,
                      ar_color c)
{
    ar_fill_rect(s, ar_rect_make(x, y, w, h), clip, c);
}

static void draw_border(ar_surface *s, ar_rect clip, ar_rect r, ar_color c)
{
    ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, 1), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x, r.y + r.h - 1, r.w, 1), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x, r.y, 1, r.h), clip, c);
    ar_fill_rect(s, ar_rect_make(r.x + r.w - 1, r.y, 1, r.h), clip, c);
}

static ar_rect nav_rect(ar_i32 i)
{
    return ar_rect_make(NAV_PAD, 64 + i * (NAV_H + 4), RAIL_W - NAV_PAD * 2, NAV_H);
}

static ar_rect card_rect(ar_i32 i)
{
    ar_i32 col = i % CARD_COLS;
    ar_i32 row = i / CARD_COLS;
    return ar_rect_make(RAIL_W + 28 + col * (CARD_W + CARD_GAP), 104 + row * (CARD_H + CARD_GAP),
                        CARD_W, CARD_H);
}

int main(void)
{
    ar_win         *win;
    ar_surface     *s;
    const ar_input *in;
    ar_rect         clip;
    ar_i32          selected = 0;
    ar_i32          i;
    char            title[256];
    ar_u32          t0, t_raster, t_present;

    win = ar_win_open("areole - hello", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }

    printf("areole %s\n", ar_version());
    printf("clock: %s\n", ar_time_source());

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

        t0 = ar_time_us();

        ar_surface_clear(s, COL_BG);

        /* rail */
        ar_fill_rect(s, ar_rect_make(0, 0, RAIL_W, s->h), clip, COL_RAIL);
        ar_fill_rect(s, ar_rect_make(RAIL_W - 1, 0, 1, s->h), clip, COL_BORDER);
        stub_text(s, clip, NAV_PAD, 26, 110, 14, COL_INK);

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

            stub_text(s, clip, r.x + 14, r.y + NAV_H / 2 - 4, 86 + i * 9, 8, COL_INK);
        }

        /* page header */
        stub_text(s, clip, RAIL_W + 28, 34, 168, 20, COL_INK);
        stub_text(s, clip, RAIL_W + 28, 66, 260, 8, AR_RGBA(0x2B, 0x2B, 0x2B, 0x60));

        /* cards */
        for (i = 0; i < CARD_COUNT; ++i)
        {
            ar_rect r = card_rect(i);
            int     hot = in->mouse_inside && ar_rect_contains(r, in->mouse_x, in->mouse_y);

            ar_fill_rect(s, r, clip, COL_CARD);
            draw_border(s, clip, r, COL_BORDER);
            ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, 4), clip, COL_ACCENT);

            stub_text(s, clip, r.x + 14, r.y + 26, 96, 10, COL_INK);
            stub_text(s, clip, r.x + 14, r.y + 48, 58, 8, AR_RGBA(0x2B, 0x2B, 0x2B, 0x70));

            /* A translucent wash on hover, which is the blend path rather than
               the opaque one. If the two disagreed this is where it would
               show, as a card that jumps in brightness under the cursor. */
            if (hot)
            {
                ar_fill_rect(s, r, clip, AR_RGBA(0xC2, 0x70, 0x3D, 0x18));
            }
        }

        t_raster = ar_time_us() - t0;

        t0 = ar_time_us();
        ar_win_present(win, clip);
        t_present = ar_time_us() - t0;

        sprintf(title, "areole hello  -  raster %u us  -  present %u us  -  %ldx%ld  -  %s",
                (unsigned)t_raster, (unsigned)t_present, (long)s->w, (long)s->h, ar_time_source());
        ar_win_set_title(win, title);
    }

    ar_win_close(win);
    return 0;
}
