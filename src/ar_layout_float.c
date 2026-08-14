/*
 * areole - floats
 * SPDX-License-Identifier: MIT
 *
 * A floated box is taken out of the normal flow and pushed as far left or
 * right as it will go, and the line boxes beside it are shortened to make room.
 * Everything else in the flow behaves as though it were not there.
 *
 * Floats are unfashionable and unavoidable. Every document written before flex
 * existed uses them, and a wrapped image or a pull quote is still expressed
 * this way.
 *
 * ------------------------------------------------------------------------
 * The one thing to keep straight
 *
 * A float shortens *line boxes*, not block boxes. A paragraph beside a float
 * still spans the full width of its container -- its border box starts at the
 * container's left edge and ends at its right -- and only the lines inside it
 * are narrowed. This is why text wraps around a float while the background of
 * the paragraph holding it runs underneath.
 *
 * Getting that backwards is the classic float bug, and it looks almost right,
 * which is what makes it worth stating here rather than leaving to be noticed.
 *
 * ------------------------------------------------------------------------
 * The list
 *
 * Active floats live in a fixed array on the stack of whoever is laying out
 * the formatting context, so there is nothing to allocate and nothing to free.
 * Sixteen is more than a document puts beside one another; past that a float
 * is placed as though the list were full, which is to say at the container's
 * edge, and that is a visible failure rather than a silent one.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

void ar_float_reset(ar_float_ctx *fc, ar_i32 left, ar_i32 right)
{
    fc->count = 0;
    fc->left = left;
    fc->right = right;
}

/*
 * The horizontal band still free between y and y + h.
 *
 * A float intrudes on a band if any part of it overlaps that vertical range,
 * which is why this is a range and not a point: a line box is as tall as its
 * contents and a float clipping only its bottom still narrows it.
 */
void ar_float_band(const ar_float_ctx *fc, ar_i32 y, ar_i32 h, ar_i32 *out_left, ar_i32 *out_right)
{
    ar_i32 lo = fc->left;
    ar_i32 hi = fc->right;
    ar_i32 i;

    if (h < 1)
    {
        h = 1; /* a zero-height probe still has to see what it would touch */
    }
    for (i = 0; i < fc->count; ++i)
    {
        const ar_float_box *f = &fc->f[i];

        if (f->y1 <= y || f->y0 >= y + h)
        {
            continue;
        }
        if (f->side == AR_FLOAT_LEFT)
        {
            if (f->x1 > lo)
            {
                lo = f->x1;
            }
        }
        else if (f->x0 < hi)
        {
            hi = f->x0;
        }
    }
    if (hi < lo)
    {
        hi = lo;
    }
    *out_left = lo;
    *out_right = hi;
}

/*
 * The lowest edge of the floats on the given sides, at or below y.
 *
 * What `clear` needs, and what a formatting context needs in order to grow tall
 * enough to hold its own floats.
 */
ar_i32 ar_float_clear_y(const ar_float_ctx *fc, ar_i32 y, ar_i32 which)
{
    ar_i32 out = y;
    ar_i32 i;

    for (i = 0; i < fc->count; ++i)
    {
        const ar_float_box *f = &fc->f[i];
        int want = (f->side == AR_FLOAT_LEFT) ? (which & AR_CLEAR_LEFT) : (which & AR_CLEAR_RIGHT);

        if (want && f->y1 > out)
        {
            out = f->y1;
        }
    }
    return out;
}

ar_i32 ar_float_bottom(const ar_float_ctx *fc)
{
    ar_i32 out = 0;
    ar_i32 i;

    for (i = 0; i < fc->count; ++i)
    {
        if (fc->f[i].y1 > out)
        {
            out = fc->f[i].y1;
        }
    }
    return out;
}

/*
 * Puts a float at or below y and records it.
 *
 * It goes as high as it can and as far to its side as it can. If it does not
 * fit beside what is already there, it drops to the first y where it does --
 * which is the bottom of one of the floats in the way, so trying each of those
 * in turn finds the answer without searching pixel by pixel.
 */
void ar_float_place(ar_float_ctx *fc, ar_node *n, ar_i32 y, ar_i32 side)
{
    ar_i32 outer_w = n->rect.w + n->style.v[AR_P_MARGIN_LEFT] + n->style.v[AR_P_MARGIN_RIGHT];
    ar_i32 outer_h = n->rect.h + n->style.v[AR_P_MARGIN_TOP] + n->style.v[AR_P_MARGIN_BOTTOM];
    ar_i32 at = y;
    ar_i32 guard;
    ar_i32 lo, hi;

    for (guard = 0; guard <= fc->count; ++guard)
    {
        ar_i32 i;
        ar_i32 next = 0;

        ar_float_band(fc, at, outer_h, &lo, &hi);
        if (hi - lo >= outer_w)
        {
            break;
        }

        /* Not enough room here. The next y worth trying is the top of the
           lowest float bottom still below us; anything nearer changes nothing. */
        for (i = 0; i < fc->count; ++i)
        {
            if (fc->f[i].y1 > at && (next == 0 || fc->f[i].y1 < next))
            {
                next = fc->f[i].y1;
            }
        }
        if (next == 0)
        {
            break; /* nothing left to drop below; it overflows, visibly */
        }
        at = next;
    }

    ar_float_band(fc, at, outer_h, &lo, &hi);
    if (side == AR_FLOAT_LEFT)
    {
        n->rect.x = lo + n->style.v[AR_P_MARGIN_LEFT];
    }
    else
    {
        n->rect.x = hi - outer_w + n->style.v[AR_P_MARGIN_LEFT];
    }
    n->rect.y = at + n->style.v[AR_P_MARGIN_TOP];

    if (fc->count < AR_MAX_FLOATS)
    {
        ar_float_box *f = &fc->f[fc->count++];

        /* The margin box, because that is what other content has to avoid. */
        f->x0 = n->rect.x - n->style.v[AR_P_MARGIN_LEFT];
        f->x1 = n->rect.x + n->rect.w + n->style.v[AR_P_MARGIN_RIGHT];
        f->y0 = at;
        f->y1 = at + outer_h;
        f->side = (ar_u8)side;
    }
}
