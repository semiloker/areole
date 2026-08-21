/*
 * areole - the flex formatting context
 * SPDX-License-Identifier: MIT
 *
 * CSS Flexible Box Layout, replacing the subset areole shipped with.
 *
 * The subset was direction, gap, justify, align and a `grow` keyword that is
 * not CSS, and it did the distribution with one division: leftover space over
 * the number of growers. That is right for the case everybody writes and wrong
 * for every case with a `flex-shrink`, a `flex-basis`, a `min-width`, or two
 * items with different factors -- which is most of what real stylesheets say.
 *
 * ------------------------------------------------------------------------
 * The part that is not one division
 *
 * §9.7 of the specification is a loop, and the loop exists because clamping is
 * not distributive. Give three items a share each, and one of them hits its
 * `max-width` and cannot take its share; the space it gave back has to go to
 * the other two, which may push one of *them* into its maximum, and so on. So
 * each pass freezes the items that hit a bound and redistributes among the
 * rest, and it terminates because every pass freezes at least one item.
 *
 * A single division gets the common case right and is silently wrong the
 * moment a `min-width` bites -- and a `min-width` that bites is the normal
 * state of a flex row of text, because the automatic minimum size below gives
 * every item one whether or not anybody wrote it.
 *
 * ------------------------------------------------------------------------
 * The automatic minimum size
 *
 * `min-width: auto` on a flex item is not zero: it resolves to the item's
 * min-content size. This is the rule behind almost every "why will my flex
 * item not shrink" question ever asked -- a long word or a wide child stops
 * the item shrinking past it, and nothing in the stylesheet says so.
 *
 * areole's `min-width` defaults to 0 rather than `auto`, which is what CSS
 * says for a block. For a flex item the automatic minimum applies unless a
 * minimum was stated, and `ar__auto_min` below is where the two meet.
 *
 * ------------------------------------------------------------------------
 * Two passes, and why there is no array of lines
 *
 * `align-content` distributes the *lines* of a wrapped container, so it needs
 * the total cross size before it can place the first line -- and a line's
 * cross size needs its items' resolved main sizes, because that is what their
 * text wrapped at. An array of lines would need to be as long as the item
 * count in the worst case, and this engine does not allocate.
 *
 * So the solve runs twice, once to total the lines and once to place them,
 * exactly as the table's does. Both passes are O(items); the cost is a
 * constant factor and the alternative is a bound nobody can justify.
 */
#include "ar_node.h"

/*
 * Where an item's main size stands during §9.7.
 *
 * The target lives in the item's own rect -- it *is* the target -- and frozen
 * is a bit on the node. The flex base size is deliberately not stored: it is a
 * pure function of the item and the container's inner main size, so each pass
 * of the loop recomputes it rather than carrying a field on every box in the
 * interface for the sake of a handful that are flex items. Every pass freezes
 * at least one item, so the recomputation is bounded by the same thing the
 * loop is.
 */

static int ar__flex_hidden(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE;
}

/* An item that is out of flow is not a flex item: it is placed against the
   container's padding box by the positioning pass and takes part in no line. */
static int ar__flex_item(const ar_node *n)
{
    return !ar__flex_hidden(n) && !ar_is_out_of_flow(n);
}

/*
 * The next item in `order`, or in document order when nothing said otherwise.
 *
 * ponytail: the ordered walk is a selection scan, so a container whose items
 * carry `order` costs O(n^2) to iterate. `order` on a handful of boxes is what
 * the property is for -- moving a sidebar to the front without moving it in
 * the markup -- and `order` on five hundred is not something anybody writes.
 * The upgrade is a frame-arena index array, the same shape as c->order, and it
 * is worth doing when a second reader appears.
 */
