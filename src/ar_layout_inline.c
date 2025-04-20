/*
 * areole - inline formatting
 * SPDX-License-Identifier: MIT
 *
 * Line boxes. A run of inline-level siblings is filled left to right until the
 * next one will not fit, and then a new line starts under the last.
 *
 * ------------------------------------------------------------------------
 * Baselines
 *
 * A line box is not "as tall as its tallest item". It is as tall as it needs
 * to be for every item to sit on a shared baseline, which is a different
 * number whenever the items have different amounts above and below theirs:
 *
 *     height = max(ascent over items) + max(descent over items)
 *
 * Two items 20 px tall can make a 30 px line, if one has its baseline 10 px
 * from its top and the other 20 px. That is the whole reason this is not four
 * lines of arithmetic, and it is what makes text of two sizes on one line look
 * like typesetting rather than like boxes.
 *
 * An inline-level box's own baseline is the baseline of the text in it. A box
 * with no text has none, and CSS says to use its bottom margin edge -- which
 * is why an image sits on the baseline rather than through it.
 *
 * ------------------------------------------------------------------------
 * What this is not
 *
 * Items are atomic: an item never splits across two lines. That is
 * `inline-block`, not `inline`, and the difference is fragmentation -- a real
 * inline box puts half of itself on one line and half on the next, with
 * borders and padding on the first and last fragment only. A fragment is a
 * second rectangle for a box that has room for one, so it needs the node model
 * to change, and that is the next piece of 0.5.0 rather than this one.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

int ar_is_inline_level(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE_BLOCK;
}

/*
 * Where this box's baseline sits, measured from its top border edge.
 *
 * Text puts it under the ascent, inside whatever padding there is. A box with
 * no text has no baseline of its own, and takes its bottom margin edge, so it
 * sits *on* the line rather than across it.
 */
ar_i32 ar_inline_baseline(const ar_node *n)
{
    if (n->text && n->text[0])
    {
        return n->style.v[AR_P_PAD_TOP] + n->ascent;
    }
    return n->rect.h + n->style.v[AR_P_MARGIN_BOTTOM];
}

/* The horizontal space an item takes on a line, margins included. */
static ar_i32 ar__outer_w(const ar_node *n)
{
    return n->rect.w + n->style.v[AR_P_MARGIN_LEFT] + n->style.v[AR_P_MARGIN_RIGHT];
}

/*
 * Positions one finished line and returns its height.
 *
 * Called once the line's members are known, because none of what it does can
 * be decided until then: the baseline is the deepest ascent among them, and
 * the alignment offset needs the width they came to.
 */
static ar_i32 ar__close_line(ar_node *nodes, ar_i32 first, ar_i32 last, ar_i32 top, ar_i32 used_w,
                             ar_i32 inner_w, ar_i32 align)
{
    ar_i32 max_ascent = 0;
    ar_i32 max_descent = 0;
    ar_i32 shift = 0;
    ar_i32 height;
    ar_i32 c;

    for (c = first;; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   base = ar_inline_baseline(ch);
        ar_i32   outer_h =
            ch->rect.h + ch->style.v[AR_P_MARGIN_TOP] + ch->style.v[AR_P_MARGIN_BOTTOM];
        ar_i32 ascent = base + ch->style.v[AR_P_MARGIN_TOP];
        ar_i32 descent = outer_h - ascent;

        if (ascent > max_ascent)
        {
            max_ascent = ascent;
        }
        if (descent > max_descent)
        {
            max_descent = descent;
        }
        if (c == last)
        {
            break;
        }
    }

    height = max_ascent + max_descent;

    if (align == AR_TEXT_ALIGN_RIGHT)
    {
        shift = inner_w - used_w;
    }
    else if (align == AR_TEXT_ALIGN_CENTER)
    {
        shift = (inner_w - used_w) / 2;
    }
    if (shift < 0)
    {
        shift = 0; /* an over-full line is left alone rather than pulled off */
    }

    for (c = first;; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   valign = ch->style.v[AR_P_VERTICAL_ALIGN];
        ar_i32   outer_h =
            ch->rect.h + ch->style.v[AR_P_MARGIN_TOP] + ch->style.v[AR_P_MARGIN_BOTTOM];

        ch->rect.x += shift;

        switch (valign)
        {
        case AR_VALIGN_TOP:
            ch->rect.y = top + ch->style.v[AR_P_MARGIN_TOP];
            break;
        case AR_VALIGN_BOTTOM:
            ch->rect.y = top + height - outer_h + ch->style.v[AR_P_MARGIN_TOP];
            break;
        case AR_VALIGN_MIDDLE:
            ch->rect.y = top + (height - outer_h) / 2 + ch->style.v[AR_P_MARGIN_TOP];
            break;
        default:
            /* On the shared baseline: as far below the line's top as this
               item's own baseline is below its own top. */
            ch->rect.y = top + max_ascent - ar_inline_baseline(ch);
            break;
        }
        if (c == last)
        {
            break;
        }
    }
    return height;
}

/*
 * Lays a run of inline-level siblings into lines and returns the total height.
 *
 * `first` and `stop` bound the run: everything from `first` up to but not
 * including `stop`, which is -1 when the run reaches the end of the children.
 * The caller has already given every item its width and height.
 */
ar_i32 ar_inline_run(ar_node *nodes, ar_i32 first, ar_i32 stop, ar_i32 left, ar_i32 top,
                     ar_i32 inner_w, ar_i32 align)
{
    ar_i32 line_first = -1;
    ar_i32 line_last = -1;
    ar_i32 x = 0;
    ar_i32 y = 0;
    ar_i32 c;

    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   w;

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            ch->rect.x = left;
            ch->rect.y = top + y;
            ch->rect.w = 0;
            ch->rect.h = 0;
            continue;
        }

        w = ar__outer_w(ch);

        /* `x > 0` matters: an item wider than the line still has to go
           somewhere, and putting it on a line of its own and letting it
           overflow is visible, where looping forever is not. */
        if (line_first >= 0 && x + w > inner_w && x > 0)
        {
            y += ar__close_line(nodes, line_first, line_last, top + y, x, inner_w, align);
            line_first = -1;
            x = 0;
        }

        ch->rect.x = left + x + ch->style.v[AR_P_MARGIN_LEFT];
        if (line_first < 0)
        {
            line_first = c;
        }
        line_last = c;
        x += w;
    }

    if (line_first >= 0)
    {
        y += ar__close_line(nodes, line_first, line_last, top + y, x, inner_w, align);
    }
    return y;
}
