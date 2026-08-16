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
/* ------------------------------------------------------------------------
 * Fragments
 *
 * One piece of an inline box, on one line. A box that fits on a single line
 * has none of these; a box the line breaker cut has one per line it touches,
 * each carrying the byte range of the text that landed there.
 * ------------------------------------------------------------------------ */
typedef struct ar_frag
{
    ar_rect rect;
    ar_i32  node;
    ar_i32  from, to; /* byte range of the node's text on this fragment */
} ar_frag;

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

    ar_i32 fit[2]; /* max-content size, from pass one */

    /*
     * The min-content width: the narrowest this box can be without its
     * contents spilling out of it. For text that is the widest single word,
     * because a word does not break; for a container it is whatever its widest
     * child needs.
     *
     * Only the width, because only the width is ever asked. A min-content
     * height would mean fragmenting, and nothing here fragments yet.
     */
    ar_i32 min_w;

    /*
     * Where this box's fragments live, when it has more than one rectangle.
     *
     * Zero count means the box is its own single fragment and `rect` is all
     * there is, which is every box that is not a split inline. When there are
     * fragments, `rect` is their union -- so hit testing, damage tracking and
     * anything else that wants one rectangle for a box still gets a truthful
     * one without knowing fragments exist.
     */
    ar_i32 frag_first;
    ar_i32 frag_count;
    /*
     * How tall this box's contents came to, whatever the box itself ended up
     * being. The two are the same for an automatic height and differ for a
     * stated one, and the difference is exactly how far a scroll container can
     * be scrolled.
     */
    ar_i32 content_h;

    /* And how wide, for the same reason on the other axis. Kept beside its
       twin rather than derived when asked, because deriving it means walking
       the subtree for the furthest right edge and a scroll container would pay
       that on every frame it is queried. */
    ar_i32 content_w;

    ar_rect rect; /* final, absolute */
    ar_rect clip; /* narrowed by every clipping ancestor */
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

    /* Where this container is scrolled to, on each axis. Their width is the
       AR_SCROLL_COMPACT switch in areole.h: every box carries a slot, so eight
       bytes here is eight bytes per box in the interface. */
    ar_scroll_pos scroll;
    ar_scroll_pos scroll_x;

    ar_u32 last_frame; /* for eviction; 0 means the slot is free */
    ar_u32 digest;     /* of the resolved style; 0 means never recorded */

    /* The last measured width of this box's text and what it was measured
       from. Text is measured once per node per frame, and with an outline face
       that is a glyph cache lookup per character -- on the shipped dashboard,
       more lookups for measuring than for drawing. Hashing the string costs a
       byte per character against a hash and a probe per character. */
    ar_u32 text_key;
    ar_i32 text_px;

    /* The min-content width, remembered under the same key. Measuring it costs
       a glyph lookup per character just as the full width does, and a test
       caught it being paid every frame. */
    ar_i32 text_min_px;
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

    /* Fragments, from the same end of the arena as the tree and released with
       it. Sized against the box budget rather than guessed at: a paragraph of
       inline runs makes a few per box, and a tree with no split inlines uses
       none of them. */
    ar_frag *frags;
    ar_i32   frag_cap;
    ar_i32   frag_count;

    /* Boxes in paint order, back to front, rebuilt each frame beside the tree.
       Painting and hit testing both read it -- one forwards, one backwards --
       so they cannot disagree about what is on top. */
    ar_i32 *order;
    ar_i32  order_count;

    ar_i32 wheel;    /* notches this frame */
    ar_i32 wheel_px; /* the same travel in pixels, when the device knows it */
    ar_u32 keys;     /* AR_KEY_* pressed this frame */
    int    scrolled; /* something moved, so the next frame differs */

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

    /*
     * A scroll that the surface has not caught up with yet, so the next frame
     * can move those pixels instead of painting them again.
     *
     * On the context rather than in the slot table because at most one container
     * can be behind at a time and this way it costs eight bytes rather than
     * eight bytes per box -- ar_slot sat at exactly the byte budget AR_MEM
     * promises, and the static assertion in ar_ctx.c said so before the arena
     * ever had a chance to run out in front of somebody.
     *
     * The key, not the index, because indices are rebuilt with the tree every
     * frame and the record has to survive into the next one. `move_many` is set
     * when a second container moves before the first has been caught up with:
     * ordering two moves against each other is where a nested pair would go
     * wrong, so that frame gives up and repaints instead.
     */
    ar_u32 move_key;
    ar_i32 move_dy;
    int    move_many;

    /*
     * The scrollbar thumb being dragged, and where inside it the press landed.
     *
     * The grab offset is what separates a scrollbar from a slider: without it
     * the thumb jumps to centre itself under the cursor on the first frame.
     *
     * On the context because only one thing can be dragged at a time, and
     * keyed rather than indexed because the tree is rebuilt every frame while a
     * drag lasts for many. Zero means nothing is being dragged.
     */
    ar_u32 drag_key;
    ar_i32 drag_grab;

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

