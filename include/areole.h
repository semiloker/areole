/*
 * areole - a GUI library in strict C89 with no graphics API.
 *
 * https://github.com/semiloker/areole
 * SPDX-License-Identifier: MIT
 *
 * This header is the portable core. It must never include a platform header
 * and must always compile under -std=c89 -pedantic-errors. Platform backends
 * live behind their own headers, such as areole_win32.h.
 */
#ifndef AREOLE_H
#define AREOLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define AR_VERSION_MAJOR 0
#define AR_VERSION_MINOR 6
#define AR_VERSION_PATCH 1

/* Names the release that has landed, bumped when the next one does -- which is
   exactly the discipline that failed here: this said 0.1.0-dev through 0.1.1,
   0.1.2, 0.2.0, 0.3.0 and 0.4.0, because nothing reads it except a status line
   nobody was checking. It then said 0.5.0-dev through 0.6.0 for the same
   reason. The benchmark stamps it into every published number, so a stale one
   mislabels the measurements as well as the status line.

   It then did it a third time, saying 0.6.0-dev while 0.6.1 was landing, so the
   discipline is no longer relied on: CMakeLists.txt parses this line rather
   than carrying its own copy, which had drifted all the way back to 0.1.0.
   One place to edit is the only version of this that has ever held. */
#define AR_VERSION_STRING "0.6.1-dev"

/* ------------------------------------------------------------------------
 * Fixed width types
 *
 * C89 has no <stdint.h>. These are the widths areole assumes. The negative
 * array sizes below are the standard C89 static assertion: if any assumption
 * is false on the target, the build fails here rather than silently
 * corrupting a pixel buffer.
 * ------------------------------------------------------------------------ */
typedef unsigned char  ar_u8;
typedef signed char    ar_i8;
typedef unsigned short ar_u16;
typedef signed short   ar_i16;
typedef unsigned int   ar_u32;
typedef signed int     ar_i32;

typedef char ar__widths_are_as_assumed[(sizeof(ar_u8) == 1 && sizeof(ar_u16) == 2 &&
                                        sizeof(ar_u32) == 4 && sizeof(ar_i32) == 4)
                                           ? 1
                                           : -1];

/* ------------------------------------------------------------------------
 * Compiler shims
 * ------------------------------------------------------------------------ */
#if defined(_MSC_VER)
#define AR_INLINE __inline
#elif defined(__GNUC__)
#define AR_INLINE __inline__
#else
#define AR_INLINE
#endif

/* Marks a parameter that is deliberately ignored, without tripping -Wextra. */
#define AR_UNUSED(x) ((void)(x))

/* ------------------------------------------------------------------------
 * Geometry and colour
 *
 * Colours are 0xAARRGGBB packed into one 32 bit word. On a little endian
 * machine that is byte order B, G, R, A in memory, which is exactly the
 * layout a 32 bit BI_RGB Windows DIB section expects. Matching the native
 * format is what keeps presentation to a straight copy with no per pixel
 * conversion in the driver.
 * ------------------------------------------------------------------------ */
typedef ar_u32 ar_color;

#define AR_RGB(r, g, b)                                                                            \
    ((ar_color)(0xFF000000u | ((ar_u32)(ar_u8)(r) << 16) | ((ar_u32)(ar_u8)(g) << 8) |             \
                (ar_u32)(ar_u8)(b)))

#define AR_RGBA(r, g, b, a)                                                                        \
    ((ar_color)(((ar_u32)(ar_u8)(a) << 24) | ((ar_u32)(ar_u8)(r) << 16) |                          \
                ((ar_u32)(ar_u8)(g) << 8) | (ar_u32)(ar_u8)(b)))

/* AR_HEX(0x1E1E1E) reads the same way the stylesheet does. */
#define AR_HEX(rgb) ((ar_color)(0xFF000000u | ((ar_u32)(rgb) & 0x00FFFFFFu)))

#define AR_ALPHA_OF(c) ((ar_u8)((c) >> 24))

typedef struct ar_rect
{
    ar_i32 x, y, w, h;
} ar_rect;

ar_rect ar_rect_make(ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h);
ar_rect ar_rect_intersect(ar_rect a, ar_rect b);
ar_rect ar_rect_union(ar_rect a, ar_rect b);
int     ar_rect_is_empty(ar_rect r);
int     ar_rect_contains(ar_rect r, ar_i32 x, ar_i32 y);

/* ------------------------------------------------------------------------
 * Surface
 *
 * A surface is somebody else's memory. On Windows it is the pixels handed
 * back by CreateDIBSection, so areole rasterizes straight into the buffer GDI
 * will blit and presenting costs one copy instead of two.
 *
 * stride is measured in pixels rather than bytes, because every path that
 * touches it indexes by pixel and converting at each use is pure noise.
 * ------------------------------------------------------------------------ */
typedef struct ar_surface
{
    ar_u32 *pixels;
    ar_i32  w, h;
    ar_i32  stride;
} ar_surface;

