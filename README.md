# areole

**A GUI library in strict C89 that uses no graphics API.**

No Direct2D. No OpenGL. No Vulkan. No SDL. No GTK. `areole` rasterizes every
pixel itself and hands the finished buffer to the operating system in a single
blit. Layout is written in **real CSS**, parsed once at startup.

![the hello example](docs/hello.png)

Every rectangle above came out of a stylesheet. The example that draws it does
not contain a single coordinate.

```c
ar_stylesheet(ui,
    ".rail    { width:220px; display:flex; flex-direction:column; padding:16px; gap:2px; }"
    ".nav     { padding:9px 12px; font-size:16px; color:#8a8175; }"
    ".nav:hover { background:#f0e9db; color:#2b2b2b; }");

ar_begin(ui, "div.rail");
    for (i = 0; i < 5; ++i)
        if (ar_button(ui, "div.nav", pages[i])) selected = i;
ar_end(ui);
```

## Why

There are excellent immediate mode GUI libraries. None of them is this one.

| | C89 | zero deps | software blit | declarative flex layout |
| --- | :-: | :-: | :-: | :-: |
| [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) | yes | yes | yes | **no** — manual rows and columns |
| [Clay](https://github.com/nicbarker/clay) | **no** — C99 compound literals | yes | **no** — emits commands only | yes |
| [Dear ImGui](https://github.com/ocornut/imgui) | **no** — C++ | no | **no** — GPU only | no |
| [LVGL](https://github.com/lvgl/lvgl) | **no** — C99 | libc | yes | yes |
| [luigi](https://github.com/nakst/luigi) | **no** — C99 | yes | yes | **no** — retained, manual |
| **areole** | **yes** | **yes** | **yes** | **yes** |

## Two invariants

Everything else is negotiable.

**No heap allocation after `ar_init`.** The caller hands areole one block and
that is all it ever gets. The example makes exactly one allocation, and the
operating system did it before `main` ran:

```c
static unsigned char memory[AR_MEM(512)];
ar_ctx *ui = ar_init(memory, sizeof memory);
```

**No floating point in the layout or raster hot path.** Flex slack is
distributed in integers and the rasterizer never sees a float. A Pentium III
has an FPU; it is still slower and less predictable than the integer unit, and
predictability is what makes p99 frame time equal p50.

## Numbers

areole measures itself. The phases are reported separately because a single
frame time hides the one thing worth knowing on old hardware: whether the cost
is the rasterizer or the blit. Those have entirely different fixes.

areole repaints only what changed, so an interface has two costs and both
matter. The shipped `dashboard` example -- rail, nav, six cards, 49 boxes, 182
glyphs -- at 1024x768 on a Ryzen 7 8840HS:

| | |
| --- | --: |
| Steady frame, cursor drifting | **8.6 us** |
| p99 | 20.5 us |
| Full repaint, everything invalidated | 233 us |
| Heap allocations after init | **0** |

The first number is what the machine actually pays, because most frames change
nothing and paint nothing. The second is the ceiling, measured with
`ar_bench --full-repaint`, and it is the one the Pentium II budget is checked
against: a machine that cannot afford its worst frame does not have a working
interface, it has one that stutters.

Per unit, which is what scales to a slower machine:

| | |
| --- | --: |
| Opaque fill | **0.22 ns/pixel** |
| Full-surface write, uncached | 0.20 ns/pixel |
| Glyph | **47 ns** |
| Box, style and layout | **138 ns** |

The glyph figure used to be the bad one. The blitter tied with GDI on text
while beating it three to ten times on everything else, which is how we learned
it was about fifteen times slower than it should be; rewriting it to work in
spans rather than per bit made it 10.7x faster and turned that tie into 10.4x.

Averages are not reported. A UI that is smooth apart from one stall every two
seconds has an excellent average and is unusable.

Every scene, every percentile and the full derivation:
**[docs/PERFORMANCE.md](docs/PERFORMANCE.md)**, which is generated from measured
JSON and checked by CI so a published number cannot drift from a measured one.

### What the style cache bought

Style resolution was 50 to 89% of every tree-driven frame, and grew linearly
with the size of the stylesheet because every box was matched against every
rule. It is now keyed on the tuple that decides the answer -- tag, class, id and
state -- so a thousand cards sharing a class resolve once.

| rules in the sheet | before | after |
| --- | --: | --: |
| 13 | 108 us | **51 us** |
| 103 | 239 us | **78 us** |
| 253 | 416 us | **80 us** |

The ratios matter less than the shape: near-flat where it used to be linear.
That is what makes a real user-agent stylesheet affordable, and it is why the
cache was built before the cascade rather than alongside it.

Sixty-four entries hold one style per distinct selector-and-state combination,
not one per box. Every scene reports a hit rate of 1.000 except the dashboard at
0.996, and that rate is published per scene rather than assumed -- a table too
small does not fail, it quietly goes back to scanning every rule for every box.

### What damage tracking bought

Every scene in the library, 0.1.1 against 0.1.2, same machine:

| scene | before | after | |
| --- | --: | --: | --: |
| `dashboard` | 344 us | 12 us | **28.2x** |
| `deep_60` | 67 us | 11 us | **6.0x** |
| `flat_100` | 74 us | 13 us | **5.6x** |
| `scroll_10k` | 707 us | 173 us | **4.1x** |
| `flat_1k` | 327 us | 157 us | **2.1x** |
| `table_1k_rows` | 2883 us | 1543 us | **1.9x** |

Twenty-six of twenty-eight scenes improved. `many_short_labels` is 1.10x
*slower* and stays that way: 240 labels means hashing 240 strings, and text has
to be hashed by content rather than by pointer because formatting a label into
a reused buffer every frame is the ordinary way to write an immediate mode
interface, and that leaves the pointer identical while the pixels differ.

The damaged region is a list of up to eight rectangles rather than one merged
box, and that is not a refinement. Two boxes in opposite corners with one
changing -- a status bar and a clock -- merge into a whole-window rectangle:
480,000 pixels presented to update 768. On a Pentium II that is 7.68 ms against
0.01 ms, 46% of the frame budget. Keeping the rectangles separate costs fifty
lines and presents exactly the 768.

The price is `arena_churn`, a scene that rebuilds four thousand boxes every
frame, which is 25% slower. That is a shape no real interface has, traded
against one every interface has.

## Style

Real CSS, a subset of it. Selectors carry several classes and combinators:

```css
.card.selected     { background: #2b7; }   /* both classes */
.page .card        { padding: 12px; }      /* a descendant */
#root > .card      { margin: 4px; }        /* a direct child, not a grandchild */
.row + .row        { border-top-width: 1px; }
```

`color` and `font-size` inherit, including through a box that only inherited
them, so a stylesheet states them once rather than on every rule.

Resolution is cached on the selector alone — tag, classes, id and state — which
is why it is fast. Inheritance and combinators are applied outside that cache,
because both depend on where a box sits and a cache keyed on the ancestor path
would not be a cache.

## Text

Without a font file, areole draws with a built-in 8x8 bitmap face. That is why a
hello-world build is 52 KB and needs nothing on disk.

Given a font file it draws outlines: its own TrueType and CFF parsers, its own
scanline rasterizer with exact analytic antialiasing, its own glyph cache, its
own UTF-8 and its own line breaking. No FreeType, no HarfBuzz, no stb_truetype,
and no floating point anywhere in any of it.

```c
ar_font_load(ui, ttf, ttf_size, 256 * 1024, 48);  /* TrueType or OpenType */
ar_font_add(ui, cjk, cjk_size);                   /* fallback for what it lacks */

ar_font_antialias(ui, 0);   /* hard edges, 1.69x faster */
ar_font_grid_fit(ui, 1);    /* snap the x-height to the pixel grid (default) */
ar_font_darken(ui, 60);     /* lift midtones so thin stems stop reading grey */
ar_font_subpixel(ui, 1);    /* four positions per pixel (default) */
```

| | ns/glyph |
| --- | --: |
| Antialiased, cached | 222 |
| Aliased, cached | **132** |
| Rasterized from the outline | 1780 |

Read the ratios rather than the absolute figures, which move with machine load
while the ratios do not. **Antialiasing off is 1.69x**, because a blend reads
and writes where an opaque store only writes, and on a machine whose whole
problem is memory bandwidth that is the entire difference. **Rasterizing costs 8.0x blitting a cached glyph**, which is why the cache is not an optimisation
but the thing that makes outlines usable at all.

**Line breaking is UAX #14**, not "at spaces". `hello world` breaks after the
space and never before it; `one-two` breaks after the hyphen; `1,000.50` does
not break anywhere; a closing bracket is never orphaned onto the next line;
Japanese breaks between ideographs. What the table covers, and the scripts it
does not, are written beside it.

**Fallback is a chain.** No face covers Unicode, and asking a Latin face for a
Japanese character gives the notdef box everyone recognises. Each character is
drawn by the first face in the chain that has it.

## Text that behaves

A string of characters is not a list of glyphs, and the order it is stored in
is not always the order it is drawn in.

**Ligatures and kerning** come from the font's own GSUB and GPOS tables, on
whenever a face carries them. In Constantia, `office` is four glyphs rather than
six and `AV` is 180 units tighter; `nnnn` is correctly left alone.

**The bidirectional algorithm** is UAX #9, not "reverse the Arabic parts".

```
abc אבג     Latin drawn first, then Hebrew
אבג abc     the Latin drawn FIRST, because in an RTL paragraph the last
            logical text is leftmost on screen
אב 123      the digits two levels up, not one, or they render as 321
```

**Line breaking** is UAX #14, not "at spaces". `one-two` breaks after the
hyphen, `1,000.50` does not break at all, a closing bracket is never orphaned,
and Japanese breaks between ideographs.

**Arabic joins.** Letters have up to four shapes depending on what they connect
to, and alef joins only rightward, so the letter after it starts a new group.
Against Arial, `beh beh beh` becomes initial, medial and final forms; `alef beh`
correctly leaves the beh isolated; `lam alef` becomes the one glyph it must --
and still does when a vowel mark sits between the two, which is how Arabic is
actually written.

**Marks stack.** A diacritic has no position of its own: the font gives an
anchor on the letter and one on the mark, and they are made to coincide. In
Arial a fatha over a beh lands at +288,-220 font units; put a shadda between
them and the fatha moves to +120, above the shadda rather than through it. A
kasra stays on the letter, because it belongs below and the font says so.

**Marks sit where the font says.** A diacritic has no position of its own: the
font gives an anchor on the letter and one on the mark, and they are made to
coincide. A fatha over a beh comes out at +288,-220 font units with zero
advance. Without that, every mark lands on the baseline at the origin, which
does not read as plain text -- it reads as text that has lost its marks.

**Devanagari reorders.** The vowel sign i is typed after its consonant and
drawn before it, so `ki` is stored KA + I and rendered I + KA. A syllable
opening ra + virama loses both to a reph mark above the *end* of the syllable.
Drawing storage order does not give plain text, it gives a different word.

**Fallback** is a chain: each character is drawn by the first face that has it,
so a Latin face plus a CJK face renders both rather than one and a row of tofu.

## Against the alternatives

Same machine, same process, same output buffer, alternating one frame each so
neither engine sits on a warmer chip. A ratio above 1.00 means areole is faster.

| case | rival | areole | rival | ratio | read |
| --- | --- | --: | --: | --: | --- |
| `clear_uncached` | Win32 GDI | 79 us | 83 us | **1.05x** | solid |
| `fill_opaque` | Win32 GDI | 308 us | 1225 us | **3.98x** | solid |
| `fill_blend` | Win32 GDI | 2515 us | 14803 us | **5.89x** | solid |
| `latin_paragraph` | Win32 GDI | 98 us | 1015 us | **10.40x** | solid |
| `hairlines` | Win32 GDI | 36 us | 421 us | **11.77x** | solid |
| `flat_1k` | Clay | 664 us | 335 us | *0.51x* | marginal |
| `flat_8k` | Clay | 7556 us | 2973 us | *0.39x* | solid |
| `flat_1k` | microui | 723 us | 14 us | *0.02x* | marginal |
| `flat_8k` | microui | 6336 us | 104 us | *0.02x* | solid |

`read` is whether the ratio survives the noise it was measured in: **solid**
when the effect is more than twice the combined per-epoch spread, **marginal**
when it is not. Regenerate the whole table with `ar_compare --all --json` and
`tools/gen_compare_doc.py`; the full version with every caveat is
[docs/COMPARISON.md](docs/COMPARISON.md).

**What this actually says, in three lines.**

*Rasterizing: areole wins comfortably.* 4.0x GDI on opaque rectangles, 5.9x on
translucent ones, 11.8x on hairlines where per-call overhead dominates.
`fill_blend` is flattered -- GDI's `AlphaBlend` must read a source surface areole
does not need -- and `clear_uncached` is nearly a tie because at 3 MB per pass
both engines are simply waiting on memory, which is the correct answer.

*Text: 10.4x, and it used to be a tie.* That tie was the most useful number the
comparison ever produced. A bitmap blitter has no business being level with
hinted, kerned, antialiased outlines rendered through the system font stack, and
it was not the outlines that were slow -- the span blitter was writing a pixel
at a time. Per-span rather than per-bit made it 10.7x faster.

*Layout: areole loses to both immediate-mode libraries, and the gap has grown.*
Clay lays out `flat_8k` 2.5x faster and microui 60x. **This table used to claim
the opposite**, from figures measured when areole's layout was a single pass over
boxes that stated their own sizes. It is now two passes per axis over a retained
tree that solves grow and shrink, floats, margin collapsing, grid tracks, table
columns and line breaking -- and it costs what that costs. microui advances a row
cursor; the comparison is real but it is not like for like, and the honest
summary is that **areole buys CSS layout and pays for it.**

Whether it should pay *this much* is an open question and 0.15.0's, and the
figure that will answer it is not in this table: it is how much of the frame is
layout at all. On the interface example that is 12 us of a 30 us frame.


The caveats are not footnotes -- Clay takes its configuration inline while areole
resolves a stylesheet per box, so areole is doing strictly more work in the
whole-frame column; microui neither builds a tree nor resolves style at all.
Each one is stored next to its case in `bench/compare/` so it cannot drift away
from the number it qualifies.

Not yet measured: Nuklear, LVGL, Direct2D.

## Measure it yourself

Nothing above is taken on trust. Every number is reproducible in three commands.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

./build/ar_bench --all --iters 150 --repeat 3      # 28 scenes, this machine
./build/ar_bench --all --iters 150 --repeat 3 --full-repaint   # the worst frame
./build/ar_compare --all --iters 200 --repeat 3    # against GDI, Clay, microui
./build/ar_hwprobe                                 # what the machine can do
```

Then the two questions that matter:

```sh
# Will it hold 60 fps on the machine I care about?
./build/ar_require --scene dashboard --fps 60 --res 640x480 --bpp 32     --results bench/baseline.json     --reference bench/profiles/reference-ryzen-8840hs.json     --target bench/profiles/pentium2-400.json

# Did my change make anything slower?
./build/ar_bench --all --iters 150 --repeat 3 --compare bench/baseline.json --gate
```

`ar_bench_selftest` runs 32 checks on the measurement tool itself, because a
benchmark that lies is worse than no benchmark. It knows the traps: a dead store
the optimiser deleted once reported infinite copy bandwidth, and a constant
stride once made a cache curve go the wrong way. Both now fail loudly.

The tool also reports its own trustworthiness. It measures this machine's
variance between epochs and refuses to gate on a difference smaller than the
noise, rather than claiming a threshold it cannot support.

## Design

```
  ar_begin / ar_button / ar_text        called fresh every frame
             |
             v   flat box array, indices never pointers
    style resolve     hash the class or id, merge base with :hover / :active
    layout            two passes per axis: bottom up fit, top down grow and place
    rasterizer        writes straight into the pixels the OS will blit
             |
             v
       one BitBlt
```

The back buffer *is* the `CreateDIBSection` memory: 32 bpp `BI_RGB`, top-down.
areole rasterizes directly into the pixels GDI owns, so presenting costs one
copy instead of two. Matching the native format is what keeps the driver from
converting every pixel, and on old hardware that is worth far more than which
blit call you choose.

Hover is resolved from the previous frame. It has to be known before the style
is resolved, but it depends on where the box ended up, which is not known until
after layout — hit testing against last frame is how every immediate mode
toolkit breaks that circle.

## Status

**Pre-alpha.** Built in the open, one issue at a time.

- **0.1.0** *It draws* — window, DIB back buffer, rasterizer, bitmap font ✅
- **0.1.1** *It measures* — 47 scenes, hardware probe, comparison harness ✅
- **0.1.2** *It redraws less* — damage tracking, up to eight dirty regions, partial present ✅
- **0.2.0** *It has real text* — TrueType and CFF, an outline rasterizer, a glyph cache ✅
- **0.3.0** *It shapes text* — bidi, ligatures, kerning, Arabic, Indic ✅
- **0.4.0** *It has the cascade* — specificity, inheritance, selector lists, combinators, `!important`, structural selectors, `:not`/`:is`/`:where` ✅
- **0.5.0** *It lays out documents* — block, inline, floats, margin collapsing ✅
- **0.6.0** *It positions and scrolls* — absolute, fixed, sticky, z-index, scroll ✅
- **0.6.1** *It scrolls properly* — both axes, region move, a draggable bar, touchpad travel ✅
- **0.6.2** *It knows where the screen is* — scroll snap, sticky containing blocks, `env()` and the safe area ✅
- **0.6.3** *It puts a dialog on top* — the top layer, `::backdrop`, `inert`, anchor positioning ✅
- **0.7.0** *It has tables* — anonymous boxes, both layout algorithms, spans, border collapse ✅
- **0.7.1** *Its tables behave* — sticky headers, a frozen column, `visibility: collapse`, `caption-side`, `empty-cells`, `col` widths ✅
- **0.8.0** *It lays out in two dimensions* — the rest of flexbox, and CSS grid ✅
- **0.8.1** *It sizes things properly* — `display: contents`, `aspect-ratio`, the intrinsic keywords, safe alignment ✅
- **0.8.2** *Its grids line up* — `subgrid`, and the card layout it exists for ✅
- **0.9.0** *It reads HTML* — the tokenizer, tree construction, encoding, a user-agent stylesheet ✅
- **0.9.1** *It agrees with a browser* — every element's defaults, presentational hints, quirks mode, and a demo gallery measured against Chrome ✅
- **0.9.2** *It adapts* — `@media` with Media Queries Level 4, and `@supports` answered from the implementation ✅
- **0.9.3** *It parses the awkward third* — the stack of template insertion modes, foster parenting, and twenty insertion-mode rules ✅
- **0.9.4** *It measures in every unit* — the whole of CSS Values Level 4's lengths, and the user-agent sheet rewritten in the `em` it always meant ✅
- **0.9.5** *It does arithmetic* — `calc()` and the maths functions, custom properties and `var()` ✅

Minor releases add architecture, patch releases add CSS and HTML coverage.

### 0.9.0, complete

The parser is real and it is public. `ar_html_parse_into` builds a document, `ar_dom_build` walks
it into the box tree through the same calls a hand-written interface makes, and everything after
that is the engine the other releases built.

Where it stands against the conformance suites, which are vendored and run offline:

| | |
| --- | --- |
| html5lib tokenizer | **7,026 of 7,026 — 100%**, nothing skipped, a CI gate |
| html5lib tree construction | **1,884 of 1,922 — 98.0%**, 8 scripting cases skipped, a CI gate |
| Named character references | all **2,231**, generated from the standard's own JSON and checked against it |
| Browser tree corpus | **183 of 183** documents agree with Edge exactly |
| Fuzzing | **50 million** iterations, five seeds, no crash, no hang, no overrun |
| Encoding sniffing | **50 documents**, the specification's prescan, not a search for the word |
| Quirks mode | **34 doctypes**, agreeing with Edge on every one |
| Parse throughput | **40.0 MB/s** on this laptop, against a 30 MB/s floor |
| Real documents | **10 saved from the web**, nine recognisable, the tenth named |

What is missing is named rather than implied: a real stack of template insertion modes, the
`<selectedcontent>` mirror, and a tail of thirty-eight cases listed by cause in the release
document. That work is 0.9.3.

**Reading real documents changed the layout engine more than it changed the parser.** A page whose
boxes come from markup is not shaped like one a program declares, and eleven bugs only that
difference could expose came out of a single example:

- **A block is as tall as the blocks inside it.** Heights sweep up while widths sweep down, and
  `<div><p>two lines</p></div>` — the ordinary shape of every page — reported one line, so what
  followed was drawn inside the paragraph.
- **A grid track and a table row are as tall as what wrapped inside them**, measured at the
  settled column width rather than at max-content.
- **Each box is laid out once**, not once to measure and once to place. `inline_wrap` −45%,
  `float_gallery` −41%, `grid_20x20` −20%.
- **`:hover` matches the ancestors of the box under the cursor**, which is every styled element on
  a parsed page, because the text is always in a child.
- **Whitespace collapses on the way into a box**, not in the tree, where html5lib compares bytes.

Two more came out of the same work and are fixed here: a grid track and a table row were
sized from unwrapped text, and a flex container's automatic height was never settled at all.

**And then ten real documents, which is the release's last acceptance criterion.**
`examples/15_real` is ten pages saved from the web -- MDN, Wikipedia, a W3C specification,
RFC 2616, two US Government sites -- every licence permitting redistribution, every one
attributed. Nine render recognisably. `rfc2616` does not, because `white-space: pre` line
breaking is 0.5.1's; the Japanese Wikipedia article lays out correctly and draws tofu,
because selecting a face by `font-family` needs a font database and that is 0.2.1's. Both
are named in the release document beside what they need, along with the three inline-layout
faults the corpus turned up.

`./build/example_real` opens the first of them in a window; left and right walk the other
nine. `--dump` prints the corpus table instead, and `--ppm <path>` writes the page as an
image.

It found one bug worth having: areole had **no default canvas colour**, so a document that
declares no background rendered on whatever was already in the surface. Every example in
this tree declares one, which is exactly why five releases never saw it.

```c
/* Reading a document. The input is not copied and must outlive the document. */
ar_ctx *c = ar_init_ex(mem, sizeof mem, 256, 96 * 1024);
ar_doc *d;

ar_ua_stylesheet(c);              /* the default style for every element */
d = ar_html_parse_into(c, bytes, len);
ar_doc_stylesheets(c, d);         /* every <style> in the document, in cascade order */

ar_frame_begin(c, &input);
ar_dom_build(c, d);               /* the document into the box tree */
ar_frame_end(c, &surface);
```

`examples/13_document` is exactly that in a window, and is the only example in
the tree that declares no boxes at all.

### 0.9.1, complete

0.9.0 could read a document. 0.9.1 is about whether what it computes from one is *right*, and the
only honest answer to that is a browser's. Four corpora arrived, all measured rather than recalled:

| | |
| --- | --- |
| Every element's defaults | **1,066 of 1,071** values, 119 elements against nine properties each |
| Presentational hints | **42 of 43** cases -- `width`, `bgcolor`, `align`, `cellpadding`, `colspan` |
| Quirks mode | **39 of 39** cases, against a corpus asked to be thirty |
| The demo gallery | **111 of 111 gated demos** agree at one pixel on every edge, 7 more reported |

`tools/compare_computed.py` drives the first three: the same markup to both engines,
`getComputedStyle` on one side and `ar_computed` on the other. The gallery is
`tools/gallery.py` -- both engines given the same file, geometry from each, pixels diffed by a
PNG reader built out of `zlib`, and a golden gate that runs with no browser at all, which is the
one CI uses.

Five of the 1,071 differ and each is named rather than unknown: `audio` wants
`audio:not([controls])` and there are no attribute selectors yet; `ruby`, `rt` and `rp` want
display values this engine does not have; and `svg` is `inline-block` here against a browser's
`inline`, which is a *choice* -- the closest thing to a replaced inline in an engine with no
replaced elements.

**Comparing against a browser is how you find out what you got wrong.** Every one of these was
invisible to a test suite written by the same person who wrote the engine:

- **The document root was 8px**, because nothing had ever said `html { font-size: 16px }` -- so
  every `em` on a parsed page was half what it should have been.
- **The universal selector did not parse.** `*` was guarded for, after a loop that had already
  failed on it.
- **`margin: 0 auto` centred nothing**, which is the way almost every page on the web is centred.
- **`colspan` and `rowspan` were never mapped**, so every table with a merged cell was wrong.
- **`table-layout: fixed` ignored the widths it was given**, which is the entire point of it.
- **A flex line had no cross gap**, and a collapsed table's intrinsic width was its uncollapsed one.
- **`line-height` was resolved against the wrong number**, and a wrapped paragraph came out as
  many times too tall as it had lines, because a fragment took its height from a field still
  being grown into the union of the fragments before it.
- **A line box had no strut.** CSS 2.1 10.8.1 starts every line with the containing block's own
  font whether or not text is on it; without that, a line holding only an inline-block was short
  by the font's descent, and so was the last line of every paragraph.

Seven demos are reported rather than gated, each with a written cause in
`examples/gallery/_not-gated.txt`. Two of them are the same residual the table corpus is down to:
a browser lays out in 1/64ths and this engine is integers all the way down, so a six-line
paragraph is 134 there and 132 here. That one is not a bug to fix but a coordinate system to
change, and it is written down as such.

**What it cost.** Style resolution roughly doubled on a parsed document -- `html_render` went
from 467 to 962 microseconds of style on the same 1,488 nodes, with layout and raster unmoved --
which is the price of a real user-agent stylesheet, a presentational-hint cascade band, and an
origin as the first sort key on every declaration. `table_1k_rows` paid 19% of the same. Two
scenes went the other way on the place-once work: `offscreen_90pc` −29% and `top_layer_off`
−23%. All 52 scenes are in `docs/PERFORMANCE.md`, stability column included, because a
number this machine cannot reproduce is not a number.

### 0.9.2, complete

This is the content the roadmap files under 0.4.2, shipped as the release after 0.9.1 and for a
reason worth stating: **the release that was going to be 0.9.2 is responsive images, and it cannot
be written without this.** `<picture>`'s `<source media>` and `sizes`' media-condition list *are*
media queries, so the next release turned out to be blocked on one four minor versions behind it.
Responsive images keeps its scope and moves down the list; `srcset` selection still wants an image
decoder, which is 0.11.0's.

**`@supports` answers from the implementation, never from a table.** A property is supported when
this engine's own declaration parser accepts it and sets something. A hand-maintained list of
supported properties is a claim that drifts the moment anything changes, and `@supports` is
precisely the tool a page uses to decide whether to trust us -- so claiming support for something
parsed and ignored would be the most damaging lie available here. The corpus is generated from
`AR_PROPS`, the table the parser itself looks a declaration up in.

**`@media` is Media Queries Level 4**, in both the legacy `min-`/`max-` forms and the range syntax,
including `(400px <= width <= 700px)` and the value-first form. Every feature says where its value
comes from: three come from the window, `aspect-ratio` and `orientation` are computed from those,
and the rest report a documented default until a backend can answer them. Two of those defaults are
permanent rather than pending, and both are useful -- `scripting: none` is how a page tells areole
what to do, and `display-mode: standalone` because an application is not a browser tab.

| | |
| --- | --- |
| Media Queries Level 4 vs Chrome | **624 of 624 — 100%**, 156 queries at four viewports, gate 90% |
| `@supports` vs the parser | **all 91 properties**, eight values each, generated from `AR_PROPS` |
| Range against legacy syntax | **60 cases**, identical answers, boundaries included |
| Demos | **20**, seventeen agreeing with Chrome exactly |
| Resize, 200 rules over 40 queries | **0.006 ms**; unchanged window, free. Budget 1.2 ms on the tier |
| Binary | **+12,764 bytes**, budget 14 KB |

**A resize re-evaluates queries; it does not parse the stylesheet again.** A window drag would
otherwise reparse sixty times a second. Each query records which parts of the media state it
consulted, so one naming only `prefers-color-scheme` is never asked again because the window got
wider -- and a frame where nothing moved costs three integer comparisons, which is what makes it
safe to call from `ar_frame_begin` every frame.

The demos found the hole that mattered. Media queries were answered against the viewport the *last*
frame was drawn into, which is right for a program that draws continuously and wrong for one that
renders a single frame -- a screenshot, a test, a document to an image -- because for it every
frame is the first one, and there is no last viewport. Twenty demos would have shipped silently
taking the false branch. `ar_set_media` is the answer, and the gallery renderer calls it.

Three of the twenty do not agree with Chrome and cannot: `scripting` (Chrome runs scripts, areole
never will), `display-mode` (a browser tab is not an application), and `prefers-color-scheme`,
where the documented default is `light` until 0.16.1 wires the OS and the headless browser reports
`dark`. Each is named in `examples/gallery/_not-gated.txt`. If the first ever agreed, something
would be broken.

### 0.9.3, complete

The last third of tree construction, and the release that was going to be about one subsystem. It
was not: fragment parsing -- the 196-case algorithm 0.9.3 was scoped around -- shipped inside 0.9.0
instead, and what remained turned out to be twenty rules, each small, each invisible until a
document happened to need it.

| | |
| --- | --- |
| html5lib tree construction | **1,914 of 1,922 — 99.58%**, from 1,884 and 98.02% |
| html5lib tokenizer | **7,026 of 7,026 — 100%**, unchanged |
| Browser tree corpus | **183 of 183** documents agree with Edge exactly |
| Fuzzing | a million iterations, no crash, no hang, no overrun |
| Failures left | **8**, each named below |

**A stack of template insertion modes**, 13.2.4.4. A template's mode is the one mode that cannot be
recovered from the shape of the open-element stack, because it depends on what has been seen
*inside* it rather than on what encloses it: two templates in identical positions are in `in row`
and `in body` according to whether a `<tr>` went past. One remembered value was not enough, and the
comment in `ar__reset_mode` had said so for two releases.

**Foster parenting is a flag on the parser**, which is how the specification phrases it -- "enable
foster parenting, process the token using the rules for the in body insertion mode, then disable
foster parenting" -- and not an argument to a single insertion. It was tried twice and reverted
twice before it worked, because it broke six cases while fixing two. It was not wrong; it was
exposing a missing rule. `style`, `script` and `template` inside a table have a rule of their own
and are not the "anything else" that gets fostered, and once they had it the flag fixed both and
broke none.

**Four bugs whose shape was the same: a rule that read the wrong name.** `ar__pop_until(t, "tbody")`
closed a tbody whatever row-group end tag arrived, so `</thead>` silently did nothing. `<![CDATA[`
demanded a byte after it that a file need not have. The in-table ignore list held nine of its eleven
end tags, and the two missing ones -- `body` and `html` -- are the two that do damage rather than
nothing. `applet`, `marquee` and `object` had no rule at all, so the marker they put in the list of
active formatting elements was never there; they are the same three elements `ar__in_scope` already
stops at, and only one of those two boundaries had been implemented.

**And a size budget that fired.** 0.9.2 was cut at forty-four bytes under the 144 KB figure, which
is not headroom, it is a coincidence. These rules cost 1,472 bytes between them. Raised to 150 KB
with the reasoning in `tools/check_size.py`, including the part that weakens it: 0.9.3's own
document allocates "under 6 KB", but that figure was written for fragment parsing and CDATA, which
shipped inside 0.9.0 and are already counted. This is a new allowance, not a draw-down.

Eight failures remain and none is an insertion-mode rule. Four are `<selectedcontent>`, an element
that mirrors another element's content -- a feature, not a rule, and named as deferred since 0.9.0.
Three are foreign content and a template meeting each other. One is a case where the vendored suite
and the current specification disagree: `<textarea>` inside a select fragment, where the suite
expects the element to be inserted and the rule that covers `input`, `keygen` *and* `textarea` says
to ignore it. That one is written down rather than papered over by special-casing a tag to match a
test.

### 0.9.4, complete

**This is the content the roadmap files under 0.4.1**, and it ships under 0.9.4's number for the
same reason 0.4.2's content shipped as 0.9.2: a version may not move backwards. The roadmap number
says what the work is; the version says when it landed.

**`AR_VERSION_STRING` is 0.9.4, and the baseline under it is the best-measured one in the
repository.** The version and `bench/baseline.json` move together -- `gen_perf_doc.py --check` ties
them, so that no published number can be labelled with an engine it was not measured on. The stamp
waited two releases for a quiet machine, and the reason it never came was not the browser or the
editor. **It was the power plan.**

| | this baseline | 0.9.3 | the three discarded attempts |
| --- | --- | --- | --- |
| Median spread | **2.41%** | 3.8% | 16.3% -- 21.9% |
| Scenes above 3% | **18 of 52** | 27 of 52 | 41 of 52 |

One run on **Balanced** measured 16.9% median with 41 of 52 scenes past the 3% a gate needs. The
same binary, on the same machine, minutes later on **High performance**: 2.41% and 18 of 52.
Nothing else was closed. A balanced governor clocks up and down *during* the timed window, which is
indistinguishable from noise in a spread column and is not noise at all.

**Which closes a question 0.9.2 left open and 0.9.3 could only suspect.** 0.9.2's baseline had
`clear_uncached` and `flat_8k` mysteriously slow; 0.9.3 found them 50% and 40% faster without
either release touching them, and wrote down "a degraded power state and not a regression, exactly
as that commit suspected but could not show." This is the showing. The governor was the variable
all along, and it is now the first thing to check rather than the last.

**Three scenes above 3% here are not noise either, and the tool says so itself.**
`opposite_corners` reports 100% spread on a **0.1 us** p50 with `below_timer_floor` set;
`corners_tree` and `html_small` are 2 us scenes where one microsecond of timer quantisation reads
as 40%. The genuinely variable ones are the large allocating scenes -- `flat_8k` at 10%,
`table_auto_10k` at 5.6% -- which is where 0.9.3 had them too.

The two commands, for the next release that needs them:

```sh
./build/ar_bench --all --iters 150 --repeat 3 --json > bench/baseline.json
python tools/gen_perf_doc.py            # after bumping AR_VERSION_* in include/areole.h
```

The release that was skipped. 0.4.2 was built out of order because 0.9.2 turned out to be blocked
on media queries, and 0.4.1 -- the units underneath them -- stayed unbuilt. So the engine had `px`
and `%` and nothing else: `width: 2em` did not become two pixels, it was **dropped whole** and the
box fell back to `auto`, and `@media (min-width: 40em)` matched at no width at all.

| | |
| --- | --- |
| Media queries vs Edge | **1,200 of 1,200 — 100%**, from 624 of 624 |
| Units corpus vs Edge | **35 of 35 — 100%** computed values |
| Element defaults vs Edge | **1,066 of 1,071**, unchanged |
| Demos | **20 new**, 17 gated against the browser |
| Checks | **1,590** in `ar_test`, from 1,573 |
| Binary | `ar_css.c.obj` **+2,152 bytes** of a 14 KB budget |
| Layout cost | none measurable |

**Every unit CSS Values Level 4 defines a length in**, in one table -- because the suffix after a
number is read in four places, and four copies of a unit list is four places for a unit to be
missing from. The absolute seven convert where they are parsed, since their ratio to a pixel is a
constant CSS states; `em`, `rem`, the four font metrics and their root-relative twins, and all
twenty-four viewport spellings carry their unit to resolution instead.

**The two moments they resolve at are the design, and neither can be moved.** A font-relative unit
has to resolve while the tree is being declared, interleaved with inheritance: `font-size: 2em` on
a parent must become a number before the child that inherits font-size copies it, or the child
inherits the *unit* and resolves it a second time against its own parent -- four times the font
rather than twice. A viewport unit must **not** resolve there, because the surface for this frame
is not known until `ar_frame_end`. Resolving it against the previous frame is what a media query
does and is documented as doing; a length cannot afford it, because on the very first frame there
is no previous one and `height: 50vh` drew nothing at all.

**The user-agent stylesheet says `em` now**, which is what the HTML rendering section always
specified and what its own comment had been asking for since 0.9.1. A page that sets
`html { font-size: 20px }` gets a 40px `h1` instead of the 32px one it got at every root.

One line did not come along, and the number is the interesting part. Every heading rounds its font
size to a whole pixel -- `h6` is 0.67em of 16, so 10.72 becomes 11 -- and a margin in `em` then
multiplies that rounding. At 2.33em the 0.28 of a pixel becomes 1.02, which is 26px against a
browser's 24.9776 and just past the one-pixel criterion the corpus is scored on. So `h6`'s margin
stays in pixels: the sheet is converted exactly as far as integer font sizes allow, and `h3`'s
margin actually got *closer* than the pixel value it replaced.

**The media corpus had been written entirely in pixels.** It scored 624 of 624 against Chrome while
`40em` matched nothing, because every length in all 156 of its queries was a `px`. Agreeing
perfectly about the queries it asked said nothing about the ones it did not -- which is the failure
mode a corpus exists to be immune to. It is 300 queries now, and 1,200 of 1,200 agree.

**What this release does not do.** Its fourth acceptance criterion was that a percentage resolve
against the correct reference for every property, and it does not: `padding: 10%` is stored with
its unit and then read as a raw number, so ten per cent of a 400px containing block comes out ten
pixels. `width` and `height` are correct. It is not a units change -- block flow, flex, grid and
the table each resolve sizes in their own code and there is no one place a padding becomes a
number -- and `css/units/percentage-references` is in the gallery, red, as the thing that will go
green.

Angle, time and frequency units are not shipped either, and the reason is not effort: no property
in this engine takes one. There is no `transform` to turn by a `deg` and no `transition` to run for
an `ms`, so the parser could accept them tomorrow and nothing would read the number. They ship with
their consumers.

**And a length is a whole number of pixels.** `0.9em` at a 16px font is 14.4 in a browser and 14
here, which is invariant 2 rather than a rounding bug. The number is carried in hundredths of its
unit through resolution and rounded once at the end, so the error is never more than half a pixel
and never accumulates -- but it is why `h6` kept its margin, and it is the same residual the table
corpus and the gallery's text demos are down to.

### 0.9.5, complete

**This is the roadmap's 0.4.3**, and it ships under 0.9.5's number for the reason 0.4.1's content
ships under 0.9.4's: a version may not move backwards.

| | |
| --- | --- |
| calc corpus vs Edge | **109 of 109 — 100%** computed values |
| Demos | **18 new**, 18 of 18 gated; 176 in the gallery |
| Checks | **1,634**, from 1,594 |
| Binary | `ar_css.c.obj` **+6,736 bytes** of the 20 KB this release asked for |
| Baseline | **2.20%** median spread, 19 of 52 scenes above 3% — the best in the repository |
| Regressed | **`html_malformed` +10%**, named below rather than absorbed |

**`calc()` did not fail. It answered.** That is why it was the first thing this release did:

```
calc(100px + 50px)        100    want 150
calc(50% - 10px)          200    want 190
calc(2 * 100px)           100    want 200
clamp(100px, 50%, 300px)  100    want 200
min(200px, 300px)         200    right, and by luck
```

Every maths function took its first term and discarded the rest. That is worse than the `em` 0.4.1
fixed: a dropped declaration leaves a box visibly the wrong size, and a plausible number leaves a
page that looks laid out and is not. Six of the ten documents in `examples/15_real` use these, two
of them 725 times.

**An expression is compiled to postfix, into a pool on the sheet, and evaluated at**
**`ar_frame_end`.** The pool is the shape a track list already takes and for the same reason — a
style slot is sixteen bits and an expression is a tree. Postfix, because evaluating one then needs
a stack and nothing else: no pointers, no recursion at frame time. Frame end, because that is the
one moment both halves are known: the font size was settled during style resolution and the
surface is the argument to the pass.

**Custom properties are scoped, not global, and that was decided by measurement.** The obvious
shortcut — one value per name for the whole document — would have been wrong on the first page
looked at: MDN declares its custom properties on `.button[data-variant=primary]` and on `:hover`,
not on `:root`. A box that declares none points at the scope its parent pointed at, and the chain
names the nearest *declaring* ancestor rather than the parent box, so a document with one `:root`
block has a chain of one however deep the tree gets.

### Seven bugs, and one of them was in the parser

- **The term loop ate the whitespace a sum needs.** CSS requires space around `+` and `-` because
  `-2px` is one token, and that byte is how the two are told apart. Eating it refused every sum —
  and a refused value is not a dropped declaration: the declaration parser recovers by reading the
  next number it finds, which is why `calc(100px + 50px)` came out **fifty**. The second operand.
- **A refused expression left its own text behind** for that same recovery to walk into.
  `calc(50% - 10px)` came out ten and `calc(1px -2px)` came out zero, both looking like an engine
  that had understood something.
- **`:root { --brand: #c02 }` never reached the rule table.** A rule whose only declarations are
  custom properties sets no property bits, and the parser discards a rule with an empty property
  set. The declaration parsed, the pool entry was written, and the rule pointing at it did not
  exist — the commonest way anyone declares one.
- **The substitution pass was gated on flags that did not include it**, so it never ran for a sheet
  whose only reason to run it was a `var()`.
- **The scope tables were never allocated.** The patch that should have added them aborted on an
  unrelated assertion in a different file and reported success for the half it had done.
- **A selector list carried its custom properties only on the first selector.**
- **A custom property holding `2rem` reached layout as a unit layout does not know**, because the
  pass that substitutes it runs after the pass that resolves font-relative lengths.

### Two budgets moved, both on the commit that spent them

`AR_BYTES_PER_BOX` 544 → 552, for the one field a box needs: which set of custom properties it can
see. Eight bytes for a sixteen-bit index, which is alignment rather than waste — `ar_node` holds a
pointer, so it is eight-aligned and was exactly 488, and narrowing the field from thirty-two bits
changed nothing at all. **The assertion in `ar_ctx.c` refused three attempts before this one**, on
the build rather than in review.

`AR_MEM_FIXED` 192 → 208 KB for the five new pools, measured rather than rounded: 208,968 of
212,992, itemised in the header. The assertion now counts every persistent block — including the
calc pool this release added a commit earlier and forgot to put inside it.

And the CSS size budget fired **on the commit that spent the money**, 60 → 80 KB. `check_size.py`
had predicted this release would raise it and demanded a reason; that is the first time either
budget has fired before the fact rather than after.

### What this release does not do

**A percentage inside `calc()` is refused.** `calc(50% - 5px)` is three of the nine expressions in
the ten real documents, and it cannot be a number where the others are: a percentage resolves
against a containing block that layout knows and the evaluator does not, so the honest answer is a
length *and* a per cent, and a style slot holds one sixteen-bit number. Refused rather than
approximated — which is what the engine used to do, and it returned a flat fifty per cent.

**A custom property holds a value, not a token sequence.** `--pad: 4px 8px` is legal CSS and
becomes a shorthand when substituted; that needs the value parser to run again at frame time over
text, and this one parses declarations once. Such a property is stored and marked unusable, and a
`var()` naming it is invalid — the same answer CSS reaches for every use except the one that was
going to work.

**The trigonometric and exponential functions are not built**, and the reason is not effort: `sin()`
takes an angle and returns a number, and there is no property in this engine that takes an angle
for it to feed. They ship with `transform`.

### And a corpus that could not ask about `:root`

The calc corpus disagreed with the browser on thirteen rows, with areole right on every one. The
twin shares one page between every case and keeps them apart by rewriting each case's selectors to
sit under `[data-page="..."]` — which turns `:root` into `[data-page="x"] :root`, matching nothing,
because the root element is not inside the case box.

**A corpus that isolates its cases cannot ask anything about the thing that encloses them.** The
declarations moved to a wrapper class, and `:root` is covered in `ar_test` and in the `vars/*`
demos, where the document is a whole document.

### The gate fired, and the release it fired on had not touched the code

`--gate` flagged two scenes against 0.9.4's baseline, and the first answer -- that a 52-scene pass
is noisy -- was wrong. Both binaries, alternating, three passes each, which is the method the 0.8.2
hunt had to invent for exactly this:

| scene | 0.9.4 | 0.9.5 | |
| --- | --- | --- | --- |
| `html_malformed` | 148.5, 149.2 | 162.9, 164.6 | **+10.0%** |
| `html_malformed_2x` | 293.7, 295.0 | 320.1, 322.5 | **+9.2%** |
| `html_page`, the same document well formed | 201.8, 203.4 | 206.1, 208.2 | +2.3% |
| `html_render`, parse and style and layout | 1406.1, 1416.9 | 1408.6, 1424.2 | +0.3% |

0.9.4 repeats to a third of a per cent across passes. This is real.

**And this release did not touch the HTML parser.** The whole of 0.4.3 is five files --
`ar_css.c`, `ar_css.h`, `ar_ctx.c`, `ar_node.h` and `include/areole.h`. `ar_html_tree.c`,
`ar_html_token.c` and `ar_dom.c` are byte for byte what 0.9.4 shipped. Malformed parsing got ten
per cent slower without a line of it changing.

What is left is second-order: `AR_MEM_FIXED` grew 16 KB for the five new pools, and every box grew
eight bytes for a two-byte `var_scope` field that 8-alignment rounds up -- the field whose width
was already the subject of one wrong guess this release. The malformed path re-walks the open
element stack on every implied end tag, so it is the scene most exposed to a working set that
moved. **That is the surviving hypothesis and not a demonstrated cause**, and the way to settle it
is to pad `AR_MEM_FIXED` on the 0.9.4 binary and see whether the regression appears with no 0.4.3
code present at all. That has not been done.

It is written down here rather than absorbed into the baseline silently, because the baseline is
what every later release is judged against: a number that goes in unremarked is a number that can
never fire again. `html_render` at +0.3% is the reason it is recorded rather than treated as
blocking -- a real page parses, styles and lays out, and pays none of this.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ar_test               # 1274 checks
./build/example_hello         # the dashboard on the front page
./build/example_tour          # one page per release, 0.1.0 to 0.8.0
./build/example_showcase      # one long page using the whole CSS subset at once
./build/example_block         # the block, inline and float corpus, drawn
./build/example_block --dump  # the same, as rectangles, for the comparison
./build/example_tour --selftest   # every page, no window; CI runs this

# on a machine with no display, the benchmarks still run
./build/ar_bench --all --iters 150 --repeat 3
```

## The tour

`example_tour` is one page per release, 0.1.0 to 0.8.0 — fourteen of them — showing what each
added while it runs:

| | |
| --- | --- |
| 0.1.0 | the one static block, the box tree rebuilt each frame, the surface |
| 0.1.1 | every phase and counter, off the same clock the overlay reads |
| 0.1.2 | the damage regions presented this frame, listed as they change |
| 0.2.0 | TrueType outlines, with antialias, grid fit, subpixel and stem darkening as toggles |
| 0.3.0 | Arabic joining, lam-alef, Hebrew, a number inside right-to-left text, Devanagari |
| 0.4.0 | inheritance, combinators, a compound selector, `!important`, striping by `:nth-child` |
| 0.5.0 | margins collapsing, text wrapping around a float, an inline box cut across lines |
| 0.6.0 | boxes overlapping by `z-index`, a pinned badge, a scrollable list with a sticky header |
| 0.6.1 | a strip that scrolls sideways from one declaration, nested containers, a draggable bar |
| 0.6.2 | a header and a footer pinned to the same scrollport at once, and two `env()` bars |
| 0.6.3 | a box that escapes both a clip and a `z-index` of 9999, a tooltip placed by its anchor |
| 0.7.0 | anonymous boxes generated around a stray cell, both border models, a `colspan` at width |
| 0.7.1 | a header that stays while its rows scroll under it, a frozen first column |
| 0.8.0 | a flex row that is three-to-one because a factor is a ratio, and a `120px / 1fr / 2fr` grid |

The toggles are not captions. Switching off antialiasing on the 0.2.0 page changes how the frame
you are looking at is rasterized; switching off shaping on the 0.3.0 page drops the same strings
back to one glyph per character, so the difference is visible rather than asserted.

`--selftest` runs every page through a real frame against a real surface with no window, and CI
runs it, so a page cannot rot unnoticed.

### Against a browser

`examples/02_tour/tour.html` declares the same six pages with the same stylesheet, so both engines
can be asked to lay out the same thing:

```sh
python tools/compare_layout.py --run ./build/example_tour.exe
```

**Seven corpora are checked against a browser, box by box, and five of them agree exactly:**

| corpus | what it checks | result |
| --- | --- | --- |
| `03_block` | block, inline, floats, intrinsic sizing, positioning | **168 / 168** |
| `05_snap` | scroll snapping, 120 pages | **960 / 960** |
| `06_sticky` | `position: sticky` against its containing block | **355 / 355** |
| `07_env` | `env()` and the safe area | **28 / 28** |
| `08_anchor` | anchor positioning and the flip | **168 / 168** |
| `11_grid` | grid, subgrid, track sizing, the card deck | 217 / 218 |
| `15_real` | ten documents saved from the web | by eye, 9 / 10 |
| `ar_hints` | what HTML's legacy attributes compute to | 42 / 43 |
| `ar_elements` | what every element's defaults compute to | 1066 / 1071 |
| `ar_quirks` | what a document with no doctype does differently | 39 / 39 |
| `ar_units` | what every CSS length unit computes to | **35 / 35** |
| `ar_calc` | what calc() and var() compute to | **109 / 109** |
| `media` | 300 media queries, both engines, four viewports | **1200 / 1200** |
| `gallery` | one standalone page per feature, both engines | 163 / 163 gated, 13 reported |
| `09_table` | tables: anonymous boxes, collapse, spans | 616 / 624 |

The table corpus is the honest exception and is not gated: **8 of its 624 boxes still land
somewhere a browser does not**, down from 208. Three model errors in the collapsed border
account for most of the 200 that went, and all three are the same sentence -- a border belongs
to the edges the box actually has. The fourth says it down the other axis: the rows of a
collapsed table tile, each one's box beginning exactly where the one above it ends. The fifth
is about rounding, and is the same on both axes -- a box's two half-lines are one number,
rounded once, not two numbers rounded apart.

The eight that are left are half-pixels. A browser lays these tables out in fractions and
reports a cell as 119 wide whose right-hand neighbour begins at 120.0; this engine has no
fraction to keep and no way to be both. A table's outer border belongs *inside* its box; a middle column's
border is not the table's side edge; a row group's border is its own top and bottom, not a line
between every pair of its rows.

What is left is one question, asked in several places: **how the two halves of a shared line are
divided between the boxes either side of it**. It is worth a pixel per line, and getting it right
means working through CSS 17.6.2's conflict resolution rather than fitting the remaining deltas.
Listed rather than compensated for.
The grid corpus disagrees on exactly one box, `width-fit-content-function`, named in the same way.

**Flex still has no corpus of its own.** Every layout release from 0.5.0 got one, 0.8.x shipped
without, and grid's arrived late; flex's has not arrived at all. It is the next one to build, and
0.9.1's gallery is where it belongs.

The first run needed six compensating rules to get there, and two of the six were bugs worth
fixing rather than documenting: **text never wrapped** — `ar_text_wrap` had implemented UAX #14
since 0.2.0 and the layout solver never called it — and **line boxes came from the bitmap face's
8-pixel cell**, so 13 px text got an 8 px box and had its glyphs clipped through the middle. Both
are fixed. Three differences remain, all deliberate, all written up in
[examples/02_tour/COMPARISON.md](examples/02_tour/COMPARISON.md).

## Requirements

Windows 2000 or newer. A C89 compiler. That is the entire list.

## Documentation

- [Performance](docs/PERFORMANCE.md) — every scene, every percentile, and what each
  number assumed. Generated, never typed
- [Against other engines](docs/COMPARISON.md) — who wins each case against Win32 GDI,
  Clay and microui, how much of it survives the noise, and the caveat beside every
  ratio rather than under it. Generated, never typed
- [The CSS subset](docs/CSS_REFERENCE.md) — every property and selector, and
  what is deliberately missing
- [Contributing](CONTRIBUTING.md) — the two invariants, the C89 rules, and why
  the core and the platform layer are separate targets
- [Third party material](THIRDPARTY.md)

## License

MIT
