/*
 * areole example 02 - the tour
 * SPDX-License-Identifier: MIT
 *
 * One page per release, showing what that release actually added, running.
 *
 *   0.1.0  one block of memory, one blit, and a box tree declared in C
 *   0.1.1  the counters, and the overlay that reads them
 *   0.1.2  damage tracking: the regions presented this frame, live
 *   0.2.0  TrueType outlines, and the four knobs that decide how they land
 *          on a pixel grid
 *   0.3.0  bidirectional text and OpenType shaping
 *   0.4.0  the cascade: inheritance, combinators, !important, structure
 *   0.5.0  block and inline: margin collapsing, floats, fragmented inlines
 *   0.6.0  position and scroll: absolute, sticky, z-index, a scrollable list
 *   0.6.1  both axes, nested containers, a scrollbar you can drag, snapping,
 *          keyboard scrolling and a styled thin bar
 *
 * Every page is declared with ar_begin and ar_text and laid out from the
 * stylesheet below. There is one coordinate in this file, for the overlay,
 * and it is there because the overlay is drawn on top of the frame rather
 * than inside it.
 *
 * The pages are not screenshots of features. Pressing a toggle on the 0.2.0
 * page changes how this frame is rasterized, and the text next to it is
 * redrawn by the setting it describes.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN_W 1120
#define WIN_H 720

/*
 * The block. Sized by the rule in areole.h:
 *
 *     AR_MEM(boxes) + (atlas_bytes + scratch) * 6 / 5
 *
 * with a 256 KB atlas and 48 px of scratch, which is about 36 KB fixed plus
 * (48 + 2) * 48 * 4 -- call the whole right-hand term 380 KB. It is static, so
 * this process makes exactly one allocation and the operating system made it
 * before main ran.
 */
#define ATLAS_BYTES (256u * 1024u)
#define MAX_PX      48
static unsigned char g_memory[AR_MEM(600) + 380u * 1024u];

/* The font file, read once and kept: ar_font_load does not copy it. */
static unsigned char g_font[4u * 1024u * 1024u];
static unsigned char g_fallback[4u * 1024u * 1024u];

/* ------------------------------------------------------------------------
 * The stylesheet
 *
 * Split because C90 guarantees only 509 characters in a string literal, and
 * adjacent literals count as one. ar_stylesheet appends, so a sheet in six
 * pieces costs six calls and nothing else.
 * ------------------------------------------------------------------------ */
/*
 * border-box, on everything, which is what an application does.
 *
 * areole follows CSS and defaults to `content-box`, where a stated width is
 * the content and the padding goes around it. That is the right default for a
 * library that claims to render real CSS, and the wrong one for writing an
 * interface: this rail is 250 px wide and has 18 px of padding, and nobody
 * wants to have said 214.
 */
static const char *SHEET_RESET = "div { box-sizing:border-box; }";

static const char *SHEET_FRAME =
    /* A column: the interface, then the status bar. The bar used to be drawn
       over the frame at a fixed y, and once text started wrapping the pages
       grew tall enough to run underneath it -- two different strings on the
       same pixels, which reads as one smeared dark one. A box cannot collide
       with its siblings. */
    "#app  { display:flex; flex-direction:column; background:#fcfaf6;"
    "        font-size:14px; color:#3a3733; }"
    /* Clipped, because areole neither shrinks a flex item nor scrolls: a
       page taller than the window would otherwise paint straight over the
       status bar below it. Clipping is the honest degradation -- the content
       is cut off where it runs out of room and says so. */
    ".main { height:grow; overflow:hidden; display:flex; flex-direction:row; }"
    ".rail { width:250px; display:flex; flex-direction:column; padding:18px;"
    "        gap:2px; background:#f4efe4; }"
    ".brand   { font-size:26px; color:#20201e; }";

static const char *SHEET_STATUS =
    ".tagline { font-size:12px; color:#8d8578; padding-bottom:16px; }"
    ".status { height:22px; padding:0 12px; background:#f4efe4;"
    "          display:flex; flex-direction:row; align-items:center; }"
    ".statustext { font-size:12px; color:#8d8578; }";

static const char *SHEET_NAV =
    ".nav    { padding:8px 11px; color:#6f685d; }"
    ".nav:hover  { background:#ece4d3; color:#20201e; }"
    ".nav:active { background:#ddd2ba; }"
    ".nav-on { padding:8px 11px; color:#20201e; background:#ddd2ba; }"
    ".navsub { font-size:11px; color:#a09789; padding:2px 11px 10px 11px; }"
    ".page   { width:grow; display:flex; flex-direction:column;"
    "          padding:26px; gap:6px; }";

static const char *SHEET_TEXT = ".h1   { font-size:30px; color:#20201e; }"
                                ".sub  { font-size:13px; color:#8d8578; padding-bottom:14px; }"
                                ".h2   { font-size:17px; color:#20201e; padding-top:12px; }"
                                ".p    { font-size:13px; color:#4a453e; }"
                                ".dim  { font-size:12px; color:#a09789; }"
                                ".num  { font-size:13px; color:#a1552b; }"
                                ".big  { font-size:34px; color:#20201e; }";

static const char *SHEET_ROWS =
    ".row  { display:flex; flex-direction:row; gap:12px; align-items:center; }"
    ".col  { display:flex; flex-direction:column; gap:5px; width:grow; }"
    ".card { width:grow; display:flex; flex-direction:column; padding:14px;"
    "        gap:6px; background:#f7f2e7; border:1px solid #e6dcc6; }"
    ".swatch { width:46px; height:46px; }"
    ".bar  { height:10px; background:#c2703d; }"
    ".spacer { height:10px; }";

static const char *SHEET_TOGGLE = ".tog    { padding:6px 10px; font-size:12px; color:#6f685d;"
                                  "          border:1px solid #e0d6c0; }"
                                  ".tog:hover  { background:#ece4d3; color:#20201e; }"
                                  ".tog-on { padding:6px 10px; font-size:12px; color:#fdfaf3;"
                                  "          background:#7a4a2a; border:1px solid #7a4a2a; }"
                                  ".tog-on:hover { background:#8f5730; }";

/*
 * The 0.4.0 page's own rules, kept apart because they are the exhibit.
 *
 *  .cascade sets a colour and a size that nothing below it repeats -- what
 *  the leaves show is inherited.
 *  .cascade .tile is a descendant combinator; #demo > .tile a child one.
 *  .tile.wide is a compound: both, on one element.
 *  .quiet is less specific than #demo and marked !important, so it wins.
 *  :first-child, :last-child, :nth-child(odd) and :empty pick boxes out by
 *  where they sit rather than by what they were called.
 */
static const char *SHEET_CASCADE = ".cascade { color:#2f5d3f; font-size:15px; display:flex;"
                                   "           flex-direction:column; gap:8px; }"
                                   ".tile  { padding:8px 10px; background:#eef1ea; }"
                                   ".cascade .tile  { border:1px solid #cdd8cd; }"
                                   "#demo > .tile   { background:#e4ebe2; }"
                                   ".tile.wide      { width:grow; }";

static const char *SHEET_STRUCTURE = ".quiet { color:#8d8578 !important; }"
                                     ".tile:first-child { background:#d8e6d8; }"
                                     ".tile:last-child  { background:#f3e6da; }"
                                     ".stripe:nth-child(odd) { background:#f2ede2; }"
                                     ".stripe:empty   { height:8px; background:#ddd2ba; }"
                                     ".loud { color:#a1552b; }"
                                     /* A group that stacks. #demo and #rows would
                                        otherwise take the default flex-row and lay
                                        their tiles out side by side -- which is
                                        exactly what --dump caught. */
                                     ".stack { display:flex; flex-direction:column;"
                                     "         gap:6px; }";

/*
 * The 0.5.0 page's rules.
 *
 * Every one of these is a thing that could not be said before 0.5.0: a block
 * that stacks, two margins that collapse into one, a float the text runs
 * around, and an inline run cut across lines.
 */
static const char *SHEET_FLOW = ".doc   { display:block; background:#f7f3ea; padding:10px; }"
                                ".para  { display:block; margin:12px 0; background:#eee6d6; }"
                                ".figure { display:block; float:left; width:70px; height:52px;"
                                "          margin:0 10px 6px 0; background:#c2703d; }"
                                ".em    { display:inline; color:#a1552b; }"
                                ".plain { display:inline; }";

/*
 * The 0.6.0 page's rules.
 *
 * The stack demonstrates z-index by overlapping three boxes on purpose; the
 * card shows a badge pinned to a corner, which is the whole reason absolute
 * positioning exists; and the list is a real scroll container with a sticky
 * header inside it.
 */
static const char *SHEET_POS =
    ".stage { display:block; position:relative; height:96px;"
    "         background:#f4efe4; }"
    ".chip  { display:block; position:absolute; width:70px; height:44px;"
    "         font-size:11px; padding:5px; color:#fdfaf3; }"
    ".c1 { top:10px; left:10px;  background:#7a4a2a; z-index:3; }"
    ".c2 { top:26px; left:46px;  background:#c2703d; z-index:2; }"
    ".c3 { top:42px; left:82px;  background:#d9a273; z-index:1; }"
    ".pin { top:8px; right:8px; width:56px; height:20px; background:#2f5d3f; }";

static const char *SHEET_POS2 =
    ".scroller { display:block; height:130px; overflow:scroll;"
    "            background:#f7f3ea; }"
    ".sticky { display:block; position:sticky; top:0; padding:4px 8px;"
    "          font-size:11px; color:#fdfaf3; background:#7a4a2a; }"
    ".sect { display:block; }"
    /* Not `.row`: that already means a flex row on four earlier pages, and
       redefining it here quietly broke every one of them. */
    ".srow { display:block; padding:3px 8px; font-size:12px; color:#4a453e; }"
    ".srow:nth-child(even) { background:#efe8d8; }";

