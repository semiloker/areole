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
    ar__collect(face, sh->gsub, "init", sh->init, &sh->init_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gsub, "medi", sh->medi, &sh->medi_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gsub, "fina", sh->fina, &sh->fina_count, AR_SHAPE_MAX_LOOKUPS);
    /* rlig is required rather than optional in Arabic: lam-alef must ligate or
       the text is wrong, not merely plain. */
    ar__collect(face, sh->gsub, "rlig", sh->rlig, &sh->rlig_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gpos, "mark", sh->mark, &sh->mark_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gpos, "mkmk", sh->mkmk, &sh->mkmk_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gsub, "ccmp", sh->ccmp, &sh->ccmp_count, AR_SHAPE_MAX_LOOKUPS);
    ar__collect(face, sh->gsub, "calt", sh->calt, &sh->calt_count, AR_SHAPE_MAX_LOOKUPS);
    if (sh->gsub)
    {
        sh->gsub_lookups = sh->gsub + ar__sh_u16(face, sh->gsub + 8);
    }

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


/*
 * Arabic joining.
 *
 * Every Arabic letter has up to four shapes, and which one it takes depends on
 * what it joins to on each side. Most letters join both ways; a handful --
 * alef, dal, ra, waw and their relatives -- join only to the right, so the
 * letter after them always starts a new group. Marks are transparent: a
 * fatha between two letters does not stop them joining.
 *
 * Getting this wrong does not produce garbage, it produces a row of isolated
 * letters. Which is readable in the way that S P A C E D  C A P I T A L S are
 * readable, and is how unshaped Arabic looks everywhere it appears.
 */
ar_i32 ar_join_type(ar_u32 cp)
{
    struct jrange
    {
        ar_u32 lo, hi;
        ar_u8  t;
    };
    /* The right-joining letters, listed because they are the exceptions and
       the rest of the block is dual-joining. */
    static const struct jrange J[] = {
        {0x0600, 0x0605, AR_JOIN_U}, {0x0610, 0x061A, AR_JOIN_T}, {0x0620, 0x0620, AR_JOIN_D},
        {0x0621, 0x0621, AR_JOIN_U}, /* hamza */
        {0x0622, 0x0625, AR_JOIN_R}, /* alef with madda, hamza above/below */
        {0x0626, 0x0626, AR_JOIN_D}, {0x0627, 0x0627, AR_JOIN_R}, /* alef */
        {0x0628, 0x0628, AR_JOIN_D}, {0x0629, 0x0629, AR_JOIN_R}, /* teh marbuta */
        {0x062A, 0x062E, AR_JOIN_D},
        {0x062F, 0x0632, AR_JOIN_R}, /* dal, thal, ra, zain */
        {0x0633, 0x063F, AR_JOIN_D}, {0x0640, 0x0640, AR_JOIN_C}, /* tatweel */
        {0x0641, 0x0647, AR_JOIN_D}, {0x0648, 0x0648, AR_JOIN_R}, /* waw */
        {0x0649, 0x064A, AR_JOIN_D}, {0x064B, 0x065F, AR_JOIN_T},
        {0x0660, 0x066F, AR_JOIN_U}, {0x0670, 0x0670, AR_JOIN_T},
        {0x0671, 0x0673, AR_JOIN_R}, {0x0674, 0x0674, AR_JOIN_U},
        {0x0675, 0x0677, AR_JOIN_R}, {0x0678, 0x0688, AR_JOIN_D},
        {0x0689, 0x0699, AR_JOIN_R}, {0x069A, 0x06BF, AR_JOIN_D},
        {0x06C0, 0x06CB, AR_JOIN_R}, {0x06CC, 0x06CC, AR_JOIN_D},
        {0x06CD, 0x06CE, AR_JOIN_R}, {0x06CF, 0x06CF, AR_JOIN_R},
        {0x06D0, 0x06D3, AR_JOIN_D}, {0x06D4, 0x06D4, AR_JOIN_U},
        {0x06D5, 0x06D5, AR_JOIN_R}, {0x06D6, 0x06ED, AR_JOIN_T},
        {0x06EE, 0x06EF, AR_JOIN_R}, {0x06F0, 0x06F9, AR_JOIN_U},
        {0x06FA, 0x06FF, AR_JOIN_D},
        {0x0710, 0x0710, AR_JOIN_R}, {0x0711, 0x0711, AR_JOIN_T}, /* Syriac */
        {0x0712, 0x072F, AR_JOIN_D}, {0x0730, 0x074A, AR_JOIN_T},
        {0x0750, 0x077F, AR_JOIN_D},
        {0x200C, 0x200C, AR_JOIN_U}, /* zero width non-joiner */
        {0x200D, 0x200D, AR_JOIN_C}  /* zero width joiner     */
    };
    ar_i32 i, n = (ar_i32)(sizeof J / sizeof J[0]);

    for (i = 0; i < n; ++i)
    {
        if (cp >= J[i].lo && cp <= J[i].hi)
        {
            return J[i].t;
        }
    }
    return AR_JOIN_U;
}

