/*
 * areole - the outline text path.
 * SPDX-License-Identifier: MIT
 */
#include "ar_text.h"

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
void ar_glyph_cache_init(ar_glyph_cache *gc, ar_glyph_slot *slots, ar_i32 nslots, ar_u8 *pixels,
                         ar_i32 cap)
{
    gc->slot = slots;
    gc->slots = nslots;
    gc->pixels = pixels;
    gc->cap = cap;
    gc->used = 0;
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

/* Glyph index in the low bits, pixel size above it, and one added so that
   glyph 0 at size 0 cannot look like a free slot. */
static ar_u32 ar__glyph_key(ar_i32 glyph, ar_i32 ppem)
{
    return ((ar_u32)ppem << 20) | ((ar_u32)glyph & 0xFFFFFu) | 0x80000000u;
}

const ar_glyph_slot *ar_glyph_get(ar_glyph_cache *gc, const ar_face *f, ar_i32 glyph, ar_i32 ppem,
                                  ar_glyph_scratch *sc)
{
    ar_u32 key = ar__glyph_key(glyph, ppem);
    ar_u32 mask = (ar_u32)gc->slots - 1u;
    ar_u32 at = key & mask;
    ar_i32 probe;
    ar_path path;
    ar_rect bounds;

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
            at = key & mask;
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
        g = ar_glyph_get(gc, f, ar_face_glyph(f, cp), ppem, sc);
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
        g = ar_glyph_get(gc, f, ar_face_glyph(f, cp), ppem, sc);
        if (g)
        {
            pen += g->advance;
        }
    }
    return pen;
}
