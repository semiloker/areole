/*
 * areole - scroll containers
 * SPDX-License-Identifier: MIT
 *
 * A list longer than its box. Until now it could not be reached: the content
 * was laid out, clipped by `overflow: hidden`, and that was the end of it.
 *
 * ------------------------------------------------------------------------
 * What a scroll container is
 *
 * A box whose overflow is `scroll` or `auto`, which therefore has three
 * numbers instead of one: the height of its viewport, the height of its
 * content, and how far down the content it currently is. The first comes from
 * layout, the second from the same stack that produced it, and the third
 * survives between frames in the per-box slot table -- because a scroll
 * position is state, and it is the only piece of layout state areole keeps.
 *
 * Scrolling is then subtraction: the subtree is shifted up by the offset and
 * clipped to the container, which is the same mechanism `position: relative`
 * uses and for the same reason.
 *
 * ------------------------------------------------------------------------
 * One frame behind
 *
 * The wheel is applied after layout, so a notch turned during frame N is
 * visible in frame N+1. It has to be: the box under the cursor is not known
 * until the frame is laid out, and laying out twice to find out costs more
 * than the frame it saves.
 *
 * This is the same trade hover already makes, and ar_needs_redraw already
 * exists to close it -- a caller whose pump blocks when idle asks for one more
 * frame and the delay is invisible.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

/*
 * The used value of each axis, which is not always the one that was written.
 *
 * CSS says a `visible` paired with anything else on the other axis computes to
 * `auto`, and it has to: `visible` means content escapes the box, and content
 * cannot escape sideways out of something that clips vertically -- there is
 * nowhere for it to go. So `overflow-x: visible; overflow-y: hidden` is a
 * horizontal scroller, which is exactly the declaration people write when they
 * mean one and is the rule a naive implementation drops.
 *
 * Applied on the way out rather than folded into the resolved style, so the
 * cascade keeps saying what the author wrote and only the reader sees the
 * coercion.
 */
static ar_i32 ar__used_overflow(ar_i32 mine, ar_i32 other)
{
    if (mine == AR_OVERFLOW_VISIBLE && other != AR_OVERFLOW_VISIBLE)
    {
        return AR_OVERFLOW_AUTO;
    }
    return mine;
}

ar_i32 ar_overflow_x(const ar_node *n)
{
    return ar__used_overflow(n->style.v[AR_P_OVERFLOW_X], n->style.v[AR_P_OVERFLOW]);
}

ar_i32 ar_overflow_y(const ar_node *n)
{
    return ar__used_overflow(n->style.v[AR_P_OVERFLOW], n->style.v[AR_P_OVERFLOW_X]);
}

/*
 * Does this box confine its children?
 *
 * One axis is enough to ask, because the coercion above leaves them agreeing
 * on whether they are visible at all: a lone "visible" has already become
 * "auto". So there is no such thing here as a box that clips sideways and not
 * vertically, and a clip stays a plain rectangle.
 */
int ar_clips(const ar_node *n)
{
    return ar_overflow_y(n) != AR_OVERFLOW_VISIBLE;
}

static int ar__scrollable(ar_i32 used)
{
    return used == AR_OVERFLOW_SCROLL || used == AR_OVERFLOW_AUTO;
}

int ar_is_scroll_container(const ar_node *n)
{
    return ar__scrollable(ar_overflow_y(n)) || ar__scrollable(ar_overflow_x(n));
}

int ar_scrolls_y(const ar_node *n)
{
    return ar__scrollable(ar_overflow_y(n));
}

int ar_scrolls_x(const ar_node *n)
{
    return ar__scrollable(ar_overflow_x(n));
}

/* How far this container can be scrolled: nothing, if it all fits. */
ar_i32 ar_scroll_range(const ar_node *n)
{
    ar_i32 inner = n->rect.h - n->style.v[AR_P_PAD_TOP] - n->style.v[AR_P_PAD_BOTTOM];
    ar_i32 over = n->content_h - inner;

    return over > 0 ? over : 0;
}

