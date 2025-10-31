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
#include "ar_edit.h"

#define AR_MAX_DEPTH 64

/* How many focusable boxes one frame may hold. A form with more than this
   many fields is a form nobody fills in; Tab stops at the ceiling rather
   than wrapping into the wrong place, and ar_test says so. */
#define AR_MAX_FOCUSABLES 256

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

    ar_u32 key; /* stable across frames, for state and hit testing */
    /* Thirty-two bits as of 0.10.0. It was sixteen and `:checked` was the
       seventeenth state, which is the widening the comment beside
       AR_STATE_FOCUS_WITHIN said the next bit would force. */
    ar_u32 state; /* hover, active, focus, checked, and the structural ones */

    /*
     * Where the inline declarations start inside `hints`, or 0 for none.
     *
     * The two lists live in one arena block, NUL-terminated one after the
     * other, because they are always written together and a second pointer
     * would cost eight bytes a box where this costs none -- it fits in the
     * hole the compiler was already leaving beside `state`.
     */
    ar_u16 inline_at;

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

    /*
     * This box's own declaration list -- what a `style=""` attribute holds --
     * or null, which is every box in an interface and most in a document.
     *
     * Kept on the node rather than passed through and forgotten, because
     * ar__resolve_late resolves a second time for any box whose :last-child,
     * :only-child or :empty state turned out differently once its parent
     * closed. That second resolve starts from the cascade again, so without
     * the string here it would quietly throw the inline style away -- and only
     * for the last child of something, only in a sheet that asks about it.
     *
     * It points into the frame arena, copied there by ar_begin_styled, so the
     * caller may pass a stack buffer and the pointer stays good until the next
     * ar_frame_begin.
     *
     * The block starts with the *presentational hints* -- what `<td bgcolor>`
     * and `<font size>` mean, which is a different band of the cascade -- and
     * `inline_at` says where they end and the inline declarations begin.
     */
    const char *hints;
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
     * The collapsed border this box owns, per side: top, right, bottom, left.
     *
     * A collapsed grid line belongs to no single box -- its width is the widest
     * of everything that meets there, which is a *neighbour's* style, and no
     * property on this box records it. It is resolved once by the table solve
     * and left here for the paint pass, which is also why ar_paint_digest mixes
     * it in: a cell whose own style did not change still has to repaint when
     * the cell beside it gets a thicker border.
     *
     * Only meaningful with AR_STATE_COLLAPSED, and zero on everything else --
     * including a collapsed table's own box and its rows, which is how they
     * come to draw nothing.
     */
    ar_u8 edge[4];

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

    /*
     * The inner width this box's height was last settled at, or -1.
     *
     * Heights sweep up while widths sweep down, so a box that stacks
     * other boxes cannot know how tall it is until its own width is
     * known -- and its parent needs that height to place whatever comes
     * after it. The answer is to lay the subtree out early, from
     * ar_wrap_height, and this is what stops that costing 2^depth: the
     * parent's stack measures a child, the forward sweep then places the
     * same child for real, and without a memo each of those two visits
     * would measure the whole subtree again.
     *
     * Paired with `content_h`, which is the height that was arrived at.
     * Two fields would not fit -- ar_node has exactly four bytes of
     * headroom against AR_BYTES_PER_BOX -- and `content_h` is already
     * that number, so only the width it was true for is new.
     */
    ar_i32 measured_w;

    /*
     * Which set of custom properties this box can see: an index into the
     * context's scope table, or -1.
     *
     * An index and not a copy, because a box that declares none points at its
     * parent's scope and most boxes declare none. The chain is short by
     * construction -- each scope names the nearest declaring ancestor rather
     * than the parent box -- so a lookup walks declarations, not depth.
     *
     * Sixteen bits and not thirty-two, and that is arithmetic rather than
     * thrift: ar_node is 488 bytes and ar_slot is 52, against a per-box
     * budget of 544. Four bytes fit and eight do not, and the assertion in
     * ar_ctx.c said so on the first build. AR_VAR_SCOPES is 256.
     */
    ar_i16 var_scope;

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
/* Set state bits on the next box to be opened. Internal: the public API has
   no notion of a state bit, and the states this carries are the ones the
   markup decides rather than the ones input does. */
void ar_state_next(ar_ctx *c, ar_u32 bits);

/*
 * What kind of control the next box is.
 *
 * `group` is what the control acts on: a radio's `name` hash, a summary's
 * details, an option's select, a swatch's colour field. `dom` is the element
 * it came from, or -1 for a control declared by hand -- a label's click and a
 * form's submission both start from the element and need to find the box.
 * `val` is a payload the kind gives meaning to: an option's index, a swatch's
 * colour, the element a label is for.
 */
void ar_control_next(ar_ctx *c, ar_u8 kind, ar_u32 group, ar_i32 dom, ar_i32 val);

/* A text field's value as the markup states it. */
void ar_value_next(ar_ctx *c, const char *value, ar_u32 len);

/*
 * What sort of field the next box is, and its limits.
 *
 * `flags` is AR_FIELD_*. `lo`, `hi` and `step` are thousandths, read only by a
 * number field and a range; `maxlen` is `maxlength` in characters, 0 for none.
 */
void ar_field_next(ar_ctx *c, ar_u32 flags, ar_i32 lo, ar_i32 hi, ar_i32 step, ar_i32 maxlen);

