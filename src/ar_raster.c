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

/* ------------------------------------------------------------------------
 * Rounded corners
 *
 * A rounded rectangle is a stack of horizontal spans, so it is built out of
 * ar_fill_rect rather than beside it: clipping, the opaque fast path and
 * blending are already right there and a second copy of them is a second
 * place to get them wrong.
 *
 * Hard edged, and that is a stated limitation rather than an oversight. The
 * coverage rasterizer in ar_path.c could antialias these, but it wants a
 * coverage buffer and a blit pass, and the shapes this is for are a 13 pixel
 * radio button and a 7 pixel bullet -- at that size the buffer costs more
 * than the jaggies do. Written down so the upgrade path is obvious when
 * something wants a 40 pixel rounded card.
 *
 * The span walk carries `dx` down as `dy` rises instead of taking a square
 * root per row: x is monotonically decreasing over the quarter circle, so the
 * whole cap costs O(radius) decrements rather than O(radius) sqrts. No
 * floating point, which is the invariant, and no table either.
 * ------------------------------------------------------------------------ */

/* How far in from the edge the fill starts, for each row of one corner.
   `inset[i]` is for the row `i` pixels below the top of the rectangle, and
   the same array read backwards serves the bottom. */
static void ar__corner_insets(ar_i32 radius, ar_i32 *inset)
{
    ar_i32 i;
    ar_i32 dx = radius;

    /*
     * Pixel centres, doubled so the half never needs a fraction: a row `i`
     * from the edge has its centre `2*(radius - i) - 1` from the circle's, and
     * it is inside when that and `2*dx - 1` fit in a circle of `2*radius`.
     * Comparing at centres rather than at corners is what stops the cap
     * looking like a staircase with one long step at the top.
     *
     * Filled from the bottom of the cap upwards, which is the direction that
     * makes the walk work: `dy` grows as `i` falls, so `dx` only ever
     * decreases and a decrement-only loop is enough. Written the other way
     * round first, where `dy` shrank while the loop could only take `dx` down
     * -- so every row kept the narrowest inset the first row had, and a radio
     * button came out as a one pixel vertical bar.
     */
    for (i = radius - 1; i >= 0; --i)
    {
        ar_i32 dy = 2 * (radius - i) - 1;

        while (dx > 0 && (2 * dx - 1) * (2 * dx - 1) + dy * dy > 4 * radius * radius)
        {
            --dx;
        }
        inset[i] = radius - dx;
    }
}

/* The most a corner can be. Beyond half the shorter side the two corners on
   that side would overlap and the shape stops being a rounded rectangle. */
static ar_i32 ar__clamp_radius(ar_rect r, ar_i32 radius)
{
    ar_i32 lim = (r.w < r.h ? r.w : r.h) / 2;

    if (radius > lim)
    {
        radius = lim;
    }
    if (radius > AR_MAX_RADIUS)
    {
        radius = AR_MAX_RADIUS;
    }
    return radius > 0 ? radius : 0;
}

void ar_fill_round_rect(ar_surface *s, ar_rect r, ar_i32 radius, ar_rect clip, ar_color c)
{
    ar_i32 inset[AR_MAX_RADIUS];
    ar_i32 i;

    radius = ar__clamp_radius(r, radius);
    if (radius <= 0)
    {
        ar_fill_rect(s, r, clip, c);
        return;
    }

    ar__corner_insets(radius, inset);

    for (i = 0; i < radius; ++i)
    {
        ar_i32 in = inset[i];

        ar_fill_rect(s, ar_rect_make(r.x + in, r.y + i, r.w - 2 * in, 1), clip, c);
        ar_fill_rect(s, ar_rect_make(r.x + in, r.y + r.h - 1 - i, r.w - 2 * in, 1), clip, c);
    }
    /* The straight middle in one call, which is the whole rectangle for
       anything much taller than its corners. */
    ar_fill_rect(s, ar_rect_make(r.x, r.y + radius, r.w, r.h - 2 * radius), clip, c);
}