/*
 * The 0.6.1 page: the axes, and the bar.
 *
 * `.strip` states only overflow-y, and the sideways scrolling it gets is the
 * specification's doing rather than a convenience -- a lone `visible` on the
 * other axis is used as `auto`, because content cannot escape sideways from a
 * box that clips it vertically. The wide row inside it is what gives the strip
 * something to travel over.
 *
 * `.outer` holds `.inner` so that two scroll containers are nested, which is
 * what makes chaining visible: the inner list swallows the wheel until it runs
 * out and then hands it upwards.
 */
static const char *SHEET_SCROLL2 =
    ".strip { display:block; height:44px; overflow-y:hidden; background:#f7f3ea; }"
    ".wide  { display:block; width:900px; height:28px; padding:6px 8px;"
    "         font-size:12px; color:#4a453e; background:#e8dfcb; }"
    ".outer { display:block; height:140px; overflow:scroll; background:#f4efe4;"
    "         padding:6px; }"
    ".inner { display:block; height:90px; overflow:scroll; background:#fdfaf3; }";

/*
 * A second sheet rather than more of the first, because C89 guarantees only
 * 509 characters in a string literal after concatenation and the combined one
 * came to 786. gcc says so with -Woverlength-strings under -pedantic-errors,
 * which is the gate doing its job: a compiler with extensions on would have
 * taken it and left the limit to be discovered on an old toolchain.
 *
 * A snapping list. Rows are 34 tall and a notch is 30, so a wheel that did not
 * snap would leave one straddling the top edge on every notch -- which is what
 * makes the snapping visible rather than asserted. `contain` stops the notch
 * chaining to the page once the list bottoms out, and the bar is thin and
 * coloured because it was asked to be.
 */
static const char *SHEET_SNAP =
    ".snap  { display:block; height:102px; overflow:scroll; background:#fdfaf3;"
    "         scroll-snap-type: y mandatory; overscroll-behavior: contain;"
    "         scrollbar-width: thin; scrollbar-color: #9c8f74 #efe8d8;"
    "         scrollbar-gutter: stable; }"
    ".slide { display:block; height:34px; padding:0px 8px; font-size:12px;"
    "         color:#4a453e; scroll-snap-align: start; }"
    ".slide:nth-child(even) { background:#efe8d8; }";

/*
 * The 0.6.2 page.
 *
 * One scroller with a header pinned to its top and a footer pinned to its
 * bottom, which is the pair the release completed: the same box obeys `top`
 * and `bottom` against the same scrollport, and both clamp to the containing
 * block rather than escaping it.
 */
static const char *SHEET_SAFE =
    ".sscroll { display:block; height:120px; overflow:scroll; background:#f7f3ea; }"
    ".shead { display:block; position:sticky; top:0px; padding:3px 8px;"
    "         font-size:11px; color:#fdfaf3; background:#2f5d3f; }"
    ".sfoot { display:block; position:sticky; bottom:0px; padding:3px 8px;"
    "         font-size:11px; color:#fdfaf3; background:#7a4a2a; }";

/*
 * env(), reading two names that resolve by different routes.
 *
 * The titlebar rectangle was reported by this file below, so it resolves to
 * what was reported. Nothing ever reports a safe-area inset on a desktop
 * window, so that one falls back -- and the two boxes are the same declaration
 * apart from the name, which is what makes the difference visible rather than
 * asserted.
 *
 * A separate literal because C89 guarantees only 509 characters after
 * concatenation, and the sheet above is most of one already.
 */
static const char *SHEET_ENV =
    ".envbar { display:block; height:15px; font-size:11px; color:#4a453e;"
    "          background:#e8dfcb; padding-left: env(titlebar-area-width, 40px); }"
    ".envfb  { display:block; height:15px; font-size:11px; color:#4a453e;"
    "          background:#efe8d8; padding-left: env(safe-area-inset-left, 40px); }";

/*
 * The 0.6.3 page.
 *
 * `.trap` clips and `.deep` asks for a z-index far beyond anything else on the
 * page, so `.esc` is inside a box that would cut it off and behind a box that
 * would cover it. It is in the top layer and neither happens, which is the
 * whole claim: a z-index cannot lift a box out of the context it is in, and
 * `overlay` is not a z-index.
 */
static const char *SHEET_LAYER =
    ".trap { display:block; height:40px; overflow:hidden; background:#f0ece2;"
    "        position:relative; }"
    ".deep { display:block; position:absolute; top:6px; left:150px; width:120px;"
    "        height:54px; z-index:9999; background:#c2703d; }"
    ".esc  { display:block; position:absolute; top:22px; left:10px; width:130px;"
    "        height:26px; padding:5px 8px; font-size:11px; color:#fdfaf3;"
    "        background:#2f5d3f; overlay:auto; }";

/*
 * The anchored pair, and a box that says it takes no input.
 *
 * `.tip` names no coordinates of its own: it is placed entirely by the box it
 * is anchored to, which is what keeps the two together when either moves.
 */
static const char *SHEET_ANCH =
    ".anchorbar { display:block; height:26px; background:#e8dfcb; position:relative; }"
    ".target { display:block; position:absolute; top:4px; left:60px; width:90px;"
    "          height:18px; background:#7a4a2a; anchor-name: --t; }"
    ".tip  { display:block; position:absolute; position-anchor: --t;"
    "        top: anchor(bottom); left: anchor(center); width:110px; height:20px; }";

/* Split for the 509-character limit again, which is now the fourth time this
   file has hit it. */
static const char *SHEET_ANCH2 =
    ".tip  { padding:3px 6px; font-size:11px; color:#fdfaf3; background:#4a453e; }"
    ".dead { display:block; padding:4px 8px; font-size:11px; background:#efe8d8;"
    "        color:#8d8578; inert: auto; }";

/* ------------------------------------------------------------------------
 * Text for the 0.3.0 page
 *
 * Written as escapes rather than as literal bytes. A C89 source file has no
 * way to declare its own encoding, and a file that renders Arabic correctly
 * only when the compiler happens to read it as UTF-8 is not a demonstration
 * of anything. These are the UTF-8 bytes, unambiguously.
 * ------------------------------------------------------------------------ */

/* Arabic: "the Arabic language" -- every letter changes shape by position */
static const char *AR_ARABIC = "\xd8\xa7\xd9\x84\xd9\x84\xd8\xba\xd8\xa9 "
                               "\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9";

/* Arabic: "lam alef", the ligature every Arabic font is required to have */
static const char *AR_LIGATURE = "\xd9\x84\xd8\xa7";

/* Hebrew: "shalom" */
static const char *AR_HEBREW = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";

/* Devanagari: "hindi" -- a pre-base matra that is typed after its consonant
   and drawn before it, which is the whole of Indic reordering in one word */
static const char *AR_HINDI = "\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d"
                              "\xe0\xa4\xa6\xe0\xa5\x80";

/* A number inside Arabic: left to right inside right to left, which is the
   case UAX #9 exists for */
static const char *AR_MIXED = "\xd8\xa7\xd9\x84\xd8\xb3\xd8\xb9\xd8\xb1 1250 "
                              "\xd8\xaf\xd9\x88\xd9\x84\xd8\xa7\xd8\xb1";

/* Latin, for the pages that are about rasterizing rather than about script */
static const char *AR_LATIN = "Waltz, bad nymph, for quick jigs vex.";

/*
 * The 0.7.0 page.
 *
 * Three tables and one shape between them: the same four cells, laid out three
 * ways. `.auto` lets the columns take what their contents want, `.fixed` gives
 * them equal shares whatever the contents want, and `.coll` collapses the
 * borders so the line between two cells is one line and not two.
 *
 * The spanning cell is written with CSS rather than an attribute. colspan and
 * rowspan are attributes in HTML and there is no parser until 0.9.0, so a
 * stylesheet is the only place to put one -- which is the whole reason those
 * two properties cost per-box style bytes.
 */
static const char *SHEET_TABLE_A =
    ".tbl  { display:table; width:260px; margin-bottom:8px; background:#f4f1ec; }"
    ".fixd { table-layout:fixed; }"
    ".coll { border-collapse:collapse; }"
    ".trow { display:table-row; }"
    ".cap  { display:table-caption; height:16px; font-size:11px; color:#6a6258;"
    "        background:#e8dfcb; padding:2px 6px; }";

static const char *SHEET_TABLE_B =
    ".cell { display:table-cell; height:20px; font-size:11px; color:#3a352e;"
    "        padding:3px 6px; background:#cfd8e3; }"
    /* Not `.wide`: the scroll page already has one, 900 px and a block, and
       the later rule would win on both boxes. */
    ".cspan { colspan:2; background:#b9c8dc; }"
    ".tall { rowspan:2; background:#c8d6c2; }"
    ".coll .cell { border:2px #6a6258; }";

/*
 * The 0.7.1 page.
 *
 * One table, scrolled, with its header pinned and its first column frozen --
 * and a second table beside it with a row and a column closed, so the two
 * meanings of "gone" can be seen next to each other.
 *
 * `.rgone` and `.cgone` are the same declaration on different boxes, which is
 * the point: `visibility: collapse` removes the track and leaves every other
 * column exactly where it was.
 */
static const char *SHEET_T71_A = ".vport { display:block; width:250px; height:88px; overflow:auto;"
                                 "         background:#faf7f0; margin-bottom:10px; }"
                                 /* Not `.wide`: the scroll page already has one, 900 px and a
                                    block, and this rule is written later so it would win on both.
                                    The 0.7.0 page learned the same lesson one release ago. */
                                 ".wtable { display:table; width:420px; }"
                                 ".thead { display:table-header-group; position:sticky; top:0px; }"
                                 ".froze { position:sticky; left:0px; }";

static const char *SHEET_T71_B =
    ".hcell { display:table-cell; width:105px; height:20px; font-size:11px;"
    "         color:#3a352e; padding:2px 6px; background:#b9c8dc; }"
    ".bcell { display:table-cell; width:105px; height:20px; font-size:11px;"
    "         color:#3a352e; padding:2px 6px; background:#cfd8e3; }"
    ".colgrp { display:table-column-group; }"
    ".colbox { display:table-column; }"
    ".rgone { visibility:collapse; }"
    ".cgone { visibility:collapse; }";

