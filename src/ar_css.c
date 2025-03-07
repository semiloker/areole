/*
 * areole - the CSS subset: tokenizer, parser, and style resolution.
 * SPDX-License-Identifier: MIT
 *
 * A stylesheet is parsed once, at startup, into a flat array of rules. Nothing
 * here runs per frame except ar_sheet_resolve, and nothing here allocates.
 *
 * The subset is deliberately small. Simple selectors only, because matching
 * one is a hash compare while matching a descendant selector means walking the
 * ancestor chain for every node on every frame. Nothing in a real interface
 * has needed one yet.
 */
#include "ar_css.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Hashing
 * ------------------------------------------------------------------------ */

/* FNV-1a. Chosen because it is eight lines and has no shift-and-add pathology
   on short lowercase identifiers, which is all this ever hashes. A hash of
   zero is remapped, since zero is the wildcard in a rule. */
ar_u32 ar_hash(const char *s, ar_u32 len)
{
    ar_u32 h = 2166136261u;
    ar_u32 i;

    for (i = 0; i < len; ++i)
    {
        h ^= (ar_u32)(unsigned char)s[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* ------------------------------------------------------------------------
 * Defaults
 * ------------------------------------------------------------------------ */
void ar_style_defaults(ar_style *s)
{
    ar_i32 i;

    for (i = 0; i < AR_P_COUNT; ++i)
    {
        s->v[i] = 0;
        s->unit[i] = AR_UNIT_PX;
    }

    s->v[AR_P_DISPLAY] = AR_DISPLAY_FLEX;
    s->unit[AR_P_DISPLAY] = AR_UNIT_KEYWORD;
    s->v[AR_P_DIRECTION] = AR_DIR_ROW;
    s->unit[AR_P_DIRECTION] = AR_UNIT_KEYWORD;
    s->v[AR_P_JUSTIFY] = AR_JUSTIFY_START;
    s->unit[AR_P_JUSTIFY] = AR_UNIT_KEYWORD;
    s->v[AR_P_ALIGN] = AR_ALIGN_START;
    s->unit[AR_P_ALIGN] = AR_UNIT_KEYWORD;
    s->v[AR_P_OVERFLOW] = AR_OVERFLOW_VISIBLE;
    s->unit[AR_P_OVERFLOW] = AR_UNIT_KEYWORD;

    /* A box with no stated size takes the size of its content. This is what
       makes a stylesheet that says nothing about width still lay out. */
    s->unit[AR_P_WIDTH] = AR_UNIT_AUTO;
    s->unit[AR_P_HEIGHT] = AR_UNIT_AUTO;

    s->v[AR_P_MAX_WIDTH] = 0x7FFFFFFF;
    s->v[AR_P_MAX_HEIGHT] = 0x7FFFFFFF;

    s->v[AR_P_BACKGROUND] = 0; /* fully transparent, so nothing is painted */
    s->unit[AR_P_BACKGROUND] = AR_UNIT_COLOR;
    s->v[AR_P_COLOR] = (ar_i32)0xFF202020u;
    s->unit[AR_P_COLOR] = AR_UNIT_COLOR;
    s->v[AR_P_BORDER_COLOR] = 0;
    s->unit[AR_P_BORDER_COLOR] = AR_UNIT_COLOR;

    s->v[AR_P_FONT_SIZE] = 8; /* one face height, meaning scale 1 */
}

void ar_style_merge(ar_style *dst, const ar_style *src, ar_u32 set)
{
    ar_i32 i;

    for (i = 0; i < AR_P_COUNT; ++i)
    {
        if (set & (1u << i))
        {
            dst->v[i] = src->v[i];
            dst->unit[i] = src->unit[i];
        }
    }
}

/* ------------------------------------------------------------------------
 * Scanner
 * ------------------------------------------------------------------------ */
typedef struct ar__scan
{
    const char *base;
    const char *p;
    const char *end;
    ar_sheet   *sheet;
} ar__scan;

static void ar__fail(ar__scan *z)
{
    if (z->sheet->errors == 0)
    {
        z->sheet->first_error_offset = (ar_u32)(z->p - z->base);
    }
    z->sheet->errors++;
}

static int ar__is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static int ar__is_ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
}

static int ar__is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static void ar__skip_ws(ar__scan *z)
{
    while (z->p < z->end)
    {
        if (ar__is_space(*z->p))
        {
            z->p++;
        }
        else if (*z->p == '/' && z->p + 1 < z->end && z->p[1] == '*')
        {
            z->p += 2;
            while (z->p + 1 < z->end && !(*z->p == '*' && z->p[1] == '/'))
            {
                z->p++;
            }
            z->p = (z->p + 2 < z->end) ? z->p + 2 : z->end;
        }
        else
        {
            return;
        }
    }
}

/* Returns the length of the identifier at the cursor and advances past it. */
static ar_u32 ar__ident(ar__scan *z, const char **out)
{
    const char *start = z->p;

    while (z->p < z->end && ar__is_ident(*z->p))
    {
        z->p++;
    }
    *out = start;
    return (ar_u32)(z->p - start);
}

static int ar__hex_digit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/* #rgb, #rrggbb and #rrggbbaa. Named colours are deliberately absent: a table
   of a hundred and forty names earns its place in a browser, not here. */
static int ar__parse_hex_color(ar__scan *z, ar_u32 *out)
{
    int    digits[8];
    ar_u32 n = 0;
    ar_u32 r, g, b, a;

    z->p++; /* the # */
    while (n < 8 && z->p < z->end)
    {
        int d = ar__hex_digit(*z->p);
        if (d < 0)
        {
            break;
        }
        digits[n++] = d;
        z->p++;
    }

    if (n == 3)
    {
        r = (ar_u32)(digits[0] * 17);
        g = (ar_u32)(digits[1] * 17);
        b = (ar_u32)(digits[2] * 17);
        a = 255u;
    }
    else if (n == 6 || n == 8)
    {
        r = (ar_u32)(digits[0] * 16 + digits[1]);
        g = (ar_u32)(digits[2] * 16 + digits[3]);
        b = (ar_u32)(digits[4] * 16 + digits[5]);
        a = (n == 8) ? (ar_u32)(digits[6] * 16 + digits[7]) : 255u;
    }
    else
    {
        return 0;
    }

    *out = (a << 24) | (r << 16) | (g << 8) | b;
    return 1;
}

/* ------------------------------------------------------------------------
 * Property table
 * ------------------------------------------------------------------------ */
typedef struct ar__prop_entry
{
    const char *name;
    ar_u8       prop;
} ar__prop_entry;

/* Shorthands are expanded by the parser rather than stored, so nothing
   downstream has to know that padding is not a single property. */
enum
{
    AR_SH_PADDING = AR_P_COUNT + 1,
    AR_SH_MARGIN,
    AR_SH_BORDER
};

static const ar__prop_entry AR_PROPS[] = {{"display", AR_P_DISPLAY},
                                          {"flex-direction", AR_P_DIRECTION},
                                          {"justify-content", AR_P_JUSTIFY},
                                          {"align-items", AR_P_ALIGN},
                                          {"gap", AR_P_GAP},
                                          {"padding", AR_SH_PADDING},
                                          {"padding-top", AR_P_PAD_TOP},
                                          {"padding-right", AR_P_PAD_RIGHT},
                                          {"padding-bottom", AR_P_PAD_BOTTOM},
                                          {"padding-left", AR_P_PAD_LEFT},
                                          {"margin", AR_SH_MARGIN},
                                          {"margin-top", AR_P_MARGIN_TOP},
                                          {"margin-right", AR_P_MARGIN_RIGHT},
                                          {"margin-bottom", AR_P_MARGIN_BOTTOM},
                                          {"margin-left", AR_P_MARGIN_LEFT},
                                          {"width", AR_P_WIDTH},
                                          {"height", AR_P_HEIGHT},
                                          {"min-width", AR_P_MIN_WIDTH},
                                          {"min-height", AR_P_MIN_HEIGHT},
                                          {"max-width", AR_P_MAX_WIDTH},
                                          {"max-height", AR_P_MAX_HEIGHT},
                                          {"background", AR_P_BACKGROUND},
                                          {"background-color", AR_P_BACKGROUND},
                                          {"color", AR_P_COLOR},
                                          {"border", AR_SH_BORDER},
                                          {"border-width", AR_P_BORDER_WIDTH},
                                          {"border-color", AR_P_BORDER_COLOR},
                                          {"border-radius", AR_P_BORDER_RADIUS},
                                          {"font-size", AR_P_FONT_SIZE},
                                          {"overflow", AR_P_OVERFLOW}};

#define AR_PROP_COUNT ((ar_i32)(sizeof AR_PROPS / sizeof AR_PROPS[0]))

static int ar__same(const char *a, ar_u32 alen, const char *b)
{
    ar_u32 i;

    for (i = 0; i < alen; ++i)
    {
        if (b[i] == 0 || a[i] != b[i])
        {
            return 0;
        }
    }
    return b[alen] == 0;
}

static ar_i32 ar__lookup_prop(const char *name, ar_u32 len)
{
    ar_i32 i;

    /* ponytail: linear scan of thirty entries, run once per declaration at
       startup and never again. A perfect hash would save microseconds that
       nobody spends. */
    for (i = 0; i < AR_PROP_COUNT; ++i)
    {
        if (ar__same(name, len, AR_PROPS[i].name))
        {
            return (ar_i32)AR_PROPS[i].prop;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------
 * Keywords
 * ------------------------------------------------------------------------ */
typedef struct ar__kw
{
    const char *name;
    ar_u8       prop;
    ar_i32      value;
} ar__kw;

static const ar__kw AR_KEYWORDS[] = {{"none", AR_P_DISPLAY, AR_DISPLAY_NONE},
                                     {"block", AR_P_DISPLAY, AR_DISPLAY_BLOCK},
                                     {"flex", AR_P_DISPLAY, AR_DISPLAY_FLEX},

                                     {"row", AR_P_DIRECTION, AR_DIR_ROW},
                                     {"column", AR_P_DIRECTION, AR_DIR_COLUMN},

                                     {"flex-start", AR_P_JUSTIFY, AR_JUSTIFY_START},
                                     {"start", AR_P_JUSTIFY, AR_JUSTIFY_START},
                                     {"center", AR_P_JUSTIFY, AR_JUSTIFY_CENTER},
                                     {"flex-end", AR_P_JUSTIFY, AR_JUSTIFY_END},
                                     {"end", AR_P_JUSTIFY, AR_JUSTIFY_END},
                                     {"space-between", AR_P_JUSTIFY, AR_JUSTIFY_BETWEEN},

                                     {"flex-start", AR_P_ALIGN, AR_ALIGN_START},
                                     {"start", AR_P_ALIGN, AR_ALIGN_START},
                                     {"center", AR_P_ALIGN, AR_ALIGN_CENTER},
                                     {"flex-end", AR_P_ALIGN, AR_ALIGN_END},
                                     {"end", AR_P_ALIGN, AR_ALIGN_END},
                                     {"stretch", AR_P_ALIGN, AR_ALIGN_STRETCH},

                                     {"visible", AR_P_OVERFLOW, AR_OVERFLOW_VISIBLE},
                                     {"hidden", AR_P_OVERFLOW, AR_OVERFLOW_HIDDEN},
                                     {"scroll", AR_P_OVERFLOW, AR_OVERFLOW_SCROLL}};

#define AR_KEYWORD_COUNT ((ar_i32)(sizeof AR_KEYWORDS / sizeof AR_KEYWORDS[0]))

static int ar__lookup_keyword(ar_u8 prop, const char *name, ar_u32 len, ar_i32 *out)
{
    ar_i32 i;

    for (i = 0; i < AR_KEYWORD_COUNT; ++i)
    {
        if (AR_KEYWORDS[i].prop == prop && ar__same(name, len, AR_KEYWORDS[i].name))
        {
            *out = AR_KEYWORDS[i].value;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------ */
typedef struct ar__value
{
    ar_i32 v;
    ar_u8  unit;
    int    ok;
} ar__value;

static ar__value ar__parse_value(ar__scan *z, ar_u8 prop)
{
    ar__value out;
    ar_u32    color;

    out.v = 0;
    out.unit = AR_UNIT_PX;
    out.ok = 0;

    ar__skip_ws(z);
    if (z->p >= z->end)
    {
        return out;
    }

    if (*z->p == '#')
    {
        if (!ar__parse_hex_color(z, &color))
        {
            return out;
        }
        out.v = (ar_i32)color;
        out.unit = AR_UNIT_COLOR;
        out.ok = 1;
        return out;
    }

    if (ar__is_digit(*z->p) || (*z->p == '-' && z->p + 1 < z->end && ar__is_digit(z->p[1])))
    {
        ar_i32 sign = 1;
        ar_i32 n = 0;

        if (*z->p == '-')
        {
            sign = -1;
            z->p++;
        }
        while (z->p < z->end && ar__is_digit(*z->p))
        {
            n = n * 10 + (*z->p - '0');
            z->p++;
        }
        /* A fractional part is accepted and floored. Sub-pixel sizes are not
           a thing here: the layout is integer end to end. */
        if (z->p < z->end && *z->p == '.')
        {
            z->p++;
            while (z->p < z->end && ar__is_digit(*z->p))
            {
                z->p++;
            }
        }

        out.v = sign * n;
        out.ok = 1;
        out.unit = AR_UNIT_PX;

        if (z->p < z->end && *z->p == '%')
        {
            z->p++;
            out.unit = AR_UNIT_PCT;
        }
        else if (z->p + 1 < z->end && z->p[0] == 'p' && z->p[1] == 'x')
        {
            z->p += 2;
        }
        return out;
    }

    {
        const char *name;
        ar_u32      len = ar__ident(z, &name);
        ar_i32      kw;

        if (len == 0)
        {
            return out;
        }

        if (ar__same(name, len, "auto"))
        {
            out.unit = AR_UNIT_AUTO;
            out.ok = 1;
            return out;
        }
        /* Not CSS, and deliberately so. "grow" says what flex-grow:1 does
           without dragging in basis, shrink and the rest of the algebra. */
        if (ar__same(name, len, "grow"))
        {
            out.unit = AR_UNIT_GROW;
            out.v = 1;
            out.ok = 1;
            return out;
        }
        if (ar__same(name, len, "transparent"))
        {
            out.unit = AR_UNIT_COLOR;
            out.v = 0;
            out.ok = 1;
            return out;
        }
        if (ar__lookup_keyword(prop, name, len, &kw))
        {
            out.v = kw;
            out.unit = AR_UNIT_KEYWORD;
            out.ok = 1;
            return out;
        }
    }
    return out;
}

static void ar__set(ar_rule *rule, ar_u8 prop, ar_i32 v, ar_u8 unit)
{
    rule->style.v[prop] = v;
    rule->style.unit[prop] = unit;
    rule->set |= 1u << prop;
}

/* ------------------------------------------------------------------------
 * Declarations
 * ------------------------------------------------------------------------ */
static void ar__parse_decl(ar__scan *z, ar_rule *rule)
{
    const char *name;
    ar_u32      len;
    ar_i32      prop;
    ar__value   vals[4];
    ar_i32      n;

    len = ar__ident(z, &name);
    if (len == 0)
    {
        /* A parser must always consume something. Returning here without
           advancing lets the block loop call this function forever on a stray
           semicolon or any other character that cannot start a property, and
           a hang on malformed input is a far worse failure than a wrong
           colour. Costing one character guarantees progress on any input. */
        ar__fail(z);
        z->p++;
        return;
    }

    ar__skip_ws(z);
    if (z->p >= z->end || *z->p != ':')
    {
        ar__fail(z);
        return;
    }
    z->p++;

    prop = ar__lookup_prop(name, len);
    if (prop < 0)
    {
        ar__fail(z);
        /* Skip to the end of this declaration so one unknown property does
           not derail the rest of the block, and step over the semicolon so
           the next call starts on a property rather than on punctuation it
           cannot consume. */
        while (z->p < z->end && *z->p != ';' && *z->p != '}')
        {
            z->p++;
        }
        if (z->p < z->end && *z->p == ';')
        {
            z->p++;
        }
        return;
    }

    /* Up to four values, which covers every shorthand in the subset. */
    n = 0;
    while (n < 4)
    {
        ar_u8       as = (ar_u8)(prop >= AR_P_COUNT ? AR_P_PAD_TOP : prop);
        const char *before;
        ar__value   val;

        if (prop == AR_SH_BORDER)
        {
            as = AR_P_BORDER_WIDTH;
        }

        ar__skip_ws(z);
        if (z->p >= z->end || *z->p == ';' || *z->p == '}')
        {
            break;
        }

        before = z->p;
        val = ar__parse_value(z, as);
        if (!val.ok)
        {
            /* Two different failures, and conflating them is what made this
               loop wrong the first time.

               A word that is simply not a value for this property, such as
               the "solid" in "1px solid #eee", has already been consumed by
               the attempt. Writing it is reflex and rejecting the declaration
               over it would be obnoxious, so carry on looking for values.

               A character that cannot begin a value at all consumed nothing,
               and continuing without forcing progress spins forever. */
            if (z->p == before)
            {
                ar__fail(z);
                z->p++;
            }
            continue;
        }
        vals[n++] = val;
    }

    if (n == 0)
    {
        ar__fail(z);
    }
    else if (prop == AR_SH_PADDING || prop == AR_SH_MARGIN)
    {
        ar_u8 base = (ar_u8)(prop == AR_SH_PADDING ? AR_P_PAD_TOP : AR_P_MARGIN_TOP);
        /* One value is all sides, two is vertical then horizontal, three adds
           a separate bottom, four is clockwise from the top. */
        ar__value top = vals[0];
        ar__value right = (n >= 2) ? vals[1] : vals[0];
        ar__value bottom = (n >= 3) ? vals[2] : vals[0];
        ar__value left = (n >= 4) ? vals[3] : right;

        ar__set(rule, (ar_u8)(base + 0), top.v, top.unit);
        ar__set(rule, (ar_u8)(base + 1), right.v, right.unit);
        ar__set(rule, (ar_u8)(base + 2), bottom.v, bottom.unit);
        ar__set(rule, (ar_u8)(base + 3), left.v, left.unit);
    }
    else if (prop == AR_SH_BORDER)
    {
        ar_i32 i;
        ar__set(rule, AR_P_BORDER_WIDTH, vals[0].v, AR_UNIT_PX);
        for (i = 0; i < n; ++i)
        {
            if (vals[i].unit == AR_UNIT_COLOR)
            {
                ar__set(rule, AR_P_BORDER_COLOR, vals[i].v, AR_UNIT_COLOR);
            }
        }
    }
    else
    {
        ar__set(rule, (ar_u8)prop, vals[0].v, vals[0].unit);
    }

    while (z->p < z->end && *z->p != ';' && *z->p != '}')
    {
        z->p++;
    }
    if (z->p < z->end && *z->p == ';')
    {
        z->p++;
    }
}

/* ------------------------------------------------------------------------
 * Selectors
 * ------------------------------------------------------------------------ */
int ar_selector_split(const char *sel, ar_u32 *tag, ar_u32 *klass, ar_u32 *id)
{
    const char *p = sel;
    int         any = 0;

    *tag = 0;
    *klass = 0;
    *id = 0;

    if (!sel)
    {
        return 0;
    }

    while (*p)
    {
        const char *start;

        if (*p == '.' || *p == '#')
        {
            char mark = *p++;
            start = p;
            while (*p && ar__is_ident(*p))
            {
                p++;
            }
            if (p == start)
            {
                return 0;
            }
            if (mark == '.')
            {
                *klass = ar_hash(start, (ar_u32)(p - start));
            }
            else
            {
                *id = ar_hash(start, (ar_u32)(p - start));
            }
            any = 1;
        }
        else if (ar__is_ident(*p))
        {
            start = p;
            while (*p && ar__is_ident(*p))
            {
                p++;
            }
            *tag = ar_hash(start, (ar_u32)(p - start));
            any = 1;
        }
        else
        {
            return 0;
        }
    }
    return any;
}

static int ar__parse_selector(ar__scan *z, ar_rule *rule)
{
    int any = 0;

    rule->tag = 0;
    rule->klass = 0;
    rule->id = 0;
    rule->state = AR_STATE_NONE;
    rule->specificity = 0;

    for (;;)
    {
        const char *name;
        ar_u32      len;

        if (z->p >= z->end)
        {
            return 0;
        }

        if (*z->p == '.' || *z->p == '#' || *z->p == ':')
        {
            char mark = *z->p++;
            len = ar__ident(z, &name);
            if (len == 0)
            {
                return 0;
            }
            if (mark == '.')
            {
                rule->klass = ar_hash(name, len);
                rule->specificity += 10;
            }
            else if (mark == '#')
            {
                rule->id = ar_hash(name, len);
                rule->specificity += 100;
            }
            else
            {
                if (ar__same(name, len, "hover"))
                {
                    rule->state |= AR_STATE_HOVER;
                }
                else if (ar__same(name, len, "active"))
                {
                    rule->state |= AR_STATE_ACTIVE;
                }
                else if (ar__same(name, len, "focus"))
                {
                    rule->state |= AR_STATE_FOCUS;
                }
                else
                {
                    return 0;
                }
                rule->specificity += 10;
            }
            any = 1;
        }
        else if (ar__is_ident(*z->p))
        {
            len = ar__ident(z, &name);
            rule->tag = ar_hash(name, len);
            rule->specificity += 1;
            any = 1;
        }
        else
        {
            break;
        }
    }
    return any;
}

/* ------------------------------------------------------------------------
 * Sheet
 * ------------------------------------------------------------------------ */
void ar_sheet_init(ar_sheet *sheet, ar_rule *storage, ar_u16 capacity)
{
    sheet->rules = storage;
    sheet->count = 0;
    sheet->capacity = capacity;
    sheet->errors = 0;
    sheet->first_error_offset = 0;
}

/* Ascending specificity, ties broken by source order, so resolution can apply
   rules front to back and let the last writer win. Insertion sort because the
   input is nearly sorted already and this runs once. */
static void ar__sort_rules(ar_sheet *sheet)
{
    ar_i32 i, j;

    for (i = 1; i < (ar_i32)sheet->count; ++i)
    {
        ar_rule tmp = sheet->rules[i];
        j = i;
        while (j > 0 && (sheet->rules[j - 1].specificity > tmp.specificity ||
                         (sheet->rules[j - 1].specificity == tmp.specificity &&
                          sheet->rules[j - 1].order > tmp.order)))
        {
            sheet->rules[j] = sheet->rules[j - 1];
            j--;
        }
        sheet->rules[j] = tmp;
    }
}

void ar_sheet_parse(ar_sheet *sheet, const char *css)
{
    ar__scan z;

    if (!css)
    {
        return;
    }

    z.base = css;
    z.p = css;
    z.end = css + strlen(css);
    z.sheet = sheet;

    for (;;)
    {
        ar_rule rule;

        ar__skip_ws(&z);
        if (z.p >= z.end)
        {
            break;
        }

        memset(&rule, 0, sizeof rule);
        ar_style_defaults(&rule.style);
        rule.set = 0;

        if (!ar__parse_selector(&z, &rule))
        {
            ar__fail(&z);
            /* Resynchronise on the next block, so one bad selector costs one
               rule rather than the remainder of the stylesheet. */
            while (z.p < z.end && *z.p != '}')
            {
                z.p++;
            }
            if (z.p < z.end)
            {
                z.p++;
            }
            continue;
        }

        ar__skip_ws(&z);
        if (z.p >= z.end || *z.p != '{')
        {
            ar__fail(&z);
            while (z.p < z.end && *z.p != '}')
            {
                z.p++;
            }
            if (z.p < z.end)
            {
                z.p++;
            }
            continue;
        }
        z.p++;

        for (;;)
        {
            ar__skip_ws(&z);
            if (z.p >= z.end || *z.p == '}')
            {
                break;
            }
            ar__parse_decl(&z, &rule);
        }
        if (z.p < z.end)
        {
            z.p++; /* the closing brace */
        }

        if (rule.set == 0)
        {
            continue; /* an empty block is legal and simply has no effect */
        }
        if (sheet->count >= sheet->capacity)
        {
            ar__fail(&z);
            break;
        }

        rule.order = sheet->count;
        sheet->rules[sheet->count++] = rule;
    }

    ar__sort_rules(sheet);
}

void ar_sheet_resolve(const ar_sheet *sheet, ar_u32 tag, ar_u32 klass, ar_u32 id, ar_u8 state,
                      ar_style *out)
{
    ar_i32 i;

    ar_style_defaults(out);

    /* ponytail: a linear pass over every rule per box. At a few hundred rules
       and a few hundred boxes that is well under the noise floor, and it is
       measured by the layout phase in the overlay. The upgrade path when it
       stops being free is a cache keyed on tag, class, id and state, which is
       exactly the tuple this function takes. */
    for (i = 0; i < (ar_i32)sheet->count; ++i)
    {
        const ar_rule *r = &sheet->rules[i];

        if (r->tag && r->tag != tag)
        {
            continue;
        }
        if (r->klass && r->klass != klass)
        {
            continue;
        }
        if (r->id && r->id != id)
        {
            continue;
        }
        /* A rule with no pseudo-class applies in every state. A rule with one
           applies only when the box is in that state. */
        if (r->state && (state & r->state) != r->state)
        {
            continue;
        }
        ar_style_merge(out, &r->style, r->set);
    }
}
