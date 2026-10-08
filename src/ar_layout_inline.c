/*
 * areole - inline formatting
 * SPDX-License-Identifier: MIT
 *
 * Line boxes. Inline-level content is filled left to right until the next
 * piece will not fit, and then a new line starts under the last.
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
 * ------------------------------------------------------------------------
 * Atomic items and fragmented ones
 *
 * `inline-block` is atomic: one box, one rectangle, never split. It goes on
 * whichever line it fits.
 *
 * `inline` is not. Its text flows into the lines around it and is cut wherever
 * a line ends, so one box becomes one rectangle per line it touches -- a
 * fragment. A bold run in the middle of a paragraph is one box and may be five
 * rectangles.
 *
 * Both go through the same walk, because the only difference is how many break
 * opportunities a child offers: an atomic one offers none.
 * ------------------------------------------------------------------------ */
#include "ar_break.h"
#include "ar_node.h"

#include <string.h>

int ar_is_inline_level(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE_BLOCK ||
           n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE;
}

int ar_is_fragmentable(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE && n->text && n->text[0];
}

/*
 * Whether this box's children join the line it is on, rather than the box
 * being placed on that line as one item.
 *
 * This is what a non-replaced inline box *is*. `<b>bold</b>` in the middle of
 * a sentence is not an item on the line; its text is on the line, sharing it
 * with the words either side and breaking across lines with them. The element
 * contributes style -- a weight, a colour, an underline -- and no box of its
 * own that anything can bump into.
 *
 * Before this the line filler walked one level of siblings, so an element's
 * text, which the document walk puts in a child box, was never reached. The
 * element arrived with no text of its own, failed ar_is_fragmentable, and was
 * placed atomically: it took a whole line to itself, gained a space either
 * side that nothing asked for, and everything past the first line's worth of
 * its text was never emitted at all. `H<sub>2</sub>O` came out as `H 2O`, and
 * a link in a narrow column lost most of its words.
 */
int ar_flows_children(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE && !(n->text && n->text[0]) &&
           n->first_child >= 0;
}

/*
 * A `<br>` ends its line, and was an empty inline that ended nothing: `one<br>
 * two` was one line, and `<label>Name</label><br><input>` -- every form
 * written by hand -- put the field beside its label. The hash is asked only of
 * a box with no children and no text, which is what reaches the atomic branch.
 */
int ar_is_line_break(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE && n->first_child < 0 &&
           !(n->text && n->text[0]) && n->sel_tag == ar_hash("br", 2);
}

/*
 * Where this box's baseline sits, measured from its top border edge.
 *
 * Text puts it under the ascent, inside whatever padding there is. A box with
 * no text has no baseline of its own, and takes its bottom margin edge, so it
 * sits *on* the line rather than across it.
 */
/*
 * How far this box's baseline is lifted off the line's, in pixels.
 *
 * `sub` and `super` are not a fourth and fifth way of aligning a box. The line
 * still has one shared baseline and everything still sits on it; these two
 * move where *this* box's own baseline meets it, which is why they are handled
 * inside the baseline case rather than beside it.
 *
 * The fractions are of this box's own font size, and the specification says
 * "an appropriate offset" without giving one -- it is deliberately the font's
 * business. `sub` and `sup` come through at 0.8125em from the user-agent
 * sheet, so a third of that is very close to the third of the *parent* size
 * that browsers settle on, and measuring against Edge is what picked the two
 * numbers rather than arithmetic.
 */
/*
 * The alignment in force for the box that is actually on the line.
 *
 * `vertical-align` is stated on the inline box -- `<sup>` -- and what reaches
 * the line is its text, which is a child box. The property does not inherit
 * and should not: it applies to an inline box and moves its whole contents
 * with it, which is a different thing from every descendant being aligned
 * independently. So the line asks the box it is placing, and where that says
 * `baseline` it asks the inline boxes the box is inside.
 *
 * The walk stops at the first ancestor that is not a flowed inline, because
 * that is the block the line belongs to -- its own `vertical-align` is about
 * how *it* sits in *its* parent's line, not about this one.
 */
static ar_i32 ar__valign_of(const ar_node *nodes, ar_i32 i)
{
    ar_i32 at = i;

    while (at >= 0)
    {
        ar_i32 v = nodes[at].style.v[AR_P_VERTICAL_ALIGN];

        /* A length is a shift of the baseline and not an alignment, and its
           number must not be read as one: `vertical-align: 2px` would
           otherwise be `middle`, which is enum value two. */
        if (nodes[at].style.unit[AR_P_VERTICAL_ALIGN] == AR_UNIT_PX && v != 0)
        {
            return AR_VALIGN_BASELINE;
        }
        if (v != AR_VALIGN_BASELINE)
        {
            return v;
        }
        at = nodes[at].parent;
        if (at < 0 || !ar_flows_children(&nodes[at]))
        {
            break;
        }
    }
    return AR_VALIGN_BASELINE;
}

