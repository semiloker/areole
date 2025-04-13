/*
 * areole - the style model and the CSS subset.
 * SPDX-License-Identifier: MIT
 *
 * Not installed. Callers see stylesheets as text and boxes as selectors; this
 * is what the text turns into.
 */
#ifndef AR_CSS_H
#define AR_CSS_H

#include "areole.h"

/* ------------------------------------------------------------------------
 * Properties
 *
 * Every property is one slot, so a style is a flat array rather than a struct
 * with a field per property. Merging one style over another is then a loop
 * over set bits instead of a switch with a case per property, which is both
 * shorter and the thing that runs once per node per frame.
 * ------------------------------------------------------------------------ */
typedef enum ar_prop
{
    AR_P_DISPLAY = 0,
    AR_P_DIRECTION,
    AR_P_JUSTIFY,
    AR_P_ALIGN,
    AR_P_GAP,

    AR_P_PAD_TOP,
    AR_P_PAD_RIGHT,
    AR_P_PAD_BOTTOM,
    AR_P_PAD_LEFT,

    AR_P_MARGIN_TOP,
    AR_P_MARGIN_RIGHT,
    AR_P_MARGIN_BOTTOM,
    AR_P_MARGIN_LEFT,

    AR_P_WIDTH,
    AR_P_HEIGHT,
    AR_P_MIN_WIDTH,
    AR_P_MIN_HEIGHT,
    AR_P_MAX_WIDTH,
    AR_P_MAX_HEIGHT,

    AR_P_BACKGROUND,
    AR_P_COLOR,
    AR_P_BORDER_WIDTH,
    AR_P_BORDER_COLOR,
    AR_P_BORDER_RADIUS,

    AR_P_FONT_SIZE,
    AR_P_OVERFLOW,

    AR_P_COUNT
} ar_prop;

/* AR_P_COUNT must stay inside the 32 bit "which properties are set" mask. */
typedef char ar__prop_mask_fits[AR_P_COUNT <= 32 ? 1 : -1];

typedef enum ar_unit
{
    AR_UNIT_PX = 0,
    AR_UNIT_PCT,  /* per cent of the parent inner box */
    AR_UNIT_AUTO, /* size to content                  */
    AR_UNIT_GROW, /* take a share of the leftover     */
    AR_UNIT_KEYWORD,
    AR_UNIT_COLOR,

    /* The explicit cascade values. They are units rather than values because
       they say where the value comes from rather than what it is, and the
       decision is taken when the parent is known, which is after resolution. */
    AR_UNIT_INHERIT,
    AR_UNIT_INITIAL
} ar_unit;

/* Keyword values, all in one space so the parser can hand back one integer. */
enum
{
    AR_DISPLAY_NONE = 0,
    AR_DISPLAY_BLOCK,
    AR_DISPLAY_FLEX
};

enum
{
    AR_DIR_ROW = 0,
    AR_DIR_COLUMN
};

enum
{
    AR_JUSTIFY_START = 0,
    AR_JUSTIFY_CENTER,
    AR_JUSTIFY_END,
    AR_JUSTIFY_BETWEEN
};

enum
{
    AR_ALIGN_START = 0,
    AR_ALIGN_CENTER,
    AR_ALIGN_END,
    AR_ALIGN_STRETCH
};

enum
{
    AR_OVERFLOW_VISIBLE = 0,
    AR_OVERFLOW_HIDDEN,
    AR_OVERFLOW_SCROLL
};

typedef struct ar_style
{
    ar_i32 v[AR_P_COUNT];
    ar_u8  unit[AR_P_COUNT];

    /* Which properties a stylesheet actually stated for this box, as opposed
       to which have a value -- every property always has a value. Inheritance
       needs the difference: a box that says nothing about colour takes its
       parent's, and a box that says `color: black` does not, even though both
       end up with a colour. */
    ar_u32 set;
} ar_style;

