/*
 * areole - the units corpus.
 * SPDX-License-Identifier: MIT
 *
 * What every CSS length unit computes to, checked against a browser rather
 * than against the conversion table in my head.
 *
 *     ar_units                                 areole's computed values
 *     ar_units --html > tests/units.html       the browser twin
 *     python tools/compare_computed.py --run ./build/ar_units.exe \
 *            tests/units.html
 *
 * 0.4.1's second acceptance criterion. The machinery is in ar_computed.h;
 * what is here is the cases.
 *
 * ------------------------------------------------------------------------
 * Why a browser is the authority for arithmetic
 *
 * It looks like the one thing that does not need checking. 1in is 96px by
 * definition, 1pt is 4/3 of a pixel, and either the multiplication is right
 * or it is not -- no rendering, no font, nothing to disagree about.
 *
 * Two of the three bugs this corpus found on its first run were exactly
 * there. `1cm` came out 37 where every browser gives 38, because the
 * conversion split the number around the decimal point and truncated 37.795
 * before it rounded; and the whole family of relative units had been reading
 * as a bare `px` and losing its suffix, so `2em` was two pixels rather than
 * thirty-two. Both are arithmetic, both were invisible to reasoning, and both
 * are one line in a table a browser answers in a second.
 *
 * ------------------------------------------------------------------------
 * What is deliberately not here
 *
 * **Fractional results.** `getComputedStyle` reports `0.9em` at a 16px font
 * as `14.4px` and areole says `14px`, because this engine's lengths are whole
 * pixels end to end -- that is invariant 2, not a rounding bug, and a corpus
 * that reported it as a disagreement would report the same disagreement
 * forever. Every case below is chosen so the exact answer is an integer, and
 * the fractional ones are checked in ar_test against the arithmetic instead.
 * The one thing that would close this gap is a sub-pixel used-value stage,
 * which is named in the release document and is not 0.4.1's.
 *
 * **`ic`, `ex` and the metrics a face has to supply.** `ic` needs a font
 * containing U+6C34 and `ex` needs one carrying sxHeight, and what each
 * engine falls back to when the face is silent is a property of the face
 * rather than of the unit. Edge reported `4ex` at a 20px font as 35.78px --
 * a real x-height of 0.447em -- against areole's 40px, which is the half-em
 * the specification names for a font that does not say. Both are correct for
 * the face they were reading. `ch` is here because both engines happen to
 * land on the same half em for it, and `cap` is checked in ar_test.
 *
 * **Angle, time and frequency.** They parse, and nothing reads one: there is
 * no property in this engine that takes an angle until transforms exist. A
 * corpus row for a value nothing consumes would be asserting that two engines
 * agree about a number neither of them uses.
 */
#include "ar_computed.h"

