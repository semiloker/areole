/*
 * areole example 04 - the showcase
 * SPDX-License-Identifier: MIT
 *
 * One page that uses everything the CSS subset can do, laid out the way a
 * person would lay out a real page rather than the way a test would.
 *
 * The honest framing, because the alternative is a demo that flatters:
 *
 *   There is no HTML here. areole has no parser yet -- that is 0.9.0 -- so the
 *   tree is declared with ar_begin and ar_text. What is real is the CSS: every
 *   rule below is parsed at startup by the same cascade a stylesheet would go
 *   through, and nothing in this file computes a coordinate.
 *
 *   Nothing is rounded, shadowed or gradient-filled, because areole cannot do
 *   any of those yet. border-radius parses and is ignored. So the design is
 *   flat colour and type, which is a constraint the page leans into rather
 *   than one it hides.
 *
 * Each section demonstrates one thing and says what it is showing. Several of
 * them are checked by --selftest, which asserts the claims rather than merely
 * drawing them: that border-box really is narrower than content-box by the
 * padding, that fit-content really does sit between the other two, and that
 * the chip with the highest z-index really is the one on top.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define WIN_W 1180
#define WIN_H 760

#define ATLAS_BYTES (256u * 1024u)
#define MAX_PX      48

static unsigned char g_memory[AR_MEM(900) + 380u * 1024u];
static unsigned char g_font[4u * 1024u * 1024u];
static unsigned char g_fallback[4u * 1024u * 1024u];

/* ------------------------------------------------------------------------
 * The stylesheet
 *
 * Split into small pieces because C90 guarantees only 509 characters in a
 * string literal and adjacent literals count as one. ar_stylesheet appends,
 * so a sheet in sixteen pieces costs sixteen calls at startup and nothing
 * per frame.
 * ------------------------------------------------------------------------ */

/* Every box a border box, which is what somebody building an interface wants:
   a rail declared 260 wide should be 260 wide, padding and all. areole follows
   CSS and defaults the other way, so this is one line to opt out of. */
static const char *S_RESET = "div { box-sizing:border-box; }";

static const char *S_APP = "#app { display:flex; flex-direction:row;"
                           "       background:#f7f4ed; color:#33302b; font-size:14px; }"
                           ".rail { width:260px; display:flex; flex-direction:column;"
                           "        padding:26px 20px; gap:1px; background:#efe9dc; }";

static const char *S_BRAND = ".brand { font-size:30px; color:#1c1a17; }"
                             ".sub { font-size:12px; color:#8a8073; padding-bottom:22px; }"
                             ".rule { height:1px; background:#ddd4c2; }"
                             ".gap { height:18px; }";

static const char *S_NAV = ".nav { padding:7px 10px; font-size:13px; color:#6d6559; }"
                           ".nav:hover { background:#e3dbc9; color:#1c1a17; }"
                           ".nav:active { background:#d3c8b0; }"
                           ".nav-on { padding:7px 10px; font-size:13px; color:#fdfbf6;"
                           "          background:#8c5a34; }";

/*
 * The scrolling column. Everything below lives inside it, which is why the
 * rail stays put while the page moves.
 *
 * Two declarations here are load-bearing and both were found the hard way.
 *
 * display:block, because areole defaults a box to flex and the content height
 * a scroll container measures itself against comes from the block path. As a
 * flex column it had no content height to overflow, so the range was zero and
 * the wheel did nothing at all.
 *
 * height:100%, because align-items:stretch does not hold a block to its
 * parent once the block's own automatic height exceeds it -- the column grew
 * to its content, 2404 px of it, and a container exactly as tall as its
 * contents has nothing to scroll. The percentage pins it to the window.
 */
static const char *S_MAIN = ".main { display:block; width:grow; height:100%; overflow:scroll; }"
                            ".page { display:block; padding:34px 40px; }";

static const char *S_HEAD = ".hero { display:block; position:relative; padding-bottom:8px; }"
                            ".h1 { font-size:40px; color:#1c1a17; }"
                            ".lede { font-size:15px; color:#6d6559; padding-top:6px; }"
                            ".badge { position:absolute; top:6px; right:0px; padding:5px 9px;"
                            "         font-size:11px; color:#fdfbf6; background:#8c5a34; }";

