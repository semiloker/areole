/*
 * areole - the outline text path.
 * SPDX-License-Identifier: MIT
 */
#include "ar_text.h"

#include "ar_break.h"
#include "ar_internal.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * UTF-8
 * ------------------------------------------------------------------------ */
#define AR_REPLACEMENT 0xFFFDu

ar_u32 ar_utf8_next(const char **p)
{
    const ar_u8 *s = (const ar_u8 *)*p;
    ar_u32       c = s[0];
    ar_i32       extra, i;
    ar_u32       cp, min;

    if (c == 0)
    {
        return 0;
    }
    if (c < 0x80u)
    {
        *p = (const char *)(s + 1);
        return c;
    }

    if ((c & 0xE0u) == 0xC0u)
    {
        cp = c & 0x1Fu;
        extra = 1;
        min = 0x80u;
    }
    else if ((c & 0xF0u) == 0xE0u)
    {
        cp = c & 0x0Fu;
        extra = 2;
        min = 0x800u;
    }
    else if ((c & 0xF8u) == 0xF0u)
    {
        cp = c & 0x07u;
        extra = 3;
        min = 0x10000u;
    }
    else
    {
        /* A continuation byte or an F8..FF lead: neither can start a
           character. One byte forward, so a stray byte costs one replacement
           rather than swallowing what follows it. */
        *p = (const char *)(s + 1);
        return AR_REPLACEMENT;
    }

    for (i = 1; i <= extra; ++i)
    {
        if ((s[i] & 0xC0u) != 0x80u)
        {
            *p = (const char *)(s + 1);
            return AR_REPLACEMENT;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }

    /* Overlong forms encode a small value in a long sequence, surrogates are
       not characters, and nothing exists above U+10FFFF. All three are how a
       decoder gets talked into producing a codepoint the rest of the program
       believed had already been checked, so all three are refused. */
    if (cp < min || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
    {
        *p = (const char *)(s + 1);
        return AR_REPLACEMENT;
    }

    *p = (const char *)(s + extra + 1);
    return cp;
}

/* ------------------------------------------------------------------------
 * The glyph cache
 * ------------------------------------------------------------------------ */
/*
 * v + darken * v * (255 - v) / 255 / 255, clamped.
 *
 * That is a parabola through 0 and 255 whose bulge is largest at half
 * coverage, which is exactly where an off-grid stem sits. It is not a gamma
 * curve, it is the cheapest thing with the same two fixed points and the same
 * shape between them, and it needs one multiply per entry of a 256 entry table
 * built once.
 */
static void ar__build_darken(ar_glyph_cache *gc)
{
    ar_i32 v;

    for (v = 0; v < 256; ++v)
    {
        ar_i32 lift = gc->darken * v * (255 - v) / 255 / 255;
        ar_i32 out = v + lift;
        gc->darken_lut[v] = (ar_u8)(out > 255 ? 255 : out);
    }
}

void ar_glyph_cache_set_darken(ar_glyph_cache *gc, ar_i32 amount)
{
    if (amount < 0)
    {
        amount = 0;
    }
    if (amount > 255)
    {
        amount = 255;
    }
    if (gc->darken == amount)
    {
        return;
    }
    gc->darken = amount;
    ar__build_darken(gc);
    ar_glyph_cache_clear(gc);
}

void ar_glyph_cache_init(ar_glyph_cache *gc, ar_glyph_slot *slots, ar_i32 nslots, ar_u8 *pixels,
                         ar_i32 cap)
{
    gc->slot = slots;
    gc->slots = nslots;
    gc->pixels = pixels;
    gc->cap = cap;
    gc->used = 0;
    gc->antialias = 1;
    gc->grid_fit = 1;
    gc->subpx = AR_SUBPX_STEPS;
    gc->darken = 0;
    ar__build_darken(gc);
    gc->hits = 0;
    gc->misses = 0;
    gc->resets = 0;
    ar_glyph_cache_clear(gc);
}

void ar_glyph_cache_clear(ar_glyph_cache *gc)
{
    ar_i32 i;

    for (i = 0; i < gc->slots; ++i)
    {
        gc->slot[i].key = 0;
    }
    gc->used = 0;
}

/* Glyph index in the low twenty bits, pixel size above it, the antialiasing
   flag above that, and the top bit always set so that glyph 0 at size 0 cannot
   look like a free slot. */
static ar_u32 ar__glyph_key(ar_i32 glyph, ar_i32 ppem, int antialias, ar_i32 subpx)
{
    return ((ar_u32)(subpx & 3) << 28) | ((ar_u32)(antialias ? 1 : 0) << 30) |
           (((ar_u32)ppem & 0xFFu) << 20) | ((ar_u32)glyph & 0xFFFFFu) | 0x80000000u;
}

/*
 * The key must be mixed before it is masked, not merely truncated.
 *
 * Masking the key directly takes its low bits, which are the glyph index --
 * so every size of one glyph lands in the same slot. Sixteen sizes exactly
 * filled the sixteen-probe window, every lookup fell through to the end of it
 * and overwrote a live entry, and the cache stopped caching. It was invisible
 * until a benchmark drew the same text at sixteen sizes: 351,140 misses across
 * 400 frames of a scene that should have missed a few hundred times in total.
 *
 * One multiply spreads every field of the key across the whole word.
 */
static ar_u32 ar__glyph_slot_of(ar_u32 key, ar_u32 mask)
{
    ar_u32 h = key * 2654435761u;
    return (h >> 16) & mask;
}

const ar_glyph_slot *ar_glyph_get(ar_glyph_cache *gc, const ar_face *f, ar_i32 glyph, ar_i32 ppem,
                                  ar_i32 subpx, ar_glyph_scratch *sc)
{
    ar_u32  key;
    ar_u32  mask = (ar_u32)gc->slots - 1u;
    ar_u32  at;
    ar_i32  probe;
    ar_path path;
    ar_rect bounds;

    if (gc->subpx <= 1 || subpx < 0 || subpx >= AR_SUBPX_STEPS)
    {
        subpx = 0;
    }
    key = ar__glyph_key(glyph, ppem, gc->antialias, subpx);
    at = ar__glyph_slot_of(key, mask);

    if (!gc->slots || !f->ok)
    {
        return 0;
    }

    for (probe = 0; probe < 16; ++probe)
    {
        ar_glyph_slot *s = &gc->slot[at];
        if (s->key == key)
        {
            ++gc->hits;
            return s;
        }
        if (s->key == 0)
        {
            break;
        }
        at = (at + 1u) & mask;
    }

    ++gc->misses;

    ar_path_init(&path, sc->path_pts, sc->path_cap);
    if (!ar_face_outline(f, glyph, ppem, 0, 0, &path, &sc->outline) || path.overflow)
    {
        /* A glyph with no outline is still a glyph: a space has an advance and
           no pixels, and caching that fact stops it being re-derived. */
        bounds = ar_rect_make(0, 0, 0, 0);
    }
    else
    {
        /* Grid fitting is applied here rather than in the parser because it is
           a rendering decision, not a fact about the file, and because the
           result lands in the cache and is paid for once. */
        if (subpx > 0)
        {
            /* Shifted before the bounds are taken, so the bitmap covers where
               the ink actually lands rather than where a whole-pixel version
               of it would have. */
            ar_i32 dx = subpx * AR_ONE_PIXEL / AR_SUBPX_STEPS;
            ar_i32 i;
            for (i = 0; i < path.count; ++i)
            {
                path.pt[i * 2] += dx;
            }
        }
        if (gc->grid_fit)
        {
            ar_i32 num = 1, den = 1;
            ar_face_grid_fit(f, ppem, &num, &den);
            if (num != den && den > 0)
            {
                ar_i32 i;
                for (i = 0; i < path.count; ++i)
                {
                    path.pt[i * 2 + 1] = path.pt[i * 2 + 1] * num / den;
                }
            }
        }
        bounds = ar_path_bounds(&path);
    }

    {
        ar_i32 w = bounds.w, h = bounds.h;
        ar_i32 need = w * h;

        if (w < 0 || h < 0 || (h > 0 && need / h != w))
        {
            return 0; /* absurd metrics; refuse rather than wrap */
        }
        if (need > 0 && (need > gc->cap || (w + 2) * h > sc->acc_cap))
        {
            return 0; /* one glyph larger than the whole budget */
        }

        if (gc->used + need > gc->cap)
        {
            /* ponytail: drop everything rather than evict least-recently-used.
               An interface has a few hundred distinct glyphs and the default
               budget holds a few thousand, so this is reached by a program
               that renders a novel at many sizes -- and for that program the
               ceiling is the cache reporting resets, not silence. Per-entry
               LRU is the upgrade if that ever appears. */
            ar_glyph_cache_clear(gc);
            ++gc->resets;
            at = ar__glyph_slot_of(key, mask);
            while (gc->slot[at].key != 0)
            {
                at = (at + 1u) & mask;
            }
        }

        {
            ar_glyph_slot *s = &gc->slot[at];

            s->key = key;
            s->off = gc->used;
            s->w = w;
            s->h = h;
            s->left = bounds.x;
            s->top = bounds.y;
            s->advance = ar_face_scale(f, ar_face_advance(f, glyph), ppem);

            if (need > 0)
            {
                memset(gc->pixels + gc->used, 0, (size_t)need);
                ar_path_rasterize(&path, gc->pixels + gc->used, w, h, w, bounds.x * AR_ONE_PIXEL,
                                  bounds.y * AR_ONE_PIXEL, AR_FILL_NONZERO, sc->acc);

                if (gc->darken > 0)
                {
                    /* Stem darkening. An unhinted stem one pixel wide that
                       straddles two columns leaves both at half coverage and
                       the letter looks grey rather than black. Lifting the
                       midtones puts the weight back without touching a stem
                       that already lands on the grid, because 0 and 255 are
                       fixed points of the curve.

                       The table is built once per cache, not per glyph, and
                       the curve is a piecewise linear approximation of a gamma
                       because a gamma needs pow() and this library has no
                       floating point. */
                    ar_u8 *q = gc->pixels + gc->used;
                    ar_i32 k;
                    for (k = 0; k < need; ++k)
                    {
                        q[k] = gc->darken_lut[q[k]];
                    }
                }

                if (!gc->antialias)
                {
                    /* Thresholded once, here, rather than tested per pixel at
                       every blit: the whole point of a cache is that work done
                       on a miss is not done again. It also means the blit hits
                       the opaque store path instead of the blend. */
                    ar_u8 *q = gc->pixels + gc->used;
                    ar_i32 k;
                    for (k = 0; k < need; ++k)
                    {
                        q[k] = q[k] >= 128u ? 255u : 0u;
                    }
                }
                gc->used += need;
            }
            return s;
        }
    }
}

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */
static void ar__blit_coverage(ar_surface *s, ar_rect clip, const ar_glyph_slot *g,
                              const ar_u8 *cov, ar_i32 px, ar_i32 py, ar_color c, ar_u32 alpha)
{
    ar_rect d = ar_rect_intersect(ar_rect_make(px, py, g->w, g->h), clip);
    ar_i32  y;

    if (ar_rect_is_empty(d))
    {
        return;
    }

    for (y = 0; y < d.h; ++y)
    {
        const ar_u8 *src = cov + (ar_i32)(d.y - py + y) * g->w + (d.x - px);
        ar_u32      *dst = s->pixels + (ar_i32)(d.y + y) * s->stride + d.x;
        ar_i32       x;

        for (x = 0; x < d.w; ++x)
        {
            ar_u32 a = src[x];
            if (a == 0)
            {
                continue;
            }
            if (alpha != 0xFFu)
            {
                a = a * alpha / 255u;
            }
            dst[x] = a >= 255u ? c : ar__blend(dst[x], c, a);
        }
        AR_COUNT(glyph_px, d.w);
    }
}

ar_i32 ar_text_draw(ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, const char *utf8,
                    const ar_face *f, ar_i32 ppem, ar_color c, ar_glyph_cache *gc,
                    ar_glyph_scratch *sc)
{
    ar_i32 pen = x * AR_ONE_PIXEL;
    ar_u32 alpha;
    ar_rect bounds;

    if (!utf8 || !f || !f->ok || !s || !s->pixels)
    {
        return 0;
    }
    alpha = AR_ALPHA_OF(c);
    if (alpha == 0)
    {
        return ar_text_measure(utf8, f, ppem, gc, sc);
    }

    bounds = ar_rect_intersect(clip, ar_rect_make(0, 0, s->w, s->h));
    if (ar_rect_is_empty(bounds))
    {
        return ar_text_measure(utf8, f, ppem, gc, sc);
    }

    AR_COUNT(text_calls, 1);

    for (;;)
    {
        ar_u32               cp = ar_utf8_next(&utf8);
        const ar_glyph_slot *g;

        if (cp == 0)
        {
            break;
        }
        /* Which quarter of a pixel the pen is standing in. */
        g = ar_glyph_get(gc, f, ar_face_glyph(f, cp), ppem,
                         (pen % AR_ONE_PIXEL) * AR_SUBPX_STEPS / AR_ONE_PIXEL, sc);
        if (!g)
        {
            continue;
        }

        if (g->w > 0 && g->h > 0)
        {
            /* The pen is in 26.6 and the bitmap was rasterized on whole pixel
               bounds, so the fractional part is dropped here rather than
               carried into a second cache dimension. Subpixel positioning is
               0.3.0's problem; at that point the key grows a third field. */
            AR_COUNT(glyphs, 1);
            ar__blit_coverage(s, bounds, g, gc->pixels + g->off, (pen / AR_ONE_PIXEL) + g->left,
                              y + g->top, c, alpha);
        }
        pen += g->advance;
    }

    return pen - x * AR_ONE_PIXEL;
}

ar_i32 ar_text_measure(const char *utf8, const ar_face *f, ar_i32 ppem, ar_glyph_cache *gc,
                       ar_glyph_scratch *sc)
{
    ar_i32 pen = 0;

    if (!utf8 || !f || !f->ok)
    {
        return 0;
    }
    for (;;)
    {
        ar_u32               cp = ar_utf8_next(&utf8);
        const ar_glyph_slot *g;

        if (cp == 0)
        {
            break;
        }
        /* Measuring must ask for the same entry drawing will, or a string
           could measure with one set of bitmaps and draw with another. */
        g = ar_glyph_get(gc, f, ar_face_glyph(f, cp), ppem,
                         (pen % AR_ONE_PIXEL) * AR_SUBPX_STEPS / AR_ONE_PIXEL, sc);
        if (g)
        {
            pen += g->advance;
        }
    }
    return pen;
}

/* Measures one byte range, which wrapping needs and ar_text_measure cannot do
   because it stops at the terminator. */
static ar_i32 ar__measure_range(const char *text, ar_i32 from, ar_i32 to, const ar_face *f,
                                ar_i32 ppem, ar_glyph_cache *gc, ar_glyph_scratch *sc)
{
    const char *p = text + from;
    ar_i32      pen = 0;

    while (p < text + to)
    {
        ar_u32               cp = ar_utf8_next(&p);
        const ar_glyph_slot *g;

        if (cp == 0)
        {
            break;
        }
        g = ar_glyph_get(gc, f, ar_face_glyph(f, cp), ppem,
                         (pen % AR_ONE_PIXEL) * AR_SUBPX_STEPS / AR_ONE_PIXEL, sc);
        if (g)
        {
            pen += g->advance;
        }
    }
    return pen;
}

ar_i32 ar_text_wrap(const char *utf8, const ar_face *f, ar_i32 ppem, ar_i32 max_w,
                    ar_glyph_cache *gc, ar_glyph_scratch *sc, ar_i32 *starts, ar_i32 max_lines)
{
    ar_i32 line_start = 0, at = 0, lines = 0;
    ar_i32 len = 0;
    ar_i32 limit = max_w * AR_ONE_PIXEL;

    if (!utf8 || !f || !f->ok || !starts || max_lines <= 0)
    {
        return 0;
    }
    while (utf8[len])
    {
        ++len;
    }
    starts[lines++] = 0;
    if (len == 0)
    {
        return lines;
    }

    for (;;)
    {
        ar_i32 kind;
        ar_i32 next = ar_break_next(utf8, at, &kind);

        if (next <= at)
        {
            break;
        }

        /* Measured from the start of the line rather than accumulated, because
           a glyph's advance can depend on where in the pixel it starts. */
        if (ar__measure_range(utf8, line_start, next, f, ppem, gc, sc) > limit && at > line_start)
        {
            if (lines >= max_lines)
            {
                return lines;
            }
            starts[lines++] = at;
            line_start = at;
            continue; /* re-test this same opportunity against the new line */
        }

        at = next;
        if (kind == AR_BREAK_MANDATORY && at < len)
        {
            if (lines >= max_lines)
            {
                return lines;
            }
            starts[lines++] = at;
            line_start = at;
        }
        if (at >= len)
        {
            break;
        }
    }
    return lines;
}
