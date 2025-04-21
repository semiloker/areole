/*
 * areole - block formatting
 * SPDX-License-Identifier: MIT
 *
 * Block-level boxes stacked down the page, each as wide as its containing
 * block lets it be and as tall as its contents need. It is the oldest layout
 * model in CSS and the one every document is written in.
 *
 * Until now `display: block` parsed and then behaved as a flex row, which is
 * to say it was a lie. This file makes it true.
 *
 * ------------------------------------------------------------------------
 * Margin collapsing
 *
 * The part everyone gets wrong, so it is worth stating the model plainly
 * rather than discovering it from the code.
 *
 * Two vertical margins that meet do not add; they collapse into one, whose
 * size is the larger of the positive parts plus the smaller of the negative
 * parts. Three places they meet:
 *
 *   - between adjacent siblings, bottom against top;
 *   - between a parent and its first child, both top edges, when nothing
 *     separates them -- no top border, no top padding, and the parent does not
 *     establish a new formatting context. The child's margin then ends up
 *     *outside* the parent, which is the behaviour that surprises people;
 *   - between a parent and its last child, both bottom edges, on the same
 *     conditions plus the parent's height being automatic.
 *
 * And a box with nothing in it -- no height, no padding, no border, no content
 * -- collapses its own top and bottom margins through itself.
 *
 * The consequence that makes this tractable: a box's *effective* top margin is
 * its own, collapsed with the effective top margin of its first child, and so
 * on down. So each box carries two numbers, computed bottom-up, and the
 * placement pass only ever collapses between siblings. See ar_block_margins.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

/* The collapse of two margins. Positive parts take the larger, negative parts
   take the smaller, and the answer is their sum -- so 20 against -8 is 12, and
   -20 against -8 is -20. */
ar_i32 ar_margin_collapse(ar_i32 a, ar_i32 b)
{
    ar_i32 pos_a = a > 0 ? a : 0;
    ar_i32 pos_b = b > 0 ? b : 0;
    ar_i32 neg_a = a < 0 ? a : 0;
    ar_i32 neg_b = b < 0 ? b : 0;
    ar_i32 pos = pos_a > pos_b ? pos_a : pos_b;
    ar_i32 neg = neg_a < neg_b ? neg_a : neg_b;

    return pos + neg;
}

int ar_is_block(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_BLOCK;
}

/*
 * Does this box establish a new block formatting context?
 *
 * Margins do not collapse across the boundary of one, and a float does not
 * escape it. The conditions here are the subset of CSS 2.1's list that areole
 * can currently be in: anything that is not a plain block, and any box whose
 * overflow is not visible. The root counts too, and its caller says so.
 */
int ar_establishes_bfc(const ar_node *n)
{
    if (n->parent < 0)
    {
        /* The root. There is nowhere above it for a margin to escape to, so a
           margin that escaped would simply be lost -- and the initial
           containing block is a formatting context in CSS for the same
           reason. */
        return 1;
    }
    if (!ar_is_block(n))
    {
        return 1; /* a flex container is one, and so is anything else */
    }
    return n->style.v[AR_P_OVERFLOW] != AR_OVERFLOW_VISIBLE;
}

/* Nothing between the box's top edge and its first child's. */
static int ar__open_at_top(const ar_node *n)
{
    return !ar_establishes_bfc(n) && n->style.v[AR_P_PAD_TOP] == 0 &&
           n->style.v[AR_P_BORDER_WIDTH] == 0;
}

/*
 * Nothing between the box's last child's bottom edge and its own.
 *
 * The automatic height matters: a box told how tall to be has a bottom edge of
 * its own that the child's margin cannot reach through.
 */
int ar_block_open_at_bottom(const ar_node *n)
{
    return !ar_establishes_bfc(n) && n->style.v[AR_P_PAD_BOTTOM] == 0 &&
           n->style.v[AR_P_BORDER_WIDTH] == 0 && n->style.unit[AR_P_HEIGHT] == AR_UNIT_AUTO;
}

