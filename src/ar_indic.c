/*
 * areole - Indic reordering.
 * SPDX-License-Identifier: MIT
 */
#include "ar_indic.h"

/*
 * Categories.
 *
 * Devanagari is spelled out because it is the script this was built and
 * checked against. The other blocks are given the same shape -- consonants in
 * one range, matras in another, a virama at a fixed offset -- which is true of
 * every Brahmi-derived script by construction, since they share an ancestor
 * and Unicode encodes them in parallel.
 *
 * Where that assumption breaks it breaks quietly, which is why the header says
 * so plainly rather than claiming coverage this does not have.
 */
ar_i32 ar_indic_category(ar_u32 cp)
{
    /* Devanagari, the one that is right rather than merely plausible. */
    if (cp >= 0x0900u && cp <= 0x097Fu)
    {
        if (cp == 0x0930u || cp == 0x0931u)
        {
            return AR_IND_RA;
        }
        if (cp == 0x094Du)
        {
            return AR_IND_VIRAMA;
        }
        if (cp == 0x093Cu)
        {
            return AR_IND_NUKTA;
        }
        if (cp == 0x093Fu)
        {
            return AR_IND_MATRA_PRE; /* the vowel sign i, drawn before */
        }
        if (cp == 0x0940u || (cp >= 0x093Eu && cp <= 0x093Eu) || cp == 0x0949u || cp == 0x094Au ||
            cp == 0x094Bu || cp == 0x094Cu)
        {
            return AR_IND_MATRA_POST;
        }
        if (cp == 0x0945u || cp == 0x0946u || cp == 0x0947u || cp == 0x0948u)
        {
            return AR_IND_MATRA_ABOVE;
        }
        if (cp >= 0x0941u && cp <= 0x0944u)
        {
            return AR_IND_MATRA_BELOW;
        }
        if (cp >= 0x0901u && cp <= 0x0903u)
        {
            return AR_IND_BINDU;
        }
        if (cp >= 0x0905u && cp <= 0x0914u)
        {
            return AR_IND_VOWEL;
        }
        if ((cp >= 0x0915u && cp <= 0x0939u) || (cp >= 0x0958u && cp <= 0x095Fu) ||
            (cp >= 0x0978u && cp <= 0x097Fu))
        {
            return AR_IND_CONSONANT;
        }
        return AR_IND_OTHER;
    }

    /* The other blocks, by the parallel structure Unicode gives them. Each is
       offset by a multiple of 0x80 from Devanagari and lays its consonants,
       matras and virama out in the same order. */
    {
        struct block
        {
            ar_u32 base;
            ar_u32 pre_matra; /* the one matra that is drawn before, or 0 */
        };
        static const struct block BLOCKS[] = {
            {0x0980u, 0x09BFu}, /* Bengali    */
            {0x0A00u, 0x0A3Fu}, /* Gurmukhi   */
            {0x0A80u, 0x0ABFu}, /* Gujarati   */
            {0x0B00u, 0x0B47u}, /* Oriya      */
            {0x0B80u, 0x0BC6u}, /* Tamil      */
            {0x0C00u, 0x0C3Fu}, /* Telugu     */
            {0x0C80u, 0x0CBFu}, /* Kannada    */
            {0x0D00u, 0x0D46u}  /* Malayalam  */
        };
        ar_i32 i, n = (ar_i32)(sizeof BLOCKS / sizeof BLOCKS[0]);

        for (i = 0; i < n; ++i)
        {
            ar_u32 b = BLOCKS[i].base;
            if (cp < b || cp > b + 0x7Fu)
            {
                continue;
            }
            if (BLOCKS[i].pre_matra && cp == BLOCKS[i].pre_matra)
            {
                return AR_IND_MATRA_PRE;
            }
            if (cp == b + 0x4Du)
            {
                return AR_IND_VIRAMA;
            }
            if (cp == b + 0x30u)
            {
                return AR_IND_RA;
            }
            if (cp == b + 0x3Cu)
            {
                return AR_IND_NUKTA;
            }
            if (cp >= b + 0x3Eu && cp <= b + 0x4Cu)
            {
                return AR_IND_MATRA_POST;
            }
            if (cp >= b + 0x01u && cp <= b + 0x03u)
            {
                return AR_IND_BINDU;
            }
            if (cp >= b + 0x05u && cp <= b + 0x14u)
            {
                return AR_IND_VOWEL;
            }
            if (cp >= b + 0x15u && cp <= b + 0x39u)
            {
                return AR_IND_CONSONANT;
            }
            return AR_IND_OTHER;
        }
    }
    return AR_IND_OTHER;
}

int ar_indic_is_indic(ar_u32 cp)
{
    return cp >= 0x0900u && cp <= 0x0D7Fu;
}

/* Moves one element of every parallel array from `from` to `to`, so a
   reordering cannot leave a glyph pointing at the wrong character. */
