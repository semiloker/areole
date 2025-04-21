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

    s->set = 0;
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
    /* Stretch, as CSS does. The whole pitch is that this is real CSS, so a
       private default that happens to be friendlier would cost more in
       surprise than it saves in typing. It only affects boxes that state no
       size of their own, which is what makes it safe as a default. */
    s->v[AR_P_ALIGN] = AR_ALIGN_STRETCH;
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

void ar_classes_clear(ar_classes *c)
{
    c->n = 0;
    c->combined = 0;
}

void ar_classes_add(ar_classes *c, ar_u32 hash)
{
    ar_i32 i;

    if (!hash || c->n >= AR_MAX_CLASSES)
    {
        return;
    }
    for (i = 0; i < c->n; ++i)
    {
        if (c->h[i] == hash)
        {
            return;
        }
    }
    c->h[c->n++] = hash;
    /* Order-independent, so that a box declared .a.b and one declared .b.a
       land on the same cache entry, because they are the same box. Addition
       and multiplication by an odd constant is enough to spread them; the
       collision bound is the same one in 2^32 the rest of this library's
       hashes carry. */
    c->combined += hash * 2654435761u;
}

int ar_classes_has(const ar_classes *have, ar_u32 klass)
{
    ar_i32 i;

    for (i = 0; i < have->n; ++i)
    {
        if (have->h[i] == klass)
        {
            return 1;
        }
    }
    return 0;
}

int ar_classes_contains(const ar_classes *have, const ar_classes *want)
{
    ar_i32 i, j;

    for (i = 0; i < want->n; ++i)
    {
        int found = 0;
        for (j = 0; j < have->n && !found; ++j)
        {
            found = (have->h[j] == want->h[i]);
        }
        if (!found)
        {
            return 0;
        }
    }
    return 1;
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
    dst->set |= set;
}

/*
 * Which properties inherit.
 *
 * CSS inherits text and colour and not layout, and the reason is worth stating
 * because it looks arbitrary until you try the alternative: a width that
 * inherited would make every box the size of its parent, and a padding that
 * inherited would compound at every level. Colour and size inherit because a
 * document has one of each and repeating them is the noise a stylesheet exists
 * to remove.
 *
 * Note that AR_P_DIRECTION is flex-direction, not the CSS `direction`
 * property, and does not inherit. That the two share a name is a trap worth
 * one line of comment.
 */
int ar_prop_inherits(ar_i32 prop)
{
    switch (prop)
    {
    case AR_P_COLOR:
    case AR_P_FONT_SIZE:
        return 1;
    default:
        return 0;
    }
}

