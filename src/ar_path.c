/*
 * areole - paths and the coverage rasterizer.
 * SPDX-License-Identifier: MIT
 *
 * Antialiasing here is analytic, not sampled. Every segment contributes the
 * exact signed area it covers in each cell it crosses, those contributions are
 * accumulated as deltas, and a prefix sum along the row turns them into
 * coverage. Sixteen-times supersampling costs sixteen times as much and still
 * bands on a near horizontal edge; this is exact and single pass.
 *
 * The whole file is integer. C89 has no type wider than 32 bits, so "integer"
 * here also means every product has to be shown to fit, and the two places
 * where that is not obvious carry the arithmetic in a comment. The technique
 * is to keep each multiplication local: a segment is clipped to one scanline
 * before anything is multiplied, which bounds one factor at 64, and then to one
 * pixel column, which bounds the other.
 */
#include "ar_path.h"

#include <string.h>

/* A quarter of a pixel, in 26.6. Flattening finer than this is invisible and
   costs segments the rasterizer then has to walk. */
#define AR_FLAT_TOL 16

/* Curves in real fonts flatten in two to four levels. Eight is a ceiling that
   a degenerate control polygon cannot walk past, not a working depth. */
#define AR_FLATTEN_MAX 8

static ar_i32 ar__abs(ar_i32 v)
{
    return v < 0 ? -v : v;
}

/*
 * Floored division by a positive divisor. C89 truncates towards zero, which
 * makes the rounding depend on which side of the origin a coordinate happens
 * to fall, and a shape's edges carry opposite signs. The symptom was clean:
 * an apex-up triangle came out 0.52% small and the same triangle apex-down
 * 0.52% large.
 *
 * Rounding to nearest fixed the sign dependence and left 0.29%, because
 * round-half-away-from-zero is symmetric about the origin and the origin is
 * not a special place on the number line. Flooring is symmetric about the
 * pixel grid, which is the symmetry the rasterizer actually has, and the error
 * falls to 0.003%. A hundredfold, from choosing the right symmetry rather than
 * a finer one.
 */
static ar_i32 ar__div_floor(ar_i32 num, ar_i32 den)
{
    if (num >= 0)
    {
        return num / den;
    }
    return -((-num + den - 1) / den);
}

void ar_path_init(ar_path *p, ar_i32 *storage, ar_i32 capacity_points)
{
    p->pt = storage;
    p->cap = capacity_points;
    p->count = 0;
    p->contours = 0;
    p->contour[0] = 0;
    p->overflow = 0;
}

static void ar__push(ar_path *p, ar_i32 x, ar_i32 y)
{
    if (p->count >= p->cap)
    {
        p->overflow = 1;
        return;
    }
    p->pt[p->count * 2] = x;
    p->pt[p->count * 2 + 1] = y;
    ++p->count;
}

void ar_path_move_to(ar_path *p, ar_i32 x, ar_i32 y)
{
    if (p->contours >= AR_PATH_MAX_CONTOURS)
    {
        p->overflow = 1;
        return;
    }
    /* A contour of fewer than two points encloses nothing, so an empty one is
       reused rather than recorded. */
    if (p->count > p->contour[p->contours])
    {
        ++p->contours;
    }
    p->contour[p->contours] = p->count;
    ar__push(p, x, y);
}

void ar_path_line_to(ar_path *p, ar_i32 x, ar_i32 y)
{
    ar__push(p, x, y);
}

void ar_path_close(ar_path *p)
{
    if (p->contours < AR_PATH_MAX_CONTOURS && p->count > p->contour[p->contours])
    {
        ++p->contours;
        p->contour[p->contours] = p->count;
    }
}

/* De Casteljau subdivision rather than evaluation at t.
 *
 * Evaluating a bezier directly needs products of a coordinate with t squared,
 * and with coordinates in 26.6 that overflows 32 bits at ordinary glyph sizes.
 * Subdivision only ever averages two coordinates, so nothing can leave the
 * range the inputs were already in. The halving is written as a division
 * rather than a shift because right-shifting a negative value is
 * implementation defined in C89, and coordinates are routinely negative --
 * every descender is.
 */
