/*
 * areole - the quirks-mode corpus.
 * SPDX-License-Identifier: MIT
 *
 * What a document with no doctype computes to, checked against a browser
 * rather than against my reading of the compatibility notes.
 *
 *     ar_quirks                                 areole's computed values
 *     ar_quirks --html > tests/quirks.html      the browser twin
 *     python tools/compare_computed.py --run ./build/ar_quirks.exe \
 *            tests/quirks.html
 *
 * 0.9.1's third acceptance criterion. The machinery is in ar_computed.h; what
 * is here is the cases, and the one thing that makes this corpus different
 * from the other two: **its twin has no doctype**, because that is what puts a
 * document in quirks mode. A mode belongs to a document and the twin is one
 * document, so a corpus is all of one or all of the other.
 *
 * ------------------------------------------------------------------------
 * What quirks mode was measured to change
 *
 * Not what it is famous for changing. A browser was asked, with the same page
 * twice and a doctype on one of them, and the differences were:
 *
 *   - **a length may leave off its unit.** `width: 100` is a hundred pixels
 *     here and an invalid declaration in standards mode, where it is dropped
 *     and the width stays `auto`. areole accepted it in both, which is the
 *     wrong half of the two to be lenient in.
 *
 *   - **a table does not inherit its font.** `body { font-size:30px }` gives a
 *     `<td>` sixteen pixels in quirks and thirty in standards, which is what
 *     browsers did before CSS and still do for a page that asks for it.
 *
 * And, as loudly, what it does **not** change: the box model is the same.
 * `width:100px; padding:10px` occupies 120 in both -- the IE box model that
 * quirks mode is remembered for is not what a modern browser does, and
 * implementing it from memory would have made every padded box wrong on every
 * doctype-less page. That is the case for measuring rather than recalling, and
 * it is in the corpus below so it stays measured.
 */
#include "ar_computed.h"

static const ar__case CASES[] = {
    /* -- a length with no unit ------------------------------------------- */
    {"width-unitless", "#x { width:100; }", "<div id=\"x\">t</div>", "width"},
    {"height-unitless", "#x { height:40; }", "<div id=\"x\">t</div>", "height"},
    {"padding-unitless", "#x { padding:7; }", "<div id=\"x\">t</div>", "padding-left"},
    {"margin-unitless", "#x { margin:9; }", "<div id=\"x\">t</div>", "margin-top"},
    {"font-size-unitless", "#x { font-size:22; }", "<div id=\"x\">t</div>", "font-size"},
    {"border-spacing-unitless", "#x { border-spacing:6; }",
     "<table id=\"x\"><tr><td>t</td></tr></table>", "border-spacing"},

    /* Zero needs no unit anywhere and never did, so these two say the same
       thing in both modes and are here to prove the rule is about the number
       and not about the property. */
    {"zero-needs-no-unit", "#x { margin:0; padding:0; }", "<div id=\"x\">t</div>",
     "margin-top padding-left"},
    {"units-still-work", "#x { width:80px; margin:5px; }", "<div id=\"x\">t</div>",
     "width margin-top"},
    {"percent-still-works", "#x { width:50%; }", "<div id=\"x\">t</div>", "width"},

    /* A broken declaration costs itself and nothing beside it, in either
       mode -- the rule that decides whether a page with one typo in it loses
       one property or a whole rule. */
    {"one-bad-value-costs-one-declaration", "#x { width:100; height:33px; }",
     "<div id=\"x\">t</div>", "height"},

    /* -- a table's font -------------------------------------------------- */
    {"table-does-not-inherit-font", "#w { font-size:30px; }",
     "<div id=\"w\"><table><tr><td id=\"x\">t</td></tr></table></div>", "font-size"},
    {"table-itself-does-not-inherit", "#w { font-size:30px; }",
     "<div id=\"w\"><table id=\"x\"><tr><td>t</td></tr></table></div>", "font-size"},
    {"a-table-can-still-be-told", "#w { font-size:30px; } table { font-size:24px; }",
     "<div id=\"w\"><table><tr><td id=\"x\">t</td></tr></table></div>", "font-size"},
    {"a-cell-can-still-be-told", "#w { font-size:30px; } td { font-size:11px; }",
     "<div id=\"w\"><table><tr><td id=\"x\">t</td></tr></table></div>", "font-size"},
    {"not-a-table-still-inherits", "#w { font-size:30px; }",
     "<div id=\"w\"><p id=\"x\">t</p></div>", "font-size"},

    /* -- what quirks does not change ------------------------------------- */
    {"the-box-model-is-the-same", "#x { width:100px; padding:10px; }", "<div id=\"x\">t</div>",
     "width padding-left"},
    {"a-percentage-height-is-the-same", "#w { height:200px; } #x { height:50%; }",
     "<div id=\"w\"><div id=\"x\">t</div></div>", "height"},
    {"the-root-is-still-sixteen", "", "<p id=\"x\">t</p>", "font-size"},
    {"headings-are-still-headings", "", "<h1 id=\"x\">t</h1>", "font-size font-weight"},
    {"blocks-are-still-blocks", "", "<p id=\"x\">t</p>", "display margin-top"},
    {"lists-are-still-indented", "", "<ul id=\"x\"><li>t</li></ul>", "padding-left margin-top"},
    {"tables-are-still-tables", "", "<table id=\"x\"><tr><td>t</td></tr></table>",
     "display border-spacing"},
    {"cells-are-still-cells", "", "<table><tr><td id=\"x\">t</td></tr></table>",
     "display padding-left"},
    {"italics-are-still-italic", "", "<em id=\"x\">t</em>", "font-style display"},

    /* -- the other two bands still work in this mode --------------------- */
    {"a-hint-still-maps", "", "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>",
     "background-color"},
    {"a-hint-still-loses-to-the-author", "td { background:#0000ff; }",
     "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>", "background-color"},
    {"a-style-attribute-still-wins", "#x { width:20px; }",
     "<div id=\"x\" style=\"width:60px\">t</div>", "width"},
    {"important-still-beats-it", "#x { width:20px !important; }",
     "<div id=\"x\" style=\"width:60px\">t</div>", "width"},
    {"a-style-attribute-may-also-be-unitless", "", "<div id=\"x\" style=\"width:70\">t</div>",
     "width"},
    {"specificity-still-decides", "div { width:10px; } #x { width:30px; }", "<div id=\"x\">t</div>",
     "width"}};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "quirks mode", "ar_quirks", 1);
}
