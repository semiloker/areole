# The areole CSS subset

Everything areole understands, and nothing it does not. The subset is small on
purpose: every property here is one integer in a flat array, and every selector
here matches in three integer compares.

A stylesheet is parsed **once**, at startup, into rules in the arena. Nothing
is parsed per frame.

```c
ar_stylesheet(ui, ".card { width:200px; padding:12px; background:#f8f3e9; }");
```

`ar_stylesheet` **appends**, so call it as many times as you like. You will
need to: C90 only guarantees 509 characters in a string literal, and adjacent
literals count as one.

Parsing never aborts. One malformed declaration costs one declaration, not the
rest of the file. Ask `ar_stylesheet_errors()` whether anything went wrong.

## Selectors

| Form | Matches | Specificity |
| --- | --- | --- |
| `div` | any box declared with that tag | 1 |
| `.card` | any box declared with that class | 10 |
| `#sidebar` | the box declared with that id | 100 |
| `.btn:hover` | while the cursor is over it | +10 |
| `.btn:active` | while a press that started on it is held | +10 |
| `.btn:focus` | reserved; nothing sets focus yet | +10 |

Parts combine: `div.card#first` is a legal selector and so is the matching
`ar_begin(ui, "div.card#first")`.

**Up to four classes per selector.** `.nav.selected` matches a box declared
with both. Four is the limit on a rule and on a box alike.

**Combinators.** All four, as of 0.4.0:

| Form | Matches |
| --- | --- |
| `.panel .btn` | a descendant, anywhere below |
| `.panel > .btn` | an immediate child |
| `.a + .b` | the sibling immediately before |
| `.a ~ .b` | any sibling before |

A rule carrying a combinator cannot be answered from the style cache, whose key
is the box's own tag, classes, id and state. Those rules are held apart and
resolved per box; a stylesheet with no combinators pays nothing for it.

Ties in specificity are broken by source order: the later rule wins. Rules are
sorted once at parse time, so resolution is a single forward pass.

## The cascade

**`!important`** wins over any declaration that is not, whatever the
specificity.

**Inheritance.** `color` and `font-size` inherit from the parent box. Any
property can be asked to with `inherit`, and `initial`, `unset` and `revert`
are accepted too. `unset` is inherit or initial depending on the property;
`revert` means "back to the UA sheet", and since there is no separate UA layer
it means the same as `initial`.

There is one author origin, so no origin ordering exists to get wrong.

**One known ordering divergence:** rules carrying a combinator are resolved in
a pass after the cached one, so a combinator rule beats a simple rule of higher
specificity. `#id .btn` and `.btn#id` are not the trap; `.panel .btn` beating
`#the-button` is.

## Properties

### Layout

| Property | Values |
| --- | --- |
| `display` | `flex`, `block`, `none` |
| `flex-direction` | `row`, `column` |
| `justify-content` | `flex-start`, `center`, `flex-end`, `space-between` |
| `align-items` | `flex-start`, `center`, `flex-end`, `stretch` |
| `gap` | length |
| `padding` | 1, 2, 3 or 4 lengths, clockwise from the top |
| `padding-top` `-right` `-bottom` `-left` | length |
| `margin` | as `padding` |
| `margin-top` `-right` `-bottom` `-left` | length |

`align-items` defaults to `stretch`, as in CSS. It only affects boxes that
state no size of their own.

### Flex

| Property | Values |
| --- | --- |
| `flex-direction` | `row`, `column` |
| `flex-wrap` | `nowrap`, `wrap`, `wrap-reverse` |
| `flex-basis` | length, per cent, `auto`, `content` |
| `flex-grow` `flex-shrink` | number, fractions kept |
| `flex` | `<grow> [<shrink>] [<basis>]` |
| `justify-content` | `flex-start`, `center`, `flex-end`, `space-between`, `space-around`, `space-evenly` |
| `align-items` | `flex-start`, `center`, `flex-end`, `stretch`, `baseline` |
| `align-self` | `auto` and the `align-items` values |
| `align-content` | the `align-items` values plus the three `space-*` |
| `order` | integer, may be negative |
| `place-items` `place-content` `place-self` | `<align> [<justify>]` |