void ar_surface_clear(ar_surface *s, ar_color c);
void ar_fill_rect(ar_surface *s, ar_rect r, ar_rect clip, ar_color c);

/* ------------------------------------------------------------------------
 * Instrumentation
 *
 * Physical quantities the benchmark needs and cannot infer: how many pixels a
 * frame actually touched, how much of that went through the blend path rather
 * than the opaque one, how many glyphs were drawn.
 *
 * Off by default and compiled out entirely, because the counters live in the
 * fill routines and this library does not put anything in a hot path that a
 * shipping application pays for. Built with -DAR_INSTRUMENT the counters are
 * incremented once per call rather than once per pixel, so even then the cost
 * is two operations against a fill of several thousand.
 *
 * The measured overhead is published in bench/baseline.json rather than
 * asserted here.
 * ------------------------------------------------------------------------ */
typedef struct ar_counters
{
    ar_u32 fills;       /* ar_fill_rect calls that drew anything */
    ar_u32 fill_px;     /* pixels written by opaque fills        */
    ar_u32 blend_px;    /* pixels written through the blend path */
    ar_u32 glyphs;      /* glyphs whose bits were tested         */
    ar_u32 glyph_px;    /* pixels written by glyph ink           */
    ar_u32 text_calls;  /* ar_draw_text calls                    */
    ar_u32 clipped_out; /* draws rejected before touching memory */
} ar_counters;

/* Always present, so a caller need not compile conditionally. Without
   AR_INSTRUMENT every field stays zero, which is honest: the library did not
   measure, rather than measured nothing. */
ar_counters *ar_counters_get(void);
void         ar_counters_reset(void);

/* Non-zero when this build actually counts. */
int ar_counters_enabled(void);

/* ------------------------------------------------------------------------
 * Text
 *
 * One embedded 8x8 bitmap face, drawn at an integer scale. Glyphs are spaced
 * by their own ink width rather than a fixed cell, so text does not read as
 * monospaced; the widths were measured when the data was generated and cost
 * nothing here.
 *
 * ponytail: integer scaling of one bitmap face. It is crisp, it is free, and
 * free is the only thing that is genuinely available on the hardware this
 * targets. Smooth text at arbitrary sizes is the optional TrueType module in
 * v0.6.
 * ------------------------------------------------------------------------ */
ar_i32 ar_text_width(const char *text, ar_i32 scale);

/* The width of a byte range of it, which is what wrapping needs: a candidate
   line is a slice of the string, and copying it out to measure it would be an
   allocation this library does not make. */
ar_i32 ar_text_width_range(const char *text, ar_i32 from, ar_i32 to, ar_i32 scale);
ar_i32 ar_text_height(ar_i32 scale);
ar_i32 ar_text_line_height(ar_i32 scale);

void ar_draw_text(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *text, ar_i32 scale,
                  ar_color c);

/* ------------------------------------------------------------------------
 * Input
 *
 * A snapshot of the pointer for one frame. It lives in the core rather than in
 * a backend header because there is nothing platform specific about it, and a
 * second copy of the same five fields would drift.
 * ------------------------------------------------------------------------ */
#define AR_MOUSE_LEFT   0x01u
#define AR_MOUSE_RIGHT  0x02u
#define AR_MOUSE_MIDDLE 0x04u

typedef struct ar_input
{
    ar_i32 mouse_x, mouse_y;
    ar_u32 mouse_down;     /* held right now            */
    ar_u32 mouse_pressed;  /* went down since last pump */
    ar_u32 mouse_released; /* came up since last pump   */
    ar_i32 wheel;          /* notches, positive is away from the user */

    /*
     * The same travel in pixels, for a device that has more to say than
     * notches.
     *
     * A mouse wheel clicks: one notch, a fixed distance, and `wheel` describes
     * it exactly. A precision touchpad or a free-spinning wheel reports a
     * fraction of a notch at a time, and rounding that to whole notches is
     * either jerky or -- as it was here until recently -- silently nothing.
     *
     * A backend that knows only notches leaves this zero and the core falls
     * back to `wheel`, so nothing has to be updated to keep working. When both
     * are set this one wins, because it is strictly more information.
     */
    ar_i32 wheel_px;

    int mouse_inside; /* the cursor is over the client area */

    /*
     * Keys pressed since the last frame, as a bitmask of AR_KEY_*.
     *
     * Deliberately not a keyboard. This is the set of keys that scroll, and
     * nothing else: no text, no focus traversal, no caret, no IME, no modifier
     * tracking. Those are 0.10.0 and starting them here is how 0.6.1 would
     * quietly become 0.10.0.
     *
     * Presses rather than held state, because scrolling is an event: holding
     * Page Down should repeat at the rate the platform repeats keys, which is
     * a thing the platform already decides and the core has no business
     * second-guessing. A backend that repeats sets the bit again.
     */
    ar_u32 keys_pressed;
} ar_input;

/*
 * The keys that scroll.
 *
 * A platform-independent enum, because src/ never includes a platform header
 * and a virtual key code is the most platform-specific thing there is. Each
 * backend maps its own codes onto these.
 */