enum
{
    AR_FIELD_MULTI = 1u << 0,    /* a textarea: Enter is a newline, arrows move by line */
    AR_FIELD_PASSWORD = 1u << 1, /* drawn as bullets, never copied            */
    AR_FIELD_NUMBER = 1u << 2,   /* digits and a sign; Up and Down step it    */
    AR_FIELD_READONLY = 1u << 3, /* a caret and a selection, and no edits     */
    AR_FIELD_DISABLED = 1u << 4
};

/*
 * A control's value as the user left it, or `dflt` if nobody has touched it.
 *
 * For the controls whose state is a number rather than a bit: the option a
 * select shows, where a slider sits, the colour a colour field holds. Asked
 * of the box most recently opened, which is the control itself while the walk
 * is deciding what to build inside it.
 */
ar_i32 ar_box_value(const ar_ctx *c, ar_i32 dflt);

/* The key of the box most recently opened, so the walk can hand it to the
   controls built inside it as the thing they act on. */
ar_u32 ar_box_key(const ar_ctx *c);

/* Whether a box already built this frame carries any of `bits` in its state:
   a summary asks it of its details, which is open by the slot and not by the
   markup once anybody has clicked. */
int ar_box_state(const ar_ctx *c, ar_i32 box, ar_u32 bits);

/* The `<option>` a select is showing now, or -1 -- for the accessibility
   tree's value and for a form's submission. */
ar_i32 ar_dom_select_option(const ar_ctx *c, const ar_doc *d, ar_i32 node);

/* The document the frame is being built from, recorded by ar_dom_build so the
   end of the frame -- a submission, a label's click -- can read attributes. */
void ar_frame_doc(ar_ctx *c, ar_doc *d);

/* DOM helpers the end of the frame calls, implemented beside the walk in
   ar_dom.c because that is where attributes are read. */
ar_i32 ar_dom_form_of(const ar_doc *d, ar_i32 node);
ar_i32 ar_dom_first_submit(const ar_doc *d, ar_i32 form);
void   ar_dom_form_reset(ar_ctx *c, const ar_doc *d, ar_i32 form);
ar_i32 ar_dom_attr_num(const ar_doc *d, ar_i32 node, const char *name, ar_i32 dflt);

/* The control state the walk and the form code share, keyed by box. */
ar_i32 ar_ctl_value(const ar_ctx *c, ar_u32 key, ar_i32 dflt);
void   ar_ctl_set_value(ar_ctx *c, ar_u32 key, ar_i32 v);
void   ar_ctl_forget(ar_ctx *c, ar_u32 key);
ar_u32 ar_ctl_key_of(const ar_ctx *c, ar_i32 dom);
int    ar_field_value_of(const ar_ctx *c, ar_u32 key, const char **text, ar_u32 *len);

/* Emit the text child of the field just opened, from whichever of the three
   places currently holds its text. */
void ar_field_child(ar_ctx *c, const char *fallback, ar_u32 n, const char *placeholder, ar_u32 pn);
void ar_text_kept(ar_ctx *c, const char *selector, const char *text, ar_u32 n);

/* Whether the box most recently opened is a `<details>` that is showing its
   contents. The walk asks so it can skip the children of a closed one. */
int ar_box_is_open(const ar_ctx *c);

/* The same question about a control's checkedness, for the mark. */
int ar_box_is_checked(const ar_ctx *c);

/* The computed `white-space` of the box just opened, for the walk that has to
   decide whether to collapse the text inside it. */
ar_i32 ar_box_white_space(const ar_ctx *c);

/* Whether the last box opened inside the current one is inline-level, which is
   what decides whether a whitespace-only text node is content. */
int ar_last_child_is_inline(const ar_ctx *c);

enum
{
    AR_CTL_NONE = 0,
    AR_CTL_CHECKBOX,
    AR_CTL_RADIO,
    AR_CTL_BUTTON, /* type=button: activates, and does nothing on its own   */
    AR_CTL_SUMMARY,
    AR_CTL_DETAILS,
    AR_CTL_TEXT,
    AR_CTL_SUBMIT, /* submits its form                                       */
    AR_CTL_RESET,  /* puts its form back the way the markup had it           */
    AR_CTL_SELECT, /* opens and closes; arrows choose                         */
    AR_CTL_OPTION, /* one row of an open select: group is the select's key   */
    AR_CTL_RANGE,  /* a slider: arrows step it, a press puts it under the pointer */
    AR_CTL_COLOR,  /* a swatch that opens a palette                           */
    AR_CTL_SWATCH, /* one colour of that palette: val is 0xRRGGBB             */
    AR_CTL_FILE,   /* asks the embedder for a file name                       */
    AR_CTL_LABEL   /* passes its click to the control it labels: val is that element */
};

/* Whether a kind is a push button of any sort, which is what a click on a
   label passes straight through to. */
#define AR_CTL_IS_BUTTON(k) ((k) == AR_CTL_BUTTON || (k) == AR_CTL_SUBMIT || (k) == AR_CTL_RESET)

/*
 * The capacities of the interaction tables, all on the context and none on
 * the boxes, for the reason the control list gives: a field on every box is a
 * cost on every box in every interface for the handful that are controls.
 */
