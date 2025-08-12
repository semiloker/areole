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
 * What is here, and what 0.9.1 adds
 *
 * Around fifty elements: the ones a document is made of. The release document
 * gives 0.9.1 the complete set of about 120, built from the HTML
 * specification's own rendering section element by element, plus the
 * presentational-hint mapping for legacy attributes and the quirks-mode
 * differences.
 *
 * Three things are deliberately absent and each needs something that does not
 * exist yet:
 *
 *   - **Font sizes in `em`.** `h1 { font-size: 2em }` needs relative units,
 *     which are 0.4.1. The headings carry pixel sizes chosen to match a
 *     browser at a 16px root, and they will be wrong at any other root until
 *     `em` arrives.
 *   - **Margins in `em`**, for the same reason.
 *   - **`list-style`, `::marker` and counters**, which are 0.5.3. A `<ul>`
 *     here indents and shows no bullets.
 *
 * And one ceiling worth stating: **AR_MAX_RULES is 256.** This sheet is around
 * fifty rules, so it fits beside an author stylesheet with room to spare. The
 * complete 0.9.1 sheet is about four hundred and does not, which is why the
 * rule table has to become a function of the arena before that release.
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
     * Headings.
     *
     * The sizes are pixels at a 16px root rather than the specification's
     * `em`, because relative units are 0.4.1. They match a browser exactly at
     * that root and are wrong at any other, which is the honest state of it.
     */
    "h1 { display:block; font-size:32px; margin:21px 0px; font-weight:bold; }"
    "h2 { display:block; font-size:24px; margin:20px 0px; font-weight:bold; }"
    "h3 { display:block; font-size:19px; margin:18px 0px; font-weight:bold; }",

    /*
     * The four elements whose whole purpose is a relative size, in pixels at
     * a 16px root for the same reason the headings are: `big` is 1.2em and
     * the other three are 0.8125em, which come to 19.2 and 13.33. Rounded,
     * because there are no fractions here -- inside the pixel the corpus
     * allows, and wrong at any other root.
     */
    "big { font-size:19px; }"
    "small, sub, sup { font-size:13px; }",

    "h4 { display:block; font-size:16px; margin:21px 0px; font-weight:bold; }"
    "h5 { display:block; font-size:13px; margin:22px 0px; font-weight:bold; }"
    "h6 { display:block; font-size:11px; margin:24px 0px; font-weight:bold; }",

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

    "iframe, embed, object, picture { display:inline; }",

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
    "code, kbd, samp, tt { font-size:13px; }",

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
     *   - `sub` and `sup` want `vertical-align: sub / super` and a smaller
     *     size. `vertical-align` has four keywords and none of them is these.
     *   - `a:link` wants a colour and an underline. `text-decoration` does not
     *     exist.
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
