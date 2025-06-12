/*
 * areole - damage tracking.
 * SPDX-License-Identifier: MIT
 *
 * Why this exists:
 *
 * A Pentium II with PC100 memory sustains around 250 MB/s of write bandwidth.
 * An 800x600 surface at 32 bits per pixel is 1.83 MB, so writing it once costs
 * 7.7 ms out of a 16.7 ms frame, and a real interface has two to three times
 * overdraw. Full-window redraw on that machine is not slow, it is
 * arithmetically impossible.
 *
 * Why it holds several rectangles rather than one:
 *
 * A single merged rectangle was the plan, and on every realistic scene it is
 * enough -- the whole scene library repaints under 15% of the surface and not
 * one frame of it degenerates. But the degenerate case is not exotic. Two boxes
 * in opposite corners, one changing, is a status bar and a clock; the merged
 * rectangle is then the entire window, and the measurement is unambiguous:
 *
 *   pixels that changed              768
 *   pixels a merged rect presents    480,000     625x too many
 *   cost on the reference machine    0.11 ms
 *   cost on a Pentium II             7.68 ms     46% of the frame budget
 *
 * The roadmap answered this with a command list and a hash grid, which is two
 * new subsystems. Measuring it first showed that is not what the case needs.
 * The rasterizer already paints only the boxes it was asked for, so an oversized
 * damage rectangle costs almost nothing to draw -- the 7.68 ms is entirely the
 * blit. Keeping a handful of rectangles instead of one fixes that in fifty
 * lines, with no new architecture and nothing for a later release to unpick.
 *
 * Eight is not a tuned number. It is where merging starts costing less than
 * tracking, for a list small enough to scan linearly. If an interface is ever
 * found that wants more, the measurement above is the one that justifies it.
 */
#include "ar_node.h"

static ar_i32 ar__area(ar_rect r)
{
    if (r.w <= 0 || r.h <= 0)
    {
        return 0;
    }
    return r.w * r.h;
}

/* How many pixels are presented needlessly by merging these two. */
static ar_i32 ar__merge_waste(ar_rect a, ar_rect b)
{
    return ar__area(ar_rect_union(a, b)) - ar__area(a) - ar__area(b);
}

/* Past a certain point, tracking regions stops being worth the arithmetic:
   once the damage covers half the surface, presenting all of it is cheaper
   than deciding which parts to skip, and every later add becomes free. This is
   what keeps a tree that rebuilds itself entirely from paying per box for a
   decision that is already made. */
static void ar__collapse_if_pointless(ar_damage *d)
{
    if (d->viewport_area > 0 && d->area * 2 >= d->viewport_area)
    {
        d->all = 1;
        d->count = 0;
    }
}

void ar_damage_reset(ar_damage *d)
{
    d->count = 0;
    d->all = 0;
    d->area = 0;
}

void ar_damage_set_viewport(ar_damage *d, ar_rect viewport)
{
    d->viewport_area = ar__area(viewport);
}

void ar_damage_add_all(ar_damage *d)
{
    d->all = 1;
}

void ar_damage_add(ar_damage *d, ar_rect r)
{
    ar_i32 i, best = -1, best_waste = 0;

    if (d->all || r.w <= 0 || r.h <= 0)
    {
        return;
    }

    /* Already covered: the common case, since a parent and its children are
       usually damaged by the same change. */
    for (i = 0; i < d->count; ++i)
    {
        ar_rect e = d->r[i];
        if (r.x >= e.x && r.y >= e.y && r.x + r.w <= e.x + e.w && r.y + r.h <= e.y + e.h)
        {
            return;
        }
    }

    /* Merging into a rectangle this one already overlaps or touches presents no
       extra pixels, so take that before spending a slot. */
    for (i = 0; i < d->count; ++i)
    {
        ar_i32 waste = ar__merge_waste(d->r[i], r);
        if (waste <= 0)
        {
            d->area += ar__area(r) + waste;
            d->r[i] = ar_rect_union(d->r[i], r);
            ar__collapse_if_pointless(d);
            return;
        }
        if (best < 0 || waste < best_waste)
        {
            best = i;
            best_waste = waste;
        }
    }

    if (d->count < AR_DAMAGE_RECTS)
    {
        d->r[d->count++] = r;
        d->area += ar__area(r);
        ar__collapse_if_pointless(d);
        return;
    }

    /* Full, so something must be merged, and the cheapest available merge is
       the one already found above. An exhaustive search over every pair would
       pack the list slightly better and costs O(n^2) per damaged box; on
       arena_churn, which rebuilds four thousand boxes every frame, that was
       measured at 56% slower with a 7.8% spread. Not worth it for a list of
       eight. */
    if (best >= 0)
    {
        d->area += ar__merge_waste(d->r[best], r) + ar__area(r);
        d->r[best] = ar_rect_union(d->r[best], r);
        ar__collapse_if_pointless(d);
    }
}

ar_rect ar_damage_bounds(const ar_damage *d, ar_rect viewport)
{
    ar_rect b;
    ar_i32  i;

    if (d->all)
    {
        return viewport;
    }
    if (d->count == 0)
    {
        return ar_rect_make(0, 0, 0, 0);
    }

    b = d->r[0];
    for (i = 1; i < d->count; ++i)
    {
        b = ar_rect_union(b, d->r[i]);
    }
    return ar_rect_intersect(b, viewport);
}