#define AR_VALUES      32   /* fields whose text survives losing the caret   */
#define AR_VALUE_BYTES 8192 /* and the bytes they share: two full textareas  */
#define AR_CVALS       64   /* selects, sliders and colours that were touched */
#define AR_RANGES      32   /* sliders in one frame                          */
#define AR_CTL_MEMO    128  /* which box each control element last became    */
#define AR_COMPOSE_CAP 256  /* an input method's composition string          */

typedef struct ar_slot
{
    ar_u32 key;
    /* Where this box's *pixels* were last frame -- ar_painted_bounds, not the
       border box. An outline is drawn outside the box, so the two differ by
       the ring's width exactly when a ring is what has to be erased. */
    ar_rect rect;

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

    /*
     * What the user has done to this control, as opposed to what the markup
     * said.
     *
     * `checked` in the markup is the default checkedness; a click changes the
     * state and not the attribute. So the state has to outlive the frame and
     * cannot come from the document, and the slot is where per-box memory
     * already lives.
     *
     * TOUCHED is the half that is easy to forget: without it there is no way
     * to tell "unchecked because the user unchecked it" from "unchecked
     * because nobody has been here yet", and a box with `checked` in the
     * markup would spring back on every frame.
     */
    ar_u8 flags;
} ar_slot;

enum
{
    AR_SLOT_CHECKED = 1 << 0,
    AR_SLOT_TOUCHED = 1 << 1,
    /* A `<details>` showing its contents. Separate from CHECKED because a
       box is never both, and sharing the bit would work until the day
       something is. */
    AR_SLOT_OPEN = 1 << 2
};

/* ------------------------------------------------------------------------
 * Damage
 *
 * One merged rectangle per frame. See ar_damage.c for why that is the whole
 * of stage one and what its known failure mode is.
 * ------------------------------------------------------------------------ */
/* One box's worth of custom property declarations, and the scope enclosing
   it. `parent` is the nearest *declaring* ancestor rather than the parent box,
   so a lookup walks declarations and not tree depth. */
typedef struct ar_var_scope
{
    ar_i32 parent;
    ar_u16 first;
    ar_u16 count;
} ar_var_scope;

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
ar_rect ar_painted_bounds(const ar_node *n);

struct ar_ctx
{
    ar_arena arena;

    /* Where <link rel=stylesheet> gets its bytes, and what to hand back to
       whoever set it. See ar_set_stylesheet_loader. */
    const char *(*link_load)(void *user, const char *href);
    void  *link_user;
    ar_i32 links_skipped;

    /* What the caller reserved for a parsed document at init, so
       ar_html_parse_into does not have to be told twice. */
    ar_u32 doc_budget;
    /*
     * Custom property scopes, one per box that declares any.
     *
     * Two arrays rather than a list per box: `var_scopes` says where a box's
     * declarations start and which scope encloses it, and `var_entries` holds
     * the declarations themselves end to end. A box that declares nothing
     * costs nothing -- it points at the scope its parent pointed at.
     */
    ar_var_scope *var_scopes;
    ar_u16        var_scope_count;
    ar_u16        var_scope_cap;
    ar_var_decl  *var_entries;
    ar_u16        var_entry_count;
    ar_u16        var_entry_cap;

    ar_sheet sheet;

    /* What the backend has said about the display it is drawing on: the
       safe-area insets, the titlebar rectangle, and whether the stylesheet
       asked for the whole display. Read by env(). */
    ar_env env;

    /* Things this frame noticed that are probably not what was meant. Fixed
       and small: a frame that produces sixteen of these has one cause, and the
       seventeenth would say nothing the first sixteen did not. */
    struct
    {
        ar_i32 code;
        ar_i32 node;
    } diag[AR_DIAG_MAX];
    ar_i32 diag_count;

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

    /* Which entries of `stack` are anonymous table boxes this frame generated
       rather than boxes the caller declared. They close when the box holding
       them closes, or when something arrives that cannot live inside them --
       not when the box that caused them closes, or two bare cells would get a
       row each instead of sharing one. Declaration state, dead once the frame
       is built, so it lives here rather than costing every box a byte. */
    ar_u8  is_anon[AR_MAX_DEPTH];
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
    ar_face face[AR_MAX_FACES];
    ar_i32  face_used; /* how many of the pool are taken */

    /*
     * A chain per style, and which face in the pool each style got.
     *
     * The index is (bold ? 1 : 0) | (italic ? 2 : 0), so the four are regular,
     * bold, italic and bold italic. `style_face` is -1 for a style nobody
     * loaded a face for, and its chain then leads with the regular face --
     * which is what a browser does with a family that has no bold: the rule
     * applies and the nearest face draws it.
     *
     * The fallbacks are shared. A face added for coverage is appended to every
     * style's chain, because a Japanese glyph is missing from the bold face
     * for exactly the same reason it is missing from the regular one.
     */
    ar_i32        style_face[4];
    ar_font_chain style_chain[4];

    /*
     * The monospace family, which is one face and not four.
     *
     * `<pre>` and `<code>` are the whole reason it exists and neither is
     * commonly bold or italic, so a second set of four style slots would be
     * three faces of arena for a case nobody writes. A bold `<code>` draws in
     * the monospace regular, which is what a family with no bold does
     * everywhere else in this file.
     */
    ar_i32        mono_face;
    ar_font_chain mono_chain;

    /* And the sans-serif family, on the same terms: one face, for controls
       and for anything that asks for sans-serif. */
    ar_i32        sans_face;
    ar_font_chain sans_chain;

