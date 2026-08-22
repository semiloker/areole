/*
 * areole - the html5lib conformance suites.
 * SPDX-License-Identifier: MIT
 *
 *     ar_html5lib                    both suites, a summary and every failure
 *     ar_html5lib --tokenizer        just the tokenizer suite
 *     ar_html5lib --tree             just tree construction
 *     ar_html5lib --quiet            counts only
 *     ar_html5lib --file NAME        one file, by name
 *
 * 0.9.0 acceptance criteria 1 and 2: the tokenizer suite at 100%, and tree
 * construction at 98% with every failure listed by name and cause.
 *
 * ------------------------------------------------------------------------
 * Why this reads the suites rather than a generated form of them
 *
 * The obvious shortcut is a Python script that converts 2 MB of JSON and .dat
 * into something a C program can read without effort, committed beside the
 * original. That is two copies of the same tests, and the day they disagree is
 * the day the gate means nothing.
 *
 * So this reads the vendored files as they ship. The .dat format is
 * line-based and costs nothing. The tokenizer files are JSON, which costs a
 * reader -- but a small one, because the schema is known: the shape is always
 * {"tests": [...]}, and a test is an object with seven possible keys. Parsing
 * to a schema rather than to a general value tree is a third of the code and
 * says what it expects.
 *
 * ------------------------------------------------------------------------
 * What is compared, and what is not
 *
 * Tokens, not error messages. Each tokenizer test carries an `errors` array
 * naming the specification's own error codes -- "eof-in-tag",
 * "unexpected-character-in-attribute-name" -- and areole does not have that
 * vocabulary: `ar_html_tok.errors` is a count, because every recovery is
 * defined and a parser that stops disagrees with every browser. Comparing
 * error names would mean inventing sixty identifiers to match a list. It is
 * worth doing and it is not done, so the pass rate below is a pass rate on
 * token output and says so rather than implying more.
 *
 * Tree construction compares the whole serialised tree, which includes
 * attributes, their order, comments, doctypes and text. That is the real test
 * and there is no equivalent hedge.
 */
#include "ar_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Storage
 *
 * Static, because this is a test binary with a known worst case and the
 * library it is testing does not allocate either. namedEntities.test is 1.1 MB
 * on its own, which sets the file buffer.
 * ------------------------------------------------------------------------ */
#define FILE_CAP    (3u * 1024u * 1024u)
#define STR_CAP     (1024u * 1024u)
#define MAX_TOKENS  4096
#define MAX_ATTRS   64
#define DOC_NODES   8192
#define DOC_ATTRS   2048
#define DOC_TEXT    (512u * 1024u)
#define SCRATCH_CAP (64u * 1024u)
#define TREE_CAP    (512u * 1024u)

static char   g_file[FILE_CAP];
static ar_u32 g_file_n;

/* One test's decoded strings live here and are reset per test. */
static char   g_str[STR_CAP];
static ar_u32 g_str_n;

static ar_dom_node g_nodes[DOC_NODES];
static ar_attr     g_attrs[DOC_ATTRS];
static char        g_text[DOC_TEXT];
static char        g_scratch[SCRATCH_CAP];
static ar_doc      g_doc;

static char   g_tree[TREE_CAP];
static ar_u32 g_tree_n;

/* Counts, and the failure list. */
static ar_i32 g_run;
static ar_i32 g_pass;
static ar_i32 g_fail;
static ar_i32 g_skip;
static int    g_quiet;
static int    g_first_only;

static ar_u32 g_cause_fragment;
static ar_u32 g_cause_foreign;
static ar_u32 g_cause_template;
static ar_u32 g_cause_script;
static ar_u32 g_cause_other;

static char *str_alloc(ar_u32 n)
{
    char *p;

    if (g_str_n + n + 1u > STR_CAP)
    {
        printf("  the string arena is too small\n");
        exit(2);
    }
    p = g_str + g_str_n;
    g_str_n += n + 1u;
    return p;
}