/*
 * The 0.8.0 page.
 *
 * Three flex rows that only a real solver gets right, and one grid.
 *
 * `.f3` and `.f1` are the ratio a factor names -- three shares against one --
 * where the old subset divided by the *number* of growers and made them equal.
 * `.capped` is the loop: three items sharing the row, one of them stopped by a
 * maximum, and the space it cannot take going to the other two.
 */
static const char *SHEET_F80_A =
    ".fbar  { display:flex; flex-direction:row; width:420px; gap:4px;"
    "         margin-bottom:8px; }"
    ".fit   { height:22px; font-size:11px; color:#3a352e; padding:3px 6px;"
    "         background:#cfd8e3; flex-basis:0px; flex-grow:1; }"
    ".f3    { flex-grow:3; background:#b9c8dc; }"
    ".capped { max-width:60px; background:#c8d6c2; }";

static const char *SHEET_F80_B =
    ".grid  { display:grid; width:420px; grid-template-columns: 120px 1fr 2fr;"
    "         gap:6px; margin-bottom:8px; }"
    ".gcell { height:24px; font-size:11px; color:#3a352e; padding:3px 6px;"
    "         background:#cfd8e3; }"
    ".gwide { grid-column: span 2; background:#b9c8dc; }";

/* ------------------------------------------------------------------------
 * Pages
 * ------------------------------------------------------------------------ */
enum
{
    PAGE_CORE = 0,
    PAGE_PERF,
    PAGE_DAMAGE,
    PAGE_TEXT,
    PAGE_SCRIPT,
    PAGE_CASCADE,
    PAGE_FLOW,
    PAGE_POSITION,
    PAGE_SCROLL,
    PAGE_SAFE,
    PAGE_LAYER,
    PAGE_TABLE,
    PAGE_TABLE2,
    PAGE_FLEXGRID,
    PAGE_COUNT
};

static const char *PAGE_VER[PAGE_COUNT] = {"0.1.0", "0.1.1", "0.1.2", "0.2.0", "0.3.0",
                                           "0.4.0", "0.5.0", "0.6.0", "0.6.1", "0.6.2",
                                           "0.6.3", "0.7.0", "0.7.1", "0.8.0"};

static const char *PAGE_NAME[PAGE_COUNT] = {"One block, one blit",
                                            "The counters",
                                            "Damage tracking",
                                            "Outlines",
                                            "Scripts",
                                            "The cascade",
                                            "Block and inline",
                                            "Position and scroll",
                                            "Scrolling",
                                            "Sticky and safe areas",
                                            "The top layer",
                                            "Tables",
                                            "Tables that behave",
                                            "Flex and grid"};

static const char *PAGE_SUB[PAGE_COUNT] = {
    "No allocator, no graphics API, no coordinates in the C file.",
    "Every phase timed, every box counted, read off a real clock.",
    "Only what changed is presented. Watch the regions move.",
    "TrueType glyphs rasterized to a coverage buffer, integer end to end.",
    "UAX #9, OpenType GSUB and GPOS, Arabic joining, Indic reordering.",
    "Inheritance, combinators, !important, and the structural selectors.",
    "Margin collapsing, floats, line boxes and fragmented inlines.",
    "Absolute, fixed, sticky, z-index, and a list you can scroll.",
    "Both axes, snapping, a styled bar, and the arrow keys.",
    "Pinned from either edge, and the display the backend described.",
    "Above every stacking context, out of every clip, attached to an anchor.",
    "A constraint solve, not a tree walk: columns, spans and collapsed lines.",
    "A header that stays, a column that stays, and a row that is not there.",
    "A ratio rather than a count, a maximum that redistributes, and fr."};

/*
 * Strings that have to survive until ar_frame_end.
 *
 * areole stores the pointer it is handed and never copies -- that is why the
 * font parser never allocates and why ar_text costs nothing. The consequence
 * is that a formatted value cannot live in a stack buffer that the next
 * sprintf overwrites. It showed up as four bytes of mojibake in every number
 * on the 0.1.1 page, and only once --dump printed the strings back.
 *
 * A pool with a frame lifetime, reset where the frame is. Sixty-four slots is
 * more than any page formats; overflow returns a marker rather than scribbling
 * past the end, because a demo that corrupts memory to show a number is worse
 * than one that shows a dash.
 */
#define STRPOOL_N   64
#define STRPOOL_CAP 64

static char   g_strpool[STRPOOL_N][STRPOOL_CAP];
static char   g_stroverflow[] = "-";
static ar_i32 g_strpool_used;

static void strpool_reset(void)
{
    g_strpool_used = 0;
}

static const char *fmt(const char *pattern, ...)
{
    va_list ap;
    char   *out;

    if (g_strpool_used >= STRPOOL_N)
    {
        return g_stroverflow;
    }
    out = g_strpool[g_strpool_used++];
    va_start(ap, pattern);
    vsprintf(out, pattern, ap);
    va_end(ap);
    return out;
}

/* Live settings the pages drive. */
struct settings
{
    int antialias;
    int grid_fit;
    int subpixel;
    int shaping;
    int darken;
    int stress; /* the damage page's changing box */
};

/* One toggle button: reports whether it was pressed, styled by its own
   state. There is no widget library here -- ar_button is a selector, a
   label, and a bool. */
static int toggle(ar_ctx *ui, const char *label, int on)
{
    return ar_button(ui, on ? "div.tog-on" : "div.tog", label);
}

static void kv(ar_ctx *ui, const char *k, const char *v)
{
    ar_begin(ui, "div.row");
    ar_text(ui, "div.p", k);
    ar_text(ui, "div.num", v);
    ar_end(ui);
}

/* ------------------------------------------------------------------------
 * 0.1.0 -- one block, one blit
 * ------------------------------------------------------------------------ */
static void page_core(ar_ctx *ui, const ar_surface *s)
{
    int i;

    ar_text(ui, "div.h2", "The box tree");
    ar_text(ui, "div.p",
            "Declared fresh every frame between ar_frame_begin and "
            "ar_frame_end, and released with one integer store.");

    ar_begin(ui, "div.row");
    for (i = 0; i < 6; ++i)
    {
        /* Six boxes whose only difference is a class. Flex decides the
           widths; nothing here knows where any of them will be. */
        ar_begin(ui, "div.swatch.sw");
        ar_end(ui);
    }
    ar_end(ui);

    ar_text(ui, "div.h2", "The block");
    kv(ui, "static block",
       fmt("%lu KB, allocated once, before main ran", (unsigned long)(sizeof g_memory / 1024u)));
    kv(ui, "ephemeral end",
       fmt("%lu bytes this frame", (unsigned long)ar_perf_of(ui)->cur.arena_frame_bytes));
    kv(ui, "boxes declared", fmt("%lu", (unsigned long)ar_perf_of(ui)->cur.nodes));

    ar_text(ui, "div.h2", "The surface");
    kv(ui, "back buffer", fmt("%ld x %ld, one blit", (long)s->w, (long)s->h));
    ar_text(ui, "div.dim",
            "The rasterizer writes pixels into a buffer this program owns. "
            "The platform layer hands that buffer to the window, and nothing "
            "in the core has ever heard of a window.");
}

/* ------------------------------------------------------------------------
 * 0.1.1 -- the counters
 * ------------------------------------------------------------------------ */
static void page_perf(ar_ctx *ui)
{
    const ar_perf *p = ar_perf_of(ui);
    ar_u32         hits, misses;

    ar_text(ui, "div.h2", "This frame");
    kv(ui, "total", fmt("%lu us", (unsigned long)p->cur.total_us));
    kv(ui, "style", fmt("%lu us", (unsigned long)p->cur.phase_us[AR_PHASE_STYLE]));
    kv(ui, "layout", fmt("%lu us", (unsigned long)p->cur.phase_us[AR_PHASE_LAYOUT]));
    kv(ui, "raster", fmt("%lu us", (unsigned long)p->cur.phase_us[AR_PHASE_RASTER]));
    kv(ui, "present", fmt("%lu us", (unsigned long)p->cur.phase_us[AR_PHASE_PRESENT]));

    ar_text(ui, "div.h2", "Over the last frames");
    kv(ui, "median", fmt("%lu us", (unsigned long)ar_perf_percentile(p, AR_PHASE_COUNT, 50)));
    kv(ui, "99th percentile",
       fmt("%lu us", (unsigned long)ar_perf_percentile(p, AR_PHASE_COUNT, 99)));
    kv(ui, "worst", fmt("%lu us", (unsigned long)ar_perf_max(p, AR_PHASE_COUNT)));

    ar_text(ui, "div.h2", "Work");
    kv(ui, "glyphs drawn", fmt("%lu", (unsigned long)p->cur.glyphs));
    kv(ui, "rectangles filled", fmt("%lu", (unsigned long)p->cur.fills));

    ar_style_cache_stats(ui, &hits, &misses);
    kv(ui, "style cache", fmt("%lu hit / %lu miss", (unsigned long)hits, (unsigned long)misses));

    ar_font_cache_stats(ui, &hits, &misses, 0);
    kv(ui, "glyph cache", fmt("%lu hit / %lu miss", (unsigned long)hits, (unsigned long)misses));

    ar_text(ui, "div.dim",
            "The overlay in the corner reads the same numbers. It reports the "
            "previous frame, because measuring the measurement is not useful.");
}

/* ------------------------------------------------------------------------
 * 0.1.2 -- damage
 * ------------------------------------------------------------------------ */
