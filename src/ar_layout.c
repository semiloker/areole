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
 * Block formatting
 *
 * A block container stacks its children down the page. Its intrinsic width is
 * the widest child, its intrinsic height the stack -- with the margins
 * collapsed, which ar_layout_block.c works out and this file only asks about.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__fit_height_of(void *ud, ar_i32 index)
{
    const ar_node *nodes = (const ar_node *)ud;

    return ar__intrinsic(&nodes[index], 1);
}

/*
 * How tall a run of inline items wants to be, which is the max-content answer:
 * everything on one line, so as tall as the tallest item plus its margins.
 *
 * It ignores baselines, and it is allowed to: intrinsic sizing asks how big
 * something wants to be, and the placement pass recomputes the real height
 * once it knows the width the run actually got. Working the baselines out
 * twice would cost more than it could ever be right about.
 */
static ar_i32 ar__fit_run(void *ud, ar_i32 first, ar_i32 stop, ar_i32 y)
{
    const ar_node *nodes = (const ar_node *)ud;
    ar_i32         tallest = 0;
    ar_i32         c;

    (void)y;
    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        const ar_node *ch = &nodes[c];
        ar_i32         h;

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        h = ar__intrinsic(ch, 1) + ch->style.v[AR_P_MARGIN_TOP] + ch->style.v[AR_P_MARGIN_BOTTOM];
        if (h > tallest)
        {
            tallest = h;
        }
    }
    return tallest;
}

static void ar__measure_block(ar_node *nodes, ar_i32 i)
{
    ar_node *n = &nodes[i];
    ar_i32   widest = 0;
    ar_i32   c;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   w;

        if (ar__hidden(ch))
        {
            continue;
        }
        w = ar__intrinsic(ch, 0) + ch->style.v[AR_P_MARGIN_LEFT] + ch->style.v[AR_P_MARGIN_RIGHT];
        if (w > widest)
        {
            widest = w;
        }
    }

    if (n->text)
    {
        ar_i32 tw = n->text_w;
        ar_i32 th = ar__text_block_height(n);

        if (tw > widest)
        {
            widest = tw;
        }
        /* Text and children stack rather than overlap, which is not what CSS
           does -- there, bare text beside a block sibling gets an anonymous
           block of its own. This is the same arrangement without the extra
           box, and is where anonymous boxes will go when they arrive. */
        n->fit[1] = th + ar_block_stack(n, nodes, ar__fit_height_of, 0, ar__fit_run, 0, nodes) +
                    n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    }
    else
    {
        n->fit[1] = ar_block_stack(n, nodes, ar__fit_height_of, 0, ar__fit_run, 0, nodes) +
                    n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    }

    n->fit[0] = widest + n->style.v[AR_P_PAD_LEFT] + n->style.v[AR_P_PAD_RIGHT];
}

/*
 * The min-content width a container needs, from its children.
 *
 * A block stacks its children, so it needs the widest of them. A flex row puts
 * them side by side, so it needs their sum; a flex column stacks them, so the
 * widest again. An inline run wraps, so its children can be taken one at a
 * time and the widest wins.
 *
 * A box's own text counts too, since text and children stack here.
 */
static void ar__min_content(ar_node *nodes, ar_i32 i)
{
    ar_node *n = &nodes[i];
    ar_i32   sum = 0;
    ar_i32   widest = n->min_w;
    ar_i32   c;
    int      side_by_side;

    if (ar__hidden(n))
    {
        n->min_w = 0;
        return;
    }

    side_by_side = !ar_is_block(n) && ar__main_axis(n) == 0;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   w;

        if (ar__hidden(ch) || ar_is_floated(ch))
        {
            continue;
        }
        w = ch->min_w + ch->style.v[AR_P_MARGIN_LEFT] + ch->style.v[AR_P_MARGIN_RIGHT];

        /* An inline item wraps to the next line rather than forcing the
           container wider, so a run of them needs only its widest member. */
        if (side_by_side && !ar_is_inline_level(ch))
        {
            sum += w;
        }
        else if (w > widest)
        {
            widest = w;
        }
    }

    if (sum > widest)
    {
        widest = sum;
    }
    n->min_w = widest + n->style.v[AR_P_PAD_LEFT] + n->style.v[AR_P_PAD_RIGHT];

    /*
     * A box told how wide to be is that wide, whatever is in it. Its content
     * may overflow, and overflowing is not the same as being narrower -- so a
     * stated width replaces the answer rather than raising it.
     *
     * A percentage does not, because it is not a width until the containing
     * block has one, and intrinsic sizing runs before that is known.
     */
    if (n->style.unit[AR_P_WIDTH] == AR_UNIT_PX)
    {
        n->min_w = n->style.v[AR_P_WIDTH];
    }
}