enum
{
    AR_KEY_UP = 1u << 0,
    AR_KEY_DOWN = 1u << 1,
    AR_KEY_LEFT = 1u << 2,
    AR_KEY_RIGHT = 1u << 3,
    AR_KEY_PAGE_UP = 1u << 4,
    AR_KEY_PAGE_DOWN = 1u << 5,
    AR_KEY_HOME = 1u << 6,
    AR_KEY_END = 1u << 7,

    /* Space pages down, and shift-space pages up in every browser. There is no
       modifier tracking here, so a backend that wants the second one sets
       AR_KEY_PAGE_UP itself -- which is the honest split: the platform knows
       about shift and this does not. */
    AR_KEY_SPACE = 1u << 8
};

/* ------------------------------------------------------------------------
 * Performance
 *
 * areole intends to publish numbers, so measuring is part of the library
 * rather than something bolted on for a blog post.
 *
 * The phases are reported separately on purpose. A single frame time hides
 * the one thing worth knowing on old hardware: whether the cost is the
 * rasterizer or the blit. Those have completely different fixes, and on a
 * machine with no 3D driver the blit is frequently the larger half.
 *
 * Averages are not offered. A UI that is smooth except for one stall every
 * two seconds has an excellent average and is unusable, which is why the
 * ring keeps the last AR_PERF_RING frames and reports percentiles.
 * ------------------------------------------------------------------------ */
#define AR_PERF_RING 256

typedef enum ar_phase
{
    AR_PHASE_STYLE = 0,
    AR_PHASE_LAYOUT,
    AR_PHASE_RASTER,
    AR_PHASE_PRESENT,
    AR_PHASE_COUNT
} ar_phase;

typedef struct ar_perf_frame
{
    ar_u32 phase_us[AR_PHASE_COUNT];
    ar_u32 total_us;
    ar_u32 nodes;
    ar_u32 glyphs;
    ar_u32 fills;
    ar_u32 dirty_px;
    ar_u32 arena_frame_bytes;
} ar_perf_frame;

typedef struct ar_perf
{
    ar_perf_frame cur;
    ar_perf_frame ring[AR_PERF_RING];
    ar_u32        count;   /* frames recorded, saturating at AR_PERF_RING */
    ar_u32        head;    /* next slot to write                          */
    ar_u32        frames;  /* frames recorded since reset, never wrapped  */
    ar_u32        started; /* timestamp of the current frame              */
    ar_u32        mark;    /* timestamp of the last phase boundary        */
} ar_perf;

void ar_perf_reset(ar_perf *p);
void ar_perf_begin(ar_perf *p, ar_u32 now_us);

/* Closes the named phase at now_us. Phases need not be reported in order and
   an unreported phase simply stays zero. */
void ar_perf_mark(ar_perf *p, ar_phase phase, ar_u32 now_us);
void ar_perf_end(ar_perf *p, ar_u32 now_us);

/* pct is 0 to 100. AR_PHASE_COUNT selects the total rather than a phase. */
ar_u32 ar_perf_percentile(const ar_perf *p, ar_phase phase, ar_u32 pct);
ar_u32 ar_perf_max(const ar_perf *p, ar_phase phase);

/* Draws the live readout. Costs one frame of its own, which is why the values
   shown are the previous frame: measuring the measurement is not useful. */
void ar_perf_overlay(ar_perf *p, ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, ar_i32 scale);

/* ------------------------------------------------------------------------
 * Context
 *
 * areole is given one block of memory at startup and never asks for another.
 * Everything lives in it: the parsed stylesheet, the per box state that has to
 * survive between frames, and the box tree, which is rebuilt from scratch every
 * frame and released with a single integer store.
 *
 * Size the block with AR_MEM. The per box figure is checked against the real
 * structure sizes by a compile time assertion, so it cannot drift as the
 * structures grow.
 * ------------------------------------------------------------------------ */
typedef struct ar_ctx ar_ctx;

/* ------------------------------------------------------------------------
 * How far a container may be scrolled, and what that costs
 *
 * A scroll container remembers a position on each axis, and those two numbers
 * live in the per-box state table -- so their width is paid by every box in the
 * interface, scrollable or not, because the table has one slot per box.
 *
 * Define AR_SCROLL_COMPACT as 1 to keep them in sixteen bits. That caps a
 * scroll at 32,767 pixels and gives eight bytes per box back. Whether that
 * matters is a property of the machine and not of the library, which is why it
 * is a switch rather than a decision made here:
 *
 *   - 32 bit, the default. Nothing is capped. A million-row table scrolls to
 *     the end.
 *   - 16 bit. 32,767 pixels is about seven hundred rows of a list at 46 px, or
 *     a page forty times the height of a 800x600 screen. Past that the
 *     container simply stops, which is a visible limit rather than a wrong
 *     rendering -- ar_scroll_clamp enforces it, so nothing wraps or corrupts.
 *
 * The cap is on the scroll *offset*, not on document height: content below it
 * still lays out and paints, it just cannot be reached by scrolling.
 * ------------------------------------------------------------------------ */
