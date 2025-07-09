/*
 * areole - HTML tokens, and the tokenizer that produces them.
 * SPDX-License-Identifier: MIT
 *
 * **Not installed, and deliberately so.** The document half of the parser --
 * ar_doc, ar_dom_node, ar_html_parse, ar_dom_build, the encoding functions --
 * is public and lives in include/areole.h. What is left here is the tokenizer:
 * thirty states and a token struct, of no use to somebody who wants a document
 * laid out, and a header is a promise that everything in it keeps working.
 *
 * A caller that genuinely wants tokens can include this. It is not promised,
 * and it will change when the tokenizer does.
 *
 * ------------------------------------------------------------------------
 * Three decisions, taken here because everything below depends on them
 *
 * **It pulls, it does not push.** `ar_html_next` returns one token. A callback
 * would mean the tree builder's state lives in a closure, and there are no
 * closures in C89 -- it would live in a struct passed as `void *`, which is
 * the same thing with the types thrown away.
 *
 * **It does not allocate and it does not copy.** A token's name and text are
 * spans into the caller's own bytes. Nothing here owns anything, which is what
 * lets the whole parser run inside the arena the caller already handed over.
 *
 * The exception is a value containing a character reference: `&amp;` is five
 * bytes in and one byte out, so the result is not a span of the input any
 * more. Those decode into a scratch buffer the caller supplies, and the span
 * points there instead. A document with no entities never touches it.
 *
 * **Character tokens are runs, not characters.** The specification emits one
 * token per character and every implementation coalesces them, because the
 * tree builder wants a run anyway. A run ends where a tag, a reference or the
 * input does.
 *
 * ------------------------------------------------------------------------
 * What "the specification, literally" means here
 *
 * The state names in ar_html_token.c are the specification's own, spelled the
 * same way, so a reader with the document open can find the paragraph. Where
 * this tokenizer departs -- the coalesced runs above, and the named-reference
 * table below -- it says so at the point of departure rather than in a list
 * somewhere else.
 */
#ifndef AR_HTML_H
#define AR_HTML_H

#include "ar_internal.h"

/* ar_span and ar_attr are in include/areole.h: a document is made of them, so
   they are part of the promise. A token borrows the same two. */

typedef enum ar_tok_kind
{
    AR_TOK_EOF = 0,
    AR_TOK_TEXT,
    AR_TOK_START,
    AR_TOK_END,
    AR_TOK_COMMENT,
    AR_TOK_DOCTYPE
} ar_tok_kind;

/*
 * How many attributes one tag may carry.
 *
 * ponytail: a fixed array on the token. Real markup does not reach sixteen on
 * one element -- the largest in the HTML specification's own examples is
 * eleven -- and the seventeenth is dropped with `attrs_dropped` set rather
 * than overrunning. The upgrade is an arena span, and it is worth doing when
 * something real hits the cap; the counter is there so that is a fact rather
 * than a guess.
 */
#define AR_HTML_MAX_ATTRS 16

typedef struct ar_token
{
    ar_tok_kind kind;

    /* Tag or doctype name, lowercased for tags. Empty for text and comments. */
    ar_span name;

    /* Text for AR_TOK_TEXT, the body for AR_TOK_COMMENT, and for a doctype the
       public and system identifiers land in `pub` and `sys`. */
    ar_span text;
    ar_span pub;
    ar_span sys;

    ar_attr attrs[AR_HTML_MAX_ATTRS];
    ar_i32  attr_count;
    ar_i32  attrs_dropped;

    int self_closing;

    /* The doctype quirks flags, which decide the box model for the whole
       document. A doctype that is missing, malformed, or one of the legacy
       strings forces quirks, and quirks is not a curiosity: it changes the box
       model, table cell inheritance and line height. */
    int force_quirks;
} ar_token;

