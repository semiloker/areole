/*
 * areole - encoding sniffing and decoding.
 * SPDX-License-Identifier: MIT
 *
 * §13.2.3.2, the encoding sniffing algorithm, and decoders for the four
 * encodings a document actually arrives in.
 *
 * ------------------------------------------------------------------------
 * Why a parser needs this at all
 *
 * The tokenizer reads UTF-8 and the rest of areole draws UTF-8. A document on
 * disk is whatever somebody saved it as, and a decade of them are Windows-1252
 * with no declaration at all. Reading one of those as UTF-8 does not fail --
 * every byte is valid on its own -- it just renders every accented letter as a
 * replacement character, silently, and looks like a font problem.
 *
 * ------------------------------------------------------------------------
 * The order, which is the whole algorithm
 *
 *   1. A byte order mark, if there is one. **It beats everything**, including
 *      a `<meta charset>` that disagrees with it, and including an encoding
 *      the transport layer declared.
 *   2. `<meta charset>` or `<meta http-equiv content-type>` within the first
 *      1024 bytes.
 *   3. A default. The specification says this is locale-dependent and
 *      recommends windows-1252 for most of them, which is what this uses.
 *
 * areole has no transport layer, so the step where an HTTP header would be
 * consulted is absent by construction rather than by omission.
 *
 * ------------------------------------------------------------------------
 * What is not here
 *
 * The specification's prescan is a small tokenizer of its own that skips
 * comments and understands attribute syntax properly. This one searches for
 * `<meta` and reads the attributes after it, which finds every declaration
 * real documents contain and would be fooled by one inside a comment in the
 * first kilobyte. Named rather than left to be discovered.
 *
 * The full Encoding Standard has around forty labels. This has the ones that
 * cover essentially every document: utf-8, utf-16 in both orders,
 * windows-1252 and the latin-1 spellings that mean it.
 */
#include "ar_html.h"

static int ar__e_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ar__e_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* Case-insensitive match of `n` bytes at `p` against a literal. */
static int ar__e_is(const char *p, ar_u32 n, const char *lit)
{
    ar_u32 i;

    for (i = 0; i < n; ++i)
    {
        if (lit[i] == 0 || ar__e_lower((unsigned char)p[i]) != ar__e_lower((unsigned char)lit[i]))
        {
            return 0;
        }
    }
    return lit[n] == 0;
}

/*
 * A label to an encoding, per the Encoding Standard's table.
 *
 * `latin1`, `iso-8859-1` and `ascii` all mean windows-1252 here, and that is
 * the standard's own answer rather than a shortcut: a document labelled
 * iso-8859-1 that contains a curly quote is relying on the 1252 mapping, and
 * every browser gives it one.
 */
ar_encoding ar_encoding_from_label(const char *label, ar_u32 n)
{
    ar_u32 start = 0;

    while (start < n && ar__e_space((unsigned char)label[start]))
    {
        ++start;
    }
    while (n > start && ar__e_space((unsigned char)label[n - 1]))
    {
        --n;
    }
    label += start;
    n -= start;

    if (ar__e_is(label, n, "utf-8") || ar__e_is(label, n, "utf8"))
    {
        return AR_ENC_UTF8;
    }
    if (ar__e_is(label, n, "utf-16le") || ar__e_is(label, n, "utf-16"))
    {
        return AR_ENC_UTF16LE;
    }
    if (ar__e_is(label, n, "utf-16be"))
    {
        return AR_ENC_UTF16BE;
    }
    if (ar__e_is(label, n, "windows-1252") || ar__e_is(label, n, "iso-8859-1") ||
        ar__e_is(label, n, "latin1") || ar__e_is(label, n, "ascii") ||
        ar__e_is(label, n, "us-ascii"))
    {
        return AR_ENC_WINDOWS1252;
    }
    return AR_ENC_UNKNOWN;
}

/* The `charset=` inside a `content` attribute, which is the older spelling and
   is still what most documents from before 2010 carry. */