/* Pseudo-class of a rule, and the matching state of a node. */
enum
{
    AR_STATE_NONE = 0,
    AR_STATE_HOVER = 1 << 0,
    AR_STATE_ACTIVE = 1 << 1,
    AR_STATE_FOCUS = 1 << 2
};

/*
 * A selector can name several classes and an element can carry several, and
 * `.card.selected` has to match a box that is both. Four is the ceiling on
 * each: a rule naming five classes and a box carrying five are both things
 * nobody writes, and the alternative is a variable-length list inside a struct
 * that must stay copyable.
 */
#define AR_MAX_CLASSES 4

typedef struct ar_classes
{
    ar_u32 h[AR_MAX_CLASSES];
    ar_i32 n;
    /* All of them mixed into one word, which is what the resolved-style cache
       keys on. Order-independent, because a box carrying .a.b is the same box
       as one carrying .b.a. */
    ar_u32 combined;
} ar_classes;

/*
 * Combinators.
 *
 * A selector like `.page .card` matches a card that has a page somewhere above
 * it, which makes the resolved style depend on where a box sits rather than
 * only on what it is. That is precisely what the resolved-style cache cannot
 * key on, so rules carrying a combinator are held apart and resolved per box
 * without it. A stylesheet with no combinators pays nothing for this.
 *
 * Three context parts, so `#app .page .card` fits and `a b c d e` does not. A
 * selector that deep is describing the document's structure rather than the
 * element, and the rule that would be quicker to write is usually a class.
 */
enum
{
    AR_COMB_NONE = 0,   /* the subject: the rightmost compound      */
    AR_COMB_DESCENDANT, /* a space: anywhere above                  */
    AR_COMB_CHILD,      /* >: the immediate parent                  */
    AR_COMB_ADJACENT,   /* +: the sibling immediately before        */
    AR_COMB_SIBLING     /* ~: any sibling before                    */
};

#define AR_MAX_SEL_PARTS 3

typedef struct ar_sel_part
{
    ar_u32     tag;
    ar_classes klass;
    ar_u32     id;
    ar_u8      comb; /* how this part reaches the one to its right */
} ar_sel_part;

typedef struct ar_rule
{
    ar_u32     tag; /* hash, 0 means any */
    ar_classes klass;
    ar_u32     id; /* hash, 0 means any */

    /* Nearest first: ctx[0] is the part immediately left of the subject. Zero
       parts means a simple rule, which is the cacheable kind. */
    ar_sel_part ctx[AR_MAX_SEL_PARTS];
    ar_i32      nctx;
    ar_u8  state; /* required state bits, 0 means any */

    ar_u16 specificity;
    ar_u16 order; /* source position, to break specificity ties */
    ar_u32 set;   /* which properties this rule sets */

    /* Which of them were marked !important. Per declaration rather than per
       rule, because that is what CSS says and because a rule mixing the two is
       ordinary: `color: red !important; width: 10px;` means one of them wins
       against everything and the other does not. */
    ar_u32 important;
    ar_style style;
} ar_rule;

/* ------------------------------------------------------------------------
 * The resolved style cache
 *
 * Style resolution is 50 to 89 per cent of every tree-driven frame, measured,
 * and it grows linearly with rule count because every box is matched against
 * every rule. The scene that shows it plainest is identical_siblings: a
 * thousand boxes carrying one class, which resolve to the same answer a
 * thousand times and spend four fifths of the frame doing so.
 *
 * The key is the tuple ar_sheet_resolve already takes. Two boxes with the same
 * tag, class, id and state cannot resolve differently, because nothing in the
 * resolver depends on anything else -- no inheritance, no positional selectors,
 * no custom properties. When 0.4.0 adds the cascade that stops being true, and
 * the key has to grow with it or the cache becomes a correctness bug.
 * ------------------------------------------------------------------------ */
typedef struct ar_cache_entry
{
    ar_u32   tag, klass, id;
    ar_u8    state;
    ar_u8    used;
    ar_style style;
} ar_cache_entry;

