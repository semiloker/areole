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
    ar_i32 min, max; /* content constraints, accumulated over the column */
    ar_i32 w, x;     /* what it got, and where it starts */
    /* Whether any cell in this column stated a width. A column that did keeps
       what it asked for when there is room to spare; the surplus goes to the
       columns that did not, which is where a browser puts it. */
    ar_i32 fixed;
    /* A `col` box asked for this width, and one that says `visibility:
       collapse` closes its column without any of the cells in it being
       touched. */
    ar_i32 stated;
    ar_i32 gone;
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

static int ar__is_caption(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_CAPTION;
}
static int ar__is_column(const ar_node *n)
{
    ar_i32 d = n->style.v[AR_P_DISPLAY];

    return d == AR_DISPLAY_TABLE_COLUMN || d == AR_DISPLAY_TABLE_COLUMN_GROUP;
}
/* A row or a cell that `visibility: collapse` has closed. Not the same as
   `display: none`: the track goes and the column widths do not move. */
static int ar__collapsed_out(const ar_node *n)
{
    return n->style.v[AR_P_VISIBILITY] == AR_VIS_COLLAPSE;
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
           d == AR_DISPLAY_TABLE_HEADER_GROUP || d == AR_DISPLAY_TABLE_FOOTER_GROUP ||
           d == AR_DISPLAY_TABLE_COLUMN || d == AR_DISPLAY_TABLE_COLUMN_GROUP;
}

/*
 * The table boxes that lay their contents out as a block.
 *
 * A cell and a caption are the same shape to everything above them: a
 * rectangle the table settles, holding a normal block flow. The rest of the
 * table -- rows, groups, columns -- has its geometry written by the solve and
 * must not be laid out again, which is what the predicate above is for.
 */
int ar_is_table_block(const ar_node *n)
{
    ar_i32 d = n->style.v[AR_P_DISPLAY];

    return d == AR_DISPLAY_TABLE_CELL || d == AR_DISPLAY_TABLE_CAPTION;
}

/* A cell is a block for everything inside it: the table gives it a rectangle
   and its contents flow in that rectangle like any other block's. */
int ar_is_table_cell(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_TABLE_CELL;
}

/*
 * Which pass a table-level child's rows belong to.
 *
 * A footer is written where it reads best and drawn where it belongs, which is
 * last -- so document order is not row order and cannot be, and an iterator
 * that walks siblings has to know it. Three passes over the table's children
 * settle it: headers, then everything else in the order it was written, then
 * footers. Bare rows are in the middle pass, so a `tfoot` declared before them
 * still ends up beneath them.
 */
static ar_i32 ar__phase_of(const ar_node *n)
{
    ar_i32 d = n->style.v[AR_P_DISPLAY];

    if (d == AR_DISPLAY_TABLE_HEADER_GROUP)
    {
        return 0;
    }
    if (d == AR_DISPLAY_TABLE_FOOTER_GROUP)
    {
        return 2;
    }
    return 1;
}