    ar_font_chain    chain;
    ar_shaper        shaper;
    int              shaping;
    ar_glyph_cache   glyphs;
    ar_glyph_scratch glyph_scratch;
    int              have_face;

    ar_damage damage;
    ar_rect   last_viewport; /* a resize repaints everything */

    /* Device pixels per CSS pixel, thousandths, for `@media (resolution)`.
       The window reports its size but never its scale, so this is the one
       piece of the media state a caller has to supply. */
    ar_i32 media_resolution;

    /* Set by ar_set_media, and it stops ar_frame_begin deriving the size from
       the last frame's viewport. A caller who has said what the window is
       must not be second-guessed by a stale one. */
    ar_media media;
    int      media_from_caller;
    ar_rect  last_damage; /* what ar_frame_end returned, for the backend */
    ar_i32   seen_last;   /* boxes in the tree last frame, to spot removals */

    ar_u32 hot;

    /* The same box as `hot`, by index rather than key. Anything asking
       "is the cursor inside this subtree" -- light dismiss, and
       inertness -- needs to walk parents, and a key would have to be
       looked up first. Rebuilt every frame beside `hot`, so it is only
       ever read in the frame that set it. */
    ar_i32 hot_index; /* key of the box under the cursor         */
    ar_u32 active;    /* key of the box the press started on     */
    ar_u32 clicked;   /* key of the box released on this frame   */
    int    hot_changed;

    /*
     * The hot box's ancestors, by key, innermost first -- and the whole reason
     * `:hover` works on a document at all.
     *
     * CSS says an element matches `:hover` while the pointer is over it *or
     * over a descendant of it*, and the hit test finds exactly one box: the
     * topmost. For a hand-declared tree those are usually the same box, which
     * is why this was never missed. For a parsed document they never are:
     * `ar_dom_build` gives every element's text a child of its own, so the
     * box under the cursor is always that child and the element carrying the
     * `:hover` rule is always its parent. Hovering anything in an HTML page
     * did nothing whatsoever.
     *
     * Keys rather than indices because state is resolved in `ar_begin`, while
     * the tree is still being built, and this frame's indices do not exist
     * yet. The chain is a path from a box to the root, so it is at most as
     * long as the tree is deep and usually about eight.
     */
    ar_u32 hot_chain[AR_MAX_DEPTH];
    ar_i32 hot_chain_n;
    ar_u32 active_chain[AR_MAX_DEPTH];
    ar_i32 active_chain_n;

    /*
     * Focus, as a key and the path from it to the root.
     *
     * The same shape as the hover chain above and for the same two reasons:
     * keys rather than indices because the state is wanted in `ar_begin` while
     * this frame's tree is still being built, and a chain because
     * `:focus-within` matches every ancestor the way `:hover` does.
     *
     * Zero means nothing is focused, which is a real state and not a missing
     * one -- a document with nothing focused is where every document starts,
     * and Tab is what leaves it.
     */
    /*
     * State bits the next box opened will carry, set and then cleared.
     *
     * It has to arrive *before* the box, not after: `ar__push_node` resolves
     * the style against the state, so a bit added afterwards would match no
     * rule and the box would be styled as though it were not checked. That is
     * why this is a pending value rather than a setter on the box.
     */
    /*
     * The field being edited, and which box it belongs to.
     *
     * One buffer and not one per field. An ar_edit is four and a half
     * kilobytes, almost all of it the undo ring, and twenty of them is a form's
     * worth of memory spent on nineteen fields nobody is typing in. Only one
     * field has the caret at a time, which is the whole of the argument.
     *
     * What that costs is named rather than hidden: **undo does not survive
     * leaving a field.** Tab away and back, and the ring is empty. A browser
     * keeps it, and keeping it here means a ring per field or a shared ring
     * that knows which field each step belongs to -- neither of which is worth
     * doing before the text is on the screen at all.
     *
     * The text itself does survive, in the pool below, because losing what
     * somebody typed is not a trade-off, it is a bug.
     */
    ar_edit edit;
    ar_u32  edit_key;

    /* This frame's typing, held until the buffer has a field to belong to.
       Tab and a character can arrive in the same frame, and the character
       belongs to the field the Tab moved to -- which is not known until the
       tree is built. */
    const char *pending_text;
    ar_u32      pending_text_len;
    ar_u32      pending_keys;
    int         pending_done;

    ar_u32 next_state;

    /* And what kind of control it is, for the same reason and taken the same
       way. A radio also carries the hash of its `name`, which is the whole of
       what makes a group a group. */
    ar_u8  next_kind;
    ar_u32 next_group;

    /* A text field's markup value, borrowed for the length of ar_begin --
       only read when the field is newly focused and has no stored text. */
    const char *next_value;
    ar_u32      next_value_len;

    ar_u32 focus_key;
    /* Where it landed in this frame's tree, or -1. Recorded in ar_begin so
       the ancestor chain costs a walk up rather than a search over every
       box, which on a ten-thousand-box document is the difference between
       eight compares and ten thousand. */
    ar_i32 focus_index;
    ar_u32 focus_chain[AR_MAX_DEPTH];
    ar_i32 focus_chain_n;

    /*
     * Whether the focus arrived from the keyboard.
     *
     * `:focus-visible` is this bit, and it is the whole reason the pseudo-class
     * exists: a click must focus without drawing a ring and a Tab must draw
     * one. It cannot be worked out from the box, only from the event that
     * moved the focus, so it is remembered here at the moment of the move.
     */
    int focus_visible;

