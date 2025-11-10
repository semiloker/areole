/*
 * areole - line breaking, after UAX #14.
 * SPDX-License-Identifier: MIT
 */
#include "ar_break.h"

#include "ar_text.h"

/*
 * What this covers, stated rather than left to be discovered.
 *
 * Latin, Greek, Cyrillic, Armenian, Hebrew and Arabic letters are AL. CJK
 * ideographs, kana, Hangul syllables and fullwidth forms are ID, so text in
 * those scripts breaks between characters as it should. Digits are NU with
 * their infix separators, so 1,000.50 does not break. ASCII and General
 * Punctuation are classified individually, as are the CJK brackets, because
 * that is where the visible mistakes are.
 *
 * What it does not cover: South and Southeast Asian scripts, which need
 * dictionary segmentation rather than a pair table and which this library
 * cannot shape anyway; the emoji classes EB and EM; and regional indicators.
 * Characters in those ranges come out AL, which breaks between them as if they
 * were Latin letters. That is wrong for Thai and right for very little, and it
 * is the honest limit of a table this size.
 */
ar_i32 ar_break_class(ar_u32 cp)
{
    /* ASCII, one row per character, because this is where text mostly lives
       and where a wrong answer is most obvious. */
    static const ar_u8 ASCII[128] =
        {
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, /* 00 */
            AR_LB_AL, AR_LB_BA, AR_LB_LF, AR_LB_BK, AR_LB_BK, AR_LB_CR, AR_LB_AL,
            AR_LB_AL, /* 08 tab, lf, vt, ff, cr */
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, /* 10 */
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, /* 18 */
            AR_LB_SP, AR_LB_EX, AR_LB_QU, AR_LB_AL, AR_LB_PR, AR_LB_PO, AR_LB_AL,
            AR_LB_QU, /*   !"#$%&' */
            AR_LB_OP, AR_LB_CP, AR_LB_AL, AR_LB_PR, AR_LB_IS, AR_LB_HY, AR_LB_IS,
            AR_LB_SY, /* ()*+,-./ */
            AR_LB_NU, AR_LB_NU, AR_LB_NU, AR_LB_NU, AR_LB_NU, AR_LB_NU, AR_LB_NU,
            AR_LB_NU, /* 01234567 */
            AR_LB_NU, AR_LB_NU, AR_LB_IS, AR_LB_IS, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_EX, /* 89:;<=>? */
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, /* @ABCDEFG */
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_OP, AR_LB_PR,
            AR_LB_CL, AR_LB_AL, AR_LB_AL, /* XYZ[\]^_ */
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL,
            AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_AL, AR_LB_OP,
            AR_LB_BA, AR_LB_CL, AR_LB_AL, AR_LB_AL /* xyz{|}~ */
        };

    struct range
    {
        ar_u32 lo, hi;
        ar_u8  cls;
    };
    static const struct range RANGES[] = {
        {0x00A0, 0x00A0, AR_LB_GL}, /* no-break space                     */
        {0x00A1, 0x00A1, AR_LB_OP}, /* inverted exclamation               */
        {0x00A2, 0x00A5, AR_LB_PR}, /* currency                           */
        {0x00AB, 0x00AB, AR_LB_QU},
        {0x00AD, 0x00AD, AR_LB_BA}, /* soft hyphen                        */
        {0x00B0, 0x00B0, AR_LB_PO},
        {0x00BB, 0x00BB, AR_LB_QU},
        {0x00BF, 0x00BF, AR_LB_OP},
        {0x0300, 0x036F, AR_LB_CM}, /* combining diacriticals             */
        {0x1AB0, 0x1AFF, AR_LB_CM},
        {0x1DC0, 0x1DFF, AR_LB_CM},
        {0x2000, 0x2006, AR_LB_BA}, /* the fixed-width spaces             */
        {0x2007, 0x2007, AR_LB_GL}, /* figure space: non-breaking         */
        {0x2008, 0x200A, AR_LB_BA},
        {0x200B, 0x200B, AR_LB_ZW}, /* zero width space                   */
        {0x2010, 0x2011, AR_LB_BA},
        {0x2012, 0x2013, AR_LB_BA}, /* figure and en dash                 */
        {0x2014, 0x2014, AR_LB_B2}, /* em dash: break either side         */
        {0x2018, 0x2019, AR_LB_QU},
        {0x201C, 0x201D, AR_LB_QU},
        {0x2020, 0x2021, AR_LB_AL},
        {0x2026, 0x2026, AR_LB_IS}, /* ellipsis                           */
        {0x2028, 0x2029, AR_LB_BK}, /* line and paragraph separator       */
        {0x2030, 0x2030, AR_LB_PO},
        {0x2039, 0x203A, AR_LB_QU},
        {0x2044, 0x2044, AR_LB_IS}, /* fraction slash                     */
        {0x2060, 0x2060, AR_LB_WJ}, /* word joiner                        */
        {0x20A0, 0x20CF, AR_LB_PR}, /* currency symbols                   */
        {0x2190, 0x2BFF, AR_LB_AL}, /* arrows, maths, misc symbols        */
        {0x3000, 0x3000, AR_LB_ID}, /* ideographic space                  */
        {0x3001, 0x3002, AR_LB_CL}, /* ideographic comma and full stop    */
        {0x3008, 0x3011, AR_LB_OP}, /* CJK brackets, opening half         */
        {0x3014, 0x301B, AR_LB_OP},
        {0x301C, 0x301C, AR_LB_NS},
        {0x3041, 0x309F, AR_LB_ID}, /* hiragana                           */
        {0x30A0, 0x30FF, AR_LB_ID}, /* katakana                           */
        {0x3400, 0x4DBF, AR_LB_ID}, /* CJK extension A                    */
        {0x4E00, 0x9FFF, AR_LB_ID}, /* CJK unified ideographs             */
        {0xAC00, 0xD7AF, AR_LB_ID}, /* Hangul syllables                   */
        {0xF900, 0xFAFF, AR_LB_ID}, /* CJK compatibility                  */
        {0xFE30, 0xFE4F, AR_LB_ID},
        {0xFEFF, 0xFEFF, AR_LB_WJ}, /* byte order mark as joiner          */
        {0xFF01, 0xFF01, AR_LB_EX},
        {0xFF08, 0xFF08, AR_LB_OP},
        {0xFF09, 0xFF09, AR_LB_CP},
        {0xFF0C, 0xFF0C, AR_LB_CL},
        {0xFF0E, 0xFF0E, AR_LB_CL},
        {0xFF1F, 0xFF1F, AR_LB_EX},
        {0xFF00, 0xFFEF, AR_LB_ID}, /* the rest of the fullwidth forms    */
        {0x20000, 0x3FFFF, AR_LB_ID}};

    ar_i32 i;
    ar_i32 n = (ar_i32)(sizeof RANGES / sizeof RANGES[0]);

    if (cp < 128u)
    {
        return ASCII[cp];
    }
    for (i = 0; i < n; ++i)
    {
        if (cp >= RANGES[i].lo && cp <= RANGES[i].hi)
        {
            return RANGES[i].cls;
        }
    }
    return AR_LB_AL;
}

