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

    /* ------------------------------------------------------------------
     * 0.8.0: the rest of flexbox
     *
     * areole shipped a subset -- direction, gap, justify, align and a `grow`
     * keyword that is not CSS. That was the right subset to start with and it
     * is not flexbox: anything written against real CSS uses these.
     * ------------------------------------------------------------------ */
    AR_P_FLEX_WRAP,
    AR_P_FLEX_BASIS,

    /*
     * The two factors, in thousandths.
     *
     * `flex-grow: 2.5` is a real declaration and there is no floating point in
     * this engine, so the value carried is 2500. Thousandths rather than
     * hundredths because `flex: 1 1 0` against `flex-grow: 0.001` is a ratio
     * somebody writes on purpose, and because the resolution loop divides by
     * the sum of the factors -- a coarse unit there is a visible pixel.
     */
    AR_P_FLEX_GROW,
    AR_P_FLEX_SHRINK,

    AR_P_ALIGN_SELF,
    AR_P_ALIGN_CONTENT,

    /* Signed: `order: -1` is how a box is moved to the front without being
       moved in the markup, which is the whole reason the property exists. */
    AR_P_ORDER,

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

    AR_P_BORDER_WIDTH,
    AR_P_BORDER_RADIUS,

    AR_P_FONT_SIZE,

    /*
     * `line-height`, which is the distance between baselines and not the
     * height of anything you can see.
     *
     * `normal` is the keyword and the initial value, and it means the face's
     * own ascent + descent + line gap -- exactly what every line box in this
     * engine was before the property existed, so a document that does not
     * mention it lays out to the same pixel.
     *
     * A unitless number is the form authors actually write, and it is a
     * multiplier of `font-size` rather than a length, which is why it
     * inherits usefully: `body { line-height: 1.5 }` gives a 32px heading a
     * 48px line and a 16px paragraph a 24px one. A length inherits as the
     * length and gives both 24. That difference is the whole reason CSS has
     * the unitless form, so it is carried as AR_UNIT_NUMBER in thousandths,
     * the same convention the flex factors use.
     */
    AR_P_LINE_HEIGHT,

    /*
     * `font-weight` as a number from 1 to 1000, which is what CSS Fonts 4
     * made the property: `normal` is 400 and `bold` is 700, and the keywords
     * are spellings of numbers rather than a separate kind of value. Storing
     * the number means `font-weight: 600` needs no new machinery the day a
     * variable face arrives.
     *
     * `bolder` and `lighter` are relative to the parent's computed value and
     * are not here: they need the cascade to resolve against an inherited
     * number rather than against a keyword, which is a different shape from
     * everything else in this table.
     */
    AR_P_FONT_WEIGHT,

    /* `font-style`: normal or italic. `oblique` is a synonym here, as it is
       in most faces -- a real oblique is a synthesised slant of the upright,
       and choosing between a designed italic and a slanted roman is a font
       database's job. */
    AR_P_FONT_STYLE,

    /* AR_P_OVERFLOW is the block axis, which is the one that existed when
       there was only one: the scroll range has always been vertical. The
       shorthand `overflow` sets both, and ar_overflow_x/ar_overflow_y apply the
       specification's rule that a `visible` paired with anything else becomes
       `auto`. */
    AR_P_OVERFLOW,
    AR_P_OVERFLOW_X,

    /* Same axis convention as the overflow pair above: the unsuffixed one is
       the block axis. Only a scroll container reads these, so they cost every
       box a slot in ar_style for something few boxes use -- which is the trade
       the whole flat-style design makes, and is why AR_PSET_WORDS is worth
       watching as the property count climbs. */
    AR_P_OVERSCROLL,
    AR_P_OVERSCROLL_X,

    /* The bar areole draws for itself. It is drawn rather than asked for, so
       these are honoured on every platform and look the same on all of them,
       which is the one thing a native scrollbar can never promise. */
    AR_P_SCROLLBAR_WIDTH,
    AR_P_SCROLLBAR_GUTTER,

    /*
     * The two insets snapping and scroll-into-view are measured against.
     *
     * scroll-padding shrinks the scrollport: it belongs to the container and
     * says "do not consider this strip to be visible", which is how a sticky
     * header stops covering the thing just scrolled to.
     *
     * scroll-margin grows the target: it belongs to the child and says "bring
     * this much of my surroundings along with me".
     *
     * Four sides each, adjacent and in top/right/bottom/left order, because
     * the shorthand expansion keys off the first slot of a contiguous group.
     */
    AR_P_SCROLL_PAD_TOP,
    AR_P_SCROLL_PAD_RIGHT,
    AR_P_SCROLL_PAD_BOTTOM,
    AR_P_SCROLL_PAD_LEFT,

    AR_P_SCROLL_MARGIN_TOP,
    AR_P_SCROLL_MARGIN_RIGHT,
    AR_P_SCROLL_MARGIN_BOTTOM,
    AR_P_SCROLL_MARGIN_LEFT,

    /* Snapping. The type is on the container, the align and the stop are on
       each child -- which is the split that makes a carousel expressible: the
       track declares that it snaps and the slides declare where. */
    AR_P_SCROLL_SNAP_TYPE,
    AR_P_SCROLL_SNAP_ALIGN,
    AR_P_SCROLL_SNAP_STOP,

    /* Whether this container keeps its reading position when something above
       the fold changes size. On by default, as CSS says, because the whole
       point is that nobody should have to ask for it. */
    AR_P_OVERFLOW_ANCHOR,

    AR_P_TEXT_ALIGN,
    AR_P_VERTICAL_ALIGN,
    AR_P_FLOAT,
    AR_P_CLEAR,

    AR_P_POSITION,
    AR_P_TOP,
    AR_P_RIGHT,
    AR_P_BOTTOM,
    AR_P_LEFT,
    AR_P_Z_INDEX,
    AR_P_BOX_SIZING,

    /*
     * `overlay`: whether this box is in the top layer.
     *
     * A property rather than a magic tag, for two reasons. ar_stack.c decides
     * paint order by reading n->style.v[...] and nothing else, so a property
     * costs it one test; and 0.14.2 schedules `overlay` as a *transitionable*
     * property, so the roadmap is already written against it being one.
     *
     * A deviation, named rather than discovered: CSS makes `overlay`
     * UA-controlled -- a page cannot put itself in the top layer, only
     * <dialog> and popover can. areole has no UA stylesheet and no elements
     * yet, so a stylesheet sets it. When 0.9.1's UA sheet lands it sets this
     * on dialog[open] and nothing here changes.
     */
    AR_P_OVERLAY,

    /*
     * `inert`: this box and its subtree take no pointer input.
     *
     * An HTML attribute rather than a CSS property, and areole has no
     * attributes -- there is no parser and no element type. A property stands
     * in until 0.9.0, on the same terms as `overlay` above: when the parser
     * lands, `inert` on an element sets this and nothing here changes.
     */
    AR_P_INERT,

    /*
     * `position-try`, cut down to the part that has stopped moving.
     *
     * The full property takes a list of fallback position sets, and
     * `position-try-fallbacks` is named in tracked-unstable.md as still
     * changing -- which the 0.6.3 plan schedules anyway, contradicting itself.
     * The mechanism is what matters and it is stable: if the box would leave
     * the viewport on an axis, flip it to the anchor's other side. That is
     * what a popover near an edge needs and it is expressible in one keyword.
     *
     * The list grammar stays out until its trigger fires, which is the promise
     * this project made about moving specifications.
     */
    AR_P_POSITION_TRY,

    /* ------------------------------------------------------------------
     * Tables
     *
     * `colspan` and `rowspan` are HTML attributes rather than CSS, and there
     * are no attributes here until the parser lands at 0.9.0. They are
     * properties on the same terms `inert` and `overlay` are: the mechanism
     * has to exist for the parser to drive, and this is the only way to say
     * it today. Named as a deviation rather than presented as CSS.
     * ------------------------------------------------------------------ */
    AR_P_TABLE_LAYOUT,
    AR_P_BORDER_COLLAPSE,
    AR_P_BORDER_SPACING,
    AR_P_COLSPAN,
    AR_P_ROWSPAN,

    /* ------------------------------------------------------------------
     * 0.7.1
     *
     * `visibility` is real CSS and inherits, which is the whole of what makes
     * `collapse` on a row useful: the row goes and its cells go with it
     * without any of them being named.
     * ------------------------------------------------------------------ */
    AR_P_VISIBILITY,
    AR_P_CAPTION_SIDE,
    AR_P_EMPTY_CELLS,

    /* ------------------------------------------------------------------
     * 0.8.0: grid
     *
     * A track list is not a value. `grid-template-columns: repeat(3, minmax(
     * 100px, 1fr))` is nine numbers and three kinds, and a style here is one
     * sixteen-bit slot per property -- so what the slot holds is an *index*
     * into a pool of tracks on the stylesheet, and the pool entry it points at
     * is a header carrying the count.
     *
     * The pool is on the sheet rather than the node because a track list comes
     * from a rule and never from a computation: it is parsed once when the
     * stylesheet is handed over and read every frame after that. Nothing about
     * it is per-box, and putting it per-box would have cost every box in the
     * interface a pointer for the sake of the handful that are grids.
     * ------------------------------------------------------------------ */
    AR_P_GRID_COLS,
    AR_P_GRID_ROWS,
    AR_P_GRID_AUTO_COLS,
    AR_P_GRID_AUTO_ROWS,
    AR_P_GRID_FLOW,

    /*
     * Where an item sits, as line numbers.
     *
     * Zero means `auto` -- CSS has no line zero, lines are numbered from one,
     * so zero is free to mean "the placement algorithm decides". A span is
     * carried as a negative: `span 2` is -2, which is unambiguous because a
     * negative line number in CSS counts from the end and areole does not do
     * that yet. Named here as the shortcut it is.
     */
    AR_P_GRID_COL_START,
    AR_P_GRID_COL_END,
    AR_P_GRID_ROW_START,
    AR_P_GRID_ROW_END,

    AR_P_JUSTIFY_ITEMS,
    AR_P_JUSTIFY_SELF,

    /* `gap` sets both; these are the two halves. Flex only ever used one, and
       a grid has two axes to put space between. */
    AR_P_ROW_GAP,
    AR_P_COL_GAP,

    /*
     * 0.8.1: `aspect-ratio`, as width over height in thousandths.
     *
     * `16 / 9` is 1777. A ratio is not a length and there is no floating
     * point here, so it is carried the way the flex factors are -- and for the
     * same reason: `4 / 3` and `1.33` are the same declaration and both have
     * to survive the parser.
     *
     * Zero means the property said nothing, which is safe because a ratio of
     * zero is not a ratio anybody can write.
     */
    AR_P_ASPECT_RATIO,

    /*
     * Everything above is stored in sixteen bits and everything below in
     * thirty-two, so this marker is load bearing rather than decorative: it is
     * the length of ar_style.v.
     *
     * A keyword needs three bits and a length needs a screen's worth of
     * pixels, so almost everything fits in sixteen. One wide array for all of
     * them spent 160 bytes a box to hold five properties' worth of range.
     *
     * The five below are the ones that genuinely need thirty-two:
     *
     *   the three colours   0xRRGGBB is twenty-four bits
     *   max-width/height    they default to a sentinel meaning "no maximum",
     *                       and ar__clamp applies them to *computed* rects.
     *                       A ten thousand row list is 240000 px of content,
     *                       so a 32767 sentinel would not mean unbounded, it
     *                       would mean a clamp -- which is a rendering bug
     *                       rather than a smaller struct.
     *
     * Putting them past the end of v[] rather than inside it is the part that
     * makes this safe: v[AR_P_COLOR] is now an out-of-bounds index on a
     * compile-time constant, which -Warray-bounds reports, where an in-range
     * index would have quietly returned a neighbouring property.
     */
    AR_P_NARROW_COUNT,

    AR_P_MAX_WIDTH = AR_P_NARROW_COUNT,
    AR_P_MAX_HEIGHT,
    AR_P_BACKGROUND,
    AR_P_COLOR,
    AR_P_BORDER_COLOR,

    /* `scrollbar-color` is two colours in one declaration, thumb then track,
       and they cascade as one. Two slots because a colour is a colour. */
    AR_P_SCROLLBAR_THUMB,
    AR_P_SCROLLBAR_TRACK,

    /*
     * `anchor-name` and `position-anchor`, as hashes of the ident.
     *
     * Wide because a hash is thirty-two bits and narrowing one would make two
     * different names collide silently -- the worst kind of bug this struct
     * can produce. Nothing ever needs the text back: an anchor is found by
     * comparing a name to a name, which a hash answers exactly.
     */
    AR_P_ANCHOR_NAME,
    AR_P_POSITION_ANCHOR,

    AR_P_COUNT
} ar_prop;