/*
 * A box with nothing to separate: its top and bottom margins meet each other.
 *
 * "Nothing" is exact here -- no height, no padding, no border, no in-flow
 * children and no text. Such a box contributes one collapsed margin to the
 * flow rather than two margins and a zero-height gap between them.
 */
static int ar__self_collapsing(const ar_node *n, const ar_node *nodes)
{
    ar_i32 c;

    if (n->text && n->text[0])
    {
        return 0;
    }
    if (n->style.unit[AR_P_HEIGHT] != AR_UNIT_AUTO || n->style.v[AR_P_PAD_TOP] != 0 ||
        n->style.v[AR_P_PAD_BOTTOM] != 0 || n->style.v[AR_P_BORDER_WIDTH] != 0)
    {
        return 0;
    }
    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        if (nodes[c].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE && !ar_is_floated(&nodes[c]))
        {
            return 0;
        }
    }
    return 1;
}

/* The first and last children that are in flow at all. */
static ar_i32 ar__first_in_flow(const ar_node *n, const ar_node *nodes)
{
    ar_i32 c;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        if (nodes[c].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE && !ar_is_floated(&nodes[c]))
        {
            return c;
        }
    }
    return -1;
}

static ar_i32 ar__last_in_flow(const ar_node *n, const ar_node *nodes)
{
    ar_i32 c;
    ar_i32 last = -1;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        if (nodes[c].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE && !ar_is_floated(&nodes[c]))
        {
            last = c;
        }
    }
    return last;
}

/*
 * The two collapsed margins a box presents to its siblings.
 *
 * Called bottom-up, so a child's numbers are already settled when its parent
 * asks for them. A box that is not a block, or that establishes a formatting
 * context, presents its own margins unchanged -- which is the whole meaning of
 * "margins do not collapse across a formatting context boundary".
 */
int ar_is_floated(const ar_node *n)
{
    return n->style.v[AR_P_FLOAT] != AR_FLOAT_NONE && n->style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE;
}

void ar_block_margins(ar_node *n, const ar_node *nodes)
{
    ar_i32 mt = n->style.v[AR_P_MARGIN_TOP];
    ar_i32 mb = n->style.v[AR_P_MARGIN_BOTTOM];

    n->mt = mt;
    n->mb = mb;

    /* A float is out of flow. Its margins meet nothing, so they collapse with
       nothing -- and it contributes neither to the stack nor to the pending
       margin between the boxes on either side of it. */
    if (ar_is_floated(n))
    {
        return;
    }

    if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
    {
        n->mt = 0;
        n->mb = 0;
        return;
    }
    if (!ar_is_block(n))
    {
        return;
    }

    if (ar__open_at_top(n))
    {
        ar_i32 first = ar__first_in_flow(n, nodes);

        if (first >= 0 && ar_is_block(&nodes[first]))
        {
            mt = ar_margin_collapse(mt, nodes[first].mt);
        }
    }
    if (ar_block_open_at_bottom(n))
    {
        ar_i32 last = ar__last_in_flow(n, nodes);

        if (last >= 0 && ar_is_block(&nodes[last]))
        {
            mb = ar_margin_collapse(mb, nodes[last].mb);
        }
    }

    /* Through itself, last, so that a box which is empty *and* open at both
       ends folds everything it inherited from its children into one number. */
    if (ar__self_collapsing(n, nodes))
    {
        ar_i32 both = ar_margin_collapse(mt, mb);

        mt = both;
        mb = both;
    }

    n->mt = mt;
    n->mb = mb;
}

/* The gap above the first child: nothing, if its margin escaped through the
   parent's top edge and was already counted outside. */
ar_i32 ar_block_top_gap(const ar_node *n, const ar_node *first)
{
    return (ar__open_at_top(n) && ar_is_block(first)) ? 0 : first->mt;
}