void ar_style_inherit(ar_style *child, const ar_style *parent)
{
    ar_style defaults;
    ar_i32   i;

    ar_style_defaults(&defaults);

    for (i = 0; i < AR_P_COUNT; ++i)
    {
        /* The explicit keywords first, because they override the question of
           whether the property inherits by default -- that is what they are
           for. `inherit` on a non-inherited property is the interesting case
           and the one CSS authors reach for. */
        if (child->set & (1u << (ar_u32)i))
        {
            if (child->unit[i] == AR_UNIT_INHERIT)
            {
                child->v[i] = parent->v[i];
                child->unit[i] = parent->unit[i];
                continue;
            }
            if (child->unit[i] == AR_UNIT_INITIAL)
            {
                child->v[i] = defaults.v[i];
                child->unit[i] = defaults.unit[i];
                continue;
            }
        }
        if (!ar_prop_inherits(i))
        {
            continue;
        }
        if (child->set & (1u << (ar_u32)i))
        {
            continue; /* the child said something; it wins */
        }
        child->v[i] = parent->v[i];
        child->unit[i] = parent->unit[i];
        /* Marked as set, so a grandchild inherits through a box that only
           inherited it -- which is the whole point of a cascade. */
        child->set |= 1u << (ar_u32)i;
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
                                          {"overflow", AR_P_OVERFLOW},
                                          {"text-align", AR_P_TEXT_ALIGN},
                                          {"vertical-align", AR_P_VERTICAL_ALIGN},
                                          {"float", AR_P_FLOAT},
                                          {"clear", AR_P_CLEAR}};

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
                                     {"inline-block", AR_P_DISPLAY, AR_DISPLAY_INLINE_BLOCK},

                                     {"left", AR_P_TEXT_ALIGN, AR_TEXT_ALIGN_LEFT},
                                     {"right", AR_P_TEXT_ALIGN, AR_TEXT_ALIGN_RIGHT},
                                     {"center", AR_P_TEXT_ALIGN, AR_TEXT_ALIGN_CENTER},

                                     {"left", AR_P_FLOAT, AR_FLOAT_LEFT},
                                     {"right", AR_P_FLOAT, AR_FLOAT_RIGHT},
                                     {"left", AR_P_CLEAR, AR_CLEAR_LEFT},
                                     {"right", AR_P_CLEAR, AR_CLEAR_RIGHT},
                                     {"both", AR_P_CLEAR, AR_CLEAR_BOTH},

                                     {"baseline", AR_P_VERTICAL_ALIGN, AR_VALIGN_BASELINE},
                                     {"top", AR_P_VERTICAL_ALIGN, AR_VALIGN_TOP},
                                     {"middle", AR_P_VERTICAL_ALIGN, AR_VALIGN_MIDDLE},
                                     {"bottom", AR_P_VERTICAL_ALIGN, AR_VALIGN_BOTTOM},

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
        if (ar__same(name, len, "min-content"))
        {
            out.unit = AR_UNIT_MIN_CONTENT;
            out.ok = 1;
            return out;
        }
        if (ar__same(name, len, "max-content"))
        {
            out.unit = AR_UNIT_MAX_CONTENT;
            out.ok = 1;
            return out;
        }
        if (ar__same(name, len, "fit-content"))
        {
            out.unit = AR_UNIT_FIT_CONTENT;
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
        /*
         * The explicit cascade keywords.
         *
         * `unset` resolves here rather than later because the property is
         * already known, and it is defined as exactly one of the other two.
         * `revert` would mean "back to the UA sheet", and there is no separate
         * UA layer to go back to, so it means the same as `initial`.
         */
        if (ar__same(name, len, "inherit"))
        {
            out.unit = AR_UNIT_INHERIT;
            out.ok = 1;
            return out;
        }
        if (ar__same(name, len, "initial") || ar__same(name, len, "revert"))
        {
            out.unit = AR_UNIT_INITIAL;
            out.ok = 1;
            return out;
        }
        if (ar__same(name, len, "unset"))
        {
            out.unit = (ar_u8)(ar_prop_inherits(prop) ? AR_UNIT_INHERIT : AR_UNIT_INITIAL);
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

/*
 * `!important` at the end of a declaration.
 *
 * Consumes it if it is there and reports whether it was. Written as a probe
 * rather than folded into the value parser because it can follow any value of
 * any type, and every value parser would otherwise have to know about it.
 */
static int ar__take_important(ar__scan *z)
{
    const char *save = z->p;
    const char *name;
    ar_u32      len;

    ar__skip_ws(z);
    if (z->p >= z->end || *z->p != '!')
    {
        z->p = save;
        return 0;
    }
    z->p++;
    ar__skip_ws(z);
    len = ar__ident(z, &name);
    if (len && ar__same(name, len, "important"))
    {
        return 1;
    }
    z->p = save;
    return 0;
}

/* ------------------------------------------------------------------------
 * Declarations
 * ------------------------------------------------------------------------ */
static void ar__parse_decl(ar__scan *z, ar_rule *rule)
{
    const char *name;
    ar_u32      len;
    ar_i32      prop;
    ar_u32      before_set;
    int         important;
    ar__value   vals[4];
    ar_i32      n;

    important = 0;

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
        /* Probed here rather than after the loop, because the loop's recovery
           for a character that cannot begin a value would otherwise eat the
           `!` and count it as a parse error. */
        if (*z->p == '!')
        {
            important = ar__take_important(z);
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

    /* The mask is the difference between what the rule had set before this
       declaration and after it, which marks a shorthand's four properties
       without the shorthand having to know it is being marked. */
    before_set = rule->set;

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

    if (important)
    {
        rule->important |= rule->set & ~before_set;
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
int ar_selector_split(const char *sel, ar_u32 *tag, ar_classes *klass, ar_u32 *id)
{
    const char *p = sel;
    int         any = 0;

    *tag = 0;
    *id = 0;
    ar_classes_clear(klass);

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
                /* Several are allowed and all must match. */
                ar_classes_add(klass, ar_hash(start, (ar_u32)(p - start)));
            }
            else
            {
                *id = ar_hash(start, (ar_u32)(p - start));
            }
            any = 1;
            continue;
        }

        if (*p == ':')
        {
            /* Pseudo-classes are parsed by the caller that cares; here they
               only end the compound. */
            break;
        }

        start = p;
        while (*p && ar__is_ident(*p))
        {
            p++;
        }
        if (p == start)
        {
            return 0;
        }
        if (!(p - start == 1 && *start == '*'))
        {
            *tag = ar_hash(start, (ar_u32)(p - start));
        }
        any = 1;
    }
    return any;
}

/* One state keyword, or zero if the name is not one. Shared by the compound
   parser and the functional pseudo-classes, which accept the same set. */
static ar_u16 ar__state_keyword(const char *name, ar_u32 len)
{
    if (ar__same(name, len, "hover"))
    {
        return AR_STATE_HOVER;
    }
    if (ar__same(name, len, "active"))
    {
        return AR_STATE_ACTIVE;
    }
    if (ar__same(name, len, "focus"))
    {
        return AR_STATE_FOCUS;
    }
    if (ar__same(name, len, "root"))
    {
        return AR_STATE_ROOT;
    }
    if (ar__same(name, len, "first-child"))
    {
        return AR_STATE_FIRST;
    }
    if (ar__same(name, len, "last-child"))
    {
        return AR_STATE_LAST;
    }
    if (ar__same(name, len, "only-child"))
    {
        return AR_STATE_ONLY;
    }
    if (ar__same(name, len, "empty"))
    {
        return AR_STATE_EMPTY;
    }
    return 0;
}

/*
 * One simple selector, for the inside of :not(), :is() and :where().
 *
 * A tag, a class, an id or a state -- one of each at most, and at least one of
 * something. Anything more elaborate is refused, which is what stops
 * `:not(.a.b)` from quietly matching `.a`.
 */
static int ar__parse_simple(ar__scan *z, ar_sel_simple *out, ar_u16 *spec)
{
    int any = 0;

    out->tag = 0;
    out->klass = 0;
    out->id = 0;
    out->state = 0;

    ar__skip_ws(z);
    for (;;)
    {
        const char *name;
        ar_u32      len;

        if (z->p >= z->end)
        {
            break;
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
                if (out->klass)
                {
                    return 0; /* two classes is a compound, not a simple */
                }
                out->klass = ar_hash(name, len);
                *spec = (ar_u16)(*spec + 10);
            }
            else if (mark == '#')
            {
                if (out->id)
                {
                    return 0;
                }
                out->id = ar_hash(name, len);
                *spec = (ar_u16)(*spec + 100);
            }
            else
            {
                ar_u16 st = ar__state_keyword(name, len);

                if (!st)
                {
                    return 0; /* nesting a functional pseudo-class is refused */
                }
                out->state |= st;
                *spec = (ar_u16)(*spec + 10);
            }
            any = 1;
            continue;
        }
        if (ar__is_ident(*z->p))
        {
            if (out->tag)
            {
                return 0;
            }
            len = ar__ident(z, &name);
            if (len == 0)
            {
                return 0;
            }
            out->tag = ar_hash(name, len);
            *spec = (ar_u16)(*spec + 1);
            any = 1;
            continue;
        }
        break;
    }
    return any;
}

/*
 * The parenthesised list inside :not(), :is() or :where().
 *
 * Specificity follows the specification as closely as this can: :not() and
 * :is() take the specificity of their most specific argument, :where() takes
 * none. That is why the running total is snapshotted and restored -- the
 * arguments are parsed the same way in all three cases and only the
 * bookkeeping differs.
 */
static int ar__parse_alt_list(ar__scan *z, ar_sel_simple *out, ar_i32 *count, ar_u16 *spec,
                              int contributes)
{
    ar_u16 before = *spec;
    ar_u16 best = 0;

    *count = 0;
    if (z->p >= z->end || *z->p != '(')
    {
        return 0;
    }
    z->p++;

    for (;;)
    {
        ar_u16 mine = 0;

        if (*count >= AR_MAX_ALTS)
        {
            return 0; /* longer than the list holds; refused, not truncated */
        }
        if (!ar__parse_simple(z, &out[*count], &mine))
        {
            return 0;
        }
        if (mine > best)
        {
            best = mine;
        }
        (*count)++;

        ar__skip_ws(z);
        if (z->p < z->end && *z->p == ',')
        {
            z->p++;
            continue;
        }
        break;
    }

    if (z->p >= z->end || *z->p != ')')
    {
        return 0;
    }
    z->p++;
    *spec = (ar_u16)(before + (contributes ? best : 0));
    return 1;
}

/* One compound: a tag, any number of classes, an id, and pseudo-classes.
   Returns zero if there was nothing to read, which is how the caller knows a
   combinator was dangling. */
static int ar__parse_compound(ar__scan *z, ar_u32 *tag, ar_classes *klass, ar_u32 *id,
                              ar_u16 *state, ar_u16 *spec, ar_sel_simple *neg, ar_i32 *nneg,
                              ar_sel_simple *alt, ar_i32 *nalt)
{
    int any = 0;

    *tag = 0;
    *id = 0;
    *nneg = 0;
    *nalt = 0;
    ar_classes_clear(klass);

    for (;;)
    {
        const char *name;
        ar_u32      len;

        if (z->p >= z->end)
        {
            break;
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
                ar_classes_add(klass, ar_hash(name, len));
                *spec = (ar_u16)(*spec + 10);
            }
            else if (mark == '#')
            {
                *id = ar_hash(name, len);
                *spec = (ar_u16)(*spec + 100);
            }
            else if (ar__same(name, len, "not") || ar__same(name, len, "is") ||
                     ar__same(name, len, "where"))
            {
                int is_not = ar__same(name, len, "not");
                int contributes = !ar__same(name, len, "where");

                if (!ar__parse_alt_list(z, is_not ? neg : alt, is_not ? nneg : nalt, spec,
                                        contributes))
                {
                    return 0;
                }
                any = 1;
                continue;
            }
            else
            {
                if (ar__same(name, len, "hover"))
                {
                    *state |= AR_STATE_HOVER;
                }
                else if (ar__same(name, len, "active"))
                {
                    *state |= AR_STATE_ACTIVE;
                }
                else if (ar__same(name, len, "focus"))
                {
                    *state |= AR_STATE_FOCUS;
                }
                else if (ar__same(name, len, "root"))
                {
                    *state |= AR_STATE_ROOT;
                }
                else if (ar__same(name, len, "first-child"))
                {
                    *state |= AR_STATE_FIRST;
                }
                else if (ar__same(name, len, "last-child"))
                {
                    *state |= AR_STATE_LAST;
                }
                else if (ar__same(name, len, "only-child"))
                {
                    *state |= AR_STATE_ONLY;
                }
                else if (ar__same(name, len, "empty"))
                {
                    *state |= AR_STATE_EMPTY;
                }
                /*
                 * :nth-child, of which only odd and even are here.
                 *
                 * They are the two that striping actually uses, and they are
                 * two bits set at declare time. The general an+b form needs a
                 * pair of numbers stored per rule and a modulo per box, which
                 * is a different mechanism for a much rarer selector; it is
                 * refused rather than silently mismatched.
                 */
                else if (ar__same(name, len, "nth-child"))
                {
                    ar_u32 arg;

                    if (z->p >= z->end || *z->p != '(')
                    {
                        return 0;
                    }
                    z->p++;
                    ar__skip_ws(z);
                    arg = ar__ident(z, &name);
                    ar__skip_ws(z);
                    if (z->p >= z->end || *z->p != ')')
                    {
                        return 0;
                    }
                    z->p++;
                    if (arg && ar__same(name, arg, "odd"))
                    {
                        *state |= AR_STATE_ODD;
                    }
                    else if (arg && ar__same(name, arg, "even"))
                    {
                        *state |= AR_STATE_EVEN;
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    return 0;
                }
                *spec = (ar_u16)(*spec + 10);
            }
            any = 1;
            continue;
        }

        if (ar__is_ident(*z->p))
        {
            len = ar__ident(z, &name);
            if (len == 0)
            {
                return 0;
            }
            *tag = ar_hash(name, len);
            *spec = (ar_u16)(*spec + 1);
            any = 1;
            continue;
        }
        break;
    }
    return any;
}

/*
 * A whole selector: compounds joined by combinators.
 *
 * The rightmost compound is the subject -- the element the rule is about --
 * and everything to its left is context. That is why this reads the parts
 * forwards into a scratch list and then stores them backwards: the subject is
 * known only when the selector ends.
 */
static int ar__parse_selector(ar__scan *z, ar_rule *rule)
{
    ar_sel_part parts[AR_MAX_SEL_PARTS + 1];
    ar_i32      n = 0;
    ar_u8       comb = AR_COMB_NONE;
    ar_i32      i;

    rule->tag = 0;
    ar_classes_clear(&rule->klass);
    rule->id = 0;
    rule->state = AR_STATE_NONE;
    rule->specificity = 0;
    rule->nctx = 0;

    rule->nneg = 0;
    rule->nalt = 0;

    for (;;)
    {
        ar_u32        tag, id;
        ar_classes    klass;
        ar_sel_simple neg[AR_MAX_ALTS], alt[AR_MAX_ALTS];
        ar_i32        nneg = 0, nalt = 0;
        int           got;

        if (n > AR_MAX_SEL_PARTS)
        {
            return 0; /* deeper than this holds; refused rather than truncated */
        }
        got = ar__parse_compound(z, &tag, &klass, &id, &rule->state, &rule->specificity, neg, &nneg,
                                 alt, &nalt);
        if (!got)
        {
            return 0; /* a dangling combinator is malformed */
        }
        parts[n].tag = tag;
        parts[n].klass = klass;
        parts[n].id = id;
        parts[n].comb = comb;
        ++n;

        /*
         * The functional pseudo-classes belong to the subject, and which
         * compound that is only becomes clear when the selector ends. So they
         * are carried on the newest part and overwrite whatever the previous
         * one left: if another compound follows, the previous one was context
         * and had no business carrying them.
         *
         * Which means `.page:not(.wide) .card` parses its :not() and then
         * silently drops it -- so it is caught here instead, once the next
         * compound proves this one was not the subject.
         */
        if (n > 1 && (rule->nneg || rule->nalt))
        {
            return 0;
        }
        {
            ar_i32 k;

            for (k = 0; k < nneg; ++k)
            {
                rule->neg[k] = neg[k];
            }
            for (k = 0; k < nalt; ++k)
            {
                rule->alt[k] = alt[k];
            }
            rule->nneg = nneg;
            rule->nalt = nalt;
        }

        /* What separates this compound from the next, if there is a next. */
        {
            const char *save = z->p;
            int         space = 0;

            while (z->p < z->end &&
                   (*z->p == ' ' || *z->p == '\t' || *z->p == '\n' || *z->p == '\r'))
            {
                ++z->p;
                space = 1;
            }
            if (z->p < z->end && (*z->p == '>' || *z->p == '+' || *z->p == '~'))
            {
                comb = (ar_u8)(*z->p == '>'   ? AR_COMB_CHILD
                               : *z->p == '+' ? AR_COMB_ADJACENT
                                              : AR_COMB_SIBLING);
                ++z->p;
                while (z->p < z->end &&
                       (*z->p == ' ' || *z->p == '\t' || *z->p == '\n' || *z->p == '\r'))
                {
                    ++z->p;
                }
                continue;
            }
            if (space && z->p < z->end &&
                (ar__is_ident(*z->p) || *z->p == '.' || *z->p == '#' || *z->p == ':'))
            {
                comb = AR_COMB_DESCENDANT;
                continue;
            }
            z->p = save;
            break;
        }
    }

    if (n == 0)
    {
        return 0;
    }

    /* The last part is the subject; the rest become context, nearest first.
       Each context part carries the combinator that reaches it from the part
       to its right, which is why the combinator is shifted along by one. */
    rule->tag = parts[n - 1].tag;
    rule->klass = parts[n - 1].klass;
    rule->id = parts[n - 1].id;
    rule->nctx = n - 1;
    for (i = 0; i < n - 1; ++i)
    {
        ar_i32 src = n - 2 - i;
        rule->ctx[i] = parts[src];
        rule->ctx[i].comb = parts[src + 1].comb;
    }
    return 1;
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
    sheet->has_contextual = 0;
    sheet->has_late_state = 0;
    sheet->cache = 0;
    sheet->cache_cap = 0;
    sheet->cache_hits = 0;
    sheet->cache_misses = 0;
}

void ar_sheet_set_cache(ar_sheet *sheet, ar_cache_entry *storage, ar_u16 capacity)
{
    sheet->cache = storage;
    sheet->cache_cap = capacity;
    ar_sheet_cache_clear(sheet);
}

void ar_sheet_cache_clear(ar_sheet *sheet)
{
    ar_u16 i;

    for (i = 0; i < sheet->cache_cap; ++i)
    {
        sheet->cache[i].used = 0;
    }
}

/* Ascending specificity, ties broken by source order, so resolution can apply
   rules front to back and let the last writer win. Insertion sort because the
   input is nearly sorted already and this runs once. */
static void ar__note_contextual(ar_sheet *sheet)
{
    ar_i32 i;

    for (i = 0; i < (ar_i32)sheet->count; ++i)
    {
        if (sheet->rules[i].nctx > 0)
        {
            sheet->has_contextual = 1;
        }
        if (sheet->rules[i].state & AR_STATE_LATE)
        {
            sheet->has_late_state = 1;
        }
        /* A state inside :not() or :is() counts as much as one on the compound
           -- `.row:not(:last-child)` needs the second pass just as much as
           `.row:last-child` does, and needing it without asking for it is a
           rule that silently never matches. */
        {
            ar_i32 k;

            for (k = 0; k < sheet->rules[i].nneg; ++k)
            {
                if (sheet->rules[i].neg[k].state & AR_STATE_LATE)
                {
                    sheet->has_late_state = 1;
                }
            }
            for (k = 0; k < sheet->rules[i].nalt; ++k)
            {
                if (sheet->rules[i].alt[k].state & AR_STATE_LATE)
                {
                    sheet->has_late_state = 1;
                }
            }
        }
    }
}

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

    /* New rules can change any answer already cached, and there is no cheap
       way to know which. Dropping all of it is correct and costs nothing,
       because adding a stylesheet is a startup operation. */
    ar_sheet_cache_clear(sheet);

    z.base = css;
    z.p = css;
    z.end = css + strlen(css);
    z.sheet = sheet;

    for (;;)
    {
        /*
         * A selector list shares one declaration block: `.a, .b { ... }` is two
         * rules that happen to have been written once. The selectors are parsed
         * into this array first and the block is parsed once into the first of
         * them, then copied across -- parsing the block once per selector would
         * mean the same declarations counted twice in the error tally, and
         * `!important` marked twice.
         */
        ar_rule rule[AR_MAX_SEL_LIST];
        ar_i32  sel_count = 0;
        ar_i32  k;

        ar__skip_ws(&z);
        if (z.p >= z.end)
        {
            break;
        }

        memset(&rule[0], 0, sizeof rule[0]);
        ar_style_defaults(&rule[0].style);
        rule[0].set = 0;

        if (!ar__parse_selector(&z, &rule[0]))
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
        sel_count = 1;

        /* The rest of the list, if there is one. Each selector is parsed into
           its own rule; they differ in nothing else. */
        for (;;)
        {
            ar__skip_ws(&z);
            if (z.p >= z.end || *z.p != ',')
            {
                break;
            }
            z.p++;
            /* ar__parse_compound starts on the first character of a compound,
               not on the whitespace before one -- the main loop above has
               always skipped it for the first selector. */
            ar__skip_ws(&z);
            if (sel_count >= AR_MAX_SEL_LIST)
            {
                sel_count = 0; /* longer than the array holds; refuse the lot */
                break;
            }
            memset(&rule[sel_count], 0, sizeof rule[0]);
            ar_style_defaults(&rule[sel_count].style);
            rule[sel_count].set = 0;
            if (!ar__parse_selector(&z, &rule[sel_count]))
            {
                sel_count = 0;
                break;
            }
            sel_count++;
        }
        if (sel_count == 0)
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
            ar__parse_decl(&z, &rule[0]);
        }
        if (z.p < z.end)
        {
            z.p++; /* the closing brace */
        }

        if (rule[0].set == 0)
        {
            continue; /* an empty block is legal and simply has no effect */
        }

        for (k = 0; k < sel_count; ++k)
        {
            if (sheet->count >= sheet->capacity)
            {
                ar__fail(&z);
                break;
            }
            /* Everything except the selector is the same, and rule[0] is the
               one the block was parsed into. */
            rule[k].style = rule[0].style;
            rule[k].set = rule[0].set;
            rule[k].important = rule[0].important;
            rule[k].order = sheet->count;
            sheet->rules[sheet->count++] = rule[k];
        }
    }

    ar__sort_rules(sheet);
    ar__note_contextual(sheet);
}