    /* The focusable boxes of the frame just built, in document order, so Tab
       has something to walk. Rebuilt every frame because the tree is. */
    ar_u32 focusables[AR_MAX_FOCUSABLES];
    ar_i32 focusable_n;

    /* And the list as it stood when the frame closed, which is what Tab and a
       click actually walk. Two lists because the live one is half-built while
       the tree is being declared, and a Tab arriving mid-frame would see a
       document that stops at whatever box is open. */
    ar_u32 focusables_prev[AR_MAX_FOCUSABLES];
    ar_i32 focusable_prev_n;

    /* The `tabindex` of each stop, in the same order. A positive one sorts
       ahead of every zero, which is the rule nobody should rely on and every
       engine has to honour. */
    ar_i16 focus_order[AR_MAX_FOCUSABLES];
    ar_i16 focus_order_prev[AR_MAX_FOCUSABLES];

    /*
     * The controls of the frame just built.
     *
     * Activation is settled at frame end, when the tree is complete and the
     * click is known, and takes effect on the next frame -- the same one-frame
     * model hover and focus already use, and for the same reason: the state a
     * box is styled with has to be settled before the box is styled.
     */
    /*
     * What every other edited field holds, without its history.
     *
     * One shared run of bytes rather than a fixed slot per field. It was
     * sixteen slots of 256 bytes, which fitted a one-line field exactly and a
     * textarea not at all: a field that can hold four kilobytes would have
     * lost everything past the 256th byte the moment the caret left it. So the
     * entries are spans of `value_bytes`, packed, and a field takes what it
     * holds rather than what the largest field might.
     *
     * Thirty-two entries and sixteen kilobytes. The field after either runs
     * out evicts the least recently seen, which loses text, and that is the
     * reason both numbers are written down rather than guessed at.
     */
    ar_u32 value_key[AR_VALUES];
    ar_i32 value_dom[AR_VALUES]; /* for a form submitting a field it did not build */
    ar_u16 value_off[AR_VALUES];
    ar_u16 value_len[AR_VALUES];
    ar_u32 value_seen[AR_VALUES];
    char   value_bytes[AR_VALUE_BYTES];
    ar_u16 value_used;

    ar_u32 control_key[AR_MAX_FOCUSABLES];
    ar_u32 control_group[AR_MAX_FOCUSABLES];
    ar_u8  control_kind[AR_MAX_FOCUSABLES];
    ar_i32 control_n;

    /* The element each control came from, and the payload its kind gives a
       meaning to -- see ar_control_next. */
    ar_i32 control_dom[AR_MAX_FOCUSABLES];
    ar_i32 control_val[AR_MAX_FOCUSABLES];
    ar_i32 control_box[AR_MAX_FOCUSABLES]; /* this frame's index, for its state */
    ar_i32 next_dom;
    ar_i32 next_val;

    /* The flags of the field most recently opened, for the text child the walk
       builds inside it -- a password draws bullets whether or not it has the
       caret, and the child is built after the field's flags were taken. */
    ar_u32 field_flags_cur;

    /* The focus moved by keyboard this frame, so its box is brought into view
       once layout has said where it is. */
    int focus_moved;

    /* Enter in a single-line field, waiting for the end of the frame to find
       the form it submits -- which needs the document, and the document is
       only certain to be complete there. */
    ar_i32 implicit_from;

    /* What the next field is and its limits, taken by ar__push_node. */
    ar_u32 next_field;
    ar_i32 next_lo, next_hi, next_step, next_maxlen;

    /*
     * This frame's sliders and their limits, by key.
     *
     * A press on a slider is resolved at the end of the frame against where it
     * was drawn, and turning an x into a value needs the range -- which is in
     * the markup, and the markup is not something the end of the frame should
     * have to parse again.
     */
    ar_u32 range_key[AR_RANGES];
    ar_i32 range_lo[AR_RANGES], range_hi[AR_RANGES], range_step[AR_RANGES];
    ar_i32 range_box[AR_RANGES];
    ar_i32 range_n;

    /*
     * The controls whose state is a number, as the user left them.
     *
     * The same relationship the text pool has with `value` and a slot's
     * CHECKED bit has with `checked`: the markup is where a control starts,
     * and this is where it went. Absent means untouched.
     */
    ar_u32 cval_key[AR_CVALS];
    ar_i32 cval[AR_CVALS];
    ar_u32 cval_seen[AR_CVALS];

    /*
     * Which box each control element became when it was last built.
     *
     * A control inside a closed `<details>` makes no box this frame, and a
     * form still submits it -- with whatever the user left in it. That state
     * is keyed by box, so the key has to outlive the frame that made it.
     */
    ar_i32        ctl_memo_dom[AR_CTL_MEMO];
    ar_u32        ctl_memo_key[AR_CTL_MEMO];
    ar_i32        ctl_memo_next;
    const ar_doc *memo_doc;

    /* The edited field, beyond its buffer: what sort it is, its limits, and
       where this frame put it. */
    ar_u32 edit_flags;
    ar_i32 edit_lo, edit_hi, edit_step;
    ar_i32 edit_dom;
    ar_i32 edit_box;       /* its text box this frame, or -1 */
    ar_i32 edit_field_box; /* the field itself, or -1        */