/*
 * A stated width, once the three intrinsic keywords are allowed to be one.
 *
 * `available` is what the containing block has left. fit-content is the only
 * one that uses it, and is the only one of the three anybody writes on purpose.
 */
static int ar__intrinsic_width(const ar_node *n, ar_i32 available, ar_i32 *out)
{
    switch (n->style.unit[AR_P_WIDTH])
    {
    case AR_UNIT_MIN_CONTENT:
        *out = n->min_w;
        return 1;
    case AR_UNIT_MAX_CONTENT:
        *out = n->fit[0];
        return 1;
    case AR_UNIT_FIT_CONTENT:
        *out = n->fit[0] < available ? n->fit[0] : available;
        if (*out < n->min_w)
        {
            *out = n->min_w;
        }
        return 1;
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

        /* Bottom-up, so every child's collapsed margins and min-content
           widths are settled before its parent asks for them. */
        ar_block_margins(n, nodes);
        ar__min_content(nodes, i);

        if (ar_is_block(n))
        {
            ar__measure_block(nodes, i);
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

    if (axis == 0 && ar__intrinsic_width(ch, inner, &v))
    {
        return ar__clamp(v, ch->style.v[ar__min_prop(axis)], ch->style.v[ar__max_prop(axis)]);
    }

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
static void ar__wrap_height(ar_node *n, ar_i32 axis, int stretch, ar_layout_env *env)
{
    ar_i32 inner_w;
    ar_i32 h;

    if (!env->wrap || !n->text)
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

    h = env->wrap(env->ud, n, inner_w);
    h += n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    if (h > n->rect.h)
    {
        n->rect.h = ar__clamp(h, n->style.v[AR_P_MIN_HEIGHT], n->style.v[AR_P_MAX_HEIGHT]);
    }
}

/* What the placement walk carries: the node array, and where the content box
   starts, because ar_block_stack works in offsets from zero. */
typedef struct ar__stack_ud
{
    ar_node       *nodes;
    ar_i32         top;
    ar_i32         left;
    ar_i32         inner_w;
    ar_i32         align;
    ar_layout_env *env;
    ar_float_ctx   floats;
} ar__stack_ud;

/* Gives an out-of-flow or inline-level box its size, which for both is
   shrink-to-fit rather than "fill the container". */
static void ar__size_shrink_to_fit(ar_node *ch, ar_i32 inner_w, ar_layout_env *env)
{
    /*
     * A box whose text will be cut into fragments is one line tall, and the
     * fragments carry the rest. Wrapping it as though it were a block would
     * make it as tall as all its lines *and* fragment it, which is every line
     * counted twice -- and the box's own rectangle is the union of its
     * fragments, so it comes out taller still.
     */
    if (ar_is_fragmentable(ch))
    {
        ch->rect.w = ch->fit[0] < inner_w ? ch->fit[0] : inner_w;
        ch->rect.h = ch->text_h;
        return;
    }

    if (!ar__intrinsic_width(ch, inner_w, &ch->rect.w))
    {
        switch (ch->style.unit[AR_P_WIDTH])
        {
        case AR_UNIT_PX:
            ch->rect.w = ch->style.v[AR_P_WIDTH];
            break;
        case AR_UNIT_PCT:
            ch->rect.w = inner_w * ch->style.v[AR_P_WIDTH] / 100;
            break;
        default:
            /* Shrink to fit, which is fit-content by another name -- an
               inline-level or floated box has wanted this all along. */
            ch->rect.w = ch->fit[0] < inner_w ? ch->fit[0] : inner_w;
            if (ch->rect.w < ch->min_w)
            {
                ch->rect.w = ch->min_w;
            }
            break;
        }
    }
    ch->rect.w = ar__clamp(ch->rect.w, ch->style.v[AR_P_MIN_WIDTH], ch->style.v[AR_P_MAX_WIDTH]);
    if (ch->rect.w < 0)
    {
        ch->rect.w = 0;
    }

    if (ch->style.unit[AR_P_HEIGHT] == AR_UNIT_PX)
    {
        ch->rect.h = ch->style.v[AR_P_HEIGHT];
    }
    else
    {
        ch->rect.h = ch->fit[1];
    }
    ch->rect.h = ar__clamp(ch->rect.h, ch->style.v[AR_P_MIN_HEIGHT], ch->style.v[AR_P_MAX_HEIGHT]);

    ar__wrap_height(ch, 1, 0, env);
}

static ar_i32 ar__rect_height_of(void *ud, ar_i32 index)
{
    return ((ar__stack_ud *)ud)->nodes[index].rect.h;
}

/*
 * A run of inline items: give each one a size, then let the line breaker
 * arrange them.
 *
 * An inline-level box shrinks to fit rather than filling its container, which
 * is the other half of what makes it inline-level -- a block child of the same
 * markup would take the whole width and force the next item onto a new line.
 */
static ar_i32 ar__place_run(void *ud, ar_i32 first, ar_i32 stop, ar_i32 y)
{
    ar__stack_ud *su = (ar__stack_ud *)ud;
    ar_node      *nodes = su->nodes;
    ar_i32        c;

    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        ar__size_shrink_to_fit(ch, su->inner_w, su->env);
    }

    return ar_inline_run(nodes, first, stop, su->left, su->top + y, su->inner_w, su->align,
                         &su->floats, su->top + y, su->env);
}

static void ar__place_child_at(void *ud, ar_i32 index, ar_i32 y, int real)
{
    ar__stack_ud *su = (ar__stack_ud *)ud;
    ar_node      *ch = &su->nodes[index];

    /* -1 is a float: sized here, because a float shrinks to fit rather than
       filling the container the way an in-flow block child does, and then
       handed to the float list to be pushed to its side. */
    if (real == -1)
    {
        ar__size_shrink_to_fit(ch, su->inner_w, su->env);
        ar_float_place(&su->floats, ch, su->top + y, ch->style.v[AR_P_FLOAT]);
        return;
    }

    /* -2 is out of flow: it gets the static position -- where the flow had
       reached -- and a shrink-to-fit size, and the pass after the flow
       resolves whichever of those its offsets override. */
    if (real == -2)
    {
        ar__size_shrink_to_fit(ch, su->inner_w, su->env);
        ch->rect.x = su->left;
        ch->rect.y = su->top + y;
        return;
    }

    ch->rect.y = su->top + y;
    if (!real)
    {
        ch->rect.h = 0;
        return;
    }

    /*
     * A box that establishes a formatting context may not overlap a float: it
     * moves aside and, if its width was automatic, narrows to what is left.
     *
     * This is what makes `overflow: hidden` beside a float sit next to it
     * rather than under it, which is the most-used float idiom there is. An
     * ordinary block does the opposite -- it spans the container and lets its
     * lines be narrowed -- and the two being different is the point.
     */
    if (su->floats.count > 0 && ar_establishes_bfc(ch))
    {
        ar_i32 ml = ch->style.v[AR_P_MARGIN_LEFT];
        ar_i32 mr = ch->style.v[AR_P_MARGIN_RIGHT];
        ar_i32 lo, hi;

        ar_float_band(&su->floats, ch->rect.y, ch->rect.h, &lo, &hi);
        ch->rect.x = lo + ml;
        if (ch->style.unit[AR_P_WIDTH] == AR_UNIT_AUTO)
        {
            ar_i32 avail = hi - lo - ml - mr;

            ch->rect.w = avail < 0 ? 0 : avail;
        }
    }
}

static ar_i32 ar__clear_to(void *ud, ar_i32 y, ar_i32 which)
{
    ar__stack_ud *su = (ar__stack_ud *)ud;

    return ar_float_clear_y(&su->floats, su->top + y, which) - su->top;
}

/*
 * Laying a block container's children out.
 *
 * Widths first, all of them, because a block child fills the width it is given
 * and its height follows from that -- including how many lines its text wraps
 * to. Then the stack, with the margins collapsed between siblings.
 */
static void ar__place_block(ar_node *nodes, ar_i32 i, ar_layout_env *env)
{
    ar_node *n = &nodes[i];
    ar_i32   inner_w, inner_h;
    ar_i32   left, top;
    ar_i32   cursor;
    ar_i32   c;

    inner_w = n->rect.w - n->style.v[AR_P_PAD_LEFT] - n->style.v[AR_P_PAD_RIGHT];
    inner_h = n->rect.h - n->style.v[AR_P_PAD_TOP] - n->style.v[AR_P_PAD_BOTTOM];
    if (inner_w < 0)
    {
        inner_w = 0;
    }
    if (inner_h < 0)
    {
        inner_h = 0;
    }
    left = n->rect.x + n->style.v[AR_P_PAD_LEFT];
    top = n->rect.y + n->style.v[AR_P_PAD_TOP];

    /* A block box with text of its own puts it above its children, in the
       space its own measurement reserved for it. */
    if (n->text)
    {
        top += ar__text_block_height(n);
    }

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   ml = ch->style.v[AR_P_MARGIN_LEFT];
        ar_i32   mr = ch->style.v[AR_P_MARGIN_RIGHT];

        if (ar__hidden(ch))
        {
            ch->rect.x = left;
            ch->rect.y = top;
            ch->rect.w = 0;
            ch->rect.h = 0;
            continue;
        }

        /*
         * A block child fills the width it is offered. `grow` means nothing
         * here -- there is no leftover to take a share of, because one box
         * per line already takes it all -- so it is treated as automatic
         * rather than refused.
         */
        if (!ar__intrinsic_width(ch, inner_w - ml - mr, &ch->rect.w))
        {
            switch (ch->style.unit[AR_P_WIDTH])
            {
            case AR_UNIT_PX:
                ch->rect.w = ch->style.v[AR_P_WIDTH];
                break;
            case AR_UNIT_PCT:
                ch->rect.w = inner_w * ch->style.v[AR_P_WIDTH] / 100;
                break;
            default:
                ch->rect.w = inner_w - ml - mr;
                break;
            }
        }
        ch->rect.w =
            ar__clamp(ch->rect.w, ch->style.v[AR_P_MIN_WIDTH], ch->style.v[AR_P_MAX_WIDTH]);
        if (ch->rect.w < 0)
        {
            ch->rect.w = 0;
        }
        ch->rect.x = left + ml;

        switch (ch->style.unit[AR_P_HEIGHT])
        {
        case AR_UNIT_PX:
            ch->rect.h = ch->style.v[AR_P_HEIGHT];
            break;
        case AR_UNIT_PCT:
            ch->rect.h = inner_h * ch->style.v[AR_P_HEIGHT] / 100;
            break;
        default:
            ch->rect.h = ch->fit[1];
            break;
        }
        ch->rect.h =
            ar__clamp(ch->rect.h, ch->style.v[AR_P_MIN_HEIGHT], ch->style.v[AR_P_MAX_HEIGHT]);

        /* The width is settled, so the text can be wrapped into it and the
           height corrected before anything is stacked on top. */
        ar__wrap_height(ch, 1, 0, env);
    }

    /* The stack, by the same walk the measure pass used. */
    {
        ar__stack_ud su;

        su.nodes = nodes;
        su.top = top;
        su.left = left;
        su.inner_w = inner_w;
        su.align = n->style.v[AR_P_TEXT_ALIGN];
        su.env = env;
        ar_float_reset(&su.floats, left, left + inner_w);

        cursor = ar_block_stack(n, nodes, ar__rect_height_of, ar__place_child_at, ar__place_run,
                                ar__clear_to, &su);

        /*
         * A formatting context grows to hold its own floats; a plain block box
         * does not, and a float sticking out of the bottom of one is the
         * behaviour `clearfix` existed to work around for fifteen years.
         */
        if (ar_establishes_bfc(n))
        {
            ar_i32 fb = ar_float_bottom(&su.floats) - top;

            if (fb > cursor)
            {
                cursor = fb;
            }
        }
    }

    /* An automatic height is whatever that came to. It was already measured
       intrinsically, but the children's real widths may have wrapped their
       text differently, so this is the number that counts. */
    /*
     * What the contents came to, whatever the box itself ends up being. For an
     * automatic height the two are the same; for a stated one they differ, and
     * the difference is exactly how far a scroll container can be scrolled.
     */
    n->content_h = cursor + (n->text ? ar__text_block_height(n) : 0) + n->style.v[AR_P_PAD_TOP] +
                   n->style.v[AR_P_PAD_BOTTOM];

    /* The root keeps the viewport, whatever it says about itself and whatever
       its contents come to -- the window is not negotiable, which is already
       true of its width. */
    if (n->style.unit[AR_P_HEIGHT] == AR_UNIT_AUTO && n->parent >= 0)
    {
        ar_i32 used = cursor;

        if (n->text)
        {
            used += ar__text_block_height(n);
        }
        used += n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
        n->rect.h = ar__clamp(used, n->style.v[AR_P_MIN_HEIGHT], n->style.v[AR_P_MAX_HEIGHT]);
    }
}

static void ar__place(ar_node *nodes, ar_i32 count, ar_layout_env *env)
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

        if (ar_is_block(n))
        {
            ar__place_block(nodes, i, env);
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
                ar__wrap_height(ch, axis, stretch, env);
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
                    ar__wrap_height(ch, axis, stretch, env);
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

void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env)
{
    ar_i32 i;

    if (count <= 0)
    {
        return;
    }

    ar__measure(nodes, count);

    /* The root takes the viewport. Anything it says about its own width is
       ignored, because the window is not negotiable. */
    nodes[0].rect = viewport;

    ar__place(nodes, count, env);

    /*
     * Positioning, after the flow has finished.
     *
     * Out of flow first, because a box measured against a positioned ancestor
     * needs that ancestor to have its final rectangle. Then `relative`, which
     * moves a whole subtree -- doing it earlier would move the children twice,
     * once with the parent and once on their own.
     */
    for (i = 0; i < count; ++i)
    {
        if (ar_is_out_of_flow(&nodes[i]) && nodes[i].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE)
        {
            ar_position_out_of_flow(nodes, i, viewport);
        }
    }
    ar_position_relative(nodes, count, viewport);
    ar_scroll_apply(nodes, count, env);
}
