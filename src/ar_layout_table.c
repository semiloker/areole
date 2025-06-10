/*
 * areole - the table formatting context
 * SPDX-License-Identifier: MIT
 *
 * The second layout algorithm, and it shares nothing with the first.
 *
 * Block layout is a walk: each child's size depends on itself and the space it
 * is given. A table is a solve. A column is as wide as its widest cell needs,
 * a row as tall as its tallest cell comes to, and a `colspan` couples columns
 * that would otherwise be independent -- so no cell can be placed until every
 * cell has been looked at.
 *
 * ------------------------------------------------------------------------
 * Why it still fits two plain sweeps
 *
 * ar_layout.c measures backwards and places forwards, with no recursion and no
 * stack, because a parent always sits at a lower index than its children. A
 * table does not break that: by the time the backward sweep reaches the table
 * node every cell beneath it has a min-content and a max-content width
 * already, which is exactly the input the column solve needs. And by the time
 * the forward sweep reaches it, the table's own rectangle is final.
 *
 * So the table is opaque to everything around it. Its ancestors see one box
 * with two intrinsic widths and need no changes at all.
 *
 * ------------------------------------------------------------------------
 * Keeping it linear
 *
 * Criterion 5 of the release gates this: a hundred rows to ten thousand must
 * grow no worse than 1.4x linear. Every pass below is O(cells) or O(columns),
 * and the rule that keeps it that way is worth stating because it is the one
 * the next change will break:
 *
 *   **column widths are derived only from min_w and fit[0], which the measure
 *   sweep already computed.** Nothing in the column loops may ask a cell to
 *   measure itself, because that turns one pass over the grid into one pass
 *   per column.
 * ------------------------------------------------------------------------ */
#include "ar_node.h"

/*
 * ponytail: sixty-four columns, and a table with more lays its extra columns
 * on top of the last one rather than growing the array. A financial report is
 * twenty columns and a spreadsheet export is rarely past forty; the ceiling is
 * here because the constraint arrays are stack locals in a codebase that does
 * not allocate, which is the same bargain AR_MAX_FLOATS makes. The upgrade is
 * a frame-arena side array sized from the cell count, the way c->frags is.
 */
#define AR_MAX_COLUMNS 64

typedef struct ar__col
{
    ar_i32 min, max;  /* content constraints, accumulated over the column */
    ar_i32 w, x;      /* what it got, and where it starts */
    ar_i32 span_left; /* rows still covered by a rowspan from above */

    /* The cell doing the covering, so its height can be settled on the last
       row it reaches rather than guessed on the first. */
    ar_i32 span_node;
    ar_i32 span_y;
    ar_i32 span_h;
    ar_i32 span_rows;
} ar__col;

static int ar__is_row(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_ROW;
}

static int ar__is_group(const ar_node *n)
{
    ar_i32 d = n->style.v[AR_P_DISPLAY];

    return d == AR_DISPLAY_TABLE_ROW_GROUP || d == AR_DISPLAY_TABLE_HEADER_GROUP ||
           d == AR_DISPLAY_TABLE_FOOTER_GROUP;
}

static int ar__is_cell(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_CELL;
}

int ar_is_table(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE;
}

/*
 * A box the table places rather than one that places itself.
 *
 * Rows and row groups hold cells whose rectangles the table has already
 * decided, so the ordinary sweep must leave them alone. It does not by
 * default: anything that is not a block falls through to the flex algorithm,
 * and flex laid the cells out again from scratch -- the table assigned three
 * columns and the row immediately overwrote them, which showed up as every
 * cell being zero wide with the row around them the right size.
 */
int ar_is_table_internal(const ar_node *n)
{
    ar_i32 d = n->style.v[AR_P_DISPLAY];

    return d == AR_DISPLAY_TABLE_ROW || d == AR_DISPLAY_TABLE_ROW_GROUP ||
           d == AR_DISPLAY_TABLE_HEADER_GROUP || d == AR_DISPLAY_TABLE_FOOTER_GROUP;
}

/* A cell is a block for everything inside it: the table gives it a rectangle
   and its contents flow in that rectangle like any other block's. */
int ar_is_table_cell(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_CELL;
}

