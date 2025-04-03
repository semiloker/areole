/*
 * areole - OpenType shaping.
 * SPDX-License-Identifier: MIT
 */
#include "ar_shape.h"

#include <string.h>

static ar_u32 ar__sh_u16(const ar_face *f, ar_u32 off)
{
    if (off + 1 >= f->size)
    {
        return 0;
    }
    return ((ar_u32)f->data[off] << 8) | f->data[off + 1];
}

static ar_i32 ar__sh_i16(const ar_face *f, ar_u32 off)
{
    ar_u32 v = ar__sh_u16(f, off);
    return v >= 0x8000u ? (ar_i32)v - 0x10000 : (ar_i32)v;
}

static ar_u32 ar__sh_u32(const ar_face *f, ar_u32 off)
{
    if (off + 3 >= f->size)
    {
        return 0;
    }
    return ((ar_u32)f->data[off] << 24) | ((ar_u32)f->data[off + 1] << 16) |
           ((ar_u32)f->data[off + 2] << 8) | f->data[off + 3];
}

/* ------------------------------------------------------------------------
 * Coverage and class definition
 *
 * Both come in two formats, a list and a set of ranges, and every lookup in
 * both tables is reached through one of them. Getting either wrong shows up as
 * shaping applying to the wrong glyphs, which looks like font corruption.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__coverage(const ar_face *f, ar_u32 at, ar_i32 glyph)
{
    ar_u32 format = ar__sh_u16(f, at);
    ar_u32 count = ar__sh_u16(f, at + 2);
    ar_u32 i;

    if (glyph < 0)
    {
        return -1;
    }
    if (format == 1)
    {
        for (i = 0; i < count; ++i)
        {
            if ((ar_i32)ar__sh_u16(f, at + 4 + i * 2) == glyph)
            {
                return (ar_i32)i;
            }
        }
        return -1;
    }
    if (format == 2)
    {
        for (i = 0; i < count; ++i)
        {
            ar_u32 rec = at + 4 + i * 6;
            ar_i32 lo = (ar_i32)ar__sh_u16(f, rec);
            ar_i32 hi = (ar_i32)ar__sh_u16(f, rec + 2);
            if (glyph >= lo && glyph <= hi)
            {
                return (ar_i32)ar__sh_u16(f, rec + 4) + (glyph - lo);
            }
        }
    }
    return -1;
}

static ar_i32 ar__class_of(const ar_face *f, ar_u32 at, ar_i32 glyph)
{
    ar_u32 format;

    if (!at)
    {
        return 0;
    }
    format = ar__sh_u16(f, at);
    if (format == 1)
    {
        ar_i32 start = (ar_i32)ar__sh_u16(f, at + 2);
        ar_u32 count = ar__sh_u16(f, at + 4);
        if (glyph >= start && glyph < start + (ar_i32)count)
        {
            return (ar_i32)ar__sh_u16(f, at + 6 + (ar_u32)(glyph - start) * 2);
        }
        return 0;
    }
    if (format == 2)
    {
        ar_u32 count = ar__sh_u16(f, at + 2);
        ar_u32 i;
        for (i = 0; i < count; ++i)
        {
            ar_u32 rec = at + 4 + i * 6;
            if (glyph >= (ar_i32)ar__sh_u16(f, rec) && glyph <= (ar_i32)ar__sh_u16(f, rec + 2))
            {
                return (ar_i32)ar__sh_u16(f, rec + 4);
            }
        }
    }
    return 0;
}

/* A value record's size depends on which of eight optional fields are present,
   and a pair position table is walked by stride, so this has to be right or
   every pair after the first is read from the wrong place. */
static ar_u32 ar__value_size(ar_u32 format)
{
    ar_u32 n = 0, bit;

    for (bit = 0; bit < 8; ++bit)
    {
        if (format & (1u << bit))
        {
            n += 2;
        }
    }
    return n;
}

/* Only the horizontal advance matters for kerning left-to-right text; the rest
   of a value record is placement and vertical, which mark attachment will need
   and pair positioning does not. */
