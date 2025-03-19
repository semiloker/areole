/*
 * areole - damage tracking, stage one: one merged rectangle per frame.
 * SPDX-License-Identifier: MIT
 *
 * Why this is the whole of the release and not a step towards it:
 *
 * A Pentium II with PC100 memory sustains around 250 MB/s of write bandwidth.
 * An 800x600 surface at 32 bits per pixel is 1.83 MB, so writing it once costs
 * 7.7 ms out of a 16.7 ms frame, and a real interface has two to three times
 * overdraw. Full-window redraw on that machine is not slow, it is
 * arithmetically impossible. Everything above this release in the roadmap is a
 * cost added on top of that traffic.
 *
 * Stage one takes the overwhelmingly common case -- a hover highlight, a caret
 * blink, a button press -- from 1.83 MB of traffic to about 32 KB, and it does
 * so with a union and an intersect. The paint pass already threads a clip
 * rectangle from the root down through every box, so damage tracking is not a
 * new mechanism: it is a narrower root.
 *
 * Its failure mode is known and is why the design document keeps a stage two.
 * Two changes in opposite corners produce a bounding rectangle covering the
 * whole window and the frame costs full price. That case is measured and
 * published rather than hidden -- see the `opposite_corners` scene -- and the
 * hash grid that fixes it is only worth building once the merged rectangle has
 * been shown to be insufficient on real interfaces rather than contrived ones.
 */
#include "ar_node.h"

void ar_damage_reset(ar_damage *d)
{
    d->r = ar_rect_make(0, 0, 0, 0);
    d->any = 0;
    d->all = 0;
}

void ar_damage_add(ar_damage *d, ar_rect r)
{
    if (r.w <= 0 || r.h <= 0)
    {
        return;
    }
    if (!d->any)
    {
        d->r = r;
        d->any = 1;
        return;
    }
    d->r = ar_rect_union(d->r, r);
}

void ar_damage_add_all(ar_damage *d)
{
    d->all = 1;
    d->any = 1;
}

ar_rect ar_damage_resolve(const ar_damage *d, ar_rect viewport)
{
    if (d->all)
    {
        return viewport;
    }
    if (!d->any)
    {
        return ar_rect_make(0, 0, 0, 0);
    }
    return ar_rect_intersect(d->r, viewport);
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
       when only that property changes. */
    static const int PAINTED[] = {AR_P_DISPLAY,      AR_P_OVERFLOW, AR_P_BACKGROUND,
                                  AR_P_BORDER_WIDTH, AR_P_BORDER_COLOR, AR_P_PAD_LEFT,
                                  AR_P_PAD_TOP,      AR_P_COLOR};
    ar_u32     h = 2166136261u;
    ar_u32     i;
    ar_u32     count = (ar_u32)(sizeof PAINTED / sizeof PAINTED[0]);

    for (i = 0; i < count; ++i)
    {
        h = ar__mix(h, (ar_u32)n->style.v[PAINTED[i]]);
    }
    h = ar__mix(h, (ar_u32)n->scale);

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