/* GSUB type 1: one glyph becomes one other glyph. Both formats: a delta added
   to the glyph id, or a table of replacements indexed by coverage. */
static ar_i32 ar__single_sub(const ar_face *f, ar_u32 sub, ar_i32 glyph)
{
    ar_u32 format = ar__sh_u16(f, sub);
    ar_i32 cov = ar__coverage(f, sub + ar__sh_u16(f, sub + 2), glyph);

    if (cov < 0)
    {
        return -1;
    }
    if (format == 1)
    {
        return (glyph + ar__sh_i16(f, sub + 4)) & 0xFFFF;
    }
    if (format == 2 && (ar_u32)cov < ar__sh_u16(f, sub + 4))
    {
        return (ar_i32)ar__sh_u16(f, sub + 6 + (ar_u32)cov * 2);
    }
    return -1;
}

/* Applies the first single-substitution lookup in a feature that covers this
   glyph. A font lists one lookup per script; the first that has the glyph is
   the one that meant it. */
static ar_i32 ar__apply_single(const ar_shaper *sh, const ar_u32 *lookups, ar_i32 nlookups,
                               ar_i32 glyph)
{
    const ar_face *f = sh->face;
    ar_i32         k;

    for (k = 0; k < nlookups; ++k)
    {
        ar_u32 lookup = lookups[k];
        ar_u32 nsub = ar__sh_u16(f, lookup + 4);
        ar_u32 j;

        for (j = 0; j < nsub; ++j)
        {
            ar_i32 type;
            ar_u32 sub = ar__subtable(f, lookup, j, &type);
            ar_i32 g;

            if (type != 1)
            {
                continue;
            }
            g = ar__single_sub(f, sub, glyph);
            if (g >= 0)
            {
                return g;
            }
        }
    }
    return -1;
}


/*
 * GSUB type 2: one glyph becomes several.
 *
 * Used by ccmp to take a character apart so that later rules can see its
 * pieces -- a precomposed letter-with-accent split into the letter and the
 * mark, so that mark positioning can then place the mark properly rather than
 * being stuck with whatever the composed glyph looks like.
 *
 * It is the only substitution that makes a run longer, which is why this is
 * the only one that needs to know the buffer's capacity. A sequence that will
 * not fit is skipped: leaving the character composed is a worse rendering,
 * writing past the buffer is a worse program.
 */
static ar_i32 ar__multiple_sub(const ar_face *f, ar_u32 sub, ar_i32 glyph, ar_i32 *out,
                               ar_i32 room)
{
    ar_i32 cov;
    ar_u32 seq, n, i;

    if (ar__sh_u16(f, sub) != 1)
    {
        return -1;
    }
    cov = ar__coverage(f, sub + ar__sh_u16(f, sub + 2), glyph);
    if (cov < 0 || (ar_u32)cov >= ar__sh_u16(f, sub + 4))
    {
        return -1;
    }
    seq = sub + ar__sh_u16(f, sub + 6 + (ar_u32)cov * 2);
    n = ar__sh_u16(f, seq);
    if (n == 0 || (ar_i32)n > room)
    {
        return -1;
    }
    for (i = 0; i < n; ++i)
    {
        out[i] = (ar_i32)ar__sh_u16(f, seq + 2 + i * 2);
    }
    return (ar_i32)n;
}

/*
 * ccmp, applied before anything else because that is what it is for. Handles
 * both single substitution and decomposition, which is all any font uses it
 * for in practice.
 */
