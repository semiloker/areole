/*
 * areole - the HTML tokenizer.
 * SPDX-License-Identifier: MIT
 *
 * The state names in this file are the specification's own, spelled the same
 * way, so a reader with §13.2.5 open can find the paragraph. Where areole
 * departs from it, the departure is at the point it happens rather than in a
 * list somewhere else.
 *
 * ------------------------------------------------------------------------
 * Why it is written as functions rather than as a state machine
 *
 * The specification is eighty states and reads as a `switch` in a loop, and
 * that is how most tokenizers are written -- resumable, one character at a
 * time, the state in a variable. That shape exists to serve *streaming*: a
 * network parser gets bytes when the network feels like it and has to stop
 * mid-tag.
 *
 * areole has no network and never will. The whole document is one span of
 * bytes the caller already owns, which means a tag can be read in one call
 * from `<` to `>` without ever being suspended. So the states become ordinary
 * functions with ordinary local variables, and the resumption bookkeeping --
 * which is most of the difficulty in the streaming shape, and most of the bugs
 * -- does not exist.
 *
 * What is kept from the specification exactly: which characters are consumed
 * in which state, what each error recovers to, and the names.
 *
 * ------------------------------------------------------------------------
 * Errors are counted and never fatal
 *
 * Every parse error in the specification has a defined recovery, because two
 * decades of real markup is malformed and browsers had to agree on what to do
 * about it. A tokenizer that stops is a tokenizer that disagrees with every
 * browser on documents that render fine everywhere else.
 */
#include "ar_html.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Characters
 * ------------------------------------------------------------------------ */
static int ar__h_space(int c)
{
    /* The specification's whitespace for tokenizing: tab, LF, FF, CR, space.
       Vertical tab is not in it, which surprises people. */
    return c == '\t' || c == '\n' || c == '\f' || c == '\r' || c == ' ';
}

static int ar__h_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int ar__h_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int ar__h_hex(int c)
{
    return ar__h_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static int ar__h_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int ar_span_is(ar_span s, const char *lit)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        if (lit[i] == 0 || ar__h_lower((unsigned char)s.p[i]) != ar__h_lower((unsigned char)lit[i]))
        {
            return 0;
        }
    }
    return lit[s.n] == 0;
}

static ar_span ar__span(const char *p, ar_u32 n)
{
    ar_span s;

    s.p = p;
    s.n = n;
    return s;
}

/* ------------------------------------------------------------------------
 * Scratch
 *
 * Only a value holding a character reference needs it: `&amp;` is five bytes
 * in and one out, so the result is not a span of the input any more. A
 * document with no entities never touches this.
 * ------------------------------------------------------------------------ */
static void ar__scratch_reset(ar_html_tok *t)
{
    t->scratch_used = 0;
}

static int ar__scratch_byte(ar_html_tok *t, char c)
{
    if (!t->scratch || t->scratch_used >= t->scratch_cap)
    {
        return 0;
    }
    t->scratch[t->scratch_used++] = c;
    return 1;
}

/* A code point as UTF-8, which is what the rest of areole reads. */
static int ar__scratch_cp(ar_html_tok *t, ar_u32 cp)
{
    if (cp < 0x80u)
    {
        return ar__scratch_byte(t, (char)cp);
    }
    if (cp < 0x800u)
    {
        return ar__scratch_byte(t, (char)(0xC0u | (cp >> 6))) &&
               ar__scratch_byte(t, (char)(0x80u | (cp & 0x3Fu)));
    }
    if (cp < 0x10000u)
    {
        return ar__scratch_byte(t, (char)(0xE0u | (cp >> 12))) &&
               ar__scratch_byte(t, (char)(0x80u | ((cp >> 6) & 0x3Fu))) &&
               ar__scratch_byte(t, (char)(0x80u | (cp & 0x3Fu)));
    }
    return ar__scratch_byte(t, (char)(0xF0u | (cp >> 18))) &&
           ar__scratch_byte(t, (char)(0x80u | ((cp >> 12) & 0x3Fu))) &&
           ar__scratch_byte(t, (char)(0x80u | ((cp >> 6) & 0x3Fu))) &&
           ar__scratch_byte(t, (char)(0x80u | (cp & 0x3Fu)));
}

/* ------------------------------------------------------------------------
 * Character references -- §13.2.5.72 onwards
 * ------------------------------------------------------------------------ */