static ar_i32 ar__valign_shift(const ar_node *nodes, ar_i32 i)
{
    ar_i32 px = nodes[i].style.v[AR_P_FONT_SIZE];
    ar_i32 v = ar__valign_of(nodes, i);
    ar_i32 at;

    /*
     * `vertical-align: <length>`: the baseline raised by that much, or lowered
     * by a negative one -- what a browser sets a progress bar and a meter on,
     * -0.2em, and what the user-agent sheet needed a negative margin to fake
     * until this read it. Up the same chain of inline boxes the keywords take.
     */
    for (at = i; at >= 0;)
    {
        /* Zero pixels is the initial value -- the unit a style starts with is
           the pixel -- and means the baseline, so the walk goes on up to the
           `<sup>` that may be above. Stopping here took every subscript and
           superscript back down to the line. */
        if (nodes[at].style.unit[AR_P_VERTICAL_ALIGN] == AR_UNIT_PX &&
            nodes[at].style.v[AR_P_VERTICAL_ALIGN] != 0)
        {
            return nodes[at].style.v[AR_P_VERTICAL_ALIGN];
        }
        if (nodes[at].style.v[AR_P_VERTICAL_ALIGN] != AR_VALIGN_BASELINE)
        {
            break;
        }
        at = nodes[at].parent;
        if (at < 0 || !ar_flows_children(&nodes[at]))
        {
            break;
        }
    }

    if (v == AR_VALIGN_SUPER)
    {
        return px / 2;
    }
    if (v == AR_VALIGN_SUB)
    {
        return -((px * 2) / 5);
    }
    return 0;
}

ar_i32 ar_inline_baseline(const ar_node *n)
{
    if (n->text && n->text[0])
    {
        return n->style.v[AR_P_PAD_TOP] + n->ascent;
    }

    /*
     * A non-replaced inline box sits on the same baseline as the text inside
     * it, and `<b>bold</b>` is one: `display:inline` with its text in a child
     * box, so it reaches here with no text of its own.
     *
     * The rule below -- bottom margin edge on the baseline -- is CSS 2.1
     * §10.8.1 for *atomic* inline-level boxes, which is an inline-block or a
     * replaced element. Applying it to every textless inline box lifted every
     * `<b>`, `<i>`, `<code>` and `<a>` off the line its surrounding text sat
     * on, by about the height of a line. On a page of plain HTML that is the
     * most visible thing wrong with it and no test saw it, because every
     * assertion here is about a rectangle and the rectangles were all the
     * right size in the wrong place.
     */
    if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE)
    {
        return n->style.v[AR_P_PAD_TOP] + n->ascent;
    }

    return n->rect.h + n->style.v[AR_P_MARGIN_BOTTOM];
}

/*
 * The absolute y of the baseline of the last line box inside a box, if it has
 * one. Last in-flow child first, descending, because the last line is in the
 * last thing that has lines.
 */
static int ar__last_line(const ar_node *nodes, ar_i32 i, ar_i32 *out)
{
    ar_i32 c;

    for (c = nodes[i].last_child; c >= 0; c = nodes[c].prev_sibling)
    {
        const ar_node *ch = &nodes[c];

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE || ar_is_floated(ch) ||
            ar_is_out_of_flow(ch))
        {
            continue;
        }
        /* A box told to be no height at all holds no line anybody sees: the
           options a closed select is not showing are exactly that, and taking
           the baseline from the last of them put the select's text a line
           away from its label's. */
        if (ch->style.unit[AR_P_HEIGHT] != AR_UNIT_AUTO && ch->rect.h == 0)
        {
            continue;
        }
        /* Any text box, even an empty one: an empty field still has a line
           and the line still has a baseline. */
        if (ch->text)
        {
            ar_i32 pt = ch->style.v[AR_P_PAD_TOP];
            ar_i32 inner = ch->rect.h - pt - ch->style.v[AR_P_PAD_BOTTOM];
            ar_i32 extra = 0;

            if (ch->frag_count > 0 || ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE)
            {
                /* A run on a line: its last line is its bottom one. */
                *out = ch->rect.y + ch->rect.h - ch->text_h + ch->ascent;
                return 1;
            }
            /* A block of its own text: the last of however many lines it
               wrapped to, each `line_h` below the one before. */
            if (ch->line_h > 0 && inner > ch->text_h)
            {
                extra = (inner - ch->text_h) / ch->line_h * ch->line_h;
            }
            *out = ch->rect.y + pt + extra + ch->ascent;
            return 1;
        }
        if (ar__last_line(nodes, c, out))
        {
            return 1;
        }
    }
    return 0;
}

/*
 * A replaced element: its contents are a picture, not lines, and it sits on its
 * bottom margin edge whatever is inside it. Known by tag, like the painter's
 * triangles -- there is no property that says "replaced" -- and only asked of
 * an inline-block, which is the one place the answer matters.
 */