/* The wide properties are indexed off the end of the narrow block. Reach them
   with this rather than with v[], which has no room for them.
   Not called AR_RGB: areole.h already has one, and this block is no longer
   only colours. */
#define AR_WIDE(style, prop) ((style)->wide[(prop) - AR_P_NARROW_COUNT])

/*
 * Which properties a style or a rule has something to say about.
 *
 * One bit each. It was a single ar_u32 until positioning needed a thirty-first
 * property and then six more; C89 has no sixty-four bit integer this project
 * is willing to use, so it is two words and a handful of one-line functions.
 *
 * Passing it by value keeps every call site reading the way it did when it was
 * an integer, which is most of why this is a struct rather than an array.
 */
/* Two words held sixty-four properties and tables needed a sixty-fifth.
   Three words is twelve bytes per style, and a style is carried by every
   box and every rule -- so this is one of the more expensive constants in
   the file, and the assertion below is what makes the cost visible rather
   than letting a property silently fall off the end of the mask. */
/* Weight is a number; these two are the names CSS gives the ones people
   write. */
enum
{
    AR_WEIGHT_NORMAL = 400,
    AR_WEIGHT_BOLD = 700
};

enum
{
    AR_FONT_STYLE_NORMAL = 0,
    AR_FONT_STYLE_ITALIC = 1
};

#define AR_PSET_WORDS 3

typedef struct ar_pset
{
    ar_u32 w[AR_PSET_WORDS];
} ar_pset;

typedef char ar__prop_mask_fits[AR_P_COUNT <= 32 * AR_PSET_WORDS ? 1 : -1];

ar_pset ar_pset_none(void);
void    ar_pset_add(ar_pset *p, ar_i32 prop);
int     ar_pset_has(ar_pset p, ar_i32 prop);
int     ar_pset_any(ar_pset p);

/* a with everything in b removed, and a with everything in b added. */
ar_pset ar_pset_minus(ar_pset a, ar_pset b);
ar_pset ar_pset_plus(ar_pset a, ar_pset b);

