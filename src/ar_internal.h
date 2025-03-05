/*
 * areole - internals shared between core translation units.
 * SPDX-License-Identifier: MIT
 *
 * Not installed and not part of the public API. Tests include it deliberately.
 */
#ifndef AR_INTERNAL_H
#define AR_INTERNAL_H

#include "areole.h"

/* Every allocation is aligned to this. Eight covers every type areole stores,
   including double, on every target it runs on. */
#define AR_ALIGN 8u

/* ------------------------------------------------------------------------
 * Arena
 *
 * The caller hands areole one block and that is all it ever gets. There is no
 * malloc after ar_init, which is what keeps p99 frame time equal to p50 and
 * lets areole run where a real allocator would not be worth having.
 *
 * The block is consumed from both ends:
 *
 *     [ persistent ->                              <- ephemeral ]
 *     0            lo                            hi            size
 *
 * Persistent memory grows up and is never released: widget state, scroll
 * offsets, the parsed stylesheet. Ephemeral memory grows down and is released
 * wholesale at the top of every frame: the node tree, traversal stacks, the
 * command list. Releasing a frame is one integer store.
 *
 * Consuming from opposite ends means init and frame allocation cannot get in
 * each other's way, and running out is a single comparison.
 * ------------------------------------------------------------------------ */
typedef struct ar_arena
{
    ar_u8 *base;
    ar_u32 size;
    ar_u32 lo;     /* persistent bytes used, [0, lo)     */
    ar_u32 hi;     /* ephemeral floor, [hi, size)        */
    ar_u32 hi_min; /* lowest hi ever seen, for peak use  */
    ar_u32 oom;    /* sticky: an allocation was refused  */
} ar_arena;

/* ------------------------------------------------------------------------
 * Embedded font
 *
 * 8x8 monochrome, ASCII 32 to 126. Row order is top to bottom, and within a
 * row bit 0 is the LEFTMOST pixel. The data lives in ar_font_data.c and is
 * generated; see THIRDPARTY.md.
 * ------------------------------------------------------------------------ */
#define AR_FONT_W      8
#define AR_FONT_H      8
#define AR_FONT_FIRST  32
#define AR_FONT_LAST   126
#define AR_FONT_GLYPHS (AR_FONT_LAST - AR_FONT_FIRST + 1)

extern const ar_u8 ar__font_rows[AR_FONT_GLYPHS][AR_FONT_H];
extern const ar_u8 ar__font_left[AR_FONT_GLYPHS];
extern const ar_u8 ar__font_advance[AR_FONT_GLYPHS];

/* Source-over of one pixel. Declared here rather than kept static in the
   rasterizer so the glyph blitter can share the exact same arithmetic; two
   implementations would drift and text would stop matching its own panel. */
ar_u32 ar__blend(ar_u32 dst, ar_u32 src, ar_u32 alpha);

void  ar_arena_init(ar_arena *a, void *mem, ar_u32 size);
void *ar_arena_persist(ar_arena *a, ar_u32 bytes);
void *ar_arena_frame(ar_arena *a, ar_u32 bytes);
void  ar_arena_frame_reset(ar_arena *a);

ar_u32 ar_arena_persist_used(const ar_arena *a);
ar_u32 ar_arena_frame_used(const ar_arena *a);
ar_u32 ar_arena_frame_peak(const ar_arena *a);
ar_u32 ar_arena_available(const ar_arena *a);

#endif /* AR_INTERNAL_H */