#ifndef AR_SCROLL_COMPACT
#define AR_SCROLL_COMPACT 0
#endif

#if AR_SCROLL_COMPACT
typedef ar_i16 ar_scroll_pos;
#define AR_SCROLL_LIMIT 32767
#else
typedef ar_i32 ar_scroll_pos;
#define AR_SCROLL_LIMIT 1073741823
#endif

/* 320 -> 336 when block formatting gave every box two collapsed margins, and
   336 -> 352 when float and clear were added, 352 -> 368 when a box gained the
   two indices that say where its fragments are, and 368 -> 400 to reserve one
   fragment per box -- which is not how fragments actually scale, since a
   twenty-line paragraph is one box and twenty of them, but it is a budget and
   running out of it costs a split rather than anything worse. 400 -> 408 for
   the paint order, one index per box. A property costs every box
   and every rule five bytes, in ar_style. The assertions in ar_ctx.c are what
   noticed each time; they are there so this number cannot quietly stop being
   true.

   408 -> 416 for the second scroll axis, and only in the default build. The
   width a box's contents came to fitted in padding the node already had, so
   the whole cost is the horizontal scroll position in the slot -- which
   AR_SCROLL_COMPACT removes by halving both positions. Compact builds still
   fit 408, so on that setting horizontal scrolling costs nothing per box.

   416 -> 440 for the top layer and anchor positioning: `overlay`, `inert` and
   `position-try` are a keyword each, and `anchor-name` and `position-anchor`
   are thirty-two bits each because they hold a hash. Narrowing those two to
   sixteen was the alternative and was refused: two anchor names colliding
   would silently attach a popover to the wrong box, which is a worse failure
   than a larger struct and an invisible one. Measured at 428 of 440 rather
   than guessed, so there are twelve bytes left before this has to move again.

   440 -> 456 for tables. Five properties -- table-layout, border-collapse,
   border-spacing, colspan and rowspan -- and the sixty-fifth property, which
   pushed the property mask from two words to three. That last one is the
   expensive half: the mask is carried by every style, and a style is carried
   by every box *and* every rule, so widening it cost four bytes in each of
   two places per box. Measured at 444 of 456, so twelve bytes again.

   colspan and rowspan are HTML attributes rather than CSS, and paying per-box
   style bytes for them is a real cost of having no attributes yet. The
   alternative was an ar_span() call writing into a side array, and the price
   of not taking it is worth stating rather than implying: those two are the
   sixty-fifth and sixty-sixth properties, so dropping them would have left
   AR_P_COUNT at 64 -- exactly what two mask words hold. That is 8 bytes of
   style per box, 8 more per rule across 256 rules, and the whole 8 KB of
   AR_MEM_FIXED below. It was paid so that a stylesheet can set a span.

   456 -> 464, for `visibility`, `caption-side` and `empty-cells` at 0.7.1.
   Measured, not guessed: ar_node went 400 -> 408 bytes and the assertion
   below fired at 460 against 456. Three narrow properties are six bytes of
   value and three of unit, which alignment rounds to eight.

   `visibility` is the one that had to be a property. It inherits, and that is
   the whole of what makes `collapse` on a row useful -- the row goes and every
   cell in it goes too, without any of them being named -- so it cannot be a
   call into the context the way a span could have been. The other two ride
   along in the same eight bytes: the alignment had already been paid.

   464 -> 488, for the seven properties real flexbox needs at 0.8.0:
   flex-wrap, flex-basis, flex-grow, flex-shrink, align-self, align-content
   and order. Measured: ar_node went 408 -> 432 and the assertion below fired
   at 484. areole shipped a flexbox *subset* -- direction, gap, justify, align
   and a `grow` keyword that is not CSS -- and this is what the rest of it
   costs. Anything written against real CSS uses these, so the alternative was
   not a smaller struct, it was laying those stylesheets out wrongly.

   The assertions in ar_ctx.c are what noticed every one of these; they are
   there so this number cannot quietly stop being true. */
#if AR_SCROLL_COMPACT
#define AR_BYTES_PER_BOX 480u
#else
#define AR_BYTES_PER_BOX 488u
#endif

/*
 * The part of the block that does not scale with the box count: the context
 * itself, the rule table, the resolved-style cache.
 *
 * Raised from 96 KB to 128 KB when selectors gained combinators. A rule now
 * carries up to three context parts and is 292 bytes, so 256 of them are 73 KB
 * on their own, and the total came to 100,280 -- two kilobytes over. The number
 * is checked by a compile-time assertion against the real structure sizes
 * rather than trusted, which is why this was a build failure and not a
 * corruption.
 *
 * 160 KB -> 168 KB for tables, and by two hundred bytes. A rule is 516 bytes
 * now, so the rule table alone is 129 KB of it; the property mask going from
 * two words to three is what tipped it over, because a rule carries two of
 * them -- the properties it sets and the ones it marked important. Measured at
 * 164,040 of 172,032.
 *
 * 168 KB -> 176 KB for the rest of flexbox at 0.8.0, and again mostly the rule
 * table: ar_style went 236 -> 260 bytes for seven properties, a rule went
 * 520 -> 544, and 256 of them is 139,264. Measured: 15,120 of context, 139,264
 * of rules, 17,664 of style cache and the 1,024 of slack the assertion carries,
 * which is 173,072 of 180,224.
 */