/*
 * The states the caller can put the tokenizer into.
 *
 * The tokenizer cannot work these out for itself: whether `<title>a<b>c` holds
 * a `<b>` element or the literal text `<b>` is decided by the *tree builder*,
 * which knows it is inside a title. So the tree builder switches the state,
 * exactly as the specification says, and that is why these are public.
 */
typedef enum ar_html_state
{
    AR_HTML_DATA = 0,
    AR_HTML_RCDATA,   /* title, textarea: entities yes, tags no */
    AR_HTML_RAWTEXT,  /* style, xmp, iframe, noembed: neither */
    AR_HTML_SCRIPT,   /* script: rawtext with the escaped states */
    AR_HTML_PLAINTEXT /* everything to the end of the file, literally */
} ar_html_state;

typedef struct ar_html_tok
{
    const char *p;
    const char *end;

    ar_html_state state;

    /* Where a decoded character reference goes. May be null, in which case a
       reference is passed through as the literal bytes that spell it -- which
       is what a caller that only renders its own markup wants, and is why the
       buffer is the caller's business rather than this file's. */
    char  *scratch;
    ar_u32 scratch_cap;
    ar_u32 scratch_used;

    /*
     * The tag name most recently emitted as a start tag, so RCDATA and RAWTEXT
     * can apply the specification's `appropriate end tag name` rule: inside
     * `<title>`, only `</title>` closes it and `</b>` is text.
     */
    char   last_start[32];
    ar_u32 last_start_n;

    /* Parse errors are counted, never fatal. The specification defines a
       recovery for every one of them, and a tokenizer that stops is a
       tokenizer that disagrees with every browser. */
    ar_u32 errors;

    /*
     * Set when a decoded character reference did not fit in `scratch`.
     *
     * Distinct from `errors`, which counts things wrong with the *document*.
     * This is something wrong with the *budget*, and it is the difference
     * between "the author wrote bad markup" and "you did not give me enough
     * room to hold what the author wrote". The document reports it as
     * `overflowed`, which is 0.9.0 acceptance criterion 7.
     */
    int scratch_full;
} ar_html_tok;

/*
 * Start tokenizing `bytes`. `scratch` may be null; see the struct.
 *
 * The input is not copied and must outlive every token produced from it.
 */
void ar_html_tok_init(ar_html_tok *t, const char *bytes, ar_u32 len, char *scratch,
                      ar_u32 scratch_cap);

/*
 * The next token. Returns 0 at end of input, having set `out->kind` to
 * AR_TOK_EOF, and non-zero otherwise.
 */
int ar_html_next(ar_html_tok *t, ar_token *out);

/*
 * The longest named character reference that is a prefix of these bytes.
 *
 * Returns how many bytes it used -- including the semicolon, when the name it
 * matched has one -- or 0 for no match. Writes the code points to out[0..1]
 * and their number, one or two, to *count.
 *
 * A longest match rather than a lookup, and the difference is not academic
 * once the semicolon-less names are in the table: `&notit;` is `&not` and then
 * the literal text `it;`, because `not` is a reference and `notit` is not.
 * The caller hands over everything after the ampersand and is told how much
 * was eaten.
 */
ar_u32 ar_html_entity_match(const char *p, ar_u32 avail, ar_u32 *out, ar_i32 *count);

/* The table, walked rather than read, so ar_test can check it is sorted --
 * a binary search over an unsorted table does not fail loudly. The names are
 * not NUL-terminated in the table, so this copies; it returns the length, or
 * 0 if `i` is out of range or `cap` is too small. */
ar_i32 ar_html_entity_count(void);
ar_u32 ar_html_entity_name(ar_i32 i, char *buf, ar_u32 cap);

/* The user-agent stylesheet is several strings, because C89 caps one literal
   at 509 characters. These are for the check that every part parses;
   ar_ua_stylesheet itself is public. */
ar_i32      ar_ua_stylesheet_parts(void);
const char *ar_ua_stylesheet_part(ar_i32 i);

#endif
