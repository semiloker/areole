/*
 * areole - HTML tokens, and the tokenizer that produces them.
 * SPDX-License-Identifier: MIT
 *
 * Not installed. This is the front half of 0.9.0; the tree builder that turns
 * these tokens into a document is the other half and does not exist yet.
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

/* A run of bytes somewhere. Points into the caller's input, or into the
   scratch buffer when a character reference had to be decoded. */
typedef struct ar_span
{
    const char *p;
    ar_u32      n;
} ar_span;

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

typedef struct ar_attr
{
    ar_span name;
    ar_span value;
} ar_attr;

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

/* Case-insensitive ASCII comparison of a span against a literal. Exposed
   because the tree builder asks it of every tag name it sees. */
int ar_span_is(ar_span s, const char *lit);

/*
 * A named character reference, or 0 if the name is not one.
 *
 * Returns the code point and consumes nothing; the caller has already found
 * the name. See the table in ar_html_entity.c for what is and is not in it.
 */
ar_u32 ar_html_entity(const char *name, ar_u32 n);

/* The table, walked rather than read, so ar_test can check it is sorted --
 * a binary search over an unsorted table does not fail loudly. */
ar_i32      ar_html_entity_count(void);
const char *ar_html_entity_name(ar_i32 i);

/* ------------------------------------------------------------------------
 * The document
 *
 * A tree, not an API. Elements with attributes, text, comments and a doctype.
 * Index-referenced rather than pointer-linked, for the reasons the box tree
 * gives at length in ar_node.h: indices survive the array moving, they halve
 * the size of the links on a 64 bit target, and a flat array is what makes a
 * walk a linear sweep.
 *
 * There is no scripting interface. There is nothing to script with.
 *
 * ------------------------------------------------------------------------
 * Who owns the text
 *
 * The tokenizer owns nothing: its spans point into the caller's bytes, or into
 * a scratch buffer it reuses for every token. The second kind cannot be kept,
 * so the document copies exactly those into storage of its own and leaves the
 * rest pointing at the input.
 *
 * That is not an optimisation for its own sake. A document of ordinary prose
 * contains almost no character references, so almost nothing is copied, and
 * the copy that does happen is bounded by the entities actually present rather
 * than by the size of the document.
 * ------------------------------------------------------------------------ */
typedef enum ar_dom_kind
{
    AR_DOM_DOCUMENT = 0,
    AR_DOM_ELEMENT,
    AR_DOM_TEXT,
    AR_DOM_COMMENT,
    AR_DOM_DOCTYPE
} ar_dom_kind;

/*
 * The three quirks modes, selected from the doctype by the specification's own
 * table.
 *
 * Quirks is not a curiosity. It changes the box model to content-box-plus-
 * padding, changes table cell inheritance and changes line height. A document
 * with no doctype has to render the way a browser renders it or the engine is
 * wrong about a large fraction of the web.
 */
typedef enum ar_quirks
{
    AR_QUIRKS_NO = 0,
    AR_QUIRKS_LIMITED,
    AR_QUIRKS_YES
} ar_quirks;

typedef struct ar_dom_node
{
    ar_dom_kind kind;

    ar_span name; /* element tag name, or the doctype's name */
    ar_span text; /* text data, or a comment's body */

    ar_i32 parent;
    ar_i32 first_child;
    ar_i32 last_child;
    ar_i32 next_sibling;
    ar_i32 prev_sibling;

    ar_i32 attr_first; /* into ar_doc.attrs, or -1 */
    ar_i32 attr_count;
} ar_dom_node;

typedef struct ar_doc
{
    ar_dom_node *nodes;
    ar_i32       node_cap;
    ar_i32       node_count;

    ar_attr *attrs;
    ar_i32   attr_cap;
    ar_i32   attr_count;

    /* Where text that could not stay a span of the input goes. */
    char  *text;
    ar_u32 text_cap;
    ar_u32 text_used;

    ar_quirks quirks;
    ar_u32    errors;

    /* Set when any of the three ran out. A document larger than the budget
       fails cleanly and says so rather than truncating silently, which is
       0.9.0 acceptance criterion 7. */
    int overflowed;
} ar_doc;

/*
 * Parse `bytes` into `doc`, which the caller has pointed at storage of its
 * own. Returns non-zero if the whole document was built; zero if anything
 * overflowed, in which case `doc->overflowed` says so and the tree holds as
 * much as fitted.
 *
 * The input is not copied and must outlive the document.
 */
int ar_html_parse(ar_doc *doc, const char *bytes, ar_u32 len, char *scratch, ar_u32 scratch_cap);

/* The root element of the document, or -1. Almost always <html>. */
ar_i32 ar_dom_root(const ar_doc *doc);

/* First child of `i` that is an element with this tag, or -1. The tree builder
   does not need this; the tests and the box-tree bridge do. */
ar_i32 ar_dom_child_element(const ar_doc *doc, ar_i32 i, const char *tag);

#endif