static ar_i32 ar__value_xadvance(const ar_face *f, ar_u32 at, ar_u32 format)
{
    ar_u32 off = 0;

    if (!(format & 0x0004u))
    {
        return 0;
    }
    if (format & 0x0001u)
    {
        off += 2;
    }
    if (format & 0x0002u)
    {
        off += 2;
    }
    return ar__sh_i16(f, at + off);
}

/* ------------------------------------------------------------------------
 * Feature selection
 * ------------------------------------------------------------------------ */
static ar_u32 ar__tag4(const char *s)
{
    return ((ar_u32)(ar_u8)s[0] << 24) | ((ar_u32)(ar_u8)s[1] << 16) | ((ar_u32)(ar_u8)s[2] << 8) |
           (ar_u8)s[3];
}

/*
 * Collects the lookups a feature uses.
 *
 * Script and language selection is deliberately blunt: every script's default
 * language system is consulted rather than one chosen from the run. For the
 * features here that is the same answer -- a font does not kern Latin
 * differently depending on whether the run was tagged latn or DFLT -- and it
 * avoids carrying a script detector that only complex shaping would need.
 */
static void ar__collect(const ar_face *f, ar_u32 table, const char *tag, ar_u32 *out,
                        ar_i32 *count, ar_i32 max)
{
    ar_u32 want = ar__tag4(tag);
    ar_u32 features, lookups, nfeat, i;

    *count = 0;
    if (!table)
    {
        return;
    }
    features = table + ar__sh_u16(f, table + 6);
    lookups = table + ar__sh_u16(f, table + 8);
    nfeat = ar__sh_u16(f, features);
    if (nfeat > 4096)
    {
        return;
    }

    for (i = 0; i < nfeat; ++i)
    {
        ar_u32 rec = features + 2 + i * 6;
        ar_u32 feat, nlook, j;

        if (ar__sh_u32(f, rec) != want)
        {
            continue;
        }
        feat = features + ar__sh_u16(f, rec + 4);
        nlook = ar__sh_u16(f, feat + 2);
        for (j = 0; j < nlook && *count < max; ++j)
        {
            ar_u32 idx = ar__sh_u16(f, feat + 4 + j * 2);
            if (idx < ar__sh_u16(f, lookups))
            {
                ar_u32 at = lookups + ar__sh_u16(f, lookups + 2 + idx * 2);
                ar_i32 k;
                int    seen = 0;
                /* The same lookup is referenced once per script that uses it,
                   and applying it twice would apply the substitution twice. */
                for (k = 0; k < *count; ++k)
                {
                    if (out[k] == at)
                    {
                        seen = 1;
                        break;
                    }
                }
                if (!seen)
                {
                    out[(*count)++] = at;
                }
            }
        }
    }
}

int ar_shape_init(ar_shaper *sh, const ar_face *face)
{
    memset(sh, 0, sizeof *sh);
    if (!face || !face->ok)
    {
        return 0;
    }
    sh->face = face;
    sh->gsub = face->gsub;
    sh->gpos = face->gpos;
    sh->kern = face->kern;

    ar__collect(face, sh->gsub, "liga", sh->liga, &sh->liga_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gpos, "kern", sh->kerns, &sh->kern_count, AR_SHAPE_MAX_LOOKUPS);

    sh->ok = 1;
    return 1;
}

/*
 * One subtable of a lookup, with extensions unwrapped.
 *
 * A lookup's offsets to its subtables are 16 bit, so a font whose tables grew
 * past 64 KB cannot reach them. The way out is an extension subtable: a
 * wrapper carrying the real type and a 32 bit offset. GSUB calls it type 7 and
 * GPOS type 9.
 *
 * Every lookup in Segoe UI is wrapped this way -- its liga lookups report type
 * 7 and its kern lookups type 9 -- so a shaper that does not unwrap them finds
 * nothing at all and silently applies no shaping. Which is exactly what this
 * one did until it was measured against a real font.
 */
static ar_u32 ar__subtable(const ar_face *f, ar_u32 lookup, ar_u32 j, ar_i32 *type)
{
    ar_u32 t = ar__sh_u16(f, lookup);
    ar_u32 at = lookup + ar__sh_u16(f, lookup + 6 + j * 2);

    if ((t == 7 || t == 9) && ar__sh_u16(f, at) == 1)
    {
        *type = (ar_i32)ar__sh_u16(f, at + 2);
        return at + ar__sh_u32(f, at + 4);
    }
    *type = (ar_i32)t;
    return at;
}

