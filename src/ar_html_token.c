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
        /* Remembered rather than merely returned, because the callers recover
           from this locally and the document as a whole still has to be able
           to say it did not fit. */
        t->scratch_full = 1;
        return 0;
    }
    t->scratch[t->scratch_used++] = c;
    return 1;
}

static ar_u32 ar__utf8_len(ar_u32 cp)
{
    return cp < 0x80u ? 1u : (cp < 0x800u ? 2u : (cp < 0x10000u ? 3u : 4u));
}

/*
 * A code point as UTF-8, which is what the rest of areole reads.
 *
 * All of it or none of it. The first version wrote byte by byte and stopped
 * where it ran out, which left a partial sequence in the buffer -- a lone
 * 0xEF that is not a character in any encoding, handed on as if it were text.
 * Checking the length first costs one comparison and removes the whole class.
 */
static int ar__scratch_cp(ar_html_tok *t, ar_u32 cp)
{
    ar_u32 need = ar__utf8_len(cp);

    if (!t->scratch || t->scratch_used + need > t->scratch_cap)
    {
        t->scratch_full = 1;
        return 0;
    }

    if (need == 1u)
    {
        return ar__scratch_byte(t, (char)cp);
    }
    if (need == 2u)
    {
        return ar__scratch_byte(t, (char)(0xC0u | (cp >> 6))) &&
               ar__scratch_byte(t, (char)(0x80u | (cp & 0x3Fu)));
    }
    if (need == 3u)
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

/* ------------------------------------------------------------------------
 * Preprocessing the input stream, §13.2.3.5
 *
 * Two substitutions apply everywhere, before any state sees a character:
 * a carriage return, alone or followed by a line feed, becomes one line feed;
 * and a NUL becomes U+FFFD REPLACEMENT CHARACTER.
 *
 * The specification does this by rewriting the stream. areole cannot: the
 * bytes are the caller's and are never copied, which is the property that lets
 * the whole parser run inside an arena somebody else supplied. So it happens
 * where characters are produced instead, using the same scratch buffer a
 * character reference already needs -- and only when the range actually
 * contains one of the two, which in a document written on any system since
 * about 2005 is never.
 *
 * Getting this wrong is not subtle. A file saved on Windows has a carriage
 * return before every line feed, and without this every text node in it
 * carries invisible bytes that no comparison expects and no shaper can lay
 * out.
 * ------------------------------------------------------------------------ */
static int ar__needs_fix(const char *p, const char *end)
{
    while (p < end)
    {
        if (*p == '\r' || *p == 0)
        {
            return 1;
        }
        ++p;
    }
    return 0;
}

/* Everything plain so far has to move into scratch before anything decoded
   can follow it, and only once. Both text loops and the attribute value need
   the same three lines. */
static int ar__begin_scratch(ar_html_tok *t, const char *start, ar_u32 plain, int *used)
{
    ar_u32 k;

    if (*used)
    {
        return 1;
    }
    for (k = 0; k < plain; ++k)
    {
        if (!ar__scratch_byte(t, start[k]))
        {
            return 0;
        }
    }
    *used = 1;
    return 1;
}

static ar_span ar__clean(ar_html_tok *t, const char *p, const char *end)
{
    ar_u32 begin;

    if (!ar__needs_fix(p, end))
    {
        return ar__span(p, (ar_u32)(end - p));
    }
    begin = t->scratch_used;
    while (p < end)
    {
        if (*p == '\r')
        {
            ar__scratch_byte(t, '\n');
            ++p;
            if (p < end && *p == '\n')
            {
                ++p; /* CRLF is one line feed, not two */
            }
        }
        else if (*p == 0)
        {
            t->errors++;
            ar__scratch_cp(t, 0xFFFDu);
            ++p;
        }
        else
        {
            ar__scratch_byte(t, *p);
            ++p;
        }
    }
    return ar__span(t->scratch + begin, t->scratch_used - begin);
}

/*
 * A tag or doctype name: lowercased, and preprocessed.
 *
 * The specification lowercases ASCII upper alpha in the tag name state, and it
 * is not only for comparison -- a document object model reports `div` for
 * `<DIV>`, and anything that serialises the tree says so too. The comparison
 * folds case anyway, which is why this went unnoticed until the tree
 * construction suite printed `<DIV>` where every browser prints `<div>`.
 *
 * A name with no upper case and no NUL -- every name in every document
 * anybody has written this century -- is still a span of the input.
 */
static ar_span ar__name_clean(ar_html_tok *t, const char *p, const char *end)
{
    const char *q = p;
    ar_u32      begin;
    int         need = 0;

    while (q < end)
    {
        if ((*q >= 'A' && *q <= 'Z') || *q == 0)
        {
            need = 1;
            break;
        }
        ++q;
    }
    if (!need)
    {
        return ar__span(p, (ar_u32)(end - p));
    }

    begin = t->scratch_used;
    while (p < end)
    {
        if (*p == 0)
        {
            t->errors++;
            ar__scratch_cp(t, 0xFFFDu);
        }
        else
        {
            ar__scratch_byte(t, (char)ar__h_lower((unsigned char)*p));
        }
        ++p;
    }
    return ar__span(t->scratch + begin, t->scratch_used - begin);
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

    /*
     * Named character reference state, §13.2.5.72.
     *
     * "Consume the maximum number of characters possible, where the consumed
     * characters are one of the identifiers in the first column of the named
     * character references table." A longest match, not a lookup -- the table
     * carries both `not` and `notin;`, so where the match ends is a fact about
     * the table and cannot be guessed by reading to the first non-alphanumeric.
     */
    {
        ar_u32 cps[2];
        ar_i32 n_cp = 0;
        ar_u32 used = ar_html_entity_match(p, (ar_u32)(t->end - p), cps, &n_cp);
        ar_u32 need;
        ar_i32 k;

        if (used == 0)
        {
            /*
             * Not a reference. A bare `&` is extremely common and is not an
             * error; `&` followed by something that looks like a name is the
             * specification's unknown-named-character-reference and is
             * counted. Either way the bytes stay as the text that spells them,
             * which is what a browser does.
             */
            if (p < t->end && (ar__h_alpha(*p) || ar__h_digit(*p)))
            {
                t->errors++;
            }
            return 0;
        }

        if (p[used - 1] != ';')
        {
            /* missing-semicolon-after-character-reference. Recovered from,
               because a hundred and six of these are in the table precisely
               because pages are full of them. */
            t->errors++;

            /*
             * And in an attribute it is not a reference at all when what
             * follows is `=` or alphanumeric. That rule exists for one reason
             * and it is a good one: `?cite=1&copy=2` would otherwise put a
             * copyright sign in the middle of a query string, and a decade of
             * URLs depend on it not doing that.
             */
            if (in_attribute)
            {
                const char *after = p + used;

                if (after < t->end && (*after == '=' || ar__h_alpha(*after) || ar__h_digit(*after)))
                {
                    return 0;
                }
            }
        }

        /* Both code points or neither: a pair is one character to the reader
           -- `&NotEqualTilde;` is a relation with a slash through it -- and
           half of one is a relation that means something else. */
        need = 0;
        for (k = 0; k < n_cp; ++k)
        {
            need += ar__utf8_len(cps[k]);
        }
        if (!t->scratch || t->scratch_used + need > t->scratch_cap)
        {
            t->scratch_full = 1;
            t->p = save;
            return 0;
        }
        for (k = 0; k < n_cp; ++k)
        {
            if (!ar__scratch_cp(t, cps[k]))
            {
                t->p = save;
                return 0;
            }
        }
        t->p = p + used;
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
        /* §13.2.3.5, applied where the characters are produced. See ar__clean. */
        if (c == '\r')
        {
            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
            }
            if (!ar__scratch_byte(t, '\n'))
            {
                break;
            }
            ++t->p;
            if (t->p < t->end && *t->p == '\n')
            {
                ++t->p;
            }
            continue;
        }
        if (c == 0)
        {
            /*
             * §13.2.5.1 data state: "emit the current input character as a
             * character token". The NUL is a parse error and it is *kept*.
             *
             * The replacement character belongs to the other states -- RCDATA,
             * RAWTEXT, script data, tag names, attributes, comments, doctypes
             * -- and putting it here as well cost twenty-one conformance tests
             * that had been passing. The rule is stated per state, not per
             * stream, which is easy to miss because the preprocessing section
             * reads as though it covers everything.
             */
            t->errors++;
        }
        if (allow_refs && c == '&')
        {
            const char *before = t->p;
            ar_u32      had = t->scratch_used;

            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
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

    /*
     * A token that consumes nothing is an infinite loop.
     *
     * Every `break` above leaves on a full scratch buffer, and one of them can
     * fire before a single byte has been consumed: `&#0` with room for one
     * byte decodes to U+FFFD, which needs three, so the reference is refused,
     * the `&` cannot be stored either, and the loop leaves with `t->p` exactly
     * where it started. The token was not empty -- it held whatever the
     * reference managed to write -- so the tree builder took it and asked for
     * the next one, and got the same token, forever.
     *
     * Found by ar_fuzz at iteration 315 of seed 1, and it took a bisect to
     * find because a hang prints nothing. The rule that prevents the whole
     * family: a tokenizer that is called with input remaining consumes some
     * of it. Dropping one character is the honest cost of a buffer that is too
     * small, and `scratch_full` is what tells the caller it happened.
     */
    if (t->p == start && t->p < t->end)
    {
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

/* `<script` or `</script` followed by something that could end a tag name. */
static int ar__is_script_tag(const ar_html_tok *t, const char *p, int closing)
{
    static const char NAME[] = "script";
    const char       *q = p + (closing ? 2 : 1);
    ar_u32            i;

    if (p[0] != '<' || (closing && p[1] != '/'))
    {
        return 0;
    }
    if (q + 6 > t->end)
    {
        return 0;
    }
    for (i = 0; i < 6; ++i)
    {
        if (ar__h_lower((unsigned char)q[i]) != NAME[i])
        {
            return 0;
        }
    }
    q += 6;
    return q >= t->end || ar__h_space(*q) || *q == '>' || *q == '/';
}

static void ar__raw_text(ar_html_tok *t, ar_token *out, int allow_refs)
{
    const char *start = t->p;
    ar_u32      plain = 0;
    int         used_scratch = 0;
    int         script = t->state == AR_HTML_SCRIPT;
    int         escaped = 0;
    int         double_escaped = 0;

    ar__scratch_reset(t);

    while (t->p < t->end)
    {
        /*
         * Script data escaped and double escaped, §13.2.5.15 to §13.2.5.28.
         *
         * Fourteen states in the specification, and what they add up to is
         * this: inside a script, `<!--` opens a comment-ish region, and inside
         * *that*, `<script` opens a second one in which `</script>` is text
         * rather than the end of the element. The first `</script>` closes the
         * inner region; the next one is the real end tag.
         *
         * It exists because of one pattern from 1998 --
         * `<script><!-- document.write("<script>...</script>") --></script>`
         * -- and every browser implements it, so a parser that does not ends
         * the script at the wrong place and renders the rest of the page as
         * source code.
         *
         * Tracked with two flags rather than fourteen states because the only
         * question this loop asks is whether the end tag it just found counts.
         */
        int closes_double = 0;

        if (script)
        {
            if (!escaped && t->p + 4 <= t->end && t->p[0] == '<' && t->p[1] == '!' &&
                t->p[2] == '-' && t->p[3] == '-')
            {
                escaped = 1;
            }
            else if (escaped && !double_escaped && ar__is_script_tag(t, t->p, 0))
            {
                double_escaped = 1;
            }
            else if (escaped && double_escaped && ar__is_script_tag(t, t->p, 1))
            {
                /* This one ends the inner region and is text; the *next*
                   end tag is the real one. Without the flag the loop clears
                   double_escaped and then reads the same characters as the
                   end of the script, one position too early. */
                double_escaped = 0;
                closes_double = 1;
            }
            else if (escaped && t->p + 3 <= t->end && t->p[0] == '-' && t->p[1] == '-' &&
                     t->p[2] == '>')
            {
                escaped = 0;
                double_escaped = 0;
            }
        }
        if (*t->p == '<' && !double_escaped && !closes_double && ar__is_appropriate_end(t, t->p))
        {
            break;
        }
        if (*t->p == '\r' || *t->p == 0)
        {
            char c = *t->p;

            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
            }
            if (c == '\r')
            {
                if (!ar__scratch_byte(t, '\n'))
                {
                    break;
                }
                ++t->p;
                if (t->p < t->end && *t->p == '\n')
                {
                    ++t->p;
                }
            }
            else
            {
                t->errors++;
                if (!ar__scratch_cp(t, 0xFFFDu))
                {
                    break;
                }
                ++t->p;
            }
            continue;
        }
        if (allow_refs && *t->p == '&')
        {
            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
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

    /* The same rule as ar__text, for the same reason, in the same shape --
       `<title>&#0` reaches this copy of the loop rather than that one. */
    if (t->p == start && t->p < t->end)
    {
        ++t->p;
    }

    out->kind = AR_TOK_TEXT;
    out->text = used_scratch ? ar__span(t->scratch, t->scratch_used) : ar__span(start, plain);
}

/* ------------------------------------------------------------------------
 * Comments -- §13.2.5.43 onwards
 * ------------------------------------------------------------------------ */
/*
 * A comment, §13.2.5.43 to §13.2.5.52.
 *
 * Ten states in the specification and the reason is the last one: inside a
 * comment, `<!--` starts a bracket that `--!>` and `-->` both close, and the
 * characters that open it are *not* part of the data. `<!-- <!--` at end of
 * file is the comment ` <!` -- not ` <!--` -- because the two dashes went into
 * the comment-less-than-sign-bang-dash-dash state and never came back out.
 *
 * The abrupt closings matter more in practice. `<!-->` and `<!--->` are both
 * an empty comment, not a comment containing `>` or `->`, and they turn up in
 * hand-written markup constantly.
 */
static void ar__comment(ar_html_tok *t, ar_token *out)
{
    const char *start;
    const char *data_end;

    out->kind = AR_TOK_COMMENT;

    /* `<!--` has been consumed. Comment start state, then comment start dash
       state: a `>` in either is an abrupt-closing-of-empty-comment. */
    if (t->p < t->end && *t->p == '>')
    {
        t->errors++;
        ++t->p;
        out->text = ar__span(t->p, 0);
        return;
    }
    if (t->p + 1 < t->end && t->p[0] == '-' && t->p[1] == '>')
    {
        t->errors++;
        t->p += 2;
        out->text = ar__span(t->p, 0);
        return;
    }

    start = t->p;
    while (t->p < t->end)
    {
        if (t->p[0] == '<')
        {
            /*
             * Comment less-than sign state and the four after it. `<!--` opens
             * a bracket whose dashes belong to no comment data; anything else
             * beginning with `<` is ordinary data.
             */
            if (t->p + 3 < t->end && t->p[1] == '!' && t->p[2] == '-' && t->p[3] == '-')
            {
                data_end = t->p + 2; /* the `<!` is data; the dashes are not */
                t->p += 4;
                if (t->p < t->end && *t->p == '>')
                {
                    ++t->p;
                    out->text = ar__clean(t, start, data_end);
                    return;
                }
                if (t->p >= t->end)
                {
                    t->errors++; /* eof-in-comment */
                    out->text = ar__clean(t, start, data_end);
                    return;
                }
                continue;
            }
            ++t->p;
            continue;
        }
        if (t->p[0] == '-' && t->p + 1 < t->end && t->p[1] == '-')
        {
            /* Comment end state. `-->` closes it, and so does `--!>`. */
            const char *q = t->p + 2;

            while (q < t->end && *q == '-')
            {
                ++q;
            }
            if (q < t->end && *q == '>')
            {
                /*
                 * Of a run of dashes, exactly the last two close the comment
                 * and the rest are data: `<!----->` is a comment containing
                 * one dash. The specification gets there by looping in the
                 * comment end state, appending a dash each time round; this is
                 * the same arithmetic in one step.
                 */
                data_end = t->p + (ar_u32)(q - t->p) - 2;
                t->p = q + 1;
                out->text = ar__clean(t, start, data_end);
                return;
            }
            if (q < t->end && *q == '!' && q + 1 < t->end && q[1] == '>')
            {
                t->errors++; /* incorrectly-closed-comment */
                data_end = t->p;
                t->p = q + 2;
                out->text = ar__clean(t, start, data_end);
                return;
            }
            t->p += 2;
            continue;
        }
        ++t->p;
    }
    /*
     * eof-in-comment. What there is, minus the dashes that were on their way
     * to closing it.
     *
     * At end of file the specification is in comment end dash, comment end or
     * comment end bang state, and the characters that put it there were never
     * appended to the data: `<!-- -` is the comment ` `, not ` -`, and
     * `<!----!` is the empty comment. Emitting the raw range instead adds one
     * or three characters that no browser shows, to every unterminated comment
     * in every truncated document.
     */
    {
        const char *stop = t->end;

        if (stop - start >= 3 && stop[-1] == '!' && stop[-2] == '-' && stop[-3] == '-')
        {
            stop -= 3;
        }
        else if (stop - start >= 2 && stop[-1] == '-' && stop[-2] == '-')
        {
            stop -= 2;
        }
        else if (stop - start >= 1 && stop[-1] == '-')
        {
            stop -= 1;
        }
        t->errors++;
        out->text = ar__clean(t, start, stop);
    }
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
    out->text = ar__clean(t, start, t->p);
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
/*
 * A quoted public or system identifier.
 *
 * Three answers, because two are not enough:
 *
 *   2  read and closed by its own quote
 *   1  the quote was there and a `>` or the end of the file arrived first --
 *      abrupt-doctype-public-identifier, which forces quirks but *keeps* the
 *      identifier, because the state was entered and an empty identifier is
 *      not the same as no identifier
 *   0  there was no quote at all, so there is no identifier
 *
 * Collapsing 1 and 0 makes `<!DOCTYPE html PUBLIC '` report no public
 * identifier where every browser reports an empty one.
 */
static int ar__doctype_quoted(ar_html_tok *t, ar_span *out)
{
    char        q;
    const char *start;

    if (t->p >= t->end || (*t->p != '"' && *t->p != '\''))
    {
        return 0;
    }
    q = *t->p++;
    start = t->p;
    while (t->p < t->end && *t->p != q && *t->p != '>')
    {
        ++t->p;
    }
    /* A NUL inside an identifier is the replacement character, and a
       carriage return is a line feed: §13.2.5.65 and its neighbours say so
       for every one of the quoted states. */
    *out = ar__clean(t, start, t->p);
    if (t->p < t->end && *t->p == q)
    {
        ++t->p;
        return 2;
    }
    return 1; /* a `>`, or the end of the file */
}

static void ar__doctype(ar_html_tok *t, ar_token *out)
{
    int bogus = 0;

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
        out->name = ar__name_clean(t, start, t->p);
        if (out->name.n == 0)
        {
            t->errors++;
            out->force_quirks = 1;
        }
    }

    /*
     * PUBLIC and SYSTEM, §13.2.5.56 to §13.2.5.71.
     *
     * Three rules here are easy to get backwards, and each of them decides the
     * box model for a whole document:
     *
     *   - Anything after the name that is not the PUBLIC or SYSTEM keyword --
     *     including a quote -- is invalid-character-sequence-after-doctype-name.
     *     Force quirks, then ignore everything to the `>`. It does *not* become
     *     an identifier: `<!DOCTYPE a "` has no system identifier.
     *   - A `>` inside a quoted identifier ends the doctype and forces quirks.
     *   - Junk *after* a complete identifier is a parse error and forces
     *     nothing. `<!DOCTYPE a SYSTEM''a` is a perfectly good doctype with a
     *     stray letter after it, and a document that renders in standards mode.
     */
    while (t->p < t->end && *t->p != '>')
    {
        const char *kw;
        ar_u32      n = 0;
        int         ok = 0;
        int         junk = 0;
        int         got_system = 0;

        while (t->p < t->end && ar__h_space(*t->p))
        {
            ++t->p;
        }
        if (t->p >= t->end || *t->p == '>')
        {
            break;
        }

        kw = t->p;
        while (t->p < t->end && ar__h_alpha(*t->p))
        {
            ++t->p;
            ++n;
        }
        while (t->p < t->end && ar__h_space(*t->p))
        {
            ++t->p;
        }

        if (n && ar_span_is(ar__span(kw, n), "public"))
        {
            ok = ar__doctype_quoted(t, &out->pub);
            if (ok == 2)
            {
                while (t->p < t->end && ar__h_space(*t->p))
                {
                    ++t->p;
                }
                if (t->p < t->end && (*t->p == '"' || *t->p == '\''))
                {
                    ok = ar__doctype_quoted(t, &out->sys);
                    got_system = ok == 2;
                }
            }
        }
        else if (n && ar_span_is(ar__span(kw, n), "system"))
        {
            ok = ar__doctype_quoted(t, &out->sys);
            got_system = ok == 2;
        }

        if (ok != 2)
        {
            t->errors++;
            out->force_quirks = 1;
        }
        if (ok == 0)
        {
            /* The keyword itself was wrong, or nothing followed it. Bogus
               DOCTYPE state, and there are no identifiers at all -- which is
               why `<!DOCTYPE a "` has none and `<!DOCTYPE a PUBLIC "` has an
               empty one. */
            out->pub = ar__span(0, 0);
            out->sys = ar__span(0, 0);
        }
        /*
         * Either way, nothing else in this doctype is read.
         *
         * Trailing junk after a good identifier is a parse error and forces
         * nothing -- and it also changes what the *end of the file* means.
         * The after-DOCTYPE-system-identifier state forces quirks at EOF; the
         * bogus DOCTYPE state, which is where the junk puts us, does not. So
         * `<!DOCTYPE a SYSTEM''` is quirks and `<!DOCTYPE a SYSTEM''!` is
         * not, on the strength of one exclamation mark.
         */
        while (t->p < t->end && *t->p != '>')
        {
            if (!ar__h_space(*t->p))
            {
                junk = 1;
            }
            ++t->p;
        }
        if (junk)
        {
            t->errors++;
            if (got_system)
            {
                /*
                 * unexpected-character-after-doctype-system-identifier, and it
                 * forces nothing: `<!DOCTYPE a SYSTEM''!` renders in standards
                 * mode. Reaching the bogus DOCTYPE state also changes what end
                 * of file means, because that state does not force quirks
                 * there either.
                 */
                bogus = 1;
            }
            else
            {
                /*
                 * The other side of the same coin.
                 * missing-quote-before-doctype-system-identifier: anything but
                 * a quote after a *public* identifier does force quirks,
                 * because what should have followed was the system identifier
                 * and it is missing. One character apart in the input, opposite
                 * answers, and the answer is the box model for the document.
                 */
                out->force_quirks = 1;
            }
        }
        break;
    }

    if (t->p < t->end)
    {
        ++t->p; /* the `>` */
    }
    else
    {
        /* eof-in-doctype forces quirks, which is the specification's answer
           and matters: a truncated doctype must not look like a good one.
           Unless the bogus DOCTYPE state got there first -- see above. */
        t->errors++;
        if (!bogus)
        {
            out->force_quirks = 1;
        }
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
        /* An attribute value is one of the states that replaces a NUL, and
           one of the states a carriage return is normalised in. */
        if (c == '\r' || c == 0)
        {
            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
            }
            if (c == '\r')
            {
                if (!ar__scratch_byte(t, '\n'))
                {
                    break;
                }
                ++t->p;
                if (t->p < t->end && *t->p == '\n')
                {
                    ++t->p;
                }
            }
            else
            {
                t->errors++;
                if (!ar__scratch_cp(t, 0xFFFDu))
                {
                    break;
                }
                ++t->p;
            }
            continue;
        }
        if (c == '&')
        {
            if (!ar__begin_scratch(t, start, plain, &used_scratch))
            {
                break;
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

/*
 * A tag, §13.2.5.6 onwards.
 *
 * Returns 1 if a tag was produced and 0 if the input ended inside it.
 *
 * The return value is the whole point. Every state between `tag open` and
 * `after attribute value (quoted)` says the same thing about end of file: "this
 * is an eof-in-tag parse error, emit an end-of-file token" -- *emit an
 * end-of-file token*, not emit the tag. A tag that was never finished is
 * dropped whole, attributes and all.
 *
 * This version finished the tag and emitted it, which is a reasonable-looking
 * thing to do and is wrong in a way that changes trees. `<table><em><p>x</em`
 * ends in an unterminated end tag; taking it seriously runs the adoption
 * agency, which relocates the paragraph out of the emphasis and clones the
 * emphasis inside it. Dropping it, as every browser does, leaves the tree
 * alone. The browser corpus disagreed on exactly that one case and on no
 * other, which is what pointed here rather than at the agency.
 */
static int ar__tag(ar_html_tok *t, ar_token *out, int is_end)
{
    const char *start;
    ar_u32      n = 0;

    out->kind = is_end ? AR_TOK_END : AR_TOK_START;
    ar__scratch_reset(t);

    /* Tag name state, §13.2.5.10. Lowercased -- see ar__name_clean, which
       leaves a name that is already lower case as a span of the input. */
    start = t->p;
    while (t->p < t->end && !ar__h_space(*t->p) && *t->p != '>' && *t->p != '/')
    {
        ++t->p;
        ++n;
    }
    out->name = ar__name_clean(t, start, start + n);

    /* Before attribute name state, and everything after it. */
    for (;;)
    {
        while (t->p < t->end && ar__h_space(*t->p))
        {
            ++t->p;
        }
        if (t->p >= t->end)
        {
            t->errors++; /* eof-in-tag: the tag is dropped, not emitted */
            return 0;
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

        /* Attribute name state, §13.2.5.32 onwards. */
        {
            const char *an = t->p;
            ar_u32      alen = 0;
            ar_span     value;
            ar_span     aname;

            value = ar__span(t->p, 0);

            /*
             * `=` before a name starts an attribute *called* `=`.
             *
             * unexpected-equals-sign-before-attribute-name: the specification
             * says to begin a new attribute, set its name to the equals sign,
             * and carry on. `<z =>` is an element with one attribute whose
             * name is `=` and whose value is empty, which looks absurd and is
             * what every browser does.
             */
            if (*t->p == '=')
            {
                t->errors++;
                ++t->p;
                ++alen;
            }
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
                ar_i32 k;
                int    duplicate = 0;

                aname = ar__name_clean(t, an, an + alen);

                /*
                 * duplicate-attribute: the first one wins and the rest are
                 * dropped. Not a curiosity -- `<x x=1 x=2 X=3>` is one
                 * attribute, and a parser that keeps all three hands the
                 * cascade three declarations where the author wrote one.
                 * Compared after lowercasing, which is why `X=3` counts.
                 */
                for (k = 0; k < out->attr_count; ++k)
                {
                    if (out->attrs[k].name.n == aname.n &&
                        (aname.n == 0 || memcmp(out->attrs[k].name.p, aname.p, aname.n) == 0))
                    {
                        duplicate = 1;
                        t->errors++;
                        break;
                    }
                }
                if (duplicate)
                {
                    /* nothing: the first is kept */
                }
                else if (out->attr_count < AR_HTML_MAX_ATTRS)
                {
                    out->attrs[out->attr_count].name = aname;
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

    /*
     * An end tag out of RCDATA, RAWTEXT or script data puts the tokenizer back
     * in the data state.
     *
     * The tree builder does this too, and did it alone until the conformance
     * suite ran: only the *appropriate* end tag reaches here, so `</xmp>`
     * inside `<xmp>` ends the raw text and everything after it is ordinary
     * markup. Leaving it to the tree builder means a caller driving the
     * tokenizer by itself never leaves RAWTEXT, and the comment after the
     * closing tag comes out as text.
     */
    if (is_end && t->state != AR_HTML_DATA && t->state != AR_HTML_PLAINTEXT)
    {
        t->state = AR_HTML_DATA;
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
    return 1;
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
        /* Even here: PLAINTEXT replaces a NUL with U+FFFD, and a document
           from 1994 is exactly the kind that has one. */
        out->text = ar__clean(t, t->p, t->end);
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
            if (!ar__tag(t, out, 0))
            {
                out->kind = AR_TOK_EOF;
                return 0;
            }
            return 1;
        }
        if (next < t->end && *next == '/')
        {
            if (next + 1 < t->end && ar__h_alpha(next[1]))
            {
                t->p = next + 1;
                if (!ar__tag(t, out, 1))
                {
                    out->kind = AR_TOK_EOF;
                    return 0;
                }
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
            if (next + 1 >= t->end)
            {
                /* eof-before-tag-name: `</` at the end of the file is the two
                   characters, not the start of anything. */
                t->errors++;
                out->kind = AR_TOK_TEXT;
                out->text = ar__span(t->p, 2);
                t->p = t->end;
                return 1;
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
        /*
         * invalid-first-character-of-tag-name, §13.2.5.6: a `<` that begins no
         * tag is emitted as a character and the next one is reconsidered.
         *
         * It has to be its own token rather than the start of the text run
         * below, because that run stops at every `<` -- which is what made
         * `foo < bar` come out as `foo  bar`, with the character silently
         * gone. Adjacent character tokens are one text node to the tree
         * builder, so nothing downstream sees the split.
         */
        out->kind = AR_TOK_TEXT;
        out->text = ar__span(t->p, 1);
        ++t->p;
        if (next < t->end)
        {
            t->errors++;
        }
        return 1;
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
