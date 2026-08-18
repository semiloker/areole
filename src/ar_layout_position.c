/*
 * areole - positioning
 * SPDX-License-Identifier: MIT
 *
 * Taking boxes out of flow. A dropdown, a tooltip, a modal, a badge in the
 * corner of an avatar: every one of them is a box that has to sit somewhere
 * other than where the flow would have put it, and until now areole could
 * express none of them.
 *
 * ------------------------------------------------------------------------
 * The containing block
 *
 * Quietly the hardest part, and the part that makes the difference between
 * "absolute positioning works" and "absolute positioning works the way people
 * expect".
 *
 *   static    in flow; there is no offset to resolve.
 *   relative  in flow, and shifted for painting only. It still occupies the
 *             space it would have, which is why a relatively positioned box
 *             leaves a hole rather than closing it up.
 *   absolute  out of flow, against the *padding box* of the nearest ancestor
 *             that is positioned at all. Not the border box, not the content
 *             box -- the padding box, so `top: 0` sits under the border and
 *             inside it.
 *   fixed     out of flow, against the viewport.
 *
 * `position: relative` on a parent with no offsets of its own is therefore not
 * a no-op: it is how you say "measure my absolute children against me", which
 * is the single most-used line of CSS there is.
 *
 * ------------------------------------------------------------------------
 * Over-constrained
 *
 * `left`, `right` and `width` can all be given at once, and then they cannot
 * all be honoured. CSS says the last one loses, and says which is last, and
 * says what happens when some are `auto` instead -- including that two `auto`
 * margins centre the box, which is how a modal is centred and why that trick
 * has outlived every layout fashion since.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

int ar_is_sticky(const ar_node *n)
{
    return n->style.v[AR_P_POSITION] == AR_POS_STICKY;
}

int ar_is_positioned(const ar_node *n)
{
    return n->style.v[AR_P_POSITION] != AR_POS_STATIC;
}

int ar_is_out_of_flow(const ar_node *n)
{
    ar_i32 p = n->style.v[AR_P_POSITION];

    return p == AR_POS_ABSOLUTE || p == AR_POS_FIXED;
}

/*
 * The box an out-of-flow child is measured against.
 *
 * Walks up to the nearest positioned ancestor and returns its padding box. A
 * `fixed` box skips the walk: its containing block is the viewport, whatever
 * is between it and the root.
 */
ar_rect ar_containing_block(const ar_node *nodes, ar_i32 i, ar_rect viewport)
{
    const ar_node *n = &nodes[i];
    ar_i32         at;

    if (n->style.v[AR_P_POSITION] == AR_POS_FIXED)
    {
        return viewport;
    }

    for (at = n->parent; at >= 0; at = nodes[at].parent)
    {
        const ar_node *a = &nodes[at];

        if (!ar_is_positioned(a))
        {
            continue;
        }
        /* The padding box: inside the border, outside the padding. */
        return ar_rect_make(a->rect.x + a->style.v[AR_P_BORDER_WIDTH],
                            a->rect.y + a->style.v[AR_P_BORDER_WIDTH],
                            a->rect.w - 2 * a->style.v[AR_P_BORDER_WIDTH],
                            a->rect.h - 2 * a->style.v[AR_P_BORDER_WIDTH]);
    }
    return viewport;
}

/* Whether an offset was given at all. `auto` means "wherever the flow or the
   other three edges put it", and is the default. */
static int ar__given(const ar_node *n, ar_prop p)
{
    return n->style.unit[p] == AR_UNIT_PX || n->style.unit[p] == AR_UNIT_PCT;
}

static ar_i32 ar__offset(const ar_node *n, ar_prop p, ar_i32 against)
{
    if (n->style.unit[p] == AR_UNIT_PCT)
    {
        return against * n->style.v[p] / 100;
    }
    return n->style.v[p];
}

/*
 * One axis of an out-of-flow box: where it starts and how big it is.
 *
 * The three-way argument between a leading offset, a trailing offset and a
 * size, which is the same on both axes and so is written once. `auto` margins
 * only centre when both edges are given and a size is too -- that is the
 * condition, and it is why centring a modal needs `left: 0; right: 0` and not
 * just `margin: auto`.
 */