static const ar__case CASES[] = {
    /* -- the absolute units ---------------------------------------------- */
    /*
     * Each of these is a definition rather than a measurement, so the
     * expected number is knowable -- which is exactly why getting one wrong
     * is so easy to not notice.
     */
    {"px", "#x { width:40px; }", "<div id=\"x\">t</div>", "width"},
    {"inch", "#x { width:2in; }", "<div id=\"x\">t</div>", "width"},
    {"pica", "#x { width:3pc; }", "<div id=\"x\">t</div>", "width"},
    {"point", "#x { width:12pt; }", "<div id=\"x\">t</div>", "width"},
    {"point-again", "#x { width:72pt; }", "<div id=\"x\">t</div>", "width"},
    /* 96/2.54 is 37.795, and the three spellings of the same distance have to
       land on the same pixel. This is the case that caught the truncation. */
    {"centimetre", "#x { width:1cm; }", "<div id=\"x\">t</div>", "width"},
    {"millimetre", "#x { width:10mm; }", "<div id=\"x\">t</div>", "width"},
    {"quarter-mm", "#x { width:40q; }", "<div id=\"x\">t</div>", "width"},
    {"centimetre-round", "#x { width:2cm; }", "<div id=\"x\">t</div>", "width"},

    /* A unit is a unit whatever case it is written in. */
    {"upper-case-px", "#x { width:40PX; }", "<div id=\"x\">t</div>", "width"},
    {"mixed-case-pt", "#x { width:12Pt; }", "<div id=\"x\">t</div>", "width"},

    /* -- em, against the element's own font ------------------------------ */
    {"em-at-root", "#x { width:2em; }", "<div id=\"x\">t</div>", "width"},
    {"em-with-own-size", "#x { font-size:20px; width:3em; }", "<div id=\"x\">t</div>",
     "width font-size"},
    /*
     * The ordering case, and the one worth having most.
     *
     * `font-size: 2em` measures against the *parent's* font and the padding
     * beside it measures against the answer. A resolution that took them in
     * the other order gives sixteen and thirty-two instead of thirty-two and
     * thirty-two, and nothing else on the page looks wrong.
     */
    {"em-on-font-size-then-padding", "#w { font-size:16px; } #x { font-size:2em; padding:1em; }",
     "<div id=\"w\"><div id=\"x\">t</div></div>", "font-size padding-left"},
    {"em-inherited-font", "#w { font-size:24px; } #x { width:2em; }",
     "<div id=\"w\"><div id=\"x\">t</div></div>", "width"},
    /* Two levels of doubling: the grandchild is four times the root, and a
       unit resolved twice against the wrong parent gives eight. */
    {"em-compounds-once-per-level",
     "#w { font-size:10px; } #m { font-size:2em; } #x { font-size:2em; }",
     "<div id=\"w\"><div id=\"m\"><div id=\"x\">t</div></div></div>", "font-size"},
    {"em-margin", "#x { font-size:16px; margin:2em; }", "<div id=\"x\">t</div>", "margin-top"},

    /* -- rem, against the root ------------------------------------------- */
    /*
     * The point of `rem` is that it does not care where it is written, so
     * these two have to agree while the `em` pair above do not.
     */
    {"rem-at-root", "#x { width:2rem; }", "<div id=\"x\">t</div>", "width"},
    {"rem-ignores-the-parent", "#w { font-size:40px; } #x { width:2rem; }",
     "<div id=\"w\"><div id=\"x\">t</div></div>", "width"},
    {"rem-beside-em", "#w { font-size:32px; } #x { width:1rem; height:1em; }",
     "<div id=\"w\"><div id=\"x\">t</div></div>", "width height"},

    /* -- the font metrics, and their fallbacks --------------------------- */
    /*
     * A font that does not carry sxHeight gets half an em, and one that does
     * not carry sCapHeight gets the stated approximation. Both engines are
     * running a face without an OS/2 version 2 table here, so both take the
     * fallback -- which is the agreement being checked.
     */
    {"ch-fallback", "#x { font-size:20px; width:4ch; }", "<div id=\"x\">t</div>", "width"},
    {"lh-is-the-line-box", "#x { font-size:20px; line-height:30px; height:2lh; }",
     "<div id=\"x\">t</div>", "height"},

    /* -- the viewport family, and why it is not compared here ----------- */
    /*
     * There are no `vw` rows below, and their absence is the finding rather
     * than an omission.
     *
     * The two engines are not looking at the same viewport. areole renders
     * this corpus into a surface CASE_W wide; the browser lays the twin out
     * in whatever a headless window turns out to be, which was 756 CSS pixels
     * when this was written and is not a number either side controls. So
     * `50vw` is 210 here and 378 there, both of them right, and a row saying
     * so would report a permanent disagreement about the harness.
     *
     * That is the same trap tools/compare_media.py fell into and climbed out
     * of by asking the browser what it actually got. This corpus has nowhere
     * to put the answer -- the surface is fixed before the browser runs -- so
     * the viewport units are checked in ar_test against a stated viewport
     * instead, where the number is known on both sides of the comparison.
     */

    /* -- zero, which needs no unit anywhere ------------------------------ */
    {"zero-is-zero", "#x { margin:0; padding:0; }", "<div id=\"x\">t</div>",
     "margin-top padding-left"},
    {"zero-em", "#x { margin:0em; }", "<div id=\"x\">t</div>", "margin-top"},

    /* -- what is not a length -------------------------------------------- */
    /*
     * A declaration with an invented unit is dropped and costs only itself,
     * which is the difference between a page with one typo losing one
     * property and losing a whole rule. `height` beside it is the witness.
     */
    {"an-invented-unit-is-dropped", "#x { width:40zz; height:33px; }", "<div id=\"x\">t</div>",
     "height"},
    {"a-bare-number-is-dropped", "#x { width:100; height:21px; }", "<div id=\"x\">t</div>",
     "height"},

    /* -- units where a length is expected, not only on width ------------- */
    {"padding-in-pt", "#x { padding:12pt; }", "<div id=\"x\">t</div>", "padding-left"},
    {"margin-in-rem", "#x { margin:1rem; }", "<div id=\"x\">t</div>", "margin-top"},
    {"font-size-in-pt", "#x { font-size:12pt; }", "<div id=\"x\">t</div>", "font-size"},
    {"line-height-in-em", "#x { font-size:20px; line-height:1.5em; }", "<div id=\"x\">t</div>",
     "line-height"},
    /* `line-height: 1.5` is a multiplier and `1.5em` is a length, and telling
       them apart is what the unit peek exists for -- it read `em` as no unit
       at all until 0.4.1 and made this a multiplier of one and a half. */
    {"line-height-bare-is-a-multiplier", "#x { font-size:20px; line-height:1.5; }",
     "<div id=\"x\">t</div>", "line-height"}};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

int main(int argc, char **argv)
{
    return ar__corpus_main(argc, argv, CASES, CASE_N, "units", "ar_units", 0);
}