**`flex: 1` and `flex-grow: 1` are different declarations.** The shorthand
writes a *zero* basis, so three boxes come out equal whatever is in them; the
longhand leaves the basis `auto`, so each keeps its content's width and only
the surplus is shared. This is the most common flexbox mistake there is and
CSS put the difference in the shorthand on purpose.

**`flex-grow: 0.5` keeps its fraction.** Every other fractional number here is
floored — sub-pixel sizes are not a thing in an integer engine — and a flex
factor is a ratio rather than a size, so the two factor properties are the only
place the parser keeps three decimal places.

**`min-width: auto` on a flex item is not zero.** It is the item's min-content
size, and it applies whether or not anybody wrote it — the rule behind almost
every "why will my flex item not shrink" question. Two ways out, both
specified: say `min-width: 0` explicitly, or make the item clip with
`overflow: hidden`, which removes the automatic minimum outright.

The `grow` keyword areole invented stays, as an alias for `flex: 1` along the
main axis, because stylesheets use it. It is areole-specific and not CSS.

### Grid

| Property | Values |
| --- | --- |
| `grid-template-columns` `grid-template-rows` | a track list |
| `grid-auto-columns` `grid-auto-rows` | one track |
| `grid-auto-flow` | `row`, `column`, and `dense` beside either |
| `grid-column` `grid-row` | `<start> [/ <end>]` |
| `grid-column-start` `grid-column-end` `grid-row-start` `grid-row-end` | integer line, or `span <n>` |
| `justify-items` `justify-self` | `start`, `center`, `end`, `stretch`; `auto` on self |
| `row-gap` `column-gap` `gap` | length; the shorthand is row then column |

A **track** is a length, a percentage, `auto`, `min-content`, `max-content`,
`<n>fr`, `minmax(<min>, <max>)` or `fit-content(<length>)`, and a list may wrap
any run of them in `repeat(<n>, ...)`.

**`1fr` is not "a share of the container".** It is "at least the contents, then
a share of what is left" — `1fr` has a minimum of `auto`, so a track holding a
long word is wider than its share and every other `fr` track gets less. The
distribution iterates for that reason, taking out each track that will not
shrink to its share and dividing the rest again.

Columns are solved before rows, because an item's height depends on the width
it ends up with.

Two bounds, and they are real: a grid has at most **64 tracks** on an axis and
**1024 items**. The arrays are on the stack, which is the same bargain the
table's columns and the float list make; beyond the bounds the extra tracks and
items are dropped rather than placed wrongly.

### Sizing

| Property | Values |
| --- | --- |
| `width` `height` `min-*` `max-*` | length, per cent, `auto`, `min-content`, `max-content`, `fit-content`, `fit-content(<length>)` |
| `aspect-ratio` | `<w> / <h>`, or a bare number |

**The intrinsic keywords work on both axes.** They used to answer for `width`
only, which made `height: max-content` a silent `auto` — the same number in
block flow, where an automatic height is already the content height, and a
different one anywhere that stretches.

`fit-content(<length>)` is the bare keyword with a ceiling: the contents fitted
into the smaller of what the container has left and the length. It never goes
below min-content, so a cap under that does nothing.

**`aspect-ratio` gives the axis nobody stated.** A width and a ratio make a
height; a height and a ratio make a width; a box that stated both keeps both,
because a stated size always wins over a derived one — which is what makes the
property safe to put in a base stylesheet. It is how a responsive placeholder
holds its space without the padding-percentage trick.

The ratio is carried in thousandths, so `16 / 9` is 1777, and the arithmetic
rounds rather than truncating: `4 / 3` is 1333.33 short, and truncating puts a
60-pixel box one pixel out where every browser says 80.

**`display: contents`** makes a box generate none: its children become its
parent's for layout, which is the only way to put a semantic wrapper around
grid items without breaking the grid. It is done by splicing the box out of its
parent's child list before layout, which keeps both the pre-order invariant and
the node keys — its children already sit at higher indices than it does.