typedef enum ar_unit
{
    AR_UNIT_PX = 0,
    AR_UNIT_PCT,  /* per cent of the parent inner box */
    AR_UNIT_AUTO, /* size to content                  */
    AR_UNIT_GROW, /* take a share of the leftover     */
    /*
     * A bare number, carried in thousandths.
     *
     * Only the flex factors use it. Every other number in CSS that is not a
     * length is a keyword, and giving those a unit of their own would mean
     * every reader of every property checking for it.
     */
    AR_UNIT_NUMBER,
    /* `flex-basis: content` -- size from the contents, which is not the same
       as `auto`: `auto` defers to `width`, and `content` ignores it. */
    AR_UNIT_CONTENT,
    /*
     * `1fr` -- a share of what is left over, and the only unit in CSS whose
     * value depends on every other value beside it. Carried in thousandths
     * like the flex factors, and for the same reason: `0.5fr` is a
     * declaration people write.
     */
    AR_UNIT_FR,
    AR_UNIT_KEYWORD,
    AR_UNIT_COLOR,

    /* The explicit cascade values. They are units rather than values because
       they say where the value comes from rather than what it is, and the
       decision is taken when the parent is known, which is after resolution. */
    AR_UNIT_INHERIT,
    AR_UNIT_INITIAL,

    /*
     * The intrinsic sizes.
     *
     * min-content is the narrowest a box can be without its contents
     * overflowing: for text, the widest word, since a word does not break.
     * max-content is the widest it would ever want to be: for text, the whole
     * string with no breaking at all.
     *
     * fit-content is the useful one, and is neither -- it is max-content
     * clamped to the space available, and then never narrower than
     * min-content. "As wide as it wants, but no wider than there is room for,
     * and never so narrow that it spills."
     *
     * They are units rather than properties because they are answers to
     * `width`, and because there were two property slots left.
     */
    AR_UNIT_MIN_CONTENT,
    AR_UNIT_MAX_CONTENT,
    AR_UNIT_FIT_CONTENT,

    /*
     * env(), one unit per name.
     *
     * A unit rather than a value for the same reason `inherit` is one: it says
     * where the number comes from, and the number is not known when the sheet
     * is parsed. The value slot carries the fallback, so `env(x, 12px)` parses
     * to this unit with a 12 in it and needs no second field.
     *
     * One unit per name rather than one unit and a packed name-and-fallback
     * pair, because unit[] is a byte per property with room to spare and v[]
     * is only sixteen bits. Packing both into v[] would have cost the fallback
     * most of its range to save a byte that was already there.
     *
     * They must stay in AR_ENV_* order: the slot is the offset from
     * AR_UNIT_ENV_FIRST, which is what lets resolution be a table lookup.
     */
    /*
     * anchor(side) and anchor-size(dimension).
     *
     * A unit for the same reason env() is one: it says where the number comes
     * from, and the number is not known until the anchor has been laid out.
     * The value slot carries which edge, so one unit covers all of them.
     */
    AR_UNIT_ANCHOR,

    /*
     * `calc()` and the maths functions, as an index into the sheet's program
     * pool rather than a number.
     *
     * A unit for the third time in this enum, and for the same reason `env()`
     * and `em` are: the number is not known when the sheet is parsed.
     * `calc(2.75rem + 2px)` needs a font size, `calc(50vw / 3)` needs a
     * surface, and neither exists yet.
     *
     * **Deliberately below AR_UNIT_REL_FIRST and AR_UNIT_ENV_FIRST.** Both of
     * those are range tests -- `u >= AR_UNIT_REL_FIRST` means "a relative
     * length" -- so a calc placed above them would be read as one and sent
     * into the font-metric path with a pool index in its value slot. That is
     * a box sized by whatever the index happened to be.
     */
    AR_UNIT_CALC,

    /*
     * `var(--name)`, as an index into the sheet's reference pool.
     *
     * Below AR_UNIT_REL_FIRST for the reason AR_UNIT_CALC is, and resolved in
     * the same pass: the value a name has depends on where the box sits, so
     * it is not knowable until there is a box.
     */
    AR_UNIT_VAR,

    AR_UNIT_ENV_FIRST,
    AR_UNIT_ENV_SAFE_TOP = AR_UNIT_ENV_FIRST,
    AR_UNIT_ENV_SAFE_RIGHT,
    AR_UNIT_ENV_SAFE_BOTTOM,
    AR_UNIT_ENV_SAFE_LEFT,
    AR_UNIT_ENV_TITLEBAR_X,
    AR_UNIT_ENV_TITLEBAR_Y,
    AR_UNIT_ENV_TITLEBAR_W,
    AR_UNIT_ENV_TITLEBAR_H,
    AR_UNIT_ENV_LAST = AR_UNIT_ENV_TITLEBAR_H,

    /*
     * The relative lengths.
     *
     * A unit rather than a converted number for the reason env() is one: the
     * number is not known when the sheet is parsed. `em` waits for the font
     * size inheritance settles, `vh` waits for a viewport that can change
     * while the stylesheet does not, and both are resolved on the copy the
     * style cache hands back rather than inside it.
     *
     * The value slot carries **hundredths of the unit**, not pixels: `1.5em`
     * is 150 here. Sixteen bits of whole units would have made `0.5em` round
     * to zero at parse time, which is where relative units are most often
     * written.
     *
     * **The order is load-bearing three times over** and adding a unit in the
     * middle breaks all three. The six font metrics run em, ex, ch, cap, ic,
     * lh; the root-relative six repeat them in that order, so `u - AR_UNIT_EM`
     * modulo six is which metric and divided by six is whether it is the
     * element's font or the root's. The four viewport families repeat w, h,
     * min, max, i, b for the same arithmetic.
     */
    AR_UNIT_REL_FIRST,

    AR_UNIT_EM = AR_UNIT_REL_FIRST,
    AR_UNIT_EX,
    AR_UNIT_CH,
    AR_UNIT_CAP,
    AR_UNIT_IC,
    AR_UNIT_LH,

    AR_UNIT_REM,
    AR_UNIT_REX,
    AR_UNIT_RCH,
    AR_UNIT_RCAP,
    AR_UNIT_RIC,
    AR_UNIT_RLH,

    AR_UNIT_VIEW_FIRST,

    AR_UNIT_VW = AR_UNIT_VIEW_FIRST,
    AR_UNIT_VH,
    AR_UNIT_VMIN,
    AR_UNIT_VMAX,
    AR_UNIT_VI,
    AR_UNIT_VB,

    AR_UNIT_SVW,
    AR_UNIT_SVH,
    AR_UNIT_SVMIN,
    AR_UNIT_SVMAX,
    AR_UNIT_SVI,
    AR_UNIT_SVB,

    AR_UNIT_LVW,
    AR_UNIT_LVH,
    AR_UNIT_LVMIN,
    AR_UNIT_LVMAX,
    AR_UNIT_LVI,
    AR_UNIT_LVB,

    AR_UNIT_DVW,
    AR_UNIT_DVH,
    AR_UNIT_DVMIN,
    AR_UNIT_DVMAX,
    AR_UNIT_DVI,
    AR_UNIT_DVB,

    AR_UNIT_VIEW_LAST = AR_UNIT_DVB,
    AR_UNIT_REL_LAST = AR_UNIT_DVB
} ar_unit;

/* The six font metrics, and the six viewport axes, in the order the enum
   above fixes. Resolution indexes both by subtraction. */
#define AR_UNIT_METRIC_COUNT 6
#define AR_UNIT_VIEW_AXES    6

/*
 * The environment a stylesheet can ask about.
 *
 * Eight numbers the core cannot work out for itself: four safe-area insets and
 * a titlebar rectangle. A backend that knows them says so through
 * ar_set_safe_area and ar_set_titlebar_area; one that does not leaves them
 * unknown, and every env() referring to them takes its fallback.
 *
 * Unknown is not the same as zero, and the difference is the whole reason the
 * `known` flags exist. A windowed desktop has real insets of zero -- there is
 * no notch and nothing is covered -- so `env(safe-area-inset-top, 20px)` must
 * resolve to 0, not to 20. A backend that has never heard of safe areas leaves
 * them unknown, and the same declaration must resolve to 20.
 */
enum
{
    AR_ENV_SAFE_TOP = 0,
    AR_ENV_SAFE_RIGHT,
    AR_ENV_SAFE_BOTTOM,
    AR_ENV_SAFE_LEFT,
    AR_ENV_TITLEBAR_X,
    AR_ENV_TITLEBAR_Y,
    AR_ENV_TITLEBAR_W,
    AR_ENV_TITLEBAR_H,
    AR_ENV_COUNT
};

typedef struct ar_env
{
    ar_i32 v[AR_ENV_COUNT];
    ar_u8  known[AR_ENV_COUNT];

    /*
     * `viewport-fit`. Auto is the initial value and means the layout viewport
     * is already the safe rectangle, so the insets a stylesheet sees are zero
     * however large the real ones are -- there is nothing left for it to avoid.
     * Cover hands the stylesheet the whole display and the real insets with it.
     *
     * The two move together or not at all: a viewport inset by the safe area
     * *and* env() reporting that inset would take it off twice.
     */
    ar_u8 fit_cover;
} ar_env;

/* The value an env() name resolves to, given what the backend has said.
   `fallback` is what the declaration wrote after the comma. */
ar_i32 ar_env_value(const ar_env *e, ar_i32 slot, ar_i32 fallback);

/* Keyword values, all in one space so the parser can hand back one integer. */
enum
{
    AR_DISPLAY_NONE = 0,
    AR_DISPLAY_BLOCK,
    AR_DISPLAY_FLEX,

    /*
     * Inline-level, and atomic: it sits on a line beside its siblings and is
     * never split across two of them.
     *
     * That last part is why this is `inline-block` and not `inline`. A real
     * inline box fragments -- half on one line, half on the next, with borders
     * and padding on the first and last fragment only -- and a fragment is a
     * second rectangle for a box that has room for one. Fragmentation is the
     * next piece of 0.5.0; this is the line box model it will need.
     */
    AR_DISPLAY_INLINE_BLOCK,

    /*
     * Inline, and not atomic: its text flows into the line boxes around it and
     * splits wherever a line ends.
     *
     * The difference from inline-block is fragmentation. `<b>` in the middle of
     * a paragraph is one box and may be two rectangles, or five, and the ones
     * in the middle get no borders or padding on the sides where they were cut.
     */
    AR_DISPLAY_INLINE,

    /* ------------------------------------------------------------------
     * The table display values.
     *
     * Kept contiguous and in this order so a range test can ask "is this
     * table-internal" in one comparison rather than nine. AR_DISPLAY_TABLE
     * itself is deliberately outside that range: a table box is a block-level
     * box that happens to lay its children out differently, and it is the one
     * value that appears in ordinary flow.
     * ------------------------------------------------------------------ */
    AR_DISPLAY_TABLE,

    AR_DISPLAY_TABLE_ROW_GROUP,
    AR_DISPLAY_TABLE_HEADER_GROUP,
    AR_DISPLAY_TABLE_FOOTER_GROUP,
    AR_DISPLAY_TABLE_ROW,
    AR_DISPLAY_TABLE_CELL,
    AR_DISPLAY_TABLE_COLUMN_GROUP,
    AR_DISPLAY_TABLE_COLUMN,
    AR_DISPLAY_TABLE_CAPTION,