#define AR_MEM_FIXED  180224u
#define AR_MEM(boxes) (AR_MEM_FIXED + (ar_u32)(boxes) * AR_BYTES_PER_BOX)

/* Returns NULL if the block is too small to be useful. */
ar_ctx *ar_init(void *mem, ar_u32 size);

/* Parses a stylesheet into the context. Call it as many times as you like at
   startup; each call appends. Never call it per frame: the whole point is that
   parsing happens once and the frame only resolves. */
void ar_stylesheet(ar_ctx *c, const char *css);

/* Non-zero if the stylesheet had anything wrong with it. Parsing never aborts,
   so this is the only way to find out. */
ar_u32 ar_stylesheet_errors(const ar_ctx *c);

/* ------------------------------------------------------------------------
 * Fonts
 *
 * Without a face loaded, text is drawn with the built-in 8x8 bitmap font,
 * which needs no file and no memory and is why a hello-world build is 52 KB.
 * Loading a TrueType face switches every subsequent frame to outlines.
 *
 * The font data is not copied. It must outlive the context -- mapping the file
 * and leaving it mapped is the intended use, and is why the parser never
 * allocates.
 *
 * Both atlas_bytes and the rasterizer scratch come out of the block handed to
 * ar_init, and there is a wrinkle worth stating plainly: ar_init derives its
 * box budget from the size of that block, so about 15% of any extra memory is
 * taken by per-box state before the font sees it. Size the block as
 *
 *     AR_MEM(boxes) + (atlas_bytes + scratch) * 6 / 5
 *
 * where scratch is roughly 36 KB plus (max_px + 2) * max_px * 4 bytes.
 *
 * max_px is the largest text size that will be drawn. The rasterizer's
 * accumulator grows with its square -- 48 px costs 9 KB, 128 px costs 66 KB --
 * so it is asked for rather than assumed. A glyph larger than this is skipped
 * rather than drawn wrong.
 *
 * Call it at startup, before the first frame, for the same reason
 * ar_stylesheet says so: a frame reserves the whole box budget from the other
 * end of the arena and does not release it until the next ar_frame_begin, so
 * the room available afterwards is a fraction of what it was.
 *
 * Returns zero if the data is not a readable TrueType file or the arena has no
 * room, and in both cases the bitmap font keeps working -- a missing font
 * should degrade an interface, not stop it.
 * ------------------------------------------------------------------------ */
int ar_font_load(ar_ctx *c, const void *data, ar_u32 size, ar_u32 atlas_bytes, ar_i32 max_px);
int ar_font_loaded(const ar_ctx *c);

/*
 * Adds a fallback face. No single face covers Unicode: one chosen for Latin
 * will not have CJK, and asking it for a Japanese character gives glyph 0 --
 * the notdef box, the tofu everyone recognises and nobody wants.
 *
 * Each character is drawn by the first face in the chain that has it, so the
 * primary face wins wherever it can and a document keeps looking like one
 * typeface rather than a ransom note. Up to three fallbacks after the primary.
 *
 * Costs no memory beyond the ar_face itself: the chain shares one glyph cache,
 * which carries the face index in its key.
 */
int ar_font_add(ar_ctx *c, const void *data, ar_u32 size);

/* How many faces are in the chain, and the family name of one of them, which
   is what a log needs to say which face a character actually came from. */
ar_i32 ar_font_count(const ar_ctx *c);
ar_i32 ar_font_family(const ar_ctx *c, ar_i32 index, char *out, ar_i32 cap);

/*
 * Antialiased text is the default. Turning it off thresholds each glyph's
 * coverage at half when it is rasterized, so every pixel is either fully inked
 * or untouched.
 *
 * It is worth having for two reasons. Blending costs a read and a write per
 * pixel where an opaque store costs one write, and on the machines this
 * library targets memory bandwidth is the entire problem: measured on twelve
 * lines of 14 px Segoe UI, turning it off is 1.89x faster, 116 ns per glyph
 * against 219. And hard edges are what those machines actually looked like, so
 * this is a legitimate choice about appearance and not only a cheap one.
 *
 * Changing it drops the glyph cache and invalidates the window, so it is a
 * setting rather than something to toggle per frame.
 */
void ar_font_antialias(ar_ctx *c, int on);

/*
 * Vertical grid fitting, on by default.
 *
 * Unhinted text at interface sizes looks soft for one specific reason: the
 * x-height rarely lands on a pixel boundary, so the flat top of every lower
 * case letter spreads across two rows at half coverage. This scales each glyph
 * vertically -- by a few per cent, invisible as a shape -- so that it does.
 *
 * It is not a bytecode interpreter. Aligning stem widths horizontally as well
 * needs to know which contours are stems, which is the expensive part and is
 * not done. This is the cheap majority of the benefit, and it declines to act
 * at all when the correction would exceed an eighth, which is where fitting
 * starts distorting a face rather than sharpening it.
 */