static const char *S_SECT = ".sect { display:block; padding-top:30px; }"
                            ".h2 { display:block; position:sticky; top:0px; padding:6px 0px;"
                            "      font-size:19px; color:#1c1a17; background:#f7f4ed; }"
                            ".note { display:block; font-size:12px; color:#8a8073;"
                            "        padding-bottom:10px; }";

static const char *S_CARD = ".card { display:block; padding:14px; background:#fdfbf6;"
                            "        border:1px solid #e2d9c7; }"
                            ".row { display:flex; flex-direction:row; gap:10px; }"
                            ".col { display:flex; flex-direction:column; gap:6px; width:grow; }"
                            ".lbl { font-size:11px; color:#8a8073; }";

/* Section 2: selectors. The colours are decided by the selectors alone --
   nothing in the C file distinguishes these boxes. */
static const char *S_SEL = ".pal { display:flex; flex-direction:row; gap:6px; }"
                           ".sw { width:60px; height:44px; padding:5px; font-size:10px;"
                           "      color:#fdfbf6; background:#b9ae99; }"
                           ".pal .sw { background:#7a6a52; }"
                           "#pick > .sw { background:#8c5a34; }"
                           ".sw.tall { height:62px; }";

static const char *S_SEL2 = ".sw:first-child { background:#2f5d3f; }"
                            ".sw:last-child { background:#7a3b3b; }"
                            ".sw:is(.mark, .flag) { background:#345b7a; }"
                            ".sw:not(.tall):hover { background:#1c1a17; }";

/* Section 2b: structure. A striped list where nothing in C says which row is
   odd, and an empty box that becomes a divider by being empty. */
static const char *S_STRIPE = ".list { display:block; }"
                              ".li { display:block; padding:5px 9px; font-size:12px; }"
                              ".li:nth-child(odd) { background:#efe9dc; }"
                              ".li:empty { height:6px; background:#d3c8b0; padding:0px; }";

/* Section 3: the cascade. .quiet is less specific than #loud and wins anyway,
   because it is marked important. */
static const char *S_CASC = "#loud { color:#8c5a34; font-size:16px; }"
                            ".quiet { color:#8a8073 !important; }"
                            ".chips { display:flex; flex-direction:row; gap:8px; }"
                            ".chip { padding:6px 10px; font-size:13px; background:#efe9dc; }"
                            ".chip.reset { font-size:initial; color:initial; }";

/* Section 4: the box model. Both are declared width:120px and they differ by
   exactly the padding, which is the whole of what box-sizing means. */
static const char *S_BOX = ".bb { box-sizing:border-box; width:120px; padding:12px;"
                           "      background:#8c5a34; color:#fdfbf6; font-size:11px; }"
                           ".cb { box-sizing:content-box; width:120px; padding:12px;"
                           "      background:#2f5d3f; color:#fdfbf6; font-size:11px; }"
                           ".collapse { display:block; background:#efe9dc; }"
                           ".cm { display:block; height:22px; margin:14px 0px;"
                           "      background:#b9ae99; }";

/* Section 5: flex. */
static const char *S_FLEX = ".fx { display:flex; flex-direction:row; height:46px;"
                            "      background:#efe9dc; gap:6px; }"
                            ".fx-end { justify-content:end; }"
                            ".fx-mid { justify-content:center; }"
                            ".fx-sp { justify-content:space-between; }"
                            ".box { width:46px; height:26px; background:#8c5a34; }"
                            ".grow { width:grow; height:26px; background:#345b7a; }";

/* Section 6 and 7: inline runs, fragmentation, floats. */
static const char *S_INLINE = ".para { display:block; width:420px; padding-top:4px; }"
                              ".t { display:inline; font-size:13px; color:#4a453e; }"
                              ".t-big { display:inline; font-size:22px; color:#1c1a17; }"
                              ".t-key { display:inline; font-size:13px; color:#8c5a34; }";