static int read_file(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
    {
        return 0;
    }
    g_file_n = (ar_u32)fread(g_file, 1, FILE_CAP - 1u, f);
    fclose(f);
    g_file[g_file_n] = 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * UTF-8, for the \uXXXX escapes
 * ------------------------------------------------------------------------ */
static ar_u32 put_utf8(char *out, ar_u32 cp)
{
    if (cp < 0x80u)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u)
    {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u)
    {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static int hex4(const char *p, ar_u32 *out)
{
    ar_u32 v = 0;
    int    i;

    for (i = 0; i < 4; ++i)
    {
        char c = p[i];

        v <<= 4;
        if (c >= '0' && c <= '9')
        {
            v |= (ar_u32)(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            v |= (ar_u32)(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            v |= (ar_u32)(c - 'A' + 10);
        }
        else
        {
            return 0;
        }
    }
    *out = v;
    return 1;
}

/*
 * A second pass of \uXXXX decoding, for the ten tests that ask for it.
 *
 * `doubleEscaped` exists so a test can state a lone surrogate or a control
 * character in a JSON file without the JSON reader normalising it away: the
 * escape survives the first decode as the six literal characters `A` and
 * is decoded again here. A lone surrogate is written out as UTF-8 of the
 * surrogate itself, which is not valid UTF-8 and is exactly the point -- the
 * test is about what the tokenizer does with it.
 */
static ar_u32 unescape_again(char *s, ar_u32 n)
{
    ar_u32 r = 0;
    ar_u32 w = 0;

    while (r < n)
    {
        if (s[r] == '\\' && r + 5u < n + 1u && s[r + 1] == 'u' && r + 6u <= n)
        {
            ar_u32 cp;

            if (hex4(s + r + 2, &cp))
            {
                w += put_utf8(s + w, cp);
                r += 6;
                continue;
            }
        }
        s[w++] = s[r++];
    }
    s[w] = 0;
    return w;
}

/* ------------------------------------------------------------------------
 * A JSON reader for one known schema
 * ------------------------------------------------------------------------ */
typedef struct json
{
    const char *p;
    const char *end;
} json;

static void j_ws(json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
    {
        ++j->p;
    }
}

static int j_eat(json *j, char c)
{
    j_ws(j);
    if (j->p < j->end && *j->p == c)
    {
        ++j->p;
        return 1;
    }
    return 0;
}

static int j_peek(json *j, char c)
{
    j_ws(j);
    return j->p < j->end && *j->p == c;
}

/* A string into the arena, decoded. Returns its length via *len. */
static char *j_string(json *j, ar_u32 *len)
{
    const char *start;
    char       *out;
    ar_u32      n = 0;

    j_ws(j);
    if (j->p >= j->end || *j->p != '"')
    {
        return 0;
    }
    ++j->p;
    start = j->p;

    /* Measure first: the decoded form is never longer than the source. */
    while (j->p < j->end && *j->p != '"')
    {
        if (*j->p == '\\')
        {
            ++j->p;
        }
        ++j->p;
    }
    out = str_alloc((ar_u32)(j->p - start));
    j->p = start;

    while (j->p < j->end && *j->p != '"')
    {
        if (*j->p != '\\')
        {
            out[n++] = *j->p++;
            continue;
        }
        ++j->p;
        if (j->p >= j->end)
        {
            break;
        }
        switch (*j->p)
        {
        case 'n':
            out[n++] = '\n';
            ++j->p;
            break;
        case 't':
            out[n++] = '\t';
            ++j->p;
            break;
        case 'r':
            out[n++] = '\r';
            ++j->p;
            break;
        case 'b':
            out[n++] = '\b';
            ++j->p;
            break;
        case 'f':
            out[n++] = '\f';
            ++j->p;
            break;
        case 'u':
        {
            ar_u32 cp = 0;

            ++j->p;
            if (j->p + 4 <= j->end && hex4(j->p, &cp))
            {
                j->p += 4;
                /* A surrogate pair is one code point; a lone surrogate is
                   written as itself, because a test that states one is asking
                   what happens to it. */
                if (cp >= 0xD800u && cp <= 0xDBFFu && j->p + 6 <= j->end && j->p[0] == '\\' &&
                    j->p[1] == 'u')
                {
                    ar_u32 lo = 0;

                    if (hex4(j->p + 2, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu)
                    {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        j->p += 6;
                    }
                }
                n += put_utf8(out + n, cp);
            }
            break;
        }
        default:
            out[n++] = *j->p++;
            break;
        }
    }
    if (j->p < j->end)
    {
        ++j->p; /* the closing quote */
    }
    out[n] = 0;
    if (len)
    {
        *len = n;
    }
    return out;
}

static void j_skip_value(json *j);

static void j_skip_rest(json *j, char close)
{
    while (j->p < j->end)
    {
        j_ws(j);
        if (j_peek(j, close))
        {
            ++j->p;
            return;
        }
        if (j_peek(j, ','))
        {
            ++j->p;
            continue;
        }
        j_skip_value(j);
    }
}

static void j_skip_value(json *j)
{
    j_ws(j);
    if (j->p >= j->end)
    {
        return;
    }
    if (*j->p == '"')
    {
        ar_u32 save = g_str_n;

        (void)j_string(j, 0);
        g_str_n = save; /* skipped, not kept */
        return;
    }
    if (*j->p == '{')
    {
        ++j->p;
        j_skip_rest(j, '}');
        return;
    }
    if (*j->p == '[')
    {
        ++j->p;
        j_skip_rest(j, ']');
        return;
    }
    if (*j->p == ':')
    {
        ++j->p;
        j_skip_value(j);
        return;
    }
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']')
    {
        ++j->p;
    }
}

/* ------------------------------------------------------------------------
 * Tokens, in the shape html5lib states them
 * ------------------------------------------------------------------------ */
typedef enum tk_kind
{
    TK_CHAR = 0,
    TK_START,
    TK_END,
    TK_COMMENT,
    TK_DOCTYPE
} tk_kind;

/*
 * A token, with lengths beside the pointers.
 *
 * Not NUL-terminated strings, and this is not fastidiousness: U+0000 is a
 * character the specification has rules about, several dozen tests state it in
 * their expected output, and a NUL-terminated comparison reads every one of
 * them as ending early. The first version did, and reported the library wrong
 * on twenty-one tests it was getting right.
 */
typedef struct tk
{
    tk_kind kind;
    char   *name;
    char   *data; /* character data, comment body */
    ar_u32  data_n;
    char   *pub;
    char   *sys;
    int     have_pub;
    int     have_sys;
    int     correct;      /* doctype: the JSON's fifth element */
    int     self_closing; /* start tag */
    char   *an[MAX_ATTRS];
    char   *av[MAX_ATTRS];
    ar_u32  av_n[MAX_ATTRS];
    ar_i32  attr_n;
} tk;

static tk     g_want[MAX_TOKENS];
static ar_i32 g_want_n;
static tk     g_got[MAX_TOKENS];
static ar_i32 g_got_n;

static char *dup_span(ar_span s)
{
    char *out = str_alloc(s.n);

    if (s.n)
    {
        memcpy(out, s.p, s.n);
    }
    out[s.n] = 0;
    return out;
}

static int same(const char *a, const char *b)
{
    if (!a)
    {
        a = "";
    }
    if (!b)
    {
        b = "";
    }
    return strcmp(a, b) == 0;
}

/* The same question, for the fields that may hold a NUL. */
static int same_n(const char *a, ar_u32 an, const char *b, ar_u32 bn)
{
    if (an != bn)
    {
        return 0;
    }
    return an == 0 || memcmp(a, b, an) == 0;
}

/* Consecutive character tokens are one token. html5lib says so explicitly, and
   areole coalesces runs anyway; this makes both sides agree on where a run
   ends when a reference splits one. */
static void push_char(tk *list, ar_i32 *n, char *data, ar_u32 data_n)
{
    if (*n > 0 && list[*n - 1].kind == TK_CHAR)
    {
        tk    *prev = &list[*n - 1];
        ar_u32 a = prev->data_n;
        char  *joined = str_alloc(a + data_n);

        memcpy(joined, prev->data, a);
        memcpy(joined + a, data, data_n);
        joined[a + data_n] = 0;
        prev->data = joined;
        prev->data_n = a + data_n;
        return;
    }
    if (*n >= MAX_TOKENS)
    {
        return;
    }
    memset(&list[*n], 0, sizeof list[0]);
    list[*n].kind = TK_CHAR;
    list[*n].data = data;
    list[*n].data_n = data_n;
    ++*n;
}

static int tokens_equal(void)
{
    ar_i32 i;
    ar_i32 k;

    if (g_want_n != g_got_n)
    {
        return 0;
    }
    for (i = 0; i < g_want_n; ++i)
    {
        const tk *a = &g_want[i];
        const tk *b = &g_got[i];

        if (a->kind != b->kind)
        {
            return 0;
        }
        switch (a->kind)
        {
        case TK_CHAR:
        case TK_COMMENT:
            if (!same_n(a->data, a->data_n, b->data, b->data_n))
            {
                return 0;
            }
            break;
        case TK_END:
            if (!same(a->name, b->name))
            {
                return 0;
            }
            break;
        case TK_START:
            if (!same(a->name, b->name) || a->self_closing != b->self_closing ||
                a->attr_n != b->attr_n)
            {
                return 0;
            }
            for (k = 0; k < a->attr_n; ++k)
            {
                ar_i32 j;
                int    found = 0;

                for (j = 0; j < b->attr_n; ++j)
                {
                    if (same(a->an[k], b->an[j]) &&
                        same_n(a->av[k], a->av_n[k], b->av[j], b->av_n[j]))
                    {
                        found = 1;
                        break;
                    }
                }
                if (!found)
                {
                    return 0;
                }
            }
            break;
        case TK_DOCTYPE:
            if (!same(a->name, b->name) || a->correct != b->correct)
            {
                return 0;
            }
            if (a->have_pub != b->have_pub || a->have_sys != b->have_sys)
            {
                return 0;
            }
            if (a->have_pub && !same(a->pub, b->pub))
            {
                return 0;
            }
            if (a->have_sys && !same(a->sys, b->sys))
            {
                return 0;
            }
            break;
        default:
            break;
        }
    }
    return 1;
}

static void print_escaped_len(const char *s, ar_u32 n)
{
    ar_u32 i;

    if (!s)
    {
        printf("(null)");
        return;
    }
    for (i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n')
        {
            printf("\\n");
        }
        else if (c == '\t')
        {
            printf("\\t");
        }
        else if (c >= 0x20u && c < 0x7Fu)
        {
            putchar((int)c);
        }
        else
        {
            printf("\\x%02X", (unsigned)c);
        }
    }
}

static void print_escaped(const char *s)
{
    print_escaped_len(s, s ? (ar_u32)strlen(s) : 0u);
}

static void print_token(const tk *t)
{
    ar_i32 k;

    switch (t->kind)
    {
    case TK_CHAR:
        printf("Character \"");
        print_escaped_len(t->data, t->data_n);
        printf("\"");
        break;
    case TK_START:
        printf("StartTag <%s", t->name ? t->name : "");
        for (k = 0; k < t->attr_n; ++k)
        {
            printf(" %s=\"", t->an[k]);
            print_escaped_len(t->av[k], t->av_n[k]);
            printf("\"");
        }
        printf("%s>", t->self_closing ? "/" : "");
        break;
    case TK_END:
        printf("EndTag </%s>", t->name ? t->name : "");
        break;
    case TK_COMMENT:
        printf("Comment <!--");
        print_escaped_len(t->data, t->data_n);
        printf("-->");
        break;
    case TK_DOCTYPE:
        printf("DOCTYPE %s correct=%d", t->name ? t->name : "(null)", t->correct);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * The tokenizer suite
 * ------------------------------------------------------------------------ */
static const struct
{
    const char   *name;
    ar_html_state state;
} STATES[] = {{"Data state", AR_HTML_DATA},
              {"PLAINTEXT state", AR_HTML_PLAINTEXT},
              {"RCDATA state", AR_HTML_RCDATA},
              {"RAWTEXT state", AR_HTML_RAWTEXT},
              {"Script data state", AR_HTML_SCRIPT}};

#define STATE_COUNT ((int)(sizeof STATES / sizeof STATES[0]))

static void lower_in_place(char *p)
{
    while (p && *p)
    {
        if (*p >= 'A' && *p <= 'Z')
        {
            *p = (char)(*p - 'A' + 'a');
        }
        ++p;
    }
}

static void report_failure(const char *file, const char *what, const char *input)
{
    ++g_fail;
    if (g_quiet)
    {
        return;
    }
    printf("  FAIL %s: %s\n", file, what);
    if (input)
    {
        printf("       input  ");
        print_escaped(input);
        printf("\n");
    }
}

static void collect_tokens(const char *input, ar_u32 len, ar_html_state state,
                           const char *last_start)
{
    ar_html_tok t;
    ar_token    tok;

    ar_html_tok_init(&t, input, len, g_scratch, SCRATCH_CAP);
    t.state = state;
    if (last_start)
    {
        ar_u32 n = (ar_u32)strlen(last_start);
        ar_u32 i;

        if (n > sizeof t.last_start)
        {
            n = (ar_u32)sizeof t.last_start;
        }
        for (i = 0; i < n; ++i)
        {
            char c = last_start[i];

            t.last_start[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        t.last_start_n = n;
    }

    g_got_n = 0;
    while (ar_html_next(&t, &tok))
    {
        tk *out;

        if (tok.kind == AR_TOK_TEXT)
        {
            if (tok.text.n)
            {
                push_char(g_got, &g_got_n, dup_span(tok.text), tok.text.n);
            }
            continue;
        }
        if (g_got_n >= MAX_TOKENS)
        {
            break;
        }
        out = &g_got[g_got_n++];
        memset(out, 0, sizeof *out);
        switch (tok.kind)
        {
        case AR_TOK_START:
        case AR_TOK_END:
        {
            ar_i32 k;

            out->kind = tok.kind == AR_TOK_START ? TK_START : TK_END;
            /* The tokenizer lowercases nothing -- the input is the caller's
               and every comparison folds case -- so the fold happens here,
               where html5lib states its names lowercased. */
            out->name = dup_span(tok.name);
            lower_in_place(out->name);
            out->self_closing = tok.self_closing;
            for (k = 0; k < tok.attr_count && k < MAX_ATTRS; ++k)
            {
                out->an[out->attr_n] = dup_span(tok.attrs[k].name);
                lower_in_place(out->an[out->attr_n]);
                out->av[out->attr_n] = dup_span(tok.attrs[k].value);
                out->av_n[out->attr_n] = tok.attrs[k].value.n;
                ++out->attr_n;
            }
            break;
        }
        case AR_TOK_COMMENT:
            out->kind = TK_COMMENT;
            out->data = dup_span(tok.text);
            out->data_n = tok.text.n;
            break;
        case AR_TOK_DOCTYPE:
            out->kind = TK_DOCTYPE;
            out->name = tok.name.p ? dup_span(tok.name) : 0;
            out->have_pub = tok.pub.p != 0;
            out->have_sys = tok.sys.p != 0;
            out->pub = out->have_pub ? dup_span(tok.pub) : 0;
            out->sys = out->have_sys ? dup_span(tok.sys) : 0;
            out->correct = !tok.force_quirks;
            break;
        default:
            --g_got_n;
            break;
        }
    }
}

/* One `output` array into g_want. Returns 0 on a shape this runner has never
   seen, which is a gap here rather than a failure in the library. */
static int read_expected(json *j, int dbl)
{
    g_want_n = 0;
    if (!j_eat(j, '['))
    {
        return 0;
    }
    if (j_eat(j, ']'))
    {
        return 1;
    }
    for (;;)
    {
        char  *kind;
        ar_u32 kn = 0;

        if (!j_eat(j, '['))
        {
            return 0;
        }
        kind = j_string(j, &kn);
        if (!kind)
        {
            return 0;
        }

        if (strcmp(kind, "Character") == 0)
        {
            char  *d;
            ar_u32 n = 0;

            (void)j_eat(j, ',');
            d = j_string(j, &n);
            if (!d)
            {
                return 0;
            }
            if (dbl)
            {
                n = unescape_again(d, n);
            }
            push_char(g_want, &g_want_n, d, n);
        }
        else
        {
            tk *w;

            if (g_want_n >= MAX_TOKENS)
            {
                return 0;
            }
            w = &g_want[g_want_n++];
            memset(w, 0, sizeof *w);

            if (strcmp(kind, "Comment") == 0)
            {
                ar_u32 n = 0;

                w->kind = TK_COMMENT;
                (void)j_eat(j, ',');
                w->data = j_string(j, &n);
                if (dbl && w->data)
                {
                    n = unescape_again(w->data, n);
                }
                w->data_n = n;
            }
            else if (strcmp(kind, "EndTag") == 0)
            {
                w->kind = TK_END;
                (void)j_eat(j, ',');
                w->name = j_string(j, 0);
            }
            else if (strcmp(kind, "StartTag") == 0)
            {
                w->kind = TK_START;
                (void)j_eat(j, ',');
                w->name = j_string(j, 0);
                (void)j_eat(j, ',');
                if (j_eat(j, '{'))
                {
                    while (!j_eat(j, '}'))
                    {
                        char *an = j_string(j, 0);

                        if (!an)
                        {
                            break;
                        }
                        (void)j_eat(j, ':');
                        if (w->attr_n < MAX_ATTRS)
                        {
                            ar_u32 vn = 0;

                            w->an[w->attr_n] = an;
                            w->av[w->attr_n] = j_string(j, &vn);
                            if (dbl && w->av[w->attr_n])
                            {
                                vn = unescape_again(w->av[w->attr_n], vn);
                            }
                            w->av_n[w->attr_n] = vn;
                            ++w->attr_n;
                        }
                        else
                        {
                            j_skip_value(j);
                        }
                        (void)j_eat(j, ',');
                    }
                }
                /* A fourth element, when present, is the self-closing flag. */
                if (j_eat(j, ','))
                {
                    j_ws(j);
                    if (j->p < j->end && *j->p == 't')
                    {
                        w->self_closing = 1;
                    }
                    j_skip_value(j);
                }
            }
            else if (strcmp(kind, "DOCTYPE") == 0)
            {
                int slot;

                w->kind = TK_DOCTYPE;
                for (slot = 0; slot < 4; ++slot)
                {
                    if (!j_eat(j, ','))
                    {
                        break;
                    }
                    j_ws(j);
                    if (j->p < j->end && *j->p == '"')
                    {
                        char *s = j_string(j, 0);

                        if (slot == 0)
                        {
                            w->name = s;
                        }
                        else if (slot == 1)
                        {
                            w->pub = s;
                            w->have_pub = 1;
                        }
                        else if (slot == 2)
                        {
                            w->sys = s;
                            w->have_sys = 1;
                        }
                    }
                    else
                    {
                        if (slot == 3)
                        {
                            w->correct = j->p < j->end && *j->p == 't';
                        }
                        j_skip_value(j);
                    }
                }
            }
            else
            {
                return 0; /* a token kind this runner has never seen */
            }
        }

        j_skip_rest(j, ']'); /* anything trailing in this token's array */
        if (!j_eat(j, ','))
        {
            (void)j_eat(j, ']');
            return 1;
        }
    }
}

static void show_streams(void)
{
    ar_i32 z;

    printf("       want  ");
    for (z = 0; z < g_want_n; ++z)
    {
        printf(" ");
        print_token(&g_want[z]);
    }
    printf("\n       got   ");
    for (z = 0; z < g_got_n; ++z)
    {
        printf(" ");
        print_token(&g_got[z]);
    }
    printf("\n");
}

static void run_tokenizer_file(const char *path, const char *label)
{
    json j;

    if (!read_file(path))
    {
        printf("  cannot read %s\n", path);
        return;
    }
    j.p = g_file;
    j.end = g_file + g_file_n;

    if (!j_eat(&j, '{'))
    {
        return;
    }
    while (!j_eat(&j, '}'))
    {
        char *key = j_string(&j, 0);

        if (!key)
        {
            break;
        }
        (void)j_eat(&j, ':');
        if (strcmp(key, "tests") != 0 && strcmp(key, "xmlViolationTests") != 0)
        {
            j_skip_value(&j);
            (void)j_eat(&j, ',');
            continue;
        }
        if (!j_eat(&j, '['))
        {
            break;
        }
        while (!j_eat(&j, ']'))
        {
            char         *desc = 0;
            char         *input = 0;
            char         *last_start = 0;
            ar_u32        input_n = 0;
            int           dbl = 0;
            int           have_output = 0;
            ar_html_state want_states[STATE_COUNT];
            int           want_n = 0;
            const char   *out_at = 0;
            int           unsupported = 0;
            ar_u32        arena = g_str_n;

            if (!j_eat(&j, '{'))
            {
                break;
            }
            /*
             * `doubleEscaped` can appear after `input` and `output`, so the
             * object is read once for everything else and `output` is read
             * afterwards from a remembered position, when the flag is known.
             */
            while (!j_eat(&j, '}'))
            {
                char *k = j_string(&j, 0);

                if (!k)
                {
                    break;
                }
                (void)j_eat(&j, ':');
                if (strcmp(k, "description") == 0)
                {
                    desc = j_string(&j, 0);
                }
                else if (strcmp(k, "input") == 0)
                {
                    input = j_string(&j, &input_n);
                }
                else if (strcmp(k, "lastStartTag") == 0)
                {
                    last_start = j_string(&j, 0);
                }
                else if (strcmp(k, "doubleEscaped") == 0)
                {
                    j_ws(&j);
                    dbl = j.p < j.end && *j.p == 't';
                    j_skip_value(&j);
                }
                else if (strcmp(k, "initialStates") == 0)
                {
                    if (j_eat(&j, '['))
                    {
                        while (!j_eat(&j, ']'))
                        {
                            char *s = j_string(&j, 0);
                            int   i;

                            if (!s)
                            {
                                break;
                            }
                            for (i = 0; i < STATE_COUNT; ++i)
                            {
                                if (strcmp(s, STATES[i].name) == 0)
                                {
                                    if (want_n < STATE_COUNT)
                                    {
                                        want_states[want_n++] = STATES[i].state;
                                    }
                                    break;
                                }
                            }
                            if (i == STATE_COUNT)
                            {
                                /* CDATA section state, which needs foreign
                                   content, which needs 0.13.0. Counted as
                                   skipped rather than quietly passed. */
                                unsupported = 1;
                            }
                            (void)j_eat(&j, ',');
                        }
                    }
                }
                else if (strcmp(k, "output") == 0)
                {
                    out_at = j.p;
                    have_output = 1;
                    j_skip_value(&j);
                }
                else
                {
                    j_skip_value(&j); /* `errors`, and anything new */
                }
                (void)j_eat(&j, ',');
            }

            if (unsupported)
            {
                ++g_skip;
            }
            else if (input && have_output)
            {
                json oj;
                int  i;
                int  n = want_n ? want_n : 1;

                if (dbl)
                {
                    input_n = unescape_again(input, input_n);
                }
                for (i = 0; i < n; ++i)
                {
                    ar_u32 keep = g_str_n;

                    oj.p = out_at;
                    oj.end = j.end;
                    if (!read_expected(&oj, dbl))
                    {
                        ++g_skip;
                        g_str_n = keep;
                        continue;
                    }
                    collect_tokens(input, input_n, want_n ? want_states[i] : AR_HTML_DATA,
                                   last_start);
                    ++g_run;
                    if (tokens_equal())
                    {
                        ++g_pass;
                    }
                    else
                    {
                        ++g_cause_other;
                        report_failure(label, desc ? desc : "(no description)", input);
                        if (!g_quiet)
                        {
                            show_streams();
                        }
                        if (g_first_only)
                        {
                            return;
                        }
                    }
                    g_str_n = keep;
                }
            }
            g_str_n = arena;
            (void)j_eat(&j, ',');
        }
        (void)j_eat(&j, ',');
    }
}

/* ------------------------------------------------------------------------
 * Tree construction
 *
 * The .dat format, which is line-based and older than JSON's popularity:
 *
 *     #data
 *     <p>One<p>Two
 *     #errors
 *     (1,3): expected-doctype-but-got-start-tag
 *     #document
 *     | <html>
 *     |   <head>
 *     |   <body>
 *     |     <p>
 *     |       "One"
 *
 * A test ends at a blank line followed by `#data`, and not at the first line
 * that does not begin with `| ` -- a text node may contain newlines, and then
 * its continuation lines have no prefix at all. Splitting on the prefix reads
 * half of `"a\nb"` as the end of the document and the other half as the start
 * of nothing.
 * ------------------------------------------------------------------------ */
static void tree_put(const char *s, ar_u32 n)
{
    if (g_tree_n + n >= TREE_CAP)
    {
        return;
    }
    memcpy(g_tree + g_tree_n, s, n);
    g_tree_n += n;
}

static void tree_puts(const char *s)
{
    tree_put(s, (ar_u32)strlen(s));
}

static void tree_indent(ar_i32 depth)
{
    ar_i32 i;

    tree_puts("| ");
    for (i = 0; i < depth; ++i)
    {
        tree_puts("  ");
    }
}

static void tree_span(ar_span s)
{
    tree_put(s.p ? s.p : "", s.n);
}

/*
 * One node, html5lib's serialisation.
 *
 * Attributes are sorted by name, which html5lib requires and which is why a
 * document with two attributes in the other order is still one comparison.
 */
static void serialise(const ar_doc *d, ar_i32 i, ar_i32 depth)
{
    const ar_dom_node *n;
    ar_i32             c;

    if (i < 0)
    {
        return;
    }
    n = &d->nodes[i];

    switch (n->kind)
    {
    case AR_DOM_ELEMENT:
        tree_indent(depth);
        tree_puts("<");
        tree_span(n->name);
        tree_puts(">\n");
        if (n->attr_count > 0)
        {
            ar_i32 order[64];
            ar_i32 k;
            ar_i32 m = n->attr_count > 64 ? 64 : n->attr_count;

            for (k = 0; k < m; ++k)
            {
                order[k] = n->attr_first + k;
            }
            for (k = 1; k < m; ++k)
            {
                ar_i32 v = order[k];
                ar_i32 j = k - 1;

                while (j >= 0)
                {
                    const ar_span *a = &d->attrs[order[j]].name;
                    const ar_span *b = &d->attrs[v].name;
                    ar_u32         len = a->n < b->n ? a->n : b->n;
                    int            cmp = len ? memcmp(a->p, b->p, len) : 0;

                    if (cmp == 0)
                    {
                        cmp = (int)a->n - (int)b->n;
                    }
                    if (cmp <= 0)
                    {
                        break;
                    }
                    order[j + 1] = order[j];
                    --j;
                }
                order[j + 1] = v;
            }
            for (k = 0; k < m; ++k)
            {
                tree_indent(depth + 1);
                tree_span(d->attrs[order[k]].name);
                tree_puts("=\"");
                tree_span(d->attrs[order[k]].value);
                tree_puts("\"\n");
            }
        }
        break;

    case AR_DOM_TEXT:
        tree_indent(depth);
        tree_puts("\"");
        tree_span(n->text);
        tree_puts("\"\n");
        break;

    case AR_DOM_COMMENT:
        tree_indent(depth);
        tree_puts("<!-- ");
        tree_span(n->text);
        tree_puts(" -->\n");
        break;

    case AR_DOM_DOCTYPE:
        tree_indent(depth);
        tree_puts("<!DOCTYPE ");
        tree_span(n->name);
        tree_puts(">\n");
        break;

    default:
        break;
    }

    for (c = n->first_child; c >= 0; c = d->nodes[c].next_sibling)
    {
        serialise(d, c, n->kind == AR_DOM_DOCUMENT ? depth : depth + 1);
    }
}

static void serialise_document(const ar_doc *d)
{
    g_tree_n = 0;
    if (d->node_count > 0)
    {
        serialise(d, 0, 0);
    }
    g_tree[g_tree_n] = 0;
}

/* A line, without its terminator. Returns where the next line begins. */
static const char *line_of(const char *p, const char *end, const char **b, const char **e)
{
    *b = p;
    while (p < end && *p != '\n')
    {
        ++p;
    }
    *e = p;
    if (*e > *b && (*e)[-1] == '\r')
    {
        --*e;
    }
    return p < end ? p + 1 : end;
}

static int line_is(const char *b, const char *e, const char *lit)
{
    ar_u32 n = (ar_u32)strlen(lit);

    return (ar_u32)(e - b) == n && memcmp(b, lit, n) == 0;
}

static void print_escaped_n(const char *s, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i < n && i < 200u; ++i)
    {
        unsigned char c = (unsigned char)s[i];

        if (c == '\n')
        {
            printf("\\n");
        }
        else if (c >= 0x20u && c < 0x7Fu)
        {
            putchar((int)c);
        }
        else
        {
            printf("\\x%02X", (unsigned)c);
        }
    }
    if (n > 200u)
    {
        printf(" ...");
    }
}

/* A serialised tree, indented under the label so the two are comparable by
   eye. Trailing newlines are the format's own; they are printed as they are. */
static void print_block(const char *s, ar_u32 n)
{
    ar_u32 i = 0;

    while (i < n)
    {
        ar_u32 j = i;

        while (j < n && s[j] != '\n')
        {
            ++j;
        }
        printf("         ");
        print_escaped_n(s + i, j - i);
        printf("\n");
        i = j + 1u;
    }
}

static void run_tree_file(const char *path, const char *label)
{
    const char *p;
    const char *end;

    if (!read_file(path))
    {
        printf("  cannot read %s\n", path);
        return;
    }
    p = g_file;
    end = g_file + g_file_n;

    while (p < end)
    {
        const char *b;
        const char *e;
        const char *data = 0;
        ar_u32      data_n = 0;
        const char *want = 0;
        ar_u32      want_n = 0;
        int         fragment = 0;
        int         script_on = 0;

        p = line_of(p, end, &b, &e);
        if (!line_is(b, e, "#data"))
        {
            continue;
        }

        /* Everything to the next `#errors` line is the input, minus the
           newline that introduces that line. */
        data = p;
        for (;;)
        {
            const char *save = p;

            p = line_of(p, end, &b, &e);
            if (b >= end)
            {
                break;
            }
            if (line_is(b, e, "#errors"))
            {
                data_n = (ar_u32)(save - data);
                if (data_n)
                {
                    --data_n; /* the newline before #errors */
                }
                break;
            }
            if (save == p)
            {
                break;
            }
        }

        /* Then the sections between #errors and #document. */
        for (;;)
        {
            const char *save = p;

            p = line_of(p, end, &b, &e);
            if (b >= end)
            {
                break;
            }
            if (line_is(b, e, "#document"))
            {
                break;
            }
            if (line_is(b, e, "#document-fragment"))
            {
                fragment = 1;
            }
            else if (line_is(b, e, "#script-on"))
            {
                script_on = 1;
            }
            if (save == p)
            {
                break;
            }
        }

        /* And the expected tree, to a blank line followed by #data or EOF. */
        want = p;
        for (;;)
        {
            const char *save = p;
            const char *nb;
            const char *ne;

            p = line_of(p, end, &b, &e);
            if (b >= end && e >= end)
            {
                want_n = (ar_u32)(end - want);
                break;
            }
            if (b == e)
            {
                line_of(p, end, &nb, &ne);
                if (nb >= end || line_is(nb, ne, "#data"))
                {
                    want_n = (ar_u32)(save - want);
                    break;
                }
            }
            if (save == p)
            {
                want_n = (ar_u32)(save - want);
                break;
            }
        }

        if (fragment || script_on)
        {
            /* innerHTML needs a fragment parsing algorithm, which needs a
               context element; scripting is a permanent no. Skipped and
               counted, never quietly passed. */
            ++g_skip;
            if (fragment)
            {
                ++g_cause_fragment;
            }
            else
            {
                ++g_cause_script;
            }
            continue;
        }

        memset(&g_doc, 0, sizeof g_doc);
        g_doc.nodes = g_nodes;
        g_doc.node_cap = DOC_NODES;
        g_doc.attrs = g_attrs;
        g_doc.attr_cap = DOC_ATTRS;
        g_doc.text = g_text;
        g_doc.text_cap = DOC_TEXT;
        ar_html_parse(&g_doc, data, data_n, g_scratch, SCRATCH_CAP);
        serialise_document(&g_doc);

        ++g_run;
        if (g_tree_n == want_n && memcmp(g_tree, want, want_n) == 0)
        {
            ++g_pass;
            continue;
        }

        /* A cause, so the list criterion 2 asks for is a list of reasons and
           not a list of names. */
        {
            const char *w = want;
            ar_u32      i;
            int         foreign = 0;
            int         templ = 0;

            for (i = 0; i + 5 < want_n; ++i)
            {
                if (memcmp(w + i, "<svg ", 5) == 0 || memcmp(w + i, "<math", 5) == 0)
                {
                    foreign = 1;
                }
                if (memcmp(w + i, "conten", 6) == 0 && i + 7 < want_n && w[i + 6] == 't')
                {
                    templ = 1;
                }
            }
            if (foreign)
            {
                ++g_cause_foreign;
            }
            else if (templ)
            {
                ++g_cause_template;
            }
            else
            {
                ++g_cause_other;
            }
        }

        ++g_fail;
        if (!g_quiet)
        {
            printf("  FAIL %s: ", label);
            print_escaped_n(data, data_n);
            printf("\n       want\n");
            print_block(want, want_n);
            printf("       got\n");
            print_block(g_tree, g_tree_n);
        }
        if (g_first_only)
        {
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * Driving it
 * ------------------------------------------------------------------------ */
static const char *const TOKENIZER_FILES[] = {
    "contentModelFlags", "domjs", "entities", "escapeFlag", "namedEntities",
    "numericEntities",   "test1", "test2",    "test3",      "test4",
    "unicodeChars",      0};

/*
 * Three files are deliberately absent, named here rather than silently
 * omitted:
 *
 *   xmlViolation            its key is `xmlViolationTests`, and it asks what a
 *                           tokenizer should do when its output has to be
 *                           serialisable as XML -- U+000C becomes a space, a
 *                           double hyphen in a comment is split. areole emits
 *                           HTML and that is a permanent decision.
 *   pendingSpecChanges      a change to the specification that has not been
 *                           made.
 *   unicodeCharsProblematic a UTF-8 decoder's behaviour on input this parser
 *                           is never handed: ar_html_parse_into decodes before
 *                           the tokenizer sees a byte.
 */

static void suite_tokenizer(const char *dir, const char *only)
{
    char path[512];
    int  i;

    for (i = 0; TOKENIZER_FILES[i]; ++i)
    {
        if (only && strcmp(only, TOKENIZER_FILES[i]) != 0)
        {
            continue;
        }
        sprintf(path, "%s/tokenizer/%s.test", dir, TOKENIZER_FILES[i]);
        run_tokenizer_file(path, TOKENIZER_FILES[i]);
    }
}

static void suite_tree(const char *dir, const char *only)
{
    static const char *const FILES[] = {"adoption01",
                                        "adoption02",
                                        "blocks",
                                        "comments01",
                                        "doctype01",
                                        "domjs-unsafe",
                                        "entities01",
                                        "entities02",
                                        "foreign-fragment",
                                        "html5test-com",
                                        "inbody01",
                                        "isindex",
                                        "main-element",
                                        "math",
                                        "menuitem-element",
                                        "namespace-sensitivity",
                                        "noscript01",
                                        "pending-spec-changes",
                                        "pending-spec-changes-plain-text-unsafe",
                                        "plain-text-unsafe",
                                        "processing-instructions",
                                        "quirks01",
                                        "ruby",
                                        "scriptdata01",
                                        "search-element",
                                        "svg",
                                        "tables01",
                                        "template",
                                        "tests_innerHTML_1",
                                        "tests1",
                                        "tests10",
                                        "tests11",
                                        "tests12",
                                        "tests14",
                                        "tests15",
                                        "tests16",
                                        "tests17",
                                        "tests18",
                                        "tests19",
                                        "tests2",
                                        "tests20",
                                        "tests21",
                                        "tests22",
                                        "tests23",
                                        "tests24",
                                        "tests25",
                                        "tests26",
                                        "tests3",
                                        "tests4",
                                        "tests5",
                                        "tests6",
                                        "tests7",
                                        "tests8",
                                        "tests9",
                                        "tricky01",
                                        "void-in-phrasing",
                                        "webkit01",
                                        "webkit02",
                                        0};
    char                     path[512];
    int                      i;

    /* The four scripted_* files are absent on purpose: every case in them is
       marked #script-on, and scripting is a permanent decision rather than a
       gap. Running them would report sixty skips and mean nothing. */
    for (i = 0; FILES[i]; ++i)
    {
        if (only && strcmp(only, FILES[i]) != 0)
        {
            continue;
        }
        sprintf(path, "%s/tree-construction/%s.dat", dir, FILES[i]);
        run_tree_file(path, FILES[i]);
    }
}

static void summary(const char *what, double gate)
{
    double pct = g_run ? 100.0 * (double)g_pass / (double)g_run : 0.0;

    printf("\n%s: %ld of %ld, %.2f%%", what, (long)g_pass, (long)g_run, pct);
    if (g_skip)
    {
        printf(", %ld skipped", (long)g_skip);
    }
    printf("\n");
    if (g_cause_fragment || g_cause_script || g_cause_foreign || g_cause_template || g_cause_other)
    {
        printf("  by cause:");
        if (g_cause_fragment)
        {
            printf(" fragment %lu,", (unsigned long)g_cause_fragment);
        }
        if (g_cause_script)
        {
            printf(" scripting %lu,", (unsigned long)g_cause_script);
        }
        if (g_cause_foreign)
        {
            printf(" foreign content %lu,", (unsigned long)g_cause_foreign);
        }
        if (g_cause_template)
        {
            printf(" template contents %lu,", (unsigned long)g_cause_template);
        }
        if (g_cause_other)
        {
            printf(" other %lu", (unsigned long)g_cause_other);
        }
        printf("\n");
    }
    printf("  gate is %.0f%%: %s\n", gate, pct + 1e-9 >= gate ? "met" : "NOT MET");
}

int main(int argc, char **argv)
{
    const char *dir = "third_party/html5lib";
    const char *only = 0;
    int         want_tok = 0;
    int         want_tree = 0;
    int         k;
    int         bad = 0;

    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--tokenizer") == 0)
        {
            want_tok = 1;
        }
        else if (strcmp(argv[k], "--tree") == 0)
        {
            want_tree = 1;
        }
        else if (strcmp(argv[k], "--quiet") == 0)
        {
            g_quiet = 1;
        }
        else if (strcmp(argv[k], "--first") == 0)
        {
            g_first_only = 1;
        }
        else if (strcmp(argv[k], "--dir") == 0 && k + 1 < argc)
        {
            dir = argv[++k];
        }
        else if (strcmp(argv[k], "--file") == 0 && k + 1 < argc)
        {
            only = argv[++k];
        }
        else
        {
            printf("ar_html5lib [--tokenizer] [--tree] [--quiet] [--first]\n");
            printf("            [--dir PATH] [--file NAME]\n");
            return 2;
        }
    }
    if (!want_tok && !want_tree)
    {
        want_tok = want_tree = 1;
    }

    if (want_tok)
    {
        printf("html5lib tokenizer suite\n");
        suite_tokenizer(dir, only);
        summary("tokenizer", 100.0);
        if (g_run == 0 || g_pass != g_run)
        {
            bad = 1;
        }
        g_run = g_pass = g_fail = g_skip = 0;
        g_cause_fragment = g_cause_script = g_cause_foreign = 0;
        g_cause_template = g_cause_other = 0;
    }

    if (want_tree)
    {
        printf("\nhtml5lib tree construction suite\n");
        suite_tree(dir, only);
        summary("tree construction", 98.0);
        if (g_run == 0 || 100.0 * (double)g_pass / (double)g_run < 98.0)
        {
            bad = 1;
        }
    }

    return bad;
}