/* The first row at or after table-level child `at` that belongs to pass `p`. */
static ar_i32 ar__scan_rows(const ar_node *nodes, ar_i32 at, ar_i32 p)
{
    for (; at >= 0; at = nodes[at].next_sibling)
    {
        if (p == 1 && ar__is_row(&nodes[at]))
        {
            return at;
        }
        if (ar__is_group(&nodes[at]) && ar__phase_of(&nodes[at]) == p)
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

/*
 * The widest border anything in this row brings to the lines above and below.
 *
 * A horizontal grid line is shared by the row above it and the row below, so
 * it cannot be settled from one row alone. The row after this one is peeked at
 * instead of the answer being deferred a row -- two scans of each row's cells
 * over the whole table, which is linear, and it means a row's four edges are
 * all known inside its own iteration. A rowspan cell settling on this row then
 * gets its bottom edge exactly rather than approximately, which is where the
 * corner cases in collapsed borders were always going to be.
 */
static ar_i32 ar__row_border_max(const ar_node *nodes, ar_i32 row)
{
    ar_i32 c = nodes[row].first_child;
    ar_i32 m = nodes[row].style.v[AR_P_BORDER_WIDTH];

    if (nodes[row].parent >= 0 && ar__is_group(&nodes[nodes[row].parent]) &&
        nodes[nodes[row].parent].style.v[AR_P_BORDER_WIDTH] > m)
    {
        m = nodes[nodes[row].parent].style.v[AR_P_BORDER_WIDTH];
    }
    for (; c >= 0; c = nodes[c].next_sibling)
    {
        if (ar__is_cell(&nodes[c]) && nodes[c].style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE &&
            nodes[c].style.v[AR_P_BORDER_WIDTH] > m)
        {
            m = nodes[c].style.v[AR_P_BORDER_WIDTH];
        }
    }
    return m;
}

/*
 * The rows of a table, in the order they are drawn.
 *
 * No list is built: the whole solve runs off this, and materialising the rows
 * of a ten-thousand-row table into an array would be the one allocation the
 * engine does not have.
 */
static ar_i32 ar__next_row(const ar_node *nodes, ar_i32 table, ar_i32 prev)
{
    ar_i32 p, r, up;

    if (prev < 0)
    {
        for (p = 0; p < 3; ++p)
        {
            r = ar__scan_rows(nodes, nodes[table].first_child, p);
            if (r >= 0)
            {
                return r;
            }
        }
        return -1;
    }

    up = nodes[prev].parent;
    if (up == table)
    {
        /* A bare row: the table's own children carry on in document order,
           and a group among them contributes its rows where it stands. */
        p = 1;
        r = ar__scan_rows(nodes, nodes[prev].next_sibling, p);
        if (r >= 0)
        {
            return r;
        }
    }
    else
    {
        p = ar__phase_of(&nodes[up]);
        for (r = nodes[prev].next_sibling; r >= 0; r = nodes[r].next_sibling)
        {
            if (ar__is_row(&nodes[r]))
            {
                return r;
            }
        }
        r = ar__scan_rows(nodes, nodes[up].next_sibling, p);
        if (r >= 0)
        {
            return r;
        }
    }

    for (++p; p < 3; ++p)
    {
        r = ar__scan_rows(nodes, nodes[table].first_child, p);
        if (r >= 0)
        {
            return r;
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
    /*
     * A cell in a collapsed table has no border of its own to make room for.
     *
     * Its border became a grid line shared with the cell beside it, and the
     * lines are subtracted from the table's width once, all together, before
     * the columns are distributed. Counting the border here as well would
     * charge for it twice and make every column too wide by exactly the
     * borders that were supposed to have been collapsed away.
     */
    if (n->state & AR_STATE_COLLAPSED)
    {
        return 0;
    }
    return 2 * n->style.v[AR_P_BORDER_WIDTH];
}

static ar_i32 ar__cell_border_y(const ar_node *n)
{
    if (n->state & AR_STATE_COLLAPSED)
    {
        return 0;
    }
    return 2 * n->style.v[AR_P_BORDER_WIDTH];
}

/* The two halves a grid line of width `v` is split into. The box above or to
   the left takes the larger one, so a one-pixel line -- which is nearly every
   line anyone writes -- is drawn once, by one box, in one colour. */
static ar_i32 ar__half_near(ar_i32 v)
{
    return (v + 1) / 2;
}
static ar_i32 ar__half_far(ar_i32 v)
{
    return v / 2;
}
static int ar__collapsed(const ar_node *t)
{
    return (t->state & AR_STATE_COLLAPSED) != 0;
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
/*
 * Which vertical grid line is how wide, and how many columns there are.
 *
 * A separate walk before the constraints, because a cell's own width demand in
 * the collapsed model includes its half of the line at each of its ends -- and
 * the line at its right-hand end belongs as much to the cell in the row below
 * as to this one. Nothing about a single cell settles it, so it cannot be done
 * in the same pass that reads the cell.
 */
static ar_i32 ar__lines(const ar_node *nodes, ar_i32 table, ar_i32 *vline)
{
    ar_i32 row = -1;
    ar_i32 ncol = 0;
    ar_i32 outer = nodes[table].style.v[AR_P_BORDER_WIDTH];
    ar_i32 i;
    ar_i32 hold[AR_MAX_COLUMNS];

    for (i = 0; i <= AR_MAX_COLUMNS; ++i)
    {
        vline[i] = 0;
    }
    for (i = 0; i < AR_MAX_COLUMNS; ++i)
    {
        hold[i] = 0;
    }

    while ((row = ar__next_row(nodes, table, row)) >= 0)
    {
        ar_i32 c = nodes[row].first_child;
        ar_i32 at = 0;
        ar_i32 rb = ar__row_border_max(nodes, row);

        if (rb > outer)
        {
            outer = rb;
        }
        for (; c >= 0; c = nodes[c].next_sibling)
        {
            ar_i32 cs, rs, bw, j;

            if (!ar__is_cell(&nodes[c]) || nodes[c].style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
            {
                continue;
            }
            while (at < AR_MAX_COLUMNS && hold[at] > 0)
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
            bw = nodes[c].style.v[AR_P_BORDER_WIDTH];
            if (bw > vline[at])
            {
                vline[at] = bw;
            }
            if (bw > vline[at + cs])
            {
                vline[at + cs] = bw;
            }
            for (j = at; j < at + cs && j < AR_MAX_COLUMNS; ++j)
            {
                if (rs > hold[j])
                {
                    hold[j] = rs;
                }
            }
            at += cs;
            if (at > ncol)
            {
                ncol = at;
            }
        }
        for (i = 0; i < ncol; ++i)
        {
            if (hold[i] > 0)
            {
                --hold[i];
            }
        }
    }

    if (outer > vline[0])
    {
        vline[0] = outer;
    }
    if (ncol > 0 && outer > vline[ncol])
    {
        vline[ncol] = outer;
    }
    return ncol;
}

/*
 * What the `col` boxes say about the columns, before any cell is read.
 *
 * A `col` is the one place a column can be spoken about directly -- its width
 * settles the column whatever the cells in it want, and `visibility: collapse`
 * on it closes the column without any cell being named. Columns are counted
 * across the table's children in order: a `col` takes the next one, a
 * `colgroup` covers the `col`s inside it, or the next one if it has none.
 *
 * Returns how many columns the col boxes describe, which is not the table's
 * column count -- a table may have more columns than it has `col` elements,
 * and usually does.
 */
static ar_i32 ar__read_columns(const ar_node *nodes, ar_i32 table, ar__col *col)
{
    ar_i32 e, k = 0;

    for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
    {
        ar_i32 c2, span = 0;

        if (!ar__is_column(&nodes[e]))
        {
            continue;
        }
        for (c2 = nodes[e].first_child; c2 >= 0; c2 = nodes[c2].next_sibling)
        {
            if (!ar__is_column(&nodes[c2]))
            {
                continue;
            }
            if (k < AR_MAX_COLUMNS)
            {
                if (nodes[c2].style.unit[AR_P_WIDTH] == AR_UNIT_PX)
                {
                    col[k].stated = nodes[c2].style.v[AR_P_WIDTH];
                }
                else if (nodes[e].style.unit[AR_P_WIDTH] == AR_UNIT_PX)
                {
                    col[k].stated = nodes[e].style.v[AR_P_WIDTH];
                }
                if (ar__collapsed_out(&nodes[c2]) || ar__collapsed_out(&nodes[e]))
                {
                    col[k].gone = 1;
                }
            }
            ++k;
            ++span;
        }
        if (span == 0)
        {
            if (k < AR_MAX_COLUMNS)
            {
                if (nodes[e].style.unit[AR_P_WIDTH] == AR_UNIT_PX)
                {
                    col[k].stated = nodes[e].style.v[AR_P_WIDTH];
                }
                if (ar__collapsed_out(&nodes[e]))
                {
                    col[k].gone = 1;
                }
            }
            ++k;
        }
    }
    return k;
}

static ar_i32 ar__grid(const ar_node *nodes, ar_i32 table, ar__col *col, ar_i32 *vline)
{
    ar_i32 row = -1;
    ar_i32 ncol = 0;
    ar_i32 i;
    int    collapse = ar__collapsed(&nodes[table]);

    for (i = 0; i <= AR_MAX_COLUMNS; ++i)
    {
        vline[i] = 0;
    }
    if (collapse)
    {
        ar__lines(nodes, table, vline);
    }

    for (i = 0; i < AR_MAX_COLUMNS; ++i)
    {
        col[i].min = 0;
        col[i].max = 0;
        col[i].w = 0;
        col[i].x = 0;
        col[i].fixed = 0;
        col[i].stated = 0;
        col[i].gone = 0;
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

            if (collapse && vline)
            {
                /*
                 * Half the line at each end, which is what this cell's own box
                 * has to be wide enough to hold.
                 *
                 * The lines are not subtracted from the table's width -- the
                 * columns partition the whole of it and the lines straddle the
                 * boundaries between them. Taking the lines out *and* handing
                 * each cell its share back paid for them twice, which is what
                 * made every collapsed case in the corpus come out narrow.
                 */
                ar_i32 rk = at + cs <= AR_MAX_COLUMNS ? at + cs : AR_MAX_COLUMNS;

                chrome += ar__half_far(vline[at]) + ar__half_near(vline[rk]);
            }

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
                    col[at].fixed = 1;
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

    ar__read_columns(nodes, table, col);

    for (i = 0; i < ncol; ++i)
    {
        if (col[i].max < col[i].min)
        {
            col[i].max = col[i].min;
        }
        /*
         * A `col` that stated a width has said what its column wants.
         *
         * A bid rather than a settlement: a cell in that column that asked for
         * more still gets it, which is what a browser does and what the
         * cascade would lead anyone to expect -- the column speaks for the
         * cells that said nothing, not over the ones that did.
         */
        if (col[i].stated > col[i].min)
        {
            col[i].min = col[i].stated;
        }
        if (col[i].stated > col[i].max)
        {
            col[i].max = col[i].stated;
        }
        if (col[i].stated > 0)
        {
            col[i].fixed = 1;
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
static void ar__fold_spans(const ar_node *nodes, ar_i32 table, ar__col *col, ar_i32 ncol,
                           const ar_i32 *vline)
{
    ar_i32 row = -1;
    ar_i32 i;
    int    collapse = ar__collapsed(&nodes[table]);

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

            if (collapse && vline)
            {
                /*
                 * Half the line at each end, which is what this cell's own box
                 * has to be wide enough to hold.
                 *
                 * The lines are not subtracted from the table's width -- the
                 * columns partition the whole of it and the lines straddle the
                 * boundaries between them. Taking the lines out *and* handing
                 * each cell its share back paid for them twice, which is what
                 * made every collapsed case in the corpus come out narrow.
                 */
                ar_i32 rk = at + cs <= AR_MAX_COLUMNS ? at + cs : AR_MAX_COLUMNS;

                chrome += ar__half_far(vline[at]) + ar__half_near(vline[rk]);
            }
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
        /*
         * The minimums do not fit, and what happens next depends on where they
         * came from.
         *
         * A minimum that is a *stated* width is a request, and when the
         * requests come to more than the table has, they are scaled down in
         * proportion rather than granted and left to overflow -- four columns
         * asking for ninety in a table of three hundred and sixty come out
         * proportional, not overflowing by sixty.
         *
         * A minimum that is content -- a word that cannot be broken -- is not
         * a request and does not scale. So the scaling applies only to the
         * slack above the content minimums, and a table narrower than its own
         * text still overflows, which is what a scroll container is for.
         */
        ar_i32 hard = 0, soft = 0;

        for (i = 0; i < ncol; ++i)
        {
            if (col[i].fixed)
            {
                soft += col[i].min;
            }
            else
            {
                hard += col[i].min;
            }
        }
        if (soft > 0 && avail > hard)
        {
            /*
             * The stated widths share what is left of the table between them,
             * in proportion to what each asked for. The sum is deliberately
             * not forced back to `avail` afterwards: rounding four columns
             * down leaves a pixel or two unused, and that is what a browser
             * does -- forcing it would hand the whole remainder to the last
             * column and make it visibly wider than its neighbours.
             */
            for (i = 0; i < ncol; ++i)
            {
                col[i].w = col[i].fixed ? ar__scale(col[i].min, avail - hard, soft) : col[i].min;
            }
        }
        else
        {
            for (i = 0; i < ncol; ++i)
            {
                col[i].w = col[i].min;
            }
        }
    }
    else if (avail >= sum_max)
    {
        /*
         * Everything fits, and there is room over.
         *
         * The surplus goes to the columns in proportion to what they wanted,
         * so a column holding a paragraph takes more of it than a column
         * holding a date. Sharing it equally was the first version and is what
         * the specification's wording permits, but it is not what any browser
         * does, and this table is compared against one -- a wide column and a
         * narrow one in a roomy table came out nearly the same width.
         *
         * Equal shares survive as the fallback for the case that produced them:
         * a table of empty cells has nothing to be proportional to, and
         * stopping at the maximums there gave every column zero width.
         * `sum_max <= sum_min` used to be folded into the clause above, which
         * is how that hid -- an empty table satisfies it.
         */
        /*
         * A column that stated a width keeps it while any column did not.
         *
         * A cell holding a paragraph beside a cell holding a date is the case
         * this is for: the date said 40 pixels and meant it, and every pixel
         * over the total belongs to the paragraph. Only when every column has
         * stated a width is the surplus shared out among them, in proportion
         * to what each asked for -- a wide column taking more of it than a
         * narrow one, which sharing it equally got wrong.
         */
        ar_i32 surplus = avail - sum_max;
        ar_i32 pool = 0, last = -1;
        int    any_auto = 0;

        for (i = 0; i < ncol; ++i)
        {
            if (!col[i].fixed)
            {
                any_auto = 1;
                pool += col[i].max;
                last = i;
            }
        }
        if (!any_auto)
        {
            pool = sum_max;
            last = ncol - 1;
        }

        for (i = 0; i < ncol; ++i)
        {
            ar_i32 share = 0;

            if (any_auto && col[i].fixed)
            {
                share = 0;
            }
            else if (i == last)
            {
                share = surplus - given;
            }
            else if (pool > 0)
            {
                share = ar__scale(col[i].max, surplus, pool);
            }
            else
            {
                ar_i32 n = 0, k;

                for (k = 0; k < ncol; ++k)
                {
                    if (!any_auto || !col[k].fixed)
                    {
                        ++n;
                    }
                }
                share = n > 0 ? surplus / n : 0;
            }
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
    ar_i32   vline[AR_MAX_COLUMNS + 1];
    ar_i32   ncol, row, y, inner_w, avail;
    ar_node *t = &nodes[table];
    int      fixed_layout = t->style.v[AR_P_TABLE_LAYOUT] == AR_TABLE_LAYOUT_FIXED;
    int      collapse = ar__collapsed(t);
    ar_i32   pad_l = t->style.v[AR_P_PAD_LEFT];
    ar_i32   pad_t = t->style.v[AR_P_PAD_TOP];
    /* `border-spacing` is the separate model's whole mechanism and has no
       meaning in the collapsed one, where the gap between two cells is the
       shared line and nothing else. */
    ar_i32 spacing = collapse ? 0 : t->style.v[AR_P_BORDER_SPACING];
    ar_i32 prev_row = -1, prev_bot = 0;
    /* Where the grid itself begins and ends, which is not where the table
       does: a caption is a table-level box above or below every column. */
    ar_i32 grid_top = 0, grid_bot = 0;

    ncol = ar__grid(nodes, table, col, vline);
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
                if (ar__is_row(&nodes[e]) || ar__is_group(&nodes[e]) || ar__is_column(&nodes[e]))
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
        ar__fold_spans(nodes, table, col, ncol, vline);
    }

    inner_w = t->rect.w - pad_l - t->style.v[AR_P_PAD_RIGHT];
    /*
     * A collapsed line is not subtracted here.
     *
     * The columns partition the whole content width and the lines straddle the
     * boundaries between them -- each cell's own demand already carries its
     * half of the line at either end, folded in by ar__grid. Taking them out
     * here as well paid for them twice, and made every collapsed table in the
     * corpus come out narrower than the browser's by the width of its borders.
     */
    avail = inner_w - spacing * (ncol + 1);
    if (avail < 0)
    {
        avail = 0;
    }
    ar__distribute(col, ncol, avail, fixed_layout);
    {
        /*
         * A closed column takes no width, and nothing else moves to take it.
         *
         * That is the whole of `visibility: collapse` on a column: the widths
         * were solved with the column in place and are left exactly as they
         * were, so the table gets narrower by that column and every other one
         * stays where it was. Recomputing would make the remaining columns
         * jump, which is what the value exists to avoid.
         */
        ar_i32 k, acc = 0, lost = 0;

        for (k = 0; k < ncol; ++k)
        {
            if (col[k].gone)
            {
                lost += col[k].w;
                col[k].w = 0;
            }
            col[k].x = acc;
            acc += col[k].w;
        }

        /*
         * And the table is narrower by exactly what was closed.
         *
         * Even when its width was stated: `width: 360px` on a table with a
         * closed column means 360 for the columns that are left plus one that
         * is not there, and a browser drops it. The remaining columns keep the
         * widths they were given, which is the point -- the table gets smaller
         * rather than the columns getting bigger.
         */
        if (lost > 0)
        {
            inner_w -= lost;
            if (assign)
            {
                t->rect.w -= lost;
            }
        }
    }

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

    y = pad_t;

    /*
     * The caption sits above the grid and is as wide as the table.
     *
     * It is a table-level box that is not part of the grid at all: it takes no
     * column, contributes to no row, and the rows simply start beneath it. Its
     * own contents are laid out by the block pass afterwards, exactly as a
     * cell's are, which is why ar_is_table_block covers both.
     *
     * A column or a column group is dealt with after the rows, because what it
     * covers is not known until they have been placed.
     */
    {
        ar_i32 e;

        for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
        {
            if (ar__is_caption(&nodes[e]) && nodes[e].style.v[AR_P_CAPTION_SIDE] == AR_CAPTION_TOP)
            {
                ar_i32 cw = inner_w - ar__cell_border_x(&nodes[e]) -
                            nodes[e].style.v[AR_P_PAD_LEFT] - nodes[e].style.v[AR_P_PAD_RIGHT];
                ar_i32 ch = ar__cell_height(&nodes[e], cw < 0 ? 0 : cw, env);

                if (assign)
                {
                    nodes[e].rect.x = t->rect.x + pad_l;
                    nodes[e].rect.y = t->rect.y + y;
                    nodes[e].rect.w = inner_w;
                    nodes[e].rect.h = ch;
                }
                y += ch;
            }
        }
    }

    y += spacing;
    grid_top = y;

    /*
     * The row after this one is wanted twice -- once to know whether this is
     * the last row, and once for the border it brings to the line between them
     * -- and it is the next row anyway. Asking ar__next_row for it a second
     * time doubled the cost of walking a ten-thousand-row table for an answer
     * already in hand, so it is carried instead.
     */
    row = ar__next_row(nodes, table, -1);
    while (row >= 0)
    {
        ar_i32 c = nodes[row].first_child;
        ar_i32 at = 0;
        ar_i32 rh = 0;
        ar_i32 k;

        ar_i32 nxt = ar__next_row(nodes, table, row);
        ar_i32 mine = 0, hl = 0, hb = 0;
        int    closed;

        /* `rowspan: 9` on the second row of a two-row table covers the rows
           that exist and no more, so the last row is a settling point for
           every span still open, whatever its countdown says. */
        int last = nxt < 0;

        /*
         * A closed row takes no height and the rows below it close up.
         *
         * Its cells are placed exactly as any other row's -- same column, same
         * width -- and given no height. They were read by the grid pass and are
         * still in the column constraints, which is the whole difference from
         * `display: none`: a filter that hides half a table's rows leaves every
         * column exactly where it was, and the reader's eye does not have to
         * find them again.
         */
        closed = ar__collapsed_out(&nodes[row]);

        if (collapse)
        {
            mine = ar__row_border_max(nodes, row);
            hl = mine > prev_bot ? mine : prev_bot;
            hb = last ? t->style.v[AR_P_BORDER_WIDTH] : ar__row_border_max(nodes, nxt);
            if (mine > hb)
            {
                hb = mine;
            }
            if (prev_row < 0 && t->style.v[AR_P_BORDER_WIDTH] > hl)
            {
                hl = t->style.v[AR_P_BORDER_WIDTH];
            }
            /* Only the half that is inside the table. The other half of the
               first line lies above the content box, as a browser puts it. */
            y += prev_row < 0 ? ar__half_far(hl) : hl;
        }

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

            if (collapse)
            {
                /*
                 * The cell's box is its columns, and the lines are inside it.
                 *
                 * Every line is split: the box on the left or above takes the
                 * larger half, so a one-pixel line -- which is nearly every
                 * line anyone writes -- is drawn once, by one box, in one
                 * colour. The outer lines are split the same way, which leaves
                 * their far halves outside the table's content box, exactly
                 * where a browser puts them.
                 */
                ar_i32 rk = at + cs <= ncol ? at + cs : ncol;

                if (assign)
                {
                    nodes[c].rect.x = t->rect.x + pad_l + col[at < ncol ? at : ncol - 1].x;
                    nodes[c].rect.w = w;
                    nodes[c].edge[3] = (ar_u8)ar__half_far(vline[at]);
                    nodes[c].edge[1] = (ar_u8)ar__half_near(vline[rk]);
                    nodes[c].edge[0] = (ar_u8)ar__half_far(hl);
                    nodes[c].edge[2] = (ar_u8)ar__half_near(hb);
                }
            }
            else if (assign)
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

            if (closed)
            {
                /* Placed, measured, and contributing nothing. */
            }
            else if (rs == 1)
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
                col[at].span_y = y - ar__half_far(hl);
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
            ar_i32 top = ar__half_far(hl);
            ar_i32 bot = ar__half_near(hb);

            /* A row's box spans its cells, not the table -- so in the
               separate model it starts one border-spacing in and is two
               narrower, which is where a browser puts it. */
            nodes[row].rect.x = t->rect.x + pad_l + spacing;
            nodes[row].rect.y = t->rect.y + y;
            nodes[row].rect.w = inner_w - 2 * spacing;
            nodes[row].rect.h = rh;

            c = nodes[row].first_child;
            for (; c >= 0; c = nodes[c].next_sibling)
            {
                if (!ar__is_cell(&nodes[c]))
                {
                    continue;
                }

                /* The band is what the row's content occupies; a collapsed
                   cell starts half a line above it and ends half a line below,
                   which is the whole difference between the two models. */
                nodes[c].rect.y = t->rect.y + y - top;

                /* A spanning cell's height is settled when its countdown ends,
                   so writing the row's height over it here would undo that. */
                if (ar__cell_span(&nodes[c], AR_P_ROWSPAN) == 1)
                {
                    nodes[c].rect.h = closed ? 0 : rh + top + bot;
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
                    /* The band's bottom, plus this row's share of the line
                       under it -- the span started half a line above its own
                       first band, so both ends have to be paid for. */
                    ar_i32 sb = ar__half_near(hb);

                    nodes[col[k].span_node].rect.h = (y + rh + sb) - col[k].span_y;
                    nodes[col[k].span_node].edge[2] = (ar_u8)sb;
                }
                col[k].span_node = -1;
            }
        }
        y += rh + spacing;
        if (collapse)
        {
            prev_bot = mine;
            prev_row = row;
            if (last)
            {
                /* Nothing follows to open the last line, so the table closes
                   it here -- and again only with the half that is inside. */
                y += ar__half_near(hb);
            }
        }
        row = nxt;
    }

    grid_bot = y;

    {
        /* And the ones that asked to go underneath, after the last row. */
        ar_i32 e;

        for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
        {
            if (ar__is_caption(&nodes[e]) &&
                nodes[e].style.v[AR_P_CAPTION_SIDE] == AR_CAPTION_BOTTOM)
            {
                ar_i32 cw = inner_w - ar__cell_border_x(&nodes[e]) -
                            nodes[e].style.v[AR_P_PAD_LEFT] - nodes[e].style.v[AR_P_PAD_RIGHT];
                ar_i32 ch = ar__cell_height(&nodes[e], cw < 0 ? 0 : cw, env);

                if (assign)
                {
                    nodes[e].rect.x = t->rect.x + pad_l;
                    nodes[e].rect.y = t->rect.y + y;
                    nodes[e].rect.w = inner_w;
                    nodes[e].rect.h = ch;
                }
                y += ch;
            }
        }
    }

    if (assign)
    {
        /*
         * A column box covers the column it describes.
         *
         * It draws no border and holds nothing, but it is where a background
         * for a whole column is written, so it has to be the shape of that
         * column -- and it is the one box in a table whose geometry comes from
         * neither its parent nor its children. Columns are counted across the
         * table's children in order: a `col` takes the next one, a `colgroup`
         * takes the span of the `col`s inside it, or the next one if it has
         * none.
         */
        ar_i32 e, k = 0;
        ar_i32 gtop = t->rect.y + grid_top;
        ar_i32 gbot = t->rect.y + grid_bot;

        for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
        {
            ar_i32 from, span, c2;

            if (!ar__is_column(&nodes[e]))
            {
                continue;
            }
            from = k;
            span = 0;
            for (c2 = nodes[e].first_child; c2 >= 0; c2 = nodes[c2].next_sibling)
            {
                if (ar__is_column(&nodes[c2]))
                {
                    if (k < ncol)
                    {
                        /* A closed column has no box either: it is not a thin
                           column, it is a column that is not there. */
                        nodes[c2].rect = ar_rect_make(t->rect.x + pad_l + col[k].x, gtop, col[k].w,
                                                      col[k].gone ? 0 : gbot - gtop);
                    }
                    else
                    {
                        nodes[c2].rect = ar_rect_make(t->rect.x + pad_l, gtop, 0, 0);
                    }
                    ++k;
                    ++span;
                }
            }
            if (span == 0)
            {
                ++k;
                span = 1;
            }
            if (from < ncol)
            {
                ar_i32 to = from + span - 1;
                ar_i32 right;

                if (to >= ncol)
                {
                    to = ncol - 1;
                }
                right = col[to].x + col[to].w;
                nodes[e].rect = ar_rect_make(
                    t->rect.x + pad_l + col[from].x, right > col[from].x ? gtop : t->rect.y + pad_t,
                    right - col[from].x, right > col[from].x ? gbot - gtop : 0);
            }
            else
            {
                nodes[e].rect = ar_rect_make(t->rect.x + pad_l, gtop, 0, 0);
            }

            /*
             * And the columns inside it take its vertical extent.
             *
             * A group whose every column is closed has none, so its columns
             * report the table's corner rather than the grid's -- which is
             * where a browser puts them, and the only place a box with no size
             * inside a group with no size can sensibly say it is. A single
             * closed column inside a group that still has extent keeps the
             * grid's top, because the group it belongs to still does.
             */
            for (c2 = nodes[e].first_child; c2 >= 0; c2 = nodes[c2].next_sibling)
            {
                if (ar__is_column(&nodes[c2]))
                {
                    nodes[c2].rect.y = nodes[e].rect.y;
                    if (nodes[e].rect.h == 0)
                    {
                        nodes[c2].rect.h = 0;
                    }
                }
            }
        }
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
                nodes[g].rect.x = t->rect.x + pad_l + spacing;
                nodes[g].rect.w = inner_w - 2 * spacing;
                nodes[g].rect.y = any ? top : t->rect.y + pad_t;
                nodes[g].rect.h = any ? bot - top : 0;
            }
        }
    }

    return y + t->style.v[AR_P_PAD_BOTTOM];
}

/*
 * Whether a box paints itself at all.
 *
 * Two reasons it might not. `visibility: hidden` is the plain one, and it
 * covers this box only -- the property inherits, so the children arrive at the
 * same answer on their own, and a child that says `visible` comes back, which
 * is the difference from `display: none`.
 *
 * `empty-cells: hide` is the table one: a cell with nothing in it shows
 * neither background nor border, so a sparse table reads as a grid with holes
 * rather than a grid of empty boxes. Only in the separate model -- a collapsed
 * grid line belongs to the boundary and not to either cell, so there is no
 * such thing as one cell withholding it.
 */
int ar_box_paints(const ar_node *n)
{
    if (n->style.v[AR_P_VISIBILITY] != AR_VIS_VISIBLE)
    {
        return 0;
    }
    if (n->style.v[AR_P_EMPTY_CELLS] == AR_EMPTY_HIDE && ar_is_table_cell(n) &&
        !(n->state & AR_STATE_COLLAPSED) && n->first_child < 0 && !n->text)
    {
        return 0;
    }
    return 1;
}

/* Every box under this one moves with it. Walked through the child links
   rather than the node array, so a cell costs its own subtree and not the
   whole tree after it -- which on a ten-thousand-row table is the difference
   between linear and not. */
static void ar__shift_kids(ar_node *nodes, ar_i32 i, ar_i32 dy)
{
    ar_i32 c;

    for (c = nodes[i].first_child; c >= 0; c = nodes[c].next_sibling)
    {
        nodes[c].rect.y += dy;
        ar__shift_kids(nodes, c, dy);
    }
}

/*
 * Where a cell's contents sit in a cell that is taller than they are.
 *
 * A row is as tall as its tallest cell, so every other cell in it has room to
 * spare, and `vertical-align` is what says where in that room the contents go.
 * Run after the block pass has laid them out, because the answer needs the
 * height they came to and that is what the block pass produces.
 *
 * `baseline` -- the initial value -- is treated as `top`. Aligning the first
 * lines of adjacent cells needs each cell's own baseline and the tallest of
 * them across the row, which is a second pass over the row after its height is
 * settled; where every cell has the same font and the same padding the two
 * answers are identical, which is most tables. Named here so its absence is a
 * decision.
 */
void ar_table_align_cell(ar_node *nodes, ar_i32 i)
{
    ar_node *n = &nodes[i];
    ar_i32   va = n->style.v[AR_P_VERTICAL_ALIGN];
    ar_i32   inner, slack;

    if (va != AR_VALIGN_MIDDLE && va != AR_VALIGN_BOTTOM)
    {
        return;
    }
    inner = n->rect.h - n->style.v[AR_P_PAD_TOP] - n->style.v[AR_P_PAD_BOTTOM];
    if (n->state & AR_STATE_COLLAPSED)
    {
        inner -= n->edge[0] + n->edge[2];
    }
    else
    {
        inner -= ar__cell_border_y(n);
    }
    slack = inner - n->content_h;
    if (slack <= 0)
    {
        return;
    }
    ar__shift_kids(nodes, i, va == AR_VALIGN_MIDDLE ? slack / 2 : slack);
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
    ar_i32   vline[AR_MAX_COLUMNS + 1];
    ar_i32   ncol, i, sum_min = 0, sum_max = 0;
    ar_node *t = &nodes[table];
    ar_i32   spacing = t->style.v[AR_P_BORDER_SPACING];
    ar_i32   chrome = t->style.v[AR_P_PAD_LEFT] + t->style.v[AR_P_PAD_RIGHT];

    ar_i32 cap_min = 0, cap_max = 0;
    ar_i32 e;

    /* A caption is not in the grid, but the table has to be wide enough to
       hold it -- so its two widths join the column sums rather than being
       distributed among them. */
    for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
    {
        if (ar__is_caption(&nodes[e]))
        {
            if (nodes[e].min_w > cap_min)
            {
                cap_min = nodes[e].min_w;
            }
            if (nodes[e].fit[0] > cap_max)
            {
                cap_max = nodes[e].fit[0];
            }
        }
    }

    ncol = ar__grid(nodes, table, col, vline);
    if (ncol > 0 && t->style.v[AR_P_TABLE_LAYOUT] != AR_TABLE_LAYOUT_FIXED)
    {
        ar__fold_spans(nodes, table, col, ncol, vline);
    }
    for (i = 0; i < ncol; ++i)
    {
        sum_min += col[i].min;
        sum_max += col[i].max;
    }
    chrome += spacing * (ncol + 1);

    t->min_w = sum_min + chrome;
    t->fit[0] = sum_max + chrome;
    if (cap_min + chrome > t->min_w)
    {
        t->min_w = cap_min + chrome;
    }
    if (cap_max + chrome > t->fit[0])
    {
        t->fit[0] = cap_max + chrome;
    }

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

        for (e = nodes[table].first_child; e >= 0; e = nodes[e].next_sibling)
        {
            if (ar__is_caption(&nodes[e]))
            {
                h += nodes[e].fit[1];
            }
        }

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
