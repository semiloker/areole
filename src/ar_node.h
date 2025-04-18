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
#include "ar_text.h"

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
    ar_u16 state; /* hover, active, focus, and the structural pseudo-classes */

    /* What the caller declared, kept because a combinator asks about an
       ancestor or a sibling and the answer is a property of that box rather
       than of this one. Only rules with combinators read it, so a stylesheet
       without them never touches it. */
    ar_u32     sel_tag;
    ar_u32     sel_id;
    ar_classes sel_class;

    ar_i32 prev_sibling;

    ar_style    style;
    const char *text;
    ar_i32      scale; /* bitmap font scale derived from font-size */

    /* Measured while the tree is built, because that is where the loaded face
       is reachable and the layout solver is deliberately pure. Also measures
       once per node rather than once per layout pass. */
    ar_i32 text_w;

    /* The vertical text metrics, in whole pixels, settled where the font is
       known rather than in the solver -- which has no business knowing what a
       font is. `text_h` is one line, `line_h` the advance to the next.

       With an outline face both come from the face's own ascender, descender
       and line gap, so a 13 px font gets a line box that a 13 px glyph fits
       in. With the built-in bitmap face they stay 8 and 10 at scale 1, which
       is what they have always been. */
    ar_i32 text_h;
    ar_i32 line_h;

    /* Where the baseline sits inside a line box. Kept beside the other two so
       painting cannot compute it differently from the way layout did. */
    ar_i32 ascent;

    /* The two vertical margins this box presents to its siblings, after
       collapsing with its own children. Computed bottom-up in the measure
       pass; see ar_layout_block.c, which is where the model is explained. */
    ar_i32 mt;
    ar_i32 mb;

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
    ar_u32  digest;     /* of the resolved style; 0 means never recorded */

    /* The last measured width of this box's text and what it was measured
       from. Text is measured once per node per frame, and with an outline face
       that is a glyph cache lookup per character -- on the shipped dashboard,
       more lookups for measuring than for drawing. Hashing the string costs a
       byte per character against a hash and a probe per character. */
    ar_u32 text_key;
    ar_i32 text_px;
    ar_u32 seen; /* frame this box last appeared in the tree */
} ar_slot;

/* ------------------------------------------------------------------------
 * Damage
 *
 * One merged rectangle per frame. See ar_damage.c for why that is the whole
 * of stage one and what its known failure mode is.
 * ------------------------------------------------------------------------ */
typedef struct ar_damage
{
    ar_rect r[AR_DAMAGE_RECTS];
    ar_i32  count;
    ar_i32  area;          /* pixels currently covered, for the collapse rule */
    ar_i32  viewport_area; /* what "half the surface" means this frame */
    int     all;           /* repaint everything: first frame, resize, or asked */
} ar_damage;

void    ar_damage_reset(ar_damage *d);
void    ar_damage_set_viewport(ar_damage *d, ar_rect viewport);
void    ar_damage_add(ar_damage *d, ar_rect r);
void    ar_damage_add_all(ar_damage *d);
ar_rect ar_damage_bounds(const ar_damage *d, ar_rect viewport);
ar_u32  ar_paint_digest(const ar_node *n);

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

    /* Text. Absent until ar_font_load, and the bitmap face is used until then,
       so a build that never calls it pays nothing for any of this. */
    ar_face          face[AR_MAX_FACES];
    ar_font_chain    chain;
    ar_shaper        shaper;
    int              shaping;
    ar_glyph_cache   glyphs;
    ar_glyph_scratch glyph_scratch;
    int              have_face;

    ar_damage damage;
    ar_rect   last_viewport; /* a resize repaints everything */
    ar_rect   last_damage;   /* what ar_frame_end returned, for the backend */
    ar_i32    seen_last;     /* boxes in the tree last frame, to spot removals */

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
/*
 * How many lines a string takes at a given width, and how tall that is.
 *
 * Layout needs this and has no business knowing what a font is, so it is
 * handed a function. ar_ctx supplies one that wraps through its fallback chain
 * and its glyph cache; a caller with no text does not have to supply anything,
 * and a null callback means every box is one line, which is what the solver
 * did before wrapping existed.
 *
 * `max_w` is the content width in whole pixels. The return value is the height
 * in whole pixels, not a line count, because line height is the font's
 * business rather than the solver's.
 */
typedef ar_i32 (*ar_wrap_fn)(void *ud, const ar_node *n, ar_i32 max_w);

void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_wrap_fn wrap, void *ud);

/* ------------------------------------------------------------------------
 * Block formatting -- ar_layout_block.c
 * ------------------------------------------------------------------------ */

/* The larger of the positive parts plus the smaller of the negative parts. */
ar_i32 ar_margin_collapse(ar_i32 a, ar_i32 b);

int ar_is_block(const ar_node *n);
int ar_establishes_bfc(const ar_node *n);

/* Whether a child's bottom margin reaches through this box's bottom edge. */
int ar_block_open_at_bottom(const ar_node *n);

/* The gap above the first child, which is zero when its margin escaped. */
ar_i32 ar_block_top_gap(const ar_node *n, const ar_node *first);

/* Fills in a box's collapsed top and bottom margins. Call bottom-up. */
void ar_block_margins(ar_node *n, const ar_node *nodes);

/* How tall a child is, asked of whoever is doing the stacking: the measure
   pass answers with the intrinsic height, the placement pass with the real
   one, and both get the same margin arithmetic out of it. */
typedef ar_i32 (*ar_block_height_fn)(void *ud, ar_i32 index);

/* Where a child ended up. `real` is zero for a self-collapsing box, which has
   no height and is placed only so it has coordinates at all. */
typedef void (*ar_block_place_fn)(void *ud, ar_i32 index, ar_i32 y, int real);

/* Walks the stack and returns the content height. `place` may be null, which
   is how the measure pass asks the question without answering it. */
ar_i32 ar_block_stack(const ar_node *n, ar_node *nodes, ar_block_height_fn height,
                      ar_block_place_fn place, void *ud);

ar_slot *ar_ctx_slot(ar_ctx *c, ar_u32 key);

#endif /* AR_NODE_H */
