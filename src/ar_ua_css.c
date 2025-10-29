/*
 * areole - the user-agent stylesheet.
 * SPDX-License-Identifier: MIT
 *
 * The default style for every element, which is what makes `<h1>` large,
 * `<ul>` indented and `<table>` use the table model. Roughly the equivalent of
 * a browser's html.css, embedded as a string and parsed once.
 *
 * ------------------------------------------------------------------------
 * Why a document needs this before it needs anything else
 *
 * areole's own default display is `flex`, because areole began as a UI library
 * where that is the useful default. HTML's is `inline`, and for most elements
 * that matter it is `block`. Without a sheet saying so, a parsed document lays
 * every paragraph out in a row.
 *
 * So this is not decoration on top of the parser. It is the difference between
 * a document that renders and one that does not.
 *
 * ------------------------------------------------------------------------
 * What is here
 *
 * The complete set, built from the HTML specification's own rendering section
 * element by element, and checked against a browser rather than against a
 * memory of one: tests/ar_units.c and the three corpora 0.9.1 added compare
 * every computed value here with Edge's, and 0.9.1 found nine defaults wrong
 * that way -- the largest being that a document read at eight pixels where
 * every browser reads at sixteen.
 *
 * Font sizes and margins are in `em` as of 0.4.1, which is what the
 * specification states and what makes a heading scale when a page sets a root
 * size. One line did not come along -- h6's margin -- and the comment beside
 * the headings has the pixel that explains it.
 *
 * One thing is still deliberately absent:
 *
 *   - **`list-style`, `::marker` and counters**, which are 0.5.3. A `<ul>`
 *     here indents and shows no bullets.
 */
#include "ar_html.h"
#include "ar_node.h"

/*
 * Split into several strings because C89 guarantees only 509 characters in one
 * literal after concatenation, and the gate enforces it. The parser takes as
 * many sheets as it is given, so this is a list rather than one blob.
 */
/*
 * ------------------------------------------------------------------------
 * Four selectors to a rule, and not one more
 *
 * AR_MAX_SEL_LIST is 4, and a list longer than that is **refused whole** --
 * not truncated. The first draft of this file had `div, p, section, article,
 * ...` at eighteen and `span, a, b, i, ...` at twenty-four, and both rules
 * were discarded silently: paragraphs stayed flex items and laid out in a row,
 * and everything in `<head>` drew on the page.
 *
 * It looked exactly like a sheet that was working. What found it was the check
 * that every part parses with no errors, which is worth having for precisely
 * this reason -- a stylesheet that fails does not crash, it just quietly
 * stops.
 *
 * So the lists are chopped into fours. The complete 0.9.1 sheet has to live
 * within the same bound, or raise it.
 * ------------------------------------------------------------------------
 */