typedef struct ar_sheet
{
    ar_rule *rules;
    ar_u16   count;
    ar_u16   capacity;

    /* Dropped wholesale whenever a stylesheet is added, which is the only
       thing that can invalidate it. Adding a stylesheet is a startup
       operation, so this never happens in a frame. */
    /* Whether any rule in this sheet carries a combinator. A sheet without
       one skips the contextual pass entirely, which is most sheets. */
    int has_contextual;

    ar_cache_entry *cache;
    ar_u16          cache_cap;
    ar_u32          cache_hits, cache_misses;

    /* Parsing never aborts. One malformed declaration should not silently
       discard the ninety that follow it, so errors are counted and reported
       rather than thrown. */
    ar_u32 errors;
    ar_u32 first_error_offset;
} ar_sheet;

ar_u32 ar_hash(const char *s, ar_u32 len);

void ar_style_defaults(ar_style *s);

/*
 * Inheritance.
 *
 * Copies the inherited properties the child did not state for itself. CSS
 * inherits a specific list -- colour and text properties, not layout ones --
 * because inheriting a width would be nonsense and inheriting a colour is what
 * makes a stylesheet short.
 *
 * Applied per box after the selectors have been resolved, deliberately: it
 * means the resolved-style cache still caches only what the selectors produced,
 * which does not depend on where the box sits in the tree. Caching after
 * inheritance would need the parent in the key and would be a different and
 * much worse cache.
 */
void ar_style_inherit(ar_style *child, const ar_style *parent);

/* Non-zero if this property inherits. One table, so adding a property to the
   list is a one-line change and cannot disagree with itself. */
int ar_prop_inherits(ar_i32 prop);
void ar_style_merge(ar_style *dst, const ar_style *src, ar_u32 set);

void ar_sheet_init(ar_sheet *sheet, ar_rule *storage, ar_u16 capacity);
void ar_sheet_set_cache(ar_sheet *sheet, ar_cache_entry *storage, ar_u16 capacity);
void ar_sheet_cache_clear(ar_sheet *sheet);
void ar_sheet_parse(ar_sheet *sheet, const char *css);

/* Resolves the style for one box. Rules already sit in ascending specificity
   order, so applying them in order leaves the winner on top. */
/* Not const: resolving populates the cache. */
void ar_sheet_resolve(ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                      ar_u8 state, ar_style *out);

/* Splits a selector such as div.card#first into its three hashes. Any part may
   be absent. Returns 0 if the selector is malformed. */
int ar_selector_split(const char *sel, ar_u32 *tag, ar_classes *klass, ar_u32 *id);

/* Adds a class hash if there is room and it is not already there. */
void ar_classes_add(ar_classes *c, ar_u32 hash);
void ar_classes_clear(ar_classes *c);

/* Every class in `want` is present in `have`. That direction is the whole of
   compound matching: a rule naming two classes matches a box carrying three. */
int ar_classes_contains(const ar_classes *have, const ar_classes *want);

/* Does one part of a selector describe this element? */
int ar_sel_part_matches(const ar_sel_part *p, ar_u32 tag, const ar_classes *klass, ar_u32 id);

/*
 * The contextual pass.
 *
 * `find` is asked for the element at a given relation from the current one and
 * fills in its tag, classes and id; it returns zero when there is no such
 * element. That indirection is how this stays ignorant of the box tree, which
 * lives a layer up and which this file has no business knowing about.
 */
typedef int (*ar_sel_walk)(void *ud, ar_i32 from, ar_i32 comb, ar_i32 *out_index, ar_u32 *tag,
                           ar_classes *klass, ar_u32 *id);

void ar_sheet_resolve_contextual(const ar_sheet *sheet, ar_i32 index, ar_u32 tag,
                                 const ar_classes *klass, ar_u32 id, ar_u8 state,
                                 ar_sel_walk find, void *ud, ar_style *out);

#endif /* AR_CSS_H */
