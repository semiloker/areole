# areole against a browser, box by box

`tour.html` declares the same pages as `main.c`, with the same stylesheet, so both engines can
be asked to lay out the same thing and the answers compared. Run it:

```sh
python tools/compare_layout.py --run ./build/example_tour.exe
```

It drives both sides — `--dump` for areole, a headless Chromium for the other — and matches boxes
on their path through the tree, which is the one identifier the two genuinely share.

## The result

**The twin lags four releases and this is the first thing to know about it.** `main.c` has
fourteen pages, through 0.8.0; `tour.html` stops at 0.6.0 and has eight. So the comparison covers
8 of 14 pages, and the 0.6.1, 0.6.2, 0.6.3, 0.7.0, 0.7.1 and 0.8.0 pages are checked by
`example_tour --selftest` in CI and by no browser anywhere.

Re-measured 2026-08-21 at 0.8.2, Edge headless, Segoe UI, 640x480 viewport:

| | |
| --- | --- |
| Boxes matched by path | **215** |
| Boxes that disagree | **11** |
| Pages covered | 8 of 14 |

The 11 are the seven geometry divergences on the 0.4.0 and 0.5.0 pages that were already known,
plus boxes whose text has drifted from the C source. **They are the known state**: anything other
than 11 from this corpus is a change somebody just made.

The five purpose-built corpora are the ones that gate -- block 168/168, snap 960/960, sticky
355/355, env 28/28, anchor 168/168, all agreeing exactly. This one is a breadth check across
releases, not a conformance test, and its twin needs six pages written before it is either.

Every box that flex places lands on the same rectangle in both engines, exactly. Boxes whose width
comes from measuring their own text land within one pixel — which is as close as two independent
rasterizers of the same TrueType outlines get, and closer than expected.

**This is not a statement that areole is a browser.** It is a statement that for the subset both
engines implement, they agree. The subset is small, and the three remaining differences below are
the shape of what is still missing.

## What the first run found, and what was done about it

The first comparison needed **six** compensating rules to reach zero. Three of the six were
limitations rather than choices, and two of those were serious enough to fix on the spot.

### Fixed

**Text did not wrap.** `ar_text_wrap` had implemented UAX #14 since 0.2.0, was tested, and *nothing
in the layout solver ever called it*. A box was one line however narrow it was, and long text ran
off the right-hand side. The solver now takes a wrapping callback — it still knows nothing about
fonts — and `ar_ctx` supplies one that wraps through its fallback chain.

**Line boxes came from the bitmap face's cell.** Text height was `8 × floor(font-size / 8)`, so 13 px
text got an 8 px box — and since text is clipped to its box, the bottom of every glyph was cut off.
This is the bug behind "text doesn't fit and 50% is cut". Line boxes now come from the face's own
ascender, descender and line gap, each rounded to a whole pixel and then added.

That last detail matters and looks wrong at first: rounding three times throws away more than
rounding the sum once. It is what every browser does, because the ascent alone is the baseline
offset and a baseline has to sit on a pixel. Round the sum instead and the baseline and the line box
disagree by a fraction that accumulates down a paragraph. Matching the browser here is what took
the comparison from *close* to *exact*.

### Still different, and deliberate

| | Difference | Verdict |
| --- | --- | --- |
| 1 | **Default display** is flex-row, not block | There is no block layout to default to; it arrives in 0.5.0 |
| 2 | **No automatic minimum size.** CSS refuses to shrink a flex item below its content and overflows the container; areole clamps to the parent and overflows the content | Arguably the better behaviour for a fixed viewport |
| 3 | **No shrinking.** A CSS flex item shrinks when the line is over-full; areole never does | A limitation, and a quiet one — on the 0.3.0 page the browser compresses to fit and areole runs off the bottom |

A border still does not take space, and that stays deliberate — it just no longer needs a rule
here, because nothing in these pages measures one.

Nothing found needed a change to the flex solver, which is the part that was under test and which
came out exact both times.

## What it found on the way

Running the comparison for the first time found four defects, none of which clicking through the
window had shown:

- **Every number on the 0.1.1 page was mojibake.** areole stores the pointer it is handed and never
  copies; the page was formatting into a stack buffer that the next `sprintf` overwrote. It was
  invisible until `--dump` printed the strings back as text.
- **The 0.4.0 page's `#demo` and `#rows`** carried no `display` rule, took the default flex-row,
  and laid four tiles out side by side off the edge of the page.
- Two mistakes in `tour.html` itself: `.app div` outscoring `.page` on specificity, and
  `flex: 0 0 120px` setting a basis on the vertical axis. Both were caught by the browser
  disagreeing, which is the comparison working in the direction nobody plans for.

## The two that differ, and why they are not a bug

Both are on the 0.5.0 page, and both are exactly one line of text — 19 pixels.

A paragraph beside a float is 568 wide in both engines, with the same float shortening the same
lines. areole fits it into three lines; Chromium needs four. Nothing about the box model differs:
the two rasterizers measure the same words about a pixel apart, and across a paragraph that is
enough to move where one line ends, which moves a whole line into or out of existence.

It is the same divergence the *text-sized* table below reports, one level removed — a container
whose height comes from wrapped descendants inherits their disagreement. The tool does not fold it
into that category, because a container whose height is wrong for a real reason looks exactly the
same, and hiding one to tidy the other would be the wrong trade.

## Reading the output

Three kinds of line, deliberately kept apart:

- **GEOMETRY** — the same box in the same place got a different rectangle. This is the flex solver
  disagreeing, and is the verdict.
- **TEXT-SIZED** — boxes whose width comes from their own text. Reported as a spread, never as a
  failure: two rasterizers will not agree exactly and pretending otherwise would mean shipping
  someone else's rasterizer.
- **TEXT DIFFERS** — the strings themselves differ, so the boxes are not comparable. Almost always
  means `tour.html` has drifted from `main.c`. Three strings on the 0.1.0 and 0.1.1 pages are
  values the running program computes and are snapshots from one run; they will report as drift
  when the build changes, and that is the tool being honest rather than a failure.
