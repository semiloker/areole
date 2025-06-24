/*
 * areole - the grid formatting context
 * SPDX-License-Identifier: MIT
 *
 * CSS Grid: two-dimensional layout, where rows and columns line up across the
 * whole container rather than within one row of it.
 *
 * That is not a convenience over flexbox. Nested one-dimensional containers
 * give up exactly the alignment that makes a grid useful -- three cards in a
 * row can each have a title, a body and a footer, and only a grid makes the
 * three footers share a line when the bodies are different lengths.
 *
 * ------------------------------------------------------------------------
 * Track sizing, in the specification's own step order
 *
 * §11.5 is written as numbered steps and this follows them in order, with the
 * numbers in the comments, because the alternative is checking it against
 * intuition and intuition is wrong about `fr`. In outline:
 *
 *   1  every track starts at its base size and its growth limit
 *   2  items spanning one track contribute to that track's base
 *   3  items spanning several contribute what the ones they span cannot cover
 *   4  the leftover is distributed to the `fr` tracks
 *
 * Step 4 is the one that surprises people: an `fr` track is not "a share of
 * the container", it is "at least its contents, then a share of what is left".
 * `1fr` has a minimum of `auto`, so a track holding a long word is wider than
 * its share and every other `fr` track gets less. That is why the parser
 * writes every track out as a range -- the algorithm never sees a shorthand.
 *
 * ------------------------------------------------------------------------
 * The two fixed arrays
 *
 * Tracks and the occupancy grid are bounded and live on the stack, the same
 * bargain ar_float_ctx and the table's column array make. A grid wider than
 * AR_GRID_MAX tracks is laid out in the tracks it has and the rest of its
 * items land in the last one, which is a visible wrong answer rather than a
 * silent overrun -- and nobody writes a sixty-five column grid by hand.
 */
#include "ar_node.h"

#define AR_GRID_MAX  64
#define AR_GRID_ROWS 256
/*
 * How many items one grid may hold.
 *
 * A forty by forty grid is sixteen hundred boxes, and every one needs its
 * placement remembered between the column solve and the row solve -- so this
 * is twenty bytes an item of stack, twenty kilobytes at the cap. Beyond it the
 * remaining items are dropped rather than placed wrongly, which is a visible
 * missing box rather than a silent overrun.
 */
#define AR_GRID_ITEMS 1024

typedef struct ar__gtrack
{
    ar_i32 base;  /* the size the contents demand */
    ar_i32 limit; /* and the size it may not exceed */
    ar_i32 fr;    /* the flexible factor, in thousandths, or zero */
    ar_i32 pos;   /* where it ends up */
    ar_i32 size;  /* and how big */
    int    intrinsic_min;
    int    intrinsic_max;
} ar__gtrack;

/* Where one item sits, in lines. */
typedef struct ar__gplace
{
    ar_i32 col, col_span;
    ar_i32 row, row_span;
} ar__gplace;

int ar_is_grid(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] == AR_DISPLAY_GRID;
}

static int ar__grid_item(const ar_node *n)
{
    return n->style.v[AR_P_DISPLAY] != AR_DISPLAY_NONE && !ar_is_out_of_flow(n);
}

/*
 * A track's two ends, resolved against a definite container size.
 *
 * `intrinsic` says the end depends on the contents, which is what steps 2 and
 * 3 fill in and what step 4 must not overwrite. A percentage against an
 * indefinite container is intrinsic too, which is CSS's rule and is the one
 * that stops a grid inside an automatic height resolving percentages against
 * a number it has not worked out yet.
 */
static void ar__track_ends(const ar_track *t, ar_i32 avail, int definite, ar__gtrack *out)
{
    out->fr = 0;
    out->intrinsic_min = 0;
    out->intrinsic_max = 0;
    out->base = 0;
    out->limit = 2147483647;

    switch (t->min_u)
    {
    case AR_UNIT_PX:
        out->base = t->min_v;
        break;
    case AR_UNIT_PCT:
        if (definite)
        {
            out->base = avail * t->min_v / 100;
        }
        else
        {
            out->intrinsic_min = 1;
        }
        break;
    default: /* auto, min-content, max-content */
        out->intrinsic_min = 1;
        break;
    }

    switch (t->max_u)
    {
    case AR_UNIT_PX:
        out->limit = t->max_v;
        break;
    case AR_UNIT_PCT:
        if (definite)
        {
            out->limit = avail * t->max_v / 100;
        }
        else
        {
            out->intrinsic_max = 1;
        }
        break;
    case AR_UNIT_FR:
        /* An fr track's growth limit is its base until step 4 hands it space:
           it is flexible, not infinite, and treating it as infinite here is
           what makes an fr track swallow a row before the leftover is known. */
        out->fr = t->max_v;
        out->intrinsic_max = 1;
        break;
    default:
        out->intrinsic_max = 1;
        break;
    }
}