void ar_stroke_round_rect(ar_surface *s, ar_rect r, ar_i32 radius, ar_i32 width, ar_rect clip,
                          ar_color c)
{
    ar_i32  outer[AR_MAX_RADIUS];
    ar_i32  inner[AR_MAX_RADIUS];
    ar_i32  i, irad;
    ar_rect in_r;

    if (width <= 0)
    {
        return;
    }
    radius = ar__clamp_radius(r, radius);
    if (radius <= 0)
    {
        ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, width), clip, c);
        ar_fill_rect(s, ar_rect_make(r.x, r.y + r.h - width, r.w, width), clip, c);
        ar_fill_rect(s, ar_rect_make(r.x, r.y + width, width, r.h - 2 * width), clip, c);
        ar_fill_rect(s, ar_rect_make(r.x + r.w - width, r.y + width, width, r.h - 2 * width), clip,
                     c);
        return;
    }

    /* The hole, and its corners are tighter by exactly the thickness -- which
       is what keeps the ring an even width all the way round instead of
       thinning at the diagonals. */
    in_r = ar_rect_make(r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width);
    if (in_r.w <= 0 || in_r.h <= 0)
    {
        ar_fill_round_rect(s, r, radius, clip, c);
        return;
    }
    irad = ar__clamp_radius(in_r, radius - width);

    ar__corner_insets(radius, outer);
    if (irad > 0)
    {
        ar__corner_insets(irad, inner);
    }

    for (i = 0; i < r.h; ++i)
    {
        ar_i32 o, x0, x1;
        ar_i32 iy = i - width; /* the same row, counted from the hole's top */

        /* Outer edge of this row. */
        if (i < radius)
        {
            o = outer[i];
        }
        else if (i >= r.h - radius)
        {
            o = outer[r.h - 1 - i];
        }
        else
        {
            o = 0;
        }
        x0 = r.x + o;
        x1 = r.x + r.w - o;

        /* Inside the top or bottom band there is no hole, so the row is solid
           from edge to edge. */
        if (iy < 0 || iy >= in_r.h)
        {
            ar_fill_rect(s, ar_rect_make(x0, r.y + i, x1 - x0, 1), clip, c);
            continue;
        }

        {
            ar_i32 ii;
            ar_i32 h0, h1;

            if (irad > 0 && iy < irad)
            {
                ii = inner[iy];
            }
            else if (irad > 0 && iy >= in_r.h - irad)
            {
                ii = inner[in_r.h - 1 - iy];
            }
            else
            {
                ii = 0;
            }
            h0 = in_r.x + ii;
            h1 = in_r.x + in_r.w - ii;

            if (h0 > x0)
            {
                ar_fill_rect(s, ar_rect_make(x0, r.y + i, h0 - x0, 1), clip, c);
            }
            if (x1 > h1)
            {
                ar_fill_rect(s, ar_rect_make(h1, r.y + i, x1 - h1, 1), clip, c);
            }
        }
    }
}

/*
 * A filled triangle inscribed in a rectangle, pointing one of four ways.
 *
 * Spans again, and for the same reason the rounded rectangle is: clipping and
 * the opaque fast path are already right in ar_fill_rect. The arithmetic is
 * the straight-line one -- half-width grows linearly from apex to base -- so
 * there is no curve to walk and no table.
 *
 * It exists for two markers a browser draws and this engine could not: a
 * `<summary>`'s disclosure triangle and a `<select>`'s arrow. Both are a
 * glyph elsewhere, and a glyph is not available here -- the built-in face is
 * ASCII 32 to 126, so U+25B8 would draw as a question mark on any build
 * without a TrueType face.
 */