static ar_encoding ar__charset_in_content(const char *p, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i + 8 <= n; ++i)
    {
        if (ar__e_is(p + i, 7, "charset"))
        {
            ar_u32 k = i + 7;
            ar_u32 start;

            while (k < n && ar__e_space((unsigned char)p[k]))
            {
                ++k;
            }
            if (k >= n || p[k] != '=')
            {
                continue;
            }
            ++k;
            while (k < n && ar__e_space((unsigned char)p[k]))
            {
                ++k;
            }
            if (k < n && (p[k] == '"' || p[k] == '\''))
            {
                char q = p[k++];

                start = k;
                while (k < n && p[k] != q)
                {
                    ++k;
                }
                return ar_encoding_from_label(p + start, k - start);
            }
            /* An unquoted value ends at whitespace, at a separator, and at a
               quote -- which it cannot contain, and which is here because
               `content="text/html; charset=utf-8"` puts the closing quote of
               the *content* attribute immediately after the label. Without it
               the label reads `utf-8"` and matches nothing. */
            start = k;
            while (k < n && !ar__e_space((unsigned char)p[k]) && p[k] != ';' && p[k] != '>' &&
                   p[k] != '"' && p[k] != '\'')
            {
                ++k;
            }
            return ar_encoding_from_label(p + start, k - start);
        }
    }
    return AR_ENC_UNKNOWN;
}

ar_encoding ar_encoding_sniff(const char *bytes, ar_u32 len, ar_u32 *skip)
{
    ar_u32 limit;
    ar_u32 i;

    if (skip)
    {
        *skip = 0;
    }
    if (!bytes || len == 0)
    {
        return AR_ENC_UTF8;
    }

    /*
     * Step 1: the byte order mark, which beats everything after it.
     *
     * A document with a UTF-8 BOM and `<meta charset=windows-1252>` is UTF-8,
     * and the specification is explicit about that because authoring tools
     * write both and disagree with themselves constantly.
     */
    if (len >= 3 && (unsigned char)bytes[0] == 0xEFu && (unsigned char)bytes[1] == 0xBBu &&
        (unsigned char)bytes[2] == 0xBFu)
    {
        if (skip)
        {
            *skip = 3;
        }
        return AR_ENC_UTF8;
    }
    if (len >= 2 && (unsigned char)bytes[0] == 0xFEu && (unsigned char)bytes[1] == 0xFFu)
    {
        if (skip)
        {
            *skip = 2;
        }
        return AR_ENC_UTF16BE;
    }
    if (len >= 2 && (unsigned char)bytes[0] == 0xFFu && (unsigned char)bytes[1] == 0xFEu)
    {
        if (skip)
        {
            *skip = 2;
        }
        return AR_ENC_UTF16LE;
    }

    /* Step 2: a declaration in the first 1024 bytes. */
    limit = len < 1024u ? len : 1024u;
    for (i = 0; i + 6 <= limit; ++i)
    {
        ar_u32 k;
        ar_u32 stop;

        if (bytes[i] != '<' || !ar__e_is(bytes + i + 1, 4, "meta"))
        {
            continue;
        }
        k = i + 5;
        stop = k;
        while (stop < limit && bytes[stop] != '>')
        {
            ++stop;
        }

        /* `<meta charset="utf-8">`, the modern spelling. */
        {
            ar_u32 a;

            for (a = k; a + 8 <= stop; ++a)
            {
                if (ar__e_is(bytes + a, 7, "charset"))
                {
                    ar_u32 v = a + 7;
                    ar_u32 start;

                    while (v < stop && ar__e_space((unsigned char)bytes[v]))
                    {
                        ++v;
                    }
                    if (v >= stop || bytes[v] != '=')
                    {
                        continue;
                    }
                    ++v;
                    while (v < stop && ar__e_space((unsigned char)bytes[v]))
                    {
                        ++v;
                    }
                    if (v < stop && (bytes[v] == '"' || bytes[v] == '\''))
                    {
                        char q = bytes[v++];

                        start = v;
                        while (v < stop && bytes[v] != q)
                        {
                            ++v;
                        }
                    }
                    else
                    {
                        start = v;
                        while (v < stop && !ar__e_space((unsigned char)bytes[v]) &&
                               bytes[v] != '/' && bytes[v] != '"' && bytes[v] != '\'' &&
                               bytes[v] != ';')
                        {
                            ++v;
                        }
                    }
                    {
                        ar_encoding e = ar_encoding_from_label(bytes + start, v - start);

                        if (e != AR_ENC_UNKNOWN)
                        {
                            return e;
                        }
                    }
                }
            }
        }

        /* `<meta http-equiv=content-type content="text/html; charset=...">`. */
        {
            ar_encoding e = ar__charset_in_content(bytes + k, stop - k);

            if (e != AR_ENC_UNKNOWN)
            {
                return e;
            }
        }
    }

    /*
     * Step 3: the default.
     *
     * The specification makes this locale-dependent and recommends
     * windows-1252 for most locales. It is the right guess for the documents
     * that carry no declaration, which are overwhelmingly old and Western.
     */
    return AR_ENC_WINDOWS1252;
}

