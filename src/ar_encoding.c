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

    /*
     * Every label the Encoding Standard gives these four, and not a subset.
     *
     * There were nine. The standard has thirty-two, and the missing ones are
     * not exotic: `iso8859-1` without the second hyphen, `cp1252`, `l1`,
     * `csisolatin1`, `unicode-1-1-utf-8`. A document labelled with one of
     * those was falling through to the windows-1252 default by luck rather
     * than by decision, and `unicode-1-1-utf-8` was falling through to it
     * wrongly.
     *
     * Only these four encodings exist here; a label for any other -- shift_jis,
     * euc-kr, koi8-r -- comes back AR_ENC_UNKNOWN and the caller falls back to
     * the default. That is a real limit and is not the same as the label being
     * unrecognised.
     */
    {
        static const char *const UTF8[] = {
            "utf-8",           "utf8", "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8",
            "x-unicode20utf8", 0};
        static const char *const W1252[] = {"windows-1252",    "ansi_x3.4-1968",
                                            "ascii",           "cp1252",
                                            "cp819",           "csisolatin1",
                                            "ibm819",          "iso-8859-1",
                                            "iso-ir-100",      "iso8859-1",
                                            "iso88591",        "iso_8859-1",
                                            "iso_8859-1:1987", "l1",
                                            "latin1",          "us-ascii",
                                            "x-cp1252",        0};
        static const char *const U16LE[] = {"utf-16",          "utf-16le", "csunicode",   "ucs-2",
                                            "iso-10646-ucs-2", "unicode",  "unicodefeff", 0};
        static const char *const U16BE[] = {"utf-16be", "unicodefffe", 0};
        ar_u32                   i;

        for (i = 0; UTF8[i]; ++i)
        {
            if (ar__e_is(label, n, UTF8[i]))
            {
                return AR_ENC_UTF8;
            }
        }
        for (i = 0; W1252[i]; ++i)
        {
            if (ar__e_is(label, n, W1252[i]))
            {
                return AR_ENC_WINDOWS1252;
            }
        }
        for (i = 0; U16LE[i]; ++i)
        {
            if (ar__e_is(label, n, U16LE[i]))
            {
                return AR_ENC_UTF16LE;
            }
        }
        for (i = 0; U16BE[i]; ++i)
        {
            if (ar__e_is(label, n, U16BE[i]))
            {
                return AR_ENC_UTF16BE;
            }
        }
    }
    return AR_ENC_UNKNOWN;
}

/* ------------------------------------------------------------------------
 * The prescan, §13.2.3.3
 *
 * "Prescan a byte stream to determine its encoding", and it is an algorithm
 * rather than a search. That distinction is the whole of this section.
 *
 * Searching the first kilobyte for `charset` finds one in a comment, in an
 * unrelated attribute, in a script, in prose. Each of those is a document
 * decoded as something its author did not write, which is not a subtle
 * failure: every accented letter in it becomes a replacement character and it
 * looks like a font problem.
 *
 * So this walks the bytes the way the specification does -- skipping comments
 * whole, skipping other tags by reading their attributes, and only reading
 * `charset` out of a `<meta>` -- with the pragma rule that a `content`
 * attribute counts only when `http-equiv` says `content-type` beside it.
 *
 * It is not a tokenizer and does not need to be: it never has to build a tree,
 * so it can be a few hundred lines that stop at the first answer.
 * ------------------------------------------------------------------------ */

/* The spec's whitespace for this algorithm: tab, LF, FF, CR, space. */
static int ar__p_space(int c)
{
    return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20;
}

static int ar__p_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/*
 * "Get an attribute", §13.2.3.3.
 *
 * Returns 0 when the tag has no more of them. The name and value are reported
 * as spans of the input, lowercased on comparison rather than in place --
 * these bytes are the caller's, as everywhere else in this parser.
 */
static int ar__p_attr(const char *b, ar_u32 n, ar_u32 *at, ar_u32 *ns, ar_u32 *nn, ar_u32 *vs,
                      ar_u32 *vn)
{
    ar_u32 i = *at;

    while (i < n && (ar__p_space((unsigned char)b[i]) || b[i] == '/'))
    {
        ++i;
    }
    if (i >= n || b[i] == '>')
    {
        *at = i;
        return 0;
    }

    *ns = i;
    while (i < n)
    {
        if (b[i] == '=' && i > *ns)
        {
            break;
        }
        if (ar__p_space((unsigned char)b[i]) || b[i] == '/' || b[i] == '>')
        {
            break;
        }
        ++i;
    }
    *nn = i - *ns;

    while (i < n && ar__p_space((unsigned char)b[i]))
    {
        ++i;
    }
    if (i >= n || b[i] != '=')
    {
        /* A bare attribute. Its value is empty and the next one starts here. */
        *vs = i;
        *vn = 0;
        *at = i;
        return 1;
    }
    ++i; /* the `=` */
    while (i < n && ar__p_space((unsigned char)b[i]))
    {
        ++i;
    }
    if (i < n && (b[i] == '"' || b[i] == '\''))
    {
        char q = b[i];

        ++i;
        *vs = i;
        while (i < n && b[i] != q)
        {
            ++i;
        }
        *vn = i - *vs;
        if (i < n)
        {
            ++i; /* the closing quote */
        }
        *at = i;
        return 1;
    }
    *vs = i;
    while (i < n && !ar__p_space((unsigned char)b[i]) && b[i] != '>')
    {
        ++i;
    }
    *vn = i - *vs;
    *at = i;
    return 1;
}

/*
 * "Extract a character encoding from a meta element", from a `content` value.
 *
 * The older spelling, and still what most documents from before 2010 carry:
 * `<meta http-equiv="Content-Type" content="text/html; charset=iso-8859-1">`.
 */