/* The tuple is four small integers, so a multiplicative mix over them is both
   cheaper and better distributed than hashing their bytes. */
static ar_u32 ar__cache_hash(ar_u32 tag, ar_u32 klass, ar_u32 id, ar_u16 state)
{
    ar_u32 h = 2166136261u;
    h = (h ^ tag) * 16777619u;
    h = (h ^ klass) * 16777619u;
    h = (h ^ id) * 16777619u;
    h = (h ^ (ar_u32)state) * 16777619u;
    return h;
}

int ar_sel_simple_matches(const ar_sel_simple *p, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                          ar_u16 state)
{
    if (p->tag && p->tag != tag)
    {
        return 0;
    }
    if (p->id && p->id != id)
    {
        return 0;
    }
    if (p->klass && !ar_classes_has(klass, p->klass))
    {
        return 0;
    }
    if (p->state && (state & p->state) != p->state)
    {
        return 0;
    }
    return 1;
}

/*
 * :not() and :is() on the subject compound.
 *
 * Every negation must fail and, if there are any alternatives at all, at least
 * one must match. Written as one function because every place that matches a
 * rule has to ask both questions, and a place that asked only one would be a
 * silent wrong answer rather than a loud one.
 */
static int ar__functional_matches(const ar_rule *r, ar_u32 tag, const ar_classes *klass, ar_u32 id,
                                  ar_u16 state)
{
    ar_i32 i;

    for (i = 0; i < r->nneg; ++i)
    {
        if (ar_sel_simple_matches(&r->neg[i], tag, klass, id, state))
        {
            return 0;
        }
    }
    if (r->nalt > 0)
    {
        for (i = 0; i < r->nalt; ++i)
        {
            if (ar_sel_simple_matches(&r->alt[i], tag, klass, id, state))
            {
                return 1;
            }
        }
        return 0;
    }
    return 1;
}