static void ar__resolve_axis(const ar_node *n, ar_i32 cb_pos, ar_i32 cb_size, ar_prop lead_p,
                             ar_prop trail_p, ar_prop size_p, ar_i32 m_lead, ar_i32 m_trail,
                             int auto_margins, ar_i32 fit, ar_i32 *out_pos, ar_i32 *out_size)
{
    int    has_lead = ar__given(n, lead_p);
    int    has_trail = ar__given(n, trail_p);
    int    has_size = n->style.unit[size_p] == AR_UNIT_PX || n->style.unit[size_p] == AR_UNIT_PCT;
    ar_i32 lead = ar__offset(n, lead_p, cb_size);
    ar_i32 trail = ar__offset(n, trail_p, cb_size);
    ar_i32 size;

    if (n->style.unit[size_p] == AR_UNIT_PCT)
    {
        size = ar_used_size(n, size_p == AR_P_HEIGHT ? 1 : 0, cb_size * n->style.v[size_p] / 100);
    }
    else if (has_size)
    {
        size = ar_used_size(n, size_p == AR_P_HEIGHT ? 1 : 0, n->style.v[size_p]);
    }
    else
    {
        size = fit;
    }

    if (has_lead && has_trail && !has_size)
    {
        /* Both edges pinned and no size: the box stretches between them. */
        *out_pos = cb_pos + lead + m_lead;
        *out_size = cb_size - lead - trail - m_lead - m_trail;
        if (*out_size < 0)
        {
            *out_size = 0;
        }
        return;
    }

    *out_size = size;

    if (has_lead && has_trail && has_size && auto_margins)
    {
        /* Over-constrained, with the escape hatch: the leftover is split
           between the two automatic margins, and the box is centred. */
        ar_i32 slack = cb_size - lead - trail - size;

        *out_pos = cb_pos + lead + (slack > 0 ? slack / 2 : 0);
        return;
    }
    if (has_lead)
    {
        *out_pos = cb_pos + lead + m_lead;
        return;
    }
    if (has_trail)
    {
        *out_pos = cb_pos + cb_size - trail - size - m_trail;
        return;
    }

    /*
     * Neither edge given: the static position, which is where the box would
     * have gone in flow. The flow already put it somewhere, so that is left
     * alone -- which is what makes `position: absolute` with no offsets look
     * like nothing happened until something else moves.
     */
    *out_pos = *out_pos + m_lead;
}

/*
 * Places one out-of-flow box against its containing block.
 *
 * The box has already been through the flow passes, so its rect holds the
 * static position and its intrinsic size, and both are used as the fallbacks
 * the specification says to use.
 */
void ar_position_out_of_flow(ar_node *nodes, ar_i32 i, ar_rect viewport)
{
    ar_node *n = &nodes[i];
    ar_rect  cb = ar_containing_block(nodes, i, viewport);
    ar_i32   x = n->rect.x, y = n->rect.y, w = n->rect.w, h = n->rect.h;
    int      auto_h = n->style.unit[AR_P_MARGIN_LEFT] == AR_UNIT_AUTO &&
                      n->style.unit[AR_P_MARGIN_RIGHT] == AR_UNIT_AUTO;
    int      auto_v = n->style.unit[AR_P_MARGIN_TOP] == AR_UNIT_AUTO &&
                      n->style.unit[AR_P_MARGIN_BOTTOM] == AR_UNIT_AUTO;

    ar__resolve_axis(n, cb.x, cb.w, AR_P_LEFT, AR_P_RIGHT, AR_P_WIDTH,
                     auto_h ? 0 : n->style.v[AR_P_MARGIN_LEFT],
                     auto_h ? 0 : n->style.v[AR_P_MARGIN_RIGHT], auto_h, n->fit[0], &x, &w);
    ar__resolve_axis(n, cb.y, cb.h, AR_P_TOP, AR_P_BOTTOM, AR_P_HEIGHT,
                     auto_v ? 0 : n->style.v[AR_P_MARGIN_TOP],
                     auto_v ? 0 : n->style.v[AR_P_MARGIN_BOTTOM], auto_v, n->fit[1], &y, &h);

    n->rect.x = x;
    n->rect.y = y;
    n->rect.w = w < 0 ? 0 : w;
    n->rect.h = h < 0 ? 0 : h;
}