/* ------------------------------------------------------------------------
 * GSUB type 4: ligatures
 * ------------------------------------------------------------------------ */
static ar_i32 ar__try_ligature(const ar_face *f, ar_u32 sub, const ar_i32 *glyphs, ar_i32 count,
                               ar_i32 at, ar_i32 *consumed)
{
    ar_i32 cov;
    ar_u32 set, nlig, i;

    if (ar__sh_u16(f, sub) != 1)
    {
        return -1;
    }
    cov = ar__coverage(f, sub + ar__sh_u16(f, sub + 2), glyphs[at]);
    if (cov < 0 || cov >= (ar_i32)ar__sh_u16(f, sub + 4))
    {
        return -1;
    }

    set = sub + ar__sh_u16(f, sub + 6 + (ar_u32)cov * 2);
    nlig = ar__sh_u16(f, set);

    for (i = 0; i < nlig; ++i)
    {
        ar_u32 lig = set + ar__sh_u16(f, set + 2 + i * 2);
        ar_u32 ncomp = ar__sh_u16(f, lig + 2);
        ar_u32 k;
        int    match = 1;

        if (ncomp < 1 || at + (ar_i32)ncomp > count)
        {
            continue;
        }
        /* The first component is the covered glyph itself and is not stored,
           which is the detail that makes this table compact and this loop look
           off by one. */
        for (k = 1; k < ncomp; ++k)
        {
            if (glyphs[at + (ar_i32)k] != (ar_i32)ar__sh_u16(f, lig + 2 + k * 2))
            {
                match = 0;
                break;
            }
        }
        if (match)
        {
            *consumed = (ar_i32)ncomp;
            return (ar_i32)ar__sh_u16(f, lig);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------
 * GPOS type 2: pair positioning
 * ------------------------------------------------------------------------ */
static ar_i32 ar__pair_adjust(const ar_face *f, ar_u32 sub, ar_i32 a, ar_i32 b)
{
    ar_u32 format = ar__sh_u16(f, sub);
    ar_u32 vf1 = ar__sh_u16(f, sub + 4);
    ar_u32 vf2 = ar__sh_u16(f, sub + 6);
    ar_i32 cov = ar__coverage(f, sub + ar__sh_u16(f, sub + 2), a);

    if (cov < 0)
    {
        return 0;
    }

    if (format == 1)
    {
        ar_u32 nsets = ar__sh_u16(f, sub + 8);
        ar_u32 set, npair, i, stride;

        if ((ar_u32)cov >= nsets)
        {
            return 0;
        }
        set = sub + ar__sh_u16(f, sub + 10 + (ar_u32)cov * 2);
        npair = ar__sh_u16(f, set);
        stride = 2 + ar__value_size(vf1) + ar__value_size(vf2);

        for (i = 0; i < npair; ++i)
        {
            ar_u32 rec = set + 2 + i * stride;
            if ((ar_i32)ar__sh_u16(f, rec) == b)
            {
                return ar__value_xadvance(f, rec + 2, vf1);
            }
        }
        return 0;
    }

    if (format == 2)
    {
        ar_u32 cd1 = sub + ar__sh_u16(f, sub + 8);
        ar_u32 cd2 = sub + ar__sh_u16(f, sub + 10);
        ar_u32 n1 = ar__sh_u16(f, sub + 12);
        ar_u32 n2 = ar__sh_u16(f, sub + 14);
        ar_i32 c1 = ar__class_of(f, cd1, a);
        ar_i32 c2 = ar__class_of(f, cd2, b);
        ar_u32 v1 = ar__value_size(vf1), v2 = ar__value_size(vf2);

        if (c1 < 0 || c2 < 0 || (ar_u32)c1 >= n1 || (ar_u32)c2 >= n2)
        {
            return 0;
        }
        return ar__value_xadvance(f, sub + 16 + ((ar_u32)c1 * n2 + (ar_u32)c2) * (v1 + v2), vf1);
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The legacy kern table
 *
 * Plenty of fonts still ship kerning only here, and a font with no GPOS looks
 * conspicuously unkerned without it. Format 0 only: the others are Apple
 * extensions that essentially nothing uses.
 * ------------------------------------------------------------------------ */
static ar_i32 ar__legacy_kern(const ar_face *f, ar_u32 kern, ar_i32 a, ar_i32 b)
{
    ar_u32 ntables = ar__sh_u16(f, kern + 2);
    ar_u32 at = kern + 4;
    ar_u32 t;

    if (ar__sh_u16(f, kern) != 0 || ntables > 32)
    {
        return 0;
    }
    for (t = 0; t < ntables; ++t)
    {
        ar_u32 len = ar__sh_u16(f, at + 2);
        ar_u32 cov = ar__sh_u16(f, at + 4);
        ar_u32 want = ((ar_u32)a << 16) | (ar_u32)b;
        ar_u32 npairs, i;

        if ((cov & 0xFF00u) != 0x0000u || !(cov & 1u))
        {
            at += len ? len : 6;
            continue; /* not a horizontal format 0 subtable */
        }
        npairs = ar__sh_u16(f, at + 6);
        for (i = 0; i < npairs; ++i)
        {
            ar_u32 rec = at + 14 + i * 6;
            if (ar__sh_u32(f, rec) == want)
            {
                return ar__sh_i16(f, rec + 4);
            }
        }
        at += len ? len : 6;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
ar_i32 ar_shape_run(const ar_shaper *sh, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *cluster,
                    ar_i32 count)
{
    const ar_face *f;
    ar_i32         i, k, out;

    if (!sh || !sh->ok || !glyphs || count <= 0)
    {
        return count;
    }
    f = sh->face;

    /* Ligatures first: kerning is between the glyphs that will be drawn, and
       fi kerns as one glyph rather than as an f and an i. */
    out = 0;
    for (i = 0; i < count;)
    {
        ar_i32 replaced = -1, consumed = 1;

        for (k = 0; k < sh->liga_count && replaced < 0; ++k)
        {
            ar_u32 lookup = sh->liga[k];
            ar_u32 nsub = ar__sh_u16(f, lookup + 4);
            ar_u32 j;

            for (j = 0; j < nsub && replaced < 0; ++j)
            {
                ar_i32 type;
                ar_u32 sub = ar__subtable(f, lookup, j, &type);
                if (type != 4)
                {
                    continue;
                }
                replaced = ar__try_ligature(f, sub, glyphs, count, i, &consumed);
            }
        }

        if (replaced >= 0 && consumed >= 1)
        {
            glyphs[out] = replaced;
            if (cluster)
            {
                /* The whole ligature belongs to the first character it came
                   from, so a caret placed before it lands before the f of fi
                   rather than inside a glyph that has no inside. */
                cluster[out] = cluster ? cluster[i] : i;
            }
            if (adv)
            {
                adv[out] = adv[i]; /* the ligature carries its own advance */
            }
            i += consumed;
            ++out;
        }
        else
        {
            glyphs[out] = glyphs[i];
            if (cluster)
            {
                cluster[out] = cluster[i];
            }
            if (adv)
            {
                adv[out] = adv[i];
            }
            ++i;
            ++out;
        }
    }
    count = out;

    /* Then kerning, over what is left. */
    if (adv)
    {
        for (i = 0; i + 1 < count; ++i)
        {
            ar_i32 d = 0;

            for (k = 0; k < sh->kern_count && d == 0; ++k)
            {
                ar_u32 lookup = sh->kerns[k];
                ar_u32 nsub = ar__sh_u16(f, lookup + 4);
                ar_u32 j;

                for (j = 0; j < nsub && d == 0; ++j)
                {
                    ar_i32 type;
                    ar_u32 sub = ar__subtable(f, lookup, j, &type);
                    if (type != 2)
                    {
                        continue;
                    }
                    d = ar__pair_adjust(f, sub, glyphs[i], glyphs[i + 1]);
                }
            }

            if (d == 0 && sh->kern && sh->kern_count == 0)
            {
                d = ar__legacy_kern(f, sh->kern, glyphs[i], glyphs[i + 1]);
            }
            adv[i] += d;
        }
    }

    return count;
}