    /* After every table value, so the contiguous internal range above stays
       contiguous -- that range is compared as a range in three places. */
    AR_DISPLAY_GRID,

    /*
     * `display: contents`: the box generates none, and its children become
     * its parent's for layout.
     *
     * Small to describe and awkward to mean, because it breaks the assumption
     * every layout pass makes -- that the box tree and the layout tree are the
     * same tree. It is worth it because it is the only way to put a semantic
     * wrapper around grid items without breaking the grid: three cards in a
     * `<section>` inside a grid are three items only if the section generates
     * no box.
     */
    AR_DISPLAY_CONTENTS,

    /*
     * `display: list-item`: a block box that also generates a marker.
     *
     * The box is the whole of it here. A marker needs `list-style`, `::marker`
     * and counters, which are 0.5.3 -- so an `<li>` lays out exactly as a
     * block and shows no bullet, which is what it did before this value
     * existed. What the value buys is that it is no longer a *lie*: `<li>` and
     * `<summary>` are list items in every browser and were `block` here, and a
     * corpus that compares computed values against one said so.
     *
     * After AR_DISPLAY_CONTENTS so the table-internal range above stays
     * contiguous -- that range is compared as a range in three places.
     */
    AR_DISPLAY_LIST_ITEM,

    AR_DISPLAY_TABLE_INTERNAL_FIRST = AR_DISPLAY_TABLE_ROW_GROUP,
    AR_DISPLAY_TABLE_INTERNAL_LAST = AR_DISPLAY_TABLE_CAPTION
};

/* Table keyword values. */
enum
{
    AR_TABLE_LAYOUT_AUTO = 0,
    AR_TABLE_LAYOUT_FIXED = 1,

    AR_BORDER_SEPARATE = 0,
    AR_BORDER_COLLAPSE = 1
};

enum
{
    AR_VIS_VISIBLE = 0,
    AR_VIS_HIDDEN,
    /*
     * `collapse` is not a third shade of hidden.
     *
     * On a row or a column it removes the track and closes the gap, and it
     * does so **without recomputing the column widths** -- which is the entire
     * difference from `display: none` and the reason the value exists. A
     * filter that hides half a table's rows should not make every remaining
     * column jump to a new width. Anywhere else it means `hidden`.
     */
    AR_VIS_COLLAPSE
};

enum
{
    AR_CAPTION_TOP = 0,
    AR_CAPTION_BOTTOM
};

enum
{
    AR_EMPTY_SHOW = 0,
    AR_EMPTY_HIDE
};

enum
{
    AR_TEXT_ALIGN_LEFT = 0,
    AR_TEXT_ALIGN_RIGHT,
    AR_TEXT_ALIGN_CENTER
};

/* Where an inline-level box sits in its line box. `baseline` is the default,
   and the only one that needs the line to have a baseline at all. */
/*
 * What a stated width or height measures.
 *
 * `content-box` is CSS's default and therefore areole's: padding is added on
 * top, so `width: 100px; padding: 10px` occupies 120. `border-box` is what
 * nearly every stylesheet written this decade asks for on its first line, and
 * what areole itself did until this existed -- a divergence the browser
 * comparison found the moment positioning first put a stated size and padding
 * on the same box.
 *
 * The default follows the specification rather than the fashion, because a
 * sheet written for the web has to lay out the way it does on the web. Anyone
 * who prefers the other one is one rule away from it.
 */
enum
{
    AR_BOX_CONTENT = 0,
    AR_BOX_BORDER
};

enum
{
    AR_POS_STATIC = 0,

    /* In flow, and shifted for painting only: it still occupies the space it
       would have, which is why it leaves a hole rather than closing one. */
    AR_POS_RELATIVE,

    /* Out of flow, against the padding box of the nearest positioned
       ancestor -- which is why `position: relative` with no offsets of its own
       is not a no-op but the most-used line of CSS there is. */
    AR_POS_ABSOLUTE,

    /* Out of flow, against the viewport. */
    AR_POS_FIXED,

    /*
     * In flow until a threshold, then pinned -- and never outside the box it
     * belongs to, which is what makes a sticky section header hand over to the
     * next one instead of stacking up.
     */
    AR_POS_STICKY
};

enum
{
    AR_FLOAT_NONE = 0,
    AR_FLOAT_LEFT,
    AR_FLOAT_RIGHT
};

/* Bits, because `clear: both` is the two of them and asking about one side at
   a time is what the float list wants. */
enum
{
    AR_CLEAR_NONE = 0,
    AR_CLEAR_LEFT = 1,
    AR_CLEAR_RIGHT = 2,
    AR_CLEAR_BOTH = 3
};

enum
{
    AR_VALIGN_BASELINE = 0,
    AR_VALIGN_TOP,
    AR_VALIGN_MIDDLE,
    AR_VALIGN_BOTTOM
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
    AR_JUSTIFY_BETWEEN,
    /*
     * `around` gives every item a half gap at each end, so the space at the
     * edges is half the space between two items. `evenly` makes all of them
     * equal. They look alike in a mock-up and differ by exactly one half-gap
     * at each edge, which is the difference somebody eventually files a bug
     * about.
     */
    AR_JUSTIFY_AROUND,
    AR_JUSTIFY_EVENLY
};

enum
{
    AR_ALIGN_START = 0,
    AR_ALIGN_CENTER,
    AR_ALIGN_END,
    AR_ALIGN_STRETCH,
    /* `baseline` aligns the first lines of the items rather than their boxes,
       which is what makes a row of labels of different sizes read as one line
       of text rather than as boxes that happen to be near each other. */
    AR_ALIGN_BASELINE,

    /* Only on `align-self`, and its initial value: defer to the container's
       `align-items`. Kept in the same enum so one switch serves both. */
    AR_ALIGN_AUTO,

    /* Only on `align-content`, which distributes the *lines* of a wrapped
       container and therefore has the justify vocabulary as well. */
    AR_ALIGN_BETWEEN,
    AR_ALIGN_AROUND,
    AR_ALIGN_EVENLY,

    /*
     * `safe` and `unsafe`, as a bit rather than a value.
     *
     * `align-items: safe center` is two words meaning one thing, so they
     * cannot be alternatives in the same slot. The bit sits above every value
     * and readers mask it off.
     *
     * What it buys: centring a box larger than its container puts half the
     * overflow *before* the start edge, where it cannot be scrolled to and
     * cannot be read. `safe` says to fall back to start alignment when that
     * would happen, which is the whole of the feature -- and it is why nobody
     * should have to know the difference to write centred content that stays
     * reachable.
     */
    AR_ALIGN_SAFE = 16,
    AR_ALIGN_MODE_MASK = 15
};

enum
{
    AR_GRID_FLOW_ROW = 0,
    AR_GRID_FLOW_COLUMN = 1,
    /* A bit rather than a third value: `grid-auto-flow: column dense` is two
       words and means both, so they cannot share a slot as alternatives. */
    AR_GRID_FLOW_DENSE = 2
};

enum
{
    AR_WRAP_NOWRAP = 0,
    AR_WRAP_WRAP,
    /* The lines are stacked from the far edge instead of the near one. The
       items inside each line keep their order; only the lines reverse. */
    AR_WRAP_WRAP_REVERSE
};

enum
{
    AR_OVERFLOW_VISIBLE = 0,
    AR_OVERFLOW_HIDDEN,

    /* Always scrollable, and always showing that it is. */
    AR_OVERFLOW_SCROLL,

    /* Scrollable only when there is somewhere to go, which is the whole
       difference between the two and the reason anybody writes `auto`. */
    AR_OVERFLOW_AUTO
};

enum
{
    /* A notch this container cannot use is offered to its ancestors. */
    AR_OVERSCROLL_AUTO = 0,

    /* It is not. The scroll stops at this boundary, which is what keeps a
       modal's wheel from scrolling the page behind it -- the single most
       common scrolling bug in any interface.

       `none` additionally suppresses the platform's overscroll affordance,
       the bounce or the glow. areole draws neither, so the two behave
       identically here and are kept apart anyway: a stylesheet saying `none`
       means it, and the day a backend grows a bounce this is where it looks. */
    AR_OVERSCROLL_CONTAIN,
    AR_OVERSCROLL_NONE
};

enum
{
    /* The widths are the drawn widths, not a request to the platform: areole
       has no platform scrollbar to ask. `none` still scrolls -- it hides the
       bar, it does not stop the wheel, which is what every implementation of
       this property has to get right or a list becomes unreachable. */
    AR_SCROLLBAR_AUTO = 0,
    AR_SCROLLBAR_THIN,
    AR_SCROLLBAR_HIDDEN
};