    /*
     * How far a single-line field's text is shifted left so the caret stays
     * in view. A field does not wrap; it scrolls, and this is the scroll.
     * Only the field with the caret has one, which is what every browser
     * does with a field the caret has left.
     */
    ar_i32 edit_scroll_x;

    /*
     * The caret as painted, and whether the blink has it showing.
     *
     * `caret_epoch` is when it was last made to show: any keystroke or move
     * restarts the blink with the caret visible, because a caret that vanishes
     * the instant you press an arrow is a caret you cannot follow.
     */
    ar_rect caret_rect;
    ar_rect caret_painted;
    int     caret_on;
    int     caret_painted_on;

    /* A finished frame is standing: ar_frame_end has run and ar_frame_begin
       has not, so the tree and the pixels it painted still agree, and
       ar_frame_blink may repaint from the one into the other. */
    int    frame_standing;
    ar_u32 caret_epoch;
    ar_u32 edit_hash; /* caret, selection and composition, for the digest */

    /* Keys that need lines to mean anything -- Up and Down in a textarea, Home
       and End there -- held until layout has made the lines. */
    ar_u32 post_keys;
    int    edit_drag; /* a press began in the field and is still held */
    ar_u32 clicks;

    /* An input method's composition, copied, because ar_input's is borrowed. */
    char   compose[AR_COMPOSE_CAP];
    ar_u16 compose_len;

    /* What Ctrl+C or Ctrl+X asked for, borrowed from the buffer or the log. */
    const char *clip;
    ar_u32      clip_len;

    /* The document this frame is built from, and what its end has to do. */
    ar_doc *frame_doc;
    ar_i32  submit_form, submit_by;
    ar_i32  reset_form;
    ar_i32  file_wanted;
    ar_u32  synth_fire; /* an assistive tool's click, fired at frame end */
    ar_u32  a11y_gen;
    ar_i32  a11y_shape; /* the box count the generation was last moved for */

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
     * The box overflow-anchor is holding still, and how far it sat below the
     * top of its scrollport when we last looked.
     *
     * On the context rather than in the slot table, for the reason the region
     * move records its shift there: a slot is carried by every box in the
     * interface, so eight bytes in it is eight bytes a box, and ar_slot is
     * close enough to the budget AR_MEM promises that a compact build would
     * land on it exactly.
     *
     * One container at a time, therefore. Two lists both growing above the
     * fold in the same frame is the case this cannot serve, and the second one
     * behaves as it does today rather than wrongly. Naming that is cheaper
     * than a per-box field nobody could afford.
     */
    ar_u32 anchor_container; /* key of the scroll container, 0 for none */
    ar_u32 anchor_node;      /* key of the box being held still */
    ar_i32 anchor_y;         /* its offset from the top of the scrollport */

    /* What the container's offset was when the anchor was taken. If it differs
       now, the reader moved it, the anchor's apparent movement was asked for,
       and compensating would cancel the scroll. */
    ar_i32 anchor_scroll;

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

    /* The same width in 1/AR_ONE_PIXEL, unrounded, so the line filler can sum
       a run's pieces and round once. Measuring the run from its start for
       every word did the same with whole pixels and made a line quadratic in
       its words -- 15% of layout on a page of wrapped paragraphs. Null when
       `measure` is exact already, as the bitmap face's whole pixels are: then
       a run's pieces sum to the run with nothing to correct. */
    ar_text_range_fn measure_fx;

    /* The stylesheet, because a grid template is a list and a style slot is
       sixteen bits -- the slot holds an index into a pool that lives here.
       May be null, in which case a grid has no templates and every track is
       implicit, which is a grid nobody wrote but a solver that still works. */
    const ar_sheet *sheet;

    /* Where a scroll container currently is. Null means nothing scrolls,
       which is what every caller before scrolling got. */
    ar_i32 (*scroll_of)(void *ud, ar_i32 index);
    ar_i32 (*scroll_x_of)(void *ud, ar_i32 index);

    /* Somewhere to put fragments. May be null, in which case inline boxes are
       not split -- which is what every caller before fragmentation got. */
    ar_frag *frags;
    ar_i32   frag_cap;
    ar_i32   frag_used;

    /* How many boxes the tree has, so a pass that lays out a subtree again
       knows where the array ends. Set by ar_layout_solve. */
    ar_i32 node_count;
} ar_layout_env;

void ar_layout_solve(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env);

/* ------------------------------------------------------------------------
 * Block formatting -- ar_layout_block.c
 * ------------------------------------------------------------------------ */

/* The larger of the positive parts plus the smaller of the negative parts. */
ar_i32 ar_margin_collapse(ar_i32 a, ar_i32 b);

int ar_is_block(const ar_node *n);
int ar_is_block_container(const ar_node *n);
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

/* How far one wheel notch goes. Three lines of an eight pixel face, which is
   what every toolkit settled on and what the hand expects. */
#define AR_SCROLL_STEP 30

/* Drawn inside the container's right edge rather than taken out of its width:
   a scrollbar that appears and reflows the text beside it makes the interface
   jump, and on a machine where relayout costs milliseconds it does so
   visibly. */
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

/* ------------------------------------------------------------------------
 * Tables -- ar_layout_table.c
 * ------------------------------------------------------------------------ */

int ar_is_table(const ar_node *n);

/* Rows and row groups: the table placed their children, so the ordinary
   sweep must not lay them out again. */