static int ar__replaced(const ar_node *n)
{
    static const char *const TAGS[] = {"svg",   "img",    "video", "canvas",
                                       "audio", "iframe", "embed", "object"};
    ar_i32                   k;

    for (k = 0; k < 8; ++k)
    {
        if (n->sel_tag == ar_hash(TAGS[k], (ar_u32)strlen(TAGS[k])))
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Where an inline-level box's baseline sits, from its top border edge.
 *
 * For an inline-block this is CSS 2.1 10.8.1 in full, and it was only ever
 * half: "the baseline of an inline-block is the baseline of its last line box
 * in the normal flow, unless it has no in-flow line boxes or its overflow is
 * not visible, in which case it is the bottom margin edge". The "unless" was
 * implemented and the rule was not, so every inline-block sat its bottom on
 * the line -- a field beside its label stood a line above the label's words,
 * and every line holding a control was four pixels taller than a browser's.
 *
 * A form control's text field is the exception browsers make to the overflow
 * clause: an `<input>` clips its text and still sits on the text's baseline. A
 * textarea, which scrolls, takes its bottom edge, as it does in Chrome.
 */
ar_i32 ar_inline_baseline_of(const ar_node *nodes, ar_i32 i)
{
    const ar_node *n = &nodes[i];
    ar_i32         y;
    int            control =
        (n->state & (AR_STATE_ENABLED | AR_STATE_DISABLED)) != 0 && !ar_is_scroll_container(n);

    if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE_BLOCK && ar__replaced(n))
    {
        return ar_inline_baseline(n);
    }

    if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE_BLOCK && !(n->text && n->text[0]) &&
        n->first_child >= 0 && (!ar_clips(n) || control) && ar__last_line(nodes, i, &y))
    {
        return y - n->rect.y;
    }

    /*
     * A control with no line of text in it -- a checkbox, a radio, a slider, a
     * colour field -- sits on the bottom of its content box, not of its margin
     * box. Measured, three ways at once: a row holding a checkbox is 20 pixels
     * in Edge, one holding a slider 22 and one holding a colour field 27, and
     * the content-box bottom gives all three where the margin edge gives 23,
     * 24 and 31. A textarea scrolls and keeps the margin edge, which is what
     * puts its label at its bottom.
     */
    if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_INLINE_BLOCK && control)
    {
        return n->rect.h - n->style.v[AR_P_PAD_BOTTOM];
    }
    return ar_inline_baseline(n);
}

/*
 * One line's worth of a box, which is emphatically not `n->rect.h`.
 *
 * `n->rect` is *grown* into the union of a box's fragments as they are
 * emitted -- that is what makes a wrapped box report one truthful rectangle to
 * hit testing and damage tracking. Reading the height back out of it while it
 * is still growing made every fragment after the first as tall as all the
 * fragments before it, so the second line was two lines tall, the third was
 * four, and a sixty-character sentence in a 200px box came out 880 pixels
 * where a browser gives 64. The narrower the box, the worse it got.
 *
 * A text box's line is `text_h` -- the height of the text itself, which is
 * what `rect.h` held before the first union and what every single-line box was
 * therefore already using. `line_h` is the wrong one: it carries the face's
 * line gap, and adding that here makes every line taller than the box a
 * browser draws. An atomic item is one fragment and its whole box sits on the
 * line, so `rect.h` is right for it and never grows.
 */
static ar_i32 ar__frag_h(const ar_node *n)
{
    if (n->text && n->text[0])
    {
        return n->style.v[AR_P_PAD_TOP] + n->text_h + n->style.v[AR_P_PAD_BOTTOM];
    }
    return n->rect.h;
}

/* The horizontal space an atomic item takes on a line, margins included. */
static ar_i32 ar__outer_w(const ar_node *n)
{
    return n->rect.w + n->style.v[AR_P_MARGIN_LEFT] + n->style.v[AR_P_MARGIN_RIGHT];
}

/*
 * How much of a line is left, once the floats that reach into it are counted.
 *
 * With no float list this is the whole content width, which is what every
 * caller before floats existed was getting.
 */
static void ar__line_band(const ar_float_ctx *fc, ar_i32 y, ar_i32 left, ar_i32 inner_w,
                          ar_i32 *out_off, ar_i32 *out_w)
{
    ar_i32 lo, hi;

    if (!fc)
    {
        *out_off = 0;
        *out_w = inner_w;
        return;
    }
    ar_float_band(fc, y, 1, &lo, &hi);
    if (lo < left)
    {
        lo = left;
    }
    if (hi > left + inner_w)
    {
        hi = left + inner_w;
    }
    *out_off = lo - left;
    *out_w = hi - lo;
    if (*out_w < 0)
    {
        *out_w = 0;
    }
}

/*
 * What the walk carries while it fills a line.
 *
 * Fragments are emitted as they are found, with a provisional y, and moved
 * once the line closes and its baseline is known -- which is the only moment
 * that number exists.
 */