static void ar__resolve_uncached(const ar_sheet *sheet, ar_u32 tag, const ar_classes *klass,
                                 ar_u32 id, ar_u16 state, ar_style *out)
{
    ar_i32 i;

    ar_style_defaults(out);

    /* A linear pass over every rule. The cache above is what keeps this off
       the per-box path; it still runs once per distinct selector and state. */
    for (i = 0; i < (ar_i32)sheet->count; ++i)
    {
        const ar_rule *r = &sheet->rules[i];

        /*
         * A rule with a combinator is the contextual pass's business alone.
         *
         * Without this, `.page .card` applied to every `.card` anywhere: this
         * loop only ever looked at the subject compound, so the context was
         * silently dropped. It went unnoticed because the sheets that had
         * combinators also had a second rule that overwrote the wrong answer,
         * which is exactly the shape of bug that survives a test suite.
         */
        if (r->nctx > 0)
        {
            continue;
        }

        if (r->tag && r->tag != tag)
        {
            continue;
        }
        if (r->klass.n && !ar_classes_contains(klass, &r->klass))
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
        if (!ar__functional_matches(r, tag, klass, id, state))
        {
            continue;
        }
        ar_style_merge(out, &r->style, r->set & ~r->important);
    }

    /*
     * The important band, after everything normal.
     *
     * CSS resolves !important by running a second cascade over only the
     * important declarations, so an important rule of low specificity beats a
     * normal rule of high specificity. A second pass in the same order is
     * exactly what the specification describes, and is cheaper than sorting on
     * a compound key -- the rules are already in the right order for both.
     */
    for (i = 0; i < (ar_i32)sheet->count; ++i)
    {
        const ar_rule *r = &sheet->rules[i];

        if (!r->important || r->nctx > 0)
        {
            continue;
        }
        if (r->tag && r->tag != tag)
        {
            continue;
        }
        if (r->klass.n && !ar_classes_contains(klass, &r->klass))
        {
            continue;
        }
        if (r->id && r->id != id)
        {
            continue;
        }
        if (r->state && (state & r->state) != r->state)
        {
            continue;
        }
        if (!ar__functional_matches(r, tag, klass, id, state))
        {
            continue;
        }
        ar_style_merge(out, &r->style, r->important);
    }
}