/*
 * The pair table.
 *
 * Row is the class before the position, column the class after. Nonzero means
 * a break is allowed there. This is UAX #14's table reduced to the classes
 * above, with the rules that do not depend on a pair -- mandatory breaks,
 * spaces, combining marks -- handled in code before it is consulted, because
 * expressing them as pairs is where implementations of this usually go wrong.
 */
#define X 0 /* no break */
#define O 1 /* break allowed */

static const ar_u8 PAIR[AR_LB_COUNT][AR_LB_COUNT] = {
    /*         XX AL SP BK CR LF NL ZW WJ GL BA HY BB B2 OP CL CP QU NS EX IS SY NU PR PO ID CM */
    /* XX */ {X, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, X, X, O, X},
    /* AL */ {X, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, X, X, O, X},
    /* SP */ {O, O, X, X, X, X, X, X, X, O, O, O, O, O, O, X, X, O, X, X, X, X, O, O, O, O, O},
    /* BK */ {O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O},
    /* CR */ {O, O, O, O, O, X, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O},
    /* LF */ {O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O},
    /* NL */ {O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O},
    /* ZW */ {O, O, O, X, X, X, X, X, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O, O},
    /* WJ */ {X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X},
    /* GL */ {X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X},
    /* BA */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, O, O, O, O, X},
    /* HY */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, X, O, O, O, X},
    /* BB */ {X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X},
    /* B2 */ {O, O, X, X, X, X, X, X, X, X, X, X, O, X, O, X, X, X, X, X, X, X, O, O, O, O, X},
    /* OP */ {X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X},
    /* CL */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, O, X, X, O, X},
    /* CP */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, X, X, X, O, X},
    /* QU */ {X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X},
    /* NS */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, O, O, O, O, X},
    /* EX */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, O, O, O, O, X},
    /* IS */ {O, X, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, X, O, O, O, X},
    /* SY */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, X, O, O, O, X},
    /* NU */ {O, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, X, X, O, X},
    /* PR */ {O, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, O, O, X, X},
    /* PO */ {O, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, O, O, O, X},
    /* ID */ {O, O, X, X, X, X, X, X, X, X, X, X, O, O, O, X, X, X, X, X, X, X, O, O, X, O, X},
    /* CM */ {X, X, X, X, X, X, X, X, X, X, X, X, O, O, X, X, X, X, X, X, X, X, X, X, X, O, X}};

