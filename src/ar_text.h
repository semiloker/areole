/*
 * areole - the outline text path: UTF-8, the glyph cache, and drawing.
 * SPDX-License-Identifier: MIT
 *
 * Rasterizing an outline costs roughly a thousand times what blitting the
 * result costs, so the cache is not an optimisation here, it is the thing that
 * makes outlines usable at all. Everything else in this file exists to feed it
 * or to read from it.
 *
 * All storage is supplied by the caller, in keeping with the library's first
 * invariant. Nothing here allocates.
 */
#ifndef AR_TEXT_H
#define AR_TEXT_H

#include "ar_font_file.h"
#include "ar_shape.h"

/* ------------------------------------------------------------------------
 * UTF-8
 * ------------------------------------------------------------------------ */

/*
 * Decodes one codepoint and advances the pointer past it. Returns 0 at the end
 * of the string.
 *
 * Malformed input yields U+FFFD and advances exactly one byte, which is what
 * keeps a decoder from either looping forever or resynchronising somewhere
 * surprising. Overlong forms, surrogates and values above U+10FFFF are all
 * rejected the same way: they are how a decoder gets talked into producing a
 * codepoint the rest of the program believed it had already validated.
 */
ar_u32 ar_utf8_next(const char **p);

/* ------------------------------------------------------------------------
 * The glyph cache
 * ------------------------------------------------------------------------ */
#define AR_SUBPX_STEPS 4

typedef struct ar_glyph_slot
{
    ar_u32 key;     /* glyph index and pixel size, packed; 0 means free */
    ar_i32 off;     /* start of the coverage in the pixel slab */
    ar_i32 w, h;    /* bitmap size; either may be zero for a blank glyph */
    ar_i32 left;    /* whole pixels from the pen to the bitmap's left edge */
    ar_i32 top;     /* whole pixels from the baseline up to its top edge */
    ar_i32 advance; /* 26.6 */
} ar_glyph_slot;

typedef struct ar_glyph_cache
{
    ar_glyph_slot *slot;
    ar_i32         slots; /* must be a power of two */

    ar_u8 *pixels;
    ar_i32 cap;
    ar_i32 used;

    /*
     * Off means the coverage is thresholded at half when the glyph is
     * rasterized, so every pixel is either fully inked or untouched.
     *
     * Two reasons it exists rather than being always on. Blending costs a read
     * and a write per pixel where an opaque store costs one write, which on a
     * machine whose whole problem is memory bandwidth is the difference the
     * rest of this library is built around -- measured at 1.89x on twelve
     * lines of 14 px text, 116 ns per glyph against 219. And hard edges are
     * what the machines this targets actually looked like, so it is a
     * legitimate choice rather than only a cheap one.
     *
     * The flag is part of the cache key, so switching it mid frame is well
     * defined: aliased and antialiased renderings of the same glyph are two
     * entries, and an interface may use one for small text and the other for
     * large without either being wrong.
     */
    int antialias;

    /* Snap the x-height to the pixel grid. See ar_face_grid_fit. On by
       default: it costs one multiply per point, once, on a cache miss. */
    int grid_fit;

    /* 0 to 255. Lifts midtone coverage so an off-grid stem does not read grey.
       Zero by default, because it is a taste decision and the honest starting
       point is the outline as the designer drew it. */
    ar_i32 darken;
    ar_u8  darken_lut[256];

    /*
     * Horizontal subpixel positions per pixel: 1 or AR_SUBPX_STEPS.
     *
     * With one, every glyph starts on a whole pixel and the fractional part of
     * the pen is thrown away, so the gaps between letters come out uneven --
     * a word looks like it was set by someone nudging each letter. With four,
     * the glyph is rasterized at each quarter offset and the right one is
     * chosen, which costs four times the cache entries and nothing per frame.
     *
     * Four is the usual answer. Eight is not visibly better and doubles the
     * atlas again.
     */
    ar_i32 subpx;

    ar_u32 hits, misses, resets;
} ar_glyph_cache;

/* Scratch for rasterizing one glyph. Reused for every glyph, so it is sized
   for the largest one rather than for the text. */
typedef struct ar_glyph_scratch
{
    ar_i32        *path_pts; /* 2 * path_cap integers */
    ar_i32         path_cap;
    ar_outline_buf outline;
    ar_i32        *acc; /* (w + 2) * h integers for the largest glyph */
    ar_i32         acc_cap;

    /* Shaping works on a run of glyphs rather than one character at a time,
       so a run has to be built somewhere. Text longer than this is shaped in
       chunks split at spaces, which keeps ligatures whole because ligatures
       do not cross words. A word longer than the buffer is the one case that
       could lose one, and it is a word of over a hundred letters. */
    ar_i32 *shape_glyph;
    ar_i32 *shape_adv;
    ar_i32 *shape_cluster;
    ar_u32 *shape_cp; /* the characters, which Arabic joining needs */
    ar_i32 *shape_dx; /* mark displacements, in font units */
    ar_i32 *shape_dy;
    ar_i32  shape_cap;
} ar_glyph_scratch;

void ar_glyph_cache_init(ar_glyph_cache *gc, ar_glyph_slot *slots, ar_i32 nslots, ar_u8 *pixels,
                         ar_i32 cap);

/* Drops everything. Called for a new face, since the key does not identify
   one -- a cache is per face by construction. */