int ar_sel_part_matches(const ar_sel_part *p, ar_u32 tag, const ar_classes *klass, ar_u32 id)
{
    if (p->tag && p->tag != tag)
    {
        return 0;
    }
    if (p->id && p->id != id)
    {
        return 0;
    }
    if (p->klass.n && !ar_classes_contains(klass, &p->klass))
    {
        return 0;
    }
    return 1;
}

/*
 * Walks a rule's context chain outwards from the box.
 *
 * Descendant and general-sibling are the two that can match more than one
 * candidate, so they retry: a rule `.page .card` has to keep looking up the
 * ancestors until it finds a page or runs out. Child and adjacent look at
 * exactly one element and fail if it is not the one.
 */
static int ar__ctx_matches(const ar_rule *r, ar_i32 index, ar_sel_walk find, void *ud)
{
    ar_i32 at = index;
    ar_i32 i;

    for (i = 0; i < r->nctx; ++i)
    {
        const ar_sel_part *part = &r->ctx[i];
        ar_i32             comb = part->comb;
        ar_i32             next;
        ar_u32             tag, id;
        ar_classes         klass;
        int                found = 0;

        if (comb == AR_COMB_CHILD || comb == AR_COMB_ADJACENT)
        {
            if (!find(ud, at, comb, &next, &tag, &klass, &id))
            {
                return 0;
            }
            if (!ar_sel_part_matches(part, tag, &klass, id))
            {
                return 0;
            }
            at = next;
            continue;
        }

        /* Descendant or general sibling: keep stepping until one matches. */
        {
            ar_i32 cursor = at;
            while (find(ud, cursor, comb, &next, &tag, &klass, &id))
            {
                cursor = next;
                if (ar_sel_part_matches(part, tag, &klass, id))
                {
                    found = 1;
                    break;
                }
            }
        }
        if (!found)
        {
            return 0;
        }
        at = next;
    }
    return 1;
}