static ar_i32 ar__apply_ccmp(const ar_shaper *sh, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *cluster,
                             ar_i32 count, ar_i32 cap)
{
    const ar_face *f = sh->face;
    ar_i32         i, k;

    if (sh->ccmp_count <= 0)
    {
        return count;
    }

    for (i = 0; i < count; ++i)
    {
        for (k = 0; k < sh->ccmp_count; ++k)
        {
            ar_u32 lookup = sh->ccmp[k];
            ar_u32 nsub = ar__sh_u16(f, lookup + 4);
            ar_u32 j;
            int    done = 0;

            for (j = 0; j < nsub && !done; ++j)
            {
                ar_i32 type;
                ar_u32 sub = ar__subtable(f, lookup, j, &type);

                if (type == 1)
                {
                    ar_i32 g = ar__single_sub(f, sub, glyphs[i]);
                    if (g >= 0)
                    {
                        glyphs[i] = g;
                        done = 1;
                    }
                }
                else if (type == 2)
                {
                    ar_i32 parts[8];
                    ar_i32 n = ar__multiple_sub(f, sub, glyphs[i], parts,
                                                cap - count + 1 < 8 ? cap - count + 1 : 8);
                    if (n > 0)
                    {
                        ar_i32 m;
                        /* Open a gap and drop the parts into it. Everything a
                           glyph carries moves with it: the advance is taken
                           from the face afterwards, but the cluster must stay
                           pointing at the character this came from or a caret
                           lands in the wrong place. */
                        for (m = count - 1; m > i; --m)
                        {
                            glyphs[m + n - 1] = glyphs[m];
                            if (adv)
                            {
                                adv[m + n - 1] = adv[m];
                            }
                            if (cluster)
                            {
                                cluster[m + n - 1] = cluster[m];
                            }
                        }
                        for (m = 0; m < n; ++m)
                        {
                            glyphs[i + m] = parts[m];
                            if (adv)
                            {
                                adv[i + m] = ar_face_advance(f, parts[m]);
                            }
                            if (cluster)
                            {
                                cluster[i + m] = cluster[i];
                            }
                        }
                        count += n - 1;
                        i += n - 1;
                        done = 1;
                    }
                }
            }
            if (done)
            {
                break;
            }
        }
    }
    return count;
}


/*
 * GSUB type 6: chained contextual substitution.
 *
 * This is the one lookup that substitutes nothing itself. It matches a pattern
 * -- some glyphs before, some at the position, some after -- and then names
 * other lookups by index to run at offsets inside the match. It is how a font
 * says "an f becomes a different f, but only when a b follows it", and it is
 * what drives contextual alternates and most of what complex scripts need.
 *
 * Format 3 is implemented, because it is what fonts produced this century use:
 * three lists of coverage tables, one per part of the context. Formats 1 and 2
 * express the same thing through rule sets keyed on a glyph or a class, and a
 * lookup in one of those formats is skipped rather than half applied.
 */
static ar_i32 ar__apply_one_lookup(const ar_shaper *sh, ar_u32 lookup, ar_i32 *glyphs,
                                   ar_i32 count, ar_i32 at, ar_i32 depth);

static int ar__chain_match(const ar_face *f, ar_u32 sub, const ar_i32 *glyphs, ar_i32 count,
                           ar_i32 at, ar_u32 *records, ar_u32 *nrecords, ar_i32 *matched)
{
    ar_u32 p = sub + 2;
    ar_u32 nback, nin, nahead, i;

    nback = ar__sh_u16(f, p);
    p += 2;
    /* Backtrack coverages are stored nearest-first, so index i is i+1 glyphs
       before the position. Reading them forwards is the classic way to get
       this exactly backwards. */
    for (i = 0; i < nback; ++i)
    {
        ar_i32 k = at - 1 - (ar_i32)i;
        if (k < 0 || ar__coverage(f, sub + ar__sh_u16(f, p + i * 2), glyphs[k]) < 0)
        {
            return 0;
        }
    }
    p += nback * 2;

    nin = ar__sh_u16(f, p);
    p += 2;
    if (nin == 0 || at + (ar_i32)nin > count)
    {
        return 0;
    }
    for (i = 0; i < nin; ++i)
    {
        if (ar__coverage(f, sub + ar__sh_u16(f, p + i * 2), glyphs[at + (ar_i32)i]) < 0)
        {
            return 0;
        }
    }
    p += nin * 2;

    nahead = ar__sh_u16(f, p);
    p += 2;
    for (i = 0; i < nahead; ++i)
    {
        ar_i32 k = at + (ar_i32)nin + (ar_i32)i;
        if (k >= count || ar__coverage(f, sub + ar__sh_u16(f, p + i * 2), glyphs[k]) < 0)
        {
            return 0;
        }
    }
    p += nahead * 2;

    *nrecords = ar__sh_u16(f, p);
    *records = p + 2;
    *matched = (ar_i32)nin;
    return 1;
}