typedef struct ar__liner
{
    ar_node       *nodes;
    ar_layout_env *env;

    ar_i32 left, top; /* the content box */
    ar_i32 inner_w;
    ar_i32 align;

    ar_i32 y;          /* the current line's top, relative to `top` */
    ar_i32 x;          /* how far along the current line we are     */
    ar_i32 line_off;   /* what a float pushed this line's start to  */
    ar_i32 line_w;     /* and how much of it is left                */
    ar_i32 line_frag0; /* the first fragment on this line           */
    ar_i32 line_has;   /* whether anything has been put on it yet   */
    ar_i32 hang;       /* trailing spaces past the edge, not content */

    /* The fragment being accumulated: a run of one node's pieces that have all
       landed on the current line. */
    ar_i32 open_node;
    ar_i32 open_from, open_to;
    ar_i32 open_x, open_w;
    ar_i32 open_fx; /* open_w unrounded, in 1/AR_ONE_PIXEL, when measure_fx exists */

    /* How many runs of the current node have been placed, so its rectangle is
       written the first time and widened afterwards. */
    ar_i32 pieces_placed;

    /*
     * The strut: what the containing block's own font contributes to every
     * line, whether or not anything on that line is text.
     *
     * CSS 2.1 10.8.1. A line box begins with a zero-width inline box carrying
     * the block's font and line-height, and its ascent and descent join the
     * maxima like any other item's. Without it a line is only as tall as the
     * tallest thing actually on it: a line holding one 30px inline-block came
     * out 30 where a browser gives 33, the three being the half-leading the
     * block's own text would have had, and the last line of every paragraph
     * was short by the same amount.
     *
     * Computed once, because it is a property of the block and not of a line.
     */
    ar_i32 strut_asc, strut_desc;
} ar__liner;

/*
 * An atomic box the line moves takes its insides with it.
 *
 * Its children were laid out before the line started, to measure it, at
 * whatever place it stood then; the line then writes the box's rectangle and
 * nothing else. In between, the line asks where the box's last line of text is
 * -- the box's baseline -- by reading those children against the box, and a
 * box that has moved without them answers with the distance it moved. An
 * inline-block holding one word sat a whole line low on a padded body that
 * way: the body's sixteen pixels of padding read as sixteen pixels of ascent.
 */
static void ar__carry(ar__liner *L, ar_i32 i, ar_i32 dx, ar_i32 dy)
{
    ar_i32 c;

    /* A run of text has nothing inside it to carry, and it is nearly every
       call: asked first, before anything that costs a call of its own. */
    if (L->nodes[i].first_child < 0 || ar_is_fragmentable(&L->nodes[i]))
    {
        return;
    }
    for (c = L->nodes[i].first_child; c >= 0; c = L->nodes[c].next_sibling)
    {
        ar_shift_subtree(L->nodes, L->env->frags, L->env->frag_used, c, dx, dy);
    }
}

static ar_frag *ar__emit(ar__liner *L)
{
    ar_layout_env *env = L->env;

    if (!env->frags || env->frag_used >= env->frag_cap)
    {
        return 0;
    }
    return &env->frags[env->frag_used++];
}

/*
 * Closes the fragment being accumulated, if there is one.
 *
 * The node's own rectangle is written here whether or not a fragment could be
 * recorded, and widened rather than replaced once it has one. That is what
 * makes running out of fragment space degrade to "this inline was not split"
 * instead of "this inline was never positioned" -- a box with nowhere to store
 * its second rectangle still has its first.
 */
static void ar__flush_open(ar__liner *L, int at_break)
{
    ar_node *n;
    ar_frag *f;
    ar_rect  r;

    if (L->open_node < 0)
    {
        return;
    }
    n = &L->nodes[L->open_node];

    r.x = L->left + L->line_off + L->open_x;
    r.y = L->top + L->y; /* provisional; closing the line fixes it */
    r.w = L->open_w;
    r.h = ar__frag_h(n);

    /*
     * The space a break leaves behind is not part of the fragment.
     *
     * A line breaks *after* the space that offered the opportunity, so the
     * last piece on a line usually ends with one. The painter already drops it
     * -- drawing it would be invisible for a left-aligned line and wrong for
     * anything else -- so counting it here makes the rectangle wider than the
     * ink inside it. That showed up twice: an inline box came out eight pixels
     * wider than Chrome's, and a link's underline, which is drawn to the
     * advance the text actually inked, stopped short of the end of its own
     * fragment.
     *
     * Only at a break. When a fragment ends because the *node* changed -- the
     * text before a `<span>`, say -- the space between them is real and the
     * two would collide without it.
     */
    if (at_break && n->text && L->env->measure && L->open_to > L->open_from)
    {
        ar_i32 to = L->open_to;

        while (to > L->open_from &&
               (n->text[to - 1] == ' ' || n->text[to - 1] == '\n' || n->text[to - 1] == '\r'))
        {
            --to;
        }
        if (to < L->open_to)
        {
            r.w -= L->env->measure(L->env->ud, n, to, L->open_to);
            if (r.w < 0)
            {
                r.w = 0;
            }
        }
    }

    if (L->pieces_placed == 0)
    {
        ar__carry(L, L->open_node, r.x - n->rect.x, r.y - n->rect.y);
        n->rect = r;
    }
    else
    {
        n->rect = ar_rect_union(n->rect, r);
    }
    L->pieces_placed++;

    f = ar__emit(L);
    if (f)
    {
        f->node = L->open_node;
        f->from = L->open_from;
        f->to = L->open_to;
        f->rect = r;
        if (n->frag_count == 0)
        {
            n->frag_first = L->env->frag_used - 1;
        }
        n->frag_count++;
    }
    L->open_node = -1;
}

