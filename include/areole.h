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

#define AR_VERSION_MAJOR  0
#define AR_VERSION_MINOR  1
#define AR_VERSION_PATCH  0
#define AR_VERSION_STRING "0.1.0-dev"

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
    int    mouse_inside;   /* the cursor is over the client area */
} ar_input;

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

#define AR_BYTES_PER_BOX 320u
#define AR_MEM_FIXED     98304u
#define AR_MEM(boxes)    (AR_MEM_FIXED + (ar_u32)(boxes) * AR_BYTES_PER_BOX)

/* Returns NULL if the block is too small to be useful. */
ar_ctx *ar_init(void *mem, ar_u32 size);

/* Parses a stylesheet into the context. Call it as many times as you like at
   startup; each call appends. Never call it per frame: the whole point is that
   parsing happens once and the frame only resolves. */
void ar_stylesheet(ar_ctx *c, const char *css);

/* Non-zero if the stylesheet had anything wrong with it. Parsing never aborts,
   so this is the only way to find out. */
ar_u32 ar_stylesheet_errors(const ar_ctx *c);

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

/* Non-zero when the box under the cursor changed during the last frame. Hover
   is resolved from the previous frame, so a caller whose pump blocks when idle
   must draw one more frame to show the highlight. */
int ar_needs_redraw(const ar_ctx *c);

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
const char *ar_version(void);

#ifdef __cplusplus
}
#endif

#endif /* AREOLE_H */