static const char *const AR__UA[] = {
    /*
     * The document skeleton. `head` and everything in it draws nothing --
     * which is what stops a stylesheet's own text appearing on the page.
     *
     * `html` is painted white, and that is not what CSS says: the initial
     * background is `transparent`, and what makes a page white is the browser
     * painting its canvas before anything else. areole has no canvas -- it
     * draws into whatever surface it is handed -- so a document that declares
     * no background of its own came out on top of last frame's pixels, which
     * for the corpus harness meant black. Every example in the tree declares a
     * background and none of them showed it; ten real documents, which mostly
     * do not, showed it immediately.
     *
     * Put here rather than in the examples because it is the rule a document
     * is written against: an author who sets no background expects white and
     * every engine gives them white. An application that wants otherwise
     * overrides `html`, which now works because there is something to
     * override.
     */
    /*
     * Sixteen pixels, which is every browser's default and the number this
     * whole sheet was written against.
     *
     * areole's own default is eight -- one face height, meaning scale 1 --
     * because areole began as a UI library where that is the useful size. A
     * document is not a UI: `h1 { font-size:32px }` here was chosen to be
     * twice a browser's root, and against a root of eight it was four times
     * the body text instead of twice. Every heading on every page was too big
     * by a factor of two relative to the words under it, and nothing said so
     * because both numbers were internally consistent.
     *
     * In the user-agent sheet rather than in ar_style_defaults, so it reaches
     * documents and leaves interfaces alone: an ar_begin tree that never asks
     * for this sheet still gets eight.
     *
     * No background on `html`. A browser's sheet has none either: the white
     * behind a page is the canvas's own colour, `Canvas`, and the painter
     * gives it to a document whose root and body name nothing. A white root
     * here meant the root always had a background, so a body's never
     * reached the canvas, and `color-scheme: dark` put a dark page on a
     * white sheet.
     */
    "html { display:block; font-size:16px; }"
    "body { display:block; margin:8px; }"
    /*
     * CSS's initial `display`, for every element nothing below names. areole's
     * own initial value is `flex`, which is right for an interface and was
     * what every unknown element in a document got: a custom element -- MDN's
     * `<mdn-dropdown>` -- laid its button and its menu out side by side, the
     * button stretched down the menu's whole height. A universal selector is
     * the least specific there is, so every rule below still wins.
     */
    "* { display:inline; }",

    "head, style, script, title { display:none; }"
    "meta, link, base { display:none; }",

    /* Block-level content, in fours. */
    "div, p, section, article { display:block; }"
    "aside, nav, header, footer { display:block; }",

    "main, figure, figcaption, blockquote { display:block; }"
    "pre, address, hgroup, dl { display:block; }"
    /* `hgroup` groups a heading with its subtitle and adds nothing of its
       own -- it was taking `pre`'s margins because it shares that rule's
       selector list and nothing said otherwise. */
    "hgroup { margin:0px; }", /* and nothing later takes it back */

    "dd, dt, form, fieldset { display:block; }",

    "p { margin:16px 0px; }"
    "blockquote, figure { margin:16px 40px; }"
    "dd { margin-left:40px; }",

    /*
     * Headings, in the `em` the specification actually states.
     *
     * They were pixels at a 16px root until 0.4.1, matching a browser exactly
     * there and wrong at every other root. Now `html { font-size: 20px }`
     * scales them, which is what a heading is for.
     *
     * **h6's margin is the one that could not come along, and the number is
     * worth keeping.** Every heading rounds its font size to a whole pixel --
     * h6 is 0.67em of 16, so 10.72 becomes 11 -- and a margin stated in `em`
     * then multiplies that rounding. At 2.33em the 0.28 of a pixel becomes
     * 1.02, which is past the one-pixel criterion the corpus is scored on:
     * 26px against a browser's 24.9776. h1 through h5 all land inside it, and
     * h3's margin gets *closer* than the pixel value it replaced.
     *
     * So this is not a sheet half-converted out of caution. It is converted
     * exactly as far as integer font sizes allow, and the one line that stays
     * behind is the same fractional residual the table corpus and the
     * gallery's text demos are down to. A sub-pixel used-value stage is what
     * finishes it, and that is not this release.
     */
    "h1 { display:block; font-size:2em; margin:0.67em 0px; font-weight:bold; }"
    "h2 { display:block; font-size:1.5em; margin:0.83em 0px; font-weight:bold; }"
    "h3 { display:block; font-size:1.17em; margin:1em 0px; font-weight:bold; }",

    /*
     * The four elements whose whole purpose is a relative size, and which can
     * now say so. One multiplication each and no margin to compound it, so
     * both land inside the pixel the corpus allows at any root rather than
     * only at sixteen.
     */
    "big { font-size:1.2em; }"
    "small, sub, sup { font-size:0.8125em; }"
    /* And where the two of them sit. `vertical-align` had four keywords
       and neither of these was one, so a subscript and a superscript were
       small text on the same baseline as everything else -- which is the
       one thing they are not. */
    "sup { vertical-align:super; }"
    "sub { vertical-align:sub; }",

    "h4 { display:block; font-size:1em; margin:1.33em 0px; font-weight:bold; }"
    "h5 { display:block; font-size:0.83em; margin:1.67em 0px; font-weight:bold; }"
    "h6 { display:block; font-size:0.67em; margin:24px 0px; font-weight:bold; }"
    /* The 24px above is h6's, and the comment beside the headings says why. */,

    /*
     * Bold and italic, which the sheet could not say until 0.9.1.
     *
     * These are the rules that make a document read as a document: a heading
     * that is heavier than its paragraph, a `<strong>` that is stronger, an
     * `<em>` that is emphasised. Every one of them was a plain roman before,
     * and ten real pages made that the most obvious thing wrong with them.
     *
     * Whether the difference is *drawn* depends on the application having
     * given areole a face for the weight and style -- `ar_font_load_styled`.
     * With one face loaded the cascade is still correct and the text still
     * comes out roman, which is what a browser does with a family that has no
     * bold: the rule applies, the face is the closest available.
     */
    "b, strong, th { font-weight:bold; }"
    "i, em, cite, var { font-style:italic; }",

    /* `dfn` is here for its slant and was nowhere for its display, so it
       fell to the initial one and a defined term was a flex container in the
       middle of a sentence. `legend` is not bold in any browser; `optgroup`
       is. */
    "dfn, address { font-style:italic; }"
    "dfn { display:inline; }"
    "optgroup { font-weight:bold; }",

    /* Lists. No markers: `list-style` and `::marker` are 0.5.3, so these
       indent and show nothing. */
    "ul, ol, menu { display:block; margin:16px 0px; padding-left:40px; }"
    "li { display:list-item; }",

    /*
     * The markers, placed by a negative margin rather than by a positioning
     * scheme.
     *
     * That is what `list-style-position: outside` is: the marker begins in the
     * padding the list has already reserved and the content begins at the
     * content edge, so a line that wraps lines up under the text and not under
     * the bullet. An inline-block with `margin-left` pulled back by its own
     * width and its gap says exactly that, and costs no new layout path.
     *
     * `ar-marker` is a fixed-width slot so that "9." and "10." end at the same
     * place -- numbers are right-aligned against the text, which is the whole
     * reason a list of ten reads as a column instead of a ragged edge. The gap
     * is a space's width, a quarter of an em in Times: a browser's marker is
     * "1. " ending at the content edge, so its period stops one space short.
     */
    "ar-marker { display:inline-block; width:1.75em; margin-left:-2em; }"
    "ar-marker { margin-right:0.25em; text-align:right; }",

    /*
     * A bullet is a box with a radius, because the built-in face has no U+2022
     * -- it is ASCII 32 to 126 and everything else comes out as `?`. Drawing
     * it means it is the same shape whatever face is loaded, and it inherits
     * its colour from the item like a glyph would.
     */
    "ar-bullet { display:inline-block; width:0.4em; height:0.4em; border-radius:0.4em; }"
    "ar-bullet { margin-left:-1.05em; margin-right:0.65em; background:currentColor; }"
    "ar-bullet.ar-circle { background:transparent; border:1px solid currentColor; }"
    "ar-bullet.ar-square { border-radius:0px; }",

    /* Inline content, in fours. */
    "span, a, b, i { display:inline; }"
    "em, strong, small, s { display:inline; }",

    "u, code, kbd, samp { display:inline; }"
    "var, sub, sup, abbr { display:inline; }",

    "cite, q, mark, time { display:inline; }"
    "label, br, wbr, img { display:inline; }",

    /* The table model, which areole has had since 0.7.0 and which is the whole
       reason a document's tables lay out at all. */
    "table { display:table; border-spacing:2px; }"
    /* Three groups, not one. The header and footer groups have displays of
       their own, and a table that puts its `<tfoot>` first still draws it
       last -- which is the only reason the distinction exists. */
    "tbody { display:table-row-group; }"
    "thead { display:table-header-group; }"
    "tfoot { display:table-footer-group; }"
    "tr { display:table-row; }",

    "td { display:table-cell; padding:1px; }"
    "th { display:table-cell; padding:1px; text-align:center; }"
    "caption { display:table-caption; text-align:center; }"
    /* Two displays, not one. A `colgroup` written as `table-column` is a
       column itself rather than a box holding columns, so it takes a slot
       of its own and every `col` inside it describes the wrong column --
       which made `<col width=70>` do nothing at all, silently, because a
       column that describes nothing has no width to be wrong about. */
    "colgroup { display:table-column-group; }"
    "col { display:table-column; }",

    /* Rules. `hr` has an inset border in a browser and a flat one here,
       because per-side border widths are not implemented. */
    "hr { display:block; margin:8px 0px; border-width:1px;"
    "     border-color:#808080; }",

    /*
     * ------------------------------------------------------------------
     * Form controls, measured against a browser
     * ------------------------------------------------------------------
     *
     * Every number below was read out of Edge on Windows with no stylesheet
     * at all -- getComputedStyle and getBoundingClientRect, and pixels sampled
     * from its screenshot -- by tools/versus.py, which renders one page in
     * both engines and shows the two side by side. The first version of these
     * rules was written from memory and looked like a different toolkit.
     *
     * Two things about how the numbers are spelled here:
     *
     *   - areole reserves no space for a border (see CSS_REFERENCE), so a
     *     browser's `padding: 1px 2px; border: 2px` becomes `padding: 3px 4px;
     *     border: 1px` -- the same box, the same place for the text, and the
     *     one-pixel line a browser actually draws over its two-pixel border.
     *   - A browser's controls are 13.333px Arial; areole has whole pixels,
     *     so they are 13px Arial, and a field's width is stated in pixels so
     *     it matches the browser's rather than following the smaller font.
     */
    "input, button, select, textarea { display:inline-block; }"
    "progress, meter { display:inline-block; }"
    /* `output` is inline in every browser and was inline-block here only
       because it shared a rule with two elements that are. */
    "output { display:inline; }",

    /* Two rules, because AR_MAX_SEL_LIST is four and a list of six is
       refused whole rather than truncated -- which is the trap this file's
       own header warns about. */
    "input, button, select, textarea { font-size:13px; }"
    "optgroup, option { font-size:13px; }"
    "input, button, select { font-family:sans-serif; }",

    /*
     * A text field: 169 pixels of content, which is twenty characters of
     * 13.333px Arial the way a browser counts them (29 + 7 per character, so
     * `size=` moves it the same way -- see ar__hints), a one-pixel #767676
     * border with a two-pixel radius, and its height from its text: 15 + 6 is
     * a browser's 21.
     */
    "input { width:169px; padding:3px 4px; border:1px solid #767676;"
    "        border-radius:2px; background:Field; color:FieldText; overflow:hidden; }"
    "input:disabled { color:#6D6D6D; background:#FAFAFA; border-color:#C8C8C8; }",

    /* A push button: the label plus six pixels a side, on #EFEFEF. */
    "button, .ar-button { width:auto; padding:3px 8px; border:1px solid #767676;"
    "                     border-radius:2px; background:#EFEFEF; color:#000000;"
    "                     text-align:center; }"
    "button:disabled, .ar-button:disabled { color:#6D6D6D; }",

    /*
     * A select is as wide as its widest option plus twenty-two pixels -- four
     * before the text, eighteen for the arrow -- and is 19 tall. Every option
     * is a box in the closed face so that width is counted, and every one but
     * the chosen one has no height and is not drawn: a browser sizes a
     * dropdown by everything it could show, and shows one.
     */
    "select { position:relative; padding:2px 15px 2px 3px; border:1px solid #767676;"
    "         border-radius:2px; background:Field; color:FieldText; }"
    "select > option, select > optgroup > option { display:block; height:0px;"
    "  padding:0px 2px; visibility:hidden; }",

    "select > option.ar-chosen, select > optgroup > option.ar-chosen"
    " { height:auto; visibility:visible; }"
    /* An option in a group is indented in the open list, and a browser counts
       the indent when it sizes the closed one. */
    "select > optgroup > option { padding-left:17px; }"
    "select > optgroup > option.ar-chosen { padding-left:2px; }"
    "select > optgroup { display:block; padding:0px; }"
    "select:disabled { color:#6D6D6D; border-color:#C8C8C8; }"
    /* Open, it is a stacking context above everything else, so the list it
       hangs over the page is painted over the page -- a positioned box with
       an automatic z-index paints its subtree as one piece, and the controls
       after it would otherwise draw over the list. */
    "select:open, .ar-color:open { z-index:10; }",

    /* The arrow, which is a chevron the painter draws -- a glyph in a browser,
       and the built-in face has none. */
    "ar-chev { display:block; position:absolute; right:5px; top:7px;"
    "          width:8px; height:5px; color:#000000; }",

    /*
     * A textarea: twenty columns of 13.333px Consolas and fifteen pixels a
     * browser keeps for a scrollbar, two rows of fifteen -- 162 by 30, which
     * `cols` and `rows` move by the same rule (ar__hints) -- with a resize
     * grip drawn in the corner.
     */
    "textarea { width:162px; height:30px; padding:3px; border:1px solid #767676;"
    "           border-radius:2px; background:Field; color:FieldText; overflow:auto;"
    "           position:relative; }"
    "textarea > ar-value { white-space:pre-wrap; }",

    "ar-grip { display:block; position:absolute; right:1px; bottom:1px;"
    "          width:7px; height:7px; color:#767676; }"
    "textarea:disabled { color:#6D6D6D; border-color:#C8C8C8; }"
    ".ar-hidden { display:none; }",

    /*
     * A checkbox: 13 by 13, margins 3 3 3 4, a #767676 square with a two-pixel
     * radius -- and checked, a #0075FF square with a white tick. The blue is
     * the browser's own and not the desktop accent, which is what Chrome and
     * Edge draw unless a page sets `accent-color`.
     */
    ".ar-checkbox { width:13px; height:13px; padding:0px; margin:3px 3px 3px 4px;"
    "               border:1px solid #767676; border-radius:2px; background:#FFFFFF; }"
    ".ar-checkbox:checked { background:#0075FF; border-color:#0075FF; }"
    "ar-tick { display:block; width:9px; height:9px; margin:2px 0px 0px 2px;"
    "          color:transparent; }",

    "ar-tick:checked { color:#FFFFFF; }"
    /* A radio: the same box, round, margins 3 3 0 5; checked, a blue ring and
       a seven-pixel blue dot three pixels in. */
    ".ar-radio { width:13px; height:13px; padding:0px; margin:3px 3px 0px 5px;"
    "            border:1px solid #767676; border-radius:7px; background:#FFFFFF; }"
    ".ar-radio:checked { border-color:#0075FF; }",

    "ar-mark { display:block; width:7px; height:7px; margin:3px 0px 0px 3px;"
    "          border-radius:4px; background:transparent; }"
    "ar-mark:checked { background:#0075FF; }",

    /*
     * The focus ring, and `:focus-visible` rather than `:focus` on purpose: a
     * click must not draw one and a Tab must. An outline rather than a border,
     * because a border takes space and a ring drawn with one shifts the page
     * every time the focus moves.
     */
    "button:focus-visible, input:focus-visible { outline:2px solid AccentColor; }"
    "summary:focus-visible, a:focus-visible { outline:2px solid AccentColor; }"
    "select:focus-visible, textarea:focus-visible { outline:2px solid AccentColor; }",

    /*
     * A slider, a progress bar and a meter are all an eight-pixel pill in a
     * sixteen-pixel box: an #EFEFEF track with a hairline edge, filled from
     * the left. A slider adds a sixteen-pixel thumb, placed by `left: N%` on a
     * rail one thumb narrower than the track; the document walk writes both
     * percentages as inline styles.
     */
    ".ar-range { width:129px; height:16px; padding:0px; margin:2px; border-width:0px;"
    "            background:transparent; overflow:visible; }"
    /* On `vertical-align: -0.2em`, as a browser sets them: three pixels below
       the baseline at the size these are drawn at. */
    "progress { width:160px; height:16px; vertical-align:-0.2em; }"
    "meter { width:80px; height:16px; vertical-align:-0.2em; }",

    "ar-track { display:block; height:8px; margin-top:4px; background:#EFEFEF;"
    "           border:1px solid #B2B2B2; border-radius:4px; }"
    "ar-fill { display:block; height:8px; background:#0075FF; border-radius:4px; }"
    "meter ar-fill { background:#107C10; }",

    "ar-rail { display:block; height:16px; margin-top:-12px; margin-right:16px; }"
    "ar-thumb { display:block; position:relative; width:16px; height:16px;"
    "           border-radius:8px; background:#0075FF; }",

    /* A colour field: a #EFEFEF button 50 by 27 with the colour in it, framed
       in #777777; and when it is open, sixteen chips hanging under it. */
    ".ar-color { width:40px; height:19px; padding:4px 5px; position:relative;"
    "            overflow:visible; border:1px solid #767676; border-radius:2px;"
    "            background:#EFEFEF; }"
    "ar-swatch { display:block; height:19px; border:1px solid #777777; }",

    "ar-palette { display:block; position:absolute; left:0px; top:100%; z-index:10;"
    "             width:88px; padding:2px; background:Field; border:1px solid #767676; }"
    "ar-chip { display:inline-block; width:16px; height:16px; margin:2px;"
    "          border:1px solid #767676; }"
    "ar-chip:checked { outline:2px solid #0075FF; }",

    /* A file field is 253 pixels: a button, and the name of what was chosen. */
    ".ar-file { width:253px; height:auto; padding:0px; border-width:0px;"
    "           background:transparent; overflow:hidden; white-space:nowrap; }"
    "ar-pick { display:inline-block; padding:3px 8px; margin-right:4px;"
    "          border:1px solid #767676; border-radius:2px; background:#EFEFEF;"
    "          color:#000000; }"
    "ar-name { display:inline; }",

    /* An open select's list hangs under it, over whatever follows, and is part
       of it -- so a press on a row lands inside the select. */
    "ar-listbox { display:block; position:absolute; left:0px; right:0px; top:100%;"
    "             z-index:10; background:Field; border:1px solid #767676; }"
    "ar-listbox > option { display:block; height:auto; padding:0px 4px; visibility:visible; }"
    "ar-listbox optgroup > option { display:block; height:auto; padding:0px 16px;"
    "  visibility:visible; }",

    "ar-listbox option.ar-chosen, ar-listbox option:hover"
    " { background:#1E90FF; color:#FFFFFF; }"
    "ar-group { display:block; font-weight:bold; padding-left:4px; }",

    /* A field's text is a box, so it is measured, laid out and painted by the
       machinery that already does all three. The tag still decides by default
       whether it keeps its spaces, and an author can say otherwise. */
    "ar-value { display:block; }"
    "pre, textarea { white-space:pre; }"
    "ar-value { white-space:pre; }"
    /* `::placeholder`, which is a class here as everything of its kind is:
       Edge's grey, read off its pixels. */
    ".ar-placeholder { color:#757575; }",

    /* The summary's triangle, a box the painter draws -- the built-in face is
       ASCII and U+25B8 would be a question mark. A browser's marker is the
       glyph and a space, seventeen pixels before the text at 16px. */
    "ar-tri-r, ar-tri-d { display:inline-block; width:8px; height:8px; }"
    "summary > ar-tri-r, summary > ar-tri-d { margin-right:9px; }",

    /* A control's label in the middle of the control when an author makes it
       taller than its text, as every browser draws it. The marks and tracks
       place themselves and opt back out. */
    "button, input, select { align-content:center; }"
    ".ar-checkbox, .ar-radio, .ar-range, .ar-color { align-content:start; }",

    "summary { display:block; }"

    /*
     * ------------------------------------------------------------------
     * The rest of the element set, 0.9.1
     * ------------------------------------------------------------------
     *
     * The sheet above was the elements a document usually has. This is the
     * elements a document may have, from the HTML specification's rendering
     * section, and the difference showed the moment ten real pages were
     * rendered: an element with no rule at all falls to the initial `display`,
     * which in this engine makes it a flex container. A `<del>` inside a
     * paragraph became a flex box in the middle of a line.
     *
     * That is the whole reason this is worth doing element by element rather
     * than waiting for a document to complain. There is no such thing as an
     * unstyled element here -- only one styled by accident.
     */

    /* Block-level, in fours. `summary` is `list-item` in the specification and
       block here, because markers are 0.5.3. */
    "details, dialog, search { display:block; }"
    "summary { display:list-item; }"
    "optgroup, option, legend, center { display:block; }",

    "xmp, listing, plaintext, marquee { display:block; }"
    "dir, frameset, noframes, fieldset { display:block; }",

    /* `dir` is a list and has been since before `ul` replaced it; `marquee`
       is inline-block; and a `dialog` that has not been opened is not
       displayed at all, which is the only state this engine can be in until
       there is something to open it. */
    "dir { margin:16px 0px; padding-left:40px; }"
    "marquee { display:inline-block; }"
    "dialog { display:none; padding:16px; margin:auto; }",

    /* Inline-level. `del` and `ins` are the pair that showed the flex-box
       default: both are inline in every browser and neither had a rule. */
    "del, ins, strike, big { display:inline; }"
    "tt, bdi, bdo, data { display:inline; }",

    /* A `slot` is transparent: it is not a box, its children are its
       parent's. `display:contents` is exactly that and areole has had it
       since 0.8.1. The three ruby elements have displays of their own that
       this engine does not, and are inline until it does. */
    "slot { display:contents; }"
    /* `ruby`, `rt` and `rp` have displays of their own that this engine does
       not have, and are inline until it does. The size is right even so: ruby
       text is half the size of the text it annotates. */
    "ruby, rt, rp { display:inline; }"
    "rt { font-size:8px; }"
    "map, canvas, video, audio { display:inline; }",

    "iframe, embed, object, picture { display:inline; }"
    /* `svg` had no rule at all, so it fell to the initial display and a
       drawing became a flex container: full width and no height. It is a
       replaced element in a browser, with a size of its own and a place on
       the line -- `inline-block` is the closest thing this engine has to
       that, and with the width and height attributes mapped beside it the
       box comes out where a browser puts it. */
    "svg { display:inline-block; }",

    /* Drawn by nobody: metadata, and elements whose content is not rendered. */
    "template, datalist, param { display:none; }"
    /*
     * Three of those four are not `none`, and a browser says so.
     *
     * `area` computes to `inline` -- it is rendered as part of an image map
     * rather than as a box, but the element itself is an ordinary inline one
     * and nothing in html.css hides it. `source` and `track` are the same:
     * inside a media element a browser builds no style for them at all, and
     * outside one they are inline like anything else.
     *
     * `noscript` is the interesting one. It is hidden only when scripting is
     * *enabled*, and there is no scripting here at all -- so its content is
     * the fallback that should be shown, which is what `inline` means. This
     * engine had it backwards and hid the one thing written for a reader
     * without a script engine.
     */
    "area, source, track, noscript { display:inline; }"
    "frame { display:none; }",

    /*
     * Margins the block rules above gave a display but no box.
     *
     * `dl` had `display:block` and no margin, so a definition list sat flush
     * against the paragraph before it. `pre` and its three legacy spellings
     * are the same shape.
     */
    /* `hgroup` is not in this rule. It groups a heading with its subtitle
       and contributes nothing of its own -- no margin, in any browser --
       and it was here only because it shares a selector list with `dl`
       further up the sheet. Adding `hgroup { margin:0 }` above this line
       did nothing, which is what a sheet resolved in source order does. */
    "dl { margin:16px 0px; }"
    /*
     * The monospace elements, at the size a browser gives them.
     *
     * Not a size rule anywhere else: it is `font-family: monospace`, and a
     * browser's `medium` is sixteen pixels for a proportional face and
     * thirteen for a monospace one. There are no font families here until
     * 0.2.1, so what is written is the consequence rather than the cause --
     * the same bargain the headings make with `em`, and wrong in the same way
     * if a page sets a size of its own on them.
     *
     * The margins follow from it: `pre` is `1em 0`, and the em is the
     * thirteen.
     */
    "pre, xmp, listing, plaintext { margin:13px 0px; font-size:13px; }"
    "code, kbd, samp, tt { font-size:13px; }"
    /* The family these four have always meant. Their whole purpose is that
       a run of characters lines up with the run above it, which no
       proportional face can do -- and it is why RFC 2616 was the one
       document in the real corpus that did not render recognisably. */
    "pre, code, kbd, samp { font-family:monospace; }"
    "tt, xmp, listing, textarea { font-family:monospace; }",

    /*
     * A nested list has no margin of its own.
     *
     * Four selectors exactly, which is AR_MAX_SEL_LIST -- a fifth would be
     * refused whole and silently, so this rule is at the limit by arithmetic
     * rather than by luck. `dir` is left out on purpose for that reason.
     */
    "ol ol, ol ul, ul ol, ul ul { margin:0px; }",

    /* `fieldset` and `legend`, which are the only elements whose default box
       is a border rather than a margin. The specification's border is
       `2px groove`; per-side widths and border styles are not implemented, so
       this is a flat two pixels and says so. */
    "fieldset { margin:0px 2px; padding:8px 14px 12px 14px;"
    "           border-width:2px; border-color:#9C9C9C; }"
    "legend { padding-left:2px; padding-right:2px; }",

    "center { text-align:center; }",

    /*
     * A link is blue, and that is the whole rule until `text-decoration`
     * exists.
     *
     * `.ar-link` rather than `a:link` because a link is an `<a>` with an
     * `href` and there are no attribute selectors here; the document walk
     * adds the class. LinkText is the system colour that means this, so a
     * dark colour scheme gets the lighter blue without a second rule.
     *
     * The underline is drawn on the text rather than as a border, which is
     * what lets a link that wraps get one line per fragment and none across
     * the gap between them. A border would take space in the line and box
     * each piece instead.
     */
    ".ar-link { color:LinkText; text-decoration:underline; }"
    "u, ins { text-decoration:underline; }"
    "s, del, strike { text-decoration:line-through; }",

    /*
     * Not expressible yet, and named here rather than left to be discovered:
     *
     *   - `dialog` is `display:none` until it has the `open` attribute, which
     *     needs an attribute selector. There are none, so a dialog is always
     *     drawn. The top layer and `::backdrop` have existed since 0.6.3 and
     *     are waiting for it.
     *   - `pre`, `xmp`, `listing`, `plaintext` and `textarea` need
     *     `white-space: pre` and a monospace family. Both are missing, which
     *     is why RFC 2616 is the one document in the real corpus that does not
     *     render recognisably.
     *   - `b`, `strong`, `th` and every heading want `font-weight: bold`;
     *     `i`, `em`, `cite`, `var`, `dfn` and `address` want
     *     `font-style: italic`. Neither property exists, so nothing on a page
     *     is bold or italic.
     *
     * Each is a missing property rather than a missing rule, which is why they
     * are listed together: the sheet is ahead of the engine, and the sheet is
     * the cheap half.
     */

    0};

void ar_ua_stylesheet(ar_ctx *c)
{
    ar_i32 i;

    /* Everything parsed inside these two calls is the user agent's, and a
       presentational hint goes in above it and below whatever the page says. */
    ar_sheet_begin_ua(&c->sheet);
    for (i = 0; AR__UA[i]; ++i)
    {
        ar_stylesheet(c, AR__UA[i]);
    }
    ar_sheet_mark_ua(&c->sheet);
}

ar_i32 ar_ua_stylesheet_parts(void)
{
    ar_i32 i = 0;

    while (AR__UA[i])
    {
        ++i;
    }
    return i;
}

const char *ar_ua_stylesheet_part(ar_i32 i)
{
    ar_i32 n = ar_ua_stylesheet_parts();

    return (i >= 0 && i < n) ? AR__UA[i] : 0;
}
