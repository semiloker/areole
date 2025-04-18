/*
 * areole - the layout solver.
 * SPDX-License-Identifier: MIT
 *
 * Two passes over a flat array, no recursion, no allocation, no floating
 * point.
 *
 * True single pass layout is impossible with content dependent sizing. A box
 * that sizes to its children needs the children measured first; a box sized as
 * a percentage of its parent needs the parent measured first. The dependencies
 * point both ways, so the minimum is one sweep up and one sweep down.
 *
 * Both sweeps are plain loops because boxes are appended in declaration order,
 * which means a parent always sits at a lower index than its children. Walking
 * backwards therefore visits every child before its parent, and walking
 * forwards visits every parent before its children. No stack, no traversal
 * bookkeeping, and a memory access pattern a prefetcher can follow.
 */
#include "ar_node.h"

/* Axis 0 is x, axis 1 is y. The main axis of a box is whichever its
   flex-direction names; the cross axis is the other one. Writing the solver
   once against an axis index rather than twice against x and y is what keeps
   it a few hundred lines instead of a thousand, and stops the two copies from
   quietly disagreeing. */
static ar_i32 ar__main_axis(const ar_node *n)
{
    return n->style.v[AR_P_DIRECTION] == AR_DIR_COLUMN ? 1 : 0;
}

static ar_i32 ar__pad_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_TOP] : s->v[AR_P_PAD_LEFT];
}

static ar_i32 ar__pad_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_BOTTOM] : s->v[AR_P_PAD_RIGHT];
}

static ar_i32 ar__margin_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_TOP] : s->v[AR_P_MARGIN_LEFT];
}

static ar_i32 ar__margin_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_BOTTOM] : s->v[AR_P_MARGIN_RIGHT];
}

static ar_prop ar__size_prop(ar_i32 axis)
{
    return axis ? AR_P_HEIGHT : AR_P_WIDTH;
}

static ar_prop ar__min_prop(ar_i32 axis)
{
    return axis ? AR_P_MIN_HEIGHT : AR_P_MIN_WIDTH;
}

static ar_prop ar__max_prop(ar_i32 axis)
{
    return axis ? AR_P_MAX_HEIGHT : AR_P_MAX_WIDTH;
}

static ar_i32 *ar__pos(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->y : &r->x;
}

static ar_i32 *ar__size(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->h : &r->w;
}

static ar_i32 ar__clamp(ar_i32 v, ar_i32 lo, ar_i32 hi)
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

static int ar__hidden(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE;
}

/* Height of a text block, counting the lines rather than assuming one. */
/* The intrinsic height: what the text wants before anything tells it how wide
   to be, which is its explicit lines only. Wrapping is not intrinsic -- it is
   a consequence of a width, and widths are settled in the pass below. */
static ar_i32 ar__text_block_height(const ar_node *n)
{
    ar_i32      lines = 1;
    const char *p;

    for (p = n->text; *p; ++p)
    {
        if (*p == '\n')
        {
            lines++;
        }
    }
    return n->text_h + (lines - 1) * n->line_h;
}

/* What a child contributes to its parent when the parent is sizing itself to
   its content.

   A percentage or a share of the leftover contributes nothing, because both
   are defined in terms of a parent size that is precisely what is being
   computed. Letting them contribute would be circular, so they are simply
   absent from the intrinsic size, which is the same choice every flexbox
   implementation makes. */