static const char *S_FLOAT = ".fl { float:left; width:64px; height:64px; margin:0px 10px 6px 0px;"
                             "      background:#2f5d3f; }"
                             ".fr { float:right; width:52px; height:40px;"
                             "      margin:0px 0px 6px 10px; background:#7a3b3b; }"
                             ".clr { display:block; clear:both; height:0px; }";

/* Section 8: intrinsic sizing, the same string three ways. */
static const char *S_FIT = ".mn { display:block; width:min-content; background:#efe9dc;"
                           "      padding:6px; font-size:12px; }"
                           ".mx { display:block; width:max-content; background:#efe9dc;"
                           "      padding:6px; font-size:12px; }"
                           ".ft { display:block; width:fit-content; background:#efe9dc;"
                           "      padding:6px; font-size:12px; }";

/* Section 9: position and stacking. */
static const char *S_POS = ".stage { display:block; position:relative; height:110px;"
                           "         background:#efe9dc; }"
                           ".pc { position:absolute; width:86px; height:52px; padding:6px;"
                           "      font-size:11px; color:#fdfbf6; }"
                           ".p1 { top:12px; left:14px; background:#8c5a34; z-index:3; }"
                           ".p2 { top:30px; left:70px; background:#345b7a; z-index:2; }"
                           ".p3 { top:48px; left:126px; background:#2f5d3f; z-index:1; }";

/* Section 10: scroll, both axes and nested. */
static const char *S_SCROLL = ".scr { display:block; height:120px; overflow:scroll;"
                              "       background:#fdfbf6; border:1px solid #e2d9c7; }"
                              ".strip { display:block; height:46px; overflow-y:hidden;"
                              "         background:#fdfbf6; border:1px solid #e2d9c7; }"
                              ".wide { display:block; width:900px; padding:8px;"
                              "        font-size:12px; background:#efe9dc; }";

/* ------------------------------------------------------------------------
 * Sections
 *
 * The titles are also the jump targets: the nav finds a heading by comparing
 * the pointer ar_node_text hands back against these, which works because
 * areole never copies a caller's string.
 * ------------------------------------------------------------------------ */
enum
{
    SEC_SELECT = 0,
    SEC_STRUCT,
    SEC_CASCADE,
    SEC_BOX,
    SEC_FLEX,
    SEC_INLINE,
    SEC_FLOAT,
    SEC_FIT,
    SEC_POS,
    SEC_SCROLL,
    SEC_TEXT,
    SEC_COUNT
};

static const char *TITLE[SEC_COUNT] = {"Selectors",
                                       "Structure",
                                       "The cascade",
                                       "The box model",
                                       "Flex",
                                       "Inline and fragments",
                                       "Floats",
                                       "Intrinsic sizing",
                                       "Position and stacking",
                                       "Scrolling",
                                       "The world's scripts"};

static const char *LEAD[SEC_COUNT] = {
    "Compound, descendant, child, :is and :not. Nothing in the C file tells "
    "these swatches apart.",
    ":nth-child stripes the list and :empty turns a box with nothing in it "
    "into a divider.",
    "!important beats a more specific id, and the four explicit keywords "
    "reset a value by name.",
    "Two boxes, both declared width:120px. They differ by exactly the "
    "padding, and margins between siblings collapse.",
    "Direction, justification, gap, and one item taking the slack.",
    "Two sizes on one baseline, and a run long enough to be cut across "
    "lines into separate rectangles.",
    "A float shortens the lines beside it, not the block, and clear finds "
    "the bottom of both sides.",
    "The same sentence at its narrowest, its widest, and shrink-to-fit.",
    "Absolute placement inside a relative parent, and z-index deciding what "
    "covers what.",
    "Both axes, and a container inside a container.",
    "Bidirectional text, Arabic joining and Devanagari, when the machine has "
    "the faces for them."};

/* ------------------------------------------------------------------------
 * Sections, declared
 * ------------------------------------------------------------------------ */
static void head(ar_ctx *ui, ar_i32 s)
{
    ar_begin(ui, "div.sect");
    ar_text(ui, "div.h2", TITLE[s]);
    ar_text(ui, "div.note", LEAD[s]);
}