/*
 * The track list for one axis, or the fallback when none was written.
 *
 * A grid with no template on an axis has only implicit tracks, which take
 * their size from `grid-auto-rows` / `grid-auto-columns` -- and from `auto`
 * when those are silent too. Returning a count of zero here is not an error;
 * it is the ordinary case for `grid-auto-flow: row` with no row template.
 */
static ar_i32 ar__axis_tracks(const ar_sheet *sheet, const ar_node *n, ar_i32 axis,
                              const ar_track **out)
{
    ar_i32 count = 0;

    *out = ar_sheet_tracks(sheet, n->style.v[axis == 0 ? AR_P_GRID_COLS : AR_P_GRID_ROWS], &count);
    return count;
}

static const ar_track *ar__auto_track(const ar_sheet *sheet, const ar_node *n, ar_i32 axis)
{
    ar_i32          count = 0;
    const ar_track *t = ar_sheet_tracks(
        sheet, n->style.v[axis == 0 ? AR_P_GRID_AUTO_COLS : AR_P_GRID_AUTO_ROWS], &count);

    return count > 0 ? t : 0;
}

/* The line properties of one item on one axis, as (start, end). */
static void ar__item_lines(const ar_node *n, ar_i32 axis, ar_i32 *start, ar_i32 *end)
{
    *start = n->style.v[axis == 0 ? AR_P_GRID_COL_START : AR_P_GRID_ROW_START];
    *end = n->style.v[axis == 0 ? AR_P_GRID_COL_END : AR_P_GRID_ROW_END];
}

/*
 * §8.5: placement.
 *
 * Every item that named a line gets it; the rest are packed into the first
 * space that fits, in the flow direction. `dense` goes back to the beginning
 * each time and takes the first hole; sparse -- the default -- never goes
 * backwards, which is what keeps the visual order close to the source order.
 *
 * The occupancy grid is a bit per cell. It is the reason AR_GRID_ROWS exists:
 * an auto-placed item has to know what is already taken, and knowing that for
 * an unbounded number of rows means storing an unbounded number of rows.
 */