/*
 * The rows of a table, in order, across row groups.
 *
 * A row may sit straight inside the table or inside a group, and a table may
 * mix the two. This hands back the next row after `prev` without building a
 * list, because building one would need somewhere to put it.
 */
static ar_i32 ar__next_row(const ar_node *nodes, ar_i32 table, ar_i32 prev)
{
    ar_i32 at, up;

    if (prev < 0)
    {
        at = nodes[table].first_child;
        while (at >= 0)
        {
            if (ar__is_row(&nodes[at]))
            {
                return at;
            }
            if (ar__is_group(&nodes[at]) && nodes[at].first_child >= 0)
            {
                ar_i32 r = nodes[at].first_child;

                while (r >= 0 && !ar__is_row(&nodes[r]))
                {
                    r = nodes[r].next_sibling;
                }
                if (r >= 0)
                {
                    return r;
                }
            }
            at = nodes[at].next_sibling;
        }
        return -1;
    }

    /* Another row in the same parent. */
    for (at = nodes[prev].next_sibling; at >= 0; at = nodes[at].next_sibling)
    {
        if (ar__is_row(&nodes[at]))
        {
            return at;
        }
    }

    /* Otherwise out of this group and on to the next thing in the table. */
    up = nodes[prev].parent;
    if (up < 0 || up == table)
    {
        return -1;
    }
    for (at = nodes[up].next_sibling; at >= 0; at = nodes[at].next_sibling)
    {
        if (ar__is_row(&nodes[at]))
        {
            return at;
        }
        if (ar__is_group(&nodes[at]))
        {
            ar_i32 r = nodes[at].first_child;

            while (r >= 0 && !ar__is_row(&nodes[r]))
            {
                r = nodes[r].next_sibling;
            }
            if (r >= 0)
            {
                return r;
            }
        }
    }
    return -1;
}

static ar_i32 ar__cell_span(const ar_node *n, ar_i32 prop)
{
    ar_i32 v = n->style.v[prop];

    return v < 1 ? 1 : v;
}

/*
 * What a cell adds around the numbers the measure sweep already produced.
 *
 * Only the border, and that is the whole point: `min_w` and `fit[0]` are built
 * by ar__measure_block as `widest + pad_left + pad_right`, so the padding is
 * already in them. Adding it again here made every column that much too wide,
 * and the tests missed it because none of them gave a cell any padding.
 */
static ar_i32 ar__cell_border_x(const ar_node *n)
{
    return 2 * n->style.v[AR_P_BORDER_WIDTH];
}

static ar_i32 ar__cell_border_y(const ar_node *n)
{
    return 2 * n->style.v[AR_P_BORDER_WIDTH];
}

/* A stated height is a content height, so it needs the padding the intrinsic
   one already carries before the two can be compared. */
static ar_i32 ar__cell_stated_h(const ar_node *n)
{
    if (n->style.unit[AR_P_HEIGHT] != AR_UNIT_PX)
    {
        return 0;
    }
    return n->style.v[AR_P_HEIGHT] + n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM] +
           ar__cell_border_y(n);
}

/*
 * a * b / d without overflowing, for numbers that are not as small as they
 * look.
 *
 * The obvious form carried a comment saying both factors were under the
 * sixteen-bit style ceiling. That was false: a column's maximum comes from
 * fit[0], which ar_style_clamp_narrow explicitly does not touch -- its own
 * comment says "nothing computed passes through here" -- so one long unbroken
 * string gives a column a max in the tens of thousands and the product leaves
 * ar_i32. Dividing first when it would costs a pixel of precision and is
 * always right, which is the better trade for a layout nobody can see.
 */
static ar_i32 ar__scale(ar_i32 a, ar_i32 b, ar_i32 d)
{
    if (a <= 0 || b <= 0 || d <= 0)
    {
        return 0;
    }
    if (a <= 2147483647 / b)
    {
        return a * b / d;
    }
    return (a / d) * b;
}

/*
 * Pass one: walk the grid, giving every cell a column, and accumulate what
 * each column needs from the cells that occupy exactly one.
 *
 * Returns the column count. `span_left` is the rowspan countdown: a cell that
 * spans three rows leaves its columns claimed for the two rows below it, which
 * is what stops the next row's first cell sliding underneath it.
 */
