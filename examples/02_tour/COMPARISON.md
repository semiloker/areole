# areole against a browser, box by box

`tour.html` declares the same six pages as `main.c`, with the same stylesheet, so both engines can
be asked to lay out the same thing and the answers compared. Run it:

```sh
python tools/compare_layout.py --run ./build/example_tour.exe
```

It drives both sides — `--dump` for areole, a headless Chromium for the other — and matches boxes
on their path through the tree, which is the one identifier the two genuinely share.

## The result

Measured 2026-08-13, Edge 151 headless, Segoe UI, 640×480 viewport, 153 boxes across six pages.

| | |
| --- | --- |
| Boxes matched by path | **153 / 153** |
| Geometry disagreements, boxes not sized by their own text | **0** |
| Text-sized boxes, width difference | **max 1 px, mean 0.8 px** |
| Text-sized boxes, x offset | **max 1 px, mean 0.4 px** |

Every box that flex places lands on the same rectangle in both engines, exactly. Boxes whose width
comes from measuring their own text land within one pixel — which is as close as two independent
rasterizers of the same TrueType outlines get, and closer than expected.

**This is not a statement that areole is a browser.** It is a statement that for the subset both
engines implement, they agree. The subset is small, and the six differences below are the shape
of what is missing.

## The six differences, and which are bugs

Getting to zero needed six compensating rules in `tour.html`. Each is a real difference in
behaviour, and each is written up beside the rule that compensates for it. Three are limitations
with a release attached; three are deliberate.

| | Difference | Verdict |
| --- | --- | --- |
| 1 | **Default display** is flex-row, not block | Deliberate. There is no block layout to default to; it arrives in 0.5.0 |
| 2 | **Line boxes are quantised**: text height is `8 × floor(font-size / 8)`, so 13 px and 15 px text are both 8 px tall | **A limitation.** Font sizes that are not multiples of eight lay out as the multiple below. It follows from the bitmap face's 8-pixel cell and outlives the reason for it |
| 3 | **No wrapping.** `ar_text_wrap` implements UAX #14 and is tested, and nothing in the layout solver calls it | **The largest gap this comparison found.** A box is one line however narrow it is, and long text overflows. 0.5.0 |
| 4 | **No automatic minimum size.** CSS refuses to shrink a flex item below its content and overflows the container; areole clamps to the parent and overflows the content | Deliberate, and arguably the better behaviour for a fixed viewport |
| 5 | **No shrinking.** A CSS flex item shrinks when the line is over-full; areole never does | A limitation, and a quiet one — on the 0.3.0 page the browser compressed a 30 px heading to 19 px to fit, and areole simply ran off the bottom |
| 6 | **Borders are paint.** areole draws a border inside the box and never counts it in a measurement | Deliberate, and it should be said out loud: a `border` in an areole stylesheet does not behave like a CSS border |

Numbers 2, 3 and 5 are on the 0.5.0 list. Nothing here needs a change to the flex solver, which is
the part that was under test and which came out exact.

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