enum
{
    /*
     * areole's bar is an overlay, drawn inside the right edge rather than
     * taken out of the width, so a bar appearing never reflows anything and
     * the layout shift `stable` exists to prevent cannot happen here.
     *
     * What `stable` still buys is the overlap: an overlay bar is drawn on top
     * of the content beside it. `stable` reserves its width at the inline end
     * so the text stops before the bar instead of running under it.
     */
    AR_GUTTER_AUTO = 0,
    AR_GUTTER_STABLE,
    AR_GUTTER_BOTH_EDGES
};

/*
 * scroll-snap-type is an axis and a strictness, and they are independent, so
 * it is two fields packed into one property rather than one enum of every
 * combination -- `both mandatory` and `y proximity` are both sayable.
 *
 * The axis occupies the low bits and the strictness the next one up.
 */
enum
{
    AR_SNAP_AXIS_NONE = 0,
    AR_SNAP_AXIS_X,
    AR_SNAP_AXIS_Y,
    AR_SNAP_AXIS_BOTH,
    AR_SNAP_AXIS_MASK = 3,

    /* Land on a snap point only if one is close enough to be plausibly
       intended, which is what lets a long list still be scrolled anywhere.

       This is the zero state because CSS says an omitted strictness means
       `proximity`: `scroll-snap-type: y` is `y proximity`, not `y mandatory`.
       Getting that round the wrong way turns every ordinary scroll container
       with one snap declaration into one that cannot be scrolled off a slide. */
    AR_SNAP_PROXIMITY = 0,

    /* Always land on a snap point. */
    AR_SNAP_MANDATORY = 4
};

enum
{
    AR_SNAP_ALIGN_NONE = 0,
    AR_SNAP_ALIGN_START,
    AR_SNAP_ALIGN_CENTER,
    AR_SNAP_ALIGN_END
};

enum
{
    /* `overlay`. `none` is the initial value and is zero, so a box says
       nothing about the top layer unless it says something. */
    AR_OVERLAY_NONE = 0,
    AR_OVERLAY_AUTO = 1,

    /*
     * `modal` is areole's stand-in for what showModal() does: the top layer,
     * *and* everything outside it made inert. CSS has no such value -- the
     * modality comes from the element and its method, neither of which exists
     * here yet. Named as a deviation rather than presented as CSS.
     */
    AR_OVERLAY_MODAL = 2,

    AR_INERT_NONE = 0,
    AR_INERT_AUTO = 1,

    /* `position-try`. Flip on the axis that would leave the viewport. */
    AR_TRY_NONE = 0,
    AR_TRY_FLIP_BLOCK = 1,
    AR_TRY_FLIP_INLINE = 2,
    AR_TRY_FLIP_BOTH = 3,

    /* Which edge of the anchor an anchor() refers to. */
    AR_ANCHOR_SIDE_TOP = 0,
    AR_ANCHOR_SIDE_RIGHT = 1,
    AR_ANCHOR_SIDE_BOTTOM = 2,
    AR_ANCHOR_SIDE_LEFT = 3,
    AR_ANCHOR_SIDE_CENTER = 4,
    AR_ANCHOR_SIZE_WIDTH = 5,
    AR_ANCHOR_SIZE_HEIGHT = 6,

    AR_ANCHOR_AUTO = 0,
    AR_ANCHOR_NONE
};

enum
{
    AR_SNAP_STOP_NORMAL = 0,

    /* A gesture may not pass this box without stopping on it. It is what
       separates a good carousel from an infuriating one: without it a hard
       flick skips three slides. */
    AR_SNAP_STOP_ALWAYS
};

/* How near a snap point has to be, under `proximity`, as a fraction of the
   viewport. Chrome and Firefox both use about half; the exact figure is not
   specified, and anything in that region feels the same. */
#define AR_SNAP_PROXIMITY_NUM 1
#define AR_SNAP_PROXIMITY_DEN 2

typedef struct ar_style
{
    /* Sixteen bits, and the ceiling that implies is real: a stated length
       above 32767 px is clamped when it is parsed rather than wrapping here.
       The largest thing anyone lays out is a scroll container's content, and
       that is computed rather than stated. */
    ar_i16 v[AR_P_NARROW_COUNT];

    /* The five that need the range. AR_WIDE indexes this. */
    ar_i32 wide[AR_P_COUNT - AR_P_NARROW_COUNT];

    ar_u8 unit[AR_P_COUNT];

    /* Which properties a stylesheet actually stated for this box, as opposed
       to which have a value -- every property always has a value. Inheritance
       needs the difference: a box that says nothing about colour takes its
       parent's, and a box that says `color: black` does not, even though both
       end up with a colour. */
    ar_pset set;
} ar_style;

/* Pseudo-class of a rule, and the matching state of a node. */
enum
{
    AR_STATE_NONE = 0,
    AR_STATE_HOVER = 1 << 0,
    AR_STATE_ACTIVE = 1 << 1,
    AR_STATE_FOCUS = 1 << 2,

    /*
     * The structural pseudo-classes.
     *
     * They are state bits like the input ones, which keeps the whole selector
     * side one integer compare, but they split into two groups by *when* they
     * can be known. :root, :first-child and the odd/even pair follow from the
     * box's position among its siblings, which is settled the moment it is
     * declared. :last-child, :only-child and :empty depend on how many
     * children the parent turns out to have, which is not settled until the
     * parent closes -- so a sheet using them gets a second resolve pass.
     */
    AR_STATE_ROOT = 1 << 3,
    AR_STATE_FIRST = 1 << 4,
    AR_STATE_ODD = 1 << 5,
    AR_STATE_EVEN = 1 << 6,

    AR_STATE_LAST = 1 << 7,
    AR_STATE_ONLY = 1 << 8,
    AR_STATE_EMPTY = 1 << 9,

    /*
     * Not a selector state: nothing parses `:inert`, and this is written after
     * the styles are resolved rather than before. It lives in the same word
     * because the word had six spare bits and a per-box byte would have cost
     * every box in the interface one.
     */
    AR_STATE_INERT = 1 << 10,

    /*
     * This box was generated to hold something, not declared.
     *
     * Not a selector state either: it exists so a combinator can climb past
     * it. `tr > td` has to keep matching when the row between them is one
     * areole invented, or fixing up malformed markup would silently break the
     * stylesheet written against the markup it was fixing.
     */
    AR_STATE_ANON = 1 << 11,

    /*
     * This box belongs to a table whose borders are collapsed.
     *
     * Not a selector state either. It is what tells the paint pass to draw the
     * four widths in `edge` rather than the one in `border-width`: a collapsed
     * table, its rows and its row groups draw no border of their own at all --
     * theirs was folded into the grid lines the cells now carry -- and a cell
     * draws a different width on each of its four sides. There was a spare bit
     * and a per-box byte would have cost every box in the interface one.
     */
    AR_STATE_COLLAPSED = 1 << 12,

    /*
     * This flex item's main size is settled and the resolution loop must not
     * move it again.
     *
     * Not a selector state, and not durable: it lives for the length of one
     * call to the flex solver. It is a bit rather than an array because the
     * loop needs one flag per item and this engine has no array to put it in
     * -- the alternative was a field on every box in the interface for the
     * sake of the handful that are flex items at any moment.
     */
    AR_STATE_FLEX_FROZEN = 1 << 13,

