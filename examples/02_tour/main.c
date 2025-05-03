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
    PAGE_COUNT
};

static const char *PAGE_VER[PAGE_COUNT] = {"0.1.0", "0.1.1", "0.1.2", "0.2.0",
                                           "0.3.0", "0.4.0", "0.5.0", "0.6.0"};

static const char *PAGE_NAME[PAGE_COUNT] = {
    "One block, one blit", "The counters",     "Damage tracking",    "Outlines", "Scripts",
    "The cascade",         "Block and inline", "Position and scroll"};

static const char *PAGE_SUB[PAGE_COUNT] = {
    "No allocator, no graphics API, no coordinates in the C file.",
    "Every phase timed, every box counted, read off a real clock.",
    "Only what changed is presented. Watch the regions move.",
    "TrueType glyphs rasterized to a coverage buffer, integer end to end.",
    "UAX #9, OpenType GSUB and GPOS, Arabic joining, Indic reordering.",
    "Inheritance, combinators, !important, and the structural selectors.",
    "Margin collapsing, floats, line boxes and fragmented inlines.",
    "Absolute, fixed, sticky, z-index, and a list you can scroll."};

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