/*
 * A box is repainted when its geometry moved or when its painted appearance
 * came out different. Geometry is compared exactly. Appearance is compared by
 * digest, because storing a copy of every box's resolved style would cost more
 * slot memory than the frames it saves are worth.
 *
 * Only what ar__paint actually reads goes into the digest. The first version
 * hashed all 128 bytes of the resolved property array, and that was measurably
 * the wrong trade: it made flat_8k 1.6x slower than no damage tracking at all,
 * because eight thousand boxes times a hundred and twenty-eight bytes is a
 * megabyte of hashing to avoid drawing nothing. Layout properties do not
 * belong here anyway -- a box whose margin changed either moved, and the
 * rectangle comparison catches it, or it did not, and nothing needs repainting.
 *
 * Text content is hashed rather than its pointer. Formatting a label into a
 * reused buffer every frame is the ordinary way to write an immediate mode
 * interface, and that leaves the pointer identical while the pixels differ.
 *
 * The digest is FNV-1a. A collision means a box that changed appearance is not
 * repainted, so the bound is worth stating rather than assuming: one in 2^32
 * per appearance change per box.
 */
static ar_u32 ar__mix(ar_u32 h, ar_u32 word)
{
    h ^= word & 0xFFu;
    h *= 16777619u;
    h ^= (word >> 8) & 0xFFu;
    h *= 16777619u;
    h ^= (word >> 16) & 0xFFu;
    h *= 16777619u;
    h ^= (word >> 24) & 0xFFu;
    h *= 16777619u;
    return h;
}

ar_u32 ar_paint_digest(const ar_node *n)
{
    /* Exactly what ar__paint reads, and nothing else. If a property is added
       to the paint pass it must be added here, or boxes will stop repainting
       when only that property changes.

       Read through ar_style_get rather than v[] directly: this list is walked
       by index, so five of its entries are the wide properties that live past
       the end of v[]. Indexing v[] here compiled without a murmur -- the
       subscript is not a constant, so -Warray-bounds cannot see it -- and
       produced a digest built from whatever followed the array. The
       pixel-identity test is what caught it.

       The four scrollbar entries are here because the warning above was not
       heeded once already: the overlay bar added ar__paint_bars to the paint
       pass, which reads scrollbar-color through AR_P_SCROLLBAR_THUMB and
       AR_P_SCROLLBAR_TRACK and reaches AR_P_SCROLLBAR_WIDTH and
       AR_P_SCROLLBAR_GUTTER through ar_scroll_bar_visible, and this list was
       left alone. A frame where hover changed nothing but the bar's colour
       produced no damage and the bar kept the colour it had -- right geometry,
       wrong pixels, which every geometry test in the suite is blind to. */
    static const int PAINTED[] = {AR_P_DISPLAY,         AR_P_OVERFLOW,         AR_P_OVERFLOW_X,
                                  AR_P_BACKGROUND,      AR_P_BORDER_WIDTH,     AR_P_BORDER_COLOR,
                                  AR_P_PAD_LEFT,        AR_P_PAD_TOP,          AR_P_COLOR,
                                  AR_P_SCROLLBAR_WIDTH, AR_P_SCROLLBAR_GUTTER, AR_P_SCROLLBAR_THUMB,
                                  AR_P_SCROLLBAR_TRACK};
    ar_u32           h = 2166136261u;
    ar_u32           i;
    ar_u32           count = (ar_u32)(sizeof PAINTED / sizeof PAINTED[0]);

    for (i = 0; i < count; ++i)
    {
        h = ar__mix(h, (ar_u32)ar_style_get(&n->style, PAINTED[i]));
    }
    h = ar__mix(h, (ar_u32)n->scale);

    /*
     * Not style, but the paint pass reads it all the same.
     *
     * Whether a bar is drawn at all, and how long its thumb is, come from
     * ar_scroll_range -- and that is content_h against the box, neither of
     * which is a property. A list that grows past its container gains a
     * scrollbar with no style change and no change to the container's own
     * rectangle, so every test above it says the box is untouched and the bar
     * is never painted. The rows that grew are damaged, but they are not the
     * column the bar lives in.
     *
     * The scroll position is deliberately not mixed in. It moves the thumb,
     * but every path that scrolls already damages the container, and hashing
     * it here would mark a scrolled container dirty a second time for no gain.
     */
    if (ar_is_scroll_container(n))
    {
        h = ar__mix(h, (ar_u32)n->content_h);
    }

    /*
     * The one thing the paint pass reads that no property on this box records.
     *
     * A collapsed grid line's width is the widest of everything that meets
     * there, so it is a *neighbour's* border-width -- and a cell whose own
     * style did not change from one frame to the next still has to repaint
     * when the cell beside it is given a thicker border. Every entry in
     * PAINTED above would say the box is untouched.
     */
    if (n->state & AR_STATE_COLLAPSED)
    {
        h = ar__mix(h, ((ar_u32)n->edge[0] << 24) | ((ar_u32)n->edge[1] << 16) |
                           ((ar_u32)n->edge[2] << 8) | (ar_u32)n->edge[3]);
    }

    if (n->text)
    {
        const char *t = n->text;
        while (*t)
        {
            h ^= (ar_u32)(unsigned char)*t++;
            h *= 16777619u;
        }
    }

    /* Zero is the "no digest recorded" marker in a slot, so never return it. */
    return h ? h : 1u;
}