/*
 * The stack.
 *
 * One walk, used by both passes: the measure pass asks with intrinsic heights
 * and ignores the positions, the placement pass asks with real heights and
 * keeps them. Sharing the walk is not tidiness -- it is the only way the
 * height a parent reserves and the positions its children get cannot come from
 * two different readings of the same rules.
 *
 * The model is a *pending margin* rather than a gap between each pair. A
 * self-collapsing box has no height and one margin, and that margin has to
 * merge with what is on both sides of it rather than be applied twice with
 * nothing in between -- which is exactly the case a per-pair gap gets wrong.
 */
ar_i32 ar_block_stack(const ar_node *n, ar_node *nodes, ar_block_height_fn height,
                      ar_block_place_fn place, ar_block_run_fn run, ar_block_clear_fn clear_to,
                      void *ud)
{
    ar_i32 cursor = 0;
    ar_i32 pending = 0;
    ar_i32 c;
    int    at_start = 1;

    for (c = n->first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   mt;

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }

        /*
         * A float is placed where the flow has reached and then stepped over.
         * It does not advance the cursor and it does not touch the pending
         * margin: the boxes either side of it are still adjacent to each other.
         */
        if (ar_is_floated(ch))
        {
            if (place)
            {
                place(ud, c, cursor + pending, -1);
            }
            continue;
        }

        /*
         * `clear` moves this box below the floats on the sides it names. The
         * pending margin is spent first: clearance is measured from where the
         * box would otherwise have gone, not from before its own margin.
         */
        if (clear_to && ch->style.v[AR_P_CLEAR] != AR_CLEAR_NONE)
        {
            ar_i32 want = clear_to(ud, cursor + pending, ch->style.v[AR_P_CLEAR]);

            if (want > cursor + pending)
            {
                cursor = want;
                pending = 0;
                at_start = 0;
            }
        }

        /*
         * A run of inline-level siblings stands in for the anonymous block CSS
         * would wrap them in. An anonymous box has no margins, so the pending
         * margin is spent before it and nothing collapses through it.
         */
        if (ar_is_inline_level(ch))
        {
            ar_i32 stop = c;

            while (stop >= 0 && (ar_is_inline_level(&nodes[stop]) ||
                                 nodes[stop].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE))
            {
                stop = nodes[stop].next_sibling;
            }
            cursor += pending;
            pending = 0;
            if (run)
            {
                cursor += run(ud, c, stop, cursor);
            }
            at_start = 0;
            if (stop < 0)
            {
                break;
            }
            /* The loop's own step would move one sibling; this jumps the run.
               Stepping back by one is what makes the two agree. */
            c = nodes[stop].prev_sibling;
            continue;
        }

        /* A first child whose margin escaped through the parent's top edge
           contributes nothing here: it was already counted outside. */
        mt = at_start ? ar_block_top_gap(n, ch) : ch->mt;

        if (ar__self_collapsing(ch, nodes))
        {
            /*
             * No height, and its two margins are already one number, so it
             * joins the pending margin rather than interrupting it.
             *
             * Its own top edge sits below the margin immediately before it,
             * not below the whole collapsed run -- the run carries on past
             * this box and positions whatever comes next. And "the margin
             * immediately before it" means the one the author wrote, not the
             * collapsed pair already stored on the node, which is why the
             * style is read directly here.
             */
            if (place)
            {
                ar_i32 own = ar_margin_collapse(pending, ch->style.v[AR_P_MARGIN_TOP]);

                place(ud, c, cursor + own, 0);
            }
            pending = ar_margin_collapse(pending, mt);
            continue;
        }

        pending = ar_margin_collapse(pending, mt);

        cursor += pending;
        if (place)
        {
            place(ud, c, cursor, 1);
        }
        cursor += height(ud, c);
        pending = ch->mb;
        at_start = 0;
    }

    if (!at_start && !ar_block_open_at_bottom(n))
    {
        cursor += pending;
    }
    return cursor < 0 ? 0 : cursor;
}