void ar_sheet_resolve_contextual(const ar_sheet *sheet, ar_i32 index, ar_u32 tag,
                                 const ar_classes *klass, ar_u32 id, ar_u16 state, ar_sel_walk find,
                                 void *ud, ar_style *out)
{
    ar_i32 i;

    if (!sheet->has_contextual || !find)
    {
        return;
    }
    /* Rules are already in ascending specificity and source order, so applying
       them front to back leaves the winner on top -- the same property the
       simple pass relies on. The two passes therefore compose: a contextual
       rule of higher specificity beats a simple one, because it is applied
       after. A contextual rule of *lower* specificity also lands after, which
       is where this parts company with the specification, and is the price of
       keeping the common case in a cache. */
    for (i = 0; i < (ar_i32)sheet->count; ++i)
    {
        const ar_rule *r = &sheet->rules[i];

        if (r->nctx == 0)
        {
            continue;
        }
        if (r->tag && r->tag != tag)
        {
            continue;
        }
        if (r->id && r->id != id)
        {
            continue;
        }
        if (r->klass.n && !ar_classes_contains(klass, &r->klass))
        {
            continue;
        }
        if (r->state && (state & r->state) != r->state)
        {
            continue;
        }
        if (!ar__functional_matches(r, tag, klass, id, state))
        {
            continue;
        }
        if (!ar__ctx_matches(r, index, find, ud))
        {
            continue;
        }
        /* Normal declarations then important ones, in one walk: a contextual
           rule's important declarations still have to land after its normal
           ones, and there are few enough of these rules that a second walk
           would cost more than it clarifies. */
        ar_style_merge(out, &r->style, r->set & ~r->important);
        ar_style_merge(out, &r->style, r->important);
    }
}