/*
 * Moves a box and its whole subtree.
 *
 * Children come after their parents in the array, so a forward walk from the
 * box reaches all of them; the ancestor test is what keeps it to the subtree.
 */
static void ar__shift_subtree(ar_node *nodes, ar_i32 count, ar_i32 i, ar_i32 dx, ar_i32 dy)
{
    ar_i32 j;

    if (!dx && !dy)
    {
        return;
    }
    nodes[i].rect.x += dx;
    nodes[i].rect.y += dy;
    for (j = i + 1; j < count; ++j)
    {
        ar_i32 at = nodes[j].parent;

        while (at >= 0 && at != i)
        {
            at = nodes[at].parent;
        }
        if (at == i)
        {
            nodes[j].rect.x += dx;
            nodes[j].rect.y += dy;
        }
    }
}

/* The box a sticky element is pinned inside: its nearest scrolling ancestor,
   or the viewport when there is none. */
static ar_rect ar__scrollport(const ar_node *nodes, ar_i32 i, ar_rect viewport)
{
    ar_i32 at;

    for (at = nodes[i].parent; at >= 0; at = nodes[at].parent)
    {
        if (ar_is_scroll_container(&nodes[at]))
        {
            const ar_node *a = &nodes[at];

            return ar_rect_make(a->rect.x + a->style.v[AR_P_PAD_LEFT],
                                a->rect.y + a->style.v[AR_P_PAD_TOP],
                                a->rect.w - a->style.v[AR_P_PAD_LEFT] - a->style.v[AR_P_PAD_RIGHT],
                                a->rect.h - a->style.v[AR_P_PAD_TOP] - a->style.v[AR_P_PAD_BOTTOM]);
        }
    }
    return viewport;
}

/* One axis of one sticky box: how far it has to move to obey its thresholds
   without leaving the box it belongs to. */
static ar_i32 ar__sticky_shift(const ar_node *n, const ar_node *parent, ar_i32 port_lo,
                               ar_i32 port_hi, ar_prop lead_p, ar_prop trail_p, ar_i32 box_lo,
                               ar_i32 box_size, ar_i32 pad_lead, ar_i32 pad_trail)
{
    ar_i32 box_hi = box_lo + box_size;
    ar_i32 shift = 0;

    if (ar__given(n, lead_p))
    {
        ar_i32 want = port_lo + ar__offset(n, lead_p, port_hi - port_lo);

        if (box_lo < want)
        {
            shift = want - box_lo;
        }
    }
    if (ar__given(n, trail_p))
    {
        ar_i32 want = port_hi - ar__offset(n, trail_p, port_hi - port_lo);

        if (box_hi + shift > want)
        {
            shift = want - box_hi;
        }
    }
    if (!shift)
    {
        return 0;
    }

    /*
     * Never outside the containing block.
     *
     * This is the part that makes sticky useful rather than merely fixed: a
     * section header pins to the top of the scrollport, and then slides back
     * up and out as its own section scrolls past, handing over to the next
     * one. Without the clamp every header would pile up at the top.
     */
    if (parent)
    {
        ar_i32 p_lo, p_hi, lo, hi;

        if (lead_p == AR_P_TOP)
        {
            p_lo = parent->rect.y + pad_lead;
            p_hi = parent->rect.y + parent->rect.h - pad_trail;
        }
        else
        {
            p_lo = parent->rect.x + pad_lead;
            p_hi = parent->rect.x + parent->rect.w - pad_trail;
        }
        /*
         * The two edges give a range the shift has to land in, and it has to
         * be read as a range rather than as two corrections in a row.
         *
         *     box_lo + shift >= p_lo   ->   shift >= p_lo - box_lo
         *     box_hi + shift <= p_hi   ->   shift <= p_hi - box_hi
         *
         * Applying them one after the other looks equivalent and is not. When
         * the box is bigger than the block it must stay inside, the range is
         * empty -- there is no shift that keeps both edges in -- and the
         * second correction simply overwrote the first, producing a large
         * shift in the wrong direction. A horizontally scrolling row 420 wide
         * in a 200 wide scrollport was pinned at -220 whatever the scroll
         * position, which the browser comparison caught: a real browser leaves
         * such a box alone, because there is nowhere it can go that satisfies
         * the constraint it is under.
         */
        lo = p_lo - box_lo;
        hi = p_hi - box_hi;
        if (lo > hi)
        {
            return 0;
        }
        if (shift < lo)
        {
            shift = lo;
        }
        if (shift > hi)
        {
            shift = hi;
        }
    }
    return shift;
}

