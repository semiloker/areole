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
    /* The document skeleton. `head` and everything in it draws nothing --
       which is what stops a stylesheet's own text appearing on the page. */
    "html { display:block; }"
    "body { display:block; margin:8px; }",

    "head, style, script, title { display:none; }"
    "meta, link, base { display:none; }",

    /* Block-level content, in fours. */
    "div, p, section, article { display:block; }"
    "aside, nav, header, footer { display:block; }",

    "main, figure, figcaption, blockquote { display:block; }"
    "pre, address, hgroup, dl { display:block; }",

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
    "h1 { display:block; font-size:32px; margin:21px 0px; }"
    "h2 { display:block; font-size:24px; margin:20px 0px; }"
    "h3 { display:block; font-size:19px; margin:18px 0px; }",

    "h4 { display:block; font-size:16px; margin:21px 0px; }"
    "h5 { display:block; font-size:13px; margin:22px 0px; }"
    "h6 { display:block; font-size:11px; margin:24px 0px; }",

    /* Lists. No markers: `list-style` and `::marker` are 0.5.3, so these
       indent and show nothing. */
    "ul, ol, menu { display:block; margin:16px 0px; padding-left:40px; }"
    "li { display:block; }",

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
    "thead, tbody, tfoot { display:table-row-group; }"
    "tr { display:table-row; }",

    "td { display:table-cell; padding:1px; }"
    "th { display:table-cell; padding:1px; text-align:center; }"
    "caption { display:table-caption; text-align:center; }"
    "colgroup, col { display:table-column; }",

    /* Rules. `hr` has an inset border in a browser and a flat one here,
       because per-side border widths are not implemented. */
    "hr { display:block; margin:8px 0px; border-width:1px;"
    "     border-color:#808080; }",

    /* Form controls, which have no appearance of their own until 0.10.1. They
       are given a display so they are not flex boxes, and nothing else. */
    "input, button, select, textarea { display:inline-block; }"
    "output, progress, meter { display:inline-block; }",

    0};

void ar_ua_stylesheet(ar_ctx *c)
{
    ar_i32 i;

    for (i = 0; AR__UA[i]; ++i)
    {
        ar_stylesheet(c, AR__UA[i]);
    }
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