/* The same question on the inline axis. */
ar_i32 ar_scroll_range_x(const ar_node *n)
{
    ar_i32 inner = n->rect.w - n->style.v[AR_P_PAD_LEFT] - n->style.v[AR_P_PAD_RIGHT];
    ar_i32 over = n->content_w - inner;

    return over > 0 ? over : 0;
}

ar_i32 ar_scroll_clamp(const ar_node *n, ar_i32 want)
{
    ar_i32 max = ar_scroll_range(n);

    /*
     * Also to what a stored position can hold.
     *
     * Without this an AR_SCROLL_COMPACT build writes a range past 32,767 into
     * sixteen bits and it comes back negative -- the list would jump to the top
     * on the way to the bottom. The limit is a visible stop instead: content
     * past it still lays out and paints, it just cannot be scrolled to. See
     * AR_SCROLL_COMPACT in areole.h for what that costs and buys.
     */
    if (max > AR_SCROLL_LIMIT)
    {
        max = AR_SCROLL_LIMIT;
    }
    if (want < 0)
    {
        return 0;
    }
    return want > max ? max : want;
}

/*
 * Whether a scrollbar should be drawn at all.
 *
 * `scroll` always shows one and `auto` shows it only when there is somewhere
 * to go, which is the whole difference between the two keywords and the reason
 * anybody writes `auto`.
 */
ar_i32 ar_scroll_bar_width(const ar_node *n)
{
    switch (n->style.v[AR_P_SCROLLBAR_WIDTH])
    {
    case AR_SCROLLBAR_HIDDEN:
        return 0;
    case AR_SCROLLBAR_THIN:
        return AR_SCROLLBAR_W_THIN;
    default:
        return AR_SCROLLBAR_W;
    }
}

/*
 * What the gutter reserves at the inline end.
 *
 * areole's bar is an overlay, so the layout shift `stable` was invented to
 * prevent cannot happen here whatever this returns. What it does buy is the
 * overlap: an overlay bar is drawn on top of whatever is beside it, and
 * `stable` stops the text running underneath.
 *
 * It reserves the width whether or not a bar is currently showing -- that is
 * the whole point of the word, and reserving it only when visible would
 * reintroduce exactly the shift being avoided.
 */
ar_i32 ar_scroll_gutter(const ar_node *n)
{
    if (!ar_is_scroll_container(n))
    {
        return 0;
    }
    if (n->style.v[AR_P_SCROLLBAR_GUTTER] == AR_GUTTER_AUTO)
    {
        return 0;
    }
    return ar_scroll_bar_width(n);
}

int ar_scroll_bar_visible(const ar_node *n)
{
    if (!ar_is_scroll_container(n))
    {
        return 0;
    }

    /* `scrollbar-width: none` hides the bar and does not stop the wheel. A
       list nobody can reach is the failure this property invites, so the test
       is here rather than in ar__apply_wheel. */
    if (ar_scroll_bar_width(n) == 0)
    {
        return 0;
    }
    if (ar_overflow_y(n) == AR_OVERFLOW_SCROLL)
    {
        return 1;
    }
    return ar_scroll_range(n) > 0;
}

/* ------------------------------------------------------------------------
 * Snapping
 *
 * Where a scroll settles, given where it was going.
 *
 * The container declares that it snaps and on which axis; each descendant
 * declares where it wants to sit when it is the one snapped to. That split is
 * what makes a carousel expressible -- the track says "snap", the slides say
 * "at my start".
 *
 * Everything here is in screen coordinates, because by the time this runs the
 * layout pass has already shifted every descendant by the container's current
 * offset. So a candidate is not an absolute position but a distance from where
 * we are: to put a child's top edge at the top of the scrollport, the scroll
 * has to change by exactly the gap between them.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__snap_axis(const ar_node *n)
{
    return n->style.v[AR_P_SCROLL_SNAP_TYPE] & AR_SNAP_AXIS_MASK;
}

int ar_scroll_snaps_y(const ar_node *n)
{
    ar_i32 axis = ar__snap_axis(n);

    return axis == AR_SNAP_AXIS_Y || axis == AR_SNAP_AXIS_BOTH;
}

static ar_i32 ar__abs(ar_i32 v)
{
    return v < 0 ? -v : v;
}

/*
 * Is `at` inside the subtree rooted at `root`?
 *
 * Walks up rather than down, which is the same shape ar_scroll_shift uses and
 * for the same reason: the tree is a flat array of parent indices and there is
 * no child list to walk.
 */