static void ar__quad(ar_path *p, ar_i32 x0, ar_i32 y0, ar_i32 cx, ar_i32 cy, ar_i32 x1, ar_i32 y1,
                     ar_i32 depth)
{
    ar_i32 dx = x0 - 2 * cx + x1;
    ar_i32 dy = y0 - 2 * cy + y1;

    if (depth >= AR_FLATTEN_MAX || ar__abs(dx) + ar__abs(dy) <= AR_FLAT_TOL)
    {
        ar_path_line_to(p, x1, y1);
        return;
    }

    {
        ar_i32 ax = (x0 + cx) / 2, ay = (y0 + cy) / 2;
        ar_i32 bx = (cx + x1) / 2, by = (cy + y1) / 2;
        ar_i32 mx = (ax + bx) / 2, my = (ay + by) / 2;

        ar__quad(p, x0, y0, ax, ay, mx, my, depth + 1);
        ar__quad(p, mx, my, bx, by, x1, y1, depth + 1);
    }
}

static void ar__cubic(ar_path *p, ar_i32 x0, ar_i32 y0, ar_i32 ax, ar_i32 ay, ar_i32 bx, ar_i32 by,
                      ar_i32 x1, ar_i32 y1, ar_i32 depth)
{
    ar_i32 d1x = x0 - 2 * ax + bx, d1y = y0 - 2 * ay + by;
    ar_i32 d2x = ax - 2 * bx + x1, d2y = ay - 2 * by + y1;
    ar_i32 dev = ar__abs(d1x) + ar__abs(d1y);
    ar_i32 dev2 = ar__abs(d2x) + ar__abs(d2y);

    if (dev2 > dev)
    {
        dev = dev2;
    }
    if (depth >= AR_FLATTEN_MAX || dev <= AR_FLAT_TOL)
    {
        ar_path_line_to(p, x1, y1);
        return;
    }

    {
        ar_i32 p01x = (x0 + ax) / 2, p01y = (y0 + ay) / 2;
        ar_i32 p12x = (ax + bx) / 2, p12y = (ay + by) / 2;
        ar_i32 p23x = (bx + x1) / 2, p23y = (by + y1) / 2;
        ar_i32 lx = (p01x + p12x) / 2, ly = (p01y + p12y) / 2;
        ar_i32 rx = (p12x + p23x) / 2, ry = (p12y + p23y) / 2;
        ar_i32 mx = (lx + rx) / 2, my = (ly + ry) / 2;

        ar__cubic(p, x0, y0, p01x, p01y, lx, ly, mx, my, depth + 1);
        ar__cubic(p, mx, my, rx, ry, p23x, p23y, x1, y1, depth + 1);
    }
}

void ar_path_quad_to(ar_path *p, ar_i32 cx, ar_i32 cy, ar_i32 x, ar_i32 y)
{
    if (p->count == 0)
    {
        ar_path_move_to(p, x, y);
        return;
    }
    ar__quad(p, p->pt[(p->count - 1) * 2], p->pt[(p->count - 1) * 2 + 1], cx, cy, x, y, 0);
}

void ar_path_cubic_to(ar_path *p, ar_i32 c1x, ar_i32 c1y, ar_i32 c2x, ar_i32 c2y, ar_i32 x,
                      ar_i32 y)
{
    if (p->count == 0)
    {
        ar_path_move_to(p, x, y);
        return;
    }
    ar__cubic(p, p->pt[(p->count - 1) * 2], p->pt[(p->count - 1) * 2 + 1], c1x, c1y, c2x, c2y, x, y,
              0);
}

