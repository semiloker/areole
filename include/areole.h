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
 * Library identity
 * ------------------------------------------------------------------------ */
const char *ar_version(void);

#ifdef __cplusplus
}
#endif

#endif /* AREOLE_H */