/* The 32 code points windows-1252 puts where C1 controls would be. The same
   table the numeric character reference rule uses, and for the same reason. */
static ar_u32 ar__cp1252_high(ar_u32 b)
{
    static const ar_u32 MAP[32] = {0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                   0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                                   0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                   0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

    return (b >= 0x80u && b <= 0x9Fu) ? MAP[b - 0x80u] : b;
}

static ar_u32 ar__emit_utf8(ar_u32 cp, char *out, ar_u32 at, ar_u32 cap)
{
    if (cp < 0x80u)
    {
        if (at + 1 > cap)
        {
            return at;
        }
        out[at++] = (char)cp;
        return at;
    }
    if (cp < 0x800u)
    {
        if (at + 2 > cap)
        {
            return at;
        }
        out[at++] = (char)(0xC0u | (cp >> 6));
        out[at++] = (char)(0x80u | (cp & 0x3Fu));
        return at;
    }
    if (cp < 0x10000u)
    {
        if (at + 3 > cap)
        {
            return at;
        }
        out[at++] = (char)(0xE0u | (cp >> 12));
        out[at++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[at++] = (char)(0x80u | (cp & 0x3Fu));
        return at;
    }
    if (at + 4 > cap)
    {
        return at;
    }
    out[at++] = (char)(0xF0u | (cp >> 18));
    out[at++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[at++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[at++] = (char)(0x80u | (cp & 0x3Fu));
    return at;
}

ar_u32 ar_encoding_decode(ar_encoding enc, const char *in, ar_u32 len, char *out, ar_u32 cap)
{
    ar_u32 at = 0;
    ar_u32 i;

    if (!in || !out || cap == 0)
    {
        return 0;
    }

    if (enc == AR_ENC_UTF8)
    {
        /* Already what the tokenizer reads. Copied rather than aliased so one
           caller does not have to know which case it got. */
        ar_u32 n = len < cap ? len : cap;

        for (i = 0; i < n; ++i)
        {
            out[i] = in[i];
        }
        return n;
    }

    if (enc == AR_ENC_WINDOWS1252)
    {
        for (i = 0; i < len && at < cap; ++i)
        {
            at = ar__emit_utf8(ar__cp1252_high((unsigned char)in[i]), out, at, cap);
        }
        return at;
    }

    /* UTF-16, both orders, with surrogate pairs joined. */
    for (i = 0; i + 1 < len && at < cap; i += 2)
    {
        ar_u32 unit = (enc == AR_ENC_UTF16BE)
                          ? (((ar_u32)(unsigned char)in[i] << 8) | (unsigned char)in[i + 1])
                          : (((ar_u32)(unsigned char)in[i + 1] << 8) | (unsigned char)in[i]);

        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 3 < len)
        {
            ar_u32 lo = (enc == AR_ENC_UTF16BE)
                            ? (((ar_u32)(unsigned char)in[i + 2] << 8) | (unsigned char)in[i + 3])
                            : (((ar_u32)(unsigned char)in[i + 3] << 8) | (unsigned char)in[i + 2]);

            if (lo >= 0xDC00u && lo <= 0xDFFFu)
            {
                unit = 0x10000u + ((unit - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2;
            }
        }
        if (unit >= 0xD800u && unit <= 0xDFFFu)
        {
            unit = 0xFFFDu; /* an unpaired surrogate is not a character */
        }
        at = ar__emit_utf8(unit, out, at, cap);
    }
    return at;
}