/*
 * Positions everything on the finished line and returns its height.
 *
 * The baseline is the deepest ascent among its members and the alignment
 * offset needs the width they came to, so neither can be decided until the
 * line is over. That is why this is a second pass over the fragments it made.
 */
static ar_i32 ar__close_line(ar__liner *L)
{
    ar_layout_env *env = L->env;
    ar_i32         max_ascent = 0;
    ar_i32         max_descent = 0;
    ar_i32         shift = 0;
    ar_i32         height;
    ar_i32         i;

    ar__flush_open(L, 1);
    if (!env->frags || L->line_frag0 >= env->frag_used)
    {
        return 0;
    }

    /* The strut first, so a line is never shorter than the block's own font
       would make it even when nothing on it is text. */
    max_ascent = L->strut_asc;
    max_descent = L->strut_desc;

    for (i = L->line_frag0; i < env->frag_used; ++i)
    {
        const ar_node *n = &L->nodes[env->frags[i].node];
        ar_i32         outer_h =
            ar__frag_h(n) + n->style.v[AR_P_MARGIN_TOP] + n->style.v[AR_P_MARGIN_BOTTOM];
        /* A raised box needs the line to be taller above the baseline by
           however far it was raised, or a superscript is clipped off the
           top of its own line. A lowered one does the same below. */
        ar_i32 ascent = ar_inline_baseline_of(L->nodes, env->frags[i].node) +
                        n->style.v[AR_P_MARGIN_TOP] +
                        ar__valign_shift(L->nodes, env->frags[i].node);
        ar_i32 descent = outer_h - ascent;

        if (ascent > max_ascent)
        {
            max_ascent = ascent;
        }
        if (descent > max_descent)
        {
            max_descent = descent;
        }
    }
    height = max_ascent + max_descent;

    /* Hanging spaces are not the line's content, so they do not count
       against its alignment either. */
    if (L->align == AR_TEXT_ALIGN_RIGHT)
    {
        shift = L->line_w - (L->x - L->hang);
    }
    else if (L->align == AR_TEXT_ALIGN_CENTER)
    {
        shift = (L->line_w - (L->x - L->hang)) / 2;
    }
    if (shift < 0)
    {
        shift = 0; /* an over-full line is left alone rather than pulled off */
    }

    for (i = L->line_frag0; i < env->frag_used; ++i)
    {
        ar_frag *f = &env->frags[i];
        ar_node *n = &L->nodes[f->node];
        ar_i32   was_y = f->rect.y;
        ar_i32   valign = ar__valign_of(L->nodes, f->node);
        /* One line's worth, for the same reason the line's own height is: this
           runs while `n->rect` is still being grown into the union of the
           fragments, so `rect.h` here is every line already emitted and not
           this one. `vertical-align: bottom` on a wrapped box pushed every
           line after the first off the bottom of its own line. */
        ar_i32 outer_h =
            ar__frag_h(n) + n->style.v[AR_P_MARGIN_TOP] + n->style.v[AR_P_MARGIN_BOTTOM];

        f->rect.x += shift;

        switch (valign)
        {
        case AR_VALIGN_TOP:
            f->rect.y = L->top + L->y + n->style.v[AR_P_MARGIN_TOP];
            break;
        case AR_VALIGN_BOTTOM:
            f->rect.y = L->top + L->y + height - outer_h + n->style.v[AR_P_MARGIN_TOP];
            break;
        case AR_VALIGN_MIDDLE:
            f->rect.y = L->top + L->y + (height - outer_h) / 2 + n->style.v[AR_P_MARGIN_TOP];
            break;
        default:
            /* On the shared baseline: as far below the line's top as this
               item's own baseline is below its own top. */
            f->rect.y = L->top + L->y + max_ascent - ar_inline_baseline_of(L->nodes, f->node) -
                        ar__valign_shift(L->nodes, f->node);
            break;
        }

        /* The box's own rectangle was written from the provisional position,
           so it moves by however far the fragment did. The union at the end
           rebuilds it exactly; this keeps it honest in between. */
        if (n->frag_count == 1)
        {
            ar__carry(L, f->node, f->rect.x - n->rect.x, f->rect.y - was_y);
            n->rect.y += f->rect.y - was_y;
            n->rect.x = f->rect.x;
        }
    }
    return height;
}

/* Ends the current line and starts the next one under it. */
static void ar__break_line(ar__liner *L, const ar_float_ctx *fc, ar_i32 abs_top)
{
    L->y += ar__close_line(L);
    L->x = 0;
    L->line_has = 0;
    L->hang = 0;
    L->line_frag0 = L->env->frags ? L->env->frag_used : 0;
    ar__line_band(fc, abs_top + L->y, L->left, L->inner_w, &L->line_off, &L->line_w);
}

/* Whether text[from, to) is nothing but white space a line may collapse. */
static int ar__only_spaces(const char *t, ar_i32 from, ar_i32 to)
{
    for (; from < to; ++from)
    {
        if (t[from] != ' ' && t[from] != '\t' && t[from] != '\n' && t[from] != '\r' &&
            t[from] != '\f')
        {
            return 0;
        }
    }
    return 1;
}

