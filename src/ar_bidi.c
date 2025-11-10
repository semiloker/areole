/*
 * areole - the bidirectional algorithm, UAX #9.
 * SPDX-License-Identifier: MIT
 */
#include "ar_bidi.h"

#include "ar_text.h"

/*
 * The class table.
 *
 * What is covered: Latin, Greek, Cyrillic and the rest of the left-to-right
 * world; Hebrew and Arabic with their marks and numerals; the European and
 * Arabic number classes and their separators and terminators; every explicit
 * formatting character; and the neutrals.
 *
 * What is not: Thaana, N'Ko, Syriac and the other smaller right-to-left
 * scripts come out as plain R rather than AL, which resolves their direction
 * correctly and their numbers as European rather than Arabic. That is a
 * visible difference only in text mixing those scripts with digits.
 */
ar_i32 ar_bidi_class(ar_u32 cp)
{
    struct range
    {
        ar_u32 lo, hi;
        ar_u8  cls;
    };
    static const struct range RANGES[] = {
        {0x0000, 0x0008, AR_BC_BN},  {0x0009, 0x0009, AR_BC_S},   {0x000A, 0x000A, AR_BC_B},
        {0x000B, 0x000B, AR_BC_S},   {0x000C, 0x000C, AR_BC_WS},  {0x000D, 0x000D, AR_BC_B},
        {0x000E, 0x001B, AR_BC_BN},  {0x001C, 0x001E, AR_BC_B},   {0x001F, 0x001F, AR_BC_S},
        {0x0020, 0x0020, AR_BC_WS},  {0x0021, 0x0022, AR_BC_ON},  {0x0023, 0x0025, AR_BC_ET},
        {0x0026, 0x002A, AR_BC_ON},  {0x002B, 0x002B, AR_BC_ES},  {0x002C, 0x002C, AR_BC_CS},
        {0x002D, 0x002D, AR_BC_ES},  {0x002E, 0x002F, AR_BC_CS},  {0x0030, 0x0039, AR_BC_EN},
        {0x003A, 0x003A, AR_BC_CS},  {0x003B, 0x0040, AR_BC_ON},  {0x0041, 0x005A, AR_BC_L},
        {0x005B, 0x0060, AR_BC_ON},  {0x0061, 0x007A, AR_BC_L},   {0x007B, 0x007E, AR_BC_ON},
        {0x007F, 0x0084, AR_BC_BN},  {0x0085, 0x0085, AR_BC_B},   {0x0086, 0x009F, AR_BC_BN},
        {0x00A0, 0x00A0, AR_BC_CS},  {0x00A1, 0x00A1, AR_BC_ON},  {0x00A2, 0x00A5, AR_BC_ET},
        {0x00A6, 0x00A9, AR_BC_ON},  {0x00AA, 0x00AA, AR_BC_L},   {0x00AB, 0x00AC, AR_BC_ON},
        {0x00AD, 0x00AD, AR_BC_BN},  {0x00AE, 0x00AF, AR_BC_ON},  {0x00B0, 0x00B1, AR_BC_ET},
        {0x00B2, 0x00B3, AR_BC_EN},  {0x00B4, 0x00B4, AR_BC_ON},  {0x00B5, 0x00B5, AR_BC_L},
        {0x00B6, 0x00B8, AR_BC_ON},  {0x00B9, 0x00B9, AR_BC_EN},  {0x00BA, 0x00BA, AR_BC_L},
        {0x00BB, 0x00BF, AR_BC_ON},  {0x00C0, 0x02B8, AR_BC_L},   {0x02B9, 0x02BA, AR_BC_ON},
        {0x02BB, 0x02C1, AR_BC_L},   {0x02C2, 0x02CF, AR_BC_ON},  {0x0300, 0x036F, AR_BC_NSM},
        {0x0370, 0x058F, AR_BC_L},   {0x0590, 0x05BD, AR_BC_NSM}, {0x05BE, 0x05BE, AR_BC_R},
        {0x05BF, 0x05BF, AR_BC_NSM}, {0x05C0, 0x05C0, AR_BC_R},   {0x05C1, 0x05C2, AR_BC_NSM},
        {0x05C3, 0x05C3, AR_BC_R},   {0x05C4, 0x05C5, AR_BC_NSM}, {0x05C6, 0x05C6, AR_BC_R},
        {0x05C7, 0x05C7, AR_BC_NSM}, {0x05C8, 0x05FF, AR_BC_R},   {0x0600, 0x0605, AR_BC_AN},
        {0x0606, 0x060A, AR_BC_ON},  {0x060B, 0x060B, AR_BC_AL},  {0x060C, 0x060C, AR_BC_CS},
        {0x060D, 0x060D, AR_BC_AL},  {0x060E, 0x060F, AR_BC_ON},  {0x0610, 0x061A, AR_BC_NSM},
        {0x061B, 0x064A, AR_BC_AL},  {0x064B, 0x065F, AR_BC_NSM}, {0x0660, 0x0669, AR_BC_AN},
        {0x066A, 0x066A, AR_BC_ET},  {0x066B, 0x066C, AR_BC_AN},  {0x066D, 0x066F, AR_BC_AL},
        {0x0670, 0x0670, AR_BC_NSM}, {0x0671, 0x06D5, AR_BC_AL},  {0x06D6, 0x06DC, AR_BC_NSM},
        {0x06DD, 0x06DD, AR_BC_AN},  {0x06DE, 0x06DE, AR_BC_ON},  {0x06DF, 0x06E4, AR_BC_NSM},
        {0x06E5, 0x06E6, AR_BC_AL},  {0x06E7, 0x06E8, AR_BC_NSM}, {0x06E9, 0x06E9, AR_BC_ON},
        {0x06EA, 0x06ED, AR_BC_NSM}, {0x06EE, 0x06EF, AR_BC_AL},  {0x06F0, 0x06F9, AR_BC_EN},
        {0x06FA, 0x070F, AR_BC_AL},  {0x0710, 0x074F, AR_BC_AL},  {0x0750, 0x077F, AR_BC_AL},
        {0x0780, 0x07BF, AR_BC_R},   {0x07C0, 0x08FF, AR_BC_R},   {0x0900, 0x1FFF, AR_BC_L},
        {0x2000, 0x200A, AR_BC_WS},  {0x200B, 0x200D, AR_BC_BN},  {0x200E, 0x200E, AR_BC_L},
        {0x200F, 0x200F, AR_BC_R},   {0x2010, 0x2027, AR_BC_ON},  {0x2028, 0x2028, AR_BC_WS},
        {0x2029, 0x2029, AR_BC_B},   {0x202A, 0x202A, AR_BC_LRE}, {0x202B, 0x202B, AR_BC_RLE},
        {0x202C, 0x202C, AR_BC_PDF}, {0x202D, 0x202D, AR_BC_LRO}, {0x202E, 0x202E, AR_BC_RLO},
        {0x202F, 0x202F, AR_BC_CS},  {0x2030, 0x2034, AR_BC_ET},  {0x2035, 0x2043, AR_BC_ON},
        {0x2044, 0x2044, AR_BC_CS},  {0x2045, 0x205E, AR_BC_ON},  {0x205F, 0x205F, AR_BC_WS},
        {0x2060, 0x2064, AR_BC_BN},  {0x2066, 0x2066, AR_BC_LRI}, {0x2067, 0x2067, AR_BC_RLI},
        {0x2068, 0x2068, AR_BC_FSI}, {0x2069, 0x2069, AR_BC_PDI}, {0x206A, 0x206F, AR_BC_BN},
        {0x2070, 0x2070, AR_BC_EN},  {0x2071, 0x2073, AR_BC_L},   {0x2074, 0x2079, AR_BC_EN},
        {0x207A, 0x207B, AR_BC_ES},  {0x207C, 0x207E, AR_BC_ON},  {0x207F, 0x207F, AR_BC_L},
        {0x2080, 0x2089, AR_BC_EN},  {0x208A, 0x208B, AR_BC_ES},  {0x208C, 0x208E, AR_BC_ON},
        {0x2090, 0x209C, AR_BC_L},   {0x20A0, 0x20CF, AR_BC_ET},  {0x20D0, 0x20F0, AR_BC_NSM},
        {0x2100, 0x2101, AR_BC_ON},  {0x2102, 0x2102, AR_BC_L},   {0x2103, 0x2106, AR_BC_ON},
        {0x2107, 0x2107, AR_BC_L},   {0x2108, 0x2109, AR_BC_ON},  {0x210A, 0x2113, AR_BC_L},
        {0x2114, 0x2114, AR_BC_ON},  {0x2115, 0x2115, AR_BC_L},   {0x2116, 0x2118, AR_BC_ON},
        {0x2119, 0x211D, AR_BC_L},   {0x211E, 0x2123, AR_BC_ON},  {0x2124, 0x2124, AR_BC_L},
        {0x2125, 0x2125, AR_BC_ON},  {0x2126, 0x2126, AR_BC_L},   {0x2127, 0x2127, AR_BC_ON},
        {0x2128, 0x2128, AR_BC_L},   {0x2129, 0x2129, AR_BC_ON},  {0x212A, 0x212D, AR_BC_L},
        {0x212E, 0x212E, AR_BC_ET},  {0x212F, 0x2139, AR_BC_L},   {0x213A, 0x213B, AR_BC_ON},
        {0x213C, 0x213F, AR_BC_L},   {0x2140, 0x2144, AR_BC_ON},  {0x2145, 0x2149, AR_BC_L},
        {0x214A, 0x214D, AR_BC_ON},  {0x214E, 0x214F, AR_BC_L},   {0x2150, 0x215F, AR_BC_ON},
        {0x2160, 0x2188, AR_BC_L},   {0x2189, 0x2BFF, AR_BC_ON},  {0x2C00, 0xD7FF, AR_BC_L},
        {0xE000, 0xFB1C, AR_BC_L},   {0xFB1D, 0xFB4F, AR_BC_R},   {0xFB50, 0xFDFF, AR_BC_AL},
        {0xFE00, 0xFE0F, AR_BC_NSM}, {0xFE20, 0xFE2F, AR_BC_NSM}, {0xFE50, 0xFE50, AR_BC_CS},
        {0xFE70, 0xFEFC, AR_BC_AL},  {0xFEFF, 0xFEFF, AR_BC_BN},  {0xFF10, 0xFF19, AR_BC_EN},
        {0x10800, 0x10FFF, AR_BC_R}, {0x1E800, 0x1EFFF, AR_BC_AL}};
    ar_i32 i, n = (ar_i32)(sizeof RANGES / sizeof RANGES[0]);

    for (i = 0; i < n; ++i)
    {
        if (cp >= RANGES[i].lo && cp <= RANGES[i].hi)
        {
            return RANGES[i].cls;
        }
    }
    return AR_BC_L;
}

