/*
 * areole - stacking order
 * SPDX-License-Identifier: MIT
 *
 * What covers what. Painting stops being a forward walk of the box array and
 * becomes a walk of the stacking tree.
 *
 * Declaration order was right often enough to have hidden the problem: a box
 * declared later is drawn later, and most of the time that is also what should
 * be on top. It stops being true the moment anything is positioned, which is
 * why a dropdown could land in exactly the right place and still be painted
 * underneath the thing after it.
 *
 * ------------------------------------------------------------------------
 * The order, from CSS 2.1 Appendix E
 *
 * Within one stacking context, in this order:
 *
 *   1  the background and borders of the element forming the context
 *   2  child stacking contexts with negative z-index, most negative first
 *   3  in-flow, non-inline-level, non-positioned descendants
 *   4  non-positioned floats
 *   5  in-flow, inline-level, non-positioned descendants
 *   6  positioned descendants with z-index auto or 0
 *   7  child stacking contexts with positive z-index, least positive first
 *
 * Two things fall out of that list which are worth saying plainly, because
 * both surprise people:
 *
 *   - A float paints *under* inline content, not over it. That is why text
 *     wraps around an image rather than being hidden by it.
 *   - A positioned box with no z-index at all still paints above every
 *     unpositioned sibling. `position: relative` on its own changes what
 *     covers what, which is why it is so often added to fix a stacking bug
 *     and so rarely understood afterwards.
 *
 * ------------------------------------------------------------------------
 * Cost
 *
 * Each context walks its own subtree once per bucket, so a tree of depth d
 * costs about 7 * n * d node visits. It is a walk of integers over an array
 * that is already in cache, it happens once per frame, and it buys an order
 * that is right rather than usually right. If it ever shows up in the
 * measurements, the fix is a single pass that appends to seven lists.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

int ar_z_is_auto(const ar_node *n)
{
    return n->style.unit[AR_P_Z_INDEX] == AR_UNIT_AUTO;
}

/*
 * Does this box start a stacking context of its own?
 *
 * The root always does. Otherwise it takes a position other than static *and*
 * a z-index that is not auto -- both, which is why `z-index` on an unpositioned
 * box does nothing at all and is the other half of the confusion above.
 */
int ar_forms_stacking_context(const ar_node *n)
{
    if (n->parent < 0)
    {
        return 1;
    }
    return ar_is_positioned(n) && !ar_z_is_auto(n);
}

/*
 * Is this box painted as one unit, subtree and all, somewhere other than where
 * the flow would have put it?
 *
 * Everything that forms a context, and every positioned box besides. CSS 2.1
 * says a positioned box with `z-index: auto` is painted as though it formed a
 * context except that its own positioned descendants rejoin the parent
 * context. areole paints the subtree atomically instead, which differs only
 * when a positioned descendant of a positioned box has to interleave with its
 * grandparent's siblings -- and is noted rather than hidden.
 */
static int ar__atomic(const ar_node *n)
{
    return n->parent >= 0 && (ar_is_positioned(n) || ar_forms_stacking_context(n));
}

/* Which of Appendix E's buckets a descendant belongs to. */
static ar_i32 ar__bucket(const ar_node *n)
{
    if (ar_forms_stacking_context(n))
    {
        ar_i32 z = n->style.v[AR_P_Z_INDEX];

        if (z < 0)
        {
            return 2;
        }
        return z > 0 ? 7 : 6;
    }
    if (ar_is_positioned(n))
    {
        return 6; /* z-index auto: above the flow, below anything with a z */
    }
    if (ar_is_floated(n))
    {
        return 4;
    }
    if (ar_is_inline_level(n))
    {
        return 5;
    }
    return 3;
}

typedef struct ar__walk
{
    ar_node *nodes;
    ar_i32   count;
    ar_i32  *order;
    ar_i32   cap;
    ar_i32   used;
} ar__walk;

/*
 * Is this box in the top layer?
 *
 * The root is never in it however it is styled: it is the layer everything
 * else is on top of, and putting it above itself would be a loop with no
 * meaning.
 */
int ar_in_top_layer(const ar_node *n)
{
    return n->parent >= 0 && n->style.v[AR_P_OVERLAY] == AR_OVERLAY_AUTO;
}

static void ar__push(ar__walk *w, ar_i32 i)
{
    if (w->used < w->cap)
    {
        w->order[w->used++] = i;
    }
}

static void ar__context(ar__walk *w, ar_i32 root);

/*
 * Emits every descendant of `root` in one bucket.
 *
 * Stops descending at anything painted atomically: that box belongs to its own
 * bucket and takes its subtree with it, so walking into it here would paint it
 * twice and in the wrong order both times.
 */