static ar_i32 ar__intrinsic(const ar_node *n, ar_i32 axis)
{
    ar_prop p = ar__size_prop(axis);

    switch (n->style.unit[p])
    {
    case AR_UNIT_PX:
        return n->style.v[p] < 0 ? 0 : n->style.v[p];
    case AR_UNIT_AUTO:
        return n->fit[axis];
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------------
 * Pass one, upwards: what each box would like to be
 * ------------------------------------------------------------------------ */
static void ar__measure(ar_node *nodes, ar_i32 count)
{
    ar_i32 i;

    for (i = count - 1; i >= 0; --i)
    {
        ar_node *n = &nodes[i];
        ar_i32   axis, cross;
        ar_i32   main_sum = 0, cross_max = 0, visible = 0;
        ar_i32   c;

        if (ar__hidden(n))
        {
            n->fit[0] = 0;
            n->fit[1] = 0;
            continue;
        }

        axis = ar__main_axis(n);
        cross = axis ^ 1;

        for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
        {
            ar_node *ch = &nodes[c];
            ar_i32   m, x;

            if (ar__hidden(ch))
            {
                continue;
            }
            m = ar__intrinsic(ch, axis) + ar__margin_lead(&ch->style, axis) +
                ar__margin_trail(&ch->style, axis);
            x = ar__intrinsic(ch, cross) + ar__margin_lead(&ch->style, cross) +
                ar__margin_trail(&ch->style, cross);

            main_sum += m;
            if (x > cross_max)
            {
                cross_max = x;
            }
            visible++;
        }

        if (visible > 1)
        {
            main_sum += n->style.v[AR_P_GAP] * (visible - 1);
        }

        if (n->text)
        {
            ar_i32 tw = n->text_w;
            ar_i32 th = ar__text_block_height(n);
            ar_i32 tm = axis ? th : tw;
            ar_i32 tc = axis ? tw : th;

            /* Text sits alongside children rather than replacing them, so a
               box that has both ends up large enough for both. */
            if (tm > main_sum)
            {
                main_sum = tm;
            }
            if (tc > cross_max)
            {
                cross_max = tc;
            }
        }

        n->fit[axis] = main_sum + ar__pad_lead(&n->style, axis) + ar__pad_trail(&n->style, axis);
        n->fit[cross] =
            cross_max + ar__pad_lead(&n->style, cross) + ar__pad_trail(&n->style, cross);
    }
}

/* Resolves one axis of one child against a known parent inner size. */
static ar_i32 ar__resolve_size(const ar_node *ch, ar_i32 axis, ar_i32 inner, int stretch)
{
    ar_prop p = ar__size_prop(axis);
    ar_i32  v;

    switch (ch->style.unit[p])
    {
    case AR_UNIT_PX:
        v = ch->style.v[p];
        break;
    case AR_UNIT_PCT:
        v = inner * ch->style.v[p] / 100;
        break;
    case AR_UNIT_GROW:
        v = inner - ar__margin_lead(&ch->style, axis) - ar__margin_trail(&ch->style, axis);
        break;
    case AR_UNIT_AUTO:
    default:
        /* align-items: stretch only stretches boxes that have not been given
           a size of their own, which is what makes it useful as a default on
           a container rather than an override. */
        v = stretch ? inner - ar__margin_lead(&ch->style, axis) - ar__margin_trail(&ch->style, axis)
                    : ch->fit[axis];
        break;
    }

    return ar__clamp(v, ch->style.v[ar__min_prop(axis)], ch->style.v[ar__max_prop(axis)]);
}

/* ------------------------------------------------------------------------
 * Pass two, downwards: what each box actually gets
 * ------------------------------------------------------------------------ */
/*
 * A text box's height, once the width it has to fit into is known.
 *
 * This is the whole of text wrapping in the solver. It is deliberately not in
 * the measure pass: intrinsic sizing asks "how big does this want to be",
 * whose answer is the widest single line, and only placement knows how much
 * width the box actually got.
 *
 * Applied only where the height came from the content in the first place. A
 * box with a stated height, a percentage, a share of the leftover, or one
 * stretched by align-items has already been told how tall to be, and text that
 * does not fit that is the caller's decision to make.
 */
static void ar__wrap_height(ar_node *n, ar_i32 axis, int stretch, ar_wrap_fn wrap, void *ud)
{
    ar_i32 inner_w;
    ar_i32 h;

    if (!wrap || !n->text)
    {
        return;
    }
    if (n->style.unit[AR_P_HEIGHT] != AR_UNIT_AUTO)
    {
        return;
    }
    /* In a row, height is the cross axis and align-items has already set it. */
    if (axis == 0 && stretch)
    {
        return;
    }

    inner_w = n->rect.w - n->style.v[AR_P_PAD_LEFT] - n->style.v[AR_P_PAD_RIGHT];
    if (inner_w <= 0)
    {
        return;
    }

    h = wrap(ud, n, inner_w);
    h += n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    if (h > n->rect.h)
    {
        n->rect.h = ar__clamp(h, n->style.v[AR_P_MIN_HEIGHT], n->style.v[AR_P_MAX_HEIGHT]);
    }
}

static void ar__place(ar_node *nodes, ar_i32 count, ar_wrap_fn wrap, void *ud)
{
    ar_i32 i;

    for (i = 0; i < count; ++i)
    {
        ar_node *n = &nodes[i];
        ar_i32   axis, cross;
        ar_i32   inner_main, inner_cross;
        ar_i32   used = 0, visible = 0, growers = 0, leftover;
        ar_i32   cursor, extra_gap = 0;
        ar_i32   gap;
        ar_i32   c;
        int      stretch;

        if (ar__hidden(n) || n->first_child < 0)
        {
            continue;
        }

        axis = ar__main_axis(n);
        cross = axis ^ 1;
        gap = n->style.v[AR_P_GAP];
        stretch = n->style.v[AR_P_ALIGN] == AR_ALIGN_STRETCH;

        inner_main = *ar__size(&n->rect, axis) - ar__pad_lead(&n->style, axis) -
                     ar__pad_trail(&n->style, axis);
        inner_cross = *ar__size(&n->rect, cross) - ar__pad_lead(&n->style, cross) -
                      ar__pad_trail(&n->style, cross);
        if (inner_main < 0)
        {
            inner_main = 0;
        }
        if (inner_cross < 0)
        {
            inner_cross = 0;
        }

        /* Sizes first, on both axes, for everything that is not taking a
           share of the leftover. */
        for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
        {
            ar_node *ch = &nodes[c];

            if (ar__hidden(ch))
            {
                ch->rect.x = n->rect.x;
                ch->rect.y = n->rect.y;
                ch->rect.w = 0;
                ch->rect.h = 0;
                continue;
            }

            if (ch->style.unit[ar__size_prop(axis)] == AR_UNIT_GROW)
            {
                growers++;
                *ar__size(&ch->rect, axis) = 0;
            }
            else
            {
                *ar__size(&ch->rect, axis) = ar__resolve_size(ch, axis, inner_main, 0);
            }
            *ar__size(&ch->rect, cross) = ar__resolve_size(ch, cross, inner_cross, stretch);

            /* The width is settled now for every child except one growing
               along a horizontal main axis, whose share is handed out below.
               In a column -- which is where paragraphs live -- width is the
               cross axis and is always settled here, so the height this
               produces is the one that feeds `used`. */
            if (!(axis == 0 && ch->style.unit[AR_P_WIDTH] == AR_UNIT_GROW))
            {
                ar__wrap_height(ch, axis, stretch, wrap, ud);
            }

            used += *ar__size(&ch->rect, axis) + ar__margin_lead(&ch->style, axis) +
                    ar__margin_trail(&ch->style, axis);
            visible++;
        }

        if (visible > 1)
        {
            used += gap * (visible - 1);
        }
        leftover = inner_main - used;

        if (growers > 0)
        {
            ar_i32 share = leftover > 0 ? leftover / growers : 0;
            ar_i32 remainder = leftover > 0 ? leftover % growers : 0;

            for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
            {
                ar_node *ch = &nodes[c];
                ar_i32   v;

                if (ar__hidden(ch) || ch->style.unit[ar__size_prop(axis)] != AR_UNIT_GROW)
                {
                    continue;
                }
                /* The remainder is handed out one pixel at a time to the
                   first boxes rather than dropped. Three columns growing into
                   a hundred pixels come out 34, 33, 33 and meet the far edge
                   exactly; dividing and discarding leaves a two pixel gap that
                   people notice and nobody can explain. */
                v = share;
                if (remainder > 0)
                {
                    v++;
                    remainder--;
                }
                v = ar__clamp(v, ch->style.v[ar__min_prop(axis)], ch->style.v[ar__max_prop(axis)]);
                *ar__size(&ch->rect, axis) = v;

                /* A box growing along a horizontal main axis only learns its
                   width here, so this is the first moment its text can be
                   wrapped. Its height is the cross axis and does not feed the
                   main-axis arithmetic above, so doing it late costs nothing. */
                if (axis == 0)
                {
                    ar__wrap_height(ch, axis, stretch, wrap, ud);
                }
            }
            leftover = 0; /* absorbed, so justify-content has nothing to place */
        }

        if (leftover < 0)
        {
            leftover = 0; /* overflow is clipped, not redistributed */
        }

        cursor = *ar__pos(&n->rect, axis) + ar__pad_lead(&n->style, axis);
        switch (n->style.v[AR_P_JUSTIFY])
        {
        case AR_JUSTIFY_CENTER:
            cursor += leftover / 2;
            break;
        case AR_JUSTIFY_END:
            cursor += leftover;
            break;
        case AR_JUSTIFY_BETWEEN:
            if (visible > 1)
            {
                extra_gap = leftover / (visible - 1);
            }
            break;
        case AR_JUSTIFY_START:
        default:
            break;
        }

        for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
        {
            ar_node *ch = &nodes[c];
            ar_i32   cross_start, cross_free;

            if (ar__hidden(ch))
            {
                continue;
            }

            cursor += ar__margin_lead(&ch->style, axis);
            *ar__pos(&ch->rect, axis) = cursor;
            cursor +=
                *ar__size(&ch->rect, axis) + ar__margin_trail(&ch->style, axis) + gap + extra_gap;

            cross_start = *ar__pos(&n->rect, cross) + ar__pad_lead(&n->style, cross);
            cross_free =
                inner_cross - (*ar__size(&ch->rect, cross) + ar__margin_lead(&ch->style, cross) +
                               ar__margin_trail(&ch->style, cross));
            if (cross_free < 0)
            {
                cross_free = 0;
            }

            switch (n->style.v[AR_P_ALIGN])
            {
            case AR_ALIGN_CENTER:
                *ar__pos(&ch->rect, cross) =
                    cross_start + cross_free / 2 + ar__margin_lead(&ch->style, cross);
                break;
            case AR_ALIGN_END:
                *ar__pos(&ch->rect, cross) =
                    cross_start + cross_free + ar__margin_lead(&ch->style, cross);
                break;
            case AR_ALIGN_START:
            case AR_ALIGN_STRETCH:
            default:
                *ar__pos(&ch->rect, cross) = cross_start + ar__margin_lead(&ch->style, cross);
                break;
            }
        }
    }
}

void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_wrap_fn wrap, void *ud)
{
    if (count <= 0)
    {
        return;
    }

    ar__measure(nodes, count);

    /* The root takes the viewport. Anything it says about its own width is
       ignored, because the window is not negotiable. */
    nodes[0].rect = viewport;

    ar__place(nodes, count, wrap, ud);
}