static ar_i32 ar__flex_next(const ar_node *nodes, ar_i32 parent, ar_i32 prev, int ordered)
{
    ar_i32 c;

    if (!ordered)
    {
        c = prev < 0 ? nodes[parent].first_child : nodes[prev].next_sibling;
        while (c >= 0 && !ar__flex_item(&nodes[c]))
        {
            c = nodes[c].next_sibling;
        }
        return c;
    }

    {
        ar_i32 best = -1;
        ar_i32 po = prev < 0 ? 0 : nodes[prev].style.v[AR_P_ORDER];

        for (c = nodes[parent].first_child; c >= 0; c = nodes[c].next_sibling)
        {
            ar_i32 o;

            if (!ar__flex_item(&nodes[c]))
            {
                continue;
            }
            o = nodes[c].style.v[AR_P_ORDER];

            /* Strictly after `prev`: a lower order, or the same order later in
               the tree. Ties keep document order, which is what makes `order`
               a stable sort rather than an arbitrary one. */
            if (prev >= 0 && (o < po || (o == po && c <= prev)))
            {
                continue;
            }
            if (best < 0 || o < nodes[best].style.v[AR_P_ORDER])
            {
                best = c;
            }
        }
        return best;
    }
}

static int ar__flex_ordered(const ar_node *nodes, ar_i32 parent)
{
    ar_i32 c;

    for (c = nodes[parent].first_child; c >= 0; c = nodes[c].next_sibling)
    {
        if (nodes[c].style.v[AR_P_ORDER] != 0)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * The automatic minimum size, which is the one nobody wrote and everybody
 * runs into.
 *
 * A stated `min-width` wins. Otherwise a flex item cannot shrink below its
 * min-content size on the main axis -- the widest unbreakable thing in it --
 * which is what stops a row of buttons from crushing their own labels, and
 * what makes a row of paragraphs overflow instead of getting narrower.
 */
static ar_i32 ar__auto_min(const ar_node *n, ar_i32 axis)
{
    ar_i32 prop = ar_axis_min_prop(axis);

    /*
     * A stated minimum wins, and `min-height: 0` is a stated minimum.
     *
     * That distinction is the whole of why this reads the set mask rather than
     * the value. areole's `min-height` defaults to 0, so a zero value is
     * ambiguous -- and `min-width: 0` / `min-height: 0` on a flex item is not
     * an idle declaration, it is *the* well-known way to turn the automatic
     * minimum off. Treating it as "nothing was said" would make the one
     * declaration that exists to disable this rule do nothing at all.
     */
    if (ar_pset_has(n->style.set, prop))
    {
        return n->style.v[prop];
    }
    if (n->style.unit[ar_axis_size_prop(axis)] == AR_UNIT_PX)
    {
        /* A stated size is its own floor: an item that says `width: 200px`
           and nothing about shrinking does not get squeezed to its text. */
        return 0;
    }
    /*
     * A box that clips has no automatic minimum, and the specification says so
     * outright. It is the rule that makes `overflow: hidden` the other
     * well-known way out of "my flex item will not shrink": a box that clips
     * its own contents has no reason to be as large as them.
     *
     * `hidden` counts, which is why this asks about overflow rather than
     * calling ar_is_scroll_container -- that one means "can be scrolled", and
     * a box that clips without a bar is exactly the case in hand.
     *
     * Without it a pane that says `height: grow; overflow: hidden` is floored
     * at the height of everything inside it and grows past the window instead
     * of clipping -- which is the whole of what it asked not to do.
     */
    if (ar_overflow_x(n) != AR_OVERFLOW_VISIBLE || ar_overflow_y(n) != AR_OVERFLOW_VISIBLE)
    {
        return 0;
    }

    /*
     * The automatic minimum, on both axes.
     *
     * Along a row it is the item's min-content width -- the widest thing in it
     * that cannot be broken -- which is what stops a row of buttons crushing
     * their own labels. Along a column it is the item's content height, which
     * is what stops a column of rows squeezing its text when the container
     * overflows.
     *
     * Returning zero on the vertical axis was the first version, and it made a
     * scrolling page of rows come out two pixels short per row against a
     * browser: every item has `flex-shrink: 1` by default, so a column whose
     * content overflows squeezes every row in it unless something stops it,
     * and this is the something.
     */
    /* min_w is the min-content width -- the widest unbreakable thing. There is
       no min-content *height* on a node, and fit[1] is the height the contents
       come to, which is the same answer for everything that is not itself a
       wrapping paragraph. */
    return axis == 0 ? n->min_w : n->fit[1];
}

/*
 * §9.2.3: the flex base size.
 *
 * `flex-basis` first, then `width`/`height`, then the contents. The order is
 * the whole of why `flex: 1` and `flex-grow: 1` behave differently -- the
 * shorthand writes a zero basis and the longhand leaves it `auto`, so one
 * makes three boxes equal and the other shares only the surplus.
 */
static ar_i32 ar__flex_base(const ar_node *n, ar_i32 axis, ar_i32 inner_main)
{
    ar_u8 bu = n->style.unit[AR_P_FLEX_BASIS];

    /*
     * Every stated number goes through ar_used_size, and the intrinsic ones do
     * not.
     *
     * A rect in this engine is a border box; a stated `width` under
     * `content-box` -- CSS's default -- is what goes *inside* the padding. So a
     * rail that says `width: 120px; padding: 10px` occupies 140, and a base
     * that returned 120 made it twenty narrower than every other box in the
     * engine with the same declaration. fit[] already carries its padding,
     * because the intrinsic pass put it there.
     */
    if (bu == AR_UNIT_PX)
    {
        return ar_used_size(n, axis, n->style.v[AR_P_FLEX_BASIS]);
    }
    if (bu == AR_UNIT_PCT)
    {
        return ar_used_size(n, axis, inner_main * n->style.v[AR_P_FLEX_BASIS] / 100);
    }
    if (bu == AR_UNIT_CONTENT)
    {
        return n->fit[axis];
    }

    /* `auto`: defer to the size property, and to the contents when that is
       automatic too. AR_UNIT_GROW is areole's own keyword and behaves as
       `flex: 1` did before there was a `flex` shorthand to say it with. */
    switch (n->style.unit[ar_axis_size_prop(axis)])
    {
    case AR_UNIT_PX:
        return ar_used_size(n, axis, n->style.v[ar_axis_size_prop(axis)]);
    case AR_UNIT_PCT:
        return ar_used_size(n, axis, inner_main * n->style.v[ar_axis_size_prop(axis)] / 100);
    case AR_UNIT_GROW:
        return 0;
    default:
        return n->fit[axis];
    }
}

static ar_i32 ar__grow_of(const ar_node *n, ar_i32 axis)
{
    /* areole's `grow` keyword is an alias for `flex: 1`, kept because existing
       stylesheets use it and documented as areole-specific. A real `flex-grow`
       beside it wins, because it is the one CSS defines.

       It only grows along the *main* axis. `width: grow; height: 24px` in a
       column means "fill the row, be 24 tall", and reading the width as a grow
       factor there made the item take the whole column instead -- a keyword on
       the cross axis is a stretch, not a factor. */
    if (n->style.v[AR_P_FLEX_GROW] != 0)
    {
        return n->style.v[AR_P_FLEX_GROW];
    }
    if (n->style.unit[ar_axis_size_prop(axis)] == AR_UNIT_GROW)
    {
        return 1000;
    }
    return 0;
}

/* a * b / d for factors in thousandths, without leaving ar_i32 on the way. */
static ar_i32 ar__share(ar_i32 space, ar_i32 factor, ar_i32 total)
{
    if (total <= 0 || factor <= 0)
    {
        return 0;
    }
    if (space >= 0)
    {
        if (space <= 2147483647 / (factor > 1 ? factor : 1))
        {
            return space * factor / total;
        }
        return (space / total) * factor;
    }
    if (-space <= 2147483647 / (factor > 1 ? factor : 1))
    {
        return space * factor / total;
    }
    return (space / total) * factor;
}

/*
 * §9.7: resolve the flexible lengths of one line.
 *
 * `first` and `stop` bound the line in the item walk; `stop` is the first item
 * of the next line, or -1. On return every item on the line has its main size
 * in its rect.
 */
static void ar__resolve_line(ar_node *nodes, ar_i32 parent, ar_i32 first, ar_i32 stop, ar_i32 axis,
                             ar_i32 inner_main, ar_i32 gap, ar_i32 count, int ordered)
{
    ar_i32 c;
    ar_i32 hypo_total = 0;
    int    growing;
    ar_i32 guard;

    /* Pass one: the hypothetical main size of every item, which is its base
       clamped to its own minimum and maximum. The sum against the container
       is what decides whether this line grows or shrinks -- one or the other,
       never both, which is §9.7 step 1. */
    for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, parent, c, ordered))
    {
        ar_node *it = &nodes[c];
        ar_i32   base = ar__flex_base(it, axis, inner_main);
        ar_i32   hypo =
            ar_clamp(base, ar__auto_min(it, axis), AR_WIDE(&it->style, ar_axis_max_prop(axis)));

        *ar_axis_size(&it->rect, axis) = hypo;
        it->state = (ar_u16)(it->state & ~AR_STATE_FLEX_FROZEN);
        hypo_total +=
            hypo + ar_axis_margin_lead(&it->style, axis) + ar_axis_margin_trail(&it->style, axis);
    }
    if (count > 1)
    {
        hypo_total += gap * (count - 1);
    }

    growing = hypo_total < inner_main;

    /*
     * The loop. Bounded by the item count because every pass either finishes
     * or freezes at least one item, and the guard says so out loud rather than
     * trusting the argument -- a solver that cannot terminate takes the whole
     * frame with it, and this one runs on input nobody validated.
     */
    for (guard = 0; guard <= count; ++guard)
    {
        ar_i32 free_space = inner_main;
        ar_i32 factor_sum = 0;
        ar_i32 unfrozen = 0;
        ar_i32 violation = 0;
        ar_i32 spent = 0;

        if (count > 1)
        {
            free_space -= gap * (count - 1);
        }
        for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, parent, c, ordered))
        {
            ar_node *it = &nodes[c];
            ar_i32   base = ar__flex_base(it, axis, inner_main);

            /*
             * The first pass is also where items that cannot move get frozen:
             * no factor in the direction being resolved, or a base already
             * past the hypothetical in the direction that would make it worse.
             *
             * This used to be a walk of its own between the hypothetical sizes
             * and this loop. It cannot fold into the *hypothetical* pass --
             * `growing` is not known until that pass has summed the whole line
             * -- but it folds into this one, which already visits every item
             * and already has its base in hand. Only on the first pass: after
             * that the rect no longer holds the hypothetical size and the
             * comparison below would be against a number that has moved.
             *
             * One walk of the line per resolution, out of the eight or nine a
             * flex container was costing every box every frame.
             */
            if (guard == 0)
            {
                ar_i32 f0 = growing ? ar__grow_of(it, axis) : it->style.v[AR_P_FLEX_SHRINK];

                if (f0 <= 0 || (growing && base > *ar_axis_size(&it->rect, axis)) ||
                    (!growing && base < *ar_axis_size(&it->rect, axis)))
                {
                    it->state = (ar_u16)(it->state | AR_STATE_FLEX_FROZEN);
                }
            }

            free_space -=
                ar_axis_margin_lead(&it->style, axis) + ar_axis_margin_trail(&it->style, axis);
            if (it->state & AR_STATE_FLEX_FROZEN)
            {
                free_space -= *ar_axis_size(&it->rect, axis);
            }
            else
            {
                free_space -= base;
                ++unfrozen;
                /* Shrinking is weighted by the base size as well as the
                   factor: a wide item gives up more than a narrow one with
                   the same `flex-shrink`, which is what stops the narrow one
                   vanishing first. */
                factor_sum += growing ? ar__grow_of(it, axis)
                                      : ar__share(base, it->style.v[AR_P_FLEX_SHRINK], 1000);
            }
        }
        if (unfrozen == 0)
        {
            break;
        }

        for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, parent, c, ordered))
        {
            ar_node *it = &nodes[c];
            ar_i32   base, factor, want, got;

            if (it->state & AR_STATE_FLEX_FROZEN)
            {
                continue;
            }
            base = ar__flex_base(it, axis, inner_main);
            factor = growing ? ar__grow_of(it, axis)
                             : ar__share(base, it->style.v[AR_P_FLEX_SHRINK], 1000);
            want = base + ar__share(free_space, factor, factor_sum);
            got =
                ar_clamp(want, ar__auto_min(it, axis), AR_WIDE(&it->style, ar_axis_max_prop(axis)));

            *ar_axis_size(&it->rect, axis) = got;
            /* What this pass actually handed out, which is what the leftover
               pixel below is the remainder of. Accumulated here rather than
               recovered by a second walk subtracting each base back off: this
               loop is already holding both numbers. */
            spent += got - base;
            /* Which way the clamp moved it, summed over the line: positive
               means the minimums pushed back, negative means the maximums
               did. Zero means nothing was clamped and the line is settled. */
            violation += got - want;
        }

        if (violation == 0)
        {
            /*
             * The pixel the division dropped.
             *
             * Three items sharing a hundred pixels get 33 each and the line
             * ends two short of the edge. Nobody can explain that gap and
             * everybody notices it, so the remainder is handed out one pixel
             * at a time to the first items that can take it -- 34, 33, 33,
             * meeting the far edge exactly.
             *
             * Only when growing, and only to items that are not already at a
             * maximum: giving a pixel to an item that is clamped would put the
             * line right back into violation and the loop would run again.
             */
            if (growing)
            {
                /* What the distribution above could not place, which is the
                   line's width minus everything it handed out. `spent` is that
                   sum, kept by the loop that did the handing out. */
                ar_i32 slack = free_space - spent;

                for (c = first; c >= 0 && c != stop && slack > 0;
                     c = ar__flex_next(nodes, parent, c, ordered))
                {
                    ar_node *it = &nodes[c];
                    ar_i32   cap;

                    if (it->state & AR_STATE_FLEX_FROZEN)
                    {
                        continue;
                    }
                    cap = AR_WIDE(&it->style, ar_axis_max_prop(axis));
                    if (*ar_axis_size(&it->rect, axis) < cap)
                    {
                        *ar_axis_size(&it->rect, axis) += 1;
                        --slack;
                    }
                }
            }
            break;
        }
        /* Freeze exactly the items the clamp moved in the same direction as
           the total. Freezing the ones that moved the other way as well is
           the bug that makes a container of mixed minimums and maximums
           oscillate instead of settling. */
        for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, parent, c, ordered))
        {
            ar_node *it = &nodes[c];
            ar_i32   base, factor, want;

            if (it->state & AR_STATE_FLEX_FROZEN)
            {
                continue;
            }
            base = ar__flex_base(it, axis, inner_main);
            factor = growing ? ar__grow_of(it, axis)
                             : ar__share(base, it->style.v[AR_P_FLEX_SHRINK], 1000);
            want = base + ar__share(free_space, factor, factor_sum);
            if ((violation > 0 && *ar_axis_size(&it->rect, axis) > want) ||
                (violation < 0 && *ar_axis_size(&it->rect, axis) < want))
            {
                it->state = (ar_u16)(it->state | AR_STATE_FLEX_FROZEN);
            }
            else
            {
                *ar_axis_size(&it->rect, axis) = want;
            }
        }
    }
}

