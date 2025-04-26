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

int ar_is_scroll_container(const ar_node *n)
{
    return n->style.v[AR_P_OVERFLOW] == AR_OVERFLOW_SCROLL ||
           n->style.v[AR_P_OVERFLOW] == AR_OVERFLOW_AUTO;
}

/* How far this container can be scrolled: nothing, if it all fits. */
ar_i32 ar_scroll_range(const ar_node *n)
{
    ar_i32 inner = n->rect.h - n->style.v[AR_P_PAD_TOP] - n->style.v[AR_P_PAD_BOTTOM];
    ar_i32 over = n->content_h - inner;

    return over > 0 ? over : 0;
}

ar_i32 ar_scroll_clamp(const ar_node *n, ar_i32 want)
{
    ar_i32 max = ar_scroll_range(n);

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
int ar_scroll_bar_visible(const ar_node *n)
{
    if (!ar_is_scroll_container(n))
    {
        return 0;
    }
    if (n->style.v[AR_P_OVERFLOW] == AR_OVERFLOW_SCROLL)
    {
        return 1;
    }
    return ar_scroll_range(n) > 0;
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
        ar_i32   dy;
        ar_i32   j;

        if (!ar_is_scroll_container(n) || n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        dy = ar_scroll_clamp(n, env->scroll_of(env->ud, i));
        if (dy == 0)
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
    ar_i32 len, pos;

    *track =
        ar_rect_make(n->rect.x + n->rect.w - AR_SCROLLBAR_W, n->rect.y, AR_SCROLLBAR_W, n->rect.h);

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

    *thumb = ar_rect_make(track->x, n->rect.y + pos, AR_SCROLLBAR_W, len);
}