/*
 * The width of text[from..to) for this box, in whole pixels.
 *
 * The line breaker needs to measure pieces of a string without copying them
 * out, and it has no business knowing what a font is -- so it is handed this,
 * the same way the wrapper is handed ar_wrap_fn.
 */
typedef ar_i32 (*ar_text_range_fn)(void *ud, const ar_node *n, ar_i32 from, ar_i32 to);

/*
 * Everything the solver is lent for one pass.
 *
 * Grouped rather than passed one at a time because it grew from two arguments
 * to five, and a struct that says what each is for reads better at the call
 * site than five positional arguments do.
 */
typedef struct ar_layout_env
{
    ar_wrap_fn       wrap;
    ar_text_range_fn measure;
    void            *ud;

    /* Where a scroll container currently is. Null means nothing scrolls,
       which is what every caller before scrolling got. */
    ar_i32 (*scroll_of)(void *ud, ar_i32 index);
    ar_i32 (*scroll_x_of)(void *ud, ar_i32 index);

    /* Somewhere to put fragments. May be null, in which case inline boxes are
       not split -- which is what every caller before fragmentation got. */
    ar_frag *frags;
    ar_i32   frag_cap;
    ar_i32   frag_used;
} ar_layout_env;

void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env);

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
/*
 * A run of inline-level siblings, from `first` up to but not including `stop`.
 *
 * CSS wraps such a run in an anonymous block box. areole does not make one --
 * there is no node to make it out of -- so the stack asks the caller how tall
 * the run is and leaves a gap of that size. An anonymous box has no margins,
 * which is why the run never collapses with anything around it, and that
 * happens to be exactly right.
 */
typedef ar_i32 (*ar_block_run_fn)(void *ud, ar_i32 first, ar_i32 stop, ar_i32 y);

/* Where a box with `clear` has to start, given where it would otherwise have
   gone. Null when the caller has no floats to clear. */
typedef ar_i32 (*ar_block_clear_fn)(void *ud, ar_i32 y, ar_i32 which);

ar_i32 ar_block_stack(const ar_node *n, ar_node *nodes, ar_block_height_fn height,
                      ar_block_place_fn place, ar_block_run_fn run, ar_block_clear_fn clear_to,
                      void *ud);

int ar_is_floated(const ar_node *n);

/* A stated size, turned into the size the box occupies. See box-sizing. */
ar_i32 ar_used_size(const ar_node *n, ar_i32 axis, ar_i32 stated);

/* ------------------------------------------------------------------------
 * Scroll containers -- ar_scroll.c
 * ------------------------------------------------------------------------ */

/* Drawn inside the container's right edge rather than taken out of its width:
   a scrollbar that appears and reflows the text beside it makes the interface
   jump, and on a machine where relayout costs milliseconds it does so
   visibly. */
/* How far one wheel notch goes. Three lines of an eight pixel face, which is
   what every toolkit settled on and what the hand expects. */
#define AR_SCROLL_STEP 30

#define AR_SCROLLBAR_W      8
#define AR_SCROLLBAR_W_THIN 4
#define AR_SCROLLBAR_MIN    16

int    ar_is_scroll_container(const ar_node *n);
int    ar_scrolls_x(const ar_node *n);
int    ar_scrolls_y(const ar_node *n);
int    ar_clips(const ar_node *n);
ar_i32 ar_overflow_x(const ar_node *n);
ar_i32 ar_overflow_y(const ar_node *n);
ar_i32 ar_scroll_range(const ar_node *n);
ar_i32 ar_scroll_clamp(const ar_node *n, ar_i32 want);
ar_i32 ar_scroll_range_x(const ar_node *n);
ar_i32 ar_scroll_clamp_x(const ar_node *n, ar_i32 want);
int    ar_scroll_bar_visible(const ar_node *n);

/* How wide this container's bar is drawn: 0 when scrollbar-width is `none`.
   Also what scrollbar-gutter reserves, which is why it is not a constant. */
ar_i32 ar_scroll_bar_width(const ar_node *n);

/* What the gutter takes out of the inline end, 0 unless `stable`. */
ar_i32 ar_scroll_gutter(const ar_node *n);

/* Does this container snap on the block axis? */
int ar_scroll_snaps_y(const ar_node *n);

/* Where a scroll heading for `want` from `cur` actually settles. Returns
   `want` unchanged when the container does not snap or nothing is near
   enough under `proximity`. */
ar_i32 ar_scroll_snap(const ar_node *nodes, ar_i32 count, ar_i32 container, ar_i32 cur,
                      ar_i32 want);