/*
 * The whole solve, optionally writing the rectangles it works out.
 *
 * Returns the cross size the contents came to, which is what an automatic
 * height on the container becomes.
 */
static ar_i32 ar__flex_solve(ar_node *nodes, ar_i32 i, ar_layout_env *env, int assign)
{
    ar_node *n = &nodes[i];
    ar_i32   axis = ar_axis_main(n);
    ar_i32   cross = axis ^ 1;
    ar_i32   gap = n->style.v[AR_P_GAP];
    ar_i32   wrap = n->style.v[AR_P_FLEX_WRAP];
    int      ordered = ar__flex_ordered(nodes, i);
    ar_i32   inner_main, inner_cross;
    ar_i32   first, c;
    ar_i32   cross_total = 0;
    ar_i32   line_count = 0;
    ar_i32   align_items = n->style.v[AR_P_ALIGN];
    ar_i32   pass;
    ar_i32   line_lead = 0, line_between = 0;

    inner_main = *ar_axis_size(&n->rect, axis) - ar_axis_pad_lead(&n->style, axis) -
                 ar_axis_pad_trail(&n->style, axis);
    inner_cross = *ar_axis_size(&n->rect, cross) - ar_axis_pad_lead(&n->style, cross) -
                  ar_axis_pad_trail(&n->style, cross);
    if (inner_main < 0)
    {
        inner_main = 0;
    }
    if (inner_cross < 0)
    {
        inner_cross = 0;
    }

    /* Out-of-flow children get a static position and nothing else; the
       positioning pass resolves whatever their offsets override. */
    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        if (assign && ar__flex_hidden(&nodes[c]))
        {
            nodes[c].rect = ar_rect_make(n->rect.x, n->rect.y, 0, 0);
        }
    }

    /*
     * Pass 0 totals the lines, pass 1 places them, and only `align-content`
     * needs the total.
     *
     * A container that does not wrap has one line, so there is nothing for
     * `align-content` to distribute and pass 0 is pure waste -- and not cheap
     * waste: every item's text is wrapped once per pass, and the font cache
     * counts every one of those. `nowrap` is the initial value, so this is the
     * common path and it runs once.
     */
    for (pass = (wrap == AR_WRAP_NOWRAP && assign) ? 1 : 0; pass < 2; ++pass)
    {
        ar_i32 cross_cursor;

        if (pass == 1)
        {
            ar_i32 free_cross = inner_cross - cross_total;

            if (!assign)
            {
                break;
            }
            if (wrap == AR_WRAP_NOWRAP || line_count <= 0)
            {
                line_lead = 0;
                line_between = 0;
            }
            else
            {
                ar_align_distribute(n->style.v[AR_P_ALIGN_CONTENT], free_cross, line_count,
                                    &line_lead, &line_between);
            }
        }

        cross_cursor = *ar_axis_pos(&n->rect, cross) + ar_axis_pad_lead(&n->style, cross) +
                       (pass == 1 ? line_lead : 0);
        cross_total = 0;
        line_count = 0;

        first = ar__flex_next(nodes, i, -1, ordered);
        while (first >= 0)
        {
            ar_i32 stop = first;
            ar_i32 count = 0;
            ar_i32 run = 0;
            ar_i32 line_cross = 0;
            ar_i32 main_used = 0;
            ar_i32 lead, between, cursor;

            /*
             * §9.3: collect a line.
             *
             * Nowrap takes everything. Wrapping takes items until the next one
             * would not fit at its *hypothetical* size -- before any growing
             * or shrinking, which is what makes the break points stable rather
             * than depending on what the line has already decided.
             */
            while (stop >= 0)
            {
                ar_node *it = &nodes[stop];
                ar_i32   base = ar__flex_base(it, axis, inner_main);
                ar_i32   hypo = ar_clamp(base, ar__auto_min(it, axis),
                                         AR_WIDE(&it->style, ar_axis_max_prop(axis)));
                ar_i32   outer = hypo + ar_axis_margin_lead(&it->style, axis) +
                                 ar_axis_margin_trail(&it->style, axis);

                if (wrap != AR_WRAP_NOWRAP && count > 0 && run + gap + outer > inner_main)
                {
                    break;
                }
                run += (count > 0 ? gap : 0) + outer;
                ++count;
                stop = ar__flex_next(nodes, i, stop, ordered);
            }

            ar__resolve_line(nodes, i, first, stop, axis, inner_main, gap, count, ordered);

            /*
             * The cross size of every item, at the main size it just got --
             * which is the first moment its text can be wrapped, and the whole
             * reason this cannot be done before the resolution loop.
             */
            main_used = count > 1 ? gap * (count - 1) : 0;
            for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, i, c, ordered))
            {
                ar_node *it = &nodes[c];
                ar_i32   self = it->style.v[AR_P_ALIGN_SELF];
                int      stretch;
                ar_i32   outer;

                if (self == AR_ALIGN_AUTO)
                {
                    self = align_items;
                }
                /*
                 * A single line stretches to the container, and a wrapped line
                 * stretches to the line -- which is as tall as its tallest
                 * item, so it cannot be known yet.
                 *
                 * The difference is not only tidiness. A stretched item's cross
                 * size is the container's, so it never needs its text measured
                 * to find out; deciding that here rather than afterwards is one
                 * whole wrap per item per frame, and the font cache counts
                 * every one. `nowrap` is the initial value, so this is the
                 * common path.
                 */
                stretch = (self == AR_ALIGN_STRETCH && wrap == AR_WRAP_NOWRAP);
                *ar_axis_size(&it->rect, cross) = ar_resolve_size(it, cross, inner_cross, stretch);
                ar_wrap_height(nodes, it, axis, stretch, env);

                outer = *ar_axis_size(&it->rect, cross) + ar_axis_margin_lead(&it->style, cross) +
                        ar_axis_margin_trail(&it->style, cross);
                if (outer > line_cross)
                {
                    line_cross = outer;
                }
                main_used += *ar_axis_size(&it->rect, axis) +
                             ar_axis_margin_lead(&it->style, axis) +
                             ar_axis_margin_trail(&it->style, axis);
            }

            /* A single unwrapped line is as tall as the container gives it,
               so `align-items: stretch` has something to stretch to. */
            if (wrap == AR_WRAP_NOWRAP && inner_cross > line_cross)
            {
                line_cross = inner_cross;
            }

            if (pass == 1)
            {
                ar_i32 index = 0;

                ar_align_distribute(ar_align_from_justify(n->style.v[AR_P_JUSTIFY]),
                                    inner_main - main_used, count, &lead, &between);
                cursor = *ar_axis_pos(&n->rect, axis) + ar_axis_pad_lead(&n->style, axis) + lead;

                for (c = first; c >= 0 && c != stop; c = ar__flex_next(nodes, i, c, ordered))
                {
                    ar_node *it = &nodes[c];
                    ar_i32   self = it->style.v[AR_P_ALIGN_SELF];
                    ar_i32   free_cross;

                    if (self == AR_ALIGN_AUTO)
                    {
                        self = align_items;
                    }

                    cursor += ar_axis_margin_lead(&it->style, axis);
                    *ar_axis_pos(&it->rect, axis) = cursor;
                    cursor += *ar_axis_size(&it->rect, axis) +
                              ar_axis_margin_trail(&it->style, axis) + gap + between;

                    /* Stretch is a size, so it happens before the offset is
                       asked for, and only for an item that stated no cross
                       size of its own. */
                    if (self == AR_ALIGN_STRETCH && wrap != AR_WRAP_NOWRAP &&
                        it->style.unit[ar_axis_size_prop(cross)] == AR_UNIT_AUTO)
                    {
                        ar_i32 room = line_cross - ar_axis_margin_lead(&it->style, cross) -
                                      ar_axis_margin_trail(&it->style, cross);

                        *ar_axis_size(&it->rect, cross) =
                            ar_clamp(room, it->style.v[ar_axis_min_prop(cross)],
                                     AR_WIDE(&it->style, ar_axis_max_prop(cross)));
                        ar_wrap_height(nodes, it, axis, 1, env);
                    }

                    free_cross = line_cross - (*ar_axis_size(&it->rect, cross) +
                                               ar_axis_margin_lead(&it->style, cross) +
                                               ar_axis_margin_trail(&it->style, cross));

                    *ar_axis_pos(&it->rect, cross) = cross_cursor +
                                                     ar_align_self_offset(self, free_cross) +
                                                     ar_axis_margin_lead(&it->style, cross);
                    ++index;
                }
                (void)index;
            }

            cross_cursor += line_cross + (pass == 1 ? line_between : 0);
            cross_total += line_cross;
            ++line_count;

            if (line_count > 1)
            {
                cross_total += 0; /* no gap between lines: `gap` is main-axis
                                     only until row-gap exists, at 0.8.1 */
            }
            first = stop;
        }
    }

    return cross_total;
}

