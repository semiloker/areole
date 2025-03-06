/*
 * areole - glyph blitting and text measurement.
 * SPDX-License-Identifier: MIT
 *
 * The face is 8x8 monochrome, drawn at an integer scale. Coverage is one bit
 * per pixel, so there is nothing to filter and nothing to cache: the inner
 * loop is a bit test and a store.
 */
#include "ar_internal.h"

/* Anything outside the embedded range is drawn as a question mark rather than
   skipped. Silently dropping a character makes a mangled string look like a
   layout bug, which is a far longer afternoon than seeing "?" and knowing. */
static ar_i32 ar__glyph_index(unsigned char ch)
{
    if (ch < AR_FONT_FIRST || ch > AR_FONT_LAST)
    {
        return (ar_i32)('?' - AR_FONT_FIRST);
    }
    return (ar_i32)ch - AR_FONT_FIRST;
}

ar_i32 ar_text_height(ar_i32 scale)
{
    return AR_FONT_H * (scale < 1 ? 1 : scale);
}

ar_i32 ar_text_line_height(ar_i32 scale)
{
    ar_i32 s = scale < 1 ? 1 : scale;
    /* Two rows of leading at scale 1, growing with the face. Eight pixel rows
       stacked with no gap are unreadable as soon as there are two lines. */
    return (AR_FONT_H + 2) * s;
}

ar_i32 ar_text_width(const char *text, ar_i32 scale)
{
    ar_i32               w = 0;
    ar_i32               line = 0;
    const unsigned char *p;

    if (!text)
    {
        return 0;
    }
    if (scale < 1)
    {
        scale = 1;
    }

    /* The width of a multi-line string is the width of its widest line, which
       is what a caller centring or sizing a box actually needs. */
    for (p = (const unsigned char *)text; *p; ++p)
    {
        if (*p == '\n')
        {
            if (line > w)
            {
                w = line;
            }
            line = 0;
            continue;
        }
        if (*p == '\r')
        {
            continue;
        }
        line += (ar_i32)ar__font_advance[ar__glyph_index(*p)] * scale;
    }
    return line > w ? line : w;
}

/* Fills a scale-by-scale block, clipped. At scale 1 this is one bounds test
   and one store, which is the case that matters. */
static void ar__ink(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, ar_i32 scale, ar_color c,
                    ar_u32 alpha)
{
    ar_rect d;
    ar_u32 *row;
    ar_i32  ix, iy;

    d = ar_rect_intersect(ar_rect_make(x, y, scale, scale), clip);
    d = ar_rect_intersect(d, ar_rect_make(0, 0, s->w, s->h));
    if (ar_rect_is_empty(d))
    {
        return;
    }

    row = s->pixels + (ar_i32)d.y * s->stride + d.x;
    if (alpha == 0xFFu)
    {
        for (iy = 0; iy < d.h; ++iy)
        {
            for (ix = 0; ix < d.w; ++ix)
            {
                row[ix] = c;
            }
            row += s->stride;
        }
        return;
    }
    for (iy = 0; iy < d.h; ++iy)
    {
        for (ix = 0; ix < d.w; ++ix)
        {
            row[ix] = ar__blend(row[ix], c, alpha);
        }
        row += s->stride;
    }
}

void ar_draw_text(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *text, ar_i32 scale,
                  ar_color c)
{
    const unsigned char *p;
    ar_rect              bounds;
    ar_i32               pen_x, pen_y;
    ar_u32               alpha;

    if (!text || !s->pixels)
    {
        return;
    }
    if (scale < 1)
    {
        scale = 1;
    }

    alpha = AR_ALPHA_OF(c);
    if (alpha == 0)
    {
        return;
    }

    bounds = ar_rect_intersect(clip, ar_rect_make(0, 0, s->w, s->h));
    if (ar_rect_is_empty(bounds))
    {
        return;
    }

    pen_x = x;
    pen_y = y;

    for (p = (const unsigned char *)text; *p; ++p)
    {
        ar_i32 g, left, advance, row, col;

        if (*p == '\n')
        {
            pen_x = x;
            pen_y += ar_text_line_height(scale);
            continue;
        }
        if (*p == '\r')
        {
            continue;
        }

        g = ar__glyph_index(*p);
        left = (ar_i32)ar__font_left[g];
        advance = (ar_i32)ar__font_advance[g] * scale;

        /* Reject the whole glyph before touching a bit of it. A string
           scrolled mostly out of view costs one rectangle test per character
           rather than sixty-four. */
        if (pen_x + advance > bounds.x && pen_x < bounds.x + bounds.w &&
            pen_y + AR_FONT_H * scale > bounds.y && pen_y < bounds.y + bounds.h)
        {
            for (row = 0; row < AR_FONT_H; ++row)
            {
                ar_u32 bits = ar__font_rows[g][row];
                if (bits == 0)
                {
                    continue;
                }
                for (col = 0; col < AR_FONT_W; ++col)
                {
                    /* Bit 0 is the leftmost pixel in this face. */
                    if ((bits >> col) & 1u)
                    {
                        ar__ink(s, bounds, pen_x + (col - left) * scale, pen_y + row * scale, scale,
                                c, alpha);
                    }
                }
            }
        }

        pen_x += advance;
    }
}