static void sec_select(ar_ctx *ui)
{
    head(ui, SEC_SELECT);
    ar_begin(ui, "div.pal#pick");
    ar_text(ui, "div.sw", "first");
    ar_text(ui, "div.sw", "child of #pick");
    ar_text(ui, "div.sw.mark", ":is match");
    ar_text(ui, "div.sw.tall", "compound");
    ar_text(ui, "div.sw", "last");
    ar_end(ui);
    ar_text(ui, "div.lbl", "Hover any of them: :not(.tall):hover darkens all but the tall one.");
    ar_end(ui);
}

static void sec_struct(ar_ctx *ui)
{
    ar_i32 i;

    head(ui, SEC_STRUCT);
    ar_begin(ui, "div.list");
    for (i = 0; i < 4; ++i)
    {
        ar_text(ui, "div.li",
                i == 0   ? "parsed once at startup"
                : i == 1 ? "resolved per box, from a cache"
                : i == 2 ? "laid out with no floating point"
                         : "painted only where it changed");
    }
    ar_begin(ui, "div.li");
    ar_end(ui);
    ar_text(ui, "div.li", "and the divider above is an empty box");
    ar_end(ui);
    ar_end(ui);
}

static void sec_cascade(ar_ctx *ui)
{
    head(ui, SEC_CASCADE);
    ar_begin(ui, "div.chips");
    ar_text(ui, "div.chip#loud", "#loud wins on specificity");
    ar_text(ui, "div.chip.quiet#loud", ".quiet !important beats it");
    ar_text(ui, "div.chip.reset", "initial, by name");
    ar_end(ui);
    ar_end(ui);
}

static void sec_box(ar_ctx *ui)
{
    head(ui, SEC_BOX);
    ar_begin(ui, "div.row");
    ar_text(ui, "div.bb", "border-box");
    ar_text(ui, "div.cb", "content-box");
    ar_end(ui);
    ar_text(ui, "div.lbl",
            "Same declared width. The green one is wider by its padding, which is what "
            "content-box means and why CSS defaults to it.");
    ar_begin(ui, "div.collapse");
    ar_begin(ui, "div.cm");
    ar_end(ui);
    ar_begin(ui, "div.cm");
    ar_end(ui);
    ar_end(ui);
    ar_text(ui, "div.lbl", "Two 14 px margins between those bars, and 14 px of space: collapsed.");
    ar_end(ui);
}

static void sec_flex(ar_ctx *ui)
{
    head(ui, SEC_FLEX);
    ar_begin(ui, "div.col");

    ar_begin(ui, "div.fx");
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_begin(ui, "div.grow");
    ar_end(ui);
    ar_end(ui);

    ar_begin(ui, "div.fx.fx-mid");
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_end(ui);

    ar_begin(ui, "div.fx.fx-sp");
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_begin(ui, "div.box");
    ar_end(ui);
    ar_end(ui);

    ar_end(ui);
    ar_end(ui);
}

static void sec_inline(ar_ctx *ui)
{
    head(ui, SEC_INLINE);
    ar_begin(ui, "div.para");
    ar_text(ui, "div.t-big", "Two sizes");
    ar_text(ui, "div.t", " sit on one baseline, because a line box takes its height from the ");
    ar_text(ui, "div.t-key", "deepest ascent and the deepest descent");
    ar_text(ui, "div.t",
            " rather than from the tallest box. This run is long enough that the line breaker "
            "has to cut it, and a box that is cut becomes one rectangle per line it touches.");
    ar_end(ui);
    ar_end(ui);
}

static void sec_float(ar_ctx *ui)
{
    head(ui, SEC_FLOAT);
    ar_begin(ui, "div.para");
    ar_begin(ui, "div.fl");
    ar_end(ui);
    ar_begin(ui, "div.fr");
    ar_end(ui);
    ar_text(ui, "div.t",
            "A float leaves the flow and the lines beside it are shortened, which is not the "
            "same as shortening the block: the block still spans its container and only its "
            "line boxes are narrowed. That distinction is why clearfix existed.");
    ar_begin(ui, "div.clr");
    ar_end(ui);
    ar_end(ui);
    ar_end(ui);
}