/* Runs one lookup, by offset, at one position. Only the substitutions that do
   not change the run's length, because a nested lookup that did would move
   every position the outer match is still holding. */
static ar_i32 ar__apply_one_lookup(const ar_shaper *sh, ar_u32 lookup, ar_i32 *glyphs,
                                   ar_i32 count, ar_i32 at, ar_i32 depth)
{
    const ar_face *f = sh->face;
    ar_u32         nsub = ar__sh_u16(f, lookup + 4);
    ar_u32         j;

    if (depth > 4 || at < 0 || at >= count)
    {
        return 0;
    }
    for (j = 0; j < nsub; ++j)
    {
        ar_i32 type;
        ar_u32 sub = ar__subtable(f, lookup, j, &type);

        if (type == 1)
        {
            ar_i32 g = ar__single_sub(f, sub, glyphs[at]);
            if (g >= 0)
            {
                glyphs[at] = g;
                return 1;
            }
        }
    }
    (void)depth;
    return 0;
}

static int ar__chained(const ar_shaper *sh, const ar_u32 *lookups, ar_i32 nlookups, ar_i32 *glyphs,
                       ar_i32 count, ar_i32 at, ar_i32 depth)
{
    const ar_face *f = sh->face;
    ar_i32         k;
    int            any = 0;

    if (!sh->gsub_lookups)
    {
        return 0;
    }
    for (k = 0; k < nlookups; ++k)
    {
        ar_u32 lookup = lookups[k];
        ar_u32 nsub = ar__sh_u16(f, lookup + 4);
        ar_u32 j;

        for (j = 0; j < nsub; ++j)
        {
            ar_i32 type;
            ar_u32 sub = ar__subtable(f, lookup, j, &type);
            ar_u32 records = 0, nrecords = 0;
            ar_i32 matched = 0;
            ar_u32 r;

            if (type != 6 || ar__sh_u16(f, sub) != 3)
            {
                continue;
            }
            if (!ar__chain_match(f, sub, glyphs, count, at, &records, &nrecords, &matched))
            {
                continue;
            }
            for (r = 0; r < nrecords; ++r)
            {
                ar_u32 seq = ar__sh_u16(f, records + r * 4);
                ar_u32 idx = ar__sh_u16(f, records + r * 4 + 2);
                ar_u32 nested;

                if ((ar_i32)seq >= matched || idx >= ar__sh_u16(f, sh->gsub_lookups))
                {
                    continue;
                }
                nested = sh->gsub_lookups + ar__sh_u16(f, sh->gsub_lookups + 2 + idx * 2);
                any |= ar__apply_one_lookup(sh, nested, glyphs, count, at + (ar_i32)seq, depth + 1);
            }
        }
    }
    return any;
}

/*
 * Chooses each letter's positional form.
 *
 * A letter takes its final form when something joins it from the right, its
 * initial form when it joins something on the left, and its medial form when
 * both. In logical order -- which is what this walks -- "joins the previous
 * letter" means the previous letter is dual or join-causing, and "joins the
 * next" means this letter is dual and the next one can be joined to.
 */