static ar_i32 ar__grid(const ar_node *nodes, ar_i32 table, ar__col *col)
{
    ar_i32 row = -1;
    ar_i32 ncol = 0;
    ar_i32 i;

    for (i = 0; i < AR_MAX_COLUMNS; ++i)
    {
        col[i].min = 0;
        col[i].max = 0;
        col[i].w = 0;
        col[i].x = 0;
        col[i].span_left = 0;
        col[i].span_node = -1;
        col[i].span_y = 0;
        col[i].span_h = 0;
        col[i].span_rows = 1;
    }

    while ((row = ar__next_row(nodes, table, row)) >= 0)
    {
        ar_i32 c = nodes[row].first_child;
        ar_i32 at = 0;

        for (; c >= 0; c = nodes[c].next_sibling)
        {
            ar_i32 cs, rs, j, chrome;

            if (!ar__is_cell(&nodes[c]) || nodes[c].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
            {
                continue;
            }

            while (at < AR_MAX_COLUMNS && col[at].span_left > 0)
            {
                ++at;
            }
            if (at >= AR_MAX_COLUMNS)
            {
                at = AR_MAX_COLUMNS - 1;
            }

            cs = ar__cell_span(&nodes[c], AR_P_COLSPAN);
            rs = ar__cell_span(&nodes[c], AR_P_ROWSPAN);
            if (at + cs > AR_MAX_COLUMNS)
            {
                cs = AR_MAX_COLUMNS - at;
            }

            chrome = ar__cell_border_x(&nodes[c]);

            if (cs == 1)
            {
                ar_i32 mn = nodes[c].min_w + chrome;
                ar_i32 mx = nodes[c].fit[0] + chrome;

                if (nodes[c].style.unit[AR_P_WIDTH] == AR_UNIT_PX)
                {
                    ar_i32 stated = nodes[c].style.v[AR_P_WIDTH] + chrome;

                    if (stated > mn)
                    {
                        mn = stated;
                    }
                    if (stated > mx)
                    {
                        mx = stated;
                    }
                }
                if (mn > col[at].min)
                {
                    col[at].min = mn;
                }
                if (mx > col[at].max)
                {
                    col[at].max = mx;
                }
            }

            /*
             * The countdown covers the row this cell is in, not just the ones
             * below it, because the decrement at the end of every row is what
             * moves it along. Storing rs - 1 here instead looked right and was
             * off by one row: the row that set the span cleared it again on
             * its way out, so the next row's first cell slid straight under a
             * cell that was still there.
             */
            /*
             * The longest claim wins.
             *
             * Assigning here released a column early: a cell spanning two
             * columns, one of which a three-row cell was still holding, wrote
             * its own single row over that three and the row below slid
             * underneath a cell that was still there. The same failure the
             * countdown was added to prevent, arriving from the other axis.
             */
            for (j = at; j < at + cs && j < AR_MAX_COLUMNS; ++j)
            {
                if (rs > col[j].span_left)
                {
                    col[j].span_left = rs;
                }
            }
            at += cs;
            if (at > ncol)
            {
                ncol = at;
            }
        }

        /*
         * Only the columns this table actually uses.
         *
         * This swept all sixty-four every row, which is linear in rows and
         * therefore not a complexity bug -- but it is sixty-four times the
         * work a four-column table needs, on every row, in four separate
         * passes. The ten-thousand-row scene is what made it worth saying: a
         * constant that large stops reading as a constant.
         */
        for (i = 0; i < ncol; ++i)
        {
            if (col[i].span_left > 0)
            {
                --col[i].span_left;
            }
        }
    }

    for (i = 0; i < ncol; ++i)
    {
        if (col[i].max < col[i].min)
        {
            col[i].max = col[i].min;
        }
    }
    return ncol;
}

/*
 * Pass two: cells that span more than one column.
 *
 * After the single-column constraints, never before: a wide spanning cell
 * would otherwise inflate a column that a narrow single cell had already
 * pinned, and the narrow cell is the one that knows what it needs.
 *
 * The deficit is shared out in proportion to what the spanned columns already
 * want, so a wide column takes more of it than a narrow one -- and equally
 * when they all want nothing, because there is nothing to be proportional to.
 */
static void ar__fold_spans(const ar_node *nodes, ar_i32 table, ar__col *col, ar_i32 ncol)
{
    ar_i32 row = -1;
    ar_i32 i;

    for (i = 0; i < ncol; ++i)
    {
        col[i].span_left = 0;
    }

    while ((row = ar__next_row(nodes, table, row)) >= 0)
    {
        ar_i32 c = nodes[row].first_child;
        ar_i32 at = 0;

        for (; c >= 0; c = nodes[c].next_sibling)
        {
            ar_i32 cs, rs, j, have_min = 0, have_max = 0, want_min, want_max, chrome;

            if (!ar__is_cell(&nodes[c]) || nodes[c].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
            {
                continue;
            }

            /*
             * This pass has to walk the grid the same way the other two do.
             *
             * It did not track the rowspan countdown at all, so in any table
             * with both a rowspan and a spanning cell it folded that cell's
             * width demand into the columns to the left of the ones it
             * actually occupies -- widening a column nobody asked to widen and
             * leaving the spanning cell too narrow.
             */
            while (at < ncol && col[at].span_left > 0)
            {
                ++at;
            }
            if (at >= ncol)
            {
                break;
            }

            cs = ar__cell_span(&nodes[c], AR_P_COLSPAN);
            rs = ar__cell_span(&nodes[c], AR_P_ROWSPAN);
            if (at + cs > ncol)
            {
                cs = ncol - at;
            }
            if (cs <= 1)
            {
                if (rs > col[at].span_left)
                {
                    col[at].span_left = rs;
                }
                at += 1;
                continue;
            }

            chrome = ar__cell_border_x(&nodes[c]);
            want_min = nodes[c].min_w + chrome;
            want_max = nodes[c].fit[0] + chrome;

            /* A stated width counts for a spanning cell too. Only the
               single-column pass honoured it, so `colspan:2; width:300px` on
               an empty cell widened nothing at all. */
            if (nodes[c].style.unit[AR_P_WIDTH] == AR_UNIT_PX)
            {
                ar_i32 stated = nodes[c].style.v[AR_P_WIDTH] + chrome;

                if (stated > want_min)
                {
                    want_min = stated;
                }
                if (stated > want_max)
                {
                    want_max = stated;
                }
            }

            for (j = at; j < at + cs; ++j)
            {
                have_min += col[j].min;
                have_max += col[j].max;
            }

            if (want_min > have_min)
            {
                ar_i32 extra = want_min - have_min;
                ar_i32 given = 0;

                for (j = at; j < at + cs; ++j)
                {
                    ar_i32 share =
                        (j == at + cs - 1)
                            ? extra - given
                            : (have_max > 0 ? extra * col[j].max / have_max : extra / cs);

                    col[j].min += share;
                    given += share;
                }
            }
            if (want_max > have_max)
            {
                ar_i32 extra = want_max - have_max;
                ar_i32 given = 0;

                for (j = at; j < at + cs; ++j)
                {
                    ar_i32 share =
                        (j == at + cs - 1)
                            ? extra - given
                            : (have_max > 0 ? extra * col[j].max / have_max : extra / cs);

                    col[j].max += share;
                    given += share;
                }
            }
            for (j = at; j < at + cs; ++j)
            {
                if (col[j].max < col[j].min)
                {
                    col[j].max = col[j].min;
                }
                if (rs > col[j].span_left)
                {
                    col[j].span_left = rs;
                }
            }
            at += cs;
        }

        for (at = 0; at < ncol; ++at)
        {
            if (col[at].span_left > 0)
            {
                --col[at].span_left;
            }
        }
    }
}

/*
 * Pass three: hand out the width the table actually has.
 *
 * Three cases, and the middle one is the whole algorithm: between the minimum
 * and the maximum, every column gets its minimum plus the same fraction of the
 * slack it asked for.
 *
 * The arithmetic is integer and the product is the part to watch. Both factors
 * are bounded by the sixteen-bit ceiling ar_style documents, so the numerator
 * reaches about 1.07e9 against a 2.1e9 limit -- inside, with less than twice
 * the margin, which is why it is computed into an explicit ar_i32 and said out
 * loud here. The remainder goes to the last column so the widths sum to the
 * space exactly rather than nearly.
 */
static void ar__distribute(ar__col *col, ar_i32 ncol, ar_i32 avail, int fixed_layout)
{
    ar_i32 sum_min = 0, sum_max = 0, i, given = 0;

    for (i = 0; i < ncol; ++i)
    {
        sum_min += col[i].min;
        sum_max += col[i].max;
    }

    if (fixed_layout)
    {
        /* Every column the same share, which is what `fixed` buys: one pass
           and no dependence on any cell past the first row. */
        for (i = 0; i < ncol; ++i)
        {
            col[i].w = (i == ncol - 1) ? avail - given : avail / ncol;
            given += col[i].w;
        }
    }
    else if (avail <= sum_min)
    {
        /* Not even the minimums fit. Every column takes its minimum and the
           table overflows, which is what a scroll container is for. */
        for (i = 0; i < ncol; ++i)
        {
            col[i].w = col[i].min;
        }
    }
    else if (avail >= sum_max)
    {
        /*
         * Everything fits, and there is room over.
         *
         * The surplus is shared equally rather than left on the floor. The
         * first version stopped at the maximums and gave three empty columns
         * nothing at all -- every cell in a table of empty cells came out zero
         * wide, because a column that wants nothing had nothing to be
         * proportional to. `sum_max <= sum_min` used to be folded into the
         * clause above, which is how it hid: an empty table satisfies it.
         */
        ar_i32 surplus = avail - sum_max;

        for (i = 0; i < ncol; ++i)
        {
            ar_i32 share = (i == ncol - 1) ? surplus - given : surplus / ncol;

            col[i].w = col[i].max + share;
            given += share;
        }
        given = 0;
    }
    else
    {
        ar_i32 slack = avail - sum_min;
        ar_i32 room = sum_max - sum_min;

        for (i = 0; i < ncol; ++i)
        {
            ar_i32 give;

            if (i == ncol - 1)
            {
                give = slack - given;
            }
            else
            {
                ar_i32 span = col[i].max - col[i].min;

                give = ar__scale(span, slack, room);
            }
            col[i].w = col[i].min + give;
            given += give;
        }
    }

    given = 0;
    for (i = 0; i < ncol; ++i)
    {
        col[i].x = given;
        given += col[i].w;
    }
}

/*
 * The height one cell comes to at the width it was given.
 *
 * ponytail: a cell's own text is re-measured at the settled width, and a cell
 * whose content is other boxes falls back to the max-content height the
 * measure sweep produced. That is right for the common cell -- text, or one
 * block of text -- and too tall for a cell whose children would themselves
 * wrap. Closing it needs a measure(subtree, width) entry point, which layout
 * does not have for anything: `ar__wrap_height` answers for one node. That
 * entry point is worth building for its own sake, since grid will want it too.
 */
static ar_i32 ar__cell_height(ar_node *n, ar_i32 inner_w, ar_layout_env *env)
{
    /*
     * A border box, from whichever of the two answers is larger.
     *
     * fit[1] is what the *content* comes to, padding included and border not;
     * a stated height is content only. `height: 20px` on an empty cell leaves
     * fit[1] at zero, so a table of empty cells came out with rows of no
     * height at all -- two different questions, and the row wants the larger
     * answer expressed in the same units.
     */
    ar_i32 stated = ar__cell_stated_h(n);
    ar_i32 h = n->fit[1] + ar__cell_border_y(n);

    if (stated > h)
    {
        h = stated;
    }

    if (n->text && env && env->wrap)
    {
        ar_i32 wrapped = env->wrap(env->ud, n, inner_w) + ar__cell_border_y(n);

        if (wrapped > h)
        {
            h = wrapped;
        }
    }
    return h;
}

/*
 * The whole solve, optionally writing the rectangles it works out.
 *
 * ponytail: called twice per table per frame -- once to answer "how tall are
 * you at this width" while the parent is stacking its children, and once to
 * place. Both are O(cells), so the cost is a constant factor rather than a
 * complexity change, and the alternative is a frame-arena side record keyed by
 * the table node. Worth doing when a second reader appears; not worth the
 * plumbing for one.
 */
static ar_i32 ar__table_solve(ar_node *nodes, ar_i32 table, ar_layout_env *env, int assign)
{
    ar__col  col[AR_MAX_COLUMNS];
    ar_i32   ncol, row, y, inner_w, avail;
    ar_node *t = &nodes[table];
    int      fixed_layout = t->style.v[AR_P_TABLE_LAYOUT] == AR_TABLE_LAYOUT_FIXED;
    ar_i32   pad_l = t->style.v[AR_P_PAD_LEFT];
    ar_i32   pad_t = t->style.v[AR_P_PAD_TOP];
    ar_i32   spacing = t->style.v[AR_P_BORDER_SPACING];

    ncol = ar__grid(nodes, table, col);
    if (ncol <= 0)
    {
        /*
         * A table with rows but no cells still has rows in the tree.
         *
         * Returning here without touching them leaves each one at the surface
         * origin -- a fresh node's rect is zeroed, so it is not stale, but
         * (0, 0) is not where an empty row is either, and hit testing and
         * ar_node_rect both read it. They collapse where the table's content
         * would have started.
         */
        if (assign)
        {
            ar_i32 e = nodes[table].first_child;

            for (; e >= 0; e = nodes[e].next_sibling)
            {
                if (ar__is_row(&nodes[e]) || ar__is_group(&nodes[e]))
                {
                    ar_i32 r;

                    nodes[e].rect.x = t->rect.x + pad_l;
                    nodes[e].rect.y = t->rect.y + pad_t;
                    nodes[e].rect.w = t->rect.w - pad_l - t->style.v[AR_P_PAD_RIGHT];
                    nodes[e].rect.h = 0;
                    for (r = nodes[e].first_child; r >= 0; r = nodes[r].next_sibling)
                    {
                        nodes[r].rect = nodes[e].rect;
                    }
                }
            }
        }
        return t->style.v[AR_P_PAD_TOP] + t->style.v[AR_P_PAD_BOTTOM];
    }
    if (!fixed_layout)
    {
        ar__fold_spans(nodes, table, col, ncol);
    }

    inner_w = t->rect.w - pad_l - t->style.v[AR_P_PAD_RIGHT];
    avail = inner_w - spacing * (ncol + 1);
    if (avail < 0)
    {
        avail = 0;
    }
    ar__distribute(col, ncol, avail, fixed_layout);

    /*
     * The placement walk has to assign columns exactly the way the grid pass
     * did, countdown and all.
     *
     * It did not, and the two quietly disagreed: the grid pass knew a rowspan
     * kept a column busy and the placement pass did not, so the row under a
     * spanning cell put its first cell in the column that cell was still
     * occupying. Everything looked right until a table had a rowspan in it.
     * The countdown is reset here because the grid pass ran it down to zero on
     * its way through.
     */
    for (row = 0; row < ncol; ++row)
    {
        col[row].span_left = 0;
        col[row].span_node = -1;
        col[row].span_y = 0;
        col[row].span_h = 0;
        col[row].span_rows = 1;
    }

    y = pad_t + spacing;
    row = -1;
    while ((row = ar__next_row(nodes, table, row)) >= 0)
    {
        ar_i32 c = nodes[row].first_child;
        ar_i32 at = 0;
        ar_i32 rh = 0;
        ar_i32 k;

        /* `rowspan: 9` on the second row of a two-row table covers the rows
           that exist and no more, so the last row is a settling point for
           every span still open, whatever its countdown says. */
        int last = ar__next_row(nodes, table, row) < 0;

        for (; c >= 0; c = nodes[c].next_sibling)
        {
            ar_i32 cs, w, j, h, rs;

            if (!ar__is_cell(&nodes[c]) || nodes[c].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
            {
                continue;
            }

            while (at < ncol && col[at].span_left > 0)
            {
                ++at;
            }
            if (at >= ncol)
            {
                at = ncol - 1;
            }

            cs = ar__cell_span(&nodes[c], AR_P_COLSPAN);
            rs = ar__cell_span(&nodes[c], AR_P_ROWSPAN);
            if (at + cs > ncol)
            {
                cs = ncol - at;
            }
            if (cs < 1)
            {
                cs = 1;
            }
            for (j = at; j < at + cs && j < ncol; ++j)
            {
                if (rs > col[j].span_left)
                {
                    col[j].span_left = rs;
                }
            }

            w = 0;
            for (j = at; j < at + cs && j < ncol; ++j)
            {
                w += col[j].w;
            }
            w += spacing * (cs - 1);

            if (assign)
            {
                nodes[c].rect.x = t->rect.x + pad_l + spacing + col[at < ncol ? at : ncol - 1].x +
                                  spacing * (at < ncol ? at : ncol - 1);
                nodes[c].rect.w = w;
            }

            {
                /* Padding and border can exceed a narrow column, and a
                   negative content width is not a question the text measurer
                   has an answer to. Every other caller of the wrap callback
                   guards this; this one did not. */
                ar_i32 inner = w - ar__cell_border_x(&nodes[c]) - nodes[c].style.v[AR_P_PAD_LEFT] -
                               nodes[c].style.v[AR_P_PAD_RIGHT];

                h = ar__cell_height(&nodes[c], inner < 0 ? 0 : inner, env);
            }

            if (rs == 1)
            {
                if (h > rh)
                {
                    rh = h;
                }
            }
            else if (at < ncol)
            {
                /*
                 * A cell that spans rows is settled on the last one it
                 * reaches, not the first.
                 *
                 * It used to be excluded from the row height and then handed
                 * its own row's height anyway, so a table whose first row was
                 * one `rowspan:2` cell came out as tall as the *second* row
                 * and the spanning cell had no height at all. Its share is
                 * spread over the rows it covers so the table is tall enough,
                 * and its rectangle is closed when the countdown runs out.
                 */
                ar_i32 share = h / rs;

                if (share > rh)
                {
                    rh = share;
                }
                col[at].span_node = c;
                col[at].span_y = y;
                col[at].span_h = h;
                col[at].span_rows = rs;
            }
            at += cs;
        }

        /*
         * A span that ends on this row takes what it is still owed.
         *
         * Spreading a spanning cell's height evenly over the rows it covers
         * gets the table close, but the rows below have content of their own
         * and may each come out taller or shorter than their share -- so the
         * total can still fall short, and a forward pass cannot go back and
         * grow the first row. The last row it reaches absorbs the difference
         * instead, which is one of the two things browsers do here and the
         * only one a single pass can do at all.
         */
        for (k = 0; k < ncol; ++k)
        {
            if ((col[k].span_left == 1 || last) && col[k].span_node >= 0)
            {
                ar_i32 covered = (y + rh) - col[k].span_y;

                if (covered < col[k].span_h)
                {
                    rh += col[k].span_h - covered;
                }
            }
        }

        if (assign)
        {
            nodes[row].rect.x = t->rect.x + pad_l;
            nodes[row].rect.y = t->rect.y + y;
            nodes[row].rect.w = inner_w;
            nodes[row].rect.h = rh;

            c = nodes[row].first_child;
            for (; c >= 0; c = nodes[c].next_sibling)
            {
                if (!ar__is_cell(&nodes[c]))
                {
                    continue;
                }
                nodes[c].rect.y = t->rect.y + y;

                /* A spanning cell's height is settled when its countdown ends,
                   so writing the row's height over it here would undo that. */
                if (ar__cell_span(&nodes[c], AR_P_ROWSPAN) == 1)
                {
                    nodes[c].rect.h = rh;
                }
            }
        }
        for (k = 0; k < ncol; ++k)
        {
            if (col[k].span_left > 0)
            {
                --col[k].span_left;
            }
            if ((col[k].span_left == 0 || last) && col[k].span_node >= 0)
            {
                if (assign)
                {
                    nodes[col[k].span_node].rect.h = (y + rh) - col[k].span_y;
                }
                col[k].span_node = -1;
            }
        }
        y += rh + spacing;
    }

    if (assign)
    {
        /* Row groups wrap their rows, so give each one the span of the rows it
           holds. A group with no rows collapses rather than floating. */
        ar_i32 g;

        for (g = nodes[table].first_child; g >= 0; g = nodes[g].next_sibling)
        {
            if (ar__is_group(&nodes[g]))
            {
                ar_i32 r = nodes[g].first_child;
                ar_i32 top = 0, bot = 0;
                int    any = 0;

                for (; r >= 0; r = nodes[r].next_sibling)
                {
                    if (!ar__is_row(&nodes[r]))
                    {
                        continue;
                    }
                    if (!any || nodes[r].rect.y < top)
                    {
                        top = nodes[r].rect.y;
                    }
                    if (!any || nodes[r].rect.y + nodes[r].rect.h > bot)
                    {
                        bot = nodes[r].rect.y + nodes[r].rect.h;
                    }
                    any = 1;
                }
                nodes[g].rect.x = t->rect.x + pad_l;
                nodes[g].rect.w = inner_w;
                nodes[g].rect.y = any ? top : t->rect.y + pad_t;
                nodes[g].rect.h = any ? bot - top : 0;
            }
        }
    }

    return y + t->style.v[AR_P_PAD_BOTTOM];
}

/*
 * The backward sweep's share: what this table needs, and what it would like.
 *
 * Only the column constraints, because no width exists yet. Everything above
 * the table then treats it as one box with a min-content and a max-content
 * width, which is what makes a table drop into block flow, flex flow and
 * shrink-to-fit without any of them knowing what a table is.
 */
void ar_table_measure(ar_node *nodes, ar_i32 table)
{
    ar__col  col[AR_MAX_COLUMNS];
    ar_i32   ncol, i, sum_min = 0, sum_max = 0;
    ar_node *t = &nodes[table];
    ar_i32   spacing = t->style.v[AR_P_BORDER_SPACING];
    ar_i32   chrome = t->style.v[AR_P_PAD_LEFT] + t->style.v[AR_P_PAD_RIGHT];

    ncol = ar__grid(nodes, table, col);
    if (ncol > 0 && t->style.v[AR_P_TABLE_LAYOUT] != AR_TABLE_LAYOUT_FIXED)
    {
        ar__fold_spans(nodes, table, col, ncol);
    }
    for (i = 0; i < ncol; ++i)
    {
        sum_min += col[i].min;
        sum_max += col[i].max;
    }
    chrome += spacing * (ncol + 1);

    t->min_w = sum_min + chrome;
    t->fit[0] = sum_max + chrome;

    /*
     * The intrinsic height, without running the whole solve to get it.
     *
     * It used to call ar__table_solve here, which meant a second grid pass and
     * a second walk of every cell purely to produce a number that
     * ar__wrap_height overwrites the moment the width is settled. The rows are
     * walked once instead and each takes its tallest cell's max-content
     * height, which is the same answer the solve would have given at an
     * unconstrained width -- and is what fit[1] means everywhere else.
     */
    {
        ar_i32 row = -1;
        ar_i32 h = t->style.v[AR_P_PAD_TOP] + t->style.v[AR_P_PAD_BOTTOM] + spacing;

        while ((row = ar__next_row(nodes, table, row)) >= 0)
        {
            ar_i32 c = nodes[row].first_child;
            ar_i32 rh = 0;

            for (; c >= 0; c = nodes[c].next_sibling)
            {
                ar_i32 ch;

                if (!ar__is_cell(&nodes[c]))
                {
                    continue;
                }
                ch = nodes[c].fit[1] + ar__cell_border_y(&nodes[c]);
                if (ar__cell_stated_h(&nodes[c]) > ch)
                {
                    ch = ar__cell_stated_h(&nodes[c]);
                }
                if (ch > rh)
                {
                    rh = ch;
                }
            }
            h += rh + spacing;
        }
        t->fit[1] = h;
    }
}

/* How tall this table comes to now that its width is settled. Called from the
   same place a paragraph's height is corrected, so a table stacks like any
   other box. */
ar_i32 ar_table_height(ar_node *nodes, ar_i32 table, ar_layout_env *env)
{
    return ar__table_solve(nodes, table, env, 0);
}

/* The forward sweep's share: the rectangles, from a grid that is already
   decided. Everything below a cell is laid out by the ordinary sweep that
   follows, at the width the table gave it. */
void ar_table_place(ar_node *nodes, ar_i32 table, ar_layout_env *env)
{
    ar__table_solve(nodes, table, env, 1);
}