static void page_damage(ar_ctx *ui, struct settings *set)
{
    ar_i32 i, n;

    ar_text(ui, "div.h2", "What was presented");
    n = ar_damage_count(ui);
    kv(ui, "this frame", fmt("%ld region(s)", (long)n));
    for (i = 0; i < n && i < 4; ++i)
    {
        ar_rect r = ar_damage_rect(ui, i);
        kv(ui, "  region", fmt("%ld,%ld %ld x %ld", (long)r.x, (long)r.y, (long)r.w, (long)r.h));
    }
    if (n == 0)
    {
        ar_text(ui, "div.dim",
                "Nothing changed, so nothing was blitted. Move the cursor over "
                "the rail and this list fills in.");
    }

    ar_text(ui, "div.h2", "A box that changes");
    ar_begin(ui, "div.row");
    if (toggle(ui, set->stress ? "wide" : "narrow", set->stress))
    {
        set->stress = !set->stress;
    }
    ar_text(ui, "div.dim", "changes one box, and only that box is presented");
    ar_end(ui);
    ar_begin(ui, set->stress ? "div.bar.b-wide" : "div.bar.b-narrow");
    ar_end(ui);

    ar_text(ui, "div.dim",
            "A hovered item in the rail and a status line in the far corner "
            "are two regions, not one. Their bounding box would be the whole "
            "window, which on a Pentium II is 7.7 ms against 0.01 ms -- the "
            "entire reason the region is a list rather than a rectangle.");
}

/* ------------------------------------------------------------------------
 * 0.2.0 -- outlines
 * ------------------------------------------------------------------------ */
static void page_text(ar_ctx *ui, struct settings *set, int have_font, const char *family)
{
    if (!have_font)
    {
        ar_text(ui, "div.h2", "No TrueType face was found");
        ar_text(ui, "div.p",
                "Everything on this page is drawn with the built-in 8 px bitmap "
                "face, which is the fallback a missing font is supposed to get: "
                "an interface that degrades rather than stops.");
        return;
    }

    ar_text(ui, "div.h2", fmt("Face: %s", family));

    ar_begin(ui, "div.row");
    if (toggle(ui, "antialias", set->antialias))
    {
        set->antialias = !set->antialias;
        ar_font_antialias(ui, set->antialias);
        ar_invalidate_all(ui);
    }
    if (toggle(ui, "grid fit", set->grid_fit))
    {
        set->grid_fit = !set->grid_fit;
        ar_font_grid_fit(ui, set->grid_fit);
        ar_invalidate_all(ui);
    }
    if (toggle(ui, "subpixel", set->subpixel))
    {
        set->subpixel = !set->subpixel;
        ar_font_subpixel(ui, set->subpixel);
        ar_invalidate_all(ui);
    }
    if (toggle(ui, "darken", set->darken != 0))
    {
        set->darken = set->darken ? 0 : 24;
        ar_font_darken(ui, set->darken);
        ar_invalidate_all(ui);
    }
    ar_end(ui);
    ar_text(ui, "div.dim",
            "Each toggle changes how the next frame is rasterized. The lines "
            "below are redrawn by whatever is switched on right now.");

    ar_text(ui, "div.h2", "A size ramp");
    ar_text(ui, "div.big", AR_LATIN);
    ar_text(ui, "div.h1", AR_LATIN);
    ar_text(ui, "div.h2", AR_LATIN);
    ar_text(ui, "div.p", AR_LATIN);
    ar_text(ui, "div.dim", AR_LATIN);

    ar_text(ui, "div.dim",
            "Coverage is accumulated as signed area and prefix-summed along "
            "each row. Twelve fractional bits, because sixteen overflows a "
            "32-bit product -- and no floating point anywhere in the path.");
}

/* ------------------------------------------------------------------------
 * 0.3.0 -- scripts
 * ------------------------------------------------------------------------ */
static void page_script(ar_ctx *ui, struct settings *set, int have_font)
{
    if (!have_font)
    {
        ar_text(ui, "div.h2", "This page needs a TrueType face");
        ar_text(ui, "div.p",
                "The bitmap face is ASCII, and shaping a script it has no "
                "glyphs for would demonstrate the fallback, not the shaper.");
        return;
    }

    ar_begin(ui, "div.row");
    if (toggle(ui, "shaping", set->shaping))
    {
        set->shaping = !set->shaping;
        ar_font_shaping(ui, set->shaping);
        ar_invalidate_all(ui);
    }
    ar_text(ui, "div.dim",
            "Switch it off and the same strings fall back to one glyph per "
            "character, in codepoint order.");
    ar_end(ui);

    ar_text(ui, "div.h2", "Arabic");
    ar_text(ui, "div.big", AR_ARABIC);
    ar_text(ui, "div.dim",
            "Every letter takes an initial, medial, final or isolated form "
            "from what it joins to. GSUB does it; the C here passes a string.");

    ar_text(ui, "div.h2", "The lam-alef ligature");
    ar_text(ui, "div.big", AR_LIGATURE);
    ar_text(ui, "div.dim", "Two codepoints, one glyph, required of every Arabic font.");

    ar_text(ui, "div.h2", "Hebrew");
    ar_text(ui, "div.big", AR_HEBREW);

    ar_text(ui, "div.h2", "A number inside right-to-left text");
    ar_text(ui, "div.big", AR_MIXED);
    ar_text(ui, "div.dim",
            "The digits run left to right inside a line that runs right to "
            "left. This is the case UAX #9 exists for.");

    ar_text(ui, "div.h2", "Devanagari");
    ar_text(ui, "div.big", AR_HINDI);
    ar_text(ui, "div.dim",
            "The vowel sign is typed after its consonant and drawn before it. "
            "Reordering happens before the font is asked for anything.");
}

/* ------------------------------------------------------------------------
 * 0.4.0 -- the cascade
 * ------------------------------------------------------------------------ */
static void page_cascade(ar_ctx *ui)
{
    ar_text(ui, "div.h2", "Inheritance");
    ar_text(ui, "div.p",
            "The container below sets a colour and a size. Nothing inside it "
            "repeats either.");

    ar_begin(ui, "div.cascade");
    ar_text(ui, "div", "A plain box, inheriting both.");
    ar_begin(ui, "div");
    ar_text(ui, "div", "Two levels down, still inheriting.");
    ar_end(ui);
    ar_text(ui, "div.loud", "Until something says otherwise.");
    ar_end(ui);

    ar_text(ui, "div.h2", "Combinators");
    ar_begin(ui, "div.cascade");
    ar_begin(ui, "div#demo.stack");
    ar_text(ui, "div.tile", "#demo > .tile -- a child");
    ar_begin(ui, "div");
    ar_text(ui, "div.tile", ".cascade .tile -- a descendant, not a child");
    ar_end(ui);
    ar_text(ui, "div.tile.wide", ".tile.wide -- a compound, both on one box");
    ar_text(ui, "div.tile.quiet", ".quiet is one class against #demo > .tile, and !important");
    ar_end(ui);
    ar_end(ui);

    ar_text(ui, "div.h2", "Structure");
    ar_text(ui, "div.dim",
            "Nothing below carries a class that says which row it is. The "
            "selectors read that off the tree.");
    ar_begin(ui, "div.cascade");
    ar_begin(ui, "div#rows.stack");
    ar_text(ui, "div.stripe", "first, and odd");
    ar_text(ui, "div.stripe", "second, and even");
    ar_text(ui, "div.stripe", "third, and odd again");
    ar_begin(ui, "div.stripe"); /* no children, no text: :empty */
    ar_end(ui);
    ar_end(ui);
    ar_end(ui);
}

/* ------------------------------------------------------------------------
 * 0.5.0 -- block and inline
 * ------------------------------------------------------------------------ */
static void page_flow(ar_ctx *ui)
{
    ar_text(ui, "div.h2", "Margin collapsing");
    ar_text(ui, "div.p",
            "Two paragraphs with 12 px above and below. The gap between them is "
            "12, not 24: adjacent margins collapse into one.");

    ar_begin(ui, "div.doc");
    ar_text(ui, "div.para", "The first paragraph.");
    ar_text(ui, "div.para", "The second, one margin below it rather than two.");
    ar_end(ui);

    ar_text(ui, "div.h2", "A float, and the text around it");
    ar_begin(ui, "div.doc");
    ar_begin(ui, "div.figure");
    ar_end(ui);
    ar_text(ui, "div.para",
            "This paragraph spans the full width of its container -- a float "
            "shortens line boxes, not block boxes -- and only its lines are "
            "narrowed by the block to the left of them. That is why text wraps "
            "around a figure while the background behind it does not.");
    ar_end(ui);

    ar_text(ui, "div.h2", "One inline box, cut across lines");
    ar_begin(ui, "div.doc");
    ar_text(ui, "div.plain", "An inline run flows into the lines around it, and ");
    ar_text(ui, "div.em", "this emphasised part is one box and several rectangles");
    ar_text(ui, "div.plain", ", because the line breaker cut it where each line ended.");
    ar_end(ui);
}

/* ------------------------------------------------------------------------
 * 0.6.0 -- position and scroll
 * ------------------------------------------------------------------------ */
static void page_position(ar_ctx *ui, ar_i32 rows)
{
    ar_i32 i;

    ar_text(ui, "div.h2", "z-index decides what covers what");
    ar_text(ui, "div.dim",
            "Three absolutely positioned boxes, declared last to first. Paint "
            "order is the stacking tree, not the order they were written in.");
    ar_begin(ui, "div.stage");
    ar_text(ui, "div.chip.c3", "z-index 1");
    ar_text(ui, "div.chip.c2", "z-index 2");
    ar_text(ui, "div.chip.c1", "z-index 3");
    ar_text(ui, "div.chip.pin", "pinned");
    ar_end(ui);

    ar_text(ui, "div.h2", "A scroll container with a sticky header");
    ar_text(ui, "div.dim", "Point at the list and turn the wheel.");
    ar_begin(ui, "div.scroller");
    ar_begin(ui, "div.sect");
    ar_text(ui, "div.sticky", "Section one");
    for (i = 0; i < 8; ++i)
    {
        ar_text(ui, "div.srow", fmt("row %ld", (long)(i + 1)));
    }
    ar_end(ui);
    ar_begin(ui, "div.sect");
    ar_text(ui, "div.sticky", "Section two");
    for (i = 0; i < rows; ++i)
    {
        ar_text(ui, "div.srow", fmt("row %ld", (long)(i + 9)));
    }
    ar_end(ui);
    ar_end(ui);
}