One gap, and it is not small: **CSS excepts replaced elements, form controls
and table parts from `display: contents`, and areole cannot.** Every one of
those exceptions is about an *element*, and a box that says `display: contents`
has no other display left to look at — nothing records that it would otherwise
have been a row. The exception needs tag names, and there are none until the
parser lands at 0.9.0. So `display: contents` on a table row removes the row
here where a browser ignores it.

**`safe` and `unsafe` before an alignment.** Centring a box larger than the
space it is given puts half the overflow before the start edge, where it cannot
be scrolled to and cannot be read; `safe center` falls back to start alignment
exactly then, and only then. Written as two words — `align-items: safe center`
— and carried as a bit above the alignment value.

### Size

| Property | Values |
| --- | --- |
| `width` `height` | `<n>px`, `<n>%`, `auto`, `grow` |
| `min-width` `min-height` | length |
| `max-width` `max-height` | length |

- `auto` — size to content. This is the default, which is why a stylesheet can
  stay silent about most dimensions.
- `%` — of the parent **inner** box, after its padding.
- `grow` — take a share of what is left on the main axis. Not CSS: it says what
  `flex-grow: 1` does without dragging in basis, shrink and the rest of the
  algebra. Leftover pixels that do not divide evenly go one at a time to the
  boxes at the front, so a row of growers meets the far edge exactly.

### Flow and position

| Property | Values |
| --- | --- |
| `display` | `flex`, `grid`, `block`, `inline-block`, `inline`, `contents`, `none`, and the nine table values below |
| `position` | `static`, `relative`, `absolute`, `fixed`, `sticky` |
| `top` `right` `bottom` `left` | length |
| `z-index` | integer |
| `float` | `left`, `right` — the initial value is `none` |
| `clear` | `left`, `right`, `both` |
| `text-align` | `left`, `right`, `center` |
| `vertical-align` | `baseline`, `top`, `middle`, `bottom` |
| `box-sizing` | `content-box`, `border-box` |

`box-sizing` defaults to `content-box`, as CSS says. areole used to treat a
stated size as the border box, which put it 18 px from a browser on a padded
box.

### Tables

| Property | Values |
| --- | --- |
| `display` | `table`, `table-row-group`, `table-header-group`, `table-footer-group`, `table-row`, `table-cell`, `table-column-group`, `table-column`, `table-caption` |
| `table-layout` | `auto`, `fixed` |
| `border-collapse` | `separate`, `collapse` |
| `border-spacing` | length — separate model only |
| `colspan` `rowspan` | integer, at least 1 |
| `visibility` | `visible`, `hidden`, `collapse` — inherited |
| `caption-side` | `top`, `bottom` |
| `empty-cells` | `show`, `hide` |

`colspan` and `rowspan` are **properties here, not attributes**. They are
attributes in HTML and there is no parser until 0.9.0, so a stylesheet is the
only place a span can be written — which is why they cost per-box style bytes
that `include/areole.h` accounts for beside the memory budget.

**The missing boxes are generated.** A cell written straight inside a table
gets a row to live in, a row with no table gets one, and ordinary content
beside a cell gets a cell of its own — the algorithm needs a rectangular grid
and real markup rarely provides one. Those boxes are real nodes: they consume
the node budget, so `AR_MEM(n)` stops meaning "n calls to `ar_begin`", and
`ar_node_generated` tells you which is which. A combinator sees through them,
so `.row > .cell` keeps matching across a row areole invented.

The display of a box is resolved before the box exists, so a rule that switches
`display` to or from a table value on `:hover` **will not regenerate the
anonymous boxes**. Write the table values unconditionally and change something
else on hover.

**`tfoot` is drawn last** wherever it is written, and `thead` first. Document
order is not row order.

**A table with no stated width is as wide as its contents**, unlike every other
block box, which fills what it is offered.

**Surplus width goes to the columns that did not state one.** When every column
has stated a width, it is shared out in proportion to what each asked for.