static void ar__place_items(ar_node *nodes, ar_i32 parent, ar__gplace *place, ar_i32 *item_index,
                            ar_i32 *out_items, ar_i32 explicit_cols, ar_i32 explicit_rows,
                            ar_i32 *out_cols, ar_i32 *out_rows, ar_i32 flow, unsigned char *taken)
{
    ar_i32 c;
    ar_i32 items = 0;
    ar_i32 cols = explicit_cols > 0 ? explicit_cols : 1;
    ar_i32 rows = 0;
    ar_i32 cursor_major = 0, cursor_minor = 0;
    int    column_flow = (flow & AR_GRID_FLOW_COLUMN) != 0;
    int    dense = (flow & AR_GRID_FLOW_DENSE) != 0;
    ar_i32 k;

    for (k = 0; k < AR_GRID_MAX * AR_GRID_ROWS / 8; ++k)
    {
        taken[k] = 0;
    }

    /* Pass one: everything that named a line, so the automatic ones pack
       around them rather than through them. */
    for (c = nodes[parent].first_child; c >= 0; c = nodes[c].next_sibling)
    {
        ar_i32 cs, ce, rs, re;
        ar_i32 i, j;

        if (!ar__grid_item(&nodes[c]))
        {
            continue;
        }
        if (items >= AR_GRID_ITEMS)
        {
            break;
        }
        item_index[items] = c;
        place[items].col = -1;
        place[items].row = -1;
        place[items].col_span = 1;
        place[items].row_span = 1;

        ar__item_lines(&nodes[c], 0, &cs, &ce);
        ar__item_lines(&nodes[c], 1, &rs, &re);

        if (cs > 0)
        {
            place[items].col = cs - 1;
            place[items].col_span = ce > cs ? ce - cs : (ce < 0 ? -ce : 1);
        }
        else if (cs < 0)
        {
            place[items].col_span = -cs;
        }
        if (ce < 0 && cs > 0)
        {
            place[items].col_span = -ce;
        }

        if (rs > 0)
        {
            place[items].row = rs - 1;
            place[items].row_span = re > rs ? re - rs : (re < 0 ? -re : 1);
        }
        else if (rs < 0)
        {
            place[items].row_span = -rs;
        }
        if (re < 0 && rs > 0)
        {
            place[items].row_span = -re;
        }

        if (place[items].col_span < 1)
        {
            place[items].col_span = 1;
        }
        if (place[items].row_span < 1)
        {
            place[items].row_span = 1;
        }
        if (place[items].col >= 0 && place[items].col + place[items].col_span > cols)
        {
            cols = place[items].col + place[items].col_span;
        }
        if (cols > AR_GRID_MAX)
        {
            cols = AR_GRID_MAX;
        }

        if (place[items].col >= 0 && place[items].row >= 0)
        {
            for (i = 0; i < place[items].row_span; ++i)
            {
                for (j = 0; j < place[items].col_span; ++j)
                {
                    ar_i32 r = place[items].row + i;
                    ar_i32 q = place[items].col + j;

                    if (r < AR_GRID_ROWS && q < AR_GRID_MAX)
                    {
                        ar_i32 bit = r * AR_GRID_MAX + q;

                        taken[bit >> 3] |= (unsigned char)(1u << (bit & 7));
                    }
                }
            }
            if (place[items].row + place[items].row_span > rows)
            {
                rows = place[items].row + place[items].row_span;
            }
        }
        ++items;
    }

    /* Pass two: the rest, packed into the first hole that fits. */
    for (k = 0; k < items; ++k)
    {
        ar_i32 major, minor;
        int    done = 0;

        if (place[k].col >= 0 && place[k].row >= 0)
        {
            continue;
        }

        if (dense)
        {
            cursor_major = 0;
            cursor_minor = 0;
        }

        for (major = cursor_major; major < AR_GRID_ROWS && !done; ++major)
        {
            ar_i32 span_minor = column_flow ? place[k].row_span : place[k].col_span;
            ar_i32 span_major = column_flow ? place[k].col_span : place[k].row_span;
            /*
             * How far along the minor axis a line may run before the item
             * moves to the next major one.
             *
             * In row flow that is the column count, which a template always
             * gives. In column flow it is the *row* count, and a grid with no
             * row template has no bound at all -- so it fills one endless
             * column, which is what `grid-auto-flow: column` means when
             * nothing says how tall the grid is.
             */
            ar_i32 limit = column_flow ? (explicit_rows > 0 ? explicit_rows : AR_GRID_ROWS) : cols;

            for (minor = (major == cursor_major ? cursor_minor : 0); minor + span_minor <= limit;
                 ++minor)
            {
                ar_i32 i, j;
                int    free_here = 1;

                /* A line the item named on one axis pins it there. */
                if (!column_flow && place[k].col >= 0 && minor != place[k].col)
                {
                    continue;
                }
                if (column_flow && place[k].row >= 0 && minor != place[k].row)
                {
                    continue;
                }

                for (i = 0; i < span_major && free_here; ++i)
                {
                    for (j = 0; j < span_minor; ++j)
                    {
                        ar_i32 r = column_flow ? minor + j : major + i;
                        ar_i32 q = column_flow ? major + i : minor + j;
                        ar_i32 bit;

                        if (r >= AR_GRID_ROWS || q >= AR_GRID_MAX)
                        {
                            free_here = 0;
                            break;
                        }
                        bit = r * AR_GRID_MAX + q;
                        if (taken[bit >> 3] & (unsigned char)(1u << (bit & 7)))
                        {
                            free_here = 0;
                            break;
                        }
                    }
                }
                if (!free_here)
                {
                    continue;
                }

                place[k].col = column_flow ? major : minor;
                place[k].row = column_flow ? minor : major;
                for (i = 0; i < place[k].row_span; ++i)
                {
                    for (j = 0; j < place[k].col_span; ++j)
                    {
                        ar_i32 r = place[k].row + i;
                        ar_i32 q = place[k].col + j;
                        ar_i32 bit;

                        if (r >= AR_GRID_ROWS || q >= AR_GRID_MAX)
                        {
                            continue;
                        }
                        bit = r * AR_GRID_MAX + q;
                        taken[bit >> 3] |= (unsigned char)(1u << (bit & 7));
                    }
                }
                if (!dense)
                {
                    cursor_major = major;
                    cursor_minor = minor + span_minor;
                }
                if (place[k].col + place[k].col_span > cols)
                {
                    cols = place[k].col + place[k].col_span;
                    if (cols > AR_GRID_MAX)
                    {
                        cols = AR_GRID_MAX;
                    }
                }
                if (place[k].row + place[k].row_span > rows)
                {
                    rows = place[k].row + place[k].row_span;
                }
                done = 1;
                break;
            }
        }
        if (!done)
        {
            /* Nowhere left inside the bounded grid. Put it in the last cell
               rather than nowhere: a visible wrong answer beats a box with no
               position at all. */
            place[k].col = cols > 0 ? cols - 1 : 0;
            place[k].row = rows > 0 ? rows - 1 : 0;
        }
    }

    *out_items = items;
    *out_cols = cols;
    *out_rows = rows > 0 ? rows : 1;
}

