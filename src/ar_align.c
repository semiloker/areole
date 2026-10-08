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
 * The axis helpers that used to live here are in ar_node.h now, as inline
 * functions. They are one line each and are called from every layout file,
 * and a call across a translation unit cannot be inlined -- which cost 2.2x
 * of the layout phase for the whole of 0.8.0. The comment beside them says
 * so; do not move them back.
 */
#include "ar_node.h"

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

    mode &= AR_ALIGN_MODE_MASK;

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
    /*
     * `safe` turns into `start` the moment the box would overflow.
     *
     * Centring a box larger than its container puts half the overflow *before*
     * the start edge, where it cannot be scrolled to and cannot be read. That
     * is what `safe` exists to prevent, and it is why the check is on the free
     * space being negative rather than on anything about the box: negative
     * free space is exactly the condition under which centring loses content.
     */
    int safe = (mode & AR_ALIGN_SAFE) != 0;

    mode &= AR_ALIGN_MODE_MASK;
    if (free < 0)
    {
        if (safe)
        {
            return 0;
        }
        /* Overflow goes both ways, which is what makes centring lose content
           and what `safe` was added to CSS to opt out of. */
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
