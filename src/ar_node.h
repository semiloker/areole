/*
 * areole - the box tree and the context that owns it.
 * SPDX-License-Identifier: MIT
 *
 * Not installed. Callers see ar_begin and ar_end; this is what those build.
 */
#ifndef AR_NODE_H
#define AR_NODE_H

#include "ar_internal.h"
#include "ar_css.h"

#define AR_MAX_DEPTH 64

/* ------------------------------------------------------------------------
 * Box
 *
 * Children are referenced by index rather than by pointer. Indices survive
 * the array being carved out of a different part of the arena between frames,
 * they halve the size of the links on a 64 bit target, and a flat array walked
 * front to back is what makes each layout pass a linear sweep.
 *
 * Boxes are appended in the order the caller declares them, so a parent always
 * precedes its children. That single property is what lets both layout passes
 * be plain loops with no recursion and no explicit stack.
 * ------------------------------------------------------------------------ */
typedef struct ar_node
{
    ar_i32 parent;
    ar_i32 first_child;
    ar_i32 last_child;
    ar_i32 next_sibling;
    ar_i32 child_count;

    ar_u32 key;   /* stable across frames, for state and hit testing */
    ar_u8  state; /* hover, active, focus */

    ar_style    style;
    const char *text;
    ar_i32      scale; /* text scale derived from font-size */

    ar_i32  fit[2]; /* intrinsic size, from pass one */
    ar_rect rect;   /* final, absolute */
    ar_rect clip;   /* narrowed by every clipping ancestor */
} ar_node;

/* ------------------------------------------------------------------------
 * Persistent per-box state
 *
 * Hover and active have to be known before the style is resolved, but they
 * depend on where the box ended up, which is not known until after layout.
 * The way every immediate mode toolkit breaks that circle is to hit test
 * against the previous frame. Interfaces only move while animating, and
 * nothing animates while a cursor is resting on it, so the delay is invisible.
 *
 * That is what this table is for: one slot per box, keyed by a hash that is
 * stable as long as the tree shape is.
 * ------------------------------------------------------------------------ */
typedef struct ar_slot
{
    ar_u32  key;
    ar_rect rect; /* where this box was last frame */
    ar_i32  scroll;
    ar_u32  last_frame; /* for eviction; 0 means the slot is free */
} ar_slot;

struct ar_ctx
{
    ar_arena arena;
    ar_sheet sheet;

    ar_node *nodes;
    ar_i32   node_cap;
    ar_i32   box_budget; /* what AR_MEM was sized for */
    ar_i32   node_count;
    ar_i32   overflowed; /* the tree did not fit; reported, never scribbled */

    ar_slot *slots;
    ar_i32   slot_cap;

    ar_i32 stack[AR_MAX_DEPTH];
    ar_i32 depth;
    ar_i32 unbalanced; /* more ar_end than ar_begin, or a depth overrun */

    ar_u32 frame;

    /* Input, as handed in at the top of the frame. */
    ar_i32 mouse_x, mouse_y;
    ar_u32 mouse_down;
    ar_u32 mouse_pressed;
    ar_u32 mouse_released;
    int    mouse_inside;

    ar_u32 hot;     /* key of the box under the cursor         */
    ar_u32 active;  /* key of the box the press started on     */
    ar_u32 clicked; /* key of the box released on this frame   */
    int    hot_changed;

    /* The core owns no clock. The backend lends it one so the frame can time
       its own phases without src/ ever learning what a platform is. */
    ar_u32 (*clock)(void);

    ar_perf perf;
};

/* The layout solver. Takes a tree whose root is index 0 and gives every box an
   absolute rect. Pure: no allocation, no clock, no drawing. */
void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport);

ar_slot *ar_ctx_slot(ar_ctx *c, ar_u32 key);

#endif /* AR_NODE_H */