static int ar__is_isolate_init(ar_i32 c)
{
    return c == AR_BC_LRI || c == AR_BC_RLI || c == AR_BC_FSI;
}

static int ar__is_strong(ar_i32 c)
{
    return c == AR_BC_L || c == AR_BC_R || c == AR_BC_AL;
}

/* Rules P2 and P3: the first strong character outside any isolate decides. */
static ar_i32 ar__paragraph_level(const ar_u8 *cls, ar_i32 from, ar_i32 count)
{
    ar_i32 i, depth = 0;

    for (i = from; i < count; ++i)
    {
        ar_i32 c = cls[i];
        if (ar__is_isolate_init(c))
        {
            ++depth;
            continue;
        }
        if (c == AR_BC_PDI)
        {
            if (depth > 0)
            {
                --depth;
            }
            continue;
        }
        if (depth > 0)
        {
            continue;
        }
        if (c == AR_BC_L)
        {
            return 0;
        }
        if (c == AR_BC_R || c == AR_BC_AL)
        {
            return 1;
        }
    }
    return 0;
}

#define MAX_DEPTH 125

ar_i32 ar_bidi_levels(const char *utf8, ar_i32 dir, ar_u8 *levels, ar_u8 *classes, ar_i32 cap,
                      ar_i32 *paragraph_level)
{
    ar_i32      n = 0;
    const char *p = utf8;
    ar_i32      para;
    ar_i32      i;

    if (!utf8 || !levels || !classes || cap <= 0)
    {
        return 0;
    }

    /* Decode once. Everything below indexes codepoints, because the algorithm
       is defined over characters and byte offsets would only be a source of
       off-by-one errors. */
    for (;;)
    {
        ar_u32 cp = ar_utf8_next(&p);
        if (cp == 0)
        {
            break;
        }
        if (n >= cap)
        {
            return 0;
        }
        classes[n++] = (ar_u8)ar_bidi_class(cp);
    }

    para = dir == AR_DIR_RTL ? 1 : dir == AR_DIR_LTR ? 0 : ar__paragraph_level(classes, 0, n);
    if (paragraph_level)
    {
        *paragraph_level = para;
    }

    /*
     * X1 to X8: explicit embeddings, overrides and isolates.
     *
     * The directional status stack. Isolates are the modern mechanism and
     * embeddings the deprecated one, and both are here because text in the
     * wild contains both.
     */
    {
        ar_u8  stack_level[MAX_DEPTH + 2];
        ar_u8  stack_override[MAX_DEPTH + 2];
        ar_u8  stack_isolate[MAX_DEPTH + 2];
        ar_i32 sp = 0;
        ar_i32 overflow_isolate = 0, overflow_embedding = 0, valid_isolate = 0;

        stack_level[0] = (ar_u8)para;
        stack_override[0] = AR_BC_ON;
        stack_isolate[0] = 0;

        for (i = 0; i < n; ++i)
        {
            ar_i32 c = classes[i];

            switch (c)
            {
            case AR_BC_RLE:
            case AR_BC_LRE:
            case AR_BC_RLO:
            case AR_BC_LRO:
            {
                int    rtl = (c == AR_BC_RLE || c == AR_BC_RLO);
                ar_i32 next = rtl ? (stack_level[sp] + 1) | 1 : (stack_level[sp] + 2) & ~1;

                levels[i] = stack_level[sp];
                classes[i] = AR_BC_BN;
                if (next <= MAX_DEPTH && !overflow_isolate && !overflow_embedding)
                {
                    ++sp;
                    stack_level[sp] = (ar_u8)next;
                    stack_override[sp] = (ar_u8)(c == AR_BC_RLO   ? AR_BC_R
                                                 : c == AR_BC_LRO ? AR_BC_L
                                                                  : AR_BC_ON);
                    stack_isolate[sp] = 0;
                }
                else if (!overflow_isolate)
                {
                    ++overflow_embedding;
                }
                break;
            }

            case AR_BC_RLI:
            case AR_BC_LRI:
            case AR_BC_FSI:
            {
                int    rtl;
                ar_i32 next;

                if (c == AR_BC_FSI)
                {
                    /* X5c: the direction of a first-strong isolate is decided
                       by scanning its own contents. */
                    ar_i32 j = i + 1, depth = 0, found = 0;
                    for (; j < n; ++j)
                    {
                        ar_i32 cj = classes[j];
                        if (ar__is_isolate_init(cj))
                        {
                            ++depth;
                        }
                        else if (cj == AR_BC_PDI)
                        {
                            if (depth == 0)
                            {
                                break;
                            }
                            --depth;
                        }
                        else if (depth == 0 && ar__is_strong(cj))
                        {
                            found = (cj != AR_BC_L);
                            break;
                        }
                    }
                    rtl = found;
                }
                else
                {
                    rtl = (c == AR_BC_RLI);
                }

                levels[i] = stack_level[sp];
                if (stack_override[sp] != AR_BC_ON)
                {
                    classes[i] = stack_override[sp];
                }

                next = rtl ? (stack_level[sp] + 1) | 1 : (stack_level[sp] + 2) & ~1;
                if (next <= MAX_DEPTH && !overflow_isolate && !overflow_embedding)
                {
                    ++valid_isolate;
                    ++sp;
                    stack_level[sp] = (ar_u8)next;
                    stack_override[sp] = AR_BC_ON;
                    stack_isolate[sp] = 1;
                }
                else
                {
                    ++overflow_isolate;
                }
                break;
            }

            case AR_BC_PDI:
                if (overflow_isolate > 0)
                {
                    --overflow_isolate;
                }
                else if (valid_isolate > 0)
                {
                    overflow_embedding = 0;
                    while (sp > 0 && !stack_isolate[sp])
                    {
                        --sp;
                    }
                    if (sp > 0)
                    {
                        --sp;
                    }
                    --valid_isolate;
                }
                levels[i] = stack_level[sp];
                if (stack_override[sp] != AR_BC_ON)
                {
                    classes[i] = stack_override[sp];
                }
                break;

            case AR_BC_PDF:
                levels[i] = stack_level[sp];
                classes[i] = AR_BC_BN;
                if (overflow_isolate > 0)
                {
                    break;
                }
                if (overflow_embedding > 0)
                {
                    --overflow_embedding;
                    break;
                }
                if (sp > 0 && !stack_isolate[sp])
                {
                    --sp;
                }
                break;

            case AR_BC_B:
                /* X8: a paragraph separator returns to the paragraph level. */
                sp = 0;
                overflow_isolate = overflow_embedding = valid_isolate = 0;
                levels[i] = (ar_u8)para;
                break;

            default:
                levels[i] = stack_level[sp];
                if (stack_override[sp] != AR_BC_ON)
                {
                    classes[i] = stack_override[sp];
                }
                break;
            }
        }
    }

    /*
     * W1 to W7, N0 to N2 and I1 to I2, applied over the whole paragraph rather
     * than per isolating run sequence.
     *
     * That simplification is worth stating. The specification resolves each
     * isolating run sequence separately with its own surrounding context; here
     * the rules run once across everything, which agrees with the full
     * algorithm on text without isolate characters -- which is nearly all text
     * -- and can differ inside nested isolates. Isolates still get their
     * levels right from X1-X8 above, so the direction is correct either way;
     * what can differ is how a neutral between two isolates resolves.
     */
    {
        ar_i32 prev_strong = para ? AR_BC_R : AR_BC_L;

        /* W1: a non-spacing mark takes the class of what precedes it. */
        for (i = 0; i < n; ++i)
        {
            if (classes[i] == AR_BC_NSM)
            {
                classes[i] =
                    (ar_u8)(i == 0 ? (para ? AR_BC_R : AR_BC_L)
                            : ar__is_isolate_init(classes[i - 1]) || classes[i - 1] == AR_BC_PDI
                                ? AR_BC_ON
                                : classes[i - 1]);
            }
        }

        /* W2: a European number after Arabic text is an Arabic number. */
        prev_strong = para ? AR_BC_R : AR_BC_L;
        for (i = 0; i < n; ++i)
        {
            if (ar__is_strong(classes[i]))
            {
                prev_strong = classes[i];
            }
            else if (classes[i] == AR_BC_EN && prev_strong == AR_BC_AL)
            {
                classes[i] = AR_BC_AN;
            }
        }

        /* W3: Arabic letters are simply right to left from here on. */
        for (i = 0; i < n; ++i)
        {
            if (classes[i] == AR_BC_AL)
            {
                classes[i] = AR_BC_R;
            }
        }

        /* W4: a single separator between two numbers of the same kind joins
           them, which is what keeps 1,234 and 12:30 together. */
        for (i = 1; i + 1 < n; ++i)
        {
            if (classes[i] == AR_BC_ES && classes[i - 1] == AR_BC_EN && classes[i + 1] == AR_BC_EN)
            {
                classes[i] = AR_BC_EN;
            }
            else if (classes[i] == AR_BC_CS && classes[i - 1] == classes[i + 1] &&
                     (classes[i - 1] == AR_BC_EN || classes[i - 1] == AR_BC_AN))
            {
                classes[i] = classes[i - 1];
            }
        }

        /* W5: a run of terminators adjacent to a European number joins it. */
        for (i = 0; i < n; ++i)
        {
            if (classes[i] != AR_BC_ET)
            {
                continue;
            }
            {
                ar_i32 j = i;
                int    en = 0;
                while (j < n && classes[j] == AR_BC_ET)
                {
                    ++j;
                }
                if ((i > 0 && classes[i - 1] == AR_BC_EN) || (j < n && classes[j] == AR_BC_EN))
                {
                    en = 1;
                }
                while (i < j)
                {
                    classes[i++] = (ar_u8)(en ? AR_BC_EN : AR_BC_ON);
                }
                --i;
            }
        }

        /* W6: whatever separators and terminators are left are neutral. */
        for (i = 0; i < n; ++i)
        {
            if (classes[i] == AR_BC_ES || classes[i] == AR_BC_ET || classes[i] == AR_BC_CS)
            {
                classes[i] = AR_BC_ON;
            }
        }

        /* W7: a European number after left-to-right text is left to right. */
        prev_strong = para ? AR_BC_R : AR_BC_L;
        for (i = 0; i < n; ++i)
        {
            if (classes[i] == AR_BC_L || classes[i] == AR_BC_R)
            {
                prev_strong = classes[i];
            }
            else if (classes[i] == AR_BC_EN && prev_strong == AR_BC_L)
            {
                classes[i] = AR_BC_L;
            }
        }

        /* N1 and N2: a run of neutrals between two matching strong directions
           takes that direction; otherwise it takes the embedding direction.
           Numbers count as right-to-left for this purpose. */
        for (i = 0; i < n; ++i)
        {
            ar_i32 c = classes[i];
            if (c != AR_BC_ON && c != AR_BC_WS && c != AR_BC_S && c != AR_BC_B && c != AR_BC_BN &&
                !ar__is_isolate_init(c) && c != AR_BC_PDI)
            {
                continue;
            }
            {
                ar_i32 j = i, before, after, resolved;
                while (j < n)
                {
                    ar_i32 cj = classes[j];
                    if (cj != AR_BC_ON && cj != AR_BC_WS && cj != AR_BC_S && cj != AR_BC_B &&
                        cj != AR_BC_BN && !ar__is_isolate_init(cj) && cj != AR_BC_PDI)
                    {
                        break;
                    }
                    ++j;
                }

                before = para ? AR_BC_R : AR_BC_L;
                if (i > 0)
                {
                    ar_i32 cb = classes[i - 1];
                    before = (cb == AR_BC_EN || cb == AR_BC_AN) ? AR_BC_R : cb;
                }
                after = para ? AR_BC_R : AR_BC_L;
                if (j < n)
                {
                    ar_i32 ca = classes[j];
                    after = (ca == AR_BC_EN || ca == AR_BC_AN) ? AR_BC_R : ca;
                }

                resolved = (before == after && (before == AR_BC_L || before == AR_BC_R))
                               ? before
                               : (levels[i] & 1 ? AR_BC_R : AR_BC_L);
                while (i < j)
                {
                    classes[i++] = (ar_u8)resolved;
                }
                --i;
            }
        }

        /* I1 and I2: the implicit levels. */
        for (i = 0; i < n; ++i)
        {
            ar_i32 c = classes[i];
            ar_i32 lv = levels[i];

            if ((lv & 1) == 0)
            {
                if (c == AR_BC_R)
                {
                    lv += 1;
                }
                else if (c == AR_BC_AN || c == AR_BC_EN)
                {
                    lv += 2;
                }
            }
            else if (c == AR_BC_L || c == AR_BC_EN || c == AR_BC_AN)
            {
                lv += 1;
            }
            levels[i] = (ar_u8)lv;
        }
    }

    return n;
}