void ar_font_grid_fit(ar_ctx *c, int on);

/*
 * Stem darkening, 0 to 255, off by default.
 *
 * A stem one pixel wide that straddles two columns leaves both at half
 * coverage, and the letter reads grey rather than black. This lifts midtone
 * coverage to put the weight back, leaving 0 and 255 untouched so a stem that
 * already lands on the grid is unaffected.
 *
 * Off by default because it is a taste decision, and the honest starting point
 * is the outline as its designer drew it. 40 to 80 is a reasonable range for
 * small text on a low-resolution screen.
 */
void ar_font_darken(ar_ctx *c, ar_i32 amount);

/*
 * Horizontal subpixel positioning, on by default.
 *
 * Off, every glyph starts on a whole pixel and the fractional part of the pen
 * is discarded, so the gaps between letters come out uneven and a word looks
 * as though someone nudged each letter by hand. On, each glyph is rasterized
 * at four quarter-pixel offsets and the right one is chosen.
 *
 * The cost is four times the cache entries and nothing per frame. Turn it off
 * where the atlas budget is tighter than the typography matters -- which on a
 * 64 MB machine it may well be.
 */
void ar_font_subpixel(ar_ctx *c, int on);

/*
 * Ligatures and kerning, on whenever the face carries the tables.
 *
 * A string of characters is not a list of glyphs: fi is one glyph in most
 * serif faces, and the gap between A and V is not the gap between n and n. A
 * font says so in GSUB and GPOS, and a renderer that ignores them produces
 * text that reads as having been set by a program.
 *
 * Turning it off is a performance choice rather than a typographic one. It
 * saves a pass over each run and the buffer that pass needs.
 */
void ar_font_shaping(ar_ctx *c, int on);

/* Cache hits, misses and resets since the face was loaded. A rising reset
   count means the atlas is too small for the interface and glyphs are being
   rasterized repeatedly; pass more atlas_bytes. Any argument may be null. */
void ar_font_cache_stats(const ar_ctx *c, ar_u32 *hits, ar_u32 *misses, ar_u32 *resets);

/* Resolved-style cache hits and misses since init. Two boxes with the same
   tag, class, id and state cannot resolve differently, so the cache is keyed
   on exactly that and holds one entry per distinct combination rather than one
   per box -- a thousand cards sharing a class occupy one entry.

   A hit rate that is not close to one means an interface with more distinct
   selector-and-state combinations than the table holds, and the frame is
   paying a full rule scan per box for the surplus. Either pass NULL, or read
   it and know. */
void ar_style_cache_stats(const ar_ctx *c, ar_u32 *hits, ar_u32 *misses);

/* Lends the context a microsecond clock so it can time its own phases. The
   core deliberately has no clock of its own; areole_win32.h supplies
   ar_time_us for this. Without one, the phase breakdown reads zero and
   everything else still works. */
void ar_set_clock(ar_ctx *c, ar_u32 (*clock_us)(void));

void ar_frame_begin(ar_ctx *c, const ar_input *in);

/* Opens a box. The selector is the same syntax the stylesheet uses:
   "div", ".card", "#sidebar", or any combination such as "div.card#first". */
void ar_begin(ar_ctx *c, const char *selector);
void ar_end(ar_ctx *c);

/* A leaf box containing text. */
void ar_text(ar_ctx *c, const char *selector, const char *text);

/* Returns non-zero on the frame the button is released, having been pressed
   on the same box. */
int ar_button(ar_ctx *c, const char *selector, const char *label);

/* Lays the tree out and paints it into the surface. Returns the region that
   was drawn, for the caller to present.

   That region is usually much smaller than the surface. areole repaints only
   what changed, so a frame in which nothing changed returns an empty rectangle
   and touches no pixels at all. Present exactly what is returned: presenting
   the whole window instead is correct but throws away most of the saving,
   because on the machines this library targets the blit costs as much as the
   rasterizer. */
ar_rect ar_frame_end(ar_ctx *c, ar_surface *s);

/* ------------------------------------------------------------------------
 * Damage
 *
 * The library invalidates by itself when hover, focus or active state
 * changes, when a box moves or resizes, when a box appears or disappears, and
 * when a box's resolved style comes out different. That covers everything
 * areole can see.
 *
 * It cannot see the application's own data. A label whose text changed, a
 * value redrawn from a sensor, a list that gained a row: call ar_invalidate
 * with the affected rectangle, or ar_invalidate_all when it is easier and the
 * frame budget allows.
 *
 * Over-invalidating costs performance. Under-invalidating leaves stale pixels
 * on screen, so when in doubt, invalidate more.
 * ------------------------------------------------------------------------ */
void ar_invalidate(ar_ctx *c, ar_rect r);
void ar_invalidate_all(ar_ctx *c);