static void sec_fit(ar_ctx *ui)
{
    head(ui, SEC_FIT);
    ar_begin(ui, "div.col");
    ar_text(ui, "div.lbl", "min-content");
    ar_text(ui, "div.mn", "shrink to the longest word");
    ar_text(ui, "div.lbl", "max-content");
    ar_text(ui, "div.mx", "shrink to the longest word");
    ar_text(ui, "div.lbl", "fit-content");
    ar_text(ui, "div.ft", "shrink to the longest word");
    ar_end(ui);
    ar_end(ui);
}

static void sec_pos(ar_ctx *ui)
{
    head(ui, SEC_POS);
    ar_begin(ui, "div.stage");
    ar_text(ui, "div.pc.p3", "z-index 1");
    ar_text(ui, "div.pc.p2", "z-index 2");
    ar_text(ui, "div.pc.p1", "z-index 3");
    ar_end(ui);
    ar_text(ui, "div.lbl",
            "Declared last to first. Paint order is the stacking tree, not the order they "
            "were written in.");
    ar_end(ui);
}

static void sec_scroll(ar_ctx *ui)
{
    ar_i32 i;

    head(ui, SEC_SCROLL);
    ar_begin(ui, "div.strip");
    ar_text(ui, "div.wide",
            "This strip says overflow-y:hidden and nothing else, and it scrolls sideways "
            "anyway, because CSS uses a lone visible on the other axis as auto.");
    ar_end(ui);
    ar_text(ui, "div.lbl", "Above: one declaration, the other axis inferred. Below: nested.");
    ar_begin(ui, "div.scr");
    for (i = 0; i < 16; ++i)
    {
        ar_text(ui, "div.li", i % 2 ? "point at me and turn the wheel" : "a row inside a scroller");
    }
    ar_end(ui);
    ar_end(ui);
}