/*
 * `wrap-reverse` stacks the lines from the far edge.
 *
 * Applied afterwards by mirroring every item's cross position inside the
 * container, rather than by running the solve backwards: the items inside a
 * line keep their order and only the lines reverse, and a mirror is the one
 * operation that does exactly that without the solve having to know.
 */
/* This box and everything under it, through the child links -- a scan of the
   node array per moved box is the quadratic the table work already found. */
static void ar__flex_shift(ar_node *nodes, ar_i32 i, ar_i32 axis, ar_i32 d)
{
    ar_i32 c;

    if (d == 0)
    {
        return;
    }
    *ar_axis_pos(&nodes[i].rect, axis) += d;
    for (c = nodes[i].first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar__flex_shift(nodes, c, axis, d);
    }
}

static void ar__flex_reverse(ar_node *nodes, ar_i32 i)
{
    ar_node *n = &nodes[i];
    ar_i32   cross = ar_axis_main(n) ^ 1;
    ar_i32   lo = *ar_axis_pos(&n->rect, cross) + ar_axis_pad_lead(&n->style, cross);
    ar_i32   hi = *ar_axis_pos(&n->rect, cross) + *ar_axis_size(&n->rect, cross) -
                  ar_axis_pad_trail(&n->style, cross);
    ar_i32   c;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_node *it = &nodes[c];
        ar_i32   top, size, moved;

        if (!ar__flex_item(it))
        {
            continue;
        }
        top = *ar_axis_pos(&it->rect, cross);
        size = *ar_axis_size(&it->rect, cross);
        moved = lo + hi - top - size;
        ar__flex_shift(nodes, c, cross, moved - top);
    }
}

void ar_flex_place(ar_node *nodes, ar_i32 i, ar_layout_env *env)
{
    ar_node *n = &nodes[i];
    ar_i32   cross = ar_axis_main(n) ^ 1;
    ar_i32   contents;

    contents = ar__flex_solve(nodes, i, env, 1);

    /* What the contents came to, whatever the box itself ended up being --
       the same number a block container records, and the same reason: it is
       exactly how far a scroll container can be scrolled. */
    if (cross == 1)
    {
        n->content_h = contents + n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    }

    if (n->style.v[AR_P_FLEX_WRAP] == AR_WRAP_WRAP_REVERSE)
    {
        ar__flex_reverse(nodes, i);
    }
}

/* The cross size of the container, for an automatic one. Runs before the
   placement pass, so it solves without assigning. */
ar_i32 ar_flex_content_cross(ar_node *nodes, ar_i32 i, ar_layout_env *env)
{
    return ar__flex_solve(nodes, i, env, 0);
}