void   ar_scroll_bar(const ar_node *n, ar_i32 scroll, ar_rect *track, ar_rect *thumb);

/* Shifts every scroll container's contents by its offset. */
void ar_scroll_apply(ar_node *nodes, ar_i32 count, ar_layout_env *env);

/* ------------------------------------------------------------------------
 * Stacking order -- ar_stack.c
 * ------------------------------------------------------------------------ */

/*
 * There is no bound on a z-index.
 *
 * There was one, because the ordering passes swept the whole range rather than
 * sorting. The comment here claimed a z past it was clamped to the top of the
 * pile; nothing clamped it, so `z-index: 99999` matched no sweep and the box
 * was never painted. The passes now visit only the values a stylesheet uses,
 * so the bound had nothing left to do and the box paints where it asked to.
 */

int ar_z_is_auto(const ar_node *n);
int ar_forms_stacking_context(const ar_node *n);

/* Fills `order` with every visible box, back to front. Returns the count. */
ar_i32 ar_stack_order(ar_node *nodes, ar_i32 count, ar_i32 *order, ar_i32 cap);

/* ------------------------------------------------------------------------
 * Positioning -- ar_layout_position.c
 * ------------------------------------------------------------------------ */
int ar_is_positioned(const ar_node *n);
int ar_is_out_of_flow(const ar_node *n);

/* The box an out-of-flow child is measured against: the padding box of the
   nearest positioned ancestor, or the viewport. */
ar_rect ar_containing_block(const ar_node *nodes, ar_i32 i, ar_rect viewport);

void ar_position_out_of_flow(ar_node *nodes, ar_i32 i, ar_rect viewport);
void ar_position_relative(ar_node *nodes, ar_i32 count, ar_rect viewport);

int  ar_is_sticky(const ar_node *n);
void ar_position_sticky(ar_node *nodes, ar_i32 count, ar_rect viewport);

/* ------------------------------------------------------------------------
 * Floats -- ar_layout_float.c
 * ------------------------------------------------------------------------ */

/* More than a document puts beside one another. Past this a float is placed as
   though the list were full, which puts it at the container's edge -- a
   visible failure rather than a silent one. */
#define AR_MAX_FLOATS 16

typedef struct ar_float_box
{
    ar_i32 x0, x1; /* the margin box, because that is what content avoids */
    ar_i32 y0, y1;
    ar_u8  side;
} ar_float_box;

typedef struct ar_float_ctx
{
    ar_float_box f[AR_MAX_FLOATS];
    ar_i32       count;
    ar_i32       left, right; /* the containing block's content edges */
} ar_float_ctx;

void ar_float_reset(ar_float_ctx *fc, ar_i32 left, ar_i32 right);

/* The horizontal band still free between y and y + h. */
void ar_float_band(const ar_float_ctx *fc, ar_i32 y, ar_i32 h, ar_i32 *out_left, ar_i32 *out_right);

/* The lowest edge of the floats on the given sides, at or below y. */
ar_i32 ar_float_clear_y(const ar_float_ctx *fc, ar_i32 y, ar_i32 which);
ar_i32 ar_float_bottom(const ar_float_ctx *fc);

/* Puts a float at or below y, as far to its side as it will go. */
void ar_float_place(ar_float_ctx *fc, ar_node *n, ar_i32 y, ar_i32 side);

/* ------------------------------------------------------------------------
 * Inline formatting -- ar_layout_inline.c
 * ------------------------------------------------------------------------ */
int    ar_is_inline_level(const ar_node *n);
ar_i32 ar_inline_baseline(const ar_node *n);

/* Lays a run of inline-level siblings into line boxes and returns how tall
   they came to. Sizes must already be resolved. */
/*
 * `fc` may be null, which is the same as there being no floats. When it is
 * not, each line asks it for the band still free at that line's own y -- which
 * is what makes text wrap around a float rather than through it.
 *
 * `abs_top` is where `top` sits in the coordinates the float list uses, since
 * the run works in offsets and the floats do not.
 */
ar_i32 ar_inline_run(ar_node *nodes, ar_i32 first, ar_i32 stop, ar_i32 left, ar_i32 top,
                     ar_i32 inner_w, ar_i32 align, const ar_float_ctx *fc, ar_i32 abs_top,
                     ar_layout_env *env);

/* Whether this box's text flows into the lines around it and may be cut. */
int ar_is_fragmentable(const ar_node *n);

ar_slot *ar_ctx_slot(ar_ctx *c, ar_u32 key);

/* The same lookup without claiming an empty slot, for queries. */
const ar_slot *ar_ctx_slot_find(const ar_ctx *c, ar_u32 key);

#endif /* AR_NODE_H */