    /* The ones that cannot be answered until the parent has closed. */
    AR_STATE_LATE = (1 << 7) | (1 << 8) | (1 << 9)
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

/* How many selectors may share one declaration block. Four covers
   `h1, h2, h3, h4`; a longer list is refused rather than truncated, because a
   truncated selector list is a rule that silently does not apply. */
#define AR_MAX_SEL_LIST 4

/*
 * The functional pseudo-classes: :not(), :is(), :where().
 *
 * Each holds a list of simple selectors -- a tag, a class, an id or a state,
 * one of each at most. `.card:not(.done, .hidden)` and `:is(.a, .b)` are the
 * shapes people actually write; `:not(.page > .card)` is not, and a complex
 * selector inside one of these is refused rather than half-matched.
 *
 * They apply to the subject compound only, which is the rightmost one -- the
 * element the rule is about. `.page:not(.wide) .card` is refused. Carrying the
 * lists on every context part as well would treble the size of ar_rule for a
 * case that has not come up, and the refusal is loud rather than silent.
 */
#define AR_MAX_ALTS 3

typedef struct ar_sel_simple
{
    ar_u32 tag;   /* hash, 0 means any */
    ar_u32 klass; /* hash, 0 means any -- one class, not a set */
    ar_u32 id;    /* hash, 0 means any */
    ar_u16 state; /* required state bits, 0 means any */
} ar_sel_simple;

typedef struct ar_sel_part
{
    ar_u32     tag;
    ar_classes klass;
    ar_u32     id;
    ar_u8      comb; /* how this part reaches the one to its right */
} ar_sel_part;

/*
 * One `@media` prelude, kept so a resize does not need the sheet parsed again.
 *
 * `at` and `len` point into the sheet's own copy of the text, because the
 * stylesheet a caller handed to ar_sheet_parse is not required to outlive the
 * sheet and nothing else here retains a pointer into it.
 *
 * `used` is which parts of ar_media the query actually consulted. That is what
 * makes a resize cheap: a query that named only `prefers-color-scheme` cannot
 * have changed its answer because the window got wider, and is not asked
 * again. `parent` chains a nested block to the one around it, so a rule inside
 * two queries is guarded by both without storing the pair.
 */
typedef struct ar_mq
{
    ar_u16 at;
    ar_u16 len;
    ar_u16 parent; /* 1-based; 0 means top level */
    ar_u32 used;
    ar_u8  result; /* this query alone            */
    ar_u8  active; /* and every query outside it  */
} ar_mq;

typedef struct ar_rule
{
    ar_u32     tag; /* hash, 0 means any */
    ar_classes klass;
    ar_u32     id; /* hash, 0 means any */

    /* Nearest first: ctx[0] is the part immediately left of the subject. Zero
       parts means a simple rule, which is the cacheable kind. */
    ar_sel_part ctx[AR_MAX_SEL_PARTS];
    ar_i32      nctx;

    /* :not() -- every one of these must fail for the rule to match. */
    ar_sel_simple neg[AR_MAX_ALTS];
    ar_i32        nneg;

    /* :is() and :where() -- at least one of these must match. They differ only
       in what they contribute to specificity, which is settled at parse time,
       so by here they are the same thing. */
    ar_sel_simple alt[AR_MAX_ALTS];
    ar_i32        nalt;
    ar_u16        state; /* required state bits, 0 means any */

    ar_u16 specificity;
    ar_u16 order; /* source position, to break specificity ties */

    /*
     * 0 for the user agent's rules, 1 for the page's.
     *
     * The first sort key, ahead of specificity, because an origin outranks it:
     * `td { padding: 0 }` written by a page beats `td { padding: 1px }`
     * written by html.css whatever the two selectors look like. Sorting on it
     * rather than checking it per box means the array is UA rules and then
     * author rules, which is what lets a presentational hint be an index.
     *
     * In practice it reorders nothing today -- every rule in the UA sheet is a
     * type selector and is parsed first, so it already lost every tie. What it
     * buys is the boundary.
     */
    ar_u8 origin;

    /* Which `@media` guards this rule: 1-based into the sheet's query table,
       0 for a rule no query guards. A guarded rule is stored whatever the
       query says right now, because a resize can turn it on. */
    ar_u16 query;

    /* The custom properties this rule declares: a run in the sheet's pool.
       Zero count is the common case and costs the matcher nothing. */
    ar_u16 var_first;
    ar_u8  var_count;

    ar_pset set; /* which properties this rule sets */

    /* Which of them were marked !important. Per declaration rather than per
       rule, because that is what CSS says and because a rule mixing the two is
       ordinary: `color: red !important; width: 10px;` means one of them wins
       against everything and the other does not. */
    ar_pset  important;
    ar_style style;

    /*
     * This rule was written `::backdrop`, so it styles the sheet painted
     * behind a modal rather than any box in the tree.
     *
     * A flag on the rule rather than a pseudo-element in the selector engine,
     * because there is exactly one pseudo-element and it matches no box: the
     * backdrop is not in the tree, has no rectangle of its own until a modal
     * gives it one, and would take a node slot and change every corpus's tree
     * paths if it were a real box.
     */
    ar_u8 backdrop;
} ar_rule;

/* ------------------------------------------------------------------------
 * The resolved style cache
 *
 * Style resolution was 50 to 89 per cent of every tree-driven frame, measured,
 * and grew linearly with rule count because every box was matched against every
 * rule. It no longer does: 13, 103 and 253 rules over the same 500 boxes now
 * cost the same to within noise. What did not change is the per-box cost --
 * about 126 ns whether the sheet has 13 rules or 253, and the same again across
 * the thousand identically-classed boxes of identical_siblings, where every box
 * after the first is a hit. A hit still copies the whole ar_style. Rule count
 * stopped mattering and box count did not; that is what style sharing is for.
 *
 * The key is the tuple ar_sheet_resolve already takes. Two boxes with the same
 * tag, class, id and state cannot resolve differently -- **and that is a
 * property of what this cache is allowed to hold, not a happy accident.**
 *
 * 0.4.0 added the cascade and the key did not grow, because the two things that
 * would have forced it are kept out of the cache instead:
 *
 *   - combinators resolve per box in ar_sheet_resolve_contextual, outside this
 *     cache, because their answer depends on where a box sits;
 *   - inheritance is applied after the lookup, in ar__resolve, because it
 *     depends on the parent. Caching after inheritance would need the parent in
 *     the key and would be a different and much worse cache.
 *
 * So the rule for anything added later is that one, not "grow the key": if a
 * new feature makes a resolved style depend on something outside this tuple,
 * either keep it out of the cache or put it in the key. Getting this wrong does
 * not produce a slow frame, it produces a silently wrong one, which is why it
 * is written here at this length.
 * ------------------------------------------------------------------------ */
typedef struct ar_cache_entry
{
    ar_u32   tag, klass, id;
    ar_u16   state;
    ar_u8    used;
    ar_style style;
} ar_cache_entry;

/*
 * One track of a grid template.
 *
 * Every track is a range, because that is what the sizing algorithm works in:
 * `100px` is minmax(100px, 100px), `auto` is minmax(min-content, max-content),
 * `1fr` is minmax(auto, 1fr). Storing them all as a pair means the algorithm
 * has one shape to handle rather than seven, and the parser is the only place
 * that knows there was ever a shorthand.
 */
typedef struct ar_track
{
    ar_i16 min_v;
    ar_i16 max_v;
    ar_u8  min_u;
    ar_u8  max_u;
} ar_track;

/* A pool entry that is a header rather than a track: `min_v` is how many
   tracks follow it. Index 0 is never a header, so zero means "no list". */
#define AR_TRACK_POOL 512

/* ------------------------------------------------------------------------
 * Custom properties
 *
 * `--brand: #c02` is not a property in the sense the table above means: the
 * names are the page's, not CSS's, so they cannot each have a slot. They live
 * in a pool on the stylesheet, keyed by the hash of the name, and a rule says
 * which run of the pool it declared.
 *
 * **A custom property here holds a value, not a token sequence.** CSS says it
 * holds whatever was written and validates it only where it is used, so
 * `--x: 3px solid red` is legal and becomes a border when substituted. That
 * needs the value parser to run again at frame time, over text, and this one
 * parses declarations once when the sheet is read. So a custom property whose
 * text is not a single value is stored with `ok` clear, and a `var()` naming
 * it is invalid -- which is the same answer CSS arrives at for every use
 * except the one that was going to work.
 *
 * The ceiling that leaves, stated because it is the one somebody will hit:
 * `--pad: 4px 8px` and `--font: bold 12px/1.4` do not work, and the shorthand
 * they would have fed does not either.
 * ------------------------------------------------------------------------ */
typedef struct ar_var_decl
{
    ar_u32 name; /* hash of the name, `--` and all */
    ar_i16 v;
    ar_u8  unit;
    ar_u8  ok; /* clear when the text was not a single value */
} ar_var_decl;

/* A `var()` in a value position: which name, and what to use when the name
   has no value. The fallback is a parsed value like any other, so
   `var(--gap, 1rem)` keeps its unit and resolves with everything else. */
typedef struct ar_var_ref
{
    ar_u32 name;
    ar_i16 fallback_v;
    ar_u8  fallback_unit;
    ar_u8  has_fallback;
} ar_var_ref;

/* Both pools are small: the busiest of the ten documents in examples/15_real
   declares twenty-seven names, and the three that use the most `var()` between
   them reference about forty. */
#define AR_VAR_POOL    256
#define AR_VARREF_POOL 256

/* How many custom properties one rule may declare. The generated ones a build
   tool emits come one or two to a rule; a hand-written `:root` block is the
   case that goes wide, and this is past every one measured. */
#define AR_RULE_VARS 16

/* How many boxes may declare custom properties, and how many declarations
   they may make between them. Sized for the busiest page measured and not for
   an imagined one: twenty-seven names over about forty rules. */
#define AR_VAR_SCOPES  256
#define AR_VAR_ENTRIES 512

/* ------------------------------------------------------------------------
 * calc(), as a program
 *
 * A maths expression is compiled once, into postfix, into a pool on the
 * stylesheet -- the same shape a track list takes and for the same reason: a
 * style slot is sixteen bits and an expression is a tree.
 *
 * Postfix rather than a tree because evaluating one needs a stack and nothing
 * else: no pointers, no recursion at frame time, and the whole program is a
 * contiguous run a cache line at a time. The parser does the recursion, once,
 * where depth is bounded by the text.
 *
 * A PUSH carries its unit, so the expression stays unresolved until something
 * knows what a `rem` is. That is the point: `calc(2.75rem + 2px)` cannot be a
 * number at parse time and must not be flattened into one.
 * ------------------------------------------------------------------------ */
enum
{
    AR_CALC_LEN = 0, /* header: `v` is how many ops follow */
    AR_CALC_PUSH,
    AR_CALC_ADD,
    AR_CALC_SUB,
    AR_CALC_MUL,
    AR_CALC_DIV,
    AR_CALC_MIN,
    AR_CALC_MAX,
    AR_CALC_CLAMP,
    AR_CALC_ABS,
    AR_CALC_SIGN,
    /* round(), and the strategy in the op's own `v`: 0 nearest, 1 up,
       2 down, 3 to-zero. A field on the op rather than a fourth operand,
       because the strategy is a keyword and the stack holds numbers. */
    AR_CALC_ROUND,
    AR_CALC_MOD,
    AR_CALC_REM,
    /* `var(--name)` as an operand: `v` is the reference pool index. Resolved
       when the expression is, because the name means different things to
       different boxes. */
    AR_CALC_PUSH_VAR
};

typedef struct ar_calc_op
{
    ar_u8  op;
    ar_u8  unit; /* PUSH only: what `v` is measured in */
    ar_i16 v;    /* PUSH only: the number, in that unit's own scale */
} ar_calc_op;

/* Programs are short -- the longest in the ten real pages saved under
   examples/15_real is nine operations -- and a sheet has few of them. */
#define AR_CALC_POOL 512

/* How deep the evaluator's stack goes. `clamp()` needs three operands live at
   once and nesting multiplies that; sixteen is past anything a stylesheet
   contains and is checked rather than assumed. */
#define AR_CALC_STACK 16

/* Every `@media` prelude in every stylesheet, and the bytes to keep them in.
   Both are small because a prelude is short and a sheet has few: the whole of
   the user-agent stylesheet has none at all. */
#define AR_QUERY_POOL 64
#define AR_QUERY_TEXT 2048

/*
 * `grid-template-rows: subgrid`, as a sentinel in the slot that holds a pool
 * index.
 *
 * Subgrid is not a track list -- it is the absence of one, and a statement
 * that the parent's tracks are this box's tracks. Zero already means "nothing
 * was said" and a pool index is never negative, so -1 is free and says
 * something no track list could.
 */
#define AR_TRACKS_SUBGRID (-1)

typedef struct ar_sheet
{
    ar_rule *rules;
    ar_u16   count;
    ar_u16   capacity;

    /*
     * How many of those rules came from the user-agent stylesheet, which is
     * the boundary a presentational hint sits on.
     *
     * `<td bgcolor=red>` loses to `td { background: blue }` written by the
     * author and beats `td { background: blue }` written by the user agent --
     * that is the whole of what the "presentational hints origin" means, and
     * it is expressible here as an index because rules are stored in source
     * order and the UA sheet is loaded first.
     *
     * Zero when nobody called ar_sheet_mark_ua, which puts hints below
     * everything and is the right answer for a sheet with no UA half.
     */
    ar_u16 ua_count;

    /* Whether rules parsed right now belong to the user agent. Set for the
       length of ar_ua_stylesheet and at no other time. */
    ar_u8 in_ua;

    /*
     * Whether a length in the sheets parsed from here on must carry its unit.
     *
     * `width: 100` is a hundred pixels in quirks mode and is *invalid* in
     * standards, where the declaration is dropped and the width stays `auto`.
     * A page with a doctype and a unitless width lays out one way in every
     * browser, and this engine laid it out the other way.
     *
     * Off by default, and that is deliberate rather than lazy. An interface's
     * stylesheet is not a document: it has no doctype to read a mode from,
     * nobody validates it, and `gap: 8` there is what somebody meant. Only
     * ar_doc_stylesheets turns this on, and only for a document whose doctype
     * asked for standards mode -- so the strictness lands exactly where the
     * distinction is observable and nowhere else.
     */
    ar_u8 strict_lengths;

    /* Every track list every rule in this sheet declared, laid end to end.
       See the comment beside AR_P_GRID_COLS. */
    ar_track *tracks;
    ar_u16    track_count;
    ar_u16    track_cap;

    /* Every `calc()` in every rule in this sheet, compiled end to end. See
       ar_calc_op. */
    ar_calc_op *calcs;
    ar_u16      calc_count;
    ar_u16      calc_cap;

    /* Every `--name: value` in every rule, and every `var()` that names one. */
    ar_var_decl *vars;
    ar_u16       var_count;
    ar_u16       var_cap;
    ar_var_ref  *varrefs;
    ar_u16       varref_count;
    ar_u16       varref_cap;

    /* Whether any rule declares a custom property. A sheet without one never
       runs the pass that matches them, which is most sheets: seven of the ten
       documents in examples/15_real declare none at all. */
    int has_vars;

    /* Whether any declaration in this sheet is a maths function, on the same
       terms as has_grid and has_rel_units: a sheet with no `calc()` in it
       never runs the pass that evaluates one. */
    int has_calc;
    /* Whether any rule says `display: grid`, on the same terms as has_table:
       a sheet without one never runs the grid pass. */
    int has_grid;

    /*
     * Whether any declaration in this sheet is a viewport length, on the same
     * terms: a sheet with no `vh` in it never runs the pass that resolves one.
     *
     * That pass cannot live with the others in ar__resolve, because style is
     * resolved while the tree is being declared and the surface for this frame
     * is not known until ar_frame_end. Resolving there instead is what makes
     * `100vh` right on the first frame and on the frame a window is resized,
     * rather than one frame behind the way a media query is.
     */
    int has_view_units;

    /*
     * Whether any declaration in this sheet is a relative length at all.
     *
     * The gate on the whole of ar__resolve_units, and it is here because the
     * pass underneath it is O(property count) per box -- which is precisely
     * the shape 0.8.2 spent a release removing from ar_style_inherit, and
     * which style resolution must never grow back. A sheet stating `16px`
     * everywhere pays one branch per box for units it does not use.
     *
     * Set by the parser rather than derived, because the answer is known when
     * the sheet is read and never changes afterwards.
     */
    int has_rel_units;

    /* Dropped wholesale whenever a stylesheet is added, which is the only
       thing that can invalidate it. Adding a stylesheet is a startup
       operation, so this never happens in a frame. */
    /* Whether any rule needs the second resolve pass, for the same reason and
       with the same payoff as the flag below: a sheet without one skips it. */
    int has_late_state;

    /* Whether any rule in this sheet says `display` is a table value. A sheet
       without one skips the anonymous-box check on every ar_begin entirely,
       which is the overwhelming majority of interfaces -- the same bargain
       has_combinator makes below. */
    int has_table;
    /* And whether any of them collapses its borders. The marking pass walks up
       from every table box to find its table, which is a real cost on a table
       of ten thousand rows and is pure waste for the separate model -- which is
       the default, and what every table that does not say otherwise uses. */
    int has_collapse;

    /* Whether any rule in this sheet carries a combinator. A sheet without
       one skips the contextual pass entirely, which is most sheets. */
    int has_contextual;

    ar_cache_entry *cache;
    ar_u16          cache_cap;
    ar_u32          cache_hits, cache_misses;

    /* Parsing never aborts. One malformed declaration should not silently
       discard the ninety that follow it, so errors are counted and reported
       rather than thrown. */
    /*
     * What @media is answered against, and how many times a query has been
     * evaluated. The counter is the evidence for "a resize re-evaluates only
     * the queries that could have changed"; without it that claim is a hope.
     */
    ar_media media;
    ar_u32   queries_evaluated;

    ar_mq *queries;
    ar_u16 query_count;
    ar_u16 query_cap;
    char  *qtext;
    ar_u16 qtext_used;
    ar_u16 qtext_cap;

    ar_u32 errors;

    /*
     * Of those, the ones that cost a whole rule.
     *
     * The two are not the same failure and conflating them hides the one
     * that matters. A declaration naming a property areole does not
     * implement is *dropped*, and the rule around it still applies -- which
     * is what CSS itself says to do, and means real-world stylesheets full
     * of `font-family` and `box-shadow` still style what they can. A rule
     * whose selector list is longer than AR_MAX_SEL_LIST is refused whole,
     * and nothing it said happens at all.
     *
     * Only the second is worth failing a build over, and until this counter
     * existed there was no way to ask.
     */
    ar_u32 rules_refused;

    ar_u32 first_error_offset;
} ar_sheet;

ar_u32 ar_hash(const char *s, ar_u32 len);

/*
 * The value of any property, whichever array it lives in.
 *
 * Every pass that walks properties by index -- defaults, merge, inheritance --
 * goes through these, so exactly one place knows that colours are stored
 * apart. Code that names a property outright still uses v[] or AR_RGB
 * directly, because there the compiler checks the choice.
 */
ar_i32 ar_style_get(const ar_style *s, ar_i32 prop);
void   ar_style_put(ar_style *s, ar_i32 prop, ar_i32 v);

/* A stated length wider than v[] can hold, clamped rather than wrapped. */
ar_i32 ar_style_clamp_narrow(ar_i32 v);

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

/* The track list a template property points at, and how many tracks it holds.
   Returns NULL when the property said nothing. */
const ar_track *ar_sheet_tracks(const ar_sheet *sheet, ar_i32 index, ar_i32 *out_count);
void            ar_sheet_set_tracks(ar_sheet *sheet, ar_track *storage, ar_u16 capacity);

void ar_sheet_set_calcs(ar_sheet *sheet, ar_calc_op *storage, ar_u16 capacity);

void ar_sheet_set_vars(ar_sheet *sheet, ar_var_decl *decls, ar_u16 decl_cap, ar_var_ref *refs,
                       ar_u16 ref_cap);

/*
 * The custom properties this box declares, in cascade order.
 *
 * A separate pass from ar_sheet_resolve and deliberately so: it walks only
 * the rules that declare one, which is a handful in the sheets that have any
 * and none at all in the sheets that do not. Threading them through the main
 * resolve would have put the cost on every box in every document.
 *
 * Returns how many were written, up to `cap`.
 */
ar_i32 ar_sheet_resolve_vars(const ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                             ar_u16 state, ar_var_decl *out, ar_i32 cap);

/* The reference at `index`, or 0 if there is none. */
const ar_var_ref *ar_sheet_varref(const ar_sheet *sheet, ar_i32 index);

/*
 * What a compiled expression needs to know before it can be a number.
 *
 * Everything here is per box except the viewport, and all of it is settled by
 * the time ar_frame_end runs the evaluation -- which is why that is where it
 * runs rather than during style resolution, where the surface is a frame old.
 */
/* How the evaluator asks what a custom property means here. Returns 0 when
   the name has no usable value, which makes the whole expression invalid --
   CSS says a `var()` that cannot be substituted poisons the declaration and
   not merely the term it stood in. */
typedef int (*ar_calc_var_fn)(const void *ctx, const void *node, ar_u32 name, ar_i16 *out_v,
                              ar_u8 *out_unit);

typedef struct ar_calc_env
{
    ar_i32 font_px;  /* the element's own, for em/ex/ch/cap/ic */
    ar_i32 root_px;  /* the root's, for the r-prefixed six */
    ar_i32 line_px;  /* the element's line box, for lh */
    ar_i32 rline_px; /* the root's, for rlh */
    ar_i32 view_w;
    ar_i32 view_h;

    /* Opaque to the evaluator on purpose. Handing it the context would make
       the CSS parser depend on the box tree, which is the one direction this
       codebase does not let dependencies run. */
    ar_calc_var_fn var_lookup;
    const void    *ud;
    const void    *ctx;
} ar_calc_env;

/*
 * Evaluates the program at `index` to whole pixels.
 *
 * Returns 0 and sets *ok to 0 when the expression cannot be a length -- a
 * bare number where a length was wanted, a division by zero, a stack that ran
 * deeper than AR_CALC_STACK. A caller that gets that keeps whatever the
 * cascade already gave the property, which is what CSS asks for.
 */
ar_i32 ar_calc_eval(const ar_sheet *sheet, ar_i32 index, const ar_calc_env *env, int *ok);
void   ar_style_merge(ar_style *dst, const ar_style *src, ar_pset set);

void ar_sheet_init(ar_sheet *sheet, ar_rule *storage, ar_u16 capacity);

/*
 * What media queries are answered against. Takes effect on the next parse.
 *
 * Defaults to a zero-sized window at one device pixel per CSS pixel, so a
 * caller that never sets it gets `min-width` queries that are all false --
 * which is wrong quietly. Every path in this tree that parses a stylesheet
 * sets it first; a caller outside the tree has to.
 */
void ar_sheet_set_media(ar_sheet *sheet, const ar_media *media);

/*
 * Where `@media` preludes are kept. Without it a guarded rule is refused, so
 * a sheet with no query storage behaves as though every query were false --
 * which is stated here because the alternative is discovering it in a page.
 */
void ar_sheet_set_queries(ar_sheet *sheet, ar_mq *storage, ar_u16 capacity, char *text,
                          ar_u16 text_capacity);
void ar_sheet_set_cache(ar_sheet *sheet, ar_cache_entry *storage, ar_u16 capacity);
void ar_sheet_cache_clear(ar_sheet *sheet);
void ar_sheet_parse(ar_sheet *sheet, const char *css);

/* Resolves the style for one box. Rules already sit in ascending specificity
   order, so applying them in order leaves the winner on top. */
/* Not const: resolving populates the cache. */
/*
 * The style of the backdrop behind one modal.
 *
 * Deliberately not cached. The cache key is tag, class, id and state, and
 * "is this the backdrop" is none of those -- so it would have to join the key
 * or stay out, and this file says which of those to prefer. There is at most
 * one backdrop per modal and modals are counted on one hand, so staying out
 * costs a linear pass nobody will measure.
 */
/* Sets sheet->has_table if any rule declares a table display. Called once
   per ar_stylesheet, never per box. */
void ar_sheet_note_tables(ar_sheet *sheet);

void ar_sheet_resolve_backdrop(const ar_sheet *sheet, ar_u32 tag, const ar_classes *klass,
                               ar_u32 id, ar_u16 state, ar_style *out);

void ar_sheet_resolve(ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id, ar_u16 state,
                      ar_style *out);

/* Splits a selector such as div.card#first into its three hashes. Any part may
   be absent. Returns 0 if the selector is malformed. */
int ar_selector_split(const char *sel, ar_u32 *tag, ar_classes *klass, ar_u32 *id);

/* Adds a class hash if there is room and it is not already there. */
void ar_classes_add(ar_classes *c, ar_u32 hash);
void ar_classes_clear(ar_classes *c);

/* Every class in `want` is present in `have`. That direction is the whole of
   compound matching: a rule naming two classes matches a box carrying three. */
int ar_classes_contains(const ar_classes *have, const ar_classes *want);

/* Whether one class is in the set. What a simple selector asks, where
   ar_classes_contains answers the compound's question. */
int ar_classes_has(const ar_classes *have, ar_u32 klass);

/* Does one part of a selector describe this element? */
int ar_sel_part_matches(const ar_sel_part *p, ar_u32 tag, const ar_classes *klass, ar_u32 id);

/* Does one simple selector describe this element? Used for the contents of
   :not(), :is() and :where(), which are lists of these and nothing else. */
int ar_sel_simple_matches(const ar_sel_simple *p, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                          ar_u16 state);

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

/*
 * The `!important` band on its own, for a box that has an inline style.
 *
 * Inline declarations outrank every selector and are outranked by every
 * `!important`, so they cannot simply be merged on top of a resolved style:
 * the band has to run again above them. See ar__important_band.
 */
/*
 * Brackets the user-agent stylesheet: every rule parsed between these two
 * calls is the user agent's, and every other rule is the page's. Called by
 * ar_ua_stylesheet and by nobody else.
 */
void ar_sheet_begin_ua(ar_sheet *sheet);
void ar_sheet_mark_ua(ar_sheet *sheet);

/*
 * Resolve with a presentational-hint rule merged at the UA/author boundary.
 *
 * Never cached: the cache key is tag, classes, id and state, and a hint is
 * none of those. A box without hints goes through ar_sheet_resolve and is
 * cached exactly as before.
 */
void ar_sheet_resolve_hinted(const ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                             ar_u16 state, const ar_rule *hints, ar_style *out);

void ar_sheet_apply_important(const ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                              ar_u16 state, ar_style *out);

/*
 * Parse a declaration list -- `color:red; width:4px` -- into a selectorless
 * rule. Returns non-zero if anything was set. The caller merges
 * `set - important` and then `important` in cascade order.
 */
int ar_decls_parse(ar_sheet *sheet, const char *decls, ar_rule *rule);

/* Whether lengths in the sheets parsed from here on must carry their unit.
   See ar_sheet.strict_lengths. */
void ar_sheet_set_strict_lengths(ar_sheet *sheet, int on);

void ar_sheet_resolve_contextual(const ar_sheet *sheet, ar_i32 index, ar_u32 tag,
                                 const ar_classes *klass, ar_u32 id, ar_u16 state, ar_sel_walk find,
                                 void *ud, ar_style *out);

#endif /* AR_CSS_H */
