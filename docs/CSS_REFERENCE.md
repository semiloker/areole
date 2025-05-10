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

**One class per selector.** `.nav.selected` is not supported — matching it
means more than a hash compare. Use a second class name (`.nav-selected`).

**No descendant selectors.** `.panel .btn` would mean walking the ancestor
chain for every box on every frame. Nothing real has needed one yet; if
something does, it is a change to `ar_sheet_resolve` and a cache.

Ties in specificity are broken by source order: the later rule wins. Rules are
sorted once at parse time, so resolution is a single forward pass.

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
| `overflow` | `visible`, `hidden`, `scroll` — `scroll` currently clips |

## Values

**Lengths.** `12px` or a bare `12`; both are pixels. A fraction such as
`12.75px` is floored at parse time, because the layout is integer end to end
and rounding it later in two different places is how a one pixel seam appears.

**Colours.** `#rgb`, `#rrggbb`, `#rrggbbaa`, and `transparent`. Named colours
are absent: a table of a hundred and forty names earns its place in a browser,
not here.

**Comments.** `/* ... */`, anywhere whitespace is allowed.

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
- `position: absolute` and `fixed`
- `align-self`, `flex-basis`, `flex-shrink`, `order`
- `box-shadow`, gradients, `opacity` on a whole subtree
- inheritance — `color` does not cascade to children; set it where you use it
- `!important`, media queries, `@` rules of any kind
- shorthand `font`, `background` with anything but a colour

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
