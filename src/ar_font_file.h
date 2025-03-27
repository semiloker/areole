/*
 * areole - TrueType font file parsing.
 * SPDX-License-Identifier: MIT
 *
 * The parser never copies the file and never allocates. It holds a pointer and
 * a length, and every read is bounds checked against that length -- a font is
 * data from outside the program, so it is a security boundary and not merely a
 * source of nuisance. A read past the end returns zero rather than reading
 * memory; the resulting glyph is wrong, which is the correct outcome for a
 * corrupt file, and nothing else in the process is disturbed.
 *
 * Font units are converted to 26.6 pixels for ar_path, in integers, without
 * the intermediate overflow the obvious expression has.
 */
#ifndef AR_FONT_FILE_H
#define AR_FONT_FILE_H

#include "ar_path.h"

typedef struct ar_face
{
    const ar_u8 *data;
    ar_u32       size;

    /* Offsets into data. Zero means the table is absent. */
    ar_u32 head, hhea, maxp, cmap, loca, glyf, hmtx, os2;
    ar_u32 glyf_len, loca_len;

    ar_i32 units_per_em;
    ar_i32 loc_long; /* loca holds 32 bit offsets rather than 16 bit halves */
    ar_i32 num_glyphs;
    ar_i32 num_hmetrics;
    ar_i32 ascender, descender, line_gap; /* font units */
    ar_i32 x_height;                      /* font units; 0 if the font does not say */
    ar_i32 cap_height;

    ar_u32 cmap_sub; /* offset of the chosen subtable */
    ar_i32 cmap_format;

    ar_i32 ok;
} ar_face;

/* Zero if the data is not a font this can read. A face that fails to
   initialise is safe to pass to everything below; it simply has no glyphs. */
int ar_face_init(ar_face *f, const void *data, ar_u32 size);

/* Glyph 0 is the "not defined" box, which is what every font is required to
   put there and what a missing codepoint correctly maps to. */
ar_i32 ar_face_glyph(const ar_face *f, ar_u32 codepoint);

/* Advance width in font units. */
ar_i32 ar_face_advance(const ar_face *f, ar_i32 glyph);

/* Scales a font-unit value to 26.6 pixels at the given pixel size, exactly and
   without overflowing 32 bits on the way. */
ar_i32 ar_face_scale(const ar_face *f, ar_i32 value, ar_i32 ppem);

/* Working storage for decoding one glyph's points. Supplied by the caller so
   the parser keeps its promise not to allocate. 256 points covers all but the
   most elaborate glyphs; overflow is reported through the path. */
typedef struct ar_outline_buf
{
    ar_i32 *x;
    ar_i32 *y;
    ar_u8  *on;
    ar_i32  cap;
} ar_outline_buf;

/*
 * Appends the glyph's outline to the path, in 26.6 pixels at `ppem`, offset by
 * (ox, oy) which are also 26.6. Returns zero if the glyph could not be read.
 *
 * The y axis is flipped: fonts measure up from the baseline, bitmaps measure
 * down from the top.
 */
int ar_face_outline(const ar_face *f, ar_i32 glyph, ar_i32 ppem, ar_i32 ox, ar_i32 oy, ar_path *p,
                    const ar_outline_buf *buf);

/*
 * Vertical grid fitting.
 *
 * Unhinted text at interface sizes looks soft for one specific reason: the
 * x-height rarely lands on a pixel boundary, so the flat top of every lower
 * case letter is spread across two rows at half coverage. The baseline is
 * already integral because the pen is; the x-height is not.
 *
 * This returns a numerator and denominator to scale y by so that it is. At 13
 * px in Segoe UI the x-height is 6.6 px and this makes it 7, moving every
 * outline by six per cent vertically -- invisible as a shape, decisive as a
 * bitmap.
 *
 * It is not a bytecode interpreter and does not pretend to be. A real hinting
 * engine also aligns stem widths horizontally, which needs to know which
 * contours are stems; that is the expensive part and it is deferred. This is
 * the cheap ninety per cent.
 */
void ar_face_grid_fit(const ar_face *f, ar_i32 ppem, ar_i32 *num, ar_i32 *den);

#endif /* AR_FONT_FILE_H */