/*
 * The numeric reference replacement table, which everybody forgets.
 *
 * `&#128;` is not U+0080. The specification says the C1 range decodes as if it
 * were Windows-1252, because a decade of documents declared themselves UTF-8
 * and pasted in curly quotes from Word by number. Getting this wrong turns
 * every smart quote on such a page into a control character.
 */
static ar_u32 ar__win1252(ar_u32 cp)
{
    static const ar_u32 MAP[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                   0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                   0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                   0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

    if (cp >= 0x80u && cp <= 0x9Fu)
    {
        return MAP[cp - 0x80u];
    }
    return cp;
}

/*
 * One character reference, starting at the `&`.
 *
 * Writes the decoded bytes into scratch and advances past the reference.
 * Returns 0 if this is not a reference after all, having consumed nothing --
 * a bare `&` in text is not an error and is extremely common.
 */
static int ar__reference(ar_html_tok *t, int in_attribute)
{
    const char *save = t->p;
    const char *p = t->p;

    if (p >= t->end || *p != '&')
    {
        return 0;
    }
    ++p;

    /* Numeric character reference state. */
    if (p < t->end && *p == '#')
    {
        ar_u32 cp = 0;
        int    any = 0;
        int    hex = 0;

        ++p;
        if (p < t->end && (*p == 'x' || *p == 'X'))
        {
            hex = 1;
            ++p;
        }
        while (p < t->end && (hex ? ar__h_hex(*p) : ar__h_digit(*p)))
        {
            int d = ar__h_digit(*p) ? *p - '0' : (ar__h_lower(*p) - 'a' + 10);

            /* Clamped rather than wrapped. A reference of nine hundred digits
               is malformed input, and the specification's answer to an
               out-of-range one is the replacement character either way. */
            if (cp < 0x110000u)
            {
                cp = cp * (ar_u32)(hex ? 16 : 10) + (ar_u32)d;
            }
            any = 1;
            ++p;
        }
        if (!any)
        {
            t->errors++;
            t->p = save;
            return 0;
        }
        if (p < t->end && *p == ';')
        {
            ++p;
        }
        else
        {
            /* missing-semicolon-after-character-reference, recovered from. */
            t->errors++;
        }

        if (cp == 0u || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
        {
            t->errors++;
            cp = 0xFFFDu;
        }
        cp = ar__win1252(cp);
        if (!ar__scratch_cp(t, cp))
        {
            t->p = save;
            return 0;
        }
        t->p = p;
        return 1;
    }

    /* Named character reference state. */
    {
        const char *name = p;
        ar_u32      n = 0;
        ar_u32      cp;

        while (p < t->end && (ar__h_alpha(*p) || ar__h_digit(*p)))
        {
            ++p;
            ++n;
        }
        if (n == 0)
        {
            return 0; /* a bare `&`, which is not an error */
        }
        if (p < t->end && *p == ';')
        {
            ++p;
            ++n; /* the semicolon is part of the name in the table */
        }
        else if (in_attribute)
        {
            /*
             * In an attribute, a reference without a semicolon followed by `=`
             * or an alphanumeric is *not* a reference. That rule exists for
             * one reason and it is a good one: `?cite=1&copy=2` would
             * otherwise put a copyright sign in the middle of a query string,
             * and a decade of URLs depend on it not doing that.
             */
            if (p < t->end && (*p == '=' || ar__h_alpha(*p) || ar__h_digit(*p)))
            {
                return 0;
            }
        }

        cp = ar_html_entity(name, n);
        if (cp == 0)
        {
            /* Not a reference this table knows. Left as the literal text that
               spells it, which is what a browser does for an unrecognised one
               -- and what this one also does for the 1,978 the table does not
               have yet. ar_html_entity.c names that gap. */
            t->errors++;
            return 0;
        }
        if (!ar__scratch_cp(t, cp))
        {
            t->p = save;
            return 0;
        }
        t->p = p;
        return 1;
    }
}

/* ------------------------------------------------------------------------
 * Text
 * ------------------------------------------------------------------------ */

/*
 * A run of character tokens, coalesced into one.
 *
 * The run ends at a `<` that could begin a tag, at a `&` that turns out to be
 * a reference, or at the input. A `&` that is not a reference does not end it,
 * which is why the loop looks ahead rather than splitting on every ampersand:
 * `AT&T` is one run.
 */
static void ar__text(ar_html_tok *t, ar_token *out, int allow_tags, int allow_refs)
{
    const char *start = t->p;
    ar_u32      plain = 0;
    int         used_scratch = 0;

    ar__scratch_reset(t);

    while (t->p < t->end)
    {
        char c = *t->p;

        if (allow_tags && c == '<')
        {
            break;
        }
        if (allow_refs && c == '&')
        {
            const char *before = t->p;
            ar_u32      had = t->scratch_used;

            /* Everything plain so far has to move into scratch before the
               decoded bytes can follow it, and only once. */
            if (!used_scratch)
            {
                ar_u32 k;

                for (k = 0; k < plain; ++k)
                {
                    if (!ar__scratch_byte(t, start[k]))
                    {
                        break;
                    }
                }
                used_scratch = 1;
            }
            if (ar__reference(t, 0))
            {
                continue;
            }
            /* Not a reference. Put the `&` in as itself and carry on. */
            (void)had;
            t->p = before;
            if (!ar__scratch_byte(t, '&'))
            {
                break;
            }
            ++t->p;
            continue;
        }
        if (used_scratch)
        {
            if (!ar__scratch_byte(t, c))
            {
                break;
            }
        }
        else
        {
            ++plain;
        }
        ++t->p;
    }

    out->kind = AR_TOK_TEXT;
    out->text = used_scratch ? ar__span(t->scratch, t->scratch_used) : ar__span(start, plain);
}

/*
 * RCDATA, RAWTEXT and script data all end the same way: at the *appropriate*
 * end tag, which is the one matching the tag that opened them. `</b>` inside
 * `<title>` is text, and only `</title>` closes it. That rule is why the
 * tokenizer keeps `last_start`.
 */
static int ar__is_appropriate_end(ar_html_tok *t, const char *p)
{
    ar_u32 i;

    if (p + 2 + t->last_start_n > t->end || t->last_start_n == 0)
    {
        return 0;
    }
    if (p[0] != '<' || p[1] != '/')
    {
        return 0;
    }
    for (i = 0; i < t->last_start_n; ++i)
    {
        if (ar__h_lower((unsigned char)p[2 + i]) != t->last_start[i])
        {
            return 0;
        }
    }
    {
        const char *after = p + 2 + t->last_start_n;

        if (after >= t->end)
        {
            return 0;
        }
        return ar__h_space(*after) || *after == '/' || *after == '>';
    }
}

static void ar__raw_text(ar_html_tok *t, ar_token *out, int allow_refs)
{
    const char *start = t->p;
    ar_u32      plain = 0;
    int         used_scratch = 0;

    ar__scratch_reset(t);

    while (t->p < t->end)
    {
        if (*t->p == '<' && ar__is_appropriate_end(t, t->p))
        {
            break;
        }
        if (allow_refs && *t->p == '&')
        {
            if (!used_scratch)
            {
                ar_u32 k;

                for (k = 0; k < plain; ++k)
                {
                    if (!ar__scratch_byte(t, start[k]))
                    {
                        break;
                    }
                }
                used_scratch = 1;
            }
            if (ar__reference(t, 0))
            {
                continue;
            }
            if (!ar__scratch_byte(t, '&'))
            {
                break;
            }
            ++t->p;
            continue;
        }
        if (used_scratch)
        {
            if (!ar__scratch_byte(t, *t->p))
            {
                break;
            }
        }
        else
        {
            ++plain;
        }
        ++t->p;
    }

    out->kind = AR_TOK_TEXT;
    out->text = used_scratch ? ar__span(t->scratch, t->scratch_used) : ar__span(start, plain);
}

/* ------------------------------------------------------------------------
 * Comments -- §13.2.5.43 onwards
 * ------------------------------------------------------------------------ */
static void ar__comment(ar_html_tok *t, ar_token *out)
{
    const char *start;

    /* `<!--` has been consumed. */
    start = t->p;
    while (t->p < t->end)
    {
        if (t->p + 2 < t->end && t->p[0] == '-' && t->p[1] == '-' && t->p[2] == '>')
        {
            out->kind = AR_TOK_COMMENT;
            out->text = ar__span(start, (ar_u32)(t->p - start));
            t->p += 3;
            return;
        }
        ++t->p;
    }
    /* eof-in-comment: emit what there is, which is what a browser shows. */
    t->errors++;
    out->kind = AR_TOK_COMMENT;
    out->text = ar__span(start, (ar_u32)(t->end - start));
}

/*
 * Bogus comment state. Everything to the next `>` is the comment's data.
 *
 * This is where `<?php`, `</>` and `<!nonsense` all end up, and it is why a
 * stray processing instruction does not destroy the rest of a document.
 */
static void ar__bogus_comment(ar_html_tok *t, ar_token *out)
{
    const char *start = t->p;

    while (t->p < t->end && *t->p != '>')
    {
        ++t->p;
    }
    out->kind = AR_TOK_COMMENT;
    out->text = ar__span(start, (ar_u32)(t->p - start));
    if (t->p < t->end)
    {
        ++t->p;
    }
}

/* ------------------------------------------------------------------------
 * DOCTYPE -- §13.2.5.53 onwards
 *
 * The name and the two identifiers are all this needs to produce; which of
 * them force quirks is the tree builder's table and not the tokenizer's.
 * What is decided here is the specification's own force-quirks flag: a
 * doctype that is malformed, or ends before its `>`, sets it.
 * ------------------------------------------------------------------------ */
static void ar__doctype_quoted(ar_html_tok *t, ar_span *out)
{
    char        q;
    const char *start;

    if (t->p >= t->end || (*t->p != '"' && *t->p != '\''))
    {
        return;
    }
    q = *t->p++;
    start = t->p;
    while (t->p < t->end && *t->p != q && *t->p != '>')
    {
        ++t->p;
    }
    *out = ar__span(start, (ar_u32)(t->p - start));
    if (t->p < t->end && *t->p == q)
    {
        ++t->p;
    }
}

static void ar__doctype(ar_html_tok *t, ar_token *out)
{
    out->kind = AR_TOK_DOCTYPE;

    while (t->p < t->end && ar__h_space(*t->p))
    {
        ++t->p;
    }

    /* Name. */
    {
        const char *start = t->p;

        while (t->p < t->end && !ar__h_space(*t->p) && *t->p != '>')
        {
            ++t->p;
        }
        out->name = ar__span(start, (ar_u32)(t->p - start));
        if (out->name.n == 0)
        {
            t->errors++;
            out->force_quirks = 1;
        }
    }

    /* PUBLIC / SYSTEM, in whichever order and however malformed. */
    while (t->p < t->end && *t->p != '>')
    {
        while (t->p < t->end && ar__h_space(*t->p))
        {
            ++t->p;
        }
        if (t->p >= t->end || *t->p == '>')
        {
            break;
        }
        if (*t->p == '"' || *t->p == '\'')
        {
            /* An identifier with no keyword in front of it. Malformed, and the
               specification says take it as the system identifier. */
            ar__doctype_quoted(t, &out->sys);
            continue;
        }
        {
            const char *kw = t->p;
            ar_u32      n = 0;

            while (t->p < t->end && ar__h_alpha(*t->p))
            {
                ++t->p;
                ++n;
            }
            if (n == 0)
            {
                t->errors++;
                out->force_quirks = 1;
                ++t->p;
                continue;
            }
            while (t->p < t->end && ar__h_space(*t->p))
            {
                ++t->p;
            }
            if (ar_span_is(ar__span(kw, n), "public"))
            {
                ar__doctype_quoted(t, &out->pub);
                while (t->p < t->end && ar__h_space(*t->p))
                {
                    ++t->p;
                }
                if (t->p < t->end && (*t->p == '"' || *t->p == '\''))
                {
                    ar__doctype_quoted(t, &out->sys);
                }
            }
            else if (ar_span_is(ar__span(kw, n), "system"))
            {
                ar__doctype_quoted(t, &out->sys);
            }
            else
            {
                t->errors++;
                out->force_quirks = 1;
            }
        }
    }

    if (t->p < t->end)
    {
        ++t->p; /* the `>` */
    }
    else
    {
        /* eof-in-doctype forces quirks, which is the specification's answer
           and matters: a truncated doctype must not look like a good one. */
        t->errors++;
        out->force_quirks = 1;
    }
}

/* ------------------------------------------------------------------------
 * Tags -- §13.2.5.6 onwards
 * ------------------------------------------------------------------------ */

/*
 * One attribute value, which is the only place a character reference can
 * appear inside a tag.
 */
static ar_span ar__attr_value(ar_html_tok *t)
{
    const char *start;
    ar_u32      plain = 0;
    int         used_scratch = 0;
    char        quote = 0;
    ar_u32      scratch_begin = t->scratch_used;

    if (t->p < t->end && (*t->p == '"' || *t->p == '\''))
    {
        quote = *t->p++;
    }
    start = t->p;

    while (t->p < t->end)
    {
        char c = *t->p;

        if (quote ? (c == quote) : (ar__h_space(c) || c == '>'))
        {
            break;
        }
        if (c == '&')
        {
            if (!used_scratch)
            {
                ar_u32 k;

                for (k = 0; k < plain; ++k)
                {
                    if (!ar__scratch_byte(t, start[k]))
                    {
                        break;
                    }
                }
                used_scratch = 1;
            }
            if (ar__reference(t, 1))
            {
                continue;
            }
            if (!ar__scratch_byte(t, '&'))
            {
                break;
            }
            ++t->p;
            continue;
        }
        if (used_scratch)
        {
            if (!ar__scratch_byte(t, c))
            {
                break;
            }
        }
        else
        {
            ++plain;
        }
        ++t->p;
    }

    if (quote && t->p < t->end && *t->p == quote)
    {
        ++t->p;
    }
    if (used_scratch)
    {
        return ar__span(t->scratch + scratch_begin, t->scratch_used - scratch_begin);
    }
    return ar__span(start, plain);
}

static void ar__tag(ar_html_tok *t, ar_token *out, int is_end)
{
    const char *start;
    ar_u32      n = 0;

    out->kind = is_end ? AR_TOK_END : AR_TOK_START;
    ar__scratch_reset(t);

    /* Tag name state. Lowercased in place is impossible -- the input is the
       caller's -- so the span points at the original and every comparison
       goes through ar_span_is, which folds case. */
    start = t->p;
    while (t->p < t->end && !ar__h_space(*t->p) && *t->p != '>' && *t->p != '/')
    {
        ++t->p;
        ++n;
    }
    out->name = ar__span(start, n);

    /* Before attribute name state, and everything after it. */
    for (;;)
    {
        while (t->p < t->end && ar__h_space(*t->p))
        {
            ++t->p;
        }
        if (t->p >= t->end)
        {
            t->errors++; /* eof-in-tag */
            break;
        }
        if (*t->p == '>')
        {
            ++t->p;
            break;
        }
        if (*t->p == '/')
        {
            ++t->p;
            if (t->p < t->end && *t->p == '>')
            {
                out->self_closing = 1;
                ++t->p;
                break;
            }
            /* unexpected-solidus-in-tag: treated as whitespace, which is what
               `<br/ >` and a hundred thousand hand-written pages rely on. */
            t->errors++;
            continue;
        }

        /* Attribute name state. */
        {
            const char *an = t->p;
            ar_u32      alen = 0;
            ar_span     value;

            value = ar__span(t->p, 0);
            while (t->p < t->end && !ar__h_space(*t->p) && *t->p != '=' && *t->p != '>' &&
                   *t->p != '/')
            {
                ++t->p;
                ++alen;
            }
            while (t->p < t->end && ar__h_space(*t->p))
            {
                ++t->p;
            }
            if (t->p < t->end && *t->p == '=')
            {
                ++t->p;
                while (t->p < t->end && ar__h_space(*t->p))
                {
                    ++t->p;
                }
                value = ar__attr_value(t);
            }
            else
            {
                value = ar__span(an, 0); /* an empty value, not a missing one */
            }

            if (alen > 0)
            {
                if (out->attr_count < AR_HTML_MAX_ATTRS)
                {
                    out->attrs[out->attr_count].name = ar__span(an, alen);
                    out->attrs[out->attr_count].value = value;
                    ++out->attr_count;
                }
                else
                {
                    ++out->attrs_dropped;
                }
            }
        }
    }

    /* An end tag with attributes is an error and its attributes are dropped,
       which the tree builder relies on: `</p class=x>` must not carry one. */
    if (is_end && out->attr_count > 0)
    {
        t->errors++;
        out->attr_count = 0;
    }

    if (!is_end)
    {
        ar_u32 i;

        t->last_start_n =
            out->name.n < (ar_u32)sizeof t->last_start ? out->name.n : (ar_u32)sizeof t->last_start;
        for (i = 0; i < t->last_start_n; ++i)
        {
            t->last_start[i] = (char)ar__h_lower((unsigned char)out->name.p[i]);
        }
    }
}

/* ------------------------------------------------------------------------
 * The entry point
 * ------------------------------------------------------------------------ */
void ar_html_tok_init(ar_html_tok *t, const char *bytes, ar_u32 len, char *scratch,
                      ar_u32 scratch_cap)
{
    memset(t, 0, sizeof *t);
    t->p = bytes;
    t->end = bytes ? bytes + len : 0;
    t->state = AR_HTML_DATA;
    t->scratch = scratch;
    t->scratch_cap = scratch_cap;
}

int ar_html_next(ar_html_tok *t, ar_token *out)
{
    memset(out, 0, sizeof *out);

    if (!t->p || t->p >= t->end)
    {
        out->kind = AR_TOK_EOF;
        return 0;
    }

    /* PLAINTEXT never leaves itself: everything to the end of the file is
       text, tags included. It exists for documents from 1994 and is two lines
       rather than a special case elsewhere. */
    if (t->state == AR_HTML_PLAINTEXT)
    {
        out->kind = AR_TOK_TEXT;
        out->text = ar__span(t->p, (ar_u32)(t->end - t->p));
        t->p = t->end;
        return 1;
    }

    if (t->state == AR_HTML_RCDATA || t->state == AR_HTML_RAWTEXT || t->state == AR_HTML_SCRIPT)
    {
        if (!ar__is_appropriate_end(t, t->p))
        {
            ar__raw_text(t, out, t->state == AR_HTML_RCDATA);
            if (out->text.n > 0)
            {
                return 1;
            }
        }
        /* At the appropriate end tag: fall through to the data path, which
           reads it as an ordinary end tag. The tree builder puts the state
           back to data when it sees it. */
    }

    if (*t->p == '<')
    {
        const char *next = t->p + 1;

        if (next < t->end && ar__h_alpha(*next))
        {
            t->p = next;
            ar__tag(t, out, 0);
            return 1;
        }
        if (next < t->end && *next == '/')
        {
            if (next + 1 < t->end && ar__h_alpha(next[1]))
            {
                t->p = next + 1;
                ar__tag(t, out, 1);
                return 1;
            }
            /* `</>` is missing-end-tag-name: the specification drops it
               entirely rather than making a comment of it. */
            if (next + 1 < t->end && next[1] == '>')
            {
                t->errors++;
                t->p = next + 2;
                return ar_html_next(t, out);
            }
            t->errors++;
            t->p = next + 1;
            ar__bogus_comment(t, out);
            return 1;
        }
        if (next < t->end && *next == '!')
        {
            /* Markup declaration open state. */
            if (next + 2 < t->end && next[1] == '-' && next[2] == '-')
            {
                t->p = next + 3;
                ar__comment(t, out);
                return 1;
            }
            if (next + 7 < t->end && ar_span_is(ar__span(next + 1, 7), "doctype"))
            {
                t->p = next + 8;
                ar__doctype(t, out);
                return 1;
            }
            /*
             * `<![CDATA[` is only a CDATA section inside foreign content, and
             * there is no foreign content until SVG arrives in 0.13.0. Outside
             * it the specification says bogus comment, which is what this is.
             */
            t->errors++;
            t->p = next + 1;
            ar__bogus_comment(t, out);
            return 1;
        }
        if (next < t->end && *next == '?')
        {
            /* unexpected-question-mark-instead-of-tag-name. `<?php` in a file
               served as HTML lands here, and becomes a comment rather than
               eating the document. */
            t->errors++;
            t->p = next;
            ar__bogus_comment(t, out);
            return 1;
        }
        /* A `<` that begins nothing is just text, and the run below takes it. */
    }

    ar__text(t, out, 1, 1);
    if (out->text.n == 0 && t->p < t->end)
    {
        /* A lone `<` at the end, or one the text run refused to start on.
           Consume it as text so the loop cannot stall. */
        out->kind = AR_TOK_TEXT;
        out->text = ar__span(t->p, 1);
        ++t->p;
    }
    return 1;
}
