/*
 * areole - the presentational-hint corpus.
 * SPDX-License-Identifier: MIT
 *
 * What HTML's legacy attributes compute to, checked against a browser rather
 * than against my reading of the rendering section.
 *
 *     ar_hints                              areole's computed values
 *     ar_hints --html > tests/hints.html    the browser twin
 *     python tools/compare_computed.py --run ./build/ar_hints.exe \
 *            tests/hints.html
 *
 * 0.9.1's second acceptance criterion. The machinery is in ar_computed.h,
 * which says why this is a corpus of computed values and not of rectangles;
 * what is here is the cases.
 */
#include "ar_computed.h"

/*
 * The corpus.
 *
 * Grouped by what each group is for rather than by attribute, because the
 * groups fail for different reasons: a mapping group fails when an attribute
 * means the wrong thing, an ordering group when it means the right thing in
 * the wrong band of the cascade, and a parsing group when HTML's own rules for
 * reading a value are not HTML's.
 */
static const ar__case CASES[] = {
    /* -- the mapping: an attribute and the declaration it becomes -------- */
    /*
     * `<body bgcolor>` and `<body text>` are not here, and the reason is the
     * corpus rather than the mapping: these forty cases share one page, and a
     * document has one body. Rewritten as a div in the twin -- which is what
     * the first version did -- a browser correctly ignores both attributes,
     * because they belong to body alone, and the case then reports a
     * disagreement about the twin rather than about areole. Both are checked
     * in tests/ar_test.c instead, where every scene is its own document.
     */
    {"bgcolor-table", "", "<table id=\"x\" bgcolor=\"red\"><tr><td>t</td></tr></table>",
     "background-color"},
    {"bgcolor-tr", "", "<table><tr id=\"x\" bgcolor=\"red\"><td>t</td></tr></table>",
     "background-color"},
    {"bgcolor-td", "", "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>",
     "background-color"},
    {"bgcolor-th", "", "<table><tr><th id=\"x\" bgcolor=\"red\">t</th></tr></table>",
     "background-color"},
    {"font-color", "", "<font id=\"x\" color=\"green\">t</font>", "color"},

    {"width-img", "", "<img id=\"x\" width=\"200\">", "width"},
    {"height-img", "", "<img id=\"x\" height=\"90\">", "height"},
    {"width-table", "", "<table id=\"x\" width=\"300\"><tr><td>t</td></tr></table>", "width"},
    {"width-td", "", "<table><tr><td id=\"x\" width=\"120\">t</td></tr></table>", "width"},
    {"width-col", "",
     "<table><colgroup><col id=\"x\" width=\"70\"></colgroup><tr><td>t</td></tr></table>", "width"},
    {"width-hr", "", "<hr id=\"x\" width=\"150\">", "width"},

    {"cellspacing", "", "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>",
     "border-spacing"},
    {"cellpadding-lands-on-the-cell", "",
     "<table cellpadding=\"8\"><tr><td id=\"x\">t</td></tr></table>", "padding-top"},
    {"border-table", "", "<table id=\"x\" border=\"3\"><tr><td>t</td></tr></table>",
     "border-top-width"},
    {"border-gives-cells-one-pixel", "", "<table border=\"3\"><tr><td id=\"x\">t</td></tr></table>",
     "border-top-width"},
    {"border-zero-is-no-border", "", "<table border=\"0\"><tr><td id=\"x\">t</td></tr></table>",
     "border-top-width"},
    {"hspace-img", "", "<img id=\"x\" hspace=\"12\">", "margin-left"},
    {"vspace-img", "", "<img id=\"x\" vspace=\"6\">", "margin-top"},

    {"align-left", "", "<p id=\"x\" align=\"left\">t</p>", "text-align"},
    {"align-right", "", "<p id=\"x\" align=\"right\">t</p>", "text-align"},
    {"align-center", "", "<p id=\"x\" align=\"center\">t</p>", "text-align"},
    {"align-middle-is-center", "", "<p id=\"x\" align=\"middle\">t</p>", "text-align"},
    /* A known divergence rather than a bug: this engine has three text-align
       values and `justify` is not one of them, so the attribute maps to
       nothing and the property stays `left`. Kept in the corpus so the day it
       arrives, the case is already here. */
    {"align-justify", "", "<p id=\"x\" align=\"justify\">t</p>", "text-align"},
    {"align-td", "", "<table><tr><td id=\"x\" align=\"right\">t</td></tr></table>", "text-align"},
    {"align-nonsense-is-nothing", "", "<p id=\"x\" align=\"sideways\">t</p>", "text-align"},

    /* -- the cascade: which band each thing sits in ---------------------- */
    {"hint-beats-the-user-agent", "",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"author-beats-hint", "table { border-spacing:4px; }",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"author-beats-hint-on-the-cell", "td { padding:3px; }",
     "<table cellpadding=\"8\"><tr><td id=\"x\">t</td></tr></table>", "padding-top"},
    {"a-sheet-that-says-nothing-leaves-the-hint", "p { color:#112233; }",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"style-beats-hint", "", "<img id=\"x\" width=\"200\" style=\"width:50px\">", "width"},
    {"style-leaves-the-other-attribute", "",
     "<img id=\"x\" width=\"200\" height=\"90\" style=\"width:50px\">", "height"},
    {"important-beats-style", "p { color:#00ff00 !important; }",
     "<p id=\"x\" style=\"color:#ff0000\">t</p>", "color"},
    {"important-style-beats-important", "p { color:#00ff00 !important; }",
     "<p id=\"x\" style=\"color:#ff0000 !important\">t</p>", "color"},
    {"style-beats-the-most-specific-selector", "p#x.a.b { width:20px; }",
     "<p id=\"x\" class=\"a b\" style=\"width:30px\">t</p>", "width"},

    /* -- HTML's own value parsing, which is not CSS's -------------------- */
    {"colour-keyword", "", "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>",
     "background-color"},
    {"colour-hash", "", "<table><tr><td id=\"x\" bgcolor=\"#ff0000\">t</td></tr></table>",
     "background-color"},
    {"colour-bare-hex", "", "<table><tr><td id=\"x\" bgcolor=\"ff0000\">t</td></tr></table>",
     "background-color"},
    {"colour-short-hash", "", "<table><tr><td id=\"x\" bgcolor=\"#f00\">t</td></tr></table>",
     "background-color"},
    {"length-pixels", "", "<img id=\"x\" width=\"200\">", "width"},
    {"length-percent", "", "<div style=\"width:400px\"><img id=\"x\" width=\"50%\"></div>",
     "width"},
    {"length-auto-is-not-a-length", "", "<img id=\"x\" width=\"auto\" height=\"77\">", "height"},
    {"length-trailing-junk", "", "<img id=\"x\" width=\"200abc\">", "width"},
    {"length-empty", "", "<img id=\"x\" width=\"\" height=\"55\">", "height"}};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "presentational hints", "ar_hints");
}