static void sec_text(ar_ctx *ui, int have_font)
{
    head(ui, SEC_TEXT);
    if (!have_font)
    {
        ar_text(ui, "div.lbl",
                "No TrueType face was found, so this is the built-in 8x8 bitmap and the "
                "scripts below need one. Everything else on this page is unaffected.");
        ar_end(ui);
        return;
    }
    ar_begin(ui, "div.col");
    ar_text(ui, "div.lbl", "Arabic, joined and shaped right to left");
    ar_text(ui, "div.t", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9");
    ar_text(ui, "div.lbl", "Hebrew, with a number that stays left to right");
    ar_text(ui, "div.t", "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA 2026");
    ar_text(ui, "div.lbl", "Devanagari, with a pre-base matra reordered");
    ar_text(ui, "div.t",
            "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80");
    ar_end(ui);
    ar_end(ui);
}

static void page(ar_ctx *ui, int have_font)
{
    ar_begin(ui, "div.page");

    ar_begin(ui, "div.hero");
    ar_text(ui, "div.h1", "Everything the cascade can do");
    ar_text(ui, "div.lede",
            "A GUI engine in strict C89 with no graphics API. Every rule on this page went "
            "through the same parser a stylesheet does, and nothing here computes a coordinate.");
    ar_text(ui, "div.badge", "0.6.1");
    ar_end(ui);

    sec_select(ui);
    sec_struct(ui);
    sec_cascade(ui);
    sec_box(ui);
    sec_flex(ui);
    sec_inline(ui);
    sec_float(ui);
    sec_fit(ui);
    sec_pos(ui);
    sec_scroll(ui);
    sec_text(ui, have_font);

    ar_begin(ui, "div.gap");
    ar_end(ui);
    ar_end(ui);
}

static void frame(ar_ctx *ui, ar_i32 selected, int have_font)
{
    ar_i32 i;

    ar_begin(ui, "div#app");

    ar_begin(ui, "div.rail");
    ar_text(ui, "div.brand", "areole");
    ar_text(ui, "div.sub", "no graphics API, no allocator, no floating point");
    for (i = 0; i < SEC_COUNT; ++i)
    {
        ar_button(ui, i == selected ? "div.nav-on" : "div.nav", TITLE[i]);
    }
    ar_begin(ui, "div.gap");
    ar_end(ui);
    ar_begin(ui, "div.rule");
    ar_end(ui);
    ar_end(ui);

    ar_begin(ui, "div.main");
    page(ui, have_font);
    ar_end(ui);

    ar_end(ui);
}

/* ------------------------------------------------------------------------
 * Finding things after the frame
 *
 * areole never copies a caller's string, so ar_node_text hands back the very
 * pointer that was passed in and comparing pointers identifies a box. That is
 * the whole of the jump-to-section machinery: no ids, no map, no bookkeeping
 * kept between frames.
 * ------------------------------------------------------------------------ */
static ar_i32 find_text_from(const ar_ctx *ui, const char *literal, ar_i32 from)
{
    ar_i32 i, n = ar_node_count(ui);

    for (i = from < 0 ? 0 : from; i < n; ++i)
    {
        if (ar_node_text(ui, i) == literal)
        {
            return i;
        }
    }
    return -1;
}

static ar_i32 find_text(const ar_ctx *ui, const char *literal)
{
    return find_text_from(ui, literal, 0);
}

/* The scrolling column: the one box with somewhere to go vertically. Asked
   for rather than counted, so adding a section cannot break it. */
static ar_i32 find_scroller(const ar_ctx *ui)
{
    ar_i32 i, n = ar_node_count(ui);

    for (i = 0; i < n; ++i)
    {
        if (ar_node_scroll_range(ui, i) > 0 && ar_node_rect(ui, i).h > 300)
        {
            return i;
        }
    }
    return -1;
}

static void jump_to(ar_ctx *ui, ar_i32 section)
{
    ar_i32 main_i = find_scroller(ui);
    ar_i32 head_i;

    if (main_i < 0)
    {
        return;
    }

    /*
     * Searched from the column rather than from the top, because the nav
     * button and the heading are the same string -- one source of truth for
     * the section's name, and two boxes carrying it. Identity by pointer needs
     * a scope, and the column is it: everything in the rail comes first.
     *
     * The first draft skipped that and jumped to the button, which sits near
     * the top and so looked exactly like a jump that did nothing.
     */
    head_i = find_text_from(ui, TITLE[section], main_i);
    if (head_i < 0)
    {
        return;
    }
    /* Where the heading is now, plus where the column already is, is where the
       column has to go for the heading to sit at its top. */
    ar_node_scroll_to(ui, main_i,
                      ar_node_rect(ui, head_i).y - ar_node_rect(ui, main_i).y +
                          ar_node_scroll(ui, main_i));
}

/* ------------------------------------------------------------------------
 * --selftest
 *
 * Asserts what the page claims rather than that it drew something. A demo
 * that only proves it did not crash is a demo that can be quietly wrong for
 * a release, which has happened here before.
 * ------------------------------------------------------------------------ */
static ar_u32 g_px[WIN_W * WIN_H];

static int selftest(ar_ctx *ui, int have_font)
{
    ar_surface s;
    ar_input   in;
    int        bad = 0;
    ar_i32     bb, cb, mn, p1;

    s.pixels = g_px;
    s.w = WIN_W;
    s.h = WIN_H;
    s.stride = WIN_W;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(ui, &in);
    frame(ui, 0, have_font);
    ar_frame_end(ui, &s);

    if (ar_unbalanced(ui))
    {
        printf("FAIL  the tree was left unbalanced\n");
        bad = 1;
    }
    if (ar_overflowed(ui))
    {
        printf("FAIL  the tree did not fit the box budget\n");
        bad = 1;
    }
    if (ar_stylesheet_errors(ui))
    {
        printf("FAIL  stylesheet reports %lu problem(s)\n",
               (unsigned long)ar_stylesheet_errors(ui));
        bad = 1;
    }

    /* box-sizing: both declared 120, and content-box is wider by its padding
       on both sides. If this ever reads equal, the section is lying. */
    bb = find_text(ui, "border-box");
    cb = find_text(ui, "content-box");
    if (bb < 0 || cb < 0)
    {
        printf("FAIL  the box-sizing pair is missing\n");
        bad = 1;
    }
    else if (ar_node_rect(ui, bb).w != 120 || ar_node_rect(ui, cb).w != 120 + 24)
    {
        printf("FAIL  box-sizing: border-box %ld, content-box %ld, expected 120 and 144\n",
               (long)ar_node_rect(ui, bb).w, (long)ar_node_rect(ui, cb).w);
        bad = 1;
    }

    /* Intrinsic sizing: the same string three ways, and fit-content has to
       land between the other two rather than beside one of them. */
    mn = find_text(ui, "shrink to the longest word");
    if (mn < 0)
    {
        printf("FAIL  the intrinsic sizing row is missing\n");
        bad = 1;
    }
    else
    {
        /* Three boxes share one string, so they are found by walking on from
           the first rather than by pointer alone. */
        ar_i32 i, n = ar_node_count(ui), seen = 0;
        ar_i32 w[3];

        for (i = 0; i < n && seen < 3; ++i)
        {
            if (ar_node_text(ui, i) == ar_node_text(ui, mn))
            {
                w[seen++] = ar_node_rect(ui, i).w;
            }
        }
        if (seen != 3)
        {
            printf("FAIL  expected three intrinsic boxes, found %ld\n", (long)seen);
            bad = 1;
        }
        else if (!(w[0] < w[1] && w[2] >= w[0] && w[2] <= w[1]))
        {
            printf("FAIL  intrinsic: min %ld, max %ld, fit %ld -- fit must sit between\n",
                   (long)w[0], (long)w[1], (long)w[2]);
            bad = 1;
        }
    }

    /*
     * Stacking: the chip with the highest z-index is the one on the glass.
     * Geometry cannot show that -- two boxes overlap either way -- so it takes
     * a pixel, and a pixel takes the section being on screen.
     *
     * Which is why the page is scrolled there first. The first draft sampled
     * where the chip would have been if the page were not a scrolling column,
     * read past the end of the surface, and reported a colour that was never
     * in the stylesheet. The bounds check below is there because that is a
     * mistake worth failing loudly rather than reading whatever is next in
     * memory.
     */
    jump_to(ui, SEC_POS);

    ar_frame_begin(ui, &in);
    frame(ui, SEC_POS, have_font);
    ar_frame_end(ui, &s);

    p1 = find_text(ui, "z-index 3");
    if (p1 < 0)
    {
        printf("FAIL  the stacking chips are missing\n");
        bad = 1;
    }
    else
    {
        ar_rect r = ar_node_rect(ui, p1);
        ar_i32  x = r.x + r.w - 2;
        ar_i32  y = r.y + r.h - 2;

        if (x < 0 || y < 0 || x >= s.w || y >= s.h)
        {
            printf("FAIL  stacking: the section did not scroll into view (%ld,%ld)\n", (long)x,
                   (long)y);
            bad = 1;
        }
        /* The bottom right corner of the top chip is the part the chip
           declared after it overlaps, so this one pixel is the whole test. */
        else if ((g_px[y * s.stride + x] & 0xFFFFFFu) != 0x8C5A34u)
        {
            printf("FAIL  stacking: the top chip is covered, pixel %06lX\n",
                   (unsigned long)(g_px[y * s.stride + x] & 0xFFFFFFu));
            bad = 1;
        }
    }

    if (!bad)
    {
        ar_i32 sc = find_scroller(ui);

        printf("ok    %ld boxes, a page %ld px longer than the window it is in\n",
               (long)ar_node_count(ui), (long)(sc >= 0 ? ar_node_scroll_range(ui, sc) : 0));
    }
    printf("%s\n", bad ? "selftest FAILED" : "selftest passed");
    return bad;
}

/* ------------------------------------------------------------------------
 * Loading a face
 * ------------------------------------------------------------------------ */
static ar_u32 read_file(const char *path, unsigned char *buf, ar_u32 cap)
{
    FILE  *f = fopen(path, "rb");
    size_t n;

    if (!f)
    {
        return 0;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (ar_u32)n;
}

static const char *FACES[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                              "C:/Windows/Fonts/calibri.ttf", "C:/Windows/Fonts/tahoma.ttf"};

static const char *FALLBACKS[] = {"C:/Windows/Fonts/nirmala.ttf", "C:/Windows/Fonts/arial.ttf"};

static void sheets(ar_ctx *ui)
{
    ar_stylesheet(ui, S_RESET);
    ar_stylesheet(ui, S_APP);
    ar_stylesheet(ui, S_BRAND);
    ar_stylesheet(ui, S_NAV);
    ar_stylesheet(ui, S_MAIN);
    ar_stylesheet(ui, S_HEAD);
    ar_stylesheet(ui, S_SECT);
    ar_stylesheet(ui, S_CARD);
    ar_stylesheet(ui, S_SEL);
    ar_stylesheet(ui, S_SEL2);
    ar_stylesheet(ui, S_STRIPE);
    ar_stylesheet(ui, S_CASC);
    ar_stylesheet(ui, S_BOX);
    ar_stylesheet(ui, S_FLEX);
    ar_stylesheet(ui, S_INLINE);
    ar_stylesheet(ui, S_FLOAT);
    ar_stylesheet(ui, S_FIT);
    ar_stylesheet(ui, S_POS);
    ar_stylesheet(ui, S_SCROLL);
}

static int load_face(ar_ctx *ui)
{
    ar_i32 i;

    for (i = 0; i < (ar_i32)(sizeof FACES / sizeof FACES[0]); ++i)
    {
        ar_u32 n = read_file(FACES[i], g_font, (ar_u32)sizeof g_font);

        if (n && ar_font_load(ui, g_font, n, ATLAS_BYTES, MAX_PX))
        {
            for (i = 0; i < (ar_i32)(sizeof FALLBACKS / sizeof FALLBACKS[0]); ++i)
            {
                ar_u32 m = read_file(FALLBACKS[i], g_fallback, (ar_u32)sizeof g_fallback);
                if (m)
                {
                    ar_font_add(ui, g_fallback, m);
                }
            }
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    ar_ctx     *ui;
    ar_win     *win;
    ar_surface *s;
    ar_i32      selected = 0;
    ar_i32      region, i;
    int         have_font;

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        printf("not enough memory for a context\n");
        return 1;
    }

    /* Before the first frame. A frame reserves the whole box budget from the
       other end of the arena and does not release it until the next one, so a
       face loaded afterwards has almost nothing left to live in. */
    have_font = load_face(ui);
    sheets(ui);
    if (ar_stylesheet_errors(ui))
    {
        printf("stylesheet has %lu problem(s)\n", (unsigned long)ar_stylesheet_errors(ui));
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
    {
        return selftest(ui, have_font);
    }

    win = ar_win_open("areole - the showcase", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }
    ar_set_clock(ui, ar_time_us);
    printf("areole %s, face: %s\n", ar_version(), have_font ? "TrueType" : "built-in 8x8");

    while (ar_win_pump(win))
    {
        ar_i32 clicked = -1;

        s = ar_win_surface(win);

        ar_frame_begin(ui, ar_win_input(win));
        ar_begin(ui, "div#app");

        ar_begin(ui, "div.rail");
        ar_text(ui, "div.brand", "areole");
        ar_text(ui, "div.sub", "no graphics API, no allocator, no floating point");
        for (i = 0; i < SEC_COUNT; ++i)
        {
            if (ar_button(ui, i == selected ? "div.nav-on" : "div.nav", TITLE[i]))
            {
                clicked = i;
            }
        }
        ar_begin(ui, "div.gap");
        ar_end(ui);
        ar_begin(ui, "div.rule");
        ar_end(ui);
        ar_end(ui);

        ar_begin(ui, "div.main");
        page(ui, have_font);
        ar_end(ui);

        ar_end(ui);
        ar_frame_end(ui, s);

        if (clicked >= 0)
        {
            selected = clicked;
            jump_to(ui, clicked);
        }

        for (region = 0; region < ar_damage_count(ui); ++region)
        {
            ar_win_present(win, ar_damage_rect(ui, region));
        }
        ar_frame_presented(ui);

        if (ar_needs_redraw(ui))
        {
            ar_win_wake(win);
        }
    }

    ar_win_close(win);
    return 0;
}
