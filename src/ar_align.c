/*
 * areole - shared box alignment
 * SPDX-License-Identifier: MIT
 *
 * The CSS Box Alignment module is common to flex, grid and, in part, block.
 * Written once here so `center` means the same thing everywhere and a fix in
 * one place fixes all three -- which is not a tidiness argument. Three copies
 * of "distribute the free space" is three chances for `space-around` to put a
 * different amount at the edges than `space-evenly` does, and nobody finds
 * that by reading; they find it by two containers looking subtly unlike each
 * other and nobody being able to say why.
 *
 * ------------------------------------------------------------------------
 * The axis helpers
 *
 * Axis 0 is x, axis 1 is y. The main axis of a flex box is whichever its
 * `flex-direction` names; the cross axis is the other one. Writing a solver
 * once against an axis index rather than twice against x and y is what keeps
 * it a few hundred lines instead of a thousand, and stops the two copies from
 * quietly disagreeing.
 *
 * These used to be static in ar_layout.c. They moved here when flexbox got a
 * file of its own and needed every one of them.
 */
#include "ar_node.h"

ar_i32 ar_axis_main(const ar_node *n)
{
    return n->style.v[AR_P_DIRECTION] == AR_DIR_COLUMN ? 1 : 0;
}

ar_i32 ar_axis_pad_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_TOP] : s->v[AR_P_PAD_LEFT];
}

ar_i32 ar_axis_pad_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_BOTTOM] : s->v[AR_P_PAD_RIGHT];
}

ar_i32 ar_axis_margin_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_TOP] : s->v[AR_P_MARGIN_LEFT];
}

ar_i32 ar_axis_margin_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_BOTTOM] : s->v[AR_P_MARGIN_RIGHT];
}

ar_i32 ar_axis_size_prop(ar_i32 axis)
{
    return axis ? AR_P_HEIGHT : AR_P_WIDTH;
}

ar_i32 ar_axis_min_prop(ar_i32 axis)
{
    return axis ? AR_P_MIN_HEIGHT : AR_P_MIN_WIDTH;
}

/*
 * Unlike the size and min pair above, these two are *wide* properties: they
 * default to a sentinel meaning "no maximum", which does not fit in v[]. So a
 * caller holding this result must read it with ar_style_get, never with v[].
 *
 * That is not a style preference. v[] would be indexed here by a value rather
 * than a constant, so -Warray-bounds cannot catch the mistake, and the result
 * would be whichever bytes follow the array.
 */
ar_i32 ar_axis_max_prop(ar_i32 axis)
{
    return axis ? AR_P_MAX_HEIGHT : AR_P_MAX_WIDTH;
}

ar_i32 *ar_axis_pos(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->y : &r->x;
}

ar_i32 *ar_axis_size(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->h : &r->w;
}

ar_i32 ar_clamp(ar_i32 v, ar_i32 lo, ar_i32 hi)
{
    if (v < lo)
    {
        v = lo;
    }
    if (v > hi)
    {
        v = hi;
    }
    return v < 0 ? 0 : v;
}

/*
 * Distributing free space along an axis.
 *
 * Answers two questions at once because they are one question: where does the
 * first box start, and how much goes between each pair. Every mode is one or
 * the other or both, and separating them means every caller re-deriving the
 * relationship.
 *
 * The three distributed modes differ only in how much lands at the two ends,
 * and the difference is the thing people get wrong:
 *
 *   between   nothing at the ends, all of it between
 *   around    half a share at each end, so an edge gap is half an inner one
 *   evenly    a full share everywhere, ends included
 *
 * `around` and `evenly` look alike in a mock-up with four items and differ by
 * exactly one half-share at each edge, which is what somebody eventually files
 * a bug about.
 *
 * With one box or none, every distributed mode degenerates to centring, which
 * is what CSS says: there are no gaps to put anything in, so the space that
 * would have gone between them goes around them instead.
 */
void ar_align_distribute(ar_i32 mode, ar_i32 free, ar_i32 count, ar_i32 *out_lead,
                         ar_i32 *out_between)
{
    ar_i32 lead = 0;
    ar_i32 between = 0;

    if (free < 0)
    {
        free = 0;
    }

    switch (mode)
    {
    case AR_ALIGN_CENTER:
        lead = free / 2;
        break;

    case AR_ALIGN_END:
        lead = free;
        break;

    case AR_ALIGN_BETWEEN:
        if (count > 1)
        {
            between = free / (count - 1);
        }
        else
        {
            lead = free / 2;
        }
        break;

    case AR_ALIGN_AROUND:
        if (count > 0)
        {
            between = free / count;
            lead = between / 2;
        }
        break;

    case AR_ALIGN_EVENLY:
        if (count > 0)
        {
            between = free / (count + 1);
            lead = between;
        }
        break;

    case AR_ALIGN_START:
    case AR_ALIGN_STRETCH:
    default:
        break;
    }

    *out_lead = lead;
    *out_between = between;
}

/*
 * `justify-content` and `align-content` are the same six modes under two
 * names, and their enums are not the same numbers -- justify grew its values
 * first and align-content grew them later, beside the four `align-items`
 * already had. One place to convert, rather than every caller remembering.
 */
ar_i32 ar_align_from_justify(ar_i32 justify)
{
    switch (justify)
    {
    case AR_JUSTIFY_CENTER:
        return AR_ALIGN_CENTER;
    case AR_JUSTIFY_END:
        return AR_ALIGN_END;
    case AR_JUSTIFY_BETWEEN:
        return AR_ALIGN_BETWEEN;
    case AR_JUSTIFY_AROUND:
        return AR_ALIGN_AROUND;
    case AR_JUSTIFY_EVENLY:
        return AR_ALIGN_EVENLY;
    case AR_JUSTIFY_START:
    default:
        return AR_ALIGN_START;
    }
}

/*
 * Where one box sits in the space a line gives it.
 *
 * `stretch` is not here: it changes the box's *size* rather than its position,
 * and the caller has to have done that before it asks where to put it.
 * `baseline` is not here either, for the same reason in reverse -- it needs
 * every box on the line, not just this one.
 */
ar_i32 ar_align_self_offset(ar_i32 mode, ar_i32 free)
{
    if (free < 0)
    {
        free = 0;
    }
    switch (mode)
    {
    case AR_ALIGN_CENTER:
        return free / 2;
    case AR_ALIGN_END:
        return free;
    default:
        return 0;
    }
}