#undef X
#undef O

static int ar__mandatory(ar_i32 cls)
{
    return cls == AR_LB_BK || cls == AR_LB_LF || cls == AR_LB_NL;
}

ar_i32 ar_break_next(const char *text, ar_i32 from, ar_i32 *kind)
{
    const char *p;
    const char *prev_start;
    ar_u32      cp, next;
    ar_i32      cls, next_cls;
    ar_i32      spaces;

    *kind = AR_BREAK_NONE;

    /* A null string separately from an exhausted one, because the branch below
       reads text[from] and the two used to share it -- so the one case that
       tested for null was also the one that dereferenced it. Found by cppcheck,
       which is right: ar_break_next(0, 0, &k) walked off a null pointer. */
    if (!text)
    {
        return from;
    }

    /* Assigned here rather than at the declaration: text + from is undefined
       when text is null, whether or not anything reads it. C89 wants the
       declaration at the top of the block, so the two are separated. */
    p = text + from;
    if (!*p)
    {
        while (text[from])
        {
            ++from;
        }
        return from;
    }

    for (;;)
    {
        prev_start = p;
        cp = ar_utf8_next(&p);
        if (cp == 0)
        {
            break;
        }
        cls = ar_break_class(cp);

        /* A carriage return followed by a line feed is one break, not two. */
        if (cls == AR_LB_CR)
        {
            const char *look = p;
            if (ar_utf8_next(&look) == 0x0A)
            {
                p = look;
            }
            *kind = AR_BREAK_MANDATORY;
            return (ar_i32)(p - text);
        }
        if (ar__mandatory(cls))
        {
            *kind = AR_BREAK_MANDATORY;
            return (ar_i32)(p - text);
        }

        /* Spaces belong to the line before the break, so a run of them is
           consumed here and the opportunity reported after it. That is what
           makes trailing spaces vanish at a wrap rather than indent the line
           that follows. */
        spaces = 0;
        while (cls == AR_LB_SP)
        {
            const char *look = p;
            ar_u32      c2 = ar_utf8_next(&look);
            ++spaces;
            if (c2 == 0)
            {
                *kind = AR_BREAK_NONE;
                return (ar_i32)(p - text);
            }
            if (ar_break_class(c2) != AR_LB_SP)
            {
                break;
            }
            p = look;
        }

        {
            const char *look = p;
            next = ar_utf8_next(&look);
            if (next == 0)
            {
                break;
            }
            next_cls = ar_break_class(next);

            /* A combining mark takes the class of what it attaches to, so it
               can never be separated from it. */
            if (next_cls == AR_LB_CM)
            {
                p = look;
                continue;
            }

            if (spaces > 0)
            {
                /* After a space, break unless the next thing forbids it --
                   a word joiner, glue, or a zero-width space. */
                if (next_cls != AR_LB_WJ && next_cls != AR_LB_GL && next_cls != AR_LB_ZW)
                {
                    *kind = AR_BREAK_ALLOWED;
                    return (ar_i32)(p - text);
                }
                p = look;
                continue;
            }

            if (PAIR[cls][next_cls])
            {
                *kind = AR_BREAK_ALLOWED;
                return (ar_i32)(p - text);
            }
        }
        (void)prev_start;
    }

    {
        ar_i32 end = from;
        while (text[end])
        {
            ++end;
        }
        return end;
    }
}