/*
 * An item's contribution on one axis: its min-content or its max-content.
 *
 * A stated size is a contribution too, and forgetting that is what made a grid
 * of empty boxes with `height: 40px` come out with rows of no height at all --
 * fit[1] is what the *contents* come to, and an empty box has none. The same
 * distinction the table solve had to make for a cell, in the same words.
 */
static ar_i32 ar__item_contribution(const ar_node *n, ar_i32 axis, int minimum)
{
    ar_i32 v = axis == 0 ? (minimum ? n->min_w : n->fit[0]) : n->fit[1];
    ar_i32 prop = ar_axis_size_prop(axis);

    if (n->style.unit[prop] == AR_UNIT_PX)
    {
        ar_i32 stated = ar_used_size(n, axis, n->style.v[prop]);

        if (stated > v)
        {
            v = stated;
        }
    }
    return v;
}

/*
 * §11.5 steps 1 to 3, on one axis.
 *
 * The tracks are already at their stated ends; what this adds is what the
 * items demand. Single-span items first, because a spanning item's demand is
 * only what the tracks it spans *cannot already* cover, and that cannot be
 * known until they have been told what the single-span items want.
 */
static void ar__size_tracks(ar_node *nodes, const ar__gplace *place, const ar_i32 *item_index,
                            ar_i32 items, ar__gtrack *tr, ar_i32 count, ar_i32 axis, ar_i32 gap)
{
    ar_i32 k, t;

    /* 2: items spanning exactly one track. */
    for (k = 0; k < items; ++k)
    {
        ar_i32 at = axis == 0 ? place[k].col : place[k].row;
        ar_i32 span = axis == 0 ? place[k].col_span : place[k].row_span;
        ar_i32 mn, mx;

        if (span != 1 || at < 0 || at >= count)
        {
            continue;
        }
        mn = ar__item_contribution(&nodes[item_index[k]], axis, 1);
        mx = ar__item_contribution(&nodes[item_index[k]], axis, 0);

        if (tr[at].intrinsic_min && mn > tr[at].base)
        {
            tr[at].base = mn;
        }
        if (tr[at].intrinsic_max && mx > tr[at].limit)
        {
            tr[at].limit = mx;
        }
    }

    /* Every base is at most its own limit, and every limit at least its base:
       `minmax(200px, 100px)` is a declaration people write and CSS says the
       maximum loses. */
    for (t = 0; t < count; ++t)
    {
        if (tr[t].intrinsic_max && tr[t].limit == 2147483647)
        {
            tr[t].limit = tr[t].base;
        }
        if (tr[t].limit < tr[t].base)
        {
            tr[t].limit = tr[t].base;
        }
    }

    /* 3: items spanning several. Only what the span cannot already cover, and
       spread evenly over the intrinsic tracks in it -- a definite track in the
       span is not asked to grow, because it already said how big it is. */
    for (k = 0; k < items; ++k)
    {
        ar_i32 at = axis == 0 ? place[k].col : place[k].row;
        ar_i32 span = axis == 0 ? place[k].col_span : place[k].row_span;
        ar_i32 have = 0, growable = 0, want, i;

        if (span <= 1 || at < 0 || at >= count)
        {
            continue;
        }
        if (at + span > count)
        {
            span = count - at;
        }
        for (i = 0; i < span; ++i)
        {
            have += tr[at + i].base;
            if (tr[at + i].intrinsic_min)
            {
                ++growable;
            }
        }
        have += gap * (span - 1);
        want = ar__item_contribution(&nodes[item_index[k]], axis, 1);
        if (want <= have || growable == 0)
        {
            continue;
        }
        {
            ar_i32 each = (want - have) / growable;
            ar_i32 rest = (want - have) % growable;

            for (i = 0; i < span; ++i)
            {
                if (!tr[at + i].intrinsic_min)
                {
                    continue;
                }
                tr[at + i].base += each;
                if (rest > 0)
                {
                    tr[at + i].base += 1;
                    --rest;
                }
                if (tr[at + i].limit < tr[at + i].base)
                {
                    tr[at + i].limit = tr[at + i].base;
                }
            }
        }
    }
}

