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
     */
    "html { display:block; background:#ffffff; font-size:16px; }"
    "body { display:block; margin:8px; }",

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
     * reason a list of ten reads as a column instead of a ragged edge.
     */
    "ar-marker { display:inline-block; width:1.6em; margin-left:-2em; }"
    "ar-marker { margin-right:0.4em; text-align:right; }",

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

    /* Form controls, which have no appearance of their own until 0.10.1. They
       are given a display so they are not flex boxes, and nothing else. */
    "input, button, select, textarea { display:inline-block; }"
    "progress, meter { display:inline-block; }"
    /* `output` is inline in every browser and was inline-block here only
       because it shared a rule with two elements that are. */
    "output { display:inline; }",

    /*
     * What a form control looks like is 0.10.1. What it *measures* is not,
     * and a control two pixels narrower than a browser's puts every following
     * word in the wrong place -- so the sizes and the padding are here and
     * the appearance is not.
     *
     * 13px is the 13.33 a browser computes from its own font shorthand,
     * rounded; `fieldset` is 12 of padding a side rather than the 10 that was
     * guessed at.
     */
    /* Two rules, because AR_MAX_SEL_LIST is four and a list of six is
       refused whole rather than truncated -- which is the trap this
       file's own header warns about, and which caught the first
       version of this rule and every rule after it in the same part. */
    "input, button, select, textarea { font-size:13px; }"
    "optgroup, option { font-size:13px; }"
    "input, textarea, option { padding-left:2px; padding-right:2px; }"
    "button { padding-left:6px; padding-right:6px; text-align:center; }"

    /*
     * ------------------------------------------------------------------
     * Controls that look like controls, 0.10.0
     * ------------------------------------------------------------------
     *
     * Every one of these is boxes and borders rather than a bitmap, which is
     * the whole argument for building controls this way: an author can restyle
     * a checkbox because a checkbox is a box.
     *
     * The colours are the system ones 0.4.4 shipped, so a control follows the
     * desktop theme and `color-scheme: dark` repaints the lot -- which is what
     * those nineteen names are for and the first thing in this engine to use
     * them for their actual purpose.
     *
     * `.ar-checkbox` and `.ar-radio` are synthetic classes the document walk
     * adds, because there are no attribute selectors here yet and
     * `input[type=checkbox]` is how this rule is written everywhere else.
     */
    "button, input { background:ButtonFace; color:ButtonText; }"
    "button, input { border:1px solid ButtonBorder; }"
    "input { background:Field; color:FieldText; }"
    "button:disabled, input:disabled { color:GrayText; }",

    /*
     * `select` and `textarea` are controls too, and were boxes with a display
     * and nothing else -- a dropdown drew as bare text with every option
     * beside it, and a textarea drew as bare text with no box at all.
     *
     * A text field's width is the other half of that. `<input type="text">`
     * has always had a default size of twenty characters, and with no rule
     * saying so it shrank to fit its value: an empty field was invisible and
     * a field with two words in it was two words wide. 11em is that twenty
     * characters at the 13px these controls use, and it scales with the text
     * the way a `size` attribute is supposed to.
     */
    "select, textarea { background:Field; color:FieldText;"
    "                   border:1px solid ButtonBorder; }"
    "select:disabled, textarea:disabled { color:GrayText; }",

    /* A checkbox and a radio are `input` too and must not take this width --
       but they say so themselves below, and a class beats a type selector, so
       no rule is needed here to keep them square. */
    "input, select { width:11em; height:1.6em; }"
    "textarea { width:11em; height:3.4em; }"
    ".ar-button { width:auto; padding-left:6px; padding-right:6px; }",

    /*
     * A dropdown shows the option it is on and not the whole list.
     *
     * `option` is `display:block` above, which is right for a list box and
     * wrong for the closed dropdown every `<select>` starts as. There are no
     * attribute selectors here, so the document walk marks the one to show
     * with a synthetic class, exactly as it does for a checkbox.
     */
    "select > option { display:none; }"
    "select > option.ar-chosen { display:block; }",

    /* A square that is a square whatever the font is: a control sized in `em`
       grows with the text around it and stops being a checkbox. */
    ".ar-checkbox, .ar-radio { width:13px; height:13px; }"
    ".ar-checkbox, .ar-radio { padding-left:0; padding-right:0; }"
    ".ar-radio { border-radius:7px; }",

    /* The mark is a child box and is built whether or not it is shown, so that
       turning it on costs no layout -- the box is already the right size and in
       the right place, and only its background changes. */
    "ar-mark { display:block; width:7px; height:7px; }"
    "ar-mark { margin-left:2px; margin-top:2px; background:transparent; }"
    "ar-mark:checked { background:AccentColor; }"
    ".ar-radio > ar-mark { border-radius:4px; }",

    /*
     * The focus ring, and `:focus-visible` rather than `:focus` on purpose: a
     * click must not draw one and a Tab must. Drawing a ring for both is why
     * so many pages ship `outline: none`.
     *
     * An outline rather than a border, because a border takes space and a ring
     * drawn with one shifts the page every time the focus moves. Avoiding that
     * is what the property is for.
     */
    "button:focus-visible, input:focus-visible { outline:2px solid AccentColor; }"
    "summary:focus-visible, a:focus-visible { outline:2px solid AccentColor; }",

    /*
     * A gauge is a track with a bar in it, and both are boxes.
     *
     * `<progress>` with no `value` is indeterminate rather than empty -- a bar
     * that is waiting, not one at zero. There is no animation here to say so,
     * so it reads as empty, which is written down in ar_dom.c rather than
     * pretended about.
     */
    "progress, meter { display:inline-block; width:160px; height:12px; }"
    "progress, meter { background:Field; border:1px solid ButtonBorder; }"
    "ar-bar { display:block; height:12px; background:AccentColor; }"
    "meter > ar-bar { background:Highlight; }",

    /* A field's text is a box, so it is measured, laid out and painted by the
       machinery that already does all three. Clipped, because a field does not
       grow to fit what is typed into it -- which is the one thing everybody
       knows about text fields and the first thing a naive one gets wrong. */
    /*
     * The field's text has no `white-space` rule, because there is no
     * `white-space` property in this engine at all -- `nowrap` here is
     * `flex-wrap`, which is a different question with the same word.
     *
     * So a long line in a narrow field wraps, where every real field scrolls.
     * Stated rather than papered over with a rule that parses and does
     * nothing: the property belongs with the text work, and the field will
     * want horizontal scrolling of its own besides.
     */
    "input { overflow:hidden; }"
    "ar-value { display:block; }",

    /* The tag still decides by default; an author can now say otherwise,
       which is what moving the decision out of the walk bought. */
    "pre, textarea { white-space:pre; }"
    "ar-value { white-space:pre; }",

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
    "fieldset { margin:0px 2px; padding:6px 12px 10px 12px;"
    "           border-width:2px; border-color:#c0c0c0; }"
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