ar_i32 ar_bidi_runs(const ar_u8 *levels, ar_i32 count, ar_bidi_run *runs, ar_i32 max_runs)
{
    ar_i32 n = 0, i;
    ar_u8  highest = 0, lowest_odd = 127;

    if (!levels || !runs || max_runs <= 0 || count <= 0)
    {
        return 0;
    }

    /* Split into level runs first: a run is a maximal stretch at one level. */
    for (i = 0; i < count;)
    {
        ar_i32 j = i;
        while (j < count && levels[j] == levels[i])
        {
            ++j;
        }
        if (n >= max_runs)
        {
            return n;
        }
        runs[n].start = i;
        runs[n].count = j - i;
        runs[n].level = levels[i];
        if (levels[i] > highest)
        {
            highest = levels[i];
        }
        if ((levels[i] & 1) && levels[i] < lowest_odd)
        {
            lowest_odd = levels[i];
        }
        ++n;
        i = j;
    }

    /*
     * L2: from the highest level down to the lowest odd one, reverse every
     * maximal stretch of runs at or above that level.
     *
     * Doing it on runs rather than characters is what makes this cheap: a
     * paragraph has a handful of runs and hundreds of characters, and the
     * result is identical because runs are contiguous by construction.
     */
    {
        ar_i32 lv;
        for (lv = highest; lv >= lowest_odd && lowest_odd <= highest; --lv)
        {
            ar_i32 a = 0;
            while (a < n)
            {
                ar_i32 b;
                if (runs[a].level < lv)
                {
                    ++a;
                    continue;
                }
                b = a;
                while (b < n && runs[b].level >= lv)
                {
                    ++b;
                }
                {
                    ar_i32 x = a, y = b - 1;
                    while (x < y)
                    {
                        ar_bidi_run t = runs[x];
                        runs[x] = runs[y];
                        runs[y] = t;
                        ++x;
                        --y;
                    }
                }
                a = b;
            }
        }
    }
    return n;
}
