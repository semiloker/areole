/*
 * areole - paths and the coverage rasterizer.
 * SPDX-License-Identifier: MIT
 *
 * A path is a set of closed contours of straight segments. Curves are
 * flattened as they are added, so the rasterizer never sees a bezier and the
 * two concerns stay separable.
 *
 * Coordinates are 26.6 fixed point: signed pixels with a sixty-fourth of a
 * pixel of resolution. That is the same unit TrueType uses, which is not a
 * coincidence -- it is what the font parser will hand over.
 *
 * No floating point anywhere. That is the library's second invariant and text
 * does not get an exemption from it; a Pentium II has an FPU but using it here
 * would put a serialising instruction in the innermost loop of the renderer.
 */
#ifndef AR_PATH_H
#define AR_PATH_H

#include "areole.h"

#define AR_SUBPIXEL     6                   /* fractional bits in a coordinate */
#define AR_ONE_PIXEL    (1 << AR_SUBPIXEL)  /* 26.6 value of exactly one pixel */
#define AR_PATH_MAX_CONTOURS 96

/* Coverage accumulates in 12 fractional bits. Sixteen would be the obvious
   choice and does not fit: the rasterizer multiplies two of these together and
   C89 has no integer wider than 32 bits, so 12 bits keeps every product inside
   2^24 with room to spare. Eight bits of output need far less than 12 anyway. */
#define AR_COV_BITS 12
#define AR_COV_ONE  (1 << AR_COV_BITS)

typedef struct ar_path
{
    ar_i32 *pt;  /* interleaved x, y in 26.6 */
    ar_i32  cap; /* capacity in points, not integers */
    ar_i32  count;

    ar_i32 contour[AR_PATH_MAX_CONTOURS + 1];
    ar_i32 contours;

    /* Set when the point or contour storage ran out. A path that overflowed is
       reported rather than silently drawn wrong, in the same spirit as the box
       tree: the caller sized it, so the caller is told. */
    ar_i32 overflow;
} ar_path;

void ar_path_init(ar_path *p, ar_i32 *storage, ar_i32 capacity_points);

void ar_path_move_to(ar_path *p, ar_i32 x, ar_i32 y);
void ar_path_line_to(ar_path *p, ar_i32 x, ar_i32 y);

/* Quadratic, as TrueType glyf stores them; cubic, as CFF and SVG do. Both are
   flattened here, adaptively, to a quarter of a pixel. */
void ar_path_quad_to(ar_path *p, ar_i32 cx, ar_i32 cy, ar_i32 x, ar_i32 y);
void ar_path_cubic_to(ar_path *p, ar_i32 c1x, ar_i32 c1y, ar_i32 c2x, ar_i32 c2y, ar_i32 x,
                      ar_i32 y);

/* Contours are implicitly closed by the rasterizer, so this only ends the
   current one. Calling it is optional before another move_to. */
void ar_path_close(ar_path *p);

/* The path's bounding box in whole pixels, which is what a caller needs to
   size a coverage bitmap. Returns an empty rect for an empty path. */
ar_rect ar_path_bounds(const ar_path *p);

enum
{
    AR_FILL_NONZERO = 0,
    AR_FILL_EVENODD = 1
};

/*
 * Rasterizes into an 8-bit coverage bitmap, 0 to 255, origin at (ox, oy) in
 * path space. `acc` is scratch of at least (w + 2) * h integers and may be
 * uninitialised; it is cleared here.
 *
 * The antialiasing is analytic rather than sampled: each segment contributes
 * the exact signed area it covers in every cell it crosses, and a prefix sum
 * along the row turns those deltas into coverage. Supersampling would need
 * sixteen samples a pixel to look as good and would still be wrong on a near
 * horizontal edge.
 */
void ar_path_rasterize(const ar_path *p, ar_u8 *cov, ar_i32 w, ar_i32 h, ar_i32 stride, ar_i32 ox,
                       ar_i32 oy, ar_i32 rule, ar_i32 *acc);

#endif /* AR_PATH_H */