**Collapsed borders resolve by width and then by origin** — cell, then row,
then row group, then table. CSS resolves `border-style` first; this engine has
one uniform `border-width` per box and no `border-style`, so there is nothing
for that step to resolve. In the collapsed model a table, its rows and its row
groups draw no border of their own: theirs is folded into the grid lines the
cells now carry, and a line is split between the two boxes that meet on it,
the left or upper one taking the larger half.

One difference from a browser worth knowing before you measure against one: a
browser's collapsed table has a **used border of half the outer line**, so its
border box is wider than its stated width and its content starts inside it.
areole reserves no space for any border anywhere, so the table's box stays at
its stated width and the grid starts at its corner. Every cell size agrees;
every offset is short by that half.

**`visibility: collapse` removes a row or a column without recomputing the
column widths.** That is the entire difference from `display: none` and the
reason the value exists: a filter that hides half a table's rows should not
make every remaining column jump to a new width. The cells of a closed row are
still in the column constraints; the row simply takes no height and the rows
below it close up. A closed column takes no width and nothing else moves to
take it, so the table gets narrower by exactly that column.

Anywhere other than a row, a column or their groups, `collapse` means `hidden`.
`visibility` inherits, which is what makes it worth writing on a row at all —
the row goes and its cells go with it, and a cell that says `visibility:
visible` comes back. That last part is the difference from `display: none`,
where nothing comes back.

**A `col` is the one place a column can be spoken about without naming a
cell.** A stated `width` on a `col` or a `colgroup` settles its column whatever
the cells in it want, and a `background` paints behind the whole column. The
column boxes are counted across the table's children in order: a `col` takes
the next column, a `colgroup` covers the `col`s inside it, or the next column
if it has none. There is no `span` attribute — there are no attributes.

**`empty-cells: hide`** stops a cell with nothing in it from showing its
background or its border, so a sparse table reads as a grid with holes rather
than a grid of empty boxes. Separate model only: a collapsed grid line belongs
to the boundary and not to either cell, so there is no such thing as one cell
withholding it.

**`position: sticky` works on table boxes** — a header group that stays while
the rows scroll under it, a first column that stays while the columns scroll
past. It is the same mechanism as everywhere else: the box keeps its place in
the flow and is nudged just far enough to obey its offsets without leaving the
box it belongs to, which for a row is its row group and for a cell is its row.
A sticky box is positioned, so it paints above the unpositioned boxes it
overlaps without needing a `z-index`.

### Overflow and scrolling

| Property | Values |
| --- | --- |
| `overflow` | one or two of `visible`, `hidden`, `scroll`, `auto` — x then y |
| `overflow-x` `overflow-y` | as above, one value |
| `overscroll-behavior` `-x` `-y` | `auto`, `contain`, `none` |
| `overflow-anchor` | `auto`, `none` |
| `scroll-snap-type` | `none`, `x`, `y`, `both`, each optionally with `mandatory` or `proximity` |
| `scroll-snap-align` | `none`, `start`, `center`, `end` |
| `scroll-snap-stop` | `normal`, `always` |
| `scroll-padding` `-top` `-right` `-bottom` `-left` | length |
| `scroll-margin` `-top` `-right` `-bottom` `-left` | length |
| `scrollbar-width` | `auto`, `thin`, `none` |
| `scrollbar-gutter` | `auto`, `stable`, `both-edges` |
| `scrollbar-color` | `<thumb> <track>` — a lone colour is the thumb |

A box whose overflow is `scroll` or `auto` is a scroll container: it clips, it
keeps a scroll position between frames, and it draws a scrollbar. `scroll`
always shows one; `auto` shows it only when there is somewhere to go.

**`visible` on one axis with anything else on the other becomes `auto`**, which
CSS requires and a naive implementation drops. So `overflow-x: visible;
overflow-y: hidden` is a horizontal scroller, which is exactly what someone
writing it means.

**The scrollbar is an overlay.** It is drawn inside the container's right edge
rather than taken out of its width, so one appearing never reflows the text
beside it. `scrollbar-gutter: stable` exists to prevent a layout shift that
therefore cannot happen here; what it buys is that a row stops before the bar
instead of running underneath it.