void ar_fill_tri(ar_surface *s, ar_rect r, ar_i32 dir, ar_rect clip, ar_color c)
{
    ar_i32 i;

    if (r.w <= 0 || r.h <= 0)
    {
        return;
    }

    if (dir == AR_TRI_UP || dir == AR_TRI_DOWN)
    {
        /* Rows, widening from the apex. `+ (r.h - 1)` rounds the half-width up
           so the apex is a pixel wide rather than none, which is the
           difference between a triangle and a triangle with its tip missing. */
        for (i = 0; i < r.h; ++i)
        {
            ar_i32 from_apex = (dir == AR_TRI_DOWN) ? (r.h - 1 - i) : i;
            ar_i32 w = (r.w * (from_apex + 1) + r.h - 1) / r.h;
            ar_i32 x = r.x + (r.w - w) / 2;

            ar_fill_rect(s, ar_rect_make(x, r.y + i, w, 1), clip, c);
        }
        return;
    }

    /* Rows again for left and right, because a horizontal span is what the
       fill is fast at -- so the triangle is built out of rows whose length
       tapers, not columns. */
    for (i = 0; i < r.h; ++i)
    {
        ar_i32 half = r.h / 2;
        ar_i32 from_mid = i < half ? half - i : i - half;
        ar_i32 w = half > 0 ? (r.w * (half - from_mid) + half - 1) / half : r.w;
        ar_i32 x;

        if (w <= 0)
        {
            continue;
        }
        x = (dir == AR_TRI_RIGHT) ? r.x : r.x + r.w - w;
        ar_fill_rect(s, ar_rect_make(x, r.y + i, w, 1), clip, c);
    }
}

/* ------------------------------------------------------------------------
 * Moving pixels that are already correct
 * ------------------------------------------------------------------------ */
int ar_surface_move_rows(ar_surface *s, ar_i32 x, ar_i32 w, ar_i32 src_y, ar_i32 dst_y, ar_i32 h)
{
    ar_i32 row;

    if (!s || !s->pixels || w <= 0 || h <= 0)
    {
        return 0;
    }
    if (src_y == dst_y)
    {
        return 1; /* already where it belongs, and nothing to do is success */
    }

    /*
     * Refused rather than clamped, and the caller is obliged to notice.
     *
     * Clamping would move part of the region and still report success, and by
     * then the caller has already decided not to repaint what it believes was
     * moved -- so a partial move is a smear that nothing will ever come back
     * and correct. Refusing sends it to a full repaint, which is merely slow.
     */
    if (x < 0 || x + w > s->w)
    {
        return 0;
    }
    if (src_y < 0 || dst_y < 0 || src_y + h > s->h || dst_y + h > s->h)
    {
        return 0;
    }

    /*
     * Whole rows at different y cannot overlap each other, so the only hazard
     * is the order they are copied in: going towards the destination never
     * reads a row that has already been written. That is why this is a row loop
     * and not one memmove over the block -- the block overlaps itself, the rows
     * do not.
     *
     * A plain copy loop rather than memcpy keeps the rasterizer free of libc,
     * which 0.17.0 needs and which costs nothing here.
     *
     * Deliberately uncounted. These pixels are moved, not filled, and adding
     * them to fill_px would inflate the fill totals and make ns_per_px describe
     * work the rasterizer did not do. The dirty ratio is what shows the move
     * working.
     */
    if (dst_y < src_y)
    {
        for (row = 0; row < h; ++row)
        {
            const ar_u32 *src = s->pixels + (src_y + row) * s->stride + x;
            ar_u32       *dst = s->pixels + (dst_y + row) * s->stride + x;
            ar_i32        k;

            for (k = 0; k < w; ++k)
            {
                dst[k] = src[k];
            }
        }
    }
    else
    {
        for (row = h - 1; row >= 0; --row)
        {
            const ar_u32 *src = s->pixels + (src_y + row) * s->stride + x;
            ar_u32       *dst = s->pixels + (dst_y + row) * s->stride + x;
            ar_i32        k;

            for (k = 0; k < w; ++k)
            {
                dst[k] = src[k];
            }
        }
    }
    return 1;
}