void ar_glyph_cache_clear(ar_glyph_cache *gc);
void ar_glyph_cache_set_darken(ar_glyph_cache *gc, ar_i32 amount);

/* The cached coverage bitmap for one glyph, rasterizing it if this is the
   first time it has been asked for. Returns null only if the glyph could not
   be rasterized at all; a blank glyph such as a space returns a slot whose
   w and h are zero, which still carries the advance. */
const ar_glyph_slot *ar_glyph_get(ar_glyph_cache *gc, const ar_face *f, ar_i32 glyph, ar_i32 ppem,
                                  ar_i32 subpx, ar_glyph_scratch *sc);
const ar_glyph_slot *ar_glyph_get_face(ar_glyph_cache *gc, const ar_face *f, ar_i32 glyph,
                                       ar_i32 ppem, ar_i32 subpx, ar_i32 face,
                                       ar_glyph_scratch *sc);

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

/* Draws UTF-8 text with the baseline at y. Returns the pen advance in 26.6,
   so a caller can lay out without measuring twice. */
ar_i32 ar_text_draw(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *utf8,
                    const ar_face *f, ar_i32 ppem, ar_color c, ar_glyph_cache *gc,
                    ar_glyph_scratch *sc);

/* ------------------------------------------------------------------------
 * Fallback
 *
 * No face covers Unicode. A primary face chosen for Latin will not have CJK,
 * and the result of asking it for a Japanese codepoint is glyph 0 -- the
 * notdef box, the tofu everyone recognises and nobody wants.
 *
 * A chain fixes that: ask each face in turn and use the first that has the
 * character. The chain is ordered, so the first face wins wherever it can,
 * which is what keeps a document looking like one typeface rather than a
 * ransom note.
 * ------------------------------------------------------------------------ */
#define AR_MAX_FACES 4

typedef struct ar_font_chain
{
    const ar_face *face[AR_MAX_FACES];
    ar_i32         count;
} ar_font_chain;

/* Which face has this codepoint, and its glyph in that face. Falls back to
   glyph 0 of the first face when nothing does, so the caller still gets a
   notdef box rather than nothing at all. */
ar_i32 ar_font_chain_glyph(const ar_font_chain *ch, ar_u32 cp, ar_i32 *face_index);

ar_i32 ar_text_measure_chain(const char *utf8, const ar_font_chain *ch, ar_i32 ppem,
                             ar_glyph_cache *gc, ar_glyph_scratch *sc);

/* Draws through the chain, choosing a face per character. */
ar_i32 ar_text_draw_chain(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *utf8,
                          const ar_font_chain *ch, ar_i32 ppem, ar_color c, ar_glyph_cache *gc,
                          ar_glyph_scratch *sc);

/* The same, with the face's ligature and kerning tables applied. Passing a
   null shaper is the same as the call above, which is what a caller with no
   shaper or a face without the tables gets. */
ar_i32 ar_text_draw_shaped(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *utf8,
                           const ar_font_chain *ch, const ar_shaper *sh, ar_i32 ppem, ar_color c,
                           ar_glyph_cache *gc, ar_glyph_scratch *sc, ar_i32 *out_width);

/*
 * Breaks text into lines no wider than max_w pixels, writing the byte offset
 * of each line's start into `starts` and returning how many there are. The
 * first entry is always 0.
 *
 * Break opportunities come from ar_break_next, so this is UAX #14 and not "at
 * spaces": it will not orphan a closing bracket, will not split 1,000.50, and
 * does break between ideographs.
 *
 * A word longer than max_w is not broken. Overflowing is a visible failure the
 * caller can see and clip; splitting a word at an arbitrary point is a silent
 * one that looks like a rendering bug. Emergency breaking belongs with
 * overflow-wrap, which is a CSS property and not this function's decision.
 */
/*
 * The width of text[from..to) in 1/AR_ONE_PIXEL units.
 *
 * A wrap candidate is a slice of the caller's string, and copying it out to
 * measure it would be an allocation this library does not make -- so the range
 * is passed rather than a substring.
 */
typedef ar_i32 (*ar_range_fn)(void *ud, const char *text, ar_i32 from, ar_i32 to);

/* The UAX #14 walk, with the measurement left to the caller. `max_w` is in
   whole pixels; `measure` returns 1/AR_ONE_PIXEL units. */
ar_i32 ar_text_wrap_by(const char *utf8, ar_range_fn measure, void *ud, ar_i32 max_w,
                       ar_i32 *starts, ar_i32 max_lines);

ar_i32 ar_text_wrap(const char *utf8, const ar_face *f, ar_i32 ppem, ar_i32 max_w,
                    ar_glyph_cache *gc, ar_glyph_scratch *sc, ar_i32 *starts, ar_i32 max_lines);

/* The same, measured through a fallback chain. This is the one layout uses:
   a paragraph that reaches into a fallback face for one word still has to
   break where it really gets too wide. */
ar_i32 ar_text_wrap_chain(const char *utf8, const ar_font_chain *ch, ar_i32 ppem, ar_i32 max_w,
                          ar_glyph_cache *gc, ar_glyph_scratch *sc, ar_i32 *starts,
                          ar_i32 max_lines);

/* The same walk without drawing. Costs a cache lookup per glyph, not a
   rasterization, once the glyphs are warm. */
ar_i32 ar_text_measure(const char *utf8, const ar_face *f, ar_i32 ppem, ar_glyph_cache *gc,
                       ar_glyph_scratch *sc);

#endif /* AR_TEXT_H */