int ar_is_table_internal(const ar_node *n);

/* A cell, which is a block for whatever is inside it. */
int ar_is_grid(const ar_node *n);
int ar_is_table_block(const ar_node *n);

/* Whether this box paints itself -- its background, its border and its text.
   Its children are asked the same question separately, because `visibility`
   inherits and a child may say `visible` and come back. */
int ar_box_paints(const ar_node *n);

/* ------------------------------------------------------------------------
 * Shared box alignment, in ar_align.c
 *
 * Axis 0 is x, axis 1 is y.
 *
 * These eleven are here, in the header, and not in ar_align.c beside the rest
 * of alignment. That is a measurement rather than a preference.
 *
 * They were static in ar_layout.c, where the compiler inlined every one of
 * them into the loops that use them. When flexbox got a file of its own in
 * 0.8.0 and needed them too, they moved to ar_align.c and stopped being
 * static -- and a one-line ternary called from another translation unit
 * cannot be inlined without link-time optimisation, which this project does
 * not require of anybody building it. Every `n->style.v[AR_P_MARGIN_LEFT]`
 * that had become `ar_axis_margin_lead(&n->style, axis)` turned back into a
 * call, several per box per pass, in the hottest loops in the engine.
 *
 * It cost 2.2x of the layout phase and nobody saw it, because the release that
 * did it measured the scenes it added rather than the scenes it slowed down.
 * flat_1k's layout went 161 -> 361 us across 0.8.0 and came back to 171 when
 * these moved here.
 *
 * So: keep them in the header, keep them one line each, and do not move them
 * back to a .c file for tidiness. The tidy version is the slow one.
 * ------------------------------------------------------------------------ */
static AR_INLINE ar_i32 ar_axis_main(const ar_node *n)
{
    return n->style.v[AR_P_DIRECTION] == AR_DIR_COLUMN ? 1 : 0;
}

static AR_INLINE ar_i32 ar_axis_pad_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_TOP] : s->v[AR_P_PAD_LEFT];
}

static AR_INLINE ar_i32 ar_axis_pad_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_PAD_BOTTOM] : s->v[AR_P_PAD_RIGHT];
}

static AR_INLINE ar_i32 ar_axis_margin_lead(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_TOP] : s->v[AR_P_MARGIN_LEFT];
}

static AR_INLINE ar_i32 ar_axis_margin_trail(const ar_style *s, ar_i32 axis)
{
    return axis ? s->v[AR_P_MARGIN_BOTTOM] : s->v[AR_P_MARGIN_RIGHT];
}

static AR_INLINE ar_i32 ar_axis_size_prop(ar_i32 axis)
{
    return axis ? AR_P_HEIGHT : AR_P_WIDTH;
}

static AR_INLINE ar_i32 ar_axis_min_prop(ar_i32 axis)
{
    return axis ? AR_P_MIN_HEIGHT : AR_P_MIN_WIDTH;
}

/*
 * Unlike the size and min pair above, these two are *wide* properties: they
 * default to a sentinel meaning "no maximum", which does not fit in v[]. So a
 * caller holding this result must read it with ar_style_get, never with v[].
 *
 * That is not a style preference. v[] would be indexed here by a value rather
 * than a constant, so -Warray-bounds cannot catch the mistake, and the result
 * would be whichever bytes follow the array.
 */
static AR_INLINE ar_i32 ar_axis_max_prop(ar_i32 axis)
{
    return axis ? AR_P_MAX_HEIGHT : AR_P_MAX_WIDTH;
}

static AR_INLINE ar_i32 *ar_axis_pos(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->y : &r->x;
}

static AR_INLINE ar_i32 *ar_axis_size(ar_rect *r, ar_i32 axis)
{
    return axis ? &r->h : &r->w;
}

/* Clamped to the pair, and then to zero: a negative size is not a size, and
   every caller here would otherwise have to say so itself. */
static AR_INLINE ar_i32 ar_clamp(ar_i32 v, ar_i32 lo, ar_i32 hi)
{
    if (v < lo)
    {
        v = lo;
    }
    if (v > hi)
    {
        v = hi;
    }
    return v < 0 ? 0 : v;
}

void   ar_align_distribute(ar_i32 mode, ar_i32 free, ar_i32 count, ar_i32 *out_lead,
                           ar_i32 *out_between);
ar_i32 ar_align_from_justify(ar_i32 justify);
ar_i32 ar_align_self_offset(ar_i32 mode, ar_i32 free);

/* ------------------------------------------------------------------------
 * The flex formatting context, in ar_layout_flex.c
 * ------------------------------------------------------------------------ */
void   ar_flex_place(ar_node *nodes, ar_i32 i, ar_layout_env *env);
ar_i32 ar_flex_content_cross(ar_node *nodes, ar_i32 i, ar_layout_env *env);

/* Places a flex container whose cross size is whatever its lines come to. */
void ar_flex_place_auto(ar_node *nodes, ar_i32 i, ar_layout_env *env);

/* ------------------------------------------------------------------------
 * The grid formatting context, in ar_layout_grid.c
 *
 * It takes the sheet because a track list lives in a pool there rather than in
 * the style -- see the comment beside AR_P_GRID_COLS.
 * ------------------------------------------------------------------------ */
