/*
 * areole - TrueType font file parsing.
 * SPDX-License-Identifier: MIT
 *
 * Written against the specification rather than adapted from an existing
 * implementation, which matters mostly for the bounds checking: every read
 * goes through one of four accessors that compare against the file length
 * first and return zero rather than reading past it. There is no path through
 * this file that dereferences an offset taken from the font without checking
 * it, because a font is data from outside the program.
 *
 * A corrupt font therefore produces a wrong glyph and nothing else. That is
 * the right failure: refusing to render is worse for the user than rendering
 * badly, and reading out of bounds is worse than both.
 */
#include "ar_font_file.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Bounds-checked big-endian reads
 * ------------------------------------------------------------------------ */
static ar_u32 ar__u8at(const ar_face *f, ar_u32 off)
{
    if (off >= f->size)
    {
        return 0;
    }
    return f->data[off];
}

static ar_u32 ar__u16at(const ar_face *f, ar_u32 off)
{
    if (off + 1 >= f->size)
    {
        return 0;
    }
    return ((ar_u32)f->data[off] << 8) | f->data[off + 1];
}

static ar_i32 ar__i16at(const ar_face *f, ar_u32 off)
{
    ar_u32 v = ar__u16at(f, off);
    return v >= 0x8000u ? (ar_i32)v - 0x10000 : (ar_i32)v;
}

static ar_u32 ar__u32at(const ar_face *f, ar_u32 off)
{
    if (off + 3 >= f->size)
    {
        return 0;
    }
    return ((ar_u32)f->data[off] << 24) | ((ar_u32)f->data[off + 1] << 16) |
           ((ar_u32)f->data[off + 2] << 8) | f->data[off + 3];
}

static ar_u32 ar__tag(const char *s)
{
    return ((ar_u32)(ar_u8)s[0] << 24) | ((ar_u32)(ar_u8)s[1] << 16) | ((ar_u32)(ar_u8)s[2] << 8) |
           (ar_u8)s[3];
}

/* ------------------------------------------------------------------------
 * The table directory
 * ------------------------------------------------------------------------ */