/*
 * §11.7: the leftover goes to the fr tracks, in proportion to their factors.
 *
 * An fr track's base is its floor and never its answer: a track holding a word
 * wider than its share keeps the word and every other fr track gets less. That
 * is the rule people are surprised by, and it is why this loop iterates --
 * a track that will not shrink to its share has to be taken out of the
 * calculation and the rest divided again, exactly as flexbox freezes an item.
 */
static void ar__grow_fr(ar__gtrack *tr, ar_i32 count, ar_i32 space)
{
    ar_i32 guard;

    for (guard = 0; guard <= count; ++guard)
    {
        ar_i32 total = 0;
        ar_i32 free_space = space;
        ar_i32 flexible = 0;
        ar_i32 t;
        int    settled = 1;

        for (t = 0; t < count; ++t)
        {
            if (tr[t].fr > 0 && tr[t].size < 0)
            {
                total += tr[t].fr;
                ++flexible;
            }
            else
            {
                free_space -= tr[t].size >= 0 ? tr[t].size : tr[t].base;
            }
        }
        if (flexible == 0 || total <= 0)
        {
            break;
        }
        if (free_space < 0)
        {
            free_space = 0;
        }

        for (t = 0; t < count; ++t)
        {
            ar_i32 share;

            if (tr[t].fr <= 0 || tr[t].size >= 0)
            {
                continue;
            }
            share = total > 0 ? (ar_i32)((double)0) : 0;
            (void)share;

            /* free_space * fr / total without leaving ar_i32 on the way. */
            if (free_space <= 2147483647 / (tr[t].fr > 1 ? tr[t].fr : 1))
            {
                share = free_space * tr[t].fr / total;
            }
            else
            {
                share = (free_space / total) * tr[t].fr;
            }
            if (share < tr[t].base)
            {
                /* It will not shrink to its share. Take it out and divide the
                   rest again -- the same freeze flexbox does, for the same
                   reason: clamping is not distributive. */
                tr[t].size = tr[t].base;
                settled = 0;
            }
        }
        if (settled)
        {
            for (t = 0; t < count; ++t)
            {
                if (tr[t].fr > 0 && tr[t].size < 0)
                {
                    ar_i32 share;

                    if (free_space <= 2147483647 / (tr[t].fr > 1 ? tr[t].fr : 1))
                    {
                        share = free_space * tr[t].fr / total;
                    }
                    else
                    {
                        share = (free_space / total) * tr[t].fr;
                    }
                    tr[t].size = share;
                }
            }
            break;
        }
    }

    /* Anything still unsettled -- no free space, or no factor -- takes its
       base, which is what it demanded and the least it can be. */
    {
        ar_i32 t;

        for (t = 0; t < count; ++t)
        {
            if (tr[t].size < 0)
            {
                tr[t].size = tr[t].base;
            }
        }
    }
}