static void ar__bucket_walk(ar__walk *w, ar_i32 root, ar_i32 at, ar_i32 want, ar_i32 z_lo,
                            ar_i32 z_hi)
{
    ar_i32 c;

    for (c = w->nodes[at].first_child; c >= 0; c = w->nodes[c].next_sibling)
    {
        ar_node *ch = &w->nodes[c];
        ar_i32   bucket;

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }

        /* In the top layer: emitted by the second pass in ar_stack_order, on
           top of everything this one produces. Skipped rather than descended
           into, so it is not also painted where the flow would have put it. */
        if (ar_in_top_layer(ch))
        {
            continue;
        }
        bucket = ar__bucket(ch);

        if (ar__atomic(ch))
        {
            ar_i32 z = ar_z_is_auto(ch) ? 0 : ch->style.v[AR_P_Z_INDEX];

            if (bucket == want && z >= z_lo && z <= z_hi)
            {
                ar__context(w, c);
            }
            continue; /* never descend into it from here */
        }

        if (bucket == want)
        {
            ar__push(w, c);
        }
        ar__bucket_walk(w, root, c, want, z_lo, z_hi);
    }
}

/*
 * The smallest z greater than `prev` used by a context-forming box in this
 * context. Zero is skipped: it is bucket 6 and needs no sweep of its own.
 *
 * Descends exactly as ar__bucket_walk does, and for the same reason: an atomic
 * box's own z belongs to this context, and its descendants' z values belong to
 * the context it forms.
 */
static void ar__z_next(ar__walk *w, ar_i32 at, int have_prev, ar_i32 prev, int *have, ar_i32 *out)
{
    ar_i32 c;

    for (c = w->nodes[at].first_child; c >= 0; c = w->nodes[c].next_sibling)
    {
        ar_node *ch = &w->nodes[c];

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (ar__atomic(ch))
        {
            if (ar_forms_stacking_context(ch))
            {
                ar_i32 z = ch->style.v[AR_P_Z_INDEX];

                if (z != 0 && (!have_prev || z > prev) && (!*have || z < *out))
                {
                    *out = z;
                    *have = 1;
                }
            }
            continue; /* never descend into it from here */
        }
        ar__z_next(w, c, have_prev, prev, have, out);
    }
}

/*
 * One stacking context, in Appendix E order.
 *
 * The negative and positive passes visit the z values the stylesheet actually
 * uses, found by repeatedly asking for the next one up. Sweeping every
 * representable value instead -- which is what this did -- walked the whole
 * tree 68 times a frame whatever the stylesheet said. flat_1k has no
 * positioned box in it at all and spent 1106 us in layout against 42 before
 * paint order existed: a 26x regression on a scene with no z-index. Bisected,
 * not guessed.
 *
 * Asking for the next value costs one walk per distinct z, which is why this
 * beats both the old sweep and a sort: a stylesheet uses a handful of layers,
 * and a file with no allocator has nowhere to sort them anyway. It also has no
 * ceiling, where the sweep silently never painted a box past its range.
 */
static void ar__context(ar__walk *w, ar_i32 root)
{
    ar_i32 z = 0, prev = 0;
    int    have_prev = 0, have;

    ar__push(w, root); /* 1: the context's own background and borders */

    /* 2: negative z, most negative first. */
    for (;;)
    {
        have = 0;
        ar__z_next(w, root, have_prev, prev, &have, &z);
        if (!have || z > 0)
        {
            break;
        }
        ar__bucket_walk(w, root, root, 2, z, z);
        prev = z;
        have_prev = 1;
    }

    ar__bucket_walk(w, root, root, 3, 0, 0); /* in-flow blocks   */
    ar__bucket_walk(w, root, root, 4, 0, 0); /* floats           */
    ar__bucket_walk(w, root, root, 5, 0, 0); /* inline content   */
    ar__bucket_walk(w, root, root, 6, 0, 0); /* positioned, z auto or 0 */

    /* 7: positive z, least positive first. The negative loop stopped on the
       first non-negative it saw without consuming it, so this finds it again. */
    for (;;)
    {
        have = 0;
        ar__z_next(w, root, have_prev, prev, &have, &z);
        if (!have)
        {
            break;
        }
        ar__bucket_walk(w, root, root, 7, z, z);
        prev = z;
        have_prev = 1;
    }
}

/*
 * Fills `order` with every visible box, back to front.
 *
 * Returns how many were written. A box that did not fit is not painted, which
 * is a visible failure; the array is sized from the box count, so it only
 * happens if a caller passes a smaller one.
 */
ar_i32 ar_stack_order(ar_node *nodes, ar_i32 count, ar_i32 *order, ar_i32 cap)
{
    ar__walk w;
    ar_i32   i;

    if (count <= 0 || !order || cap <= 0)
    {
        return 0;
    }
    w.nodes = nodes;
    w.count = count;
    w.order = order;
    w.cap = cap;
    w.used = 0;

    ar__context(&w, 0);

    /*
     * The top layer, on top of all of it.
     *
     * A second short list rather than a very large z-index, which is the whole
     * point of the concept: a z-index cannot lift a box out of its stacking
     * context, so a modal inside anything positioned could always be covered
     * by that thing's siblings. This is also why the bound on z-index was
     * removed rather than raised -- the escape hatch was never going to work.
     *
     * Tree order, so a box opened later covers one opened earlier, which is
     * what a stack of dialogs means. Each is emitted exactly once: the walk
     * above skipped them, so nothing is painted twice and `order` stays one
     * slot per box.
     */
    for (i = 0; i < count; ++i)
    {
        if (ar_in_top_layer(&nodes[i]) && nodes[i].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE)
        {
            ar__context(&w, i);
        }
    }
    return w.used;
}