/* The page dispatch, so the window loop and the self-check below run exactly
   the same declarations rather than two versions that can drift apart. */
/*
 * 0.6.1: both axes, nested containers, and a bar that can be dragged.
 *
 * The strip states only overflow-y and scrolls sideways anyway, which is the
 * specification rather than a shortcut -- a lone `visible` on the other axis is
 * used as `auto`. Nothing in areole binds the wheel to the inline axis yet, so
 * the strip is driven from here, which is what an application would do for a
 * keyboard or a swipe.
 */
static ar_i32 g_slide;

static void page_scroll(ar_ctx *ui, ar_i32 slide)
{
    ar_i32 i;

    ar_text(ui, "div.h2", "Sideways, from one declaration");
    ar_text(ui, "div.dim",
            "The strip below says overflow-y: hidden and nothing else. CSS "
            "turns the other axis into a scroller, because content cannot "
            "escape sideways from a box that clips it vertically.");
    ar_begin(ui, "div.strip");
    ar_text(ui, "div.wide",
            "a row nine hundred pixels wide, in a strip that is not - drag the "
            "slider under it and watch this travel");
    ar_end(ui);
    ar_text(ui, "div.dim", fmt("scrolled to %ld px", (long)slide));

    ar_text(ui, "div.h2", "Two containers, one wheel");
    ar_text(ui, "div.dim",
            "Point at the inner list. It takes the wheel until it runs out, "
            "then hands the notch outwards. Both bars can be dragged.");
    ar_begin(ui, "div.outer");
    ar_begin(ui, "div.inner");
    for (i = 0; i < 14; ++i)
    {
        ar_text(ui, "div.srow", fmt("inner row %ld", (long)(i + 1)));
    }
    ar_end(ui);
    for (i = 0; i < 10; ++i)
    {
        ar_text(ui, "div.srow", fmt("outer row %ld", (long)(i + 1)));
    }
    ar_end(ui);

    ar_text(ui, "div.h2", "Snapping, and a bar that was asked to be thin");
    ar_text(ui, "div.dim",
            "Rows are 34 tall and a notch is 30, so an unsnapped wheel would "
            "leave one straddling the top edge every time. Page Up and Down, "
            "Home and End work here too. The list says overscroll-behavior: "
            "contain, so the notch stops at its own end rather than moving "
            "the page.");
    ar_begin(ui, "div.snap");
    for (i = 0; i < 12; ++i)
    {
        ar_text(ui, "div.slide", fmt("slide %ld", (long)(i + 1)));
    }
    ar_end(ui);
}

/*
 * 0.6.2: pinned from either edge, and what the backend said about the display.
 *
 * The scroller holds a sticky header and a sticky footer at once. Scroll it and
 * both stay put while the rows pass between them; scroll to either end and the
 * one at that end lets go, because a sticky box may not leave its containing
 * block. That clamp is what separates sticky from fixed, and it is where the
 * bug this release fixed was living.
 *
 * The two env() bars underneath are the same declaration with a different name
 * in it. The titlebar rectangle was reported, so it resolves to 120. Nothing
 * reports a safe-area inset on a desktop window, so that one takes its 40 px
 * fallback. A reported zero would resolve to zero rather than to the fallback,
 * which is the distinction the whole design turns on and is not visible here
 * because a window has no notch to report.
 */
static void page_safe(ar_ctx *ui)
{
    ar_i32 i;

    ar_text(ui, "div.h2", "Pinned from both edges at once");
    ar_text(ui, "div.dim",
            "The green header holds at the top and the brown footer at the "
            "bottom, both against the same scrollport. Scroll to either end "
            "and that one lets go: a sticky box may not leave the box it "
            "belongs to, which is the whole difference between sticky and "
            "fixed.");
    ar_begin(ui, "div.sscroll");
    ar_text(ui, "div.shead", "pinned to the top");
    for (i = 0; i < 16; ++i)
    {
        ar_text(ui, "div.srow", fmt("row %ld", (long)(i + 1)));
    }
    ar_text(ui, "div.sfoot", "pinned to the bottom");
    ar_end(ui);

    ar_text(ui, "div.h2", "env(), and whether anyone answered");
    ar_text(ui, "div.dim",
            "Both bars are indented by env() with a 40 px fallback. The first "
            "asks for the titlebar width, which this program reported as 120. "
            "The second asks for a safe-area inset, which nothing on a desktop "
            "window reports, so it falls back. A backend answering zero would "
            "give zero, not the fallback -- silence and zero are different "
            "answers.");
    ar_text(ui, "div.envbar", "titlebar-area-width, reported: indented 120");
    ar_text(ui, "div.envfb", "safe-area-inset-left, unreported: indented 40");
}

/*
 * 0.6.3: above everything, out of every clip, and attached to an anchor.
 *
 * There is no modal on this page and that is deliberate rather than an
 * omission: a ::backdrop is a full-viewport fill, so putting one here would
 * cover the tour's own navigation and make it inert -- correct behaviour, and
 * a page nobody could leave. The backdrop and the inertness a modal brings are
 * checked on the pixels in ar_test.c, where a covered viewport costs nothing.
 *
 * What is here is the part that can be looked at: a box that escapes both a
 * clip and a z-index, a tooltip that names no coordinates of its own, and a
 * strip that refuses the pointer.
 */
static void page_layer(ar_ctx *ui)
{
    ar_text(ui, "div.h2", "Out of the clip, over the z-index");
    ar_text(ui, "div.dim",
            "The green box lives inside the grey strip, which clips, and "
            "behind the orange one, which asks for z-index 9999. It is in the "
            "top layer, so neither applies. A z-index cannot lift a box out of "
            "the stacking context it is in -- that is what the top layer is "
            "for, and why it is not simply a larger number.");
    ar_begin(ui, "div.trap");
    ar_begin(ui, "div.esc");
    ar_text(ui, "div.dim", "in the top layer");
    ar_end(ui);
    ar_end(ui);
    ar_begin(ui, "div.deep");
    ar_end(ui);

    ar_text(ui, "div.h2", "Attached to an anchor");
    ar_text(ui, "div.dim",
            "The dark tooltip states no coordinates. It is placed by the box "
            "it names: its top edge at the anchor's bottom, its left edge at "
            "the anchor's centre. Move the anchor and the tooltip goes with "
            "it, which is the point of naming one.");
    ar_begin(ui, "div.anchorbar");
    ar_begin(ui, "div.target");
    ar_end(ui);
    ar_begin(ui, "div.tip");
    ar_end(ui);
    ar_end(ui);

    ar_text(ui, "div.h2", "A strip that takes no pointer");
    ar_text(ui, "div.dim",
            "The row below says inert. It is painted normally and it never "
            "becomes hot, however far into it the cursor goes -- which is what "
            "a modal does to everything behind it, without needing a modal.");
    ar_text(ui, "div.dead", "inert: auto - hovering this does nothing");
}

/*
 * 0.7.0: the first algorithm in this engine that is a solve rather than a walk.
 *
 * A column is as wide as its widest cell needs and a row as tall as its tallest
 * comes to, so no cell can be placed until every cell has been looked at. The
 * three tables below are the same four cells three ways, which is the only way
 * to show that the width came from the contents rather than from the stylesheet.
 */
static void page_table(ar_ctx *ui)
{
    ar_text(ui, "div.h2", "Automatic: the columns take what the contents want");
    ar_text(ui, "div.dim",
            "Nothing here states a column width. The long cell widens its own "
            "column and the short ones stay short, which is what separates a "
            "table from a row of boxes -- every cell in a column has a say in "
            "how wide that column is, and none of them can be placed until all "
            "of them have been read.");
    ar_begin(ui, "div.tbl.auto");
    ar_begin(ui, "div.cap");
    ar_text(ui, "div.dim", "a caption is in no row and no column");
    ar_end(ui);
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell", "a considerably longer cell");
    ar_text(ui, "div.cell", "short");
    ar_end(ui);
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell", "one");
    ar_text(ui, "div.cell", "two");
    ar_end(ui);
    ar_end(ui);

    ar_text(ui, "div.h2", "Fixed: equal shares, whatever the contents want");
    ar_text(ui, "div.dim",
            "The same cells with table-layout: fixed. The columns are settled "
            "without reading past the first row, which is the only affordable "
            "option on a very long table and the reason the mode exists.");
    ar_begin(ui, "div.tbl.fixd");
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell", "a considerably longer cell");
    ar_text(ui, "div.cell", "short");
    ar_end(ui);
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell", "one");
    ar_text(ui, "div.cell", "two");
    ar_end(ui);
    ar_end(ui);

    ar_text(ui, "div.h2", "Spans, and one line between two cells");
    ar_text(ui, "div.dim",
            "The top cell covers two columns and the left one covers two rows. "
            "Borders are collapsed, so the line between two cells is one line "
            "as wide as the wider of the two asked for -- not two borders with "
            "a gap. Both spans are written in CSS: they are HTML attributes, "
            "and there is no parser until 0.9.0.");
    ar_begin(ui, "div.tbl.coll");
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell.tall", "rowspan 2");
    ar_text(ui, "div.cell.cspan", "colspan 2");
    ar_end(ui);
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.cell", "b");
    ar_text(ui, "div.cell", "c");
    ar_end(ui);
    ar_end(ui);
}

/*
 * 0.7.1: what a table has to do before anyone can read one.
 *
 * A header that scrolls away is a table you have to keep scrolling back to,
 * and a filter that moves every column is a table you have to read twice.
 * Both are on this page, and the second is the one worth looking at twice: the
 * closed row's columns do not move, which is the whole reason `collapse` is
 * not `display: none`.
 */