static ar_u32 ar__find_table(const ar_face *f, const char *tag, ar_u32 *len_out)
{
    ar_u32 want = ar__tag(tag);
    ar_u32 count = ar__u16at(f, 4);
    ar_u32 i;

    if (len_out)
    {
        *len_out = 0;
    }
    /* A directory claiming more tables than could fit in the file is the first
       thing a fuzzer produces, so the count is bounded before it is trusted. */
    if (count > (f->size - 12) / 16)
    {
        return 0;
    }

    for (i = 0; i < count; ++i)
    {
        ar_u32 rec = 12 + i * 16;
        if (ar__u32at(f, rec) == want)
        {
            ar_u32 off = ar__u32at(f, rec + 8);
            ar_u32 len = ar__u32at(f, rec + 12);

            if (off >= f->size)
            {
                return 0;
            }
            if (len > f->size - off)
            {
                len = f->size - off; /* truncated file: use what is there */
            }
            if (len_out)
            {
                *len_out = len;
            }
            return off;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * cmap
 * ------------------------------------------------------------------------ */
static void ar__pick_cmap(ar_face *f)
{
    ar_u32 count, i;
    ar_u32 best = 0;
    ar_i32 best_score = -1;

    f->cmap_sub = 0;
    f->cmap_format = 0;
    if (!f->cmap)
    {
        return;
    }

    count = ar__u16at(f, f->cmap + 2);
    if (count > (f->size - f->cmap) / 8)
    {
        return;
    }

    for (i = 0; i < count; ++i)
    {
        ar_u32 rec = f->cmap + 4 + i * 8;
        ar_u32 plat = ar__u16at(f, rec);
        ar_u32 enc = ar__u16at(f, rec + 2);
        ar_u32 off = f->cmap + ar__u32at(f, rec + 4);
        ar_u32 format;
        ar_i32 score = -1;

        if (off >= f->size)
        {
            continue;
        }
        format = ar__u16at(f, off);
        if (format != 4 && format != 12)
        {
            continue; /* 0 and 6 exist; nothing shipping this century needs them */
        }

        /* Full Unicode beats the basic plane, and a Unicode table beats a
           symbol table that happens to be first in the file. */
        if (plat == 3 && enc == 10)
        {
            score = 4;
        }
        else if (plat == 0)
        {
            score = format == 12 ? 4 : 3;
        }
        else if (plat == 3 && enc == 1)
        {
            score = 2;
        }
        else if (plat == 3 && enc == 0)
        {
            score = 1;
        }

        if (score > best_score)
        {
            best_score = score;
            best = off;
            f->cmap_format = (ar_i32)format;
        }
    }
    f->cmap_sub = best;
}

static ar_i32 ar__cmap4(const ar_face *f, ar_u32 cp)
{
    ar_u32 sub = f->cmap_sub;
    ar_u32 seg2 = ar__u16at(f, sub + 6);
    ar_u32 ends = sub + 14;
    ar_u32 starts = ends + seg2 + 2;
    ar_u32 deltas = starts + seg2;
    ar_u32 ranges = deltas + seg2;
    ar_u32 i;

    if (cp > 0xFFFFu || seg2 == 0)
    {
        return 0;
    }

    /* Linear rather than binary: segment counts are in the low hundreds and
       every lookup this makes is behind a glyph cache. */
    for (i = 0; i < seg2; i += 2)
    {
        ar_u32 end = ar__u16at(f, ends + i);
        ar_u32 start;

        if (cp > end)
        {
            continue;
        }
        start = ar__u16at(f, starts + i);
        if (cp < start)
        {
            return 0;
        }

        {
            ar_u32 range = ar__u16at(f, ranges + i);
            ar_i32 delta = ar__i16at(f, deltas + i);
            ar_u32 g;

            if (range == 0)
            {
                g = (cp + (ar_u32)delta) & 0xFFFFu;
            }
            else
            {
                ar_u32 at = ranges + i + range + (cp - start) * 2;
                g = ar__u16at(f, at);
                if (g != 0)
                {
                    g = (g + (ar_u32)delta) & 0xFFFFu;
                }
            }
            return (ar_i32)g;
        }
    }
    return 0;
}

static ar_i32 ar__cmap12(const ar_face *f, ar_u32 cp)
{
    ar_u32 sub = f->cmap_sub;
    ar_u32 groups = ar__u32at(f, sub + 12);
    ar_u32 i;

    if (groups > (f->size - sub) / 12)
    {
        return 0;
    }
    for (i = 0; i < groups; ++i)
    {
        ar_u32 rec = sub + 16 + i * 12;
        ar_u32 first = ar__u32at(f, rec);
        ar_u32 last = ar__u32at(f, rec + 4);

        if (cp >= first && cp <= last)
        {
            return (ar_i32)(ar__u32at(f, rec + 8) + (cp - first));
        }
    }
    return 0;
}

ar_i32 ar_face_glyph(const ar_face *f, ar_u32 codepoint)
{
    ar_i32 g;

    if (!f->ok || !f->cmap_sub)
    {
        return 0;
    }
    g = f->cmap_format == 12 ? ar__cmap12(f, codepoint) : ar__cmap4(f, codepoint);
    return (g >= 0 && g < f->num_glyphs) ? g : 0;
}

/* ------------------------------------------------------------------------
 * Metrics
 * ------------------------------------------------------------------------ */
ar_i32 ar_face_advance(const ar_face *f, ar_i32 glyph)
{
    if (!f->ok || !f->hmtx || f->num_hmetrics <= 0)
    {
        return 0;
    }
    /* Runs of glyphs sharing the last advance are stored once, which is how a
       monospaced font is small. */
    if (glyph >= f->num_hmetrics)
    {
        glyph = f->num_hmetrics - 1;
    }
    if (glyph < 0)
    {
        return 0;
    }
    return (ar_i32)ar__u16at(f, f->hmtx + (ar_u32)glyph * 4);
}

/*
 * value * 64 * ppem / units_per_em, without the overflow the obvious spelling
 * has: a coordinate of 32767 times 64 times a ppem of 1000 is 2.1e9, which
 * does not fit in a signed 32 bit integer, and C89 has nothing wider.
 *
 * Splitting the value into whole ems and a remainder keeps both products
 * small: the remainder is under units_per_em by construction, so the second
 * term is bounded by units_per_em * 64 * ppem / units_per_em -- that is, by
 * 64 * ppem, whatever the coordinate was.
 */
ar_i32 ar_face_scale(const ar_face *f, ar_i32 value, ar_i32 ppem)
{
    ar_i32 upem = f->units_per_em;
    ar_i32 whole, rest, sign = 1;

    if (upem <= 0 || ppem <= 0)
    {
        return 0;
    }
    if (value < 0)
    {
        sign = -1;
        value = -value;
    }
    whole = value / upem;
    rest = value % upem;
    return sign * (whole * ppem * AR_ONE_PIXEL + rest * ppem * AR_ONE_PIXEL / upem);
}

/* ------------------------------------------------------------------------
 * glyf
 * ------------------------------------------------------------------------ */
static ar_u32 ar__glyph_offset(const ar_face *f, ar_i32 glyph, ar_u32 *len)
{
    ar_u32 a, b;

    *len = 0;
    if (glyph < 0 || glyph >= f->num_glyphs || !f->loca)
    {
        return 0;
    }
    if (f->loc_long)
    {
        a = ar__u32at(f, f->loca + (ar_u32)glyph * 4);
        b = ar__u32at(f, f->loca + (ar_u32)glyph * 4 + 4);
    }
    else
    {
        a = ar__u16at(f, f->loca + (ar_u32)glyph * 2) * 2;
        b = ar__u16at(f, f->loca + (ar_u32)glyph * 2 + 2) * 2;
    }
    if (b <= a || a >= f->glyf_len)
    {
        return 0; /* an empty glyph, which a space legitimately is */
    }
    if (b > f->glyf_len)
    {
        b = f->glyf_len;
    }
    *len = b - a;
    return f->glyf + a;
}

/*
 * Turns one decoded contour into path segments.
 *
 * TrueType stores quadratic outlines with a wrinkle: two consecutive
 * off-curve points imply an on-curve point at their midpoint, so a run of
 * control points is a chain of curves rather than an error. A contour can also
 * begin off-curve, in which case the start has to be synthesised the same way.
 */
static void ar__emit_contour(ar_path *p, const ar_i32 *xs, const ar_i32 *ys, const ar_u8 *on,
                             ar_i32 first, ar_i32 last)
{
    ar_i32 n = last - first + 1;
    ar_i32 startx, starty;
    ar_i32 i;

    if (n < 2)
    {
        return;
    }

    if (on[first])
    {
        startx = xs[first];
        starty = ys[first];
        i = first + 1;
    }
    else if (on[last])
    {
        startx = xs[last];
        starty = ys[last];
        i = first;
        last = last - 1;
    }
    else
    {
        /* Every point is off-curve, so the start is the implied midpoint. */
        startx = (xs[first] + xs[last]) / 2;
        starty = (ys[first] + ys[last]) / 2;
        i = first;
    }

    ar_path_move_to(p, startx, starty);

    {
        ar_i32 ctrlx = 0, ctrly = 0;
        ar_i32 have_ctrl = 0;

        for (; i <= last; ++i)
        {
            if (on[i])
            {
                if (have_ctrl)
                {
                    ar_path_quad_to(p, ctrlx, ctrly, xs[i], ys[i]);
                    have_ctrl = 0;
                }
                else
                {
                    ar_path_line_to(p, xs[i], ys[i]);
                }
            }
            else if (have_ctrl)
            {
                /* Two controls in a row: the on-curve point between them is
                   implied at their midpoint. */
                ar_i32 mx = (ctrlx + xs[i]) / 2;
                ar_i32 my = (ctrly + ys[i]) / 2;
                ar_path_quad_to(p, ctrlx, ctrly, mx, my);
                ctrlx = xs[i];
                ctrly = ys[i];
            }
            else
            {
                ctrlx = xs[i];
                ctrly = ys[i];
                have_ctrl = 1;
            }
        }

        if (have_ctrl)
        {
            ar_path_quad_to(p, ctrlx, ctrly, startx, starty);
        }
    }
    ar_path_close(p);
}

#define AR_COMPOSITE_MAX_DEPTH 4

static int ar__outline(const ar_face *f, ar_i32 glyph, ar_i32 ppem, ar_i32 ox, ar_i32 oy,
                       ar_path *p, const ar_outline_buf *buf, ar_i32 depth);

static int ar__simple_glyph(const ar_face *f, ar_u32 g, ar_u32 glen, ar_i32 ncont, ar_i32 ppem,
                            ar_i32 ox, ar_i32 oy, ar_path *p, const ar_outline_buf *buf)
{
    ar_u32 ends = g + 10;
    ar_u32 cursor;
    ar_i32 npoints, i, c;
    ar_i32 x = 0, y = 0;

    if (ncont <= 0 || (ar_u32)ncont * 2 + 12 > glen)
    {
        return 0;
    }

    npoints = (ar_i32)ar__u16at(f, ends + (ar_u32)(ncont - 1) * 2) + 1;
    if (npoints <= 0 || npoints > buf->cap)
    {
        return 0;
    }

    /* Skip the hinting bytecode. areole does not interpret it; the two cheap
       mitigations named in the release document replace it. */
    cursor = ends + (ar_u32)ncont * 2;
    cursor += 2 + ar__u16at(f, cursor);

    /* Flags, with the run-length encoding TrueType uses for repeats. */
    for (i = 0; i < npoints;)
    {
        ar_u32 flag = ar__u8at(f, cursor++);
        ar_i32 repeat = 1;

        if (flag & 0x08u)
        {
            repeat += (ar_i32)ar__u8at(f, cursor++);
        }
        while (repeat-- > 0 && i < npoints)
        {
            /* The x and y coordinate flags are needed by the next two passes,
               so the whole flag byte is kept and reduced to the on-curve bit
               only once the coordinates have been read. */
            buf->on[i] = (ar_u8)(flag & 0x37u);
            ++i;
        }
    }

    for (i = 0; i < npoints; ++i)
    {
        ar_u32 flag = buf->on[i];
        if (flag & 0x02u)
        {
            ar_i32 d = (ar_i32)ar__u8at(f, cursor++);
            x += (flag & 0x10u) ? d : -d;
        }
        else if (!(flag & 0x10u))
        {
            x += ar__i16at(f, cursor);
            cursor += 2;
        }
        buf->x[i] = x;
    }

    for (i = 0; i < npoints; ++i)
    {
        ar_u32 flag = buf->on[i];
        if (flag & 0x04u)
        {
            ar_i32 d = (ar_i32)ar__u8at(f, cursor++);
            y += (flag & 0x20u) ? d : -d;
        }
        else if (!(flag & 0x20u))
        {
            y += ar__i16at(f, cursor);
            cursor += 2;
        }
        buf->y[i] = y;
    }

    /* To 26.6 pixels, and the y axis flipped: a font measures up from the
       baseline, a bitmap measures down from the top. */
    for (i = 0; i < npoints; ++i)
    {
        buf->x[i] = ox + ar_face_scale(f, buf->x[i], ppem);
        buf->y[i] = oy - ar_face_scale(f, buf->y[i], ppem);
        buf->on[i] = (ar_u8)(buf->on[i] & 0x01u);
    }

    {
        ar_i32 first = 0;
        for (c = 0; c < ncont; ++c)
        {
            ar_i32 last = (ar_i32)ar__u16at(f, ends + (ar_u32)c * 2);
            if (last >= npoints)
            {
                last = npoints - 1;
            }
            if (last >= first)
            {
                ar__emit_contour(p, buf->x, buf->y, buf->on, first, last);
            }
            first = last + 1;
        }
    }
    return 1;
}

static int ar__composite_glyph(const ar_face *f, ar_u32 g, ar_i32 ppem, ar_i32 ox, ar_i32 oy,
                               ar_path *p, const ar_outline_buf *buf, ar_i32 depth)
{
    ar_u32 cursor = g + 10;
    ar_i32 guard = 0;

    for (;;)
    {
        ar_u32 flags = ar__u16at(f, cursor);
        ar_i32 index = (ar_i32)ar__u16at(f, cursor + 2);
        ar_i32 dx = 0, dy = 0;

        cursor += 4;

        if (flags & 0x0001u) /* arguments are words */
        {
            dx = ar__i16at(f, cursor);
            dy = ar__i16at(f, cursor + 2);
            cursor += 4;
        }
        else
        {
            ar_u32 a = ar__u8at(f, cursor);
            ar_u32 b = ar__u8at(f, cursor + 1);
            dx = a >= 0x80u ? (ar_i32)a - 256 : (ar_i32)a;
            dy = b >= 0x80u ? (ar_i32)b - 256 : (ar_i32)b;
            cursor += 2;
        }

        /* Scaled and two-by-two components exist, and skipping their transform
           places the component correctly but unrotated. Accented Latin, which
           is what composites are overwhelmingly used for, uses translation
           only, so this is a visible limitation for very little of the corpus
           and it is named rather than hidden. */
        if (flags & 0x0008u)
        {
            cursor += 2;
        }
        else if (flags & 0x0040u)
        {
            cursor += 4;
        }
        else if (flags & 0x0080u)
        {
            cursor += 8;
        }

        if (!(flags & 0x0002u)) /* args are point indices, not offsets */
        {
            dx = 0;
            dy = 0;
        }

        ar__outline(f, index, ppem, ox + ar_face_scale(f, dx, ppem),
                    oy - ar_face_scale(f, dy, ppem), p, buf, depth + 1);

        if (!(flags & 0x0020u)) /* no more components */
        {
            break;
        }
        /* A font that points a component at itself would otherwise loop here
           until the stack ran out. Depth alone does not catch a long chain of
           siblings, so the count is bounded too. */
        if (++guard > 32 || cursor >= f->size)
        {
            break;
        }
    }
    return 1;
}

static int ar__outline(const ar_face *f, ar_i32 glyph, ar_i32 ppem, ar_i32 ox, ar_i32 oy,
                       ar_path *p, const ar_outline_buf *buf, ar_i32 depth)
{
    ar_u32 glen;
    ar_u32 g;
    ar_i32 ncont;

    if (!f->ok || depth > AR_COMPOSITE_MAX_DEPTH)
    {
        return 0;
    }
    if (f->cff)
    {
        return ar_cff_outline(f, glyph, ppem, ox, oy, p);
    }

    g = ar__glyph_offset(f, glyph, &glen);
    if (!g || glen < 10)
    {
        return 0; /* an empty glyph: a space, or one this font omits */
    }

    ncont = ar__i16at(f, g);
    if (ncont >= 0)
    {
        return ar__simple_glyph(f, g, glen, ncont, ppem, ox, oy, p, buf);
    }
    return ar__composite_glyph(f, g, ppem, ox, oy, p, buf, depth);
}

int ar_face_outline(const ar_face *f, ar_i32 glyph, ar_i32 ppem, ar_i32 ox, ar_i32 oy, ar_path *p,
                    const ar_outline_buf *buf)
{
    if (!buf || !buf->x || !buf->y || !buf->on || buf->cap <= 0)
    {
        return 0;
    }
    return ar__outline(f, glyph, ppem, ox, oy, p, buf, 0);
}

/*
 * The family name, copied into the caller's buffer as ASCII.
 *
 * The name table stores strings in whichever encoding the platform record
 * says, and the two that matter are Windows UTF-16BE and Macintosh Roman.
 * Non-ASCII is replaced rather than transcoded: this exists so a stylesheet
 * can say Helvetica and an application can log which face it got, and both of
 * those are ASCII questions.
 */
ar_i32 ar_face_family(const ar_face *f, char *out, ar_i32 cap)
{
    ar_u32 count, offset, i;
    ar_u32 best = 0, best_len = 0;
    int    best_utf16 = 0;
    ar_i32 n = 0;

    if (cap > 0)
    {
        out[0] = 0;
    }
    if (!f->ok || !f->name || cap <= 1)
    {
        return 0;
    }

    count = ar__u16at(f, f->name + 2);
    offset = f->name + ar__u16at(f, f->name + 4);
    if (count > (f->size - f->name) / 12)
    {
        return 0;
    }

    for (i = 0; i < count; ++i)
    {
        ar_u32 rec = f->name + 6 + i * 12;
        ar_u32 plat = ar__u16at(f, rec);
        ar_u32 enc = ar__u16at(f, rec + 2);
        ar_u32 nid = ar__u16at(f, rec + 6);
        ar_u32 len = ar__u16at(f, rec + 8);
        ar_u32 at = offset + ar__u16at(f, rec + 10);

        if (nid != 1 || len == 0 || at + len > f->size)
        {
            continue; /* name ID 1 is the family */
        }
        /* Windows Unicode preferred; Macintosh Roman accepted. */
        if (plat == 3 && (enc == 1 || enc == 0))
        {
            best = at;
            best_len = len;
            best_utf16 = 1;
            break;
        }
        if (plat == 1 && best == 0)
        {
            best = at;
            best_len = len;
            best_utf16 = 0;
        }
    }

    if (!best)
    {
        return 0;
    }
    if (best_utf16)
    {
        for (i = 0; i + 1 < best_len && n < cap - 1; i += 2)
        {
            ar_u32 c = ar__u16at(f, best + i);
            out[n++] = (char)(c < 128u ? c : '?');
        }
    }
    else
    {
        for (i = 0; i < best_len && n < cap - 1; ++i)
        {
            ar_u32 c = ar__u8at(f, best + i);
            out[n++] = (char)(c < 128u ? c : '?');
        }
    }
    out[n] = 0;
    return n;
}

void ar_face_grid_fit(const ar_face *f, ar_i32 ppem, ar_i32 *num, ar_i32 *den)
{
    ar_i32 xh, fitted;

    *num = 1;
    *den = 1;
    if (!f->ok || f->x_height <= 0 || ppem <= 0)
    {
        return;
    }

    /* The x-height in 26.6 pixels, and what it would be rounded to the grid. */
    xh = ar_face_scale(f, f->x_height, ppem);
    if (xh <= 0)
    {
        return;
    }
    fitted = ((xh + AR_ONE_PIXEL / 2) / AR_ONE_PIXEL) * AR_ONE_PIXEL;
    if (fitted <= 0)
    {
        return;
    }

    /* Refuse a correction larger than an eighth. Below about 8 px the rounding
       is a large fraction of the height and fitting it distorts the face
       instead of sharpening it, which is exactly when a reader most needs the
       shapes to be recognisable. */
    if (fitted * 8 > xh * 9 || fitted * 9 < xh * 8)
    {
        return;
    }

    *num = fitted;
    *den = xh;
}

/* ------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------ */
int ar_face_init(ar_face *f, const void *data, ar_u32 size)
{
    ar_u32 version;

    memset(f, 0, sizeof *f);
    if (!data || size < 12)
    {
        return 0;
    }
    f->data = (const ar_u8 *)data;
    f->size = size;

    version = ar__u32at(f, 0);
    /* 0x00010000 is TrueType, 'true' is the Apple spelling of the same thing,
       'OTTO' is PostScript outlines in a CFF table. 'ttcf' is a collection and
       is still refused: picking one face out of it needs an index the caller
       has no way to pass yet. */
    if (version != 0x00010000u && version != ar__tag("true") && version != ar__tag("OTTO"))
    {
        return 0;
    }

    f->os2 = ar__find_table(f, "OS/2", 0);
    f->name = ar__find_table(f, "name", 0);
    f->gsub = ar__find_table(f, "GSUB", 0);
    f->gpos = ar__find_table(f, "GPOS", 0);
    f->kern = ar__find_table(f, "kern", 0);
    f->head = ar__find_table(f, "head", 0);
    f->hhea = ar__find_table(f, "hhea", 0);
    f->maxp = ar__find_table(f, "maxp", 0);
    f->cmap = ar__find_table(f, "cmap", 0);
    f->hmtx = ar__find_table(f, "hmtx", 0);
    f->loca = ar__find_table(f, "loca", &f->loca_len);
    f->glyf = ar__find_table(f, "glyf", &f->glyf_len);

    if (!f->head || !f->maxp)
    {
        return 0;
    }

    f->units_per_em = (ar_i32)ar__u16at(f, f->head + 18);
    f->loc_long = ar__i16at(f, f->head + 50) != 0;
    f->num_glyphs = (ar_i32)ar__u16at(f, f->maxp + 4);

    if (f->hhea)
    {
        f->ascender = ar__i16at(f, f->hhea + 4);
        f->descender = ar__i16at(f, f->hhea + 6);
        f->line_gap = ar__i16at(f, f->hhea + 8);
        f->num_hmetrics = (ar_i32)ar__u16at(f, f->hhea + 34);
    }

    /* sxHeight and sCapHeight only exist in OS/2 version 2 and later, and
       plenty of shipping fonts still write version 1. Zero means "not stated"
       and the caller falls back to measuring a glyph. */
    if (f->os2 && ar__u16at(f, f->os2) >= 2)
    {
        f->x_height = ar__i16at(f, f->os2 + 86);
        f->cap_height = ar__i16at(f, f->os2 + 88);
    }

    if (f->units_per_em <= 0 || f->num_glyphs <= 0)
    {
        return 0;
    }

    /* One kind of outline or the other. A font with neither is not one this
       can draw, whatever else it contains. */
    if (!f->glyf || !f->loca)
    {
        ar_u32 cff = ar__find_table(f, "CFF ", 0);
        if (!cff || !ar_cff_init(f, cff))
        {
            return 0;
        }
    }

    ar__pick_cmap(f);

    f->ok = 1;
    return 1;
}
