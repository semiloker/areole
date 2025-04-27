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

    /* The fragment being accumulated: a run of one node's pieces that have all
       landed on the current line. */
    ar_i32 open_node;
    ar_i32 open_from, open_to;
    ar_i32 open_x, open_w;

    /* How many runs of the current node have been placed, so its rectangle is
       written the first time and widened afterwards. */
    ar_i32 pieces_placed;
} ar__liner;

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
static void ar__flush_open(ar__liner *L)
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
    r.h = n->rect.h;

    if (L->pieces_placed == 0)
    {
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

    ar__flush_open(L);
    if (!env->frags || L->line_frag0 >= env->frag_used)
    {
        return 0;
    }

    for (i = L->line_frag0; i < env->frag_used; ++i)
    {
        const ar_node *n = &L->nodes[env->frags[i].node];
        ar_i32 outer_h = n->rect.h + n->style.v[AR_P_MARGIN_TOP] + n->style.v[AR_P_MARGIN_BOTTOM];
        ar_i32 ascent = ar_inline_baseline(n) + n->style.v[AR_P_MARGIN_TOP];
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

    if (L->align == AR_TEXT_ALIGN_RIGHT)
    {
        shift = L->line_w - L->x;
    }
    else if (L->align == AR_TEXT_ALIGN_CENTER)
    {
        shift = (L->line_w - L->x) / 2;
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
        ar_i32   valign = n->style.v[AR_P_VERTICAL_ALIGN];
        ar_i32   outer_h = n->rect.h + n->style.v[AR_P_MARGIN_TOP] + n->style.v[AR_P_MARGIN_BOTTOM];

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
            f->rect.y = L->top + L->y + max_ascent - ar_inline_baseline(n);
            break;
        }

        /* The box's own rectangle was written from the provisional position,
           so it moves by however far the fragment did. The union at the end
           rebuilds it exactly; this keeps it honest in between. */
        if (n->frag_count == 1)
        {
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
    L->line_frag0 = L->env->frags ? L->env->frag_used : 0;
    ar__line_band(fc, abs_top + L->y, L->left, L->inner_w, &L->line_off, &L->line_w);
}

/* Adds one piece of one node to the current line. */
static void ar__add_piece(ar__liner *L, ar_i32 c, ar_i32 from, ar_i32 to, ar_i32 w)
{
    if (L->open_node != c)
    {
        ar__flush_open(L);
        L->pieces_placed = L->nodes[c].frag_count > 0 ? 1 : 0;
        L->open_node = c;
        L->open_from = from;
        L->open_x = L->x;
        L->open_w = 0;
    }
    L->open_to = to;
    L->open_w += w;
    L->x += w;
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
    ar_i32    c;
    int       anything = 0;

    L.nodes = nodes;
    L.env = env;
    L.left = left;
    L.top = top;
    L.inner_w = inner_w;
    L.align = align;
    L.y = 0;
    L.x = 0;
    L.line_frag0 = env->frags ? env->frag_used : 0;
    L.open_node = -1;
    L.open_from = 0;
    L.open_to = 0;
    L.open_x = 0;
    L.open_w = 0;
    L.pieces_placed = 0;
    ar__line_band(fc, abs_top, left, inner_w, &L.line_off, &L.line_w);

    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];

        if (ch->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            ch->rect.x = left;
            ch->rect.y = top + L.y;
            ch->rect.w = 0;
            ch->rect.h = 0;
            continue;
        }

        ch->frag_first = 0;
        ch->frag_count = 0;

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
                ar_i32 w;

                if (next <= at)
                {
                    break;
                }
                w = env->measure(env->ud, ch, at, next);
                if (L.x > 0 && L.x + w > L.line_w)
                {
                    ar__break_line(&L, fc, abs_top);
                }
                ar__add_piece(&L, c, at, next, w);
                anything = 1;
                at = next;
                if (kind == AR_BREAK_MANDATORY && ch->text[at])
                {
                    ar__break_line(&L, fc, abs_top);
                }
            }
            continue;
        }

        /* Atomic: one piece, the whole box, never split. */
        {
            ar_i32 w = ar__outer_w(ch);

            if (L.x > 0 && L.x + w > L.line_w)
            {
                ar__break_line(&L, fc, abs_top);
            }
            ar__add_piece(&L, c, 0, 0, w);
            anything = 1;
        }
    }

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
    for (c = first; c >= 0 && c != stop; c = nodes[c].next_sibling)
    {
        ar_node *ch = &nodes[c];
        ar_i32   k;

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
        }
    }
    return L.y;
}