static void page_table2(ar_ctx *ui)
{
    ar_i32 r;

    ar_text(ui, "div.h2", "A header that stays, and a column that stays");
    ar_text(ui, "div.dim",
            "Scroll the panel below with the wheel or drag it sideways. The "
            "header row stays at the top and the first column stays at the "
            "left, both by saying `position: sticky` -- the same mechanism a "
            "sticky sidebar uses, applied to a table box. Nothing here asks "
            "for a z-index: a sticky box is positioned, so it already paints "
            "above the rows going under it.");

    ar_begin(ui, "div.vport");
    ar_begin(ui, "div.wtable");
    ar_begin(ui, "div.thead");
    ar_begin(ui, "div.trow");
    ar_text(ui, "div.hcell.froze", "name");
    ar_text(ui, "div.hcell", "opened");
    ar_text(ui, "div.hcell", "closed");
    ar_text(ui, "div.hcell", "owner");
    ar_end(ui);
    ar_end(ui);
    for (r = 0; r < 6; ++r)
    {
        ar_begin(ui, "div.trow");
        ar_text(ui, "div.bcell.froze", "row");
        ar_text(ui, "div.bcell", "may");
        ar_text(ui, "div.bcell", "june");
        ar_text(ui, "div.bcell", "us");
        ar_end(ui);
    }
    ar_end(ui);
    ar_end(ui);

    ar_text(ui, "div.h2", "A row and a column that are not there");
    ar_text(ui, "div.dim",
            "The middle row and the third column say `visibility: collapse`. "
            "The row is gone and the rows below it have closed up; the column "
            "is gone and the table is narrower by exactly that column. What "
            "has not happened is the interesting part -- none of the other "
            "columns moved. That is the whole difference from `display: none`, "
            "and the reason a filter over a table is bearable to read.");

    ar_begin(ui, "div.tbl.auto");
    /* The column is closed on the `col` box, which is the only place CSS lets
       it be said -- `visibility: collapse` on a cell is not a column. */
    ar_begin(ui, "div.colgrp");
    ar_begin(ui, "div.colbox");
    ar_end(ui);
    ar_begin(ui, "div.colbox");
    ar_end(ui);
    ar_begin(ui, "div.colbox.cgone");
    ar_end(ui);
    ar_end(ui);
    for (r = 0; r < 3; ++r)
    {
        ar_begin(ui, r == 1 ? "div.trow.rgone" : "div.trow");
        ar_text(ui, "div.cell", "one");
        ar_text(ui, "div.cell", "two");
        ar_text(ui, "div.cell", "three");
        ar_end(ui);
    }
    ar_end(ui);
}

/*
 * 0.8.0: the two things a subset could not do.
 *
 * A flex factor is a ratio and not a flag, and a maximum makes the whole
 * distribution a loop rather than a division. Both are visible here: the first
 * row is three-to-one, the second has an item that stops and gives its share
 * back to the others.
 */
static void page_flexgrid(ar_ctx *ui)
{
    ar_text(ui, "div.h2", "A factor is a ratio");
    ar_text(ui, "div.dim",
            "The first box says flex-grow: 3 and the other two say 1, so it "
            "takes three of the five shares. The subset areole shipped with "
            "divided the leftover by the *number* of growing boxes, which made "
            "all three the same width and read the factor as a flag.");
    ar_begin(ui, "div.fbar");
    ar_text(ui, "div.fit.f3", "grow 3");
    ar_text(ui, "div.fit", "grow 1");
    ar_text(ui, "div.fit", "grow 1");
    ar_end(ui);

    ar_text(ui, "div.h2", "A maximum makes it a loop");
    ar_text(ui, "div.dim",
            "The middle box has a max-width it reaches before its share runs "
            "out, so it stops -- and what it could not take goes to the other "
            "two rather than being left on the floor. One division never "
            "revisits that, which is why the specification writes this step as "
            "a loop and why areole now runs one.");
    ar_begin(ui, "div.fbar");
    ar_text(ui, "div.fit", "takes more");
    ar_text(ui, "div.fit.capped", "capped");
    ar_text(ui, "div.fit", "takes more");
    ar_end(ui);

    ar_text(ui, "div.h2", "Two dimensions at once");
    ar_text(ui, "div.dim",
            "A grid of 120px, 1fr and 2fr. The first column is what it says; "
            "the other two share what is left in the ratio they name. The "
            "columns line up down the whole grid, which is the thing nested "
            "flex rows cannot do -- and the wide cell spans two of them.");
    ar_begin(ui, "div.grid");
    ar_text(ui, "div.gcell", "120px");
    ar_text(ui, "div.gcell", "1fr");
    ar_text(ui, "div.gcell", "2fr");
    ar_text(ui, "div.gcell", "a");
    ar_text(ui, "div.gcell.gwide", "span 2");
    ar_end(ui);
}

static void page_body(ar_ctx *ui, const ar_surface *s, ar_i32 page, struct settings *set,
                      int have_font, const char *family)
{
    switch (page)
    {
    case PAGE_CORE:
        page_core(ui, s);
        break;
    case PAGE_PERF:
        page_perf(ui);
        break;
    case PAGE_DAMAGE:
        page_damage(ui, set);
        break;
    case PAGE_TEXT:
        page_text(ui, set, have_font, family);
        break;
    case PAGE_SCRIPT:
        page_script(ui, set, have_font);
        break;
    case PAGE_FLOW:
        page_flow(ui);
        break;
    case PAGE_POSITION:
        page_position(ui, 10);
        break;
    case PAGE_SAFE:
        page_safe(ui);
        break;
    case PAGE_LAYER:
        page_layer(ui);
        break;
    case PAGE_TABLE:
        page_table(ui);
        break;
    case PAGE_TABLE2:
        page_table2(ui);
        break;
    case PAGE_FLEXGRID:
        page_flexgrid(ui);
        break;
    case PAGE_SCROLL:
        page_scroll(ui, g_slide);
        break;
    default:
        page_cascade(ui);
        break;
    }
}

/*
 * --selftest: every page, no window.
 *
 * Runs each page through a real frame against a real surface, so a page that
 * leaves the tree unbalanced, overruns the box budget or trips the stylesheet
 * is caught by a build rather than by someone clicking on it. It is the one
 * check this example carries, and it is the one that would actually fail.
 */
static ar_u32 g_selftest_px[640 * 480];

/*
 * Two identical frames must produce identical pixels.
 *
 * The overlay and the status line are drawn over the frame by this file, and
 * areole cannot see them -- so unless their rectangles are invalidated, damage
 * tracking has no reason to repaint what is underneath and the new text lands
 * on top of the old. The overlay's panel is deliberately translucent, so it
 * does not hide the previous frame's glyphs either; the text goes darker and
 * blurrier every frame until it cannot be read.
 *
 * Nothing about that is visible to a test that renders one frame. This renders
 * two, with the same input and the same page, and compares them. Remove either
 * ar_invalidate call in the loop above and this fails.
 */
/*
 * Nothing paints over the status bar.
 *
 * This is the bug the user saw: once text started wrapping, the pages grew
 * taller than the window, and the status line -- then drawn over the frame at
 * a fixed y -- ended up sharing its pixels with whatever page text had run
 * down that far. Two strings on the same pixels read as one smeared dark one.
 *
 * The bar is a box now, so it cannot collide with its siblings, and `.main` is
 * clipped so an overflowing page is cut off rather than painted past.
 *
 * Checked by rendering the band twice: once under the tallest page, once under
 * an empty one. Any difference is the page bleeding through. Comparing pixels
 * rather than sampling colours, because the first version of this check used a
 * brightness threshold and let the page's own light grey straight past it.
 */
static ar_u32 g_bar_ref[640 * 22];

static void render_with_page(ar_ctx *ui, ar_surface *s, int page, int empty, struct settings *set,
                             int have_font, const char *family)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    strpool_reset();
    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#app");
    ar_begin(ui, "div.main");
    ar_begin(ui, "div.page");
    if (!empty)
    {
        ar_text(ui, "div.h1", PAGE_NAME[page]);
        ar_text(ui, "div.sub", PAGE_SUB[page]);
        page_body(ui, s, page, set, have_font, family);
    }
    ar_end(ui);
    ar_end(ui);
    ar_begin(ui, "div.status");
    ar_text(ui, "div.statustext", "status");
    ar_end(ui);
    ar_end(ui);
    ar_invalidate_all(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);
}

static int check_status_bar_is_clean(ar_ctx *ui, int have_font, const char *family)
{
    ar_surface      s;
    struct settings set;
    ar_i32          x, y;
    ar_i32          strangers = 0;

    /* Deliberately narrow and short. Wrapping makes the page far taller than
       the window at this width, which is the condition the bug needed: a
       comfortable 640x480 does not overflow far enough to reach the bar, and a
       check that cannot fail is not a check. */
    s.pixels = g_selftest_px;
    s.w = 320;
    s.h = 240;
    s.stride = 640;

    set.antialias = 1;
    set.grid_fit = 1;
    set.subpixel = 0;
    set.shaping = 1;
    set.darken = 0;
    set.stress = 0;

    /* The bar with nothing above it: the reference. */
    render_with_page(ui, &s, PAGE_SCRIPT, 1, &set, have_font, family);
    for (y = 0; y < 22; ++y)
    {
        for (x = 0; x < s.w; ++x)
        {
            g_bar_ref[y * 640 + x] = g_selftest_px[(s.h - 22 + y) * s.stride + x];
        }
    }

    /* The same bar under the tallest page, which overflows well past it. */
    render_with_page(ui, &s, PAGE_SCRIPT, 0, &set, have_font, family);
    for (y = 0; y < 22; ++y)
    {
        for (x = 0; x < s.w; ++x)
        {
            if (g_bar_ref[y * 640 + x] != g_selftest_px[(s.h - 22 + y) * s.stride + x])
            {
                strangers++;
            }
        }
    }

    if (strangers)
    {
        printf("FAIL  status bar: %ld pixel(s) of the page bled into it\n", (long)strangers);
        return 1;
    }
    printf("ok    status bar: the tallest page does not paint into it\n");
    return 0;
}