static int ar__within(const ar_node *nodes, ar_i32 at, ar_i32 root)
{
    while (at >= 0)
    {
        at = nodes[at].parent;
        if (at == root)
        {
            return 1;
        }
    }
    return 0;
}

ar_i32 ar_scroll_snap(const ar_node *nodes, ar_i32 count, ar_i32 container, ar_i32 cur, ar_i32 want)
{
    const ar_node *c = &nodes[container];
    ar_i32         port_top, port_bottom, port_h;
    ar_i32         best = want, best_d = 0, found = 0;
    ar_i32         stop_at = 0, stop_d = 0, stopped = 0;
    ar_i32         moved = want - cur;
    ar_i32         j;

    if (!ar_scroll_snaps_y(c))
    {
        return want;
    }

    /* scroll-padding shrinks the scrollport. It is what stops a sticky header
       covering the thing that was just snapped to. */
    port_top = c->rect.y + c->style.v[AR_P_SCROLL_PAD_TOP];
    port_bottom = c->rect.y + c->rect.h - c->style.v[AR_P_SCROLL_PAD_BOTTOM];
    port_h = port_bottom - port_top;
    if (port_h <= 0)
    {
        return want;
    }

    for (j = container + 1; j < count; ++j)
    {
        const ar_node *ch = &nodes[j];
        ar_i32         align = ch->style.v[AR_P_SCROLL_SNAP_ALIGN];
        ar_i32         top, bottom, delta, cand, d;

        if (align == AR_SNAP_ALIGN_NONE || ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (!ar__within(nodes, j, container))
        {
            continue;
        }

        /* scroll-margin grows the target: bring this much of my surroundings
           along with me. */
        top = ch->rect.y - ch->style.v[AR_P_SCROLL_MARGIN_TOP];
        bottom = ch->rect.y + ch->rect.h + ch->style.v[AR_P_SCROLL_MARGIN_BOTTOM];

        if (align == AR_SNAP_ALIGN_START)
        {
            delta = top - port_top;
        }
        else if (align == AR_SNAP_ALIGN_END)
        {
            delta = bottom - port_bottom;
        }
        else
        {
            delta = (top + bottom) / 2 - (port_top + port_bottom) / 2;
        }

        cand = ar_scroll_clamp(c, cur + delta);
        d = ar__abs(cand - want);

        if (!found || d < best_d)
        {
            best = cand;
            best_d = d;
            found = 1;
        }

        /*
         * scroll-snap-stop: always.
         *
         * A gesture may not travel past this box without stopping on it, so of
         * everything lying between where we are and where we were going, the
         * nearest one wins outright. Without this a hard flick skips three
         * slides, which is the whole difference between a good carousel and an
         * infuriating one.
         */
        if (ch->style.v[AR_P_SCROLL_SNAP_STOP] == AR_SNAP_STOP_ALWAYS && moved != 0)
        {
            ar_i32 travel = cand - cur;

            if (travel != 0 && ((moved > 0 && travel > 0 && travel <= moved) ||
                                (moved < 0 && travel < 0 && travel >= moved)))
            {
                ar_i32 td = ar__abs(travel);

                if (!stopped || td < stop_d)
                {
                    stop_at = cand;
                    stop_d = td;
                    stopped = 1;
                }
            }
        }
    }

    if (stopped)
    {
        return stop_at;
    }
    if (!found)
    {
        return want;
    }

    /*
     * `proximity` takes the snap point only when one is close enough to be
     * plausibly what was meant, which is what lets a long list still be
     * scrolled to an arbitrary position. `mandatory` always takes it.
     */
    if ((c->style.v[AR_P_SCROLL_SNAP_TYPE] & AR_SNAP_MANDATORY) == 0 &&
        best_d > port_h * AR_SNAP_PROXIMITY_NUM / AR_SNAP_PROXIMITY_DEN)
    {
        return want;
    }
    return best;
}

/*
 * Shifts every scroll container's contents by its offset.
 *
 * The same subtree walk `position: relative` uses, and for the same reason:
 * the container itself does not move, everything inside it does, and the clip
 * the container already had is what stops the overflow from being seen.
 *
 * The offset comes from the caller, because the scroll position lives in the
 * per-box slot table and this file has no business knowing that table exists.
 */
ar_i32 ar_scroll_clamp_x(const ar_node *n, ar_i32 want)
{
    ar_i32 max = ar_scroll_range_x(n);

    if (max > AR_SCROLL_LIMIT)
    {
        max = AR_SCROLL_LIMIT;
    }
    if (want < 0)
    {
        return 0;
    }
    return want > max ? max : want;
}

void ar_scroll_apply(ar_node *nodes, ar_i32 count, ar_layout_env *env)
{
    ar_i32 i;

    if (!env->scroll_of)
    {
        return;
    }
    for (i = 0; i < count; ++i)
    {
        ar_node *n = &nodes[i];
        ar_i32   dy, dx;
        ar_i32   j;

        if (!ar_is_scroll_container(n) || n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        dy = ar_scrolls_y(n) ? ar_scroll_clamp(n, env->scroll_of(env->ud, i)) : 0;
        dx = (ar_scrolls_x(n) && env->scroll_x_of)
                 ? ar_scroll_clamp_x(n, env->scroll_x_of(env->ud, i))
                 : 0;
        if (dy == 0 && dx == 0)
        {
            continue;
        }
        for (j = i + 1; j < count; ++j)
        {
            ar_i32 at = nodes[j].parent;

            while (at >= 0 && at != i)
            {
                at = nodes[at].parent;
            }
            if (at == i)
            {
                nodes[j].rect.y -= dy;
                nodes[j].rect.x -= dx;
            }
        }
    }
}

/*
 * The scrollbar's track and thumb.
 *
 * An overlay bar, drawn inside the container's right edge rather than taken
 * out of its width. On a machine where a relayout costs milliseconds, a
 * scrollbar that appears and reflows the text beside it is a scrollbar that
 * makes the interface jump, and the overlay is both cheaper and calmer.
 */
void ar_scroll_bar(const ar_node *n, ar_i32 scroll, ar_rect *track, ar_rect *thumb)
{
    ar_i32 range = ar_scroll_range(n);
    ar_i32 inner = n->rect.h - n->style.v[AR_P_PAD_TOP] - n->style.v[AR_P_PAD_BOTTOM];
    ar_i32 bw = ar_scroll_bar_width(n);
    ar_i32 len, pos;

    *track = ar_rect_make(n->rect.x + n->rect.w - bw, n->rect.y, bw, n->rect.h);

    if (range <= 0 || n->content_h <= 0 || inner <= 0)
    {
        *thumb = *track;
        return;
    }

    /* As long as the viewport is of the content, and never so short that it
       cannot be seen or grabbed. */
    len = n->rect.h * inner / n->content_h;
    if (len < AR_SCROLLBAR_MIN)
    {
        len = AR_SCROLLBAR_MIN;
    }
    if (len > n->rect.h)
    {
        len = n->rect.h;
    }
    pos = (n->rect.h - len) * scroll / range;

    *thumb = ar_rect_make(track->x, n->rect.y + pos, bw, len);
}