/* The damaged region, as up to AR_DAMAGE_RECTS separate rectangles.
   ar_frame_end returns their bounding box, which is all a simple backend
   needs. Presenting them individually is what stops two changes in opposite
   corners costing a whole-window blit -- on a Pentium II that is 7.7 ms
   against 0.01 ms, so it is worth the loop.

       for (i = 0; i < ar_damage_count(ui); ++i)
           present(ar_damage_rect(ui, i));

   Valid between ar_frame_end and the next ar_frame_begin. */
#define AR_DAMAGE_RECTS 8

ar_i32  ar_damage_count(const ar_ctx *c);
ar_rect ar_damage_rect(const ar_ctx *c, ar_i32 i);

/* Whether the frame in progress will paint anything. Valid between
   ar_frame_begin and ar_frame_end. */
int ar_frame_is_dirty(const ar_ctx *c);

/* Closes the frame, after the caller has put the surface on screen. Separate
   from ar_frame_end because presenting belongs to the backend, and timing the
   blit is the whole reason the phases are split. */
void ar_frame_presented(ar_ctx *c);

/* Diagnostics. All three should be zero in a healthy frame. */
int ar_overflowed(const ar_ctx *c);
int ar_unbalanced(const ar_ctx *c);

/* Non-zero when the next frame will differ from the one just drawn: the box
   under the cursor changed, or a wheel notch moved a scroll container. Both
   settle after the paint, so a caller whose pump blocks when idle must draw one
   more frame to show either. Ask this and nothing else; ar_scrolled is for a
   caller that wants to know which of the two it was. */
int ar_needs_redraw(const ar_ctx *c);

/* ------------------------------------------------------------------------
 * Scrolling
 *
 * A box with `overflow: scroll` or `auto` keeps a scroll position between
 * frames -- the only piece of layout state areole keeps, because it is the
 * only one that belongs to the user rather than to the stylesheet.
 *
 * A wheel notch is applied after the frame is laid out, so it lands on the
 * next one. That is the same trade hover makes, for the same reason, and
 * ar_needs_redraw covers both.
 * ------------------------------------------------------------------------ */
ar_i32 ar_node_scroll(const ar_ctx *c, ar_i32 i);
ar_i32 ar_node_scroll_range(const ar_ctx *c, ar_i32 i);

/* The same three on the inline axis. A container scrolls sideways when
   overflow-x resolves to scroll or auto -- which includes the case where only
   overflow-y was stated, since a lone visible on the other axis becomes auto.
   AR_KEY_LEFT and AR_KEY_RIGHT drive it, and so does this call. There is still
   no horizontal scrollbar to drag and no wheel binding, because nothing here
   generates an inline wheel event. */
ar_i32 ar_node_scroll_x(const ar_ctx *c, ar_i32 i);
ar_i32 ar_node_scroll_range_x(const ar_ctx *c, ar_i32 i);
ar_i32 ar_node_scroll_to_x(ar_ctx *c, ar_i32 i, ar_i32 x);

/* Moves a container, clamped to its range. For a keyboard, a scrollbar drag,
   or scrolling something into view. Returns where it ended up. */
ar_i32 ar_node_scroll_to(ar_ctx *c, ar_i32 i, ar_i32 y);

/*
 * Brings a box into view by scrolling its nearest scrollable ancestor.
 *
 * Honours `scroll-margin` on the box and `scroll-padding` on the container, so
 * a sticky header does not end up covering the thing that was just scrolled
 * to -- which is the entire reason those two properties exist.
 *
 * Scrolls the minimum distance: a box already fully visible does not move, and
 * one below the fold comes to the bottom rather than jumping to the top. That
 * is what a reader expects and what "nearest" means in the CSS of the same
 * name.
 *
 * Returns 1 if anything moved. Only the block axis, matching the rest of the
 * scrolling here.
 */
int ar_node_scroll_into_view(ar_ctx *c, ar_i32 i);

/* Whether a wheel notch moved anything this frame, so a caller whose pump
   blocks knows to ask for the frame that shows it. */
int ar_scrolled(const ar_ctx *c);

/* ------------------------------------------------------------------------
 * The display the backend is drawing on
 *
 * Two things the core cannot work out for itself and will not guess: how much
 * of the display is covered by something the window does not control, and
 * where the titlebar sits when the backend draws its own. A stylesheet reaches
 * them through `env()`.
 *
 * A backend that says nothing leaves them unknown, and every `env()` naming
 * one takes its fallback. That is not the same as reporting zero: a windowed
 * desktop has real insets of zero, so `env(safe-area-inset-top, 20px)` must
 * resolve to 0 there and to 20 on a backend that has never heard of the idea.
 * ------------------------------------------------------------------------ */

/* The four safe-area insets, in pixels. Calling this at all marks them known,
   so a backend that means "zero on every side" should call it with zeroes
   rather than not call it. */
void ar_set_safe_area(ar_ctx *c, ar_i32 top, ar_i32 right, ar_i32 bottom, ar_i32 left);

/* Where a backend drawing its own window controls has put them. */
void ar_set_titlebar_area(ar_ctx *c, ar_i32 x, ar_i32 y, ar_i32 w, ar_i32 h);