void ar_sheet_resolve(ar_sheet *sheet, ar_u32 tag, const ar_classes *klass, ar_u32 id, ar_u16 state,
                      ar_style *out)
{
    ar_u32 slot, probe;

    if (!sheet->cache_cap)
    {
        ar__resolve_uncached(sheet, tag, klass, id, state, out);
        return;
    }

    slot = ar__cache_hash(tag, klass->combined, id, state) & (ar_u32)(sheet->cache_cap - 1u);

    /* Open addressing with a short probe. The full tuple is compared, never
       the hash alone: a collision that returned the wrong style would be a
       silent rendering bug rather than a slow frame. */
    for (probe = 0; probe < 8u; ++probe)
    {
        ar_cache_entry *e = &sheet->cache[slot];

        if (!e->used)
        {
            ar__resolve_uncached(sheet, tag, klass, id, state, out);
            e->tag = tag;
            e->klass = klass->combined;
            e->id = id;
            e->state = state;
            e->style = *out;
            e->used = 1;
            ++sheet->cache_misses;
            return;
        }
        if (e->tag == tag && e->klass == klass->combined && e->id == id && e->state == state)
        {
            *out = e->style;
            ++sheet->cache_hits;
            return;
        }
        slot = (slot + 1u) & (ar_u32)(sheet->cache_cap - 1u);
    }

    /* Eight distinct tuples landed on the same slot. Resolving without storing
       is slower than evicting something, and simpler than deciding what; an
       interface with that many colliding selectors has not been seen, and if
       one appears the counters say so. */
    ar__resolve_uncached(sheet, tag, klass, id, state, out);
    ++sheet->cache_misses;
}