static int check_no_ghosting(ar_ctx *ui, int have_font, const char *family)
{
    static ar_u32 first[640 * 24];

    ar_surface      s;
    struct settings set;
    ar_rect         overlay, status_area;
    ar_i32          pass, y, x;
    int             bad = 0;

    s.pixels = g_selftest_px;
    s.w = 640;
    s.h = 480;
    s.stride = 640;

    set.antialias = 1;
    set.grid_fit = 1;
    set.subpixel = 0;
    set.shaping = 1;
    set.darken = 0;
    set.stress = 0;

    overlay = ar_rect_make(s.w - 296, 16, 296, 200);
    status_area = ar_rect_make(0, s.h - 24, s.w, 24);

    for (pass = 0; pass < 2; ++pass)
    {
        ar_input in;
        char     status[192];

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        strpool_reset();
        ar_frame_begin(ui, &in);
        ar_begin(ui, "div#app");
        ar_begin(ui, "div.page");
        ar_text(ui, "div.h1", PAGE_NAME[PAGE_CASCADE]);
        ar_text(ui, "div.sub", PAGE_SUB[PAGE_CASCADE]);
        page_body(ui, &s, PAGE_CASCADE, &set, have_font, family);
        ar_end(ui);
        ar_end(ui);

        ar_invalidate(ui, overlay);
        ar_invalidate(ui, status_area);
        ar_frame_end(ui, &s);

        ar_perf_overlay(ar_perf_of(ui), &s, ar_rect_make(0, 0, s.w, s.h), s.w - 296, 16, 1);
        /* A fixed string: the live counters differ between two frames for
           honest reasons, and this is asking about pixels, not numbers. */
        sprintf(status, "areole %s   page %s   ghosting check", ar_version(),
                PAGE_VER[PAGE_CASCADE]);
        ar_draw_text(&s, ar_rect_make(0, 0, s.w, s.h), 12, s.h - 18, status, 1, AR_HEX(0x8D8578));
        ar_frame_presented(ui);

        /* The status strip only. The overlay reports the previous frame's
           timings, so its pixels differ between two frames for an honest
           reason; the strip is drawn from a fixed string and must not. Both
           are repainted by the same two ar_invalidate calls, so the strip
           answers the question for both. */
        for (y = 0; y < 24; ++y)
        {
            for (x = 0; x < 640; ++x)
            {
                ar_i32 sy = s.h - 24 + y;
                ar_u32 px = g_selftest_px[sy * s.stride + x];

                if (pass == 0)
                {
                    first[y * 640 + x] = px;
                }
                else if (first[y * 640 + x] != px)
                {
                    bad++;
                }
            }
        }
    }

    if (bad)
    {
        printf("FAIL  ghosting: %d pixel(s) changed between two identical frames\n", bad);
        return 1;
    }
    printf("ok    ghosting: two identical frames are identical\n");
    return 0;
}

/*
 * The 0.5.0 page claims to cut an inline box across lines. This is the check
 * that it does, and that the pieces add up.
 *
 * ar_node_frag and ar_node_frag_count are the two inspection calls no
 * application was using -- the same gap that left ar_scrolled with no caller
 * and scrolling broken for a release. A fragment's documented invariant is
 * that the slices partition the text end to end, and an invariant nothing
 * exercises is a comment.
 */
static int check_fragments(ar_ctx *ui)
{
    ar_i32 i, n = ar_node_count(ui);
    ar_i32 split = -1;

    for (i = 0; i < n; ++i)
    {
        if (ar_node_frag_count(ui, i) > 1)
        {
            split = i;
            break;
        }
    }
    if (split < 0)
    {
        printf("FAIL  0.5.0  nothing on the page was cut across lines\n");
        return 1;
    }

    {
        ar_i32  k, count = ar_node_frag_count(ui, split);
        ar_i32  expect = 0;
        ar_rect whole = ar_node_rect(ui, split);
        ar_rect seen = ar_node_frag(ui, split, 0, 0, 0);

        for (k = 0; k < count; ++k)
        {
            ar_i32  from = -1, to = -1;
            ar_rect r = ar_node_frag(ui, split, k, &from, &to);

            if (from != expect)
            {
                printf("FAIL  0.5.0  fragment %ld starts at %ld, not where %ld ended\n", (long)k,
                       (long)from, (long)(k - 1));
                return 1;
            }
            if (to < from)
            {
                printf("FAIL  0.5.0  fragment %ld runs backwards\n", (long)k);
                return 1;
            }
            expect = to;
            seen = ar_rect_union(seen, r);
        }

        /* Every piece inside the box, and the box no larger than the pieces:
           ar_node_rect is documented as their union, which is what lets damage
           tracking and hit testing ignore fragments entirely. */
        if (seen.x != whole.x || seen.y != whole.y || seen.w != whole.w || seen.h != whole.h)
        {
            printf("FAIL  0.5.0  the box is not the union of its %ld fragments\n", (long)count);
            return 1;
        }
    }
    return 0;
}

static int selftest(ar_ctx *ui, int have_font, const char *family)
{
    ar_surface      s;
    struct settings set;
    ar_i32          page;
    int             bad = 0;

    s.pixels = g_selftest_px;
    s.w = 640;
    s.h = 480;
    s.stride = 640;

    set.antialias = 1;
    set.grid_fit = 1;
    set.subpixel = 0;
    set.shaping = 1;
    set.darken = 0;
    set.stress = 0;

    for (page = 0; page < PAGE_COUNT; ++page)
    {
        ar_input in;
        ar_u32   boxes;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;

        strpool_reset();
        ar_frame_begin(ui, &in);
        ar_begin(ui, "div#app");
        ar_begin(ui, "div.main");
        ar_begin(ui, "div.page");
        ar_text(ui, "div.h1", PAGE_NAME[page]);
        ar_text(ui, "div.sub", PAGE_SUB[page]);
        page_body(ui, &s, page, &set, have_font, family);
        ar_end(ui);
        ar_end(ui);
        ar_end(ui);
        ar_frame_end(ui, &s);
        ar_frame_presented(ui);

        boxes = ar_perf_of(ui)->cur.nodes;
        if (ar_unbalanced(ui))
        {
            printf("FAIL  %s  tree left unbalanced\n", PAGE_VER[page]);
            bad = 1;
        }
        /* A page that outgrows the box budget drops boxes and says nothing
           about it unless somebody asks, which until now nobody did. */
        else if (ar_overflowed(ui))
        {
            printf("FAIL  %s  tree did not fit the box budget\n", PAGE_VER[page]);
            bad = 1;
        }
        else if (page == 6 && check_fragments(ui))
        {
            bad = 1;
        }
        else if (boxes == 0)
        {
            printf("FAIL  %s  declared nothing\n", PAGE_VER[page]);
            bad = 1;
        }
        else
        {
            printf("ok    %s  %-22s %lu boxes\n", PAGE_VER[page], PAGE_NAME[page],
                   (unsigned long)boxes);
        }
    }
    if (ar_stylesheet_errors(ui))
    {
        printf("FAIL  stylesheet reports %lu problem(s)\n",
               (unsigned long)ar_stylesheet_errors(ui));
        bad = 1;
    }
    if (check_no_ghosting(ui, have_font, family))
    {
        bad = 1;
    }
    if (check_status_bar_is_clean(ui, have_font, family))
    {
        bad = 1;
    }
    printf("%s\n", bad ? "selftest FAILED" : "selftest passed");
    return bad;
}

/*
 * --dump: every box's rectangle, for comparison against a browser.
 *
 * Boxes are identified by their path through the tree -- "0/2/1" is the second
 * child of the third child of the root -- rather than by a selector. A path is
 * the one identifier both sides genuinely share: examples/02_tour/tour.html
 * declares the same tree with the same stylesheet, and tools/compare_layout.py
 * lines the two dumps up on it.
 *
 * The surface is 640x480 so the HTML can state the same viewport.
 */
static void dump_path(const ar_ctx *c, ar_i32 i, char *out)
{
    ar_i32 stack[32];
    ar_i32 depth = 0;
    ar_i32 at = i;
    char  *w = out;

    while (at >= 0 && depth < 32)
    {
        stack[depth++] = at;
        at = ar_node_parent(c, at);
    }
    while (depth > 0)
    {
        --depth;
        if (w != out)
        {
            *w++ = '/';
        }
        sprintf(w, "%ld", (long)ar_node_child_index(c, stack[depth]));
        while (*w)
        {
            ++w;
        }
    }
    *w = 0;
}

static void dump_page(ar_ctx *ui, ar_surface *s, ar_i32 page, struct settings *set, int have_font,
                      const char *family)
{
    ar_input in;
    ar_i32   i;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    strpool_reset();
    ar_frame_begin(ui, &in);
    ar_begin(ui, "div#app");
    /* The same shell the window builds, minus the rail: `.page` is a row child
       of `.main`, so it is stretched rather than sized to its content, and the
       dump describes the layout the application actually gets. */
    ar_begin(ui, "div.main");
    ar_begin(ui, "div.page");
    ar_text(ui, "div.h1", PAGE_NAME[page]);
    ar_text(ui, "div.sub", PAGE_SUB[page]);
    page_body(ui, s, page, set, have_font, family);
    ar_end(ui);
    ar_end(ui);
    ar_end(ui);
    ar_frame_end(ui, s);
    ar_frame_presented(ui);

    printf("# page %s %s\n", PAGE_VER[page], PAGE_NAME[page]);
    for (i = 0; i < ar_node_count(ui); ++i)
    {
        char    path[128];
        ar_rect r = ar_node_rect(ui, i);

        dump_path(ui, i, path);
        printf("%s %ld %ld %ld %ld |%s\n", path, (long)r.x, (long)r.y, (long)r.w, (long)r.h,
               ar_node_text(ui, i));
    }
}