/* One axis, end to end: ends, contents, flex, and then the positions. */
static void ar__solve_axis(ar_node *nodes, const ar_sheet *sheet, ar_i32 parent,
                           const ar__gplace *place, const ar_i32 *item_index, ar_i32 items,
                           ar__gtrack *tr, ar_i32 count, ar_i32 axis, ar_i32 avail, int definite,
                           ar_i32 gap)
{
    const ar_node  *n = &nodes[parent];
    const ar_track *list;
    ar_i32          declared = ar__axis_tracks(sheet, n, axis, &list);
    const ar_track *autos = ar__auto_track(sheet, n, axis);
    ar_track        fallback;
    ar_i32          t;
    ar_i32          used = 0;

    /* An implicit track with nothing said about it is `auto`. */
    fallback.min_v = 0;
    fallback.max_v = 0;
    fallback.min_u = AR_UNIT_MIN_CONTENT;
    fallback.max_u = AR_UNIT_MAX_CONTENT;

    for (t = 0; t < count; ++t)
    {
        const ar_track *src = t < declared ? &list[t] : (autos ? autos : &fallback);

        ar__track_ends(src, avail, definite, &tr[t]);
        tr[t].size = -1;
    }

    ar__size_tracks(nodes, place, item_index, items, tr, count, axis, gap);

    /* Every non-flexible track takes its base, clamped to its limit, before
       the leftover is worked out -- the leftover is what is left after the
       tracks that are not competing for it have been paid. */
    for (t = 0; t < count; ++t)
    {
        if (tr[t].fr <= 0)
        {
            tr[t].size = tr[t].base > tr[t].limit ? tr[t].limit : tr[t].base;
        }
    }

    ar__grow_fr(tr, count, definite ? avail - gap * (count > 0 ? count - 1 : 0) : 0);

    for (t = 0; t < count; ++t)
    {
        tr[t].pos = used;
        used += tr[t].size + (t + 1 < count ? gap : 0);
    }
}

/* Where a box sits inside the space its tracks give it. */
static ar_i32 ar__self_mode(const ar_node *n, const ar_node *container, ar_i32 axis)
{
    ar_i32 self = axis == 0 ? n->style.v[AR_P_JUSTIFY_SELF] : n->style.v[AR_P_ALIGN_SELF];
    ar_i32 items =
        axis == 0 ? container->style.v[AR_P_JUSTIFY_ITEMS] : container->style.v[AR_P_ALIGN];

    return self == AR_ALIGN_AUTO ? items : self;
}