static ar_encoding ar__p_from_content(const char *b, ar_u32 n)
{
    ar_u32 i = 0;

    while (i + 7 <= n)
    {
        if (!ar__e_is(b + i, 7, "charset"))
        {
            ++i;
            continue;
        }
        i += 7;
        while (i < n && ar__p_space((unsigned char)b[i]))
        {
            ++i;
        }
        if (i >= n || b[i] != '=')
        {
            continue; /* `charset` without an `=` is not a declaration */
        }
        ++i;
        while (i < n && ar__p_space((unsigned char)b[i]))
        {
            ++i;
        }
        if (i < n && (b[i] == '"' || b[i] == '\''))
        {
            char   q = b[i];
            ar_u32 s;

            ++i;
            s = i;
            while (i < n && b[i] != q)
            {
                ++i;
            }
            return ar_encoding_from_label(b + s, i - s);
        }
        {
            ar_u32 s = i;

            while (i < n && !ar__p_space((unsigned char)b[i]) && b[i] != ';')
            {
                ++i;
            }
            return ar_encoding_from_label(b + s, i - s);
        }
    }
    return AR_ENC_UNKNOWN;
}

/*
 * A found encoding, adjusted the way the specification adjusts it.
 *
 * A document that declares UTF-16 in a `<meta>` is lying by construction: the
 * declaration itself was read as ASCII, so the bytes cannot have been UTF-16.
 * The specification says to treat it as UTF-8, and that is not a courtesy --
 * it is the only reading under which the document makes sense.
 */
static ar_encoding ar__p_adjust(ar_encoding e)
{
    if (e == AR_ENC_UTF16LE || e == AR_ENC_UTF16BE)
    {
        return AR_ENC_UTF8;
    }
    return e;
}

static ar_encoding ar__prescan(const char *b, ar_u32 n)
{
    ar_u32 i = 0;

    while (i < n)
    {
        if (b[i] != '<')
        {
            ++i;
            continue;
        }

        /* A comment is skipped whole, which is the single most important line
           here: `<!-- charset=utf-8 -->` declares nothing. */
        if (i + 4 <= n && b[i + 1] == '!' && b[i + 2] == '-' && b[i + 3] == '-')
        {
            ar_u32 j = i + 4;

            while (j + 3 <= n && !(b[j] == '-' && b[j + 1] == '-' && b[j + 2] == '>'))
            {
                ++j;
            }
            i = j + 3 <= n ? j + 3 : n;
            continue;
        }

        if (i + 6 <= n && ar__e_is(b + i + 1, 4, "meta") &&
            (ar__p_space((unsigned char)b[i + 5]) || b[i + 5] == '/'))
        {
            ar_u32      at = i + 5;
            ar_u32      ns, nn, vs, vn;
            int         got_pragma = 0;
            int         need_pragma = -1; /* -1 is the specification's null */
            ar_encoding charset = AR_ENC_UNKNOWN;

            while (ar__p_attr(b, n, &at, &ns, &nn, &vs, &vn))
            {
                if (ar__e_is(b + ns, nn, "http-equiv"))
                {
                    if (ar__e_is(b + vs, vn, "content-type"))
                    {
                        got_pragma = 1;
                    }
                }
                else if (ar__e_is(b + ns, nn, "content"))
                {
                    ar_encoding e = ar__p_from_content(b + vs, vn);

                    if (e != AR_ENC_UNKNOWN && charset == AR_ENC_UNKNOWN)
                    {
                        charset = e;
                        need_pragma = 1;
                    }
                }
                else if (ar__e_is(b + ns, nn, "charset"))
                {
                    charset = ar_encoding_from_label(b + vs, vn);
                    need_pragma = 0;
                }
            }

            /*
             * The pragma rule. A `content` attribute counts only when
             * `http-equiv="content-type"` stands beside it -- so
             * `<meta name="description" content="charset=utf-8">` declares
             * nothing, which is exactly the kind of sentence that used to fool
             * the search this replaced.
             */
            if (need_pragma < 0 || (need_pragma == 1 && !got_pragma) || charset == AR_ENC_UNKNOWN)
            {
                i = at;
                continue;
            }
            return ar__p_adjust(charset);
        }

        /* Any other tag: step over its name, then read its attributes so the
           walk resumes after them rather than inside a quoted value. */
        if (i + 2 <= n && (ar__p_alpha((unsigned char)b[i + 1]) ||
                           (b[i + 1] == '/' && i + 3 <= n && ar__p_alpha((unsigned char)b[i + 2]))))
        {
            ar_u32 at = i + 1;
            ar_u32 ns, nn, vs, vn;

            while (at < n && !ar__p_space((unsigned char)b[at]) && b[at] != '>')
            {
                ++at;
            }
            while (ar__p_attr(b, n, &at, &ns, &nn, &vs, &vn))
            {
                /* nothing: the point is where it leaves `at` */
            }
            i = at < n ? at + 1 : n;
            continue;
        }

        if (i + 2 <= n && (b[i + 1] == '!' || b[i + 1] == '/' || b[i + 1] == '?'))
        {
            ar_u32 j = i + 2;

            while (j < n && b[j] != '>')
            {
                ++j;
            }
            i = j < n ? j + 1 : n;
            continue;
        }

        ++i;
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

    /* Step 2: a declaration in the first 1024 bytes, found by the
       specification's prescan rather than by looking for the word. */
    limit = len < 1024u ? len : 1024u;
    i = ar__prescan(bytes, limit);
    if (i != (ar_u32)AR_ENC_UNKNOWN)
    {
        return (ar_encoding)i;
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