void   ar_grid_place(ar_node *nodes, ar_i32 i, const ar_sheet *sheet, ar_layout_env *env);
ar_i32 ar_grid_content_height(ar_node *nodes, ar_i32 i, const ar_sheet *sheet, ar_layout_env *env);

/* Both defined in ar_layout.c, which owns the sizing rules; the flex solver
   is the second caller and the reason they stopped being static. */
ar_i32 ar_resolve_size(const ar_node *ch, ar_i32 axis, ar_i32 inner, int stretch);

/* min-content, max-content and fit-content on any size property. Returns 0
   when the property is not one of them, leaving *out untouched. */
int ar_intrinsic_size(const ar_node *n, ar_i32 prop, ar_i32 axis, ar_i32 available, ar_i32 *out);

/* Give a box the axis it did not state, from the one it did and its ratio.
   Does nothing when there is no ratio, or when both axes were stated. */
void ar_apply_ratio(ar_node *n, int w_definite);
void ar_wrap_height(ar_node *nodes, ar_node *n, ar_i32 axis, int stretch, ar_layout_env *env);

/*
 * What a box's contents come to at a width, in the units fit[1] is in.
 *
 * For the sizing algorithms that have to know how tall an item will be
 * before they can decide the track, row or line it sits in. A measurement
 * only: the box is left needing to be placed again, and saying so.
 */
ar_i32 ar_content_height(ar_node *nodes, ar_i32 i, ar_i32 inner_w, ar_layout_env *env);
void   ar_table_align_cell(ar_node *nodes, ar_i32 i, ar_frag *frags, ar_i32 frag_n);
int    ar_is_table_cell(const ar_node *n);

/* The backward sweep's share: column constraints, and the two intrinsic widths
   they give the table box. No width exists yet, so nothing is placed. */
void ar_table_measure(ar_node *nodes, ar_i32 table);

/* How tall the table comes to at the width it now has. Called from the same
   place a paragraph's height is corrected. */
ar_i32 ar_table_height(ar_node *nodes, ar_i32 table, ar_layout_env *env);

/* The forward sweep's share: rectangles, from a grid already decided. */
void ar_table_place(ar_node *nodes, ar_i32 table, ar_layout_env *env);

/* Whether this box is in the top layer, which paints above every stacking
   context rather than merely above its siblings. */
int ar_in_top_layer(const ar_node *n);

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

/* Rewrites every anchor() and anchor-size() into a plain length, against the
   box each one names. Before placement, so nothing downstream knows about
   anchors. */
void ar_resolve_anchors(ar_node *nodes, ar_i32 count, ar_rect viewport);

/* Flips an anchored box to the anchor's other side when it left the viewport.
   After placement, because it is a reaction to where the box ended up. */
/*
 * Move one box and the fragments it was cut into. The only way to move a box:
 * a split inline is painted from its fragments' own rectangles, so touching
 * `rect` alone moves everything that reads a rectangle and nothing that is
 * drawn.
 */
void ar_shift_node(ar_node *nodes, ar_frag *frags, ar_i32 frag_n, ar_i32 i, ar_i32 dx, ar_i32 dy);

/* The same, for the box and everything beneath it. */
void ar_shift_subtree(ar_node *nodes, ar_frag *frags, ar_i32 frag_n, ar_i32 i, ar_i32 dx,
                      ar_i32 dy);

/* The face chain a box's text is measured and drawn through, chosen by its
   resolved `font-weight` and `font-style`. */
const ar_font_chain *ar_chain_for(const ar_ctx *c, const ar_node *n);

/*
 * Move a box to the rectangle just written into it, taking its subtree along.
 *
 * `was` is where the box stood before the caller assigned its new position.
 * Every algorithm that positions a box -- block flow, a grid track, a flex
 * line, a table row -- must call this after writing the rectangle. A box whose
 * contents were already laid out is *moved*, never repositioned; assigning
 * without this leaves everything inside it where it was, which is a page with
 * every rectangle correct and all of its text in the top-left corner.
 *
 * Does nothing for a box with no settled subtree, so it is always safe to call
 * and never needs a condition at the call site.
 */
void ar_settle_at(ar_node *nodes, ar_layout_env *env, ar_i32 i, ar_rect was);

void ar_position_try(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env);

void ar_position_out_of_flow(ar_node *nodes, ar_i32 count, ar_i32 i, ar_rect viewport,
                             ar_layout_env *env);

/* Lay a subtree out again at its root's current rectangle -- for a box that
   positioning gave a width the flow did not. See ar_layout.c. */
void ar_relayout_subtree(ar_node *nodes, ar_i32 count, ar_i32 root, ar_layout_env *env);
void ar_position_relative(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env);

int  ar_is_sticky(const ar_node *n);
void ar_position_sticky(ar_node *nodes, ar_i32 count, ar_rect viewport, ar_layout_env *env);

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

/* The same, for a box in its tree: an inline-block's baseline is the last line
   inside it, which needs the tree to find. */
ar_i32 ar_inline_baseline_of(const ar_node *nodes, ar_i32 i);

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
int ar_flows_children(const ar_node *n);

/* Whether this box is a `<br>`: a forced line break and nothing else. */
int ar_is_line_break(const ar_node *n);

ar_slot *ar_ctx_slot(ar_ctx *c, ar_u32 key);

/* The same lookup without claiming an empty slot, for queries. */
const ar_slot *ar_ctx_slot_find(const ar_ctx *c, ar_u32 key);

#endif /* AR_NODE_H */