/* Adds one piece of one node to the current line. `fx` is its unrounded
   width, which the run keeps a sum of (see ar__piece_w). */
static void ar__add_piece(ar__liner *L, ar_i32 c, ar_i32 from, ar_i32 to, ar_i32 w, ar_i32 fx)
{
    if (L->open_node != c)
    {
        ar__flush_open(L, 0);
        L->pieces_placed = L->nodes[c].frag_count > 0 ? 1 : 0;
        L->open_node = c;
        L->open_from = from;
        L->open_x = L->x;
        L->open_w = 0;
        L->open_fx = 0;
    }
    L->open_to = to;
    L->open_w += w;
    L->open_fx += fx;
    L->x += w;
    L->line_has = 1;
    L->hang = 0;
}

/*
 * How many whole pixels a piece adds to the line, rounded as part of its run.
 *
 * Each measurement rounds up to a whole pixel, so summing the pieces adds up
 * to a pixel per word that the text does not have -- and a box sized to its
 * contents, which measured the whole string once, is then too narrow for its
 * own words. A button whose label is two words broke onto two lines inside
 * itself. So the run keeps its width unrounded and each piece adds whatever
 * the rounded total grows by: a run's pieces sum to exactly the run, rounded
 * once. The advances are summed without kerning, so the pieces' sum is the
 * whole string's width to the unit, not an approximation of it.
 *
 * `cont` says the piece continues the open run on this line. Without an
 * unrounded measure the whole-pixel one is exact, so the piece is measured on
 * its own and nothing is carried.
 */
