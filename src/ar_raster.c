/*
 * areole - the software rasterizer.
 * SPDX-License-Identifier: MIT
 *
 * Every routine here is integer only. A Pentium III has an FPU; it is still
 * slower and less predictable than the integer unit, and predictability is
 * what keeps p99 frame time near p50.
 */
#include "ar_internal.h"

/* ------------------------------------------------------------------------
 * Rectangles
 * ------------------------------------------------------------------------ */

ar_rect ar_rect_make(ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h)
{
    ar_rect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

int ar_rect_is_empty(ar_rect r)
{
    return r.w <= 0 || r.h <= 0;
}

ar_rect ar_rect_intersect(ar_rect a, ar_rect b)
{
    ar_i32 x0, y0, x1, y1;

    x0 = a.x > b.x ? a.x : b.x;
    y0 = a.y > b.y ? a.y : b.y;
    x1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    y1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    /* An empty result is normalised to zero rather than left negative, so
       callers can test with ar_rect_is_empty and never see a rect whose
       width happens to be negative in one place and zero in another. */
    if (x1 <= x0 || y1 <= y0)
    {
        return ar_rect_make(x0, y0, 0, 0);
    }
    return ar_rect_make(x0, y0, x1 - x0, y1 - y0);
}

ar_rect ar_rect_union(ar_rect a, ar_rect b)
{
    ar_i32 x0, y0, x1, y1;

    /* The union of something with nothing is that something. Without this the
       dirty region would be dragged to the origin by the first empty rect it
       ever merged, and the whole window would repaint every frame. */
    if (ar_rect_is_empty(a))
    {
        return b;
    }
    if (ar_rect_is_empty(b))
    {
        return a;
    }

    x0 = a.x < b.x ? a.x : b.x;
    y0 = a.y < b.y ? a.y : b.y;
    x1 = (a.x + a.w) > (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    y1 = (a.y + a.h) > (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);

    return ar_rect_make(x0, y0, x1 - x0, y1 - y0);
}

int ar_rect_contains(ar_rect r, ar_i32 x, ar_i32 y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ------------------------------------------------------------------------
 * Blending
 *
 * Source-over, two multiplies per half-word pair rather than one per channel.
 *
 * The red and blue channels are processed together in bits 0..7 and 16..23,
 * green on its own in 8..15. Multiplying a masked value by an 8 bit factor
 * cannot spill from one channel into the next, because each product needs at
 * most 16 bits and each channel has 16 bits of room.
 *
 * The factor is first stretched from 0..255 to 0..256, so the shift by 8 is a
 * true divide instead of a divide by 255 that darkens everything slightly.
 *
 * What this buys, and what it costs:
 *
 *   exact at the ends   alpha 0 leaves the destination untouched and alpha 255
 *                       reproduces the source bit for bit, so every opaque fill
 *                       in the UI is the colour the stylesheet asked for.
 *   no drift            the two weights sum to exactly 256, so blending a
 *                       colour over itself returns it unchanged at every alpha.
 *                       A hover highlight redrawn for a thousand frames does
 *                       not creep.
 *   +/- 1 in between    intermediate alphas can land one step off the exactly
 *                       rounded value. Correcting it costs two more adds and
 *                       two more shifts per pixel pair, to move a colour by one
 *                       part in 255. Not worth it on the hardware this targets.
 * ------------------------------------------------------------------------ */
ar_u32 ar__blend(ar_u32 dst, ar_u32 src, ar_u32 alpha)
{
    ar_u32 a, ia, rb, g;

    a = alpha + (alpha >> 7); /* 0..255 -> 0..256, and 255 maps exactly to 256 */
    ia = 256u - a;

    rb = (((src & 0x00FF00FFu) * a) + ((dst & 0x00FF00FFu) * ia)) >> 8;
    g = (((src & 0x0000FF00u) * a) + ((dst & 0x0000FF00u) * ia)) >> 8;

    return 0xFF000000u | (rb & 0x00FF00FFu) | (g & 0x0000FF00u);
}

/* ------------------------------------------------------------------------
 * Fills
 * ------------------------------------------------------------------------ */

void ar_surface_clear(ar_surface *s, ar_color c)
{
    ar_fill_rect(s, ar_rect_make(0, 0, s->w, s->h), ar_rect_make(0, 0, s->w, s->h), c);
}

void ar_fill_rect(ar_surface *s, ar_rect r, ar_rect clip, ar_color c)
{
    ar_rect d;
    ar_u32 *row;
    ar_i32  x, y;
    ar_u32  alpha;

    /* Clip against the caller's region and against the surface itself. The
       second is not paranoia: a scroll container can hand down a clip that is
       entirely valid and still sit partly off screen. */
    d = ar_rect_intersect(r, clip);
    d = ar_rect_intersect(d, ar_rect_make(0, 0, s->w, s->h));
    if (ar_rect_is_empty(d))
    {
        AR_COUNT(clipped_out, 1);
        return;
    }

    alpha = AR_ALPHA_OF(c);
    if (alpha == 0)
    {
        AR_COUNT(clipped_out, 1);
        return;
    }

    row = s->pixels + (ar_i32)d.y * s->stride + d.x;
    AR_COUNT(fills, 1);

    if (alpha == 0xFFu)
    {
        AR_COUNT(fill_px, d.w * d.h);

        /* Opaque is the overwhelmingly common case: panels, cards, the window
           background. A straight word store per pixel is what the compiler
           turns into a rep stos, and nothing hand written beats it. */
        for (y = 0; y < d.h; ++y)
        {
            for (x = 0; x < d.w; ++x)
            {
                row[x] = c;
            }
            row += s->stride;
        }
        return;
    }

    AR_COUNT(blend_px, d.w * d.h);
    for (y = 0; y < d.h; ++y)
    {
        for (x = 0; x < d.w; ++x)
        {
            row[x] = ar__blend(row[x], c, alpha);
        }
        row += s->stride;
    }
}