Scrolling is driven by the wheel, by dragging the thumb, by the keys that
scroll — arrows, Page Up and Down, Home, End, space — and by
`ar_node_scroll_to` and `ar_node_scroll_into_view`.

Where it settles is the same wherever it came from: a container with
`scroll-snap-type` snaps after a wheel notch, after a key, and after
`ar_node_scroll_to`, which is what CSS requires of any scrolling operation.
Home and End deliberately do not snap, so the two ends of a list stay
reachable.

Four limits worth knowing, rather than discovering:

- Snapping resolves on the block axis. The inline axis parses and is stored.
- `ar_node_scroll_into_view` does not snap. Its contract is the minimum
  distance that brings a box into view; the wheel, the keys and
  `ar_node_scroll_to` all snap.
- `scroll-padding` and `scroll-margin` have their four physical longhands, not
  their logical forms. There are no logical properties anywhere yet.
- `overflow-anchor` holds one container at a time.

### The top layer and anchoring

| Property | Values |
| --- | --- |
| `overlay` | `none`, `auto`, `modal` |
| `inert` | `none`, `auto` |
| `anchor-name` | a custom ident, e.g. `--tip` |
| `position-anchor` | the ident of an `anchor-name` |
| `position-try` | `none`, `flip-block`, `flip-inline`, `flip-both` |

Inside `top`, `right`, `bottom` and `left`: **`anchor(top | right | bottom |
left | center)`**, which resolves to that edge of the anchored box's anchor.
Inside `width` and `height`: **`anchor-size(width | height)`**.

And one pseudo-element, the only one areole has: **`::backdrop`**, the sheet
painted under a modal and over everything else. It matches no box, so it is
written against the element it belongs to — `.dialog::backdrop { background:
#0008; }` — and says nothing about that element.

**A box with `overlay: auto` or `modal` is in the top layer**, which paints
above every stacking context and clips to the viewport rather than to its
ancestors. That is not the same as a large `z-index` and cannot be reproduced
by one: a z-index orders a box among its siblings inside one stacking context
and cannot lift it out of that context, so a dialog declared inside anything
positioned can always be covered by that thing's siblings, whatever number it
asks for.

`modal` additionally makes everything outside it `inert`. `auto` does not —
that is the whole difference between a dialog and a popover.

Three deviations, named rather than discovered:

- **CSS makes `overlay` UA-controlled**: only `<dialog>` and `popover` may
  enter the top layer, and a stylesheet cannot put a box there. areole has no
  UA stylesheet and no elements, so a stylesheet sets it. When the parser lands
  it will set this on `dialog[open]` and nothing else changes.
- **`inert` is an HTML attribute**, and there are no attributes here. Same
  arrangement, same migration.
- **`modal` is not a CSS value at all.** It is what `showModal()` does, and
  there is no method to call.

`position-try` ships as the flip only. The full property takes a list of
fallback position sets and that grammar is still moving; the flip is the part
that has settled and is what a popover near an edge needs. A flipped box is
mirrored about its anchor rather than pushed inside the viewport, because a
tooltip shoved sideways to fit stops pointing at anything.

### Paint

| Property | Values |
| --- | --- |
| `background`, `background-color` | colour |
| `color` | colour |
| `border` | `<width> [solid] <colour>` in any order |
| `border-width` | length |
| `border-color` | colour |
| `border-radius` | length — **parsed, not yet drawn** |
| `font-size` | length |

## Values

**Lengths.** `12px` or a bare `12`; both are pixels. A fraction such as
`12.75px` is floored at parse time, because the layout is integer end to end
and rounding it later in two different places is how a one pixel seam appears.

**Colours.** `#rgb`, `#rrggbb`, `#rrggbbaa`, and `transparent`. Named colours
are absent: a table of a hundred and forty names earns its place in a browser,
not here.

**Comments.** `/* ... */`, anywhere whitespace is allowed.

**`env(name)` and `env(name, fallback)`.** Eight names, in two families:

| Name | Meaning |
| --- | --- |
| `safe-area-inset-top` `-right` `-bottom` `-left` | how much of each edge is covered by something the window does not control |
| `titlebar-area-x` `-y` `-width` `-height` | where the window controls sit, when the backend draws its own |

The values come from the backend through `ar_set_safe_area` and
`ar_set_titlebar_area`. A backend that says nothing leaves them unknown and
every `env()` naming one takes its fallback — which is **not** the same as a
backend reporting zero. A windowed desktop has real insets of zero, so
`env(safe-area-inset-top, 20px)` resolves to `0` there and to `20` on a backend
that has never heard of safe areas.

An unknown name takes its fallback too. Without a fallback it has no value and
the declaration is dropped.

**`viewport-fit`**, set through `ar_set_viewport_fit_cover`, decides two things
at once. With `auto` — the initial value — the layout viewport is the surface
with the insets already taken off, and `env(safe-area-inset-*)` reports zero,
because there is nothing left for the stylesheet to avoid. With `cover` the
viewport is the whole surface and the real insets are reported. The pair always
moves together: a viewport inset by the safe area *and* an `env()` reporting
that inset would take it off twice.

`titlebar-area-*` is not gated on `viewport-fit`. Where the window controls are
does not change because the viewport was inset.

One deviation: a known name whose backend stayed silent and which carries no
fallback resolves to `0`, where CSS makes it invalid at computed-value time.
There is no way to say "invalid at computed-value time" here yet.

## The font-size scale

The embedded face is 8 pixels tall and is drawn at integer scales, so
`font-size` is rounded down to a multiple of 8 and clamped at one face:

| Written | Drawn at |
| --- | --- |
| `8px` … `15px` | 8 px |
| `16px` … `23px` | 16 px |
| `24px` … `31px` | 24 px |
| `32px` | 32 px |

Smooth text at arbitrary sizes is the optional TrueType module, later.

## Not implemented

Named so that their absence is a decision rather than an oversight:

- named grid lines and `grid-template-areas` — a name table on top of the track
  model rather than a change to it
- `repeat(auto-fill)` and `repeat(auto-fit)` — the count cannot be known until
  the container has a width, and a track list is parsed when the stylesheet is
  handed over. Refused at parse time rather than guessed
- subgrid — 0.8.2
- `box-shadow`, gradients, `opacity` on a whole subtree
- media queries, container queries, `@` rules of any kind
- custom properties, `var()`, `calc()`
- attribute selectors, pseudo-elements, `:nth-child(an+b)`
- writing modes and logical properties, so no `-inline` or `-block` longhands
- the `display: contents` exceptions for replaced elements, form controls and
  table parts — every one of them needs a tag name, and there are none yet
- `scroll-behavior: smooth` — it wants the frame scheduler, which is 0.14.0
- `border-style`, and per-side border widths — so a collapsed border resolves by
  width and origin and not by style
- table fragmentation across pages or columns
- `env()` names beyond safe-area and titlebar — nothing can supply them
- `<dialog>`, `popover`, `popovertarget` — HTML, and there is no parser yet
- focus of any kind, so no focus trap, no `autofocus`, no Escape to dismiss
- `position-try-fallbacks` as a list — the grammar is still moving
- `position-area` — tracked, no version
- every pseudo-element except `::backdrop`
- shorthand `font`, `background` with anything but a colour
- `font-family` is parsed and ignored: one face at a time, chosen by
  `ar_font_load`

## Fonts

`font-size` is in pixels and both faces honour it. Without a loaded face it
picks a whole-number scale of the built-in 8x8 bitmap font, rounded down and
clamped at 1. With one it is the pixel size handed to the rasterizer.

`font-family` is parsed and ignored. Which face draws is decided by
`ar_font_load` and `ar_font_add` rather than by the stylesheet, because
selecting between families needs a font database and that is later in 0.2.x. A
stylesheet naming families is not an error; it simply does not choose yet.

Antialiasing, grid fitting, stem darkening and subpixel positioning are not CSS
properties in any specification and are not invented as ones here. They are
context settings: `ar_font_antialias`, `ar_font_grid_fit`, `ar_font_darken`,
`ar_font_subpixel`.