static ar_i32 ar__piece_w(ar__liner *L, const ar_node *n, ar_i32 at, ar_i32 next, int cont,
                          ar_i32 *fx)
{
    ar_layout_env *env = L->env;

    if (env->measure_fx)
    {
        ar_i32 base = cont ? L->open_fx : 0;
        ar_i32 had = cont ? L->open_w : 0;

        *fx = env->measure_fx(env->ud, n, at, next);
        return (base + *fx + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL - had;
    }
    *fx = 0;
    return env->measure(env->ud, n, at, next);
}

/*
 * One level of inline children, and it descends into the inline boxes among
 * them rather than placing them.
 *
 * A non-replaced inline box is not an item on the line -- its *contents*
 * are, sharing the line with whatever is either side and breaking across
 * lines with it. That is what ar_flows_children says, and recursing here is
 * the whole of implementing it: the liner carries the line being filled, so
 * a nested run adds to the same line rather than starting its own.
 *
 * Bounded by the tree, which is already bounded by AR_MAX_DEPTH. Nothing
 * here can recurse further than the document nests.
 */
static int ar__flow(ar__liner *L, ar_i32 first, ar_i32 stop, const ar_float_ctx *fc, ar_i32 abs_top,
                    int *anything)
{
    ar_node       *nodes = L->nodes;
    ar_layout_env *env = L->env;
    ar_i32         c;

    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            ch->rect.x = L->left;
            ch->rect.y = L->top + L->y;
            ch->rect.w = 0;
            ch->rect.h = 0;
            continue;
        }

        ch->frag_first = 0;
        ch->frag_count = 0;

        /*
         * An inline box contributes style and no box of its own: its
         * children join this line. Its rectangle is rebuilt from theirs
         * once the run is placed -- see ar__settle_run.
         *
         * Its own margin, border and padding are the one thing it puts on the
         * line itself: before its first piece and after its last, as CSS 2.1
         * 9.4.2 has them, so `padding-left: 8px` on a label moves the label's
         * words eight pixels away from the radio before it. They were not
         * counted at all, and the words sat against the radio.
         */
        if (ar_flows_children(ch))
        {
            ar_i32 lead = ch->style.v[AR_P_MARGIN_LEFT] + ch->style.v[AR_P_BORDER_WIDTH] +
                          ch->style.v[AR_P_PAD_LEFT];
            ar_i32 trail = ch->style.v[AR_P_MARGIN_RIGHT] + ch->style.v[AR_P_BORDER_WIDTH] +
                           ch->style.v[AR_P_PAD_RIGHT];

            if (lead > 0)
            {
                ar__flush_open(L, 0);
                L->x += lead;
            }
            ar__flow(L, ch->first_child, -1, fc, abs_top, anything);
            if (trail > 0)
            {
                ar__flush_open(L, 0);
                L->x += trail;
            }
            continue;
        }

        if (ar_is_fragmentable(ch) && env->measure)
        {
            /*
             * A fragmentable box offers its text one break opportunity at a
             * time. A piece that does not fit starts a new line; a piece wider
             * than a whole line goes on one anyway, because putting it
             * somewhere and letting it overflow is visible, where looping
             * forever is not.
             */
            ar_i32 at = 0;

            for (;;)
            {
                ar_i32 kind;
                ar_i32 next = ar_break_next(ch->text, at, &kind);
                ar_i32 w, fx;

                if (next <= at)
                {
                    break;
                }

                /* Rounded as part of its run, not on its own: ar__piece_w. */
                w = ar__piece_w(L, ch, at, next, L->open_node == c, &fx);

                /*
                 * White space that would start a line, or that does not fit at
                 * the end of one where the line may wrap, is not a line's
                 * content (CSS Text 4.1.3): at the end it hangs, at the start
                 * it is removed. The space after a field as wide as its line --
                 * `<input style="width:100%">` and the newline before the next
                 * `<label>` -- wrapped onto a line of its own and pushed every
                 * field after it down by one.
                 *
                 * "Start" is nothing on the line yet, not x == 0: the space
                 * after an empty inline-block is content, and the space just
                 * inside a padded `<a>` at a line's start is not. A newline
                 * `pre-line` kept is a break and still has to happen.
                 */
                if (AR_WS_COLLAPSES(ch->style.v[AR_P_WHITE_SPACE]) && kind != AR_BREAK_MANDATORY &&
                    ar__only_spaces(ch->text, at, next) &&
                    (!L->line_has ||
                     (AR_WS_WRAPS(ch->style.v[AR_P_WHITE_SPACE]) && L->x + w > L->line_w)))
                {
                    at = next;
                    continue;
                }

                /*
                 * `white-space` decides whether this line may end here at all.
                 *
                 * A field, a button's label and a table's `nowrap` column all
                 * want the text to run off the end rather than to fold, and
                 * until now nothing could say so -- which is why a long value
                 * in a narrow field wrapped where every real field scrolls.
                 * The mandatory break below is deliberately outside this test:
                 * a newline in `pre` text breaks the line whatever the
                 * wrapping says, which is the whole of what `pre` means.
                 */
                /*
                 * A word is a piece with the spaces after it, and those spaces
                 * hang past the edge rather than count against it (CSS Text 3
                 * 4.1.3): a word that fits stays on the line even when its
                 * space does not. Measured again without them only when the
                 * whole piece does not fit, which is once a line rather than
                 * once a word. Georgia's "rendered it" fitted a 640-pixel
                 * column with 2.6 to spare and went down a line, because its
                 * space did not (#20).
                 */
                if (AR_WS_WRAPS(ch->style.v[AR_P_WHITE_SPACE]) && L->x > 0 &&
                    L->x + w > L->line_w && AR_WS_COLLAPSES(ch->style.v[AR_P_WHITE_SPACE]))
                {
                    ar_i32 end = next;

                    while (end > at && (ch->text[end - 1] == ' ' || ch->text[end - 1] == '\t' ||
                                        ch->text[end - 1] == '\n' || ch->text[end - 1] == '\r' ||
                                        ch->text[end - 1] == '\f'))
                    {
                        --end;
                    }
                    if (end > at && end < next)
                    {
                        ar_i32 fx_word;
                        ar_i32 word = ar__piece_w(L, ch, at, end, L->open_node == c, &fx_word);

                        if (L->x + word <= L->line_w)
                        {
                            ar__add_piece(L, c, at, next, w, fx);
                            L->hang = w - word;
                            *anything = 1;
                            at = next;
                            if (kind == AR_BREAK_MANDATORY && ch->text[at])
                            {
                                ar__break_line(L, fc, abs_top);
                            }
                            continue;
                        }
                    }
                }
                if (AR_WS_WRAPS(ch->style.v[AR_P_WHITE_SPACE]) && L->x > 0 && L->x + w > L->line_w)
                {
                    ar__break_line(L, fc, abs_top);
                    /* The run it was measured against ended with the line, so
                       it starts one: its own width, rounded on its own. The
                       unrounded width is already in hand, and is the same
                       whichever run it is in. */
                    if (env->measure_fx)
                    {
                        w = (fx + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL;
                    }
                }
                ar__add_piece(L, c, at, next, w, fx);
                *anything = 1;
                at = next;
                if (kind == AR_BREAK_MANDATORY && ch->text[at])
                {
                    ar__break_line(L, fc, abs_top);
                }
            }
            continue;
        }

        /* Atomic: one piece, the whole box, never split. */
        {
            ar_i32 w = ar__outer_w(ch);

            if (L->x > 0 && L->x + w > L->line_w)
            {
                ar__break_line(L, fc, abs_top);
            }
            ar__add_piece(L, c, 0, 0, w, 0);
            *anything = 1;
            /* Placed first, so a line holding nothing but the break -- the
               middle of `<br><br>` -- is a line, and as tall as the strut. A
               break at the very end opens a line nothing goes on, which
               closes at no height, as a browser's does. */
            if (ar_is_line_break(ch))
            {
                ar__break_line(L, fc, abs_top);
            }
        }
    }
    return *anything;
}

/*
 * Every box's own rectangle, rebuilt from what the line filler produced.
 *
 * A fragmented box is the union of its fragments -- that is what lets hit
 * testing, damage tracking and the inspection API keep asking for one
 * rectangle without knowing fragments exist.
 *
 * An inline box that flowed its children has no fragments of its own and is
 * the union of *theirs*. It has to be done after them, which is why this
 * recurses before it unions: a `<b>` inside an `<a>` has to settle before the
 * `<a>` can ask where it ended up.
 */
