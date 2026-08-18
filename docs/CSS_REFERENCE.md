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
| `display` | `flex`, `block`, `inline-block`, `inline`, `none` |
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

- `flex-wrap` — the next real gap in the solver. Use explicit rows meanwhile.
- `align-self`, `flex-basis`, `flex-shrink`, `order`
- `box-shadow`, gradients, `opacity` on a whole subtree
- media queries, container queries, `@` rules of any kind
- custom properties, `var()`, `calc()`
- attribute selectors, pseudo-elements, `:nth-child(an+b)`
- writing modes and logical properties, so no `-inline` or `-block` longhands
- `scroll-behavior: smooth` — it wants the frame scheduler, which is 0.14.0
- grid, tables
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