/*
 * `viewport-fit`. Zero is `auto`, which is the initial value.
 *
 * With `auto` the layout viewport is the safe rectangle -- the surface with
 * the insets already taken off -- and `env(safe-area-inset-*)` reports zero,
 * because there is nothing left for the stylesheet to avoid. With `cover` the
 * layout viewport is the whole surface and the real insets are reported.
 *
 * The two always move together. A viewport inset by the safe area *and* an
 * env() reporting that inset would take it off twice, which is the bug this
 * pairing exists to make impossible.
 */
void ar_set_viewport_fit_cover(ar_ctx *c, int cover);

/* ------------------------------------------------------------------------
 * Diagnostics
 *
 * Things a frame noticed that are probably not what the author meant, and are
 * not errors: the stylesheet is valid, the layout is correct, and the result
 * is still unlikely to be what anyone wanted.
 *
 * The first of them is the reason this exists. A sticky box inside an
 * `overflow: hidden` ancestor never sticks -- that ancestor is its scrollport
 * and it does not scroll -- which is correct behaviour and the most reported
 * non-bug in every engine. Rather than leave people to find that out, areole
 * says so.
 *
 * Refreshed every frame. Reading them is optional and costs nothing when
 * nobody does.
 * ------------------------------------------------------------------------ */
enum
{
    AR_DIAG_STICKY_NEVER_STICKS = 1
};

#define AR_DIAG_MAX 16

/* How many this frame produced. */
ar_i32 ar_diag_count(const ar_ctx *c);

/* The code of one, and through `out_node` the box it is about. Returns 0 for
   an index nobody reported. */
ar_i32 ar_diag_at(const ar_ctx *c, ar_i32 i, ar_i32 *out_node);

/* A fixed English sentence for a code. Never null. */
const char *ar_diag_text(ar_i32 code);

ar_perf *ar_perf_of(ar_ctx *c);

/* Where the arena stands. Every figure is bytes.
 *
 * persist    consumed once at init and never released: the stylesheet, the
 *            per-box state table
 * frame_now  in use by the current frame's tree
 * frame_peak the high water mark across the session
 * available  what is left between the two ends
 *
 * The invariant a benchmark checks with this: persist must not move after
 * init. If it does, something is allocating during a frame, and the whole
 * p99-equals-p50 property is gone. */
void ar_memory_stats(const ar_ctx *c, ar_u32 *persist, ar_u32 *frame_now, ar_u32 *frame_peak,
                     ar_u32 *available);

/* ------------------------------------------------------------------------
 * Library identity
 * ------------------------------------------------------------------------ */
/* ------------------------------------------------------------------------
 * Inspecting the frame that was just laid out
 *
 * Valid between ar_frame_end and the next ar_frame_begin, which is the window
 * in which the box tree exists and has coordinates. Boxes are numbered in
 * declaration order, so index 0 is the root and every parent comes before its
 * children.
 *
 * This is what a layout comparison needs: examples/02_tour --dump walks these
 * and prints one line per box, and tools/compare_layout.py lines that up
 * against the same tree measured by a browser.
 * ------------------------------------------------------------------------ */
ar_i32  ar_node_count(const ar_ctx *c);
ar_rect ar_node_rect(const ar_ctx *c, ar_i32 i);
ar_i32  ar_node_parent(const ar_ctx *c, ar_i32 i);

/* Which child of its parent this box is, counting from zero. The root is 0. */
ar_i32 ar_node_child_index(const ar_ctx *c, ar_i32 i);

/*
 * Whether areole made this box up.
 *
 * A cell declared straight inside a table needs a row to live in, and the
 * table model needs a rectangular grid before it can solve anything, so the
 * missing boxes are generated. They are real boxes with real rectangles and
 * they are not in the tree the caller described -- which matters to anything
 * comparing the two, including the layout corpora: a browser's generated boxes
 * are not elements and never appear in a walk of the DOM, so a comparison that
 * counted areole's would disagree on every case that has one.
 */
int ar_node_generated(const ar_ctx *c, ar_i32 i);

/* The text this box was given, or a pointer to "" if it was given none. The
   caller's own string, not a copy -- areole never copied it. */
const char *ar_node_text(const ar_ctx *c, ar_i32 i);

/*
 * Fragments: the rectangles of an inline box the line breaker cut.
 *
 * Zero means the box was not cut and ar_node_rect is the whole of it, which is
 * every box that is not a split inline. When there are fragments, ar_node_rect
 * is their union -- so a caller that does not care about them still gets one
 * truthful rectangle.
 */
ar_i32 ar_node_frag_count(const ar_ctx *c, ar_i32 i);

/* One fragment's rectangle, and the byte range of the node's text on it. The
   two out parameters may be null. */
ar_rect ar_node_frag(const ar_ctx *c, ar_i32 i, ar_i32 k, ar_i32 *out_from, ar_i32 *out_to);

const char *ar_version(void);

#ifdef __cplusplus
}
#endif

#endif /* AREOLE_H */