void ar_grid_place(ar_node *nodes, ar_i32 i, const ar_sheet *sheet, ar_layout_env *env)
{
    ar_node      *n = &nodes[i];
    ar__gtrack    col[AR_GRID_MAX];
    ar__gtrack    row[AR_GRID_ROWS > AR_GRID_MAX ? AR_GRID_MAX : AR_GRID_ROWS];
    ar__gplace    place[AR_GRID_ITEMS];
    ar_i32        index[AR_GRID_ITEMS];
    unsigned char taken[AR_GRID_MAX * AR_GRID_ROWS / 8];
    ar_i32        items = 0, cols = 0, rows = 0;
    ar_i32        inner_w, inner_h;
    ar_i32 col_gap = n->style.v[AR_P_COL_GAP] ? n->style.v[AR_P_COL_GAP] : n->style.v[AR_P_GAP];
    ar_i32 row_gap = n->style.v[AR_P_ROW_GAP] ? n->style.v[AR_P_ROW_GAP] : n->style.v[AR_P_GAP];
    ar_i32 declared_cols, declared_rows;
    const ar_track *list;
    ar_i32          k;

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

    declared_cols = ar__axis_tracks(sheet, n, 0, &list);
    declared_rows = ar__axis_tracks(sheet, n, 1, &list);
    ar__place_items(nodes, i, place, index, &items, declared_cols, declared_rows, &cols, &rows,
                    n->style.v[AR_P_GRID_FLOW], taken);
    if (declared_rows > rows)
    {
        rows = declared_rows;
    }
    if (cols > AR_GRID_MAX)
    {
        cols = AR_GRID_MAX;
    }
    if (rows > AR_GRID_MAX)
    {
        rows = AR_GRID_MAX;
    }

    /*
     * Columns before rows, and the order matters.
     *
     * An item's height depends on the width it ends up with -- that is what
     * text wrapping means -- so the rows cannot be sized until the columns
     * are, and every item has been given its column width and asked how tall
     * it came out. Sizing them together is what makes a grid of paragraphs
     * come out the wrong height.
     */
    ar__solve_axis(nodes, sheet, i, place, index, items, col, cols, 0, inner_w, 1, col_gap);

    for (k = 0; k < items; ++k)
    {
        ar_node *it = &nodes[index[k]];
        ar_i32   at = place[k].col;
        ar_i32   span = place[k].col_span;
        ar_i32   w = 0;
        ar_i32   j;

        for (j = 0; j < span && at + j < cols; ++j)
        {
            w += col[at + j].size;
        }
        w += col_gap * (span - 1 > 0 ? span - 1 : 0);
        it->rect.w = ar_resolve_size(it, 0, w, ar__self_mode(it, n, 0) == AR_ALIGN_STRETCH);
        /*
         * The automatic minimum applies to a grid item too.
         *
         * A track narrower than an item's unbreakable content does not crush
         * it -- the item overflows the track, exactly as a flex item overflows
         * its line. Same rule, same reason, and the same escape hatches: state
         * a minimum, or make the item clip.
         */
        if (!ar_pset_has(it->style.set, AR_P_MIN_WIDTH) &&
            it->style.unit[AR_P_WIDTH] != AR_UNIT_PX && ar_overflow_x(it) == AR_OVERFLOW_VISIBLE &&
            ar_overflow_y(it) == AR_OVERFLOW_VISIBLE && it->rect.w < it->min_w)
        {
            it->rect.w = it->min_w;
        }
        ar_wrap_height(nodes, it, 1, 0, env);
    }

    ar__solve_axis(nodes, sheet, i, place, index, items, row, rows, 1, inner_h,
                   n->style.unit[AR_P_HEIGHT] != AR_UNIT_AUTO, row_gap);

    for (k = 0; k < items; ++k)
    {
        ar_node *it = &nodes[index[k]];
        ar_i32   cw = 0, rh = 0, j;
        ar_i32   cx, cy;
        ar_i32   jm = ar__self_mode(it, n, 0);
        ar_i32   am = ar__self_mode(it, n, 1);

        for (j = 0; j < place[k].col_span && place[k].col + j < cols; ++j)
        {
            cw += col[place[k].col + j].size;
        }
        cw += col_gap * (place[k].col_span - 1 > 0 ? place[k].col_span - 1 : 0);
        for (j = 0; j < place[k].row_span && place[k].row + j < rows; ++j)
        {
            rh += row[place[k].row + j].size;
        }
        rh += row_gap * (place[k].row_span - 1 > 0 ? place[k].row_span - 1 : 0);

        cx = n->rect.x + n->style.v[AR_P_PAD_LEFT] +
             (place[k].col < cols ? col[place[k].col].pos : 0);
        cy = n->rect.y + n->style.v[AR_P_PAD_TOP] +
             (place[k].row < rows ? row[place[k].row].pos : 0);

        /*
         * The height, the same way the width was settled above.
         *
         * Only the stretch case was here at first, so an item that stated a
         * height of its own never got one at all -- every cell in a grid of
         * `height: 24px` boxes came out zero tall, which the tour page found
         * the moment it drew one. Stretch is a size like any other and belongs
         * in the same call rather than beside it.
         */
        it->rect.h = ar_resolve_size(it, 1, rh, am == AR_ALIGN_STRETCH);

        it->rect.x = cx + ar_align_self_offset(jm, cw - it->rect.w);
        it->rect.y = cy + ar_align_self_offset(am, rh - it->rect.h);
    }

    /* What the contents came to, for a scroll container and for an automatic
       height, the same as every other formatting context records. */
    {
        ar_i32 total = 0;

        for (k = 0; k < rows; ++k)
        {
            total += row[k].size + (k + 1 < rows ? row_gap : 0);
        }
        n->content_h = total + n->style.v[AR_P_PAD_TOP] + n->style.v[AR_P_PAD_BOTTOM];
    }
}

/* The height a grid comes to, for the sizing pass that runs before placement. */
ar_i32 ar_grid_content_height(ar_node *nodes, ar_i32 i, const ar_sheet *sheet, ar_layout_env *env)
{
    ar_grid_place(nodes, i, sheet, env);
    return nodes[i].content_h;
}