ar_rect ar_path_bounds(const ar_path *p)
{
    ar_i32 minx, miny, maxx, maxy, i;

    if (p->count == 0)
    {
        return ar_rect_make(0, 0, 0, 0);
    }

    minx = maxx = p->pt[0];
    miny = maxy = p->pt[1];
    for (i = 1; i < p->count; ++i)
    {
        ar_i32 x = p->pt[i * 2], y = p->pt[i * 2 + 1];
        if (x < minx)
        {
            minx = x;
        }
        if (x > maxx)
        {
            maxx = x;
        }
        if (y < miny)
        {
            miny = y;
        }
        if (y > maxy)
        {
            maxy = y;
        }
    }

    /* Outward to whole pixels: a shape covering any part of a pixel needs that
       pixel in the bitmap. Flooring a negative value needs the arithmetic
       written out, because C89 division truncates towards zero. */
    {
        ar_i32 x0 = minx >= 0 ? minx / AR_ONE_PIXEL : -((-minx + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL);
        ar_i32 y0 = miny >= 0 ? miny / AR_ONE_PIXEL : -((-miny + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL);
        ar_i32 x1 = maxx >= 0 ? (maxx + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL : -((-maxx) / AR_ONE_PIXEL);
        ar_i32 y1 = maxy >= 0 ? (maxy + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL : -((-maxy) / AR_ONE_PIXEL);
        return ar_rect_make(x0, y0, x1 - x0, y1 - y0);
    }
}

/* ------------------------------------------------------------------------
 * The rasterizer
 * ------------------------------------------------------------------------ */

/*
 * One segment, already clipped to a single scanline, distributed across the
 * pixel columns it crosses.
 *
 * `d` is the signed vertical extent this segment covers within the row, in
 * coverage units, so |d| <= AR_COV_ONE. Each column receives the share of d
 * proportional to how much of the horizontal span lies in it, split between
 * that column and the next according to where the centroid falls -- which is
 * what makes the result the exact covered area rather than a sample of it.
 *
 * Overflow: d <= 2^12 and every weight is <= 64 = 2^6, so no product here
 * exceeds 2^18.
 */
static void ar__row_span(ar_i32 *row, ar_i32 w, ar_i32 xa, ar_i32 xb, ar_i32 d)
{
    ar_i32 x0 = xa < xb ? xa : xb;
    ar_i32 x1 = xa < xb ? xb : xa;
    ar_i32 total, col0, col1, xi;
    ar_i32 walked = 0, given = 0;

    /* Everything left of the bitmap still has to contribute its winding, or
       the prefix sum starts from the wrong number and the whole row is wrong.
       Clamping into column zero is exactly right for that: the area is outside
       the visible range, the winding is not. */
    col0 = x0 / AR_ONE_PIXEL;
    col1 = x1 / AR_ONE_PIXEL;
    if (x0 < 0)
    {
        col0 = 0;
        x0 = 0;
    }
    if (x1 < 0)
    {
        col1 = 0;
        x1 = 0;
    }
    if (col0 > w)
    {
        col0 = w;
    }
    if (col1 > w)
    {
        col1 = w;
    }
    total = x1 - x0;

    if (total == 0 || col0 == col1)
    {
        /* One column. The centroid is the midpoint of the span, and the second
           cell takes the remainder rather than its own rounded share, so the
           pair sums to exactly d. */
        ar_i32 mid = (x0 + x1) / 2 - col0 * AR_ONE_PIXEL;
        ar_i32 first = ar__div_floor(d * (AR_ONE_PIXEL - mid), AR_ONE_PIXEL);
        row[col0] += first;
        row[col0 + 1] += d - first;
        return;
    }

    for (xi = col0; xi <= col1; ++xi)
    {
        ar_i32 left = xi * AR_ONE_PIXEL;
        ar_i32 right = left + AR_ONE_PIXEL;
        ar_i32 sa = x0 > left ? x0 : left;
        ar_i32 sb = x1 < right ? x1 : right;
        ar_i32 width = sb - sa;
        ar_i32 reached, share, mid, first;

        if (width <= 0)
        {
            continue;
        }

        /* Each column's share is the difference of two exact prefixes rather
           than its own rounded quotient. Rounding a quotient per column loses
           up to one unit each time and the losses are all in the same
           direction, which showed up as a systematic one per cent of missing
           area; differences of prefixes sum to exactly d instead.

           Overflow: d <= 2^12 and walked <= total, so this stays inside 32
           bits for any span narrower than 2^19 in 26.6, which is 8192 pixels
           -- wider than any bitmap this rasterizes into. */
        walked += width;
        reached = ar__div_floor(d * walked, total);
        share = reached - given;
        given = reached;

        mid = (sa + sb) / 2 - left;
        first = ar__div_floor(share * (AR_ONE_PIXEL - mid), AR_ONE_PIXEL);
        row[xi] += first;
        row[xi + 1] += share - first;
    }
}

/*
 * One segment, walked scanline by scanline.
 *
 * Overflow: the x step is computed from the remaining distance to the endpoint
 * rather than from a precomputed slope, because a slope for a near horizontal
 * segment is enormous and multiplying it by anything overflows. Here
 * |p1x - cx| <= 2^21 for any sane coordinate and dy <= 64 = 2^6, so the
 * product stays under 2^27. Recomputing from the endpoint each row also stops
 * error accumulating along a long edge.
 */
static void ar__line(ar_i32 *acc, ar_i32 w, ar_i32 h, ar_i32 accw, ar_i32 x0, ar_i32 y0, ar_i32 x1,
                     ar_i32 y1)
{
    ar_i32 dir = 1;
    ar_i32 cx, cy, y, ylast;

    if (y0 == y1)
    {
        return; /* a horizontal edge encloses no area */
    }
    if (y0 > y1)
    {
        ar_i32 t;
        dir = -1;
        t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
    }

    cx = x0;
    cy = y0;

    y = y0 >= 0 ? y0 / AR_ONE_PIXEL : -((-y0 + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL);
    ylast = (y1 - 1) >= 0 ? (y1 - 1) / AR_ONE_PIXEL
                          : -((-(y1 - 1) + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL);

    for (; y <= ylast; ++y)
    {
        ar_i32 ynext = (y + 1) * AR_ONE_PIXEL;
        ar_i32 yb = ynext < y1 ? ynext : y1;
        ar_i32 dy = yb - cy;
        ar_i32 xb;

        if (dy <= 0)
        {
            continue;
        }
        if (yb == y1)
        {
            xb = x1;
        }
        else
        {
            /* Rounded, for the reason given on ar__div_round: truncating here
               moves a right-going edge left and a left-going edge right, so
               every shape came out slightly small and every hole slightly
               large. This is the one that mattered. */
            xb = cx + ar__div_floor((x1 - cx) * dy, y1 - cy);
        }

        if (y >= 0 && y < h)
        {
            /* dy is at most one pixel, so d is at most AR_COV_ONE. */
            ar_i32 d = dir * (dy * AR_COV_ONE / AR_ONE_PIXEL);
            ar__row_span(acc + y * accw, w, cx, xb, d);
        }

        cx = xb;
        cy = yb;
    }
}

void ar_path_rasterize(const ar_path *p, ar_u8 *cov, ar_i32 w, ar_i32 h, ar_i32 stride, ar_i32 ox,
                       ar_i32 oy, ar_i32 rule, ar_i32 *acc)
{
    ar_i32 accw = w + 2;
    ar_i32 c, i, x, y;

    if (w <= 0 || h <= 0)
    {
        return;
    }

    memset(acc, 0, (size_t)(accw * h) * sizeof(ar_i32));

    /* contour[c] is where contour c starts; it ends where the next one begins,
       or at the end of the points for the last. A contour of fewer than two
       points, which is what a trailing ar_path_close leaves behind, encloses
       nothing. */
    for (c = 0; c <= p->contours; ++c)
    {
        ar_i32 start = p->contour[c];
        ar_i32 end = (c < p->contours) ? p->contour[c + 1] : p->count;

        if (end - start < 2)
        {
            continue;
        }

        for (i = start; i < end - 1; ++i)
        {
            ar__line(acc, w, h, accw, p->pt[i * 2] - ox, p->pt[i * 2 + 1] - oy,
                     p->pt[(i + 1) * 2] - ox, p->pt[(i + 1) * 2 + 1] - oy);
        }
        /* Contours are closed implicitly: an unclosed one would leak winding
           into every pixel to its right. */
        ar__line(acc, w, h, accw, p->pt[(end - 1) * 2] - ox, p->pt[(end - 1) * 2 + 1] - oy,
                 p->pt[start * 2] - ox, p->pt[start * 2 + 1] - oy);
    }

    for (y = 0; y < h; ++y)
    {
        const ar_i32 *row = acc + y * accw;
        ar_u8        *out = cov + y * stride;
        ar_i32        sum = 0;

        for (x = 0; x < w; ++x)
        {
            ar_i32 v;

            sum += row[x];
            v = sum < 0 ? -sum : sum;

            if (rule == AR_FILL_EVENODD)
            {
                v = v % (2 * AR_COV_ONE);
                if (v > AR_COV_ONE)
                {
                    v = 2 * AR_COV_ONE - v;
                }
            }
            else if (v > AR_COV_ONE)
            {
                v = AR_COV_ONE;
            }

            /* Rounded, not truncated. Truncating loses up to half a level in
               every partly covered cell and always downwards, which showed as
               a full pixel of coverage summing to 252 rather than 255. */
            out[x] = (ar_u8)((v * 255 + AR_COV_ONE / 2) / AR_COV_ONE);
        }
    }
}