static void ar__arabic_forms(const ar_shaper *sh, const ar_u32 *cps, ar_i32 *glyphs, ar_i32 count)
{
    ar_i32 i;

    if (!sh->init_count && !sh->medi_count && !sh->fina_count)
    {
        return;
    }

    for (i = 0; i < count; ++i)
    {
        ar_i32 t = ar_join_type(cps[i]);
        ar_i32 prev, next, j;
        ar_i32 joins_prev = 0, joins_next = 0;
        ar_i32 g;

        if (t == AR_JOIN_T || t == AR_JOIN_U)
        {
            continue; /* marks and non-joining letters keep their shape */
        }

        /* Marks are transparent, so look past them in both directions. */
        prev = -1;
        for (j = i - 1; j >= 0; --j)
        {
            if (ar_join_type(cps[j]) != AR_JOIN_T)
            {
                prev = j;
                break;
            }
        }
        next = -1;
        for (j = i + 1; j < count; ++j)
        {
            if (ar_join_type(cps[j]) != AR_JOIN_T)
            {
                next = j;
                break;
            }
        }

        if (prev >= 0)
        {
            ar_i32 pt = ar_join_type(cps[prev]);
            joins_prev = (pt == AR_JOIN_D || pt == AR_JOIN_C);
        }
        if (next >= 0 && t == AR_JOIN_D)
        {
            ar_i32 nt = ar_join_type(cps[next]);
            joins_next = (nt == AR_JOIN_D || nt == AR_JOIN_R || nt == AR_JOIN_C);
        }

        g = -1;
        if (joins_prev && joins_next)
        {
            g = ar__apply_single(sh, sh->medi, sh->medi_count, glyphs[i]);
        }
        else if (joins_prev)
        {
            g = ar__apply_single(sh, sh->fina, sh->fina_count, glyphs[i]);
        }
        else if (joins_next)
        {
            g = ar__apply_single(sh, sh->init, sh->init_count, glyphs[i]);
        }
        if (g >= 0)
        {
            glyphs[i] = g;
        }
    }
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


/* An anchor is a point in the glyph's own coordinates. Format 2 adds a
   contour point index for hinted attachment and format 3 device tables; both
   carry the plain coordinates first, so both are read as format 1. */
static void ar__anchor(const ar_face *f, ar_u32 at, ar_i32 *x, ar_i32 *y)
{
    *x = 0;
    *y = 0;
    if (!at || ar__sh_u16(f, at) == 0)
    {
        return;
    }
    *x = ar__sh_i16(f, at + 2);
    *y = ar__sh_i16(f, at + 4);
}

/*
 * GPOS types 4, 5 and 6 all say the same thing in the same shape: a mark
 * carries an anchor and a class, the thing it attaches to carries one anchor
 * per class, and the mark is placed so the two coincide. What differs is only
 * where the second array's anchors live.
 *
 *   type 4  mark to base       one anchor set per base glyph
 *   type 5  mark to ligature   one anchor set per component of the ligature
 *   type 6  mark to mark       one anchor set per preceding mark
 *
 * So one function does all three, told how to find the second anchor.
 */
static int ar__mark_attach(const ar_face *f, ar_u32 sub, ar_i32 kind, ar_i32 anchor_glyph,
                           ar_i32 mark, ar_i32 *dx, ar_i32 *dy)
{
    ar_u32 mark_cov, other_cov, classes, mark_array, other_array;
    ar_i32 mi, oi, cls;
    ar_i32 ax, ay, mx, my;
    ar_u32 anchor_off;

    if (ar__sh_u16(f, sub) != 1)
    {
        return 0;
    }
    mark_cov = sub + ar__sh_u16(f, sub + 2);
    other_cov = sub + ar__sh_u16(f, sub + 4);
    classes = ar__sh_u16(f, sub + 6);
    mark_array = sub + ar__sh_u16(f, sub + 8);
    other_array = sub + ar__sh_u16(f, sub + 10);

    mi = ar__coverage(f, mark_cov, mark);
    oi = ar__coverage(f, other_cov, anchor_glyph);
    if (mi < 0 || oi < 0 || classes == 0)
    {
        return 0;
    }
    if ((ar_u32)mi >= ar__sh_u16(f, mark_array) || (ar_u32)oi >= ar__sh_u16(f, other_array))
    {
        return 0;
    }

    cls = (ar_i32)ar__sh_u16(f, mark_array + 2 + (ar_u32)mi * 4);
    if (cls < 0 || (ar_u32)cls >= classes)
    {
        return 0;
    }
    ar__anchor(f, mark_array + ar__sh_u16(f, mark_array + 4 + (ar_u32)mi * 4), &mx, &my);

    if (kind == 5)
    {
        /* A ligature has one anchor set per component, and which component a
           mark belongs to depends on which character it followed before the
           ligature swallowed them. This shaper collapses a ligature to one
           glyph and does not keep that, so the last component is used.

           That is right for Arabic lam-alef, where a mark after the pair
           belongs to the alef, and wrong for a mark that belonged to the lam.
           Keeping component indices means carrying them through substitution,
           which is what a full shaper does and this one does not yet. */
        ar_u32 attach = other_array + ar__sh_u16(f, other_array + 2 + (ar_u32)oi * 2);
        ar_u32 ncomp = ar__sh_u16(f, attach);
        if (ncomp == 0)
        {
            return 0;
        }
        anchor_off = ar__sh_u16(f, attach + 2 + ((ncomp - 1) * classes + (ar_u32)cls) * 2);
        if (!anchor_off)
        {
            return 0;
        }
        ar__anchor(f, attach + anchor_off, &ax, &ay);
    }
    else
    {
        anchor_off = ar__sh_u16(f, other_array + 2 + ((ar_u32)oi * classes + (ar_u32)cls) * 2);
        if (!anchor_off)
        {
            return 0;
        }
        ar__anchor(f, other_array + anchor_off, &ax, &ay);
    }

    *dx = ax - mx;
    *dy = ay - my;
    return 1;
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
ar_i32 ar_shape_run_cp(const ar_shaper *sh, const ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv,
                       ar_i32 *cluster, ar_i32 count)
{
    return ar_shape_run_pos(sh, cps, glyphs, adv, 0, 0, cluster, count, count);
}

ar_i32 ar_shape_run_pos(const ar_shaper *sh, const ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv,
                        ar_i32 *dx, ar_i32 *dy, ar_i32 *cluster, ar_i32 count, ar_i32 cap)
{
    ar_i32 i;

    if (cap < count)
    {
        cap = count;
    }
    if (sh && sh->ok && glyphs && count > 0)
    {
        /* ccmp first: it exists to put the run into the shape the rest of the
           rules expect to find. Decomposition can lengthen it, which is why
           this is the only place a capacity matters. */
        count = ar__apply_ccmp(sh, glyphs, adv, cluster, count, cap);
    }
    if (sh && sh->ok && cps && glyphs && count > 0)
    {
        ar__arabic_forms(sh, cps, glyphs, count);
    }
    if (sh && sh->ok && glyphs && sh->calt_count > 0)
    {
        /* Contextual alternates last among the substitutions, because it is
           the one that reacts to what the others produced. */
        for (i = 0; i < count; ++i)
        {
            ar__chained(sh, sh->calt, sh->calt_count, glyphs, count, i, 0);
        }
    }
    if (dx && dy)
    {
        for (i = 0; i < count; ++i)
        {
            dx[i] = 0;
            dy[i] = 0;
        }
    }

    count = ar_shape_run(sh, glyphs, adv, cluster, count);

    if (dx && dy && sh && sh->ok && (sh->mark_count > 0 || sh->mkmk_count > 0))
    {
        const ar_face *f = sh->face;
        ar_i32         base = -1;     /* the last thing a mark can attach to  */
        ar_i32         last_mark = -1; /* and the last mark, for mark to mark */

        for (i = 0; i < count; ++i)
        {
            ar_i32 k;
            int    attached = 0;

            /* Mark to mark first: a vowel above a shadda belongs to the
               shadda, and only falls back to the letter if the font does not
               say otherwise. */
            for (k = 0; k < sh->mkmk_count && !attached && last_mark >= 0; ++k)
            {
                ar_u32 lookup = sh->mkmk[k];
                ar_u32 nsub = ar__sh_u16(f, lookup + 4);
                ar_u32 j;

                for (j = 0; j < nsub && !attached; ++j)
                {
                    ar_i32 type;
                    ar_u32 sub = ar__subtable(f, lookup, j, &type);
                    if (type != 6)
                    {
                        continue;
                    }
                    attached = ar__mark_attach(f, sub, 6, glyphs[last_mark], glyphs[i], &dx[i],
                                               &dy[i]);
                    if (attached)
                    {
                        /* Relative to a mark that is itself displaced. */
                        dx[i] += dx[last_mark];
                        dy[i] += dy[last_mark];
                    }
                }
            }

            for (k = 0; k < sh->mark_count && !attached && base >= 0; ++k)
            {
                ar_u32 lookup = sh->mark[k];
                ar_u32 nsub = ar__sh_u16(f, lookup + 4);
                ar_u32 j;

                for (j = 0; j < nsub && !attached; ++j)
                {
                    ar_i32 type;
                    ar_u32 sub = ar__subtable(f, lookup, j, &type);
                    if (type != 4 && type != 5)
                    {
                        continue;
                    }
                    attached = ar__mark_attach(f, sub, type, glyphs[base], glyphs[i], &dx[i],
                                               &dy[i]);
                }
            }

            /* Anything that was not attached is a base for what follows. A
               mark that no lookup claimed is left where it was, which is the
               honest outcome: the font did not say where it goes. */
            if (!attached)
            {
                base = i;
                last_mark = -1;
            }
            else
            {
                last_mark = i;
                if (adv)
                {
                    adv[i] = 0; /* an attached mark takes no space of its own */
                }
            }
        }
    }
    return count;
}

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

        for (k = 0; k < sh->liga_count + sh->rlig_count && replaced < 0; ++k)
        {
            ar_u32 lookup = k < sh->liga_count ? sh->liga[k] : sh->rlig[k - sh->liga_count];
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