/*
 * `position: sticky`: in flow, until a threshold.
 *
 * It keeps its place in the flow exactly as `relative` does -- the space it
 * occupied stays occupied -- and is then nudged just far enough to obey
 * whichever of its four offsets were given, without ever leaving the box it
 * belongs to.
 *
 * Runs after scrolling, because the whole point is to react to it.
 */
void ar_position_sticky(ar_node *nodes, ar_i32 count, ar_rect viewport)
{
    ar_i32 i;

    for (i = 0; i < count; ++i)
    {
        ar_node *n = &nodes[i];
        ar_node *parent;
        ar_rect  port;
        ar_i32   dx, dy;

        if (!ar_is_sticky(n) || n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        parent = n->parent >= 0 ? &nodes[n->parent] : 0;
        port = ar__scrollport(nodes, i, viewport);

        dy = ar__sticky_shift(n, parent, port.y, port.y + port.h, AR_P_TOP, AR_P_BOTTOM, n->rect.y,
                              n->rect.h, parent ? parent->style.v[AR_P_PAD_TOP] : 0,
                              parent ? parent->style.v[AR_P_PAD_BOTTOM] : 0);
        dx = ar__sticky_shift(n, parent, port.x, port.x + port.w, AR_P_LEFT, AR_P_RIGHT, n->rect.x,
                              n->rect.w, parent ? parent->style.v[AR_P_PAD_LEFT] : 0,
                              parent ? parent->style.v[AR_P_PAD_RIGHT] : 0);

        ar__shift_subtree(nodes, count, i, dx, dy);
    }
}

/*
 * `relative` shifts a box and everything inside it, for painting only.
 *
 * The space it occupied stays occupied -- that is the whole difference from
 * `absolute`, and the reason a relatively positioned box leaves a hole where
 * it was rather than letting its siblings close up.
 *
 * The subtree moves with it, which is why this is applied after the flow has
 * finished placing everything: shifting a parent before its children were
 * placed would move them twice.
 */
void ar_position_relative(ar_node *nodes, ar_i32 count, ar_rect viewport)
{
    ar_i32 i;

    for (i = 0; i < count; ++i)
    {
        ar_node *n = &nodes[i];
        ar_i32   dx = 0, dy = 0;

        if (n->style.v[AR_P_POSITION] != AR_POS_RELATIVE)
        {
            continue;
        }

        if (ar__given(n, AR_P_LEFT))
        {
            dx = ar__offset(n, AR_P_LEFT, viewport.w);
        }
        else if (ar__given(n, AR_P_RIGHT))
        {
            dx = -ar__offset(n, AR_P_RIGHT, viewport.w);
        }
        if (ar__given(n, AR_P_TOP))
        {
            dy = ar__offset(n, AR_P_TOP, viewport.h);
        }
        else if (ar__given(n, AR_P_BOTTOM))
        {
            dy = -ar__offset(n, AR_P_BOTTOM, viewport.h);
        }
        if (!dx && !dy)
        {
            continue;
        }

        ar__shift_subtree(nodes, count, i, dx, dy);
    }
}