static void ar__move(ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *cluster, ar_i32 from,
                     ar_i32 to)
{
    ar_u32 cp = cps[from];
    ar_i32 g = glyphs[from];
    ar_i32 a = adv ? adv[from] : 0;
    ar_i32 c = cluster ? cluster[from] : 0;
    ar_i32 i;

    if (from == to)
    {
        return;
    }
    if (from > to)
    {
        for (i = from; i > to; --i)
        {
            cps[i] = cps[i - 1];
            glyphs[i] = glyphs[i - 1];
            if (adv)
            {
                adv[i] = adv[i - 1];
            }
            if (cluster)
            {
                cluster[i] = cluster[i - 1];
            }
        }
    }
    else
    {
        for (i = from; i < to; ++i)
        {
            cps[i] = cps[i + 1];
            glyphs[i] = glyphs[i + 1];
            if (adv)
            {
                adv[i] = adv[i + 1];
            }
            if (cluster)
            {
                cluster[i] = cluster[i + 1];
            }
        }
    }
    cps[to] = cp;
    glyphs[to] = g;
    if (adv)
    {
        adv[to] = a;
    }
    if (cluster)
    {
        cluster[to] = c;
    }
}

/*
 * One syllable: an optional reph, a run of consonants joined by viramas, and
 * the vowel signs and marks that follow.
 *
 * The base consonant is the last one not followed by a virama -- the one that
 * keeps its full form while the others become half forms or conjuncts. Pre-base
 * matras go before it; reph goes after everything.
 */
static ar_i32 ar__syllable_end(const ar_u32 *cps, ar_i32 at, ar_i32 count)
{
    ar_i32 i = at;

    while (i < count)
    {
        ar_i32 c = ar_indic_category(cps[i]);

        if (c == AR_IND_CONSONANT || c == AR_IND_RA || c == AR_IND_VOWEL)
        {
            ++i;
            /* A virama binds this consonant to the next, so the syllable
               continues. Without one, the next consonant starts a new
               syllable. */
            while (i < count && ar_indic_category(cps[i]) == AR_IND_NUKTA)
            {
                ++i;
            }
            if (i < count && ar_indic_category(cps[i]) == AR_IND_VIRAMA)
            {
                ++i;
                continue;
            }
            break;
        }
        if (c == AR_IND_OTHER)
        {
            return i > at ? i : at + 1;
        }
        ++i;
    }

    /* Then everything that hangs off the base. */
    while (i < count)
    {
        ar_i32 c = ar_indic_category(cps[i]);
        if (c == AR_IND_MATRA_PRE || c == AR_IND_MATRA_POST || c == AR_IND_MATRA_ABOVE ||
            c == AR_IND_MATRA_BELOW || c == AR_IND_BINDU || c == AR_IND_NUKTA)
        {
            ++i;
            continue;
        }
        break;
    }
    return i > at ? i : at + 1;
}

int ar_indic_reorder(const ar_shaper *sh, ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *cluster,
                     ar_i32 count)
{
    ar_i32 at = 0;
    int    moved = 0;

    (void)sh;
    if (!cps || !glyphs || count <= 0)
    {
        return 0;
    }

    while (at < count)
    {
        ar_i32 end, i, base = -1;
        int    reph = 0;

        if (!ar_indic_is_indic(cps[at]))
        {
            ++at;
            continue;
        }
        end = ar__syllable_end(cps, at, count);

        /* Reph: the syllable opens RA + VIRAMA and has something after it. */
        if (end - at >= 3 && ar_indic_category(cps[at]) == AR_IND_RA &&
            ar_indic_category(cps[at + 1]) == AR_IND_VIRAMA)
        {
            ar_i32 c = ar_indic_category(cps[at + 2]);
            if (c == AR_IND_CONSONANT || c == AR_IND_RA)
            {
                reph = 1;
            }
        }

        /* The base is the last consonant that no virama follows. */
        for (i = at + (reph ? 2 : 0); i < end; ++i)
        {
            ar_i32 c = ar_indic_category(cps[i]);
            if (c != AR_IND_CONSONANT && c != AR_IND_RA)
            {
                continue;
            }
            {
                ar_i32 j = i + 1;
                while (j < end && ar_indic_category(cps[j]) == AR_IND_NUKTA)
                {
                    ++j;
                }
                if (j >= end || ar_indic_category(cps[j]) != AR_IND_VIRAMA)
                {
                    base = i;
                    break;
                }
            }
        }
        if (base < 0)
        {
            at = end;
            continue;
        }

        /* A pre-base matra is typed after its consonant and drawn before it.
           This is the reordering that changes a word rather than its
           appearance, and the one whose absence is unmistakable. */
        for (i = base + 1; i < end; ++i)
        {
            if (ar_indic_category(cps[i]) == AR_IND_MATRA_PRE)
            {
                ar__move(cps, glyphs, adv, cluster, i, reph ? at + 2 : at);
                moved = 1;
                break;
            }
        }

        /* Reph is drawn above the end of the syllable, not at its start. Its
           two characters became one mark, so the virama goes and the ra moves
           to the back. */
        if (reph)
        {
            ar__move(cps, glyphs, adv, cluster, at, end - 1);
            ar__move(cps, glyphs, adv, cluster, at, end - 1);
            moved = 1;
        }

        at = end;
    }
    return moved;
}