static void dump_all(ar_ctx *ui, int have_font, const char *family)
{
    ar_surface      s;
    struct settings set;
    ar_i32          page;

    s.pixels = g_selftest_px;
    s.w = 640;
    s.h = 480;
    s.stride = 640;

    set.antialias = 1;
    set.grid_fit = 1;
    set.subpixel = 0;
    set.shaping = 1;
    set.darken = 0;
    set.stress = 0;

    printf("# areole %s  viewport %ld %ld  face %s\n", ar_version(), (long)s.w, (long)s.h, family);
    for (page = 0; page < PAGE_COUNT; ++page)
    {
        dump_page(ui, &s, page, &set, have_font, family);
    }
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

/* Tried in order. The first that exists and parses wins, and if none do the
   bitmap face keeps working. */
static const char *FACES[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                              "C:/Windows/Fonts/calibri.ttf", "C:/Windows/Fonts/tahoma.ttf"};

/* A second face for the scripts the first one will not have. Arial has
   Arabic and Hebrew; Devanagari lives in its own file on Windows. */
static const char *FALLBACKS[] = {"C:/Windows/Fonts/nirmala.ttf", "C:/Windows/Fonts/mangal.ttf",
                                  "C:/Windows/Fonts/arial.ttf"};

int main(int argc, char **argv)
{
    ar_ctx     *ui;
    ar_win     *win;
    ar_surface *s;
    ar_rect     overlay;
    ar_i32      region;
    ar_i32      page = PAGE_CORE;
    ar_i32      i;
    int         have_font = 0;
    char        family[64];

    struct settings set;

    set.antialias = 1;
    set.grid_fit = 1;
    set.subpixel = 0;
    set.shaping = 1;
    set.darken = 0;
    set.stress = 0;

    strcpy(family, "built-in bitmap");

    ui = ar_init(g_memory, (ar_u32)sizeof g_memory);
    if (!ui)
    {
        printf("not enough memory for a context\n");
        return 1;
    }

    ar_stylesheet(ui, SHEET_RESET);
    ar_stylesheet(ui, SHEET_FRAME);
    ar_stylesheet(ui, SHEET_STATUS);
    ar_stylesheet(ui, SHEET_NAV);
    ar_stylesheet(ui, SHEET_TEXT);
    ar_stylesheet(ui, SHEET_ROWS);
    ar_stylesheet(ui, SHEET_TOGGLE);
    ar_stylesheet(ui, SHEET_CASCADE);
    ar_stylesheet(ui, SHEET_STRUCTURE);
    ar_stylesheet(ui, SHEET_FLOW);
    ar_stylesheet(ui, SHEET_POS);
    ar_stylesheet(ui, SHEET_POS2);
    ar_stylesheet(ui, SHEET_SCROLL2);
    ar_stylesheet(ui, SHEET_SNAP);
    ar_stylesheet(ui, SHEET_SAFE);
    ar_stylesheet(ui, SHEET_ENV);
    ar_stylesheet(ui, SHEET_LAYER);
    ar_stylesheet(ui, SHEET_ANCH);
    ar_stylesheet(ui, SHEET_ANCH2);
    ar_stylesheet(ui, SHEET_TABLE_A);
    ar_stylesheet(ui, SHEET_TABLE_B);
    ar_stylesheet(ui, SHEET_T71_A);
    ar_stylesheet(ui, SHEET_T71_B);
    ar_stylesheet(ui, SHEET_F80_A);
    ar_stylesheet(ui, SHEET_F80_B);
    /* Written last so it can override, which is what the 0.1.0 page's
       swatches are: six boxes differing only in the colour a rule gives
       them. */
    ar_stylesheet(ui, ".sw { background:#c2703d; }"
                      ".swatch:nth-child(even) { background:#7a4a2a; }"
                      ".b-narrow { width:120px; }"
                      ".b-wide   { width:grow; }");
    if (ar_stylesheet_errors(ui))
    {
        printf("stylesheet has %lu problem(s)\n", (unsigned long)ar_stylesheet_errors(ui));
        return 1;
    }

    /*
     * What this backend knows about the display, for the 0.6.2 page.
     *
     * The titlebar rectangle is reported, so `env(titlebar-area-*)` resolves to
     * it. The safe-area insets deliberately are not: a windowed desktop has
     * nothing covering its edges, and leaving them unreported is what makes the
     * fallback path visible beside the reported one.
     *
     * `cover` rather than the default `auto`, and it changes nothing here. Under
     * `auto` the layout viewport is the safe rectangle and `env(safe-area-*)`
     * reports zero whatever the backend knows, because the stylesheet has
     * already been kept clear of the insets -- so the page would have nothing to
     * show. A real window's insets are zero either way, so asking for the whole
     * display costs this tour no pixels and buys the demonstration.
     */
    ar_set_titlebar_area(ui, 0, 0, 120, 28);
    ar_set_viewport_fit_cover(ui, 1);

    /* The font, before the first frame: a frame reserves the whole box budget
       from the other end of the arena and does not give it back until the
       next ar_frame_begin. */
    for (i = 0; i < (ar_i32)(sizeof FACES / sizeof FACES[0]) && !have_font; ++i)
    {
        ar_u32 n = read_file(FACES[i], g_font, (ar_u32)sizeof g_font);
        if (n && ar_font_load(ui, g_font, n, ATLAS_BYTES, MAX_PX))
        {
            have_font = 1;
            ar_font_family(ui, 0, family, (ar_i32)sizeof family);
        }
    }
    if (have_font)
    {
        for (i = 0; i < (ar_i32)(sizeof FALLBACKS / sizeof FALLBACKS[0]); ++i)
        {
            ar_u32 n = read_file(FALLBACKS[i], g_fallback, (ar_u32)sizeof g_fallback);
            if (n && ar_font_add(ui, g_fallback, n))
            {
                break;
            }
        }
        ar_font_antialias(ui, set.antialias);
        ar_font_grid_fit(ui, set.grid_fit);
        ar_font_shaping(ui, set.shaping);
    }

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
    {
        return selftest(ui, have_font, family);
    }
    if (argc > 1 && strcmp(argv[1], "--dump") == 0)
    {
        dump_all(ui, have_font, family);
        return 0;
    }

    win = ar_win_open("areole - the tour", WIN_W, WIN_H);
    if (!win)
    {
        printf("could not open a window\n");
        return 1;
    }

    ar_set_clock(ui, ar_time_us);

    printf("areole %s, clock: %s, faces: %ld (%s)\n", ar_version(), ar_time_source(),
           (long)ar_font_count(ui), family);

    while (ar_win_pump(win))
    {
        s = ar_win_surface(win);

        strpool_reset();
        ar_frame_begin(ui, ar_win_input(win));

        ar_begin(ui, "div#app");
        ar_begin(ui, "div.main");

        ar_begin(ui, "div.rail");
        ar_text(ui, "div.brand", "areole");
        ar_text(ui, "div.tagline", "one release per page");
        for (i = 0; i < PAGE_COUNT; ++i)
        {
            if (ar_button(ui, i == page ? "div.nav-on" : "div.nav", PAGE_VER[i]))
            {
                page = i;
                /* The whole page changes, and areole cannot see that coming
                   from a button press alone. */
                ar_invalidate_all(ui);
            }
            ar_text(ui, "div.navsub", PAGE_NAME[i]);
        }
        ar_end(ui);

        ar_begin(ui, "div.page");
        ar_text(ui, "div.h1", PAGE_NAME[page]);
        ar_text(ui, "div.sub", PAGE_SUB[page]);

        page_body(ui, s, page, &set, have_font, family);
        ar_end(ui);

        ar_end(ui); /* .main */

        /* The counters describe the frame that has just been declared, so the
           bar reports the previous one -- the same trade the overlay makes,
           and for the same reason. */
        ar_begin(ui, "div.status");
        ar_text(ui, "div.statustext",
                fmt("areole %s   page %s   %ld boxes   %ld region(s)   %s", ar_version(),
                    PAGE_VER[page], (long)ar_perf_of(ui)->cur.nodes, (long)ar_damage_count(ui),
                    ar_time_source()));
        ar_end(ui);

        ar_end(ui); /* #app */

        /*
         * The overlay is drawn over the frame and changes every frame, so its
         * rectangle is marked dirty BEFORE ar_frame_end and areole repaints
         * the interface underneath it.
         *
         * Without this it composites over its own previous output. Damage
         * tracking has no reason to repaint a region where no box changed, and
         * the overlay's panel is deliberately translucent -- so it darkens
         * frame after frame instead of sitting on a clean background.
         */
        overlay = ar_rect_make(s->w - 296, 16, 296, 200);
        ar_invalidate(ui, overlay);

        ar_frame_end(ui, s);

        /*
         * The inline axis, driven from here.
         *
         * Nothing in areole binds the wheel sideways yet, so this is what an
         * application does for a keyboard, a swipe or a shift-wheel -- and it
         * is the reason ar_node_scroll_to_x is public. The strip is found by
         * asking which box has somewhere to go horizontally rather than by
         * counting boxes, so adding a paragraph to the page cannot break it.
         */
        if (page == PAGE_SCROLL)
        {
            const ar_input *raw = ar_win_input(win);
            ar_i32          by = raw->wheel_px ? raw->wheel_px : raw->wheel * 30;
            ar_i32          k;

            for (k = 0; by != 0 && k < ar_node_count(ui); ++k)
            {
                if (ar_node_scroll_range_x(ui, k) > 0 &&
                    ar_rect_contains(ar_node_rect(ui, k), raw->mouse_x, raw->mouse_y))
                {
                    g_slide = ar_node_scroll_to_x(ui, k, g_slide - by);
                    break;
                }
            }
        }

        ar_perf_overlay(ar_perf_of(ui), s, ar_rect_make(0, 0, s->w, s->h), s->w - 296, 16, 1);

        for (region = 0; region < ar_damage_count(ui); ++region)
        {
            ar_win_present(win, ar_damage_rect(ui, region));
        }
        ar_win_present(win, overlay);
        ar_frame_presented(ui);

        /* Hover and a wheel notch both settle after the paint, and this pump
           blocks when nothing is happening, so the frame that discovers either
           has to ask for one more in order to show it. ar_needs_redraw covers
           every such reason, which is why this is one condition and not a list
           the caller has to keep up to date. */
        if (ar_needs_redraw(ui))
        {
            ar_win_wake(win);
        }
    }

    ar_win_close(win);
    return 0;
}