static void ar__settle_run(ar_node *nodes, ar_layout_env *env, ar_i32 first, ar_i32 stop)
{
    ar_i32 c;

    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   k;

        if (ar_flows_children(ch))
        {
            ar_i32 kid;
            int    any = 0;

            ar__settle_run(nodes, env, ch->first_child, -1);
            for (kid = ch->first_child; kid >= 0; kid = nodes[kid].next_sibling)
            {
                if (nodes[kid].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
                {
                    continue;
                }
                {
                    /* An atomic child's margins are on the line inside this box,
                       so they are inside its rectangle too: a label wrapping a
                       checkbox starts where the checkbox's margin does. */
                    ar_rect kr = nodes[kid].rect;

                    if (!ar_is_fragmentable(&nodes[kid]) && !ar_flows_children(&nodes[kid]))
                    {
                        kr.x -= nodes[kid].style.v[AR_P_MARGIN_LEFT];
                        kr.w += nodes[kid].style.v[AR_P_MARGIN_LEFT] +
                                nodes[kid].style.v[AR_P_MARGIN_RIGHT];
                    }
                    ch->rect = any ? ar_rect_union(ch->rect, kr) : kr;
                }
                any = 1;
            }
            /* And out to its own padding and border, which the line made room
               for either side of its contents. */
            if (any)
            {
                ar_i32 l = ch->style.v[AR_P_BORDER_WIDTH] + ch->style.v[AR_P_PAD_LEFT];
                ar_i32 r = ch->style.v[AR_P_BORDER_WIDTH] + ch->style.v[AR_P_PAD_RIGHT];

                ch->rect.x -= l;
                ch->rect.w += l + r;
            }
            continue;
        }

        if (ch->frag_count <= 0)
        {
            continue;
        }
        ch->rect = env->frags[ch->frag_first].rect;
        for (k = 1; k < ch->frag_count; ++k)
        {
            ch->rect = ar_rect_union(ch->rect, env->frags[ch->frag_first + k].rect);
        }

        /* An atomic item is its own single fragment, and the width reserved
           for it included its margins, so its box is inset back out of them.
           Nothing is left to paint fragment by fragment. */
        if (!ar_is_fragmentable(ch))
        {
            ar_frag *f = &env->frags[ch->frag_first];

            ch->rect.x = f->rect.x + ch->style.v[AR_P_MARGIN_LEFT];
            ch->rect.w = f->rect.w - ch->style.v[AR_P_MARGIN_LEFT] - ch->style.v[AR_P_MARGIN_RIGHT];
            ch->frag_count = 0;

            /*
             * The line has just decided where this box goes, by writing its
             * rectangle -- not by moving it, so its insides are still where
             * they were laid out to measure it. The memo would tell the
             * forward sweep they are settled; clearing it has the sweep lay
             * them out again at the box's place on the line. Every field's
             * text was left at the left edge of the page without this, the
             * moment a field's height was automatic.
             */
            ch->measured_w = -1;
        }
    }
}

/*
 * Lays a run of inline-level siblings into lines and returns the total height.
 *
 * `first` and `stop` bound the run: everything from `first` up to but not
 * including `stop`, which is -1 when the run reaches the end of the children.
 */
ar_i32 ar_inline_run(ar_node *nodes, ar_i32 first, ar_i32 stop, ar_i32 left, ar_i32 top,
                     ar_i32 inner_w, ar_i32 align, const ar_float_ctx *fc, ar_i32 abs_top,
                     ar_layout_env *env)
{
    ar__liner L;
    int       anything = 0;

    L.nodes = nodes;
    L.env = env;
    L.left = left;
    L.top = top;
    L.inner_w = inner_w;
    L.align = align;
    L.y = 0;
    L.x = 0;
    L.line_has = 0;
    L.hang = 0;
    L.line_frag0 = env->frags ? env->frag_used : 0;

    /*
     * The block is the run's parent: the box whose font every line here is
     * measured against. Its `ascent` and `text_h` already carry the
     * half-leading that `line-height` asked for, so the strut needs no
     * arithmetic of its own.
     */
    {
        ar_i32 block = first >= 0 ? nodes[first].parent : -1;

        L.strut_asc = block >= 0 ? nodes[block].ascent : 0;
        L.strut_desc = block >= 0 ? nodes[block].text_h - nodes[block].ascent : 0;
        if (L.strut_desc < 0)
        {
            L.strut_desc = 0;
        }
    }
    L.open_node = -1;
    L.open_from = 0;
    L.open_to = 0;
    L.open_x = 0;
    L.open_w = 0;
    L.open_fx = 0;
    L.pieces_placed = 0;
    ar__line_band(fc, abs_top, left, inner_w, &L.line_off, &L.line_w);

    ar__flow(&L, first, stop, fc, abs_top, &anything);

    if (anything)
    {
        L.y += ar__close_line(&L);
    }

    /*
     * A box's own rect becomes the union of its fragments, so everything that
     * wants one rectangle for a box -- hit testing, damage tracking, the
     * inspection API -- keeps getting a truthful one without knowing that
     * fragments exist.
     */
    ar__settle_run(nodes, env, first, stop);
    return L.y;
}
