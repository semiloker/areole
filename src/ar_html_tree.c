/*
 * areole - HTML tree construction.
 * SPDX-License-Identifier: MIT
 *
 * §13.2.6. The tokenizer says what the bytes are; this says what tree they
 * mean, and the two are separate because the answer is not "nest the tags".
 *
 * ------------------------------------------------------------------------
 * Why this is a specification and not a heuristic
 *
 * `<p>a<p>b` is two paragraphs, not one nested in another. `<table><em>x`
 * puts the emphasis *before* the table. `<b><i></b></i>` produces the same
 * tree in every browser on earth, and that tree is not the one the markup
 * appears to describe.
 *
 * None of those are edge cases. They are what two decades of hand-written
 * markup looks like, and the specification exists because browsers had to
 * agree on them. An implementation that is not the specification will disagree
 * with every browser on some real document, and the disagreement will be
 * silent -- the page will simply look wrong, with nothing to point at.
 *
 * So the three algorithms that make this a specification rather than a guess
 * are all here, with their step numbers:
 *
 *   - **Foster parenting**, which relocates content that appears inside a
 *     table where it may not, to just before the table.
 *   - **The adoption agency algorithm**, which reconstructs correct nesting
 *     from misnested formatting elements.
 *   - **The list of active formatting elements** and its reconstruction, which
 *     is what carries `<b>` across a block boundary and reopens it.
 *
 * ------------------------------------------------------------------------
 * What is not here yet, named rather than discovered
 *
 * Templates, frameset, foreign content (SVG and MathML), fragment parsing, and
 * the `in select` modes. Each is a named insertion mode in the specification
 * and each is absent; a token that would need one is handled by the mode that
 * would otherwise apply, which is wrong and is not silent -- `doc->errors`
 * counts it.
 */
#include "ar_html.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Bounds
 *
 * ponytail: the open-element stack and the active formatting list are fixed
 * arrays. Sixty-four is deeper than any real document nests -- the HTML
 * specification's own rendering section reaches eleven -- and the adoption
 * agency's outer loop is capped at eight by the specification itself. Past the
 * cap the token is dropped with an error counted rather than the array
 * overrun. The upgrade is an arena span, the same as everything else here.
 * ------------------------------------------------------------------------ */
#define AR_HTML_STACK 64
#define AR_HTML_FMT   24

typedef struct ar__tree
{
    ar_doc      *doc;
    ar_html_tok *tok;

    /* The stack of open elements. Index 0 is the document. */
    ar_i32 open[AR_HTML_STACK];
    ar_i32 open_n;

    /*
     * The stack of template insertion modes, 13.2.4.4.
     *
     * A template's mode cannot be recovered from the shape of the open-element
     * stack the way every other mode can, because it depends on what has been
     * *seen inside it* rather than on what encloses it: two templates sitting
     * in identical positions are in `in row` and `in body` according to
     * whether a `<tr>` went past. So it is the one mode that has to be
     * remembered, and remembered per template rather than once.
     *
     * One value was not enough. `<template><tr></tr><template></template>`
     * closes the inner template and has to resume the outer one's `in row`;
     * with a single value the inner `</template>` overwrote it.
     */
    ar_u8  tmpl_mode[AR_HTML_STACK];
    ar_i32 tmpl_mode_n;

    /*
     * "Enable foster parenting", 13.2.6.4.9.
     *
     * The specification turns foster parenting on around the whole of "process
     * the token using the rules for the in body insertion mode", not around
     * one insertion. Passing it per call is the same thing right up until `in
     * body` hands the token on again, and it does that constantly: `</p>` with
     * no p in scope inserts one through a path that never saw the flag, and
     * `<title>` goes to `in head`, which has a flag of its own set to zero. A
     * flag the insertion reads gets both, because every layer below inherits
     * it without having to be told.
     *
     * Saved and restored rather than set and cleared, because in body can
     * re-enter a table mode and these then nest.
     */
    int foster;

    /*
     * The list of active formatting elements.
     *
     * A marker is -1 and is pushed by a table cell or a caption, so
     * reconstruction cannot reach past one -- which is what stops a `<b>` in
     * one cell reopening itself in the next.
     */
    ar_i32 fmt[AR_HTML_FMT];
    ar_i32 fmt_n;

    ar_i32 head; /* the <head> element, remembered for `after head` */
    ar_i32 form; /* the form element pointer, for the same reason */
    int    mode;
    int    original_mode; /* what `text` mode returns to */

    int frameset_ok;

    /* Set by `<pre>`, `<listing>` and `<textarea>`; consumed by the very next
       token in the parse loop, which is where "the next token" is. */
    int drop_lf;

    /*
     * Fragment parsing: the element this markup is being parsed as if it were
     * inside. Null for an ordinary document.
     *
     * It is not in the tree and never becomes a node. It exists to answer two
     * questions -- what insertion mode to start in, and what the *adjusted
     * current node* is while the stack holds only the synthetic root -- and
     * the specification treats it exactly that way.
     */
    const char *ctx;
    ar_ns       ctx_ns;

    /*
     * How deep the current token is in reprocessing.
     *
     * A mode that cannot handle a token switches mode and reprocesses it, and
     * two modes can hand the same token back and forth forever. It has
     * happened twice: foreign content and the insertion-mode dispatcher in a
     * fragment, and `after head` routing head content to `in head` for a tag
     * `in head` did not know.
     *
     * Neither was catchable by the tokenizer-progress backstop in
     * ar_html_parse, because no token is consumed and that check never runs --
     * the loop is inside processing *one* token. This is the net for that
     * shape, and it costs one increment.
     */
    ar_i32 depth;
} ar__tree;

enum
{
    M_INITIAL = 0,
    M_BEFORE_HTML,
    M_BEFORE_HEAD,
    M_IN_HEAD,
    M_AFTER_HEAD,
    M_IN_BODY,
    M_TEXT,
    M_IN_TABLE,
    M_IN_TABLE_TEXT,
    M_IN_CAPTION,
    M_IN_COLUMN_GROUP,
    M_IN_TABLE_BODY,
    M_IN_ROW,
    M_IN_CELL,
    M_AFTER_BODY,
    M_AFTER_AFTER_BODY,
    M_IN_HEAD_NOSCRIPT,
    M_IN_SELECT,
    M_IN_SELECT_IN_TABLE,
    M_IN_TEMPLATE,
    M_IN_FRAMESET,

    /*
     * After the outermost `</frameset>`.
     *
     * A mode of its own because a frameset document is not finished when its
     * frameset closes: whitespace and comments after it belong to the html
     * element, and `<noframes>` is still allowed. Without it all of that was
     * a parse error and dropped -- twenty conformance cases whose whole point
     * is that the text survives.
     */
    M_AFTER_FRAMESET,

    /*
     * After `</html>` in a frameset document.
     *
     * It differs from `after frameset` in one thing only -- a comment goes on
     * the *document*, beside the html element rather than inside it -- and
     * that one thing is four conformance cases.
     */
    M_AFTER_AFTER_FRAMESET
};

/* ------------------------------------------------------------------------
 * The document
 * ------------------------------------------------------------------------ */
static int ar__in_scratch(const ar__tree *t, const char *p)
{
    return t->tok->scratch && p >= t->tok->scratch && p < t->tok->scratch + t->tok->scratch_cap;
}

/*
 * Keep a span. Text that points into the caller's input already outlives the
 * document and is left alone; text the tokenizer decoded into its scratch is
 * about to be overwritten by the next token and is copied.
 */
static ar_span ar__keep(ar__tree *t, ar_span s)
{
    ar_span out = s;

    if (s.n == 0 || !ar__in_scratch(t, s.p))
    {
        return out;
    }
    if (t->doc->text_used + s.n > t->doc->text_cap)
    {
        t->doc->overflowed = 1;
        out.n = 0;
        return out;
    }
    memcpy(t->doc->text + t->doc->text_used, s.p, s.n);
    out.p = t->doc->text + t->doc->text_used;
    t->doc->text_used += s.n;
    return out;
}

static ar_i32 ar__node(ar__tree *t, ar_dom_kind kind)
{
    ar_doc      *d = t->doc;
    ar_dom_node *n;

    if (d->node_count >= d->node_cap)
    {
        d->overflowed = 1;
        return -1;
    }
    n = &d->nodes[d->node_count];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->parent = -1;
    n->first_child = -1;
    n->last_child = -1;
    n->next_sibling = -1;
    n->prev_sibling = -1;
    n->attr_first = -1;
    return d->node_count++;
}

static void   ar__detach(ar__tree *t, ar_i32 i);
static ar_i32 ar__content_of(const ar__tree *t, ar_i32 node);

/*
 * Would putting `child` under `parent` make a loop?
 *
 * A node cannot go inside its own descendant. The DOM calls the refusal a
 * HierarchyRequestError; here the consequence of not asking is a `parent`
 * link that points at the node itself and a tree walk that never returns.
 *
 * The check is a walk up the ancestors, which is not free, so it is asked only
 * when `child` has children at all. A node that was created a moment ago has
 * none -- which is every insertion in an ordinary document -- so the common
 * path pays one comparison and the walk happens only when a subtree is being
 * moved. That is the adoption agency and foster parenting, and those are
 * exactly the two that can produce a cycle.
 *
 * The depth is bounded by the tree, and the loop counts its own steps rather
 * than trusting it: this runs on a tree that may already be malformed, and a
 * cycle detector that can itself hang is not one.
 */
static int ar__would_loop(const ar__tree *t, ar_i32 parent, ar_i32 child)
{
    const ar_doc *d = t->doc;
    ar_i32        up;
    ar_i32        steps = 0;

    if (child < 0 || d->nodes[child].first_child < 0)
    {
        return 0;
    }
    for (up = parent; up >= 0; up = d->nodes[up].parent)
    {
        if (up == child)
        {
            return 1;
        }
        if (++steps > d->node_count)
        {
            return 1; /* already looped; refusing is the only safe answer */
        }
    }
    return 0;
}

/*
 * Insertion is a move.
 *
 * Both of the functions below take a node out of wherever it is before they
 * put it where it is going, because that is what every caller means and
 * because the alternative is not a wrong tree, it is a *corrupt* one: append a
 * node that is already its parent's last child and the first thing the code
 * does is set its own previous sibling to itself. The sibling walk that
 * follows never terminates.
 *
 * The specification says this in a clause everybody skims -- "if node has a
 * parent, remove it from its parent" -- and the callers here mostly did it by
 * hand, which is how one of them came not to. ar_fuzz found the survivor at
 * iteration 8955409 of seed 3, in a document with a hundred and eighteen bytes
 * of nested formatting elements and no room at all for text.
 */
static void ar__append(ar__tree *t, ar_i32 parent, ar_i32 child)
{
    ar_doc *d = t->doc;

    /*
     * A node is never its own parent.
     *
     * A tree invariant rather than a special case, and checked here because
     * here is the only place a parent link is made. The bug that put it here
     * reached this function with parent == child == the paragraph in
     * `<table><em><p>x</em`; the cycle it made was invisible until something
     * walked the tree, and then that walk never came back.
     *
     * The insertion point below is the real fix. This is the invariant, stated
     * where it can be enforced.
     */
    if (parent < 0 || child < 0 || parent == child || ar__would_loop(t, parent, child))
    {
        return;
    }
    if (d->nodes[child].parent >= 0)
    {
        ar__detach(t, child);
    }
    d->nodes[child].parent = parent;
    d->nodes[child].prev_sibling = d->nodes[parent].last_child;
    d->nodes[child].next_sibling = -1;
    if (d->nodes[parent].last_child >= 0)
    {
        d->nodes[d->nodes[parent].last_child].next_sibling = child;
    }
    else
    {
        d->nodes[parent].first_child = child;
    }
    d->nodes[parent].last_child = child;
}

static void ar__detach(ar__tree *t, ar_i32 i)
{
    ar_doc *d = t->doc;
    ar_i32  p;

    if (i < 0 || d->nodes[i].parent < 0)
    {
        return;
    }
    p = d->nodes[i].parent;
    if (d->nodes[i].prev_sibling >= 0)
    {
        d->nodes[d->nodes[i].prev_sibling].next_sibling = d->nodes[i].next_sibling;
    }
    else
    {
        d->nodes[p].first_child = d->nodes[i].next_sibling;
    }
    if (d->nodes[i].next_sibling >= 0)
    {
        d->nodes[d->nodes[i].next_sibling].prev_sibling = d->nodes[i].prev_sibling;
    }
    else
    {
        d->nodes[p].last_child = d->nodes[i].prev_sibling;
    }
    d->nodes[i].parent = -1;
    d->nodes[i].prev_sibling = -1;
    d->nodes[i].next_sibling = -1;
}

/* Insert `child` immediately before `before` in `before`'s parent. */
static void ar__insert_before(ar__tree *t, ar_i32 before, ar_i32 child)
{
    ar_doc *d = t->doc;
    ar_i32  p;

    if (before < 0 || child < 0 || before == child || d->nodes[before].parent < 0 ||
        ar__would_loop(t, d->nodes[before].parent, child))
    {
        return;
    }
    if (d->nodes[child].parent >= 0)
    {
        ar__detach(t, child);
    }
    /* Re-read after the detach: taking the child out can have changed which
       node comes before this one. */
    if (d->nodes[before].parent < 0)
    {
        return;
    }
    p = d->nodes[before].parent;
    d->nodes[child].parent = p;
    d->nodes[child].next_sibling = before;
    d->nodes[child].prev_sibling = d->nodes[before].prev_sibling;
    if (d->nodes[before].prev_sibling >= 0)
    {
        d->nodes[d->nodes[before].prev_sibling].next_sibling = child;
    }
    else
    {
        d->nodes[p].first_child = child;
    }
    d->nodes[before].prev_sibling = child;
}

/* ------------------------------------------------------------------------
 * The stack of open elements
 * ------------------------------------------------------------------------ */
static ar_i32 ar__current(const ar__tree *t)
{
    return t->open_n > 0 ? t->open[t->open_n - 1] : -1;
}

static int ar__h_lower_c(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/*
 * Is this name one of a list of literals?
 *
 * The screen on the first byte is the whole point. `ar_span_is` walks both
 * strings, so a thirty-four entry list costs thirty-four walks, and these
 * lists are asked about every tag in the document -- `ar__closes_p` for every
 * start tag *and*, since the block end tags got their own rule, every end tag
 * as well.
 *
 * Comparing one folded byte first rejects nineteen entries in twenty before
 * anything is walked. Measured, not assumed: the foreign content release cost
 * the parse 26%, an alternating build said so, and this is where it went.
 */
static int ar__node_name_in(const ar__tree *t, ar_i32 node, const char *const *list);

static int ar__name_in(ar_span name, const char *const *list)
{
    int    first;
    ar_i32 i;

    if (name.n == 0)
    {
        return 0;
    }
    first = ar__h_lower_c((unsigned char)name.p[0]);
    for (i = 0; list[i]; ++i)
    {
        if (list[i][0] == first && ar_span_is(name, list[i]))
        {
            return 1;
        }
    }
    return 0;
}

static int ar__node_name_in(const ar__tree *t, ar_i32 node, const char *const *list)
{
    if (node < 0 || t->doc->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return 0;
    }
    return ar__name_in(t->doc->nodes[node].name, list);
}

/* Two spans, case-insensitively. ar_span_is compares against a literal; this
   is the same question when both sides came out of the document. */
static int ar__span_eq(ar_span a, ar_span b)
{
    ar_u32 i;

    if (a.n != b.n)
    {
        return 0;
    }
    for (i = 0; i < a.n; ++i)
    {
        char x = a.p[i];
        char y = b.p[i];

        if (x >= 'A' && x <= 'Z')
        {
            x = (char)(x + 32);
        }
        if (y >= 'A' && y <= 'Z')
        {
            y = (char)(y + 32);
        }
        if (x != y)
        {
            return 0;
        }
    }
    return 1;
}

static int ar__is(const ar__tree *t, ar_i32 node, const char *tag)
{
    if (node < 0 || t->doc->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return 0;
    }
    return ar_span_is(t->doc->nodes[node].name, tag);
}

static void ar__push(ar__tree *t, ar_i32 node)
{
    if (t->open_n >= AR_HTML_STACK)
    {
        t->doc->errors++;
        return;
    }
    t->open[t->open_n++] = node;
}

/*
 * The stack of template insertion modes, 13.2.4.4.
 *
 * Pushed when a template element is inserted and popped when one is popped,
 * which is why the popping lives in ar__pop rather than beside `</template>`:
 * a template also comes off the stack at end of file, inside ar__pop_until for
 * an enclosing tag, and on the "stop parsing" path, and a mode stack that
 * tracked only the explicit close would drift a little further out of step
 * with every unclosed template in a document.
 */
static void ar__tmpl_push(ar__tree *t, int mode)
{
    if (t->tmpl_mode_n < AR_HTML_STACK)
    {
        t->tmpl_mode[t->tmpl_mode_n++] = (ar_u8)mode;
    }
}

/* Replace the current template insertion mode, which is what every rule in
   `in template` does: pop one off, push the mode it is switching to. */
static void ar__tmpl_set(ar__tree *t, int mode)
{
    if (t->tmpl_mode_n > 0)
    {
        t->tmpl_mode[t->tmpl_mode_n - 1] = (ar_u8)mode;
    }
}

static void ar__pop(ar__tree *t)
{
    if (t->open_n > 0)
    {
        --t->open_n;
        if (t->doc->nodes[t->open[t->open_n]].ns == AR_NS_HTML &&
            ar__is(t, t->open[t->open_n], "template") && t->tmpl_mode_n > 0)
        {
            t->tmpl_mode_n--;
        }
    }
}

/*
 * "Has an element in scope", §13.2.4.2.
 *
 * The walk stops at a scoping element -- a table cell, a caption, a table
 * itself -- which is what makes `</p>` inside a cell close only that cell's
 * paragraph and not one outside the table.
 */
static int ar__in_scope(const ar__tree *t, const char *tag, int button_scope)
{
    static const char *const STOP[] = {"applet", "caption", "html",   "table", "td",
                                       "th",     "marquee", "object", 0};
    ar_i32                   i;
    int                      tag0 = ar__h_lower_c((unsigned char)tag[0]);

    for (i = t->open_n - 1; i >= 1; --i)
    {
        if (t->doc->nodes[t->open[i]].ns == AR_NS_HTML && t->doc->nodes[t->open[i]].name.n &&
            ar__h_lower_c((unsigned char)t->doc->nodes[t->open[i]].name.p[0]) == tag0 &&
            ar__is(t, t->open[i], tag))
        {
            return 1;
        }

        /*
         * An integration point is a wall for scope as well as for insertion.
         *
         * `<p><svg><desc><p>` is two paragraphs nested, not two siblings: the
         * inner `<p>` asks whether a `p` is in button scope, and the walk stops
         * at the `<desc>` before it can find the outer one. Without this the
         * outer paragraph is closed and the inner one ends up outside the SVG
         * entirely, which is a visibly different document.
         */
        if (t->doc->nodes[t->open[i]].ns == AR_NS_MATHML)
        {
            if (ar__is(t, t->open[i], "mi") || ar__is(t, t->open[i], "mo") ||
                ar__is(t, t->open[i], "mn") || ar__is(t, t->open[i], "ms") ||
                ar__is(t, t->open[i], "mtext") || ar__is(t, t->open[i], "annotation-xml"))
            {
                return 0;
            }
            continue;
        }
        if (t->doc->nodes[t->open[i]].ns == AR_NS_SVG)
        {
            if (ar__is(t, t->open[i], "foreignObject") || ar__is(t, t->open[i], "desc") ||
                ar__is(t, t->open[i], "title"))
            {
                return 0;
            }
            continue;
        }
        if (button_scope && ar__is(t, t->open[i], "button"))
        {
            return 0;
        }
        if (ar__node_name_in(t, t->open[i], STOP))
        {
            return 0;
        }
    }
    return 0;
}

/*
 * Pop through the named element, or do nothing at all if it is not open.
 *
 * The guard is the whole point. Every caller in the specification is
 * preceded by "if the stack of open elements does not have an element in
 * scope with that tag name, ignore the token" -- and without it this walks
 * the stack to the floor, taking the html element with it.
 *
 * Invisible in a document, where a `<table>` is almost always open by the
 * time anything asks. Not invisible in a *fragment*: `<table><tr>` parsed
 * against a `table` context has no table on the stack at all, because the
 * context element is not a node. The tree came back empty.
 */
/*
 * "In table scope", §13.2.4.2, which stops at three elements rather than
 * eight.
 *
 * It exists so that a table part with no table around it is ignored instead of
 * closing something it does not belong to. The fragment cases are where it
 * shows: parsing `<tbody><a>` against a `tbody` context, there is no tbody on
 * the stack -- the context element is not pushed, only a synthetic html root
 * -- so the `<tbody>` is dropped and the `<a>` is all that is left. Without
 * the guard the pop loop ate the synthetic root and the document had nowhere
 * to put anything.
 */
static int ar__in_table_scope(const ar__tree *t, const char *const *tags)
{
    ar_i32 i;

    for (i = t->open_n - 1; i >= 1; --i)
    {
        if (t->doc->nodes[t->open[i]].ns != AR_NS_HTML)
        {
            continue;
        }
        if (ar__node_name_in(t, t->open[i], tags))
        {
            return 1;
        }
        if (ar__is(t, t->open[i], "html") || ar__is(t, t->open[i], "table") ||
            ar__is(t, t->open[i], "template"))
        {
            return 0;
        }
    }
    return 0;
}

/*
 * "In select scope", §13.2.4.4, which is the inverse of every other scope:
 * instead of listing what stops the walk it lists the two things that do not.
 * Only `optgroup` and `option` may stand between the current node and the
 * select; anything else means there is no select in scope.
 */
static int ar__in_select_scope(const ar__tree *t)
{
    ar_i32 i;

    for (i = t->open_n - 1; i >= 1; --i)
    {
        if (t->doc->nodes[t->open[i]].ns != AR_NS_HTML)
        {
            return 0;
        }
        if (ar__is(t, t->open[i], "select"))
        {
            return 1;
        }
        if (!ar__is(t, t->open[i], "optgroup") && !ar__is(t, t->open[i], "option"))
        {
            return 0;
        }
    }
    return 0;
}

static int ar__on_stack_named(const ar__tree *t, const char *tag);

/* Take one element off the stack wherever it is, which is what the head
   element needs when it is pushed back temporarily. */
static void ar__pop_index(ar__tree *t, ar_i32 node)
{
    ar_i32 i;
    ar_i32 w = 0;

    for (i = 0; i < t->open_n; ++i)
    {
        if (t->open[i] != node)
        {
            t->open[w++] = t->open[i];
        }
    }
    t->open_n = w;
}

static void ar__pop_until(ar__tree *t, const char *tag)
{
    if (!ar__on_stack_named(t, tag))
    {
        return;
    }
    while (t->open_n > 1)
    {
        int hit = ar__is(t, ar__current(t), tag);

        ar__pop(t);
        if (hit)
        {
            return;
        }
    }
}

/*
 * "Generate implied end tags", §13.2.6.
 *
 * This is why `<li>a<li>b` is two list items rather than one inside the other,
 * and why `<p>a<div>` closes the paragraph. The elements listed close
 * themselves when something else needs to.
 */
static void ar__implied_end_tags(ar__tree *t, const char *except)
{
    static const char *const IMPLIED[] = {"dd", "dt", "li", "optgroup", "option", "p",
                                          "rb", "rp", "rt", "rtc",      0};

    for (;;)
    {
        if (except && ar__is(t, ar__current(t), except))
        {
            return;
        }
        if (!ar__node_name_in(t, ar__current(t), IMPLIED))
        {
            return;
        }
        ar__pop(t);
    }
}

/* ------------------------------------------------------------------------
 * Insertion
 * ------------------------------------------------------------------------ */

/*
 * Foster parenting, §13.2.6.1.
 *
 * Content that appears inside a `<table>` where it may not does not go in the
 * table and is not dropped: it is relocated to immediately before the table.
 * `<table><em>x</em><tr>` puts the emphasis before the table, and every
 * browser agrees because they all implement this paragraph.
 *
 * Returns the parent to insert into, and sets `*before` to the node to insert
 * before, or -1 to append.
 *
 * ------------------------------------------------------------------------
 * The override target
 *
 * `target` is the specification's *override target*: the element the caller
 * has already decided to insert into, or -1 for "wherever we currently are".
 *
 * Nearly every caller passes -1. The adoption agency's step 4.14 does not, and
 * that is the whole reason this parameter exists. By then it has worked out
 * for itself that the destination is the common ancestor and that the common
 * ancestor is a table, so foster parenting applies -- but the *current node*
 * at that moment is something else entirely. Deciding again from the current
 * node gives a different answer to the one the caller already committed to,
 * and for `<table><em><p>x</em` the answer it gives is the paragraph that is
 * being moved. Nothing can be inserted into itself.
 *
 * Found by ar_fuzz, and only with a node budget small enough that the tree ran
 * out mid-agency -- which is why no corpus reached it.
 */
static ar_i32 ar__insertion_point(ar__tree *t, ar_i32 target, ar_i32 *before)
{
    static const char *const FOSTER[] = {"table", "tbody", "tfoot", "thead", "tr", 0};
    ar_i32                   i;
    ar_i32                   cur = target >= 0 ? target : ar__current(t);

    *before = -1;

    for (i = 0; FOSTER[i]; ++i)
    {
        if (ar__is(t, cur, FOSTER[i]))
        {
            ar_i32 k;
            ar_i32 last_table = -1;
            ar_i32 last_tmpl = -1;

            /*
             * The last table and the last template, and which of them is
             * lower on the stack decides where this goes -- 13.2.6.1, steps 2
             * to 4 of "the appropriate place for inserting a node".
             *
             * The template step was missing entirely, and it is not an edge
             * case: a template's contents are their own document, so anything
             * fostered out of a table inside one has to land in the template
             * rather than beside it. Without it `<template><tr><div></div>`
             * put the div outside the template altogether, which is the one
             * place it certainly does not belong.
             */
            for (k = t->open_n - 1; k >= 1; --k)
            {
                if (last_table < 0 && ar__is(t, t->open[k], "table"))
                {
                    last_table = k;
                }
                if (last_tmpl < 0 && t->doc->nodes[t->open[k]].ns == AR_NS_HTML &&
                    ar__is(t, t->open[k], "template"))
                {
                    last_tmpl = k;
                }
            }
            if (last_tmpl >= 0 && (last_table < 0 || last_tmpl > last_table))
            {
                return ar__content_of(t, t->open[last_tmpl]);
            }
            if (last_table >= 0)
            {
                if (t->doc->nodes[t->open[last_table]].parent >= 0)
                {
                    *before = t->open[last_table];
                    return t->doc->nodes[t->open[last_table]].parent;
                }
                /* A table with no parent: fall back to the element below it. */
                return last_table > 1 ? t->open[last_table - 1] : t->open[0];
            }
            return t->open[0];
        }
    }
    return cur;
}

static void ar__insert_node_into(ar__tree *t, ar_i32 node, int foster, ar_i32 target)
{
    ar_i32 before = -1;
    ar_i32 parent =
        foster ? ar__insertion_point(t, target, &before) : (target >= 0 ? target : ar__current(t));

    parent = ar__content_of(t, parent);

    if (before >= 0)
    {
        parent = t->doc->nodes[before].parent;
    }

    /*
     * Out of the node's own subtree, if the placement landed inside it.
     *
     * Foster parenting picks the last table on the stack and inserts before
     * it, into the table's parent. Once the list of active formatting
     * elements has overflowed -- AR_HTML_FMT is a fixed cap, and past it a
     * clone is dropped rather than made -- the stack and the tree can disagree
     * about who contains whom, and that parent can turn out to be the element
     * being moved. The result was an element that was its own parent and its
     * own first child at once.
     *
     * Refusing the insertion is not enough on its own: the caller has already
     * detached the node, so refusing loses the subtree, which is the same bug
     * with the damage moved somewhere quieter. So walk up instead, and place
     * it at the first ancestor that is not inside it.
     *
     * The floor is the html element, which nothing can be a descendant of and
     * which therefore always terminates this. A node parked there is in the
     * wrong place in a document that was already beyond repair; it is in the
     * tree, and the tree is a tree.
     */
    if (ar__would_loop(t, parent, node))
    {
        ar_i32 steps = 0;

        before = -1;
        while (parent >= 0 && ar__would_loop(t, parent, node))
        {
            parent = t->doc->nodes[parent].parent;
            if (++steps > t->doc->node_count)
            {
                parent = -1;
                break;
            }
        }
        if (parent < 0)
        {
            parent = t->open_n > 1 ? t->open[1] : 0;
        }
        t->doc->errors++;
    }

    if (before >= 0)
    {
        ar__insert_before(t, before, node);
    }
    else
    {
        ar__append(t, parent, node);
    }
}

/* Wherever we currently are, which is what every caller but one wants. */
static void ar__insert_node(ar__tree *t, ar_i32 node, int foster)
{
    ar__insert_node_into(t, node, foster, -1);
}

/*
 * Text, stored NUL-terminated in the document's own buffer -- always, even
 * when the span already points at the caller's stable input.
 *
 * Tag names and attribute values are *compared*, so they can stay spans of
 * whatever they came from. Text is *rendered*, and ar_text takes a C string
 * and keeps the pointer for the whole frame. A span into the middle of a
 * document has no terminator after it and one into the tokenizer's scratch is
 * overwritten by the next token, so neither can be handed straight to the box
 * tree.
 *
 * This is the one place areole copies a document's bytes, and it is bounded by
 * the text the document actually contains.
 */
/*
 * How a NUL in a run of text is treated, and it is a property of *where* the
 * text is rather than of the text.
 *
 * Every HTML insertion mode ignores it. Foreign content replaces it with
 * U+FFFD -- §13.2.6.5 says so in one line, and it is the only place the two
 * differ. Passing the rule down rather than keeping a flag on the tree, so a
 * reader of ar__text_store can see which answer it is giving.
 */
#define AR__NUL_DROP    0
#define AR__NUL_REPLACE 1

/* memchr rather than a loop: this is asked of every run of text in every
   document, and the answer is no for almost all of them. */
static int ar__has_nul(ar_span s)
{
    return s.n != 0 && memchr(s.p, 0, s.n) != 0;
}

/* What `s` becomes once the NULs in it are dealt with. */
static ar_u32 ar__stored_len(ar_span s, int rule)
{
    ar_u32 i;
    ar_u32 n = 0;

    if (!ar__has_nul(s))
    {
        return s.n;
    }
    for (i = 0; i < s.n; ++i)
    {
        if (s.p[i] != 0)
        {
            ++n;
        }
        else if (rule == AR__NUL_REPLACE)
        {
            n += 3u; /* U+FFFD is three bytes of UTF-8 */
        }
    }
    return n;
}

/* The bytes, into `out`. Returns how many were written. */
static ar_u32 ar__store_bytes(char *out, ar_span s, int rule)
{
    ar_u32 i;
    ar_u32 n = 0;

    if (!ar__has_nul(s))
    {
        memcpy(out, s.p, s.n);
        return s.n;
    }
    for (i = 0; i < s.n; ++i)
    {
        if (s.p[i] != 0)
        {
            out[n++] = s.p[i];
        }
        else if (rule == AR__NUL_REPLACE)
        {
            out[n++] = (char)0xEF;
            out[n++] = (char)0xBF;
            out[n++] = (char)0xBD;
        }
    }
    return n;
}

static ar_span ar__text_store(ar__tree *t, ar_span s, int rule)
{
    ar_span out;
    ar_u32  need;

    out.p = 0;
    out.n = 0;
    if (s.n == 0)
    {
        return out;
    }
    need = ar__stored_len(s, rule);
    if (t->doc->text_used + need + 1u > t->doc->text_cap)
    {
        t->doc->overflowed = 1;
        return out;
    }
    out.p = t->doc->text + t->doc->text_used;
    out.n = ar__store_bytes(t->doc->text + t->doc->text_used, s, rule);
    t->doc->text_used += out.n;
    t->doc->text[t->doc->text_used++] = 0;
    return out;
}

/* Extend the last text node in place, overwriting its terminator. Only
   possible when it is the most recent thing in the buffer, which is the
   ordinary case for a run split by a character reference. */
static int ar__text_extend(ar__tree *t, ar_i32 node, ar_span s, int rule)
{
    ar_span old = t->doc->nodes[node].text;

    if (old.n == 0 || old.p + old.n + 1 != t->doc->text + t->doc->text_used)
    {
        return 0;
    }
    if (t->doc->text_used + ar__stored_len(s, rule) > t->doc->text_cap)
    {
        t->doc->overflowed = 1;
        return 0;
    }
    --t->doc->text_used; /* drop the terminator; a new one goes after */
    {
        ar_u32 wrote = ar__store_bytes(t->doc->text + t->doc->text_used, s, rule);

        t->doc->text_used += wrote;
        t->doc->nodes[node].text.n += wrote;
    }
    t->doc->text[t->doc->text_used++] = 0;
    return 1;
}

/*
 * How many bytes of this run are not NUL.
 *
 * Every HTML insertion mode says the same thing about a U+0000 character
 * token: parse error, ignore the token. Not replace -- *ignore*. The
 * replacement character belongs to the tokenizer's other states and to foreign
 * content; in the tree builder a NUL simply does not become text.
 *
 * The data state keeps a NUL, correctly, so it arrives here and has to be
 * dropped here. Inserting it instead put a byte in the document that no
 * browser has, and a run that is *only* NULs has to insert nothing at all
 * rather than an empty text node.
 */
static ar_u32 ar__non_nul(ar_span s)
{
    ar_u32 i;
    ar_u32 n = 0;

    for (i = 0; i < s.n; ++i)
    {
        if (s.p[i] != 0)
        {
            ++n;
        }
    }
    return n;
}

static void ar__insert_text_ex(ar__tree *t, ar_span s, int foster, int rule)
{
    ar_i32 before = -1;
    ar_i32 parent;
    ar_i32 node;

    foster = foster || t->foster;
    parent = ar__content_of(t, foster ? ar__insertion_point(t, -1, &before) : ar__current(t));

    if (s.n == 0 || (rule == AR__NUL_DROP && ar__non_nul(s) == 0))
    {
        return;
    }

    /*
     * Appended to the previous text node when there is one, which the
     * specification requires and which matters more than it looks: a run split
     * by a character reference would otherwise become two text nodes, and
     * every consumer would have to join them again.
     */
    if (before < 0 && parent >= 0)
    {
        ar_i32 last = t->doc->nodes[parent].last_child;

        if (last >= 0 && t->doc->nodes[last].kind == AR_DOM_TEXT &&
            ar__text_extend(t, last, s, rule))
        {
            return;
        }
    }
    else if (before >= 0)
    {
        /*
         * Fostered text joins the text already before the table.
         *
         * The specification appends to "the node immediately before the
         * insertion position", and when foster parenting that is the sibling
         * before the table rather than the parent's last child. Without it
         * `A<table><tr> B</tr> B</table>` came out as three text nodes where
         * every browser has one: the append condition only ever looked at the
         * end of the parent, which is past the table.
         */
        ar_i32 prev = t->doc->nodes[before].prev_sibling;

        if (prev >= 0 && t->doc->nodes[prev].kind == AR_DOM_TEXT &&
            ar__text_extend(t, prev, s, rule))
        {
            return;
        }
    }
    s = ar__text_store(t, s, rule);
    if (s.n == 0)
    {
        return;
    }

    node = ar__node(t, AR_DOM_TEXT);
    if (node < 0)
    {
        return;
    }
    t->doc->nodes[node].text = s;
    if (before >= 0)
    {
        ar__insert_before(t, before, node);
    }
    else
    {
        ar__append(t, parent, node);
    }
}

/*
 * Where a child of `node` actually goes.
 *
 * For everything except a `<template>` this is `node` itself. A template owns
 * a content fragment and everything written inside the template belongs to
 * that instead -- which is what makes the contents inert, and what every
 * serialisation of a template shows as a `content` line between the element
 * and its children.
 */
static ar_i32 ar__content_of(const ar__tree *t, ar_i32 node)
{
    /* The first-byte screen again: this runs on every insertion in every
       document, and almost none of them is a template. */
    if (node >= 0 && t->doc->nodes[node].kind == AR_DOM_ELEMENT &&
        t->doc->nodes[node].ns == AR_NS_HTML && t->doc->nodes[node].name.n == 8 &&
        ar__h_lower_c((unsigned char)t->doc->nodes[node].name.p[0]) == 't' &&
        ar__is(t, node, "template"))
    {
        ar_i32 c = t->doc->nodes[node].first_child;

        if (c >= 0 && t->doc->nodes[c].kind == AR_DOM_FRAGMENT)
        {
            return c;
        }
    }
    return node;
}

/*
 * The special category, §13.2.4.2.
 *
 * The whole list, not the handful an algorithm happened to need. Two places
 * ask this question and they are the two that decide what a stray end tag can
 * reach: the adoption agency looks for the topmost special element below a
 * formatting element, and "any other end tag" stops walking at the first one.
 *
 * The second is what makes `</div>` inside `<p>` harmless instead of
 * destructive -- the walk hits the `p`, which is special, and gives up. A walk
 * that does not check simply keeps going and closes the div, taking everything
 * between with it.
 *
 * The last three entries are foreign, and they are why `<svg><foreignObject>
 * <p></div>` puts the text in the paragraph: an integration point is a wall.
 */
static int ar__is_special(const ar__tree *t, ar_i32 node)
{
    static const char *const HTML_SPECIAL[] = {
        "address", "applet",     "area",     "article",    "aside",     "base",     "basefont",
        "bgsound", "blockquote", "body",     "br",         "button",    "caption",  "center",
        "col",     "colgroup",   "dd",       "details",    "dir",       "div",      "dl",
        "dt",      "embed",      "fieldset", "figcaption", "figure",    "footer",   "form",
        "frame",   "frameset",   "h1",       "h2",         "h3",        "h4",       "h5",
        "h6",      "head",       "header",   "hgroup",     "hr",        "html",     "iframe",
        "img",     "input",      "keygen",   "li",         "link",      "listing",  "main",
        "marquee", "menu",       "meta",     "nav",        "noembed",   "noframes", "noscript",
        "object",  "ol",         "p",        "param",      "plaintext", "pre",      "script",
        "search",  "section",    "select",   "source",     "style",     "summary",  "table",
        "tbody",   "td",         "template", "textarea",   "tfoot",     "th",       "thead",
        "title",   "tr",         "track",    "ul",         "wbr",       "xmp",      0};

    if (node < 0 || t->doc->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return 0;
    }
    if (t->doc->nodes[node].ns == AR_NS_MATHML)
    {
        return ar__is(t, node, "mi") || ar__is(t, node, "mo") || ar__is(t, node, "mn") ||
               ar__is(t, node, "ms") || ar__is(t, node, "mtext") ||
               ar__is(t, node, "annotation-xml");
    }
    if (t->doc->nodes[node].ns == AR_NS_SVG)
    {
        return ar__is(t, node, "foreignObject") || ar__is(t, node, "desc") ||
               ar__is(t, node, "title");
    }
    return ar__name_in(t->doc->nodes[node].name, HTML_SPECIAL);
}

static ar_i32 ar__insert_element(ar__tree *t, const ar_token *tok, int foster)
{
    ar_i32 node;
    ar_i32 k;

    foster = foster || t->foster;
    node = ar__node(t, AR_DOM_ELEMENT);

    if (node < 0)
    {
        return -1;
    }
    t->doc->nodes[node].name = ar__keep(t, tok->name);

    if (tok->attr_count > 0 && t->doc->attr_count + tok->attr_count <= t->doc->attr_cap)
    {
        t->doc->nodes[node].attr_first = t->doc->attr_count;
        t->doc->nodes[node].attr_count = tok->attr_count;
        for (k = 0; k < tok->attr_count; ++k)
        {
            t->doc->attrs[t->doc->attr_count].name = ar__keep(t, tok->attrs[k].name);
            t->doc->attrs[t->doc->attr_count].value = ar__keep(t, tok->attrs[k].value);

            /*
             * An HTML attribute is in no namespace, and saying so is not
             * redundant: the caller owns the attribute table and areole never
             * clears it, so a slot holds whatever the last document to use it
             * left there. ar__insert_foreign sets `ns`; this path did not, so
             * an ordinary attribute on an ordinary element inherited the
             * xlink or xml namespace from a document parsed before it.
             *
             * It made the conformance score depend on run order --
             * `<foo bar="baz">` passed in a fresh process and failed after
             * tests9.dat's `<math xlink:href=foo>` had used slot 1.
             */
            t->doc->attrs[t->doc->attr_count].ns = AR_ATTR_NS_NONE;
            ++t->doc->attr_count;
        }
    }
    else if (tok->attr_count > 0)
    {
        t->doc->overflowed = 1;
    }

    ar__insert_node(t, node, foster);
    ar__push(t, node);

    /* A template is given its content fragment the moment it exists, so that
       ar__content_of has something to find on the very next token. */
    if (ar_span_is(tok->name, "template"))
    {
        ar_i32 frag = ar__node(t, AR_DOM_FRAGMENT);

        if (frag >= 0)
        {
            ar__append(t, node, frag);
        }
    }
    return node;
}

/* ------------------------------------------------------------------------
 * The list of active formatting elements
 * ------------------------------------------------------------------------ */
/*
 * Merge a second `<html>` or `<body>`'s attributes onto the first, §13.2.6.4.7.
 *
 * The elements are not created twice -- a document has one html element and one
 * body element -- but the attributes on the duplicate tag are not discarded:
 * every name the first element does not already carry is added to it. So
 * `<body class=a><body id=b>` is one body with both, and the first value wins
 * on a clash.
 *
 * A node's attributes are a contiguous run in one table, so growing a run that
 * is not the last one would overwrite its neighbour. The run is copied to the
 * end of the table first and the old copy abandoned. That leaks table entries,
 * and deliberately: this happens only on a duplicate `<html>` or `<body>`,
 * which is at most twice in a document, and an arena has nothing to free into.
 */
static void ar__merge_attrs(ar__tree *t, ar_i32 node, const ar_token *tok)
{
    ar_doc *d = t->doc;
    ar_i32  k;
    ar_i32  added = 0;

    if (node < 0)
    {
        return;
    }
    for (k = 0; k < tok->attr_count; ++k)
    {
        ar_i32 j;
        int    have = 0;

        for (j = 0; j < d->nodes[node].attr_count; ++j)
        {
            if (ar__span_eq(d->attrs[d->nodes[node].attr_first + j].name, tok->attrs[k].name))
            {
                have = 1;
                break;
            }
        }
        if (have)
        {
            continue;
        }
        if (!added)
        {
            /* Relocate the run, once, before the first addition. */
            ar_i32 n = d->nodes[node].attr_count;

            if (d->attr_count + n + (tok->attr_count - k) > d->attr_cap)
            {
                d->overflowed = 1;
                return;
            }
            for (j = 0; j < n; ++j)
            {
                d->attrs[d->attr_count + j] = d->attrs[d->nodes[node].attr_first + j];
            }
            d->nodes[node].attr_first = d->attr_count;
            d->attr_count += n;
            added = 1;
        }
        if (d->attr_count >= d->attr_cap)
        {
            d->overflowed = 1;
            return;
        }
        d->attrs[d->attr_count].name = ar__keep(t, tok->attrs[k].name);
        d->attrs[d->attr_count].value = ar__keep(t, tok->attrs[k].value);
        d->attrs[d->attr_count].ns = AR_ATTR_NS_NONE;
        ++d->attr_count;
        ++d->nodes[node].attr_count;
    }
}

/*
 * Two elements are the same for Noah's Ark: same name, same namespace, same
 * attributes -- names and values, in any order.
 */
static int ar__fmt_alike(const ar__tree *t, ar_i32 a, ar_i32 b)
{
    const ar_dom_node *x = &t->doc->nodes[a];
    const ar_dom_node *y = &t->doc->nodes[b];
    ar_i32             i;

    if (x->ns != y->ns || !ar__span_eq(x->name, y->name) || x->attr_count != y->attr_count)
    {
        return 0;
    }
    for (i = 0; i < x->attr_count; ++i)
    {
        const ar_attr *ax = &t->doc->attrs[x->attr_first + i];
        ar_i32         j;
        int            found = 0;

        for (j = 0; j < y->attr_count; ++j)
        {
            const ar_attr *ay = &t->doc->attrs[y->attr_first + j];

            if (ax->ns == ay->ns && ar__span_eq(ax->name, ay->name) &&
                ar__span_eq(ax->value, ay->value))
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
    return 1;
}

/*
 * The Noah's Ark clause, §13.2.4.3: three of a kind and no more.
 *
 * If the list already holds three entries with the same name, namespace and
 * attributes since the last marker, the earliest of them is dropped before the
 * fourth goes on. The stack of open elements is untouched -- all four `<b>`
 * in `<p><b><b><b><b><p>x` really are nested -- so what the clause decides is
 * how many get *reopened* after the paragraph: three, not four.
 *
 * It exists because reconstruction is what makes a formatting element survive
 * a block boundary, and a page that opens the same tag in a loop would
 * otherwise reopen it once per iteration and nest a thousand deep. `tests23`
 * is four cases of exactly that, written with `<font size=4>`.
 */
static void ar__fmt_push(ar__tree *t, ar_i32 node)
{
    ar_i32 seen = 0;
    ar_i32 i;

    if (t->fmt_n >= AR_HTML_FMT)
    {
        t->doc->errors++;
        return;
    }
    for (i = t->fmt_n - 1; i >= 0; --i)
    {
        if (t->fmt[i] < 0)
        {
            break; /* a marker ends the search */
        }
        if (ar__fmt_alike(t, t->fmt[i], node))
        {
            ++seen;
            if (seen == 3)
            {
                ar_i32 k;

                for (k = i; k + 1 < t->fmt_n; ++k)
                {
                    t->fmt[k] = t->fmt[k + 1];
                }
                --t->fmt_n;
                break;
            }
        }
    }
    t->fmt[t->fmt_n++] = node;
}

static void ar__fmt_marker(ar__tree *t)
{
    ar__fmt_push(t, -1);
}

static void ar__fmt_clear_to_marker(ar__tree *t)
{
    while (t->fmt_n > 0)
    {
        ar_i32 e = t->fmt[--t->fmt_n];

        if (e < 0)
        {
            return;
        }
    }
}

static void ar__fmt_remove(ar__tree *t, ar_i32 node)
{
    ar_i32 i, k;

    for (i = 0; i < t->fmt_n; ++i)
    {
        if (t->fmt[i] == node)
        {
            for (k = i; k + 1 < t->fmt_n; ++k)
            {
                t->fmt[k] = t->fmt[k + 1];
            }
            --t->fmt_n;
            return;
        }
    }
}

static int ar__on_stack(const ar__tree *t, ar_i32 node)
{
    ar_i32 i;

    for (i = 0; i < t->open_n; ++i)
    {
        if (t->open[i] == node)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Reconstruct the active formatting elements, §13.2.4.3.
 *
 * This is what carries `<b>` across a block boundary: `<b>one<p>two</p>` puts
 * the `two` inside a *fresh* `<b>` inside the paragraph, because the original
 * `<b>` is still active but is no longer open. Without it the bold simply
 * stops at the paragraph, which is not what any browser does.
 */
static void ar__reconstruct(ar__tree *t)
{
    ar_i32 i;

    if (t->fmt_n == 0)
    {
        return;
    }
    if (t->fmt[t->fmt_n - 1] < 0 || ar__on_stack(t, t->fmt[t->fmt_n - 1]))
    {
        return;
    }

    i = t->fmt_n - 1;
    while (i > 0 && t->fmt[i - 1] >= 0 && !ar__on_stack(t, t->fmt[i - 1]))
    {
        --i;
    }
    for (; i < t->fmt_n; ++i)
    {
        ar_i32 src = t->fmt[i];
        ar_i32 fresh;

        if (src < 0)
        {
            continue;
        }
        fresh = ar__node(t, AR_DOM_ELEMENT);
        if (fresh < 0)
        {
            return;
        }
        t->doc->nodes[fresh].name = t->doc->nodes[src].name;
        t->doc->nodes[fresh].attr_first = t->doc->nodes[src].attr_first;
        t->doc->nodes[fresh].attr_count = t->doc->nodes[src].attr_count;
        ar__insert_node(t, fresh, 1);
        ar__push(t, fresh);
        t->fmt[i] = fresh;
    }
}

/* ------------------------------------------------------------------------
 * The adoption agency algorithm, §13.2.6.4.7
 *
 * `<b><i></b></i>` produces the same tree in every browser, and it is not the
 * tree the markup describes. Eight steps, an outer loop the specification caps
 * at eight iterations, and the reason a misnested `<b>` does not swallow the
 * rest of the document.
 *
 * This is the implementation people warn about. It is written in the
 * specification's step order with the numbers in the comments, because the
 * only way to check it is against the document.
 * ------------------------------------------------------------------------ */
static int ar__adoption(ar__tree *t, const char *tag)
{
    ar_i32 outer;

    /* Step 1: if the current node is the subject and not in the list, pop it. */
    if (ar__is(t, ar__current(t), tag))
    {
        ar_i32 cur = ar__current(t);
        ar_i32 i;
        int    in_list = 0;

        for (i = 0; i < t->fmt_n; ++i)
        {
            if (t->fmt[i] == cur)
            {
                in_list = 1;
            }
        }
        if (!in_list)
        {
            ar__pop(t);
            return 1;
        }
    }

    for (outer = 0; outer < 8; ++outer)
    {
        ar_i32 formatting = -1;
        ar_i32 fmt_index = -1;
        ar_i32 furthest = -1;
        ar_i32 stack_index = -1;
        ar_i32 i;

        /* Step 4.2: the last formatting element with this tag, after any
           marker. */
        for (i = t->fmt_n - 1; i >= 0; --i)
        {
            if (t->fmt[i] < 0)
            {
                break;
            }
            if (ar__is(t, t->fmt[i], tag))
            {
                formatting = t->fmt[i];
                fmt_index = i;
                break;
            }
        }
        if (formatting < 0)
        {
            return 0; /* "any other end tag" handles it */
        }

        if (!ar__on_stack(t, formatting))
        {
            /* Step 4.4: in the list but not open. Remove and stop. */
            t->doc->errors++;
            ar__fmt_remove(t, formatting);
            return 1;
        }
        for (i = 0; i < t->open_n; ++i)
        {
            if (t->open[i] == formatting)
            {
                stack_index = i;
            }
        }
        if (!ar__in_scope(t, tag, 0))
        {
            t->doc->errors++;
            return 1;
        }

        /* Step 4.6: the furthest block -- the topmost *special* element below
           the formatting element on the stack. */
        {
            for (i = stack_index + 1; i < t->open_n && furthest < 0; ++i)
            {
                ar_i32 k;

                for (k = 0; k < 1; ++k)
                {
                    if (ar__is_special(t, t->open[i]))
                    {
                        furthest = t->open[i];
                        break;
                    }
                }
            }
        }

        /* Step 4.7: no furthest block. Pop to and including the formatting
           element and remove it from the list. This is the common case and is
           what makes `<b>x</b>` ordinary. */
        if (furthest < 0)
        {
            while (t->open_n > 0 && ar__current(t) != formatting)
            {
                ar__pop(t);
            }
            ar__pop(t);
            ar__fmt_remove(t, formatting);
            return 1;
        }

        /*
         * Steps 4.9 to 4.19, in the specification's own step order.
         *
         * This is the part people warn about, and it is where the first
         * version cut a corner: it moved the furthest block's children into a
         * clone and stopped. Six of the eight cases a browser was asked came
         * out right anyway -- including `<b>1<i>2<em>3</b>4</em>5</i>`, which
         * is three levels deep -- and two did not, both for the same reason.
         *
         * The missing piece is step 4.14. The furthest block has to be moved
         * to the **common ancestor**, the element immediately above the
         * formatting element on the stack. Without it `<b>1<p>2</b>3</p>`
         * leaves the paragraph inside the bold instead of beside it, which is
         * the one thing anybody would notice on a page.
         */
        {
            ar_i32 common = stack_index > 0 ? t->open[stack_index - 1] : 0;
            ar_i32 bookmark = fmt_index;
            ar_i32 node_at = -1;
            ar_i32 node;
            ar_i32 last = furthest;
            ar_i32 inner;
            ar_i32 clone;
            ar_i32 child;

            /* Where the furthest block sits, so the inner loop can walk down
               from it towards the formatting element. */
            for (i = 0; i < t->open_n; ++i)
            {
                if (t->open[i] == furthest)
                {
                    node_at = i;
                }
            }

            /* 4.13, the inner loop. */
            for (inner = 1; inner <= 64 && node_at > 0; ++inner)
            {
                ar_i32 k;
                ar_i32 in_list = -1;

                --node_at; /* 4.13.2: the element immediately above */
                node = t->open[node_at];
                if (node == formatting)
                {
                    break; /* 4.13.3 */
                }
                for (k = 0; k < t->fmt_n; ++k)
                {
                    if (t->fmt[k] == node)
                    {
                        in_list = k;
                    }
                }
                /* 4.13.4: past three passes a node still in the list is
                   dropped from it -- the specification's own guard against a
                   pathological chain of formatting elements. */
                if (inner > 3 && in_list >= 0)
                {
                    ar__fmt_remove(t, node);
                    if (bookmark > in_list)
                    {
                        --bookmark;
                    }
                    in_list = -1;
                }
                if (in_list < 0)
                {
                    /* 4.13.5: not a formatting element. Off the stack, and the
                       tree is left alone. */
                    for (k = node_at; k + 1 < t->open_n; ++k)
                    {
                        t->open[k] = t->open[k + 1];
                    }
                    --t->open_n;
                    continue;
                }
                /* 4.13.6: a clone takes its place in both lists. */
                clone = ar__node(t, AR_DOM_ELEMENT);
                if (clone < 0)
                {
                    return 1;
                }
                t->doc->nodes[clone].name = t->doc->nodes[node].name;
                t->doc->nodes[clone].attr_first = t->doc->nodes[node].attr_first;
                t->doc->nodes[clone].attr_count = t->doc->nodes[node].attr_count;
                t->fmt[in_list] = clone;
                t->open[node_at] = clone;
                node = clone;

                /* 4.13.7 */
                if (last == furthest)
                {
                    bookmark = in_list + 1;
                }
                /* 4.13.8 */
                ar__detach(t, last);
                ar__append(t, node, last);
                /* 4.13.9 */
                last = node;
            }

            /*
             * 4.14: the last node goes into the common ancestor.
             *
             * The step the first version left out, and the whole of what makes
             * a paragraph a sibling of the bold rather than its child.
             */
            ar__detach(t, last);
            if (ar__is(t, common, "table") || ar__is(t, common, "tbody") ||
                ar__is(t, common, "tfoot") || ar__is(t, common, "thead") || ar__is(t, common, "tr"))
            {
                /* Foster parented into the common ancestor -- the override
                   target, and not the current node. */
                ar__insert_node_into(t, last, 1, common);
            }
            else
            {
                ar__append(t, common, last);
            }

            /* 4.15 to 4.17: a clone of the formatting element takes the
               furthest block's children and becomes its only child. */
            clone = ar__node(t, AR_DOM_ELEMENT);
            if (clone < 0)
            {
                return 1;
            }
            t->doc->nodes[clone].name = t->doc->nodes[formatting].name;
            t->doc->nodes[clone].attr_first = t->doc->nodes[formatting].attr_first;
            t->doc->nodes[clone].attr_count = t->doc->nodes[formatting].attr_count;

            child = t->doc->nodes[furthest].first_child;
            while (child >= 0)
            {
                ar_i32 next = t->doc->nodes[child].next_sibling;

                ar__detach(t, child);
                ar__append(t, clone, child);
                child = next;
            }
            ar__append(t, furthest, clone);

            /* 4.18: out of the list, and the clone in at the bookmark. */
            ar__fmt_remove(t, formatting);
            if (bookmark < 0 || bookmark > t->fmt_n)
            {
                bookmark = t->fmt_n;
            }
            if (t->fmt_n < AR_HTML_FMT)
            {
                for (i = t->fmt_n; i > bookmark; --i)
                {
                    t->fmt[i] = t->fmt[i - 1];
                }
                t->fmt[bookmark] = clone;
                ++t->fmt_n;
            }

            /* 4.19: off the stack, and the clone immediately above the
               furthest block. */
            {
                ar_i32 w = 0;
                ar_i32 at = -1;

                for (i = 0; i < t->open_n; ++i)
                {
                    if (t->open[i] != formatting)
                    {
                        t->open[w++] = t->open[i];
                    }
                }
                t->open_n = w;
                for (i = 0; i < t->open_n; ++i)
                {
                    if (t->open[i] == furthest)
                    {
                        at = i;
                    }
                }
                if (at >= 0 && t->open_n < AR_HTML_STACK)
                {
                    for (i = t->open_n; i > at + 1; --i)
                    {
                        t->open[i] = t->open[i - 1];
                    }
                    t->open[at + 1] = clone;
                    ++t->open_n;
                }
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * Quirks, §13.2.6.1
 * ------------------------------------------------------------------------ */
static int ar__starts_with_ci(ar_span s, const char *prefix)
{
    ar_u32 i;

    for (i = 0; prefix[i]; ++i)
    {
        char a, b;

        if (i >= s.n)
        {
            return 0;
        }
        a = s.p[i];
        b = prefix[i];
        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a + 32);
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b + 32);
        }
        if (a != b)
        {
            return 0;
        }
    }
    return 1;
}

/*
 * Which doctypes force quirks.
 *
 * The specification's table is around sixty public-identifier prefixes, nearly
 * all of them HTML 3.2 and 4.0 variants from the 1990s. The ones here are the
 * families that table is made of; the tail of individual vendor DTDs is not,
 * and a document carrying one gets no-quirks where a browser gives quirks.
 */
static ar_quirks ar__quirks_for(const ar_token *tok)
{
    /*
     * The three-way decision of §13.2.6.4.1, from the specification's own
     * lists rather than from an approximation of them.
     *
     * It used to be eight prefixes chosen by eye, and a thirty-four doctype
     * corpus measured against a browser said sixteen of them were wrong. Some
     * were wrong in both directions: `-" AR__FPI "W3C" AR__FPI "DTD HTML 3.0" AR__FPI "EN` is *not*
     * on the legacy list although 3.2 is, and `nonsense` as a public
     * identifier is standards mode rather than quirks.
     *
     * Quirks is not a curiosity: it changes the box model to
     * content-box-plus-padding, changes table cell inheritance and changes
     * line height. Sixteen doctypes in the wrong mode is sixteen documents
     * laid out wrongly from end to end.
     *
     * ------------------------------------------------------------------
     * The double solidus is spelled AR__FPI rather than written
     *
     * A DTD public identifier is full of it, and the CI gate that keeps C99
     * comments out of this codebase greps for the pair with a regular
     * expression, which cannot tell one inside a string literal from one
     * starting a comment. The gate has caught three real portability bugs and
     * is worth more than the readability of sixty strings, so the strings
     * bend.
     */
#define AR__FPI                                                                                    \
    "/"                                                                                            \
    "/"

    static const char *const QUIRKY_EXACT[] = {"-" AR__FPI "W3O" AR__FPI
                                               "DTD W3 HTML Strict 3.0" AR__FPI "EN" AR__FPI,
                                               "-/W3C/DTD HTML 4.0 Transitional/EN", "HTML", 0};

    static const char *const QUIRKY_PREFIX[] = {
        "+" AR__FPI "Silmaril" AR__FPI "dtd html Pro v0r11 19970101" AR__FPI,
        "-" AR__FPI "AS" AR__FPI "DTD HTML 3.0 asWedit + extensions" AR__FPI,
        "-" AR__FPI "AdvaSoft Ltd" AR__FPI "DTD HTML 3.0 asWedit + extensions" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0 Level 1" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0 Level 2" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0 Strict Level 1" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0 Strict Level 2" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0 Strict" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.0" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 2.1E" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 3.0" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 3.2 Final" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 3.2" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML 3" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Level 0" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Level 1" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Level 2" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Level 3" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Strict Level 0" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Strict Level 1" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Strict Level 2" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Strict Level 3" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML Strict" AR__FPI,
        "-" AR__FPI "IETF" AR__FPI "DTD HTML" AR__FPI,
        "-" AR__FPI "Metrius" AR__FPI "DTD Metrius Presentational" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 2.0 HTML Strict" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 2.0 HTML" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 2.0 Tables" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 3.0 HTML Strict" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 3.0 HTML" AR__FPI,
        "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer 3.0 Tables" AR__FPI,
        "-" AR__FPI "Netscape Comm. Corp." AR__FPI "DTD HTML" AR__FPI,
        "-" AR__FPI "Netscape Comm. Corp." AR__FPI "DTD Strict HTML" AR__FPI,
        "-" AR__FPI "O'Reilly and Associates" AR__FPI "DTD HTML 2.0" AR__FPI,
        "-" AR__FPI "O'Reilly and Associates" AR__FPI "DTD HTML Extended 1.0" AR__FPI,
        "-" AR__FPI "O'Reilly and Associates" AR__FPI "DTD HTML Extended Relaxed 1.0" AR__FPI,
        "-" AR__FPI "SQ" AR__FPI "DTD HTML 2.0 HoTMetaL + extensions" AR__FPI,
        "-" AR__FPI "SoftQuad Software" AR__FPI
        "DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0" AR__FPI,
        "-" AR__FPI "SoftQuad" AR__FPI
        "DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0" AR__FPI,
        "-" AR__FPI "Spyglass" AR__FPI "DTD HTML 2.0 Extended" AR__FPI,
        "-" AR__FPI "Sun Microsystems Corp." AR__FPI "DTD HotJava HTML" AR__FPI,
        "-" AR__FPI "Sun Microsystems Corp." AR__FPI "DTD HotJava Strict HTML" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 3 1995-03-24" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 3.2 Draft" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 3.2 Final" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 3.2" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 3.2S Draft" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.0 Frameset" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.0 Transitional" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML Experimental 19960712" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML Experimental 970421" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD W3 HTML" AR__FPI,
        "-" AR__FPI "W3O" AR__FPI "DTD W3 HTML 3.0" AR__FPI,
        "-" AR__FPI "WebTechs" AR__FPI "DTD Mozilla HTML 2.0" AR__FPI,
        "-" AR__FPI "WebTechs" AR__FPI "DTD Mozilla HTML" AR__FPI,
        0};

    /* Two that are quirks only when no system identifier stands beside them,
       and limited quirks when one does. The empty string counts as missing for
       these two and for nothing else, which the specification says out loud
       because it is otherwise unguessable. */
    static const char *const FOUR_OH_ONE[] = {
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.01 Frameset" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.01 Transitional" AR__FPI, 0};

    static const char *const LIMITED_PREFIX[] = {
        "-" AR__FPI "W3C" AR__FPI "DTD XHTML 1.0 Frameset" AR__FPI,
        "-" AR__FPI "W3C" AR__FPI "DTD XHTML 1.0 Transitional" AR__FPI, 0};

    ar_i32 i;
    int    has_system = tok->sys.p != 0 && tok->sys.n != 0;

    if (tok->force_quirks || !ar_span_is(tok->name, "html"))
    {
        return AR_QUIRKS_YES;
    }
    if (tok->sys.p &&
        ar_span_is(tok->sys, "http:" AR__FPI "www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
    {
        return AR_QUIRKS_YES;
    }
    for (i = 0; QUIRKY_EXACT[i]; ++i)
    {
        if (ar_span_is(tok->pub, QUIRKY_EXACT[i]))
        {
            return AR_QUIRKS_YES;
        }
    }
    for (i = 0; QUIRKY_PREFIX[i]; ++i)
    {
        if (ar__starts_with_ci(tok->pub, QUIRKY_PREFIX[i]))
        {
            return AR_QUIRKS_YES;
        }
    }
    for (i = 0; FOUR_OH_ONE[i]; ++i)
    {
        if (ar__starts_with_ci(tok->pub, FOUR_OH_ONE[i]))
        {
            return has_system ? AR_QUIRKS_LIMITED : AR_QUIRKS_YES;
        }
    }
    for (i = 0; LIMITED_PREFIX[i]; ++i)
    {
        if (ar__starts_with_ci(tok->pub, LIMITED_PREFIX[i]))
        {
            return AR_QUIRKS_LIMITED;
        }
    }
    return AR_QUIRKS_NO;

#undef AR__FPI
}

/* ------------------------------------------------------------------------
 * The insertion modes, §13.2.6.4
 * ------------------------------------------------------------------------ */

/* The elements that never have children and never close. */
static int ar__is_void(ar_span name)
{
    /* `basefont`, `bgsound` and `keygen` are void too. They are obsolete
       rather than absent, and `in head` reaches them: a tag it does not know
       is popped back to `after head`, which routes head content here again,
       which is a loop. */
    static const char *const VOID_TAGS[] = {
        "area",  "base",   "basefont", "bgsound", "br",    "col",    "embed", "hr",  "img",
        "input", "keygen", "link",     "meta",    "param", "source", "track", "wbr", 0};
    ar_i32 i;

    for (i = 0; VOID_TAGS[i]; ++i)
    {
        if (ar_span_is(name, VOID_TAGS[i]))
        {
            return 1;
        }
    }
    return 0;
}

/* The formatting elements the adoption agency exists for. */
/*
 * Does this start tag put the frameset-ok flag out?
 *
 * The flag decides whether a `<frameset>` reaching `in body` is honoured or
 * dropped, and the specification names the tags that clear it one at a time
 * rather than by category -- which is why the list reads arbitrarily and why
 * it has to be copied rather than reasoned about. `<br>` clears it and
 * `<param>` does not; both are void. `<div>` does not clear it, so
 * `<div><frameset>` really does throw the div away and build a frameset
 * document.
 *
 * `<input>` is the one with a condition: an input clears the flag unless its
 * `type` is `hidden`, ASCII case-insensitively. A hidden input is not visible
 * content, so it does not commit the document to having a body.
 */
static int ar__clears_frameset(const ar_token *tok)
{
    static const char *const TAGS[] = {
        "pre",    "listing", "li",      "dd",     "dt",     "button", "area",  "br",
        "embed",  "img",     "hr",      "keygen", "wbr",    "select", "table", "textarea",
        "iframe", "xmp",     "marquee", "object", "applet", 0};
    ar_i32 i;

    if (ar_span_is(tok->name, "input"))
    {
        for (i = 0; i < tok->attr_count; ++i)
        {
            if (ar_span_is(tok->attrs[i].name, "type"))
            {
                /* ar_span_is is already ASCII case-insensitive, which is what
                   `type=hidDEN` needs and is easy to assume it is not. */
                return !ar_span_is(tok->attrs[i].value, "hidden");
            }
        }
        return 1; /* no type at all is a text field */
    }
    return ar__name_in(tok->name, TAGS);
}

static int ar__is_formatting(ar_span name)
{
    /* `nobr` is one of these, which surprises people -- it is in the
       specification's list because it nests wrongly in exactly the way `<b>`
       does and needs the same repair. */
    static const char *const FMT_TAGS[] = {"a",      "b",      "big",  "code", "em",
                                           "font",   "i",      "nobr", "s",    "small",
                                           "strike", "strong", "tt",   "u",    0};
    ar_i32                   i;

    for (i = 0; FMT_TAGS[i]; ++i)
    {
        if (ar_span_is(name, FMT_TAGS[i]))
        {
            return 1;
        }
    }
    return 0;
}

/* Block-level starts that close an open paragraph first. This is why
   `<p>one<div>two` is two siblings and not a paragraph containing a div. */
static int ar__closes_p(ar_span name)
{
    static const char *const BLOCKS[] = {
        "address",   "article", "aside",  "blockquote", "center",   "details",
        "dialog",    "dir",     "div",    "dl",         "fieldset", "figcaption",
        "figure",    "footer",  "form",   "h1",         "h2",       "h3",
        "h4",        "h5",      "h6",     "header",     "hgroup",   "hr",
        "listing",   "main",    "menu",   "nav",        "ol",       "p",
        "plaintext", "pre",     "search", "section",    "summary",  "table",
        "ul",        "xmp",     0};
    return ar__name_in(name, BLOCKS);
}

/*
 * Whitespace, counting a NUL as though it were not there.
 *
 * The specification ignores a U+0000 character token first and asks about
 * the rest afterwards, so `<html> \0 <frameset>` is whitespace between the
 * tags -- and whitespace in `before head` is ignored, which is what lets the
 * frameset be honoured. Treating the NUL as content opened a body instead,
 * and the frameset then had nowhere to go.
 */
static int ar__space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

static int ar__all_space(ar_span s)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        char c = s.p[i];

        if (c == 0)
        {
            continue; /* ignored first, and the rest is asked about after */
        }
        if (!ar__space_char(c))
        {
            return 0;
        }
    }
    return 1;
}

/* Every HTML insertion mode ignores a NUL; only foreign content replaces it,
   and only ar__foreign says so. */
static void ar__insert_text(ar__tree *t, ar_span s, int foster)
{
    ar__insert_text_ex(t, s, foster, AR__NUL_DROP);
}

/*
 * A comment, or a processing instruction.
 *
 * A processing instruction arrives as a comment token carrying a target in
 * `name`, and it is handled here rather than in a branch of its own for a
 * reason that is not laziness: the specification inserts one in exactly the
 * places it inserts a comment, and every insertion mode already routes comment
 * tokens through this function. A separate token kind would mean the same
 * twelve `== AR_TOK_COMMENT` tests, each written twice.
 *
 * A comment never carries a name, so the two cannot be confused.
 */
static void ar__comment_node(ar__tree *t, const ar_token *tok, ar_i32 parent)
{
    ar_i32 node = ar__node(t, tok->name.n ? AR_DOM_PI : AR_DOM_COMMENT);

    if (node < 0)
    {
        return;
    }
    if (tok->name.n)
    {
        /*
         * Split the comment text into the target and the data.
         *
         * The token carries `?target data` whole, because that is what the
         * tokenizer layer is required to emit. The data begins after the `?`
         * and the target, skips any whitespace, and loses one `?` immediately
         * before the `>` that ended it -- so `<?good?>` has empty data and
         * `<?hey   there?>` has `there`.
         *
         * The offset is exact: a target is a valid XML name, so it contains no
         * carriage return and no NUL and the preprocessing that produced this
         * text cannot have changed its length.
         */
        ar_span data = tok->text;
        ar_u32  skip = 1u + tok->name.n;

        t->doc->nodes[node].name = ar__keep(t, tok->name);

        if (data.n >= skip)
        {
            data.p += skip;
            data.n -= skip;
        }
        else
        {
            data.n = 0;
        }
        while (data.n && (data.p[0] == ' ' || data.p[0] == '\t' || data.p[0] == '\n' ||
                          data.p[0] == '\f' || data.p[0] == '\r'))
        {
            ++data.p;
            --data.n;
        }
        if (data.n && data.p[data.n - 1] == '?')
        {
            --data.n;
        }
        t->doc->nodes[node].text = ar__keep(t, data);
        ar__append(t, ar__content_of(t, parent >= 0 ? parent : ar__current(t)), node);
        return;
    }
    t->doc->nodes[node].text = ar__keep(t, tok->text);
    ar__append(t, ar__content_of(t, parent >= 0 ? parent : ar__current(t)), node);
}

/* An element the document needs but the author did not write. `<html>`,
   `<head>` and `<body>` are all optional tags and most documents omit at least
   one of them. */
static ar_i32 ar__insert_implied(ar__tree *t, const char *tag)
{
    ar_token fake;

    memset(&fake, 0, sizeof fake);
    fake.kind = AR_TOK_START;
    fake.name.p = tag;
    fake.name.n = (ar_u32)strlen(tag);
    return ar__insert_element(t, &fake, 0);
}

static void   ar__process(ar__tree *t, const ar_token *tok);
static void   ar__process_mode(ar__tree *t, const ar_token *tok);
static ar_i32 ar__insert_foreign(ar__tree *t, const ar_token *tok, ar_ns ns, int foster);

/* `</p>` with no open paragraph still produces one, which is what the
   specification says and is how `</p>` alone in a body makes an empty
   paragraph in every browser. */
static void ar__close_p(ar__tree *t)
{
    if (!ar__in_scope(t, "p", 1))
    {
        return;
    }
    ar__implied_end_tags(t, "p");
    if (!ar__is(t, ar__current(t), "p"))
    {
        t->doc->errors++;
    }
    ar__pop_until(t, "p");
}

/*
 * "Reset the insertion mode appropriately", §13.2.4.1.
 *
 * Walk the stack from the current node down and take the mode from the first
 * element that names one. It is how a mode is recovered rather than
 * remembered, and it is the answer to the question every closing construct
 * asks: what were we doing before this?
 *
 * `</template>` used to answer it with `original_mode`, which is a single
 * remembered value and was `in head` -- because a template is head content
 * wherever it appears. So closing a template inside `<div>` inside `<body>`
 * put the parser back in `in head`, and the next start tag opened a second
 * `<body>` for itself.
 *
 * A remembered mode cannot be right here: the same `</template>` has to resume
 * `in body`, `in table`, `in row` or `in cell` depending on nothing but where
 * the template sits.
 */
static int ar__on_stack_named(const ar__tree *t, const char *tag)
{
    ar_i32 i;

    for (i = t->open_n - 1; i >= 1; --i)
    {
        if (t->doc->nodes[t->open[i]].ns == AR_NS_HTML && ar__is(t, t->open[i], tag))
        {
            return 1;
        }
    }
    return 0;
}

/* Two C strings, case-insensitively -- the context element is a literal from
   the caller rather than a span of the document. */
static int ar__lit_is(const char *a, const char *b)
{
    while (*a && *b)
    {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        int y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;

        if (x != y)
        {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static void ar__reset_mode(ar__tree *t)
{
    ar_i32 i;

    for (i = t->open_n - 1; i >= 1; --i)
    {
        ar_i32 node = t->open[i];
        int    last = i == 1;

        /*
         * In a fragment the bottom of the stack is the synthetic root, and the
         * specification says to use the *context* element in its place. That
         * is the whole reason a fragment starts in the right mode: `<td>x`
         * with a `tr` context begins in `in row`, and with a `div` context it
         * begins in `in body` and the tag is dropped.
         */
        if (last && t->ctx)
        {
            if (t->ctx_ns != AR_NS_HTML)
            {
                t->mode = M_IN_BODY;
                return;
            }
            if (ar__lit_is(t->ctx, "select"))
            {
                /*
                 * Step 4 of the mode reset, and the one context that was
                 * missing. A select fragment began in `in body`, where every
                 * rule that makes a select a select is absent: `<input>` was
                 * inserted instead of ignored, because "ignore it when there
                 * is no select in select scope" is a rule of `in select` and
                 * nothing was reading it.
                 *
                 * Never the in-table variant: the context element is not on
                 * the stack, so there is nothing below it to be a table.
                 */
                t->mode = M_IN_SELECT;
                return;
            }
            if (ar__lit_is(t->ctx, "td") || ar__lit_is(t->ctx, "th"))
            {
                t->mode = M_IN_BODY; /* a cell context parses as body content */
                return;
            }
            if (ar__lit_is(t->ctx, "tr"))
            {
                t->mode = M_IN_ROW;
                return;
            }
            if (ar__lit_is(t->ctx, "tbody") || ar__lit_is(t->ctx, "thead") ||
                ar__lit_is(t->ctx, "tfoot"))
            {
                t->mode = M_IN_TABLE_BODY;
                return;
            }
            if (ar__lit_is(t->ctx, "caption"))
            {
                t->mode = M_IN_CAPTION;
                return;
            }
            if (ar__lit_is(t->ctx, "colgroup"))
            {
                t->mode = M_IN_COLUMN_GROUP;
                return;
            }
            if (ar__lit_is(t->ctx, "table"))
            {
                t->mode = M_IN_TABLE;
                return;
            }
            if (ar__lit_is(t->ctx, "template"))
            {
                t->mode = M_IN_TEMPLATE;
                return;
            }
            if (ar__lit_is(t->ctx, "html"))
            {
                /*
                 * Step 15 of the reset: an html context with no head element
                 * yet is `before head`, and a fragment never has one. So
                 * `<body><span>` parsed against `html` grows a head *and* a
                 * body, which is what the suite asks for and what `in body`
                 * cannot produce.
                 */
                t->mode = t->head < 0 ? M_BEFORE_HEAD : M_AFTER_HEAD;
                return;
            }
            if (ar__lit_is(t->ctx, "head") || ar__lit_is(t->ctx, "body"))
            {
                /* A head context is `in body` too: step 12 wants `last` to be
                   false and for a context element it never is. */
                t->mode = M_IN_BODY;
                return;
            }
            if (ar__lit_is(t->ctx, "frameset"))
            {
                t->mode = M_IN_FRAMESET;
                return;
            }
            t->mode = M_IN_BODY;
            return;
        }

        if (t->doc->nodes[node].ns != AR_NS_HTML)
        {
            continue;
        }
        /*
         * A select is checked first, and which select mode it resolves to
         * depends on what is under it: a select standing inside a table
         * resumes `in select in table`, so a table part after it still closes
         * the select rather than being dropped. Walking down for a table is
         * the specification's own step 4, and it stops at a template because
         * a template's contents are their own document.
         */
        if (ar__is(t, node, "select"))
        {
            ar_i32 k;

            t->mode = M_IN_SELECT;
            for (k = i - 1; k >= 1; --k)
            {
                if (ar__is(t, t->open[k], "template"))
                {
                    break;
                }
                if (ar__is(t, t->open[k], "table"))
                {
                    t->mode = M_IN_SELECT_IN_TABLE;
                    break;
                }
            }
            return;
        }
        if ((ar__is(t, node, "td") || ar__is(t, node, "th")) && !last)
        {
            t->mode = M_IN_CELL;
            return;
        }
        if (ar__is(t, node, "tr"))
        {
            t->mode = M_IN_ROW;
            return;
        }
        if (ar__is(t, node, "tbody") || ar__is(t, node, "thead") || ar__is(t, node, "tfoot"))
        {
            t->mode = M_IN_TABLE_BODY;
            return;
        }
        if (ar__is(t, node, "caption"))
        {
            t->mode = M_IN_CAPTION;
            return;
        }
        if (ar__is(t, node, "colgroup"))
        {
            t->mode = M_IN_COLUMN_GROUP;
            return;
        }
        if (ar__is(t, node, "table"))
        {
            t->mode = M_IN_TABLE;
            return;
        }
        if (ar__is(t, node, "template"))
        {
            /* Step 4 of "reset the insertion mode appropriately": the current
               template insertion mode, which is the top of the stack and not
               a value computed from anything visible here. */
            t->mode = t->tmpl_mode_n > 0 ? (int)t->tmpl_mode[t->tmpl_mode_n - 1] : M_IN_TEMPLATE;
            return;
        }
        if (ar__is(t, node, "head") && !last)
        {
            t->mode = M_IN_HEAD;
            return;
        }
        if (ar__is(t, node, "body"))
        {
            t->mode = M_IN_BODY;
            return;
        }
        if (ar__is(t, node, "frameset"))
        {
            t->mode = M_IN_FRAMESET;
            return;
        }
        if (ar__is(t, node, "html"))
        {
            t->mode = t->head < 0 ? M_BEFORE_HEAD : M_AFTER_HEAD;
            return;
        }
        if (last)
        {
            t->mode = M_IN_BODY;
            return;
        }
    }
    t->mode = M_IN_BODY;
}

/*
 * Run a token through `in head` and come back.
 *
 * Four modes route head content here -- `in head noscript`, `in select`,
 * `in frameset` and `after frameset` -- and all four got the same thing wrong
 * the same way. `<style>`, `<script>`, `<noframes>` and `<title>` are raw
 * text, so `in head` leaves the insertion mode as `text` and records in
 * `original_mode` where to come back to. Restoring the mode over that loses
 * the element's contents: `<noscript><style>x</style>` put the x in the body,
 * `<select><script>` lost the script and the select with it, and
 * `<frameset></frameset><noframes>abc` produced an empty noframes.
 *
 * So there is one helper rather than four copies of the mistake.
 */
/*
 * Keep the whitespace in a run and count everything else as an error.
 *
 * `in frameset` and `after frameset` both say "a character token that is
 * whitespace: insert the character" and "anything else: parse error". The
 * specification emits one character token per character, so it inserts the
 * spaces in `<frameset> te st` and drops the letters between them -- the
 * frameset ends up with two spaces in it and no words.
 *
 * This tokenizer coalesces runs, which is right everywhere else, so the run
 * has to be sieved here. Each whitespace stretch is inserted on its own and
 * they join into one text node, because ar__insert_text appends to the
 * previous one when it is still the last thing in the text arena.
 *
 * Truncating at the first non-whitespace character is the version that looks
 * right and is not: it keeps the leading space of ` te st` and loses the one
 * in the middle.
 */
static void ar__keep_only_space(ar__tree *t, const ar_token *tok)
{
    ar_u32 i = 0;

    while (i < tok->text.n)
    {
        ar_u32 begin = i;

        while (i < tok->text.n && (ar__space_char(tok->text.p[i]) || tok->text.p[i] == 0))
        {
            ++i;
        }
        if (i > begin)
        {
            ar_span run;

            run.p = tok->text.p + begin;
            run.n = i - begin;
            ar__insert_text(t, run, 0);
            continue;
        }
        while (i < tok->text.n && !ar__space_char(tok->text.p[i]) && tok->text.p[i] != 0)
        {
            ++i;
        }
        t->doc->errors++;
    }
}

static void ar__via_head(ar__tree *t, const ar_token *tok, int back)
{
    t->mode = M_IN_HEAD;
    ar__process_mode(t, tok);
    if (t->mode == M_TEXT)
    {
        t->original_mode = back;
    }
    else if (t->mode == M_IN_HEAD)
    {
        t->mode = back;
    }
}

static void ar__in_body(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_TEXT)
    {
        ar__reconstruct(t);
        ar__insert_text(t, tok->text, 1);
        if (!ar__all_space(tok->text))
        {
            t->frameset_ok = 0;
        }
        return;
    }
    if (tok->kind == AR_TOK_COMMENT)
    {
        ar__comment_node(t, tok, -1);
        return;
    }
    if (tok->kind == AR_TOK_DOCTYPE)
    {
        t->doc->errors++; /* a doctype here is ignored */
        return;
    }

    if (tok->kind == AR_TOK_START)
    {
        if (ar_span_is(tok->name, "html"))
        {
            t->doc->errors++;
            if (!ar__on_stack_named(t, "template"))
            {
                ar__merge_attrs(t, ar_dom_root(t->doc), tok);
            }
            return;
        }
        if (ar_span_is(tok->name, "body"))
        {
            t->doc->errors++;
            if (t->open_n >= 3 && ar__is(t, t->open[2], "body") &&
                !ar__on_stack_named(t, "template"))
            {
                t->frameset_ok = 0;
                ar__merge_attrs(t, t->open[2], tok);
            }
            return;
        }
        if (ar_span_is(tok->name, "head"))
        {
            t->doc->errors++;
            return;
        }

        /*
         * `<frameset>` in the body, §13.2.6.4.7.
         *
         * A document may still become a frameset document after the body has
         * opened, and it does so by throwing the body away: the second element
         * on the stack is removed from its parent, everything below the root
         * is popped, and the frameset takes the body's place. So
         * `<div><frameset>` is a frameset document with no div in it, which
         * looks like data loss and is what every browser does.
         *
         * The frameset-ok flag is what stops it once the document has
         * committed to having content. It starts set and the tags in
         * ar__clears_frameset put it out -- so `<br><frameset>` keeps the
         * body and drops the frameset, while `<param><frameset>` does the
         * opposite, and the difference between those two is a list rather
         * than a principle.
         *
         * The fragment guard is the specification's own: a fragment has no
         * body as the second element on the stack, so there is nothing to
         * replace and the token is dropped.
         */
        if (ar_span_is(tok->name, "frameset"))
        {
            t->doc->errors++;

            /*
             * open[0] is the document and open[1] is the root html element, so
             * the specification's "second element on the stack" is open[2] and
             * "up to but not including the root html element" leaves two.
             */
            if (t->open_n < 3 || !ar__is(t, t->open[2], "body") || !t->frameset_ok)
            {
                return;
            }
            ar__detach(t, t->open[2]);
            while (t->open_n > 2)
            {
                ar__pop(t);
            }
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_FRAMESET;
            return;
        }
        /*
         * The flag test first, and it is not a micro-optimisation.
         *
         * ar__clears_frameset walks a twenty-one entry name list, and this
         * runs for every start tag in the body. The flag starts set and is put
         * out by the first piece of visible content in the document, so after
         * a handful of tags the answer can no longer change -- and the walk is
         * pure cost for the rest of the page. Measured on `html_page`: 377 to
         * 355 microseconds, a third of this release's parse regression for one
         * `&&`.
         */
        if (t->frameset_ok && ar__clears_frameset(tok))
        {
            t->frameset_ok = 0;
        }
        if (ar_span_is(tok->name, "pre") || ar_span_is(tok->name, "listing") ||
            ar_span_is(tok->name, "textarea"))
        {
            t->drop_lf = 1;
        }
        if (ar_span_is(tok->name, "table"))
        {
            /*
             * A `<table>` closes an open paragraph -- unless the document is
             * in quirks mode, where it does not, and `<p><table>` puts the
             * table *inside* the paragraph.
             *
             * The condition is written into the specification's own rule for
             * this one tag, and it is the only place quirks mode changes tree
             * construction rather than layout. A document with no doctype is
             * in quirks mode, so this is not a legacy corner: it is what
             * happens to any page that forgot the first line.
             */
            if (t->doc->quirks != AR_QUIRKS_YES)
            {
                ar__close_p(t);
            }
            ar__insert_element(t, tok, 1);
            t->mode = M_IN_TABLE;
            return;
        }
        /*
         * A table part with no table around it is ignored -- the element is
         * not created at all, though its contents still are.
         *
         * `<td>orphan` is a paragraph of text in every browser, not a cell
         * floating in the body, and a cell outside a table would be laid out
         * by the table formatting context with nothing to belong to.
         */
        if (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th") ||
            ar_span_is(tok->name, "tr") || ar_span_is(tok->name, "tbody") ||
            ar_span_is(tok->name, "tfoot") || ar_span_is(tok->name, "thead") ||
            ar_span_is(tok->name, "caption") || ar_span_is(tok->name, "col") ||
            ar_span_is(tok->name, "colgroup") || ar_span_is(tok->name, "frame"))
        {
            t->doc->errors++;
            return;
        }
        if (ar_span_is(tok->name, "li"))
        {
            /* An open <li> closes before a new one opens, which is why
               `<li>a<li>b` is two items rather than one inside the other. */
            ar_i32 i;

            for (i = t->open_n - 1; i >= 1; --i)
            {
                if (ar__is(t, t->open[i], "li"))
                {
                    ar__implied_end_tags(t, "li");
                    ar__pop_until(t, "li");
                    break;
                }
                if (ar__closes_p(t->doc->nodes[t->open[i]].name) &&
                    !ar__is(t, t->open[i], "address") && !ar__is(t, t->open[i], "div") &&
                    !ar__is(t, t->open[i], "p"))
                {
                    break;
                }
            }
            ar__close_p(t);
            ar__insert_element(t, tok, 1);
            return;
        }
        if (ar_span_is(tok->name, "dd") || ar_span_is(tok->name, "dt"))
        {
            ar_i32 i;

            for (i = t->open_n - 1; i >= 1; --i)
            {
                if (ar__is(t, t->open[i], "dd") || ar__is(t, t->open[i], "dt"))
                {
                    ar__implied_end_tags(t, ar__is(t, t->open[i], "dd") ? "dd" : "dt");
                    ar__pop_until(t, ar__is(t, ar__current(t), "dd") ? "dd" : "dt");
                    break;
                }
            }
            ar__close_p(t);
            ar__insert_element(t, tok, 1);
            return;
        }
        /*
         * Head content is head content wherever it appears, §13.2.6.4.7.
         *
         * `<base>`, `<basefont>`, `<bgsound>`, `<link>` and `<meta>` reaching
         * `in body` are handed to the `in head` rules rather than inserted
         * here, and the difference is the reconstruction: the void branch
         * below reopens the active formatting elements first, so
         * `<a><b></a><basefont>` put the basefont inside a reopened `<a>`.
         * `in head` does not reconstruct, and the specification is explicit
         * that these go there.
         *
         * `noframes`, `script`, `style`, `template` and `title` are on the
         * same list and already have branches of their own below.
         */
        if (ar_span_is(tok->name, "base") || ar_span_is(tok->name, "basefont") ||
            ar_span_is(tok->name, "bgsound") || ar_span_is(tok->name, "link") ||
            ar_span_is(tok->name, "meta"))
        {
            ar__via_head(t, tok, t->mode);
            return;
        }
        if (ar__is_void(tok->name))
        {
            /* `<hr>` is the one void element that also closes an open
               paragraph, and this branch runs before the block-level one, so
               it has to do the close itself. `<p>a<hr>b` is a paragraph, a
               rule and a text node -- not all three inside the paragraph. */
            if (ar__closes_p(tok->name))
            {
                ar__close_p(t);
            }
            ar__reconstruct(t);
            ar__insert_element(t, tok, 1);
            ar__pop(t);
            return;
        }
        if (ar__is_formatting(tok->name))
        {
            /*
             * An open <a> is closed by a new one before it opens, which the
             * specification spells out because nested links are meaningless
             * and authors write them by accident constantly.
             */
            if (ar_span_is(tok->name, "a"))
            {
                ar_i32 i;

                for (i = t->fmt_n - 1; i >= 0 && t->fmt[i] >= 0; --i)
                {
                    if (ar__is(t, t->fmt[i], "a"))
                    {
                        t->doc->errors++;
                        ar__adoption(t, "a");
                        break;
                    }
                }
            }
            /*
             * A nobr does the same, and for the reason a nested anchor does:
             * the pair have no meaning nested, the specification closes an
             * open one before opening another, and authors nest them anyway.
             */
            if (ar_span_is(tok->name, "nobr") && ar__in_scope(t, "nobr", 0))
            {
                t->doc->errors++;
                ar__adoption(t, "nobr");
            }
            ar__reconstruct(t);
            {
                ar_i32 node = ar__insert_element(t, tok, 1);

                if (node >= 0)
                {
                    ar__fmt_push(t, node);
                }
            }
            return;
        }
        /*
         * `<image>` is `<img>`. Not a typo in this file -- the specification
         * says so outright, because an early browser shipped the wrong name
         * and enough documents used it that the mistake had to be kept.
         */
        if (ar_span_is(tok->name, "image"))
        {
            ar_token fixed = *tok;

            fixed.name.p = "img";
            fixed.name.n = 3;
            ar__reconstruct(t);
            ar__insert_element(t, &fixed, 1);
            ar__pop(t);
            return;
        }
        /* A heading closes an open heading rather than nesting inside it. */
        if (ar_span_is(tok->name, "h1") || ar_span_is(tok->name, "h2") ||
            ar_span_is(tok->name, "h3") || ar_span_is(tok->name, "h4") ||
            ar_span_is(tok->name, "h5") || ar_span_is(tok->name, "h6"))
        {
            ar__close_p(t);
            if (ar__is(t, ar__current(t), "h1") || ar__is(t, ar__current(t), "h2") ||
                ar__is(t, ar__current(t), "h3") || ar__is(t, ar__current(t), "h4") ||
                ar__is(t, ar__current(t), "h5") || ar__is(t, ar__current(t), "h6"))
            {
                t->doc->errors++;
                ar__pop(t);
            }
            ar__insert_element(t, tok, 1);
            return;
        }
        /*
         * `<select>` opens a mode of its own, and which one depends on where
         * it stands: inside a table it is `in select in table`, so a stray
         * `<tr>` afterwards closes the select and belongs to the table rather
         * than being dropped.
         */
        if (ar_span_is(tok->name, "select"))
        {
            int in_table = t->mode == M_IN_TABLE || t->mode == M_IN_CAPTION ||
                           t->mode == M_IN_TABLE_BODY || t->mode == M_IN_ROW ||
                           t->mode == M_IN_CELL;

            ar__reconstruct(t);
            ar__insert_element(t, tok, 1);
            t->frameset_ok = 0;
            t->mode = in_table ? M_IN_SELECT_IN_TABLE : M_IN_SELECT;
            return;
        }

        /* And so do an option, an optgroup and a button. */
        if (ar_span_is(tok->name, "option") || ar_span_is(tok->name, "optgroup"))
        {
            if (ar__is(t, ar__current(t), "option"))
            {
                ar__pop(t);
            }
            if (ar_span_is(tok->name, "optgroup") && ar__is(t, ar__current(t), "optgroup"))
            {
                ar__pop(t);
            }
            ar__reconstruct(t);
            ar__insert_element(t, tok, 1);
            return;
        }
        if (ar_span_is(tok->name, "button"))
        {
            if (ar__in_scope(t, "button", 0))
            {
                t->doc->errors++;
                ar__implied_end_tags(t, 0);
                ar__pop_until(t, "button");
            }
            ar__reconstruct(t);
            ar__insert_element(t, tok, 1);
            return;
        }
        /*
         * A second `<form>` is ignored while one is open. The specification
         * keeps a form element *pointer* for exactly this, because forms may
         * not nest and authors nest them constantly.
         */
        if (ar_span_is(tok->name, "form"))
        {
            if (t->form >= 0)
            {
                t->doc->errors++;
                return;
            }
            ar__close_p(t);
            t->form = ar__insert_element(t, tok, 1);
            return;
        }
        /*
         * The rawtext elements: everything inside them is text, and a `<` is
         * not a tag. `<iframe><p>x</iframe>` holds the five characters `<p>x`,
         * which is what makes a fallback inside an iframe invisible rather
         * than part of the page.
         */
        if (ar_span_is(tok->name, "iframe") || ar_span_is(tok->name, "xmp") ||
            ar_span_is(tok->name, "noembed") || ar_span_is(tok->name, "noframes"))
        {
            if (ar_span_is(tok->name, "xmp"))
            {
                ar__close_p(t);
            }
            ar__insert_element(t, tok, 1);
            t->tok->state = AR_HTML_RAWTEXT;
            t->original_mode = t->mode;
            t->mode = M_TEXT;
            return;
        }
        /* `<plaintext>` takes the rest of the file, tags included. It is from
           1994, it cannot be closed, and it is two lines rather than a special
           case somewhere else. */
        if (ar_span_is(tok->name, "plaintext"))
        {
            ar__close_p(t);
            ar__insert_element(t, tok, 1);
            t->tok->state = AR_HTML_PLAINTEXT;
            return;
        }
        if (ar_span_is(tok->name, "textarea"))
        {
            ar__insert_element(t, tok, 0);
            t->tok->state = AR_HTML_RCDATA;
            t->original_mode = t->mode;
            t->mode = M_TEXT;
            return;
        }
        if (ar_span_is(tok->name, "style") || ar_span_is(tok->name, "script") ||
            ar_span_is(tok->name, "title"))
        {
            ar__insert_element(t, tok, 0);
            /*
             * A script is script data, not raw text.
             *
             * The two look interchangeable -- neither reads tags, both end at
             * their own end tag -- and they are not: script data has the
             * escaped and double-escaped states, which exist so that
             * `<script><!--<script </script>` does not end the element at the
             * inner tag. Tokenizing a script as RAWTEXT skips all of that,
             * and the escape tracking in ar_html_token.c never ran once
             * because nothing ever put the tokenizer in the state it keys on.
             */
            t->tok->state =
                ar_span_is(tok->name, "script")
                    ? AR_HTML_SCRIPT
                    : (ar_span_is(tok->name, "title") ? AR_HTML_RCDATA : AR_HTML_RAWTEXT);
            t->original_mode = t->mode;
            t->mode = M_TEXT;
            return;
        }

        /*
         * A template is head content wherever it appears, and `in body` has to
         * say so or the mode never changes.
         *
         * The element was being inserted and given its content fragment --
         * that part is in ar__insert_element and works anywhere -- but the
         * insertion mode stayed `in body`, so `</template>` never reached the
         * rule that closes it and fell through to the general end-tag walk
         * instead. That walk correctly refuses to cross a `<div>`, so
         * `<div><template><div><span></template><b>` put the `<b>` inside the
         * span and left the template open forever.
         */
        if (ar_span_is(tok->name, "template"))
        {
            t->mode = M_IN_HEAD;
            ar__process_mode(t, tok);
            return;
        }

        /*
         * Ruby, §13.2.6.4.7. `<rb>` and `<rtc>` generate implied end tags;
         * `<rt>` and `<rp>` generate them *except* for `rtc`, which is what
         * lets a `<rtc>` hold several `<rt>`s.
         *
         * Without these, `<ruby>a<rb>b<rb>` puts the second `<rb>` inside the
         * first, because nothing closed it.
         */
        if (ar_span_is(tok->name, "rb") || ar_span_is(tok->name, "rtc"))
        {
            if (ar__in_scope(t, "ruby", 0))
            {
                ar__implied_end_tags(t, 0);
            }
            ar__insert_element(t, tok, 1);
            return;
        }
        if (ar_span_is(tok->name, "rt") || ar_span_is(tok->name, "rp"))
        {
            if (ar__in_scope(t, "ruby", 0))
            {
                ar__implied_end_tags(t, "rtc");
            }
            ar__insert_element(t, tok, 1);
            return;
        }
        /*
         * `<svg>` and `<math>` are the two doors into foreign content, and
         * they are the only ones: nothing else in an HTML document changes
         * namespace. Everything after them is decided by where it sits, not by
         * what it says -- see ar__use_insertion_mode.
         */
        if (ar_span_is(tok->name, "svg") || ar_span_is(tok->name, "math"))
        {
            ar_ns  ns = ar_span_is(tok->name, "svg") ? AR_NS_SVG : AR_NS_MATHML;
            ar_i32 node;

            ar__reconstruct(t);
            /* Foster parented like any other in-body insertion: a <math> in
               a table goes before the table, not inside it. */
            node = ar__insert_foreign(t, tok, ns, 1);
            if (node >= 0 && tok->self_closing)
            {
                ar__pop(t);
            }
            return;
        }
        if (ar__closes_p(tok->name))
        {
            ar__close_p(t);
            ar__insert_element(t, tok, 1);
            return;
        }
        ar__reconstruct(t);
        ar__insert_element(t, tok, 1);
        return;
    }

    /* End tags. */

    /*
     * `</br>` is a `<br>`, §13.2.6.4.7, with its attributes thrown away.
     *
     * Not a curiosity: authors write it, and the specification says so in
     * those words -- "drop the attributes from the token, and act as described
     * in the next entry", which is the `<br>` start tag. Without it the tag
     * reached the general end-tag walk, found no `br` on the stack, stopped at
     * the first special element and did nothing at all.
     */
    if (ar_span_is(tok->name, "br"))
    {
        ar_token fixed;

        t->doc->errors++;
        memset(&fixed, 0, sizeof fixed);
        fixed.kind = AR_TOK_START;
        fixed.name = tok->name;
        ar__reconstruct(t);
        ar__insert_element(t, &fixed, 1);
        ar__pop(t);
        t->frameset_ok = 0;
        return;
    }

    if (ar_span_is(tok->name, "body") || ar_span_is(tok->name, "html"))
    {
        t->mode = M_AFTER_BODY;
        if (ar_span_is(tok->name, "html"))
        {
            ar__process(t, tok); /* reprocessed in the new mode */
        }
        return;
    }
    if (ar_span_is(tok->name, "form"))
    {
        t->form = -1;
        /* and then closed like any other element, below */
    }
    if (ar_span_is(tok->name, "p"))
    {
        if (!ar__in_scope(t, "p", 1))
        {
            /* No open paragraph, so one is created and immediately closed. */
            t->doc->errors++;
            ar__insert_implied(t, "p");
            ar__pop(t);
            return;
        }
        ar__close_p(t);
        return;
    }
    if (ar__is_formatting(tok->name))
    {
        char   tag[16];
        ar_u32 n = tok->name.n < 15 ? tok->name.n : 15;
        ar_u32 k;

        for (k = 0; k < n; ++k)
        {
            char c = tok->name.p[k];

            tag[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        tag[n] = 0;
        if (!ar__adoption(t, tag))
        {
            t->doc->errors++;
        }
        return;
    }

    /*
     * `</h1>` to `</h6>`, §13.2.6.4.7, and the rule is that *any* heading
     * closes *any* heading.
     *
     * `<h1><div><h3><span></h1>` closes the h3, not the h1: the end tag names
     * one level and the algorithm pops through whichever heading it finds
     * first. Treating it as an ordinary end tag looks for an `h1` in
     * particular, walks past the h3, and stops at the div because a div is
     * special -- so the tag did nothing at all.
     */
    if (ar_span_is(tok->name, "h1") || ar_span_is(tok->name, "h2") || ar_span_is(tok->name, "h3") ||
        ar_span_is(tok->name, "h4") || ar_span_is(tok->name, "h5") || ar_span_is(tok->name, "h6"))
    {
        static const char *const H[] = {"h1", "h2", "h3", "h4", "h5", "h6", 0};
        ar_i32                   i;

        for (i = t->open_n - 1; i >= 1; --i)
        {
            if (ar__node_name_in(t, t->open[i], H))
            {
                break;
            }
        }
        if (i < 1)
        {
            t->doc->errors++;
            return;
        }
        ar__implied_end_tags(t, 0);
        while (t->open_n > i)
        {
            ar__pop(t);
        }
        return;
    }

    /* `</template>` is head content too, and for the same reason. */
    if (ar_span_is(tok->name, "template"))
    {
        t->mode = M_IN_TEMPLATE;
        ar__process_mode(t, tok);
        return;
    }

    /*
     * An end tag for a block element, §13.2.6.4.7.
     *
     * `</div>`, `</section>`, `</blockquote>` and the rest of the list have a
     * rule of their own, and it is not the same as the general walk below: it
     * generates implied end tags first, so `<div><p>x</div>` closes the
     * paragraph on the way out instead of tripping over it.
     *
     * These had been going through the general walk, which worked only because
     * that walk did not stop at special elements. Once it did -- which the
     * specification requires, and which is what keeps a stray `</div>` inside
     * a paragraph from closing the div and everything between -- thirty
     * conformance cases turned red at once and said so.
     */
    if ((ar__closes_p(tok->name) || ar_span_is(tok->name, "button")) &&
        !ar_span_is(tok->name, "p") && !ar_span_is(tok->name, "form") &&
        !ar_span_is(tok->name, "table") && !ar_span_is(tok->name, "hr"))
    {
        char   name[32];
        ar_u32 n = tok->name.n < sizeof name - 1u ? tok->name.n : (ar_u32)sizeof name - 1u;
        ar_u32 k;

        for (k = 0; k < n; ++k)
        {
            name[k] = tok->name.p[k];
        }
        name[n] = 0;

        if (!ar__in_scope(t, name, 0))
        {
            t->doc->errors++;
            return;
        }
        ar__implied_end_tags(t, 0);
        if (!ar__span_eq(t->doc->nodes[ar__current(t)].name, tok->name))
        {
            t->doc->errors++;
        }
        ar__pop_until(t, name);
        return;
    }

    /*
     * "Any other end tag", §13.2.6.4.7.
     *
     * Walk the stack for an element with this name and close through to it --
     * but stop at the first element in the special category, which is what
     * makes a stray `</div>` inside a paragraph harmless instead of
     * destructive. Without that stop the walk keeps going, finds the div, and
     * closes everything between.
     */
    {
        ar_i32 i;

        for (i = t->open_n - 1; i >= 1; --i)
        {
            if (t->doc->nodes[t->open[i]].kind != AR_DOM_ELEMENT)
            {
                continue;
            }
            if (t->doc->nodes[t->open[i]].ns == AR_NS_HTML &&
                ar__span_eq(t->doc->nodes[t->open[i]].name, tok->name))
            {
                while (t->open_n > i + 1)
                {
                    ar__pop(t);
                }
                ar__pop(t);
                return;
            }
            if (ar__is_special(t, t->open[i]))
            {
                t->doc->errors++;
                return;
            }
        }
        t->doc->errors++;
    }
}

/* ------------------------------------------------------------------------
 * Tables
 *
 * The table modes are what foster parenting exists for: a token that may not
 * be in a table is relocated rather than dropped, and every browser agrees
 * because they all implement the same paragraph.
 * ------------------------------------------------------------------------ */
/*
 * "Clear the stack back to a table context", §13.2.6.4.9, and its two
 * siblings for a table body and a table row.
 *
 * Three sentences in the specification and the reason four browser-corpus
 * cases disagreed. Anything the table fostered out -- a stray `<b>`, an
 * `<em>`, an `<a>` -- is relocated in the *tree* but stays on the stack of
 * open elements, so it is still the current node when the next table part
 * arrives. Without this, `<table><b><td>` puts the implied tbody inside the
 * bold, and the whole table is built in the wrong place while looking
 * perfectly well-formed.
 *
 * The `html` at the end of every list is the floor: the stack always has the
 * document and the html element under everything, and popping past them would
 * leave nowhere to insert into.
 */
static void ar__clear_stack_to(ar__tree *t, const char *const *keep)
{
    while (t->open_n > 1)
    {
        ar_i32 i;

        for (i = 0; keep[i]; ++i)
        {
            if (ar__is(t, ar__current(t), keep[i]))
            {
                return;
            }
        }
        ar__pop(t);
    }
}

static void ar__clear_to_table(ar__tree *t)
{
    static const char *const KEEP[] = {"table", "template", "html", 0};

    ar__clear_stack_to(t, KEEP);
}

static void ar__clear_to_table_body(ar__tree *t)
{
    static const char *const KEEP[] = {"tbody", "tfoot", "thead", "template", "html", 0};

    ar__clear_stack_to(t, KEEP);
}

static void ar__clear_to_table_row(ar__tree *t)
{
    static const char *const KEEP[] = {"tr", "template", "html", 0};

    ar__clear_stack_to(t, KEEP);
}

static void ar__in_table(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_COMMENT)
    {
        ar__comment_node(t, tok, -1);
        return;
    }
    if (tok->kind == AR_TOK_TEXT)
    {
        /* Text in a table is foster parented unless it is only whitespace. */
        if (ar__all_space(tok->text))
        {
            ar__insert_text(t, tok->text, 0);
        }
        else
        {
            t->doc->errors++;
            ar__reconstruct(t);
            ar__insert_text(t, tok->text, 1);
        }
        return;
    }
    if (tok->kind == AR_TOK_START)
    {
        if (ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
            ar_span_is(tok->name, "thead"))
        {
            ar__clear_to_table(t);
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_TABLE_BODY;
            return;
        }
        if (ar_span_is(tok->name, "tr"))
        {
            ar__clear_to_table(t);
            ar__insert_implied(t, "tbody");
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_ROW;
            return;
        }
        if (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th"))
        {
            ar__clear_to_table(t);
            ar__insert_implied(t, "tbody");
            ar__insert_implied(t, "tr");
            ar__insert_element(t, tok, 0);
            ar__fmt_marker(t);
            t->mode = M_IN_CELL;
            return;
        }
        if (ar_span_is(tok->name, "caption"))
        {
            ar__clear_to_table(t);
            ar__insert_element(t, tok, 0);
            ar__fmt_marker(t);
            t->mode = M_IN_CAPTION;
            return;
        }
        /*
         * Head content inside a table stays inside it. 13.2.6.4.9 gives
         * `style`, `script` and `template` a rule of their own -- "process the
         * token using the rules for the in head insertion mode" -- so they are
         * not the "anything else" that gets fostered out, and
         * `<table><style> <tr>x </style>` keeps its stylesheet in the table
         * where the author put it.
         *
         * Reached through the fall-through from `in table body` and `in row`
         * as well, so the mode to come back to is whichever one asked, not
         * `in table`.
         */
        if (ar_span_is(tok->name, "style") || ar_span_is(tok->name, "script"))
        {
            ar__via_head(t, tok, t->mode);
            return;
        }
        /* A template inside a table stays inside it, for the same reason. */
        if (ar_span_is(tok->name, "template"))
        {
            ar__insert_element(t, tok, 0);
            ar__fmt_marker(t);
            t->original_mode = M_IN_TABLE;
            /* The second place a template is inserted, and the one that was
               missing its push: without it the mode stack is a rule behind
               the element stack, and every `in template` rule inside a table
               fired against an empty stack and changed nothing. */
            ar__tmpl_push(t, M_IN_TEMPLATE);
            t->mode = M_IN_TEMPLATE;
            return;
        }
        if (ar_span_is(tok->name, "colgroup"))
        {
            ar__clear_to_table(t);
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_COLUMN_GROUP;
            return;
        }
        if (ar_span_is(tok->name, "col"))
        {
            /* A `<col>` with no `<colgroup>` around it gets one, the same way
               a `<td>` gets a tbody and a row. The column then belongs to a
               group in the tree, which is what the table layout expects and
               what `<table><col>` produces in every browser. */
            ar__clear_to_table(t);
            ar__insert_implied(t, "colgroup");
            t->mode = M_IN_COLUMN_GROUP;
            ar__process(t, tok);
            return;
        }

        /*
         * `<input type=hidden>` is the one element a table does not foster out.
         *
         * It draws nothing, so relocating it before the table would be pure
         * damage to the document order for no visible gain -- and the
         * specification says so explicitly rather than leaving it to the
         * general rule. Five conformance cases, all of them writing the type
         * in a different case, which is the actual point being made.
         */
        if (ar_span_is(tok->name, "input") && !ar__clears_frameset(tok))
        {
            t->doc->errors++;
            ar__insert_element(t, tok, 0);
            ar__pop(t);
            return;
        }

        /*
         * A `<form>` in a table is inserted where it stands rather than
         * fostered out, and then popped straight off the stack -- so it holds
         * nothing at all and the rows that follow are still the table's.
         * A second one is ignored, because the form pointer is already set.
         *
         * The oddity is the specification's, not a simplification: a form is
         * the one element that may not nest, and a table is where authors put
         * one by accident most often.
         */
        if (ar_span_is(tok->name, "form"))
        {
            t->doc->errors++;
            if (t->form >= 0 || ar__on_stack_named(t, "template"))
            {
                return;
            }
            t->form = ar__insert_element(t, tok, 0);
            ar__pop(t);
            return;
        }
        if (ar_span_is(tok->name, "table"))
        {
            /*
             * A table inside a table closes the first, which is the recovery
             * for the single most common malformed table on the web -- but
             * only if there is a first. Parsing `<table><tr>` against a
             * `table` context there is no table element on the stack at all,
             * so the tag is dropped and the row gets its implied tbody; the
             * unconditional version built a second table inside the fragment.
             */
            static const char *const TABLE[] = {"table", 0};

            t->doc->errors++;
            if (!ar__in_table_scope(t, TABLE))
            {
                return;
            }
            ar__pop_until(t, "table");
            ar__reset_mode(t);
            ar__process(t, tok);
            return;
        }
        /* Anything else is foster parented. */
        t->doc->errors++;
        {
            int was = t->foster;

            t->foster = 1;
            ar__in_body(t, tok);
            t->foster = was;
        }
        return;
    }
    if (tok->kind == AR_TOK_END)
    {
        /* 13.2.6.4.9's ignore list, all eleven of it. `body` and `html` were
           missing, and they are the two that do damage rather than nothing:
           `</body>` reaches `in body`, which switches to `after body`, so
           `<table></body></tbody><td>` left the table behind and put the cell
           at the end of the document. */
        static const char *const STRUCTURAL[] = {"body", "caption", "col", "colgroup",
                                                 "html", "tbody",   "td",  "tfoot",
                                                 "th",   "thead",   "tr",  0};
        ar_i32                   i;

        if (ar_span_is(tok->name, "table"))
        {
            /*
             * Ignored when there is no table to close, and the mode is left
             * alone -- which is the part that mattered. `ar__pop_until` was
             * already a no-op here, so the tree was right and the *mode* was
             * not: it went to `in body`, and the `<tr>` after `</table>` in a
             * `table` fragment was then a table part with no table and was
             * dropped. The document came out empty.
             */
            static const char *const TABLE[] = {"table", 0};

            if (!ar__in_table_scope(t, TABLE))
            {
                t->doc->errors++;
                return;
            }
            ar__pop_until(t, "table");
            ar__reset_mode(t);
            return;
        }
        for (i = 0; STRUCTURAL[i]; ++i)
        {
            if (ar_span_is(tok->name, STRUCTURAL[i]))
            {
                /* A structural end tag with nothing open to match it. Ignored,
                   which is what the specification says and what stops
                   `</td>` outside a row from closing something else. */
                t->doc->errors++;
                return;
            }
        }
        /*
         * Anything else goes to `in body`, foster parenting enabled -- and it
         * has to, because dropping it leaves the element it would have closed
         * open on the stack.
         *
         * This branch used to drop every end tag but `</table>`, and the cost
         * was not the missing close: `<table><em>x</em><tr><td>y` left the
         * `<em>` open, so the implied tbody and the row were inserted *into
         * the emphasis* and the table came out empty. The corpus check that
         * only asked whether `em` appeared before `table` passed on that tree.
         */
        {
            int was = t->foster;

            t->foster = 1;
            ar__in_body(t, tok);
            t->foster = was;
        }
        return;
    }
}

/*
 * `in select`, §13.2.6.4.16, and `in select in table`, §13.2.6.4.17.
 *
 * These were the last two named insertion modes with nothing behind them.
 * `<select>` was handled as an ordinary element in `in body`, and the comment
 * in ar__reset_mode said so: "areole has no `in select` mode to name".
 *
 * The mode exists because a select is a closed world. Almost nothing may be
 * inside one -- an option, an optgroup, an `<hr>` between groups, and since
 * the customizable-select work a `<button>` holding a `<selectedcontent>` --
 * and *everything else is dropped* rather than nested. That is the part an
 * ordinary element cannot express: `<select><div>x` keeps the text and throws
 * the div away, and `<select><input>` closes the select entirely rather than
 * putting a field inside it.
 *
 * `in select in table` is the same mode with one extra rule: a table part
 * closes the select first. A select really can contain a stray `<tr>` in
 * source and mean the row to belong to the table around it.
 */
static void ar__select_pop_out(ar__tree *t)
{
    ar__pop_until(t, "select");
    ar__reset_mode(t);
}

static void ar__in_select(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_DOCTYPE)
    {
        t->doc->errors++;
        return;
    }
    if (tok->kind == AR_TOK_START)
    {
        if (ar_span_is(tok->name, "html"))
        {
            ar__in_body(t, tok);
            return;
        }
        if (ar_span_is(tok->name, "option"))
        {
            if (ar__is(t, ar__current(t), "option"))
            {
                ar__pop(t);
            }
            /* A select is ordinary content now, so formatting carries
               into it: `<select><div><i></div><option>` reopens the
               italic around the option. */
            ar__reconstruct(t);
            ar__insert_element(t, tok, 0);
            return;
        }
        if (ar_span_is(tok->name, "optgroup") || ar_span_is(tok->name, "hr"))
        {
            /*
             * An `<hr>` is a separator *between* groups, so it closes an open
             * option and an open optgroup before it lands -- which is what
             * makes `<select><option><hr>` two siblings rather than a rule
             * inside the option.
             */
            if (ar__is(t, ar__current(t), "option"))
            {
                ar__pop(t);
            }
            if (ar__is(t, ar__current(t), "optgroup"))
            {
                ar__pop(t);
            }
            ar__reconstruct(t);
            ar__insert_element(t, tok, 0);
            if (ar_span_is(tok->name, "hr"))
            {
                ar__pop(t);
            }
            return;
        }
        if (ar_span_is(tok->name, "select"))
        {
            /* A second `<select>` closes the first rather than nesting, which
               is why a page with an unclosed select does not swallow the rest
               of its form. */
            t->doc->errors++;
            if (ar__in_scope(t, "select", 0))
            {
                ar__select_pop_out(t);
            }
            return;
        }
        /* `<keygen>` used to close a select and no longer does: the
           relaxation that let a `<div>` live inside one let it in too.
           `<input>` and `<textarea>` still close it. */
        if (ar_span_is(tok->name, "input") || ar_span_is(tok->name, "textarea"))
        {
            t->doc->errors++;
            if (!ar__in_select_scope(t))
            {
                return;
            }
            ar__select_pop_out(t);
            ar__process(t, tok);
            return;
        }
        if (ar_span_is(tok->name, "script") || ar_span_is(tok->name, "template"))
        {
            ar__via_head(t, tok, t->mode);
            return;
        }
        ar__in_body(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END)
    {
        if (ar_span_is(tok->name, "optgroup"))
        {
            if (ar__is(t, ar__current(t), "option") && t->open_n >= 2 &&
                ar__is(t, t->open[t->open_n - 2], "optgroup"))
            {
                ar__pop(t);
            }
            if (ar__is(t, ar__current(t), "optgroup"))
            {
                ar__pop(t);
                return;
            }
            t->doc->errors++;
            return;
        }
        if (ar_span_is(tok->name, "option"))
        {
            if (ar__is(t, ar__current(t), "option"))
            {
                ar__pop(t);
                return;
            }
            t->doc->errors++;
            return;
        }
        if (ar_span_is(tok->name, "select"))
        {
            if (!ar__in_select_scope(t))
            {
                t->doc->errors++; /* fragment case */
                return;
            }
            ar__select_pop_out(t);
            return;
        }
        if (ar_span_is(tok->name, "template"))
        {
            ar__via_head(t, tok, t->mode);
            return;
        }
        ar__in_body(t, tok);
        return;
    }
    ar__in_body(t, tok);
}

static void ar__in_select_in_table(ar__tree *t, const ar_token *tok)
{
    static const char *const PARTS[] = {"caption", "table", "tbody", "tfoot", "thead",
                                        "tr",      "td",    "th",    0};

    if ((tok->kind == AR_TOK_START || tok->kind == AR_TOK_END) && ar__name_in(tok->name, PARTS))
    {
        t->doc->errors++;
        if (tok->kind == AR_TOK_END)
        {
            char        name[16];
            ar_u32      n = tok->name.n < sizeof name - 1u ? tok->name.n : (ar_u32)sizeof name - 1u;
            ar_u32      k;
            const char *one[2];

            for (k = 0; k < n; ++k)
            {
                name[k] = tok->name.p[k];
            }
            name[n] = 0;
            one[0] = name;
            one[1] = 0;
            if (!ar__in_table_scope(t, one))
            {
                return;
            }
        }
        ar__select_pop_out(t);
        ar__process(t, tok);
        return;
    }
    ar__in_select(t, tok);
}

/*
 * `in column group`, §13.2.6.4.12.
 *
 * A mode of six lines that had none at all: `M_IN_COLUMN_GROUP` was in the
 * enum, was switched to, and had no case in the dispatcher -- so everything
 * inside a `<colgroup>` fell through to whatever the default was.
 * `<table><colgroup>foo` lost the text and `<table><col foo=bar>` lost the
 * attribute and the group.
 *
 * The shape is the same as every other table mode: a short list of things that
 * belong here, and anything else closes the group and is reprocessed one level
 * out. The closing is the part worth stating -- a `<colgroup>` has no end tag
 * in practice, so almost every real document leaves through "anything else".
 */
static void ar__in_column_group(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_TEXT)
    {
        /*
         * 13.2.6.4.12 takes the whitespace and hands the rest back, which
         * means splitting the token: the tokenizer emits `" foo"` as one
         * character run, and only its leading spaces belong in the column
         * group. Treating it as indivisible fostered the spaces out with the
         * word, so `<colgroup> foo</colgroup>` lost the one character that
         * was in the right place already.
         */
        ar_u32 lead = 0;

        while (lead < tok->text.n && (tok->text.p[lead] == 0 || ar__space_char(tok->text.p[lead])))
        {
            lead++;
        }
        if (lead > 0)
        {
            ar_span space;

            space.p = tok->text.p;
            space.n = lead;
            ar__insert_text(t, space, 0);
        }
        if (lead == tok->text.n)
        {
            return;
        }
        /* The rest closes the group and is reprocessed in `in table`, which
           is where it gets fostered out to the body. */
        if (!ar__is(t, ar__current(t), "colgroup"))
        {
            t->doc->errors++;
            return;
        }
        {
            ar_token rest = *tok;

            rest.text.p = tok->text.p + lead;
            rest.text.n = tok->text.n - lead;
            ar__pop(t);
            t->mode = M_IN_TABLE;
            ar__process(t, &rest);
        }
        return;
    }
    if (tok->kind == AR_TOK_COMMENT)
    {
        ar__comment_node(t, tok, -1);
        return;
    }
    if (tok->kind == AR_TOK_DOCTYPE)
    {
        t->doc->errors++;
        return;
    }
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "html"))
    {
        ar__in_body(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "col"))
    {
        ar__insert_element(t, tok, 0);
        ar__pop(t);
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "colgroup"))
    {
        if (!ar__is(t, ar__current(t), "colgroup"))
        {
            t->doc->errors++;
            return;
        }
        ar__pop(t);
        t->mode = M_IN_TABLE;
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "col"))
    {
        t->doc->errors++;
        return;
    }
    if (ar_span_is(tok->name, "template"))
    {
        t->mode = M_IN_TEMPLATE;
        ar__process_mode(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_EOF)
    {
        ar__in_body(t, tok);
        return;
    }

    /* Anything else closes the group -- unless there is no group to close,
       in which case the token is dropped rather than escaping into the
       table, which is what a fragment parsed against `colgroup` needs. */
    if (!ar__is(t, ar__current(t), "colgroup"))
    {
        t->doc->errors++;
        return;
    }
    ar__pop(t);
    t->mode = M_IN_TABLE;
    ar__process(t, tok);
}

static void ar__in_table_body(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "tr"))
    {
        ar__clear_to_table_body(t);
        ar__insert_element(t, tok, 0);
        t->mode = M_IN_ROW;
        return;
    }
    if (tok->kind == AR_TOK_START && (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th")))
    {
        ar__clear_to_table_body(t);
        ar__insert_implied(t, "tr");
        ar__insert_element(t, tok, 0);
        ar__fmt_marker(t);
        t->mode = M_IN_CELL;
        return;
    }
    if (tok->kind == AR_TOK_START &&
        (ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
         ar_span_is(tok->name, "thead") || ar_span_is(tok->name, "caption") ||
         ar_span_is(tok->name, "col") || ar_span_is(tok->name, "colgroup")))
    {
        static const char *const BODY[] = {"tbody", "thead", "tfoot", 0};

        if (!ar__in_table_scope(t, BODY))
        {
            t->doc->errors++;
            return;
        }
        ar__clear_to_table_body(t);
        ar__pop(t);
        t->mode = M_IN_TABLE;
        ar__process(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END &&
        (ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
         ar_span_is(tok->name, "thead")))
    {
        ar__pop_until(t, "tbody");
        t->mode = M_IN_TABLE;
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "table"))
    {
        /* A `</table>` with no table on the stack is dropped rather than
           popping the fragment root out from under the document. */
        static const char *const TABLE[] = {"table", 0};

        if (!ar__in_table_scope(t, TABLE))
        {
            t->doc->errors++;
            return;
        }
        ar__pop_until(t, "table");
        ar__reset_mode(t);
        return;
    }
    ar__in_table(t, tok);
}

static void ar__in_row(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_START && (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th")))
    {
        ar__clear_to_table_row(t);
        ar__insert_element(t, tok, 0);
        ar__fmt_marker(t);
        t->mode = M_IN_CELL;
        return;
    }
    /*
     * `<tr>` and `</tr>` both need a row to act on, and in a fragment parsed
     * against a `tr` context there is not one -- the context element is not
     * pushed, only a synthetic html root. So both are dropped and a following
     * `<td>` is the whole document, which is what `innerHTML` on a row does.
     * Without the guard `<tr><td>` grew a second row inside the fragment.
     */
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "tr"))
    {
        static const char *const TR[] = {"tr", 0};

        if (!ar__in_table_scope(t, TR))
        {
            t->doc->errors++;
            return;
        }
        ar__clear_to_table_row(t);
        ar__pop(t);
        t->mode = M_IN_TABLE_BODY;
        return;
    }
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "tr"))
    {
        static const char *const TR[] = {"tr", 0};

        if (!ar__in_table_scope(t, TR))
        {
            t->doc->errors++;
            return;
        }
        ar__clear_to_table_row(t);
        ar__pop(t);
        t->mode = M_IN_TABLE_BODY;
        ar__process(t, tok);
        return;
    }
    /* A row group opening inside a row closes the row, and the group it lands
       in is decided one level out rather than here. */
    if (tok->kind == AR_TOK_START &&
        (ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
         ar_span_is(tok->name, "thead") || ar_span_is(tok->name, "caption") ||
         ar_span_is(tok->name, "col") || ar_span_is(tok->name, "colgroup")))
    {
        static const char *const TR[] = {"tr", 0};

        if (!ar__in_table_scope(t, TR))
        {
            t->doc->errors++;
            return;
        }
        ar__clear_to_table_row(t);
        ar__pop(t);
        t->mode = M_IN_TABLE_BODY;
        ar__process(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "table"))
    {
        /* A `</table>` with no table on the stack is dropped rather than
           popping the fragment root out from under the document. */
        static const char *const TABLE[] = {"table", 0};

        if (!ar__in_table_scope(t, TABLE))
        {
            t->doc->errors++;
            return;
        }
        ar__pop_until(t, "table");
        ar__reset_mode(t);
        return;
    }
    ar__in_table(t, tok);
}

static void ar__in_cell(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_END && (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th")))
    {
        ar__implied_end_tags(t, 0);
        ar__pop_until(t, ar_span_is(tok->name, "td") ? "td" : "th");
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_ROW;
        return;
    }
    if (tok->kind == AR_TOK_START &&
        (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th") ||
         ar_span_is(tok->name, "tr") || ar_span_is(tok->name, "tbody") ||
         ar_span_is(tok->name, "tfoot") || ar_span_is(tok->name, "thead") ||
         ar_span_is(tok->name, "caption") || ar_span_is(tok->name, "col") ||
         ar_span_is(tok->name, "colgroup")))
    {
        /* A new cell closes the open one. Missing `</td>` is the normal state
           of hand-written tables.

           Every other table structural tag closes it too, which is what makes
           `<thead><tr><td>h<tbody>` two row groups rather than a tbody inside
           a cell -- the tree the corpus found when only td, th and tr were
           listed here. */
        ar__implied_end_tags(t, 0);
        while (t->open_n > 1 && !ar__is(t, ar__current(t), "td") &&
               !ar__is(t, ar__current(t), "th"))
        {
            ar__pop(t);
        }
        ar__pop(t);
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_ROW;
        ar__process(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END &&
        (ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
         ar_span_is(tok->name, "thead") || ar_span_is(tok->name, "tr")))
    {
        /*
         * A row-group or row end tag closes the cell and is then reprocessed,
         * 13.2.6.4.15 -- but only if there is one of them in table scope,
         * because a stray `</tbody>` in a cell that is not in a tbody must
         * not tear the cell down around it.
         *
         * Without this the tag reached `in body`, which found nothing to
         * close and dropped it, and the cell stayed open: `<table><td></tbody>A`
         * put the A in the cell where a browser fosters it out of the table.
         */
        char        name[16];
        ar_u32      n = tok->name.n < sizeof name - 1u ? tok->name.n : (ar_u32)sizeof name - 1u;
        ar_u32      k;
        const char *one[2];

        for (k = 0; k < n; ++k)
        {
            name[k] = tok->name.p[k];
        }
        name[n] = 0;
        one[0] = name;
        one[1] = 0;
        if (!ar__in_table_scope(t, one))
        {
            t->doc->errors++;
            return;
        }
        ar__implied_end_tags(t, 0);
        while (t->open_n > 1 && !ar__is(t, ar__current(t), "td") &&
               !ar__is(t, ar__current(t), "th"))
        {
            ar__pop(t);
        }
        ar__pop(t);
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_ROW;
        ar__process(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "table"))
    {
        while (t->open_n > 1 && !ar__is(t, ar__current(t), "td") &&
               !ar__is(t, ar__current(t), "th"))
        {
            ar__pop(t);
        }
        ar__pop(t);
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_ROW;
        ar__process(t, tok);
        return;
    }
    ar__in_body(t, tok);
}

static void ar__in_caption(ar__tree *t, const ar_token *tok)
{
    /*
     * A caption has no end tag in practice. It is closed by the next table
     * part, or by `</table>`, and everything else is body content -- which is
     * why this mode is four lines of its own and then `in body`.
     *
     * `<table><caption><td>` had been leaving the caption open, so the cell
     * and the implied row and body were built *inside* it and the table came
     * out with one caption and no rows.
     */
    static const char *const CLOSERS[] = {"caption", "col", "colgroup", "tbody", "td",
                                          "tfoot",   "th",  "thead",    "tr",    0};
    static const char *const CAPTION[] = {"caption", 0};
    int                      closes = 0;

    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "caption"))
    {
        closes = 1;
    }
    else if (tok->kind == AR_TOK_START && ar__name_in(tok->name, CLOSERS))
    {
        closes = 2;
    }
    else if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "table"))
    {
        closes = 2;
    }

    if (closes)
    {
        if (!ar__in_table_scope(t, CAPTION))
        {
            t->doc->errors++; /* fragment case: no caption to close */
            return;
        }
        ar__implied_end_tags(t, 0);
        if (!ar__is(t, ar__current(t), "caption"))
        {
            t->doc->errors++;
        }
        ar__pop_until(t, "caption");
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_TABLE;
        if (closes == 2)
        {
            ar__process(t, tok); /* the token that closed it still has to land */
        }
        return;
    }
    ar__in_body(t, tok);
}

/* ------------------------------------------------------------------------
 * The dispatcher
 * ------------------------------------------------------------------------ */
/*
 * An end tag arriving before the body exists.
 *
 * `before html`, `before head`, `in head` and `after head` each let exactly
 * `head`, `body`, `html` and `br` through and **ignore everything else**.
 * Falling through instead builds the skeleton and reprocesses the tag in
 * `in body`, where `</p>` inserts an empty paragraph -- so a document opening
 * with a stray `</p>` gained one that no browser has.
 *
 * Found by the tree corpus: `</p></div></b><p>a` came out as two paragraphs
 * against a browser's one.
 */
static int ar__early_end_tag_ignored(const ar_token *tok)
{
    if (tok->kind != AR_TOK_END)
    {
        return 0;
    }
    return !ar_span_is(tok->name, "head") && !ar_span_is(tok->name, "body") &&
           !ar_span_is(tok->name, "html") && !ar_span_is(tok->name, "br");
}

/* ------------------------------------------------------------------------
 * Foreign content, §13.2.6.5
 *
 * SVG and MathML inside HTML, which is not a second parser but a set of
 * exceptions to the first one. Four things make it awkward and all four are
 * here:
 *
 *   1. Names are case-sensitive there and case-insensitive here. The tokenizer
 *      lowercases every tag and attribute name, correctly, and then SVG needs
 *      `foreignObject` and `clipPath` and `attributeName` back. So there are
 *      tables, and they are the specification's own tables.
 *
 *   2. Attributes can have a namespace. `xlink:href` is `href` in the XLink
 *      namespace and is not the same attribute as `href`.
 *
 *   3. Integration points. Inside `<foreignObject>` or `<mtext>` the HTML
 *      rules resume, which is how `<svg><foreignObject><div>` works, and the
 *      test for it has to be asked before every token.
 *
 *   4. Breakout. A `<p>` or a `<table>` inside `<svg>` does not become an SVG
 *      element -- it pops the whole foreign subtree and reprocesses as HTML.
 *      Authors write it constantly, mostly by forgetting to close something.
 * ------------------------------------------------------------------------ */

/* The SVG tag names whose lowercase form is not their real form, §13.2.6.5. */
static const char *const AR__SVG_TAGS[] = {"altglyph",
                                           "altGlyph",
                                           "altglyphdef",
                                           "altGlyphDef",
                                           "altglyphitem",
                                           "altGlyphItem",
                                           "animatecolor",
                                           "animateColor",
                                           "animatemotion",
                                           "animateMotion",
                                           "animatetransform",
                                           "animateTransform",
                                           "clippath",
                                           "clipPath",
                                           "feblend",
                                           "feBlend",
                                           "fecolormatrix",
                                           "feColorMatrix",
                                           "fecomponenttransfer",
                                           "feComponentTransfer",
                                           "fecomposite",
                                           "feComposite",
                                           "feconvolvematrix",
                                           "feConvolveMatrix",
                                           "fediffuselighting",
                                           "feDiffuseLighting",
                                           "fedisplacementmap",
                                           "feDisplacementMap",
                                           "fedistantlight",
                                           "feDistantLight",
                                           "fedropshadow",
                                           "feDropShadow",
                                           "feflood",
                                           "feFlood",
                                           "fefunca",
                                           "feFuncA",
                                           "fefuncb",
                                           "feFuncB",
                                           "fefuncg",
                                           "feFuncG",
                                           "fefuncr",
                                           "feFuncR",
                                           "fegaussianblur",
                                           "feGaussianBlur",
                                           "feimage",
                                           "feImage",
                                           "femerge",
                                           "feMerge",
                                           "femergenode",
                                           "feMergeNode",
                                           "femorphology",
                                           "feMorphology",
                                           "feoffset",
                                           "feOffset",
                                           "fepointlight",
                                           "fePointLight",
                                           "fespecularlighting",
                                           "feSpecularLighting",
                                           "fespotlight",
                                           "feSpotLight",
                                           "fetile",
                                           "feTile",
                                           "feturbulence",
                                           "feTurbulence",
                                           "foreignobject",
                                           "foreignObject",
                                           "glyphref",
                                           "glyphRef",
                                           "lineargradient",
                                           "linearGradient",
                                           "radialgradient",
                                           "radialGradient",
                                           "textpath",
                                           "textPath",
                                           0};

/* SVG attribute names, same idea. */
static const char *const AR__SVG_ATTRS[] = {"attributename",
                                            "attributeName",
                                            "attributetype",
                                            "attributeType",
                                            "basefrequency",
                                            "baseFrequency",
                                            "baseprofile",
                                            "baseProfile",
                                            "calcmode",
                                            "calcMode",
                                            "clippathunits",
                                            "clipPathUnits",
                                            "diffuseconstant",
                                            "diffuseConstant",
                                            "edgemode",
                                            "edgeMode",
                                            "filterunits",
                                            "filterUnits",
                                            "glyphref",
                                            "glyphRef",
                                            "gradienttransform",
                                            "gradientTransform",
                                            "gradientunits",
                                            "gradientUnits",
                                            "kernelmatrix",
                                            "kernelMatrix",
                                            "kernelunitlength",
                                            "kernelUnitLength",
                                            "keypoints",
                                            "keyPoints",
                                            "keysplines",
                                            "keySplines",
                                            "keytimes",
                                            "keyTimes",
                                            "lengthadjust",
                                            "lengthAdjust",
                                            "limitingconeangle",
                                            "limitingConeAngle",
                                            "markerheight",
                                            "markerHeight",
                                            "markerunits",
                                            "markerUnits",
                                            "markerwidth",
                                            "markerWidth",
                                            "maskcontentunits",
                                            "maskContentUnits",
                                            "maskunits",
                                            "maskUnits",
                                            "numoctaves",
                                            "numOctaves",
                                            "pathlength",
                                            "pathLength",
                                            "patterncontentunits",
                                            "patternContentUnits",
                                            "patterntransform",
                                            "patternTransform",
                                            "patternunits",
                                            "patternUnits",
                                            "pointsatx",
                                            "pointsAtX",
                                            "pointsaty",
                                            "pointsAtY",
                                            "pointsatz",
                                            "pointsAtZ",
                                            "preservealpha",
                                            "preserveAlpha",
                                            "preserveaspectratio",
                                            "preserveAspectRatio",
                                            "primitiveunits",
                                            "primitiveUnits",
                                            "refx",
                                            "refX",
                                            "refy",
                                            "refY",
                                            "repeatcount",
                                            "repeatCount",
                                            "repeatdur",
                                            "repeatDur",
                                            "requiredextensions",
                                            "requiredExtensions",
                                            "requiredfeatures",
                                            "requiredFeatures",
                                            "specularconstant",
                                            "specularConstant",
                                            "specularexponent",
                                            "specularExponent",
                                            "spreadmethod",
                                            "spreadMethod",
                                            "startoffset",
                                            "startOffset",
                                            "stddeviation",
                                            "stdDeviation",
                                            "stitchtiles",
                                            "stitchTiles",
                                            "surfacescale",
                                            "surfaceScale",
                                            "systemlanguage",
                                            "systemLanguage",
                                            "tablevalues",
                                            "tableValues",
                                            "targetx",
                                            "targetX",
                                            "targety",
                                            "targetY",
                                            "textlength",
                                            "textLength",
                                            "viewbox",
                                            "viewBox",
                                            "viewtarget",
                                            "viewTarget",
                                            "xchannelselector",
                                            "xChannelSelector",
                                            "ychannelselector",
                                            "yChannelSelector",
                                            "zoomandpan",
                                            "zoomAndPan",
                                            0};

/* MathML has one, and it is the one nobody remembers. */
static const char *const AR__MATH_ATTRS[] = {"definitionurl", "definitionURL", 0};

/* The attributes written with a colon, and the namespace each lands in. */
static const struct
{
    const char *written;
    const char *local;
    int         ns;
} AR__FOREIGN_ATTRS[] = {
    {"xlink:actuate", "actuate", AR_ATTR_NS_XLINK}, {"xlink:arcrole", "arcrole", AR_ATTR_NS_XLINK},
    {"xlink:href", "href", AR_ATTR_NS_XLINK},       {"xlink:role", "role", AR_ATTR_NS_XLINK},
    {"xlink:show", "show", AR_ATTR_NS_XLINK},       {"xlink:title", "title", AR_ATTR_NS_XLINK},
    {"xlink:type", "type", AR_ATTR_NS_XLINK},       {"xml:lang", "lang", AR_ATTR_NS_XML},
    {"xml:space", "space", AR_ATTR_NS_XML},         {"xmlns", "xmlns", AR_ATTR_NS_XMLNS},
    {"xmlns:xlink", "xlink", AR_ATTR_NS_XMLNS},     {0, 0, 0}};

/* A lowercase name to its real spelling, or the name unchanged. The tables are
   pairs, so this walks two at a time. */
static ar_span ar__fix_case(const char *const *table, ar_span name)
{
    ar_i32 i;

    for (i = 0; table[i]; i += 2)
    {
        if (ar_span_is(name, table[i]))
        {
            ar_span out;

            out.p = table[i + 1];
            out.n = (ar_u32)strlen(table[i + 1]);
            return out;
        }
    }
    return name;
}

static int ar__is_html(const ar__tree *t, ar_i32 node)
{
    return node < 0 || t->doc->nodes[node].ns == AR_NS_HTML;
}

/*
 * A MathML text integration point: `<mi>`, `<mo>`, `<mn>`, `<ms>`, `<mtext>`.
 * Inside one, HTML content is HTML again -- which is the whole reason MathML
 * can carry a sentence.
 */
static int ar__math_text_point(const ar__tree *t, ar_i32 node)
{
    if (node < 0 || t->doc->nodes[node].ns != AR_NS_MATHML)
    {
        return 0;
    }
    return ar__is(t, node, "mi") || ar__is(t, node, "mo") || ar__is(t, node, "mn") ||
           ar__is(t, node, "ms") || ar__is(t, node, "mtext");
}

/* An `annotation-xml` whose `encoding` says its contents are HTML. */
static int ar__annotation_html(const ar__tree *t, ar_i32 node)
{
    ar_i32 k;

    if (node < 0 || t->doc->nodes[node].ns != AR_NS_MATHML || !ar__is(t, node, "annotation-xml"))
    {
        return 0;
    }
    for (k = 0; k < t->doc->nodes[node].attr_count; ++k)
    {
        const ar_attr *a = &t->doc->attrs[t->doc->nodes[node].attr_first + k];

        if (ar_span_is(a->name, "encoding") &&
            (ar_span_is(a->value, "text/html") || ar_span_is(a->value, "application/xhtml+xml")))
        {
            return 1;
        }
    }
    return 0;
}

static int ar__html_point(const ar__tree *t, ar_i32 node)
{
    if (node < 0)
    {
        return 0;
    }
    if (t->doc->nodes[node].ns == AR_NS_SVG)
    {
        return ar__is(t, node, "foreignObject") || ar__is(t, node, "desc") ||
               ar__is(t, node, "title");
    }
    return ar__annotation_html(t, node);
}

/*
 * Whether this token is handled by the insertion mode or by the foreign
 * content rules, §13.2.6.
 *
 * Asked before every token, which is why it is written as one expression
 * rather than a walk: the answer for an ordinary document is the first line.
 */
/* The same two questions asked of a context element, which is a name and a
   namespace rather than a node. A fragment's context element is never on the
   stack, so there is nothing to point `ar__math_text_point` at. */
static int ar__ctx_math_text_point(const ar__tree *t)
{
    return t->ctx_ns == AR_NS_MATHML &&
           (ar__lit_is(t->ctx, "mi") || ar__lit_is(t->ctx, "mo") || ar__lit_is(t->ctx, "mn") ||
            ar__lit_is(t->ctx, "ms") || ar__lit_is(t->ctx, "mtext"));
}

static int ar__ctx_html_point(const ar__tree *t)
{
    /*
     * `annotation-xml` is deliberately absent: whether it is an HTML
     * integration point depends on its `encoding` attribute, and a context
     * element is a name with no attributes to read.
     */
    return t->ctx_ns == AR_NS_SVG && (ar__lit_is(t->ctx, "foreignObject") ||
                                      ar__lit_is(t->ctx, "desc") || ar__lit_is(t->ctx, "title"));
}

static int ar__use_insertion_mode(const ar__tree *t, const ar_token *tok)
{
    ar_i32 cur = ar__current(t);

    /*
     * The *adjusted* current node: in a fragment with only the synthetic root
     * on the stack, it is the context element. That is what makes
     * `<path/>` with an `svg svg` context an SVG element rather than an
     * unknown HTML one.
     *
     * And an integration point is still an integration point when it is the
     * context element. `<figure>` parsed against `math ms` is an *HTML*
     * figure, because a text integration point is where HTML resumes -- the
     * short-circuit here used to answer "foreign" for every non-HTML context
     * and made it `<math figure>`. Twenty-one cases, all in one file, and the
     * repeated `<figure></figure>` against five different MathML contexts is
     * the suite saying so five times.
     */
    if (t->ctx && t->open_n <= 2 && t->ctx_ns != AR_NS_HTML)
    {
        if (tok->kind == AR_TOK_EOF)
        {
            return 1;
        }
        if (ar__ctx_math_text_point(t))
        {
            if (tok->kind == AR_TOK_TEXT)
            {
                return 1;
            }
            if (tok->kind == AR_TOK_START && !ar_span_is(tok->name, "mglyph") &&
                !ar_span_is(tok->name, "malignmark"))
            {
                return 1;
            }
        }
        if (ar__ctx_html_point(t) && (tok->kind == AR_TOK_START || tok->kind == AR_TOK_TEXT))
        {
            return 1;
        }
        return 0;
    }
    if (t->open_n <= 1 || ar__is_html(t, cur) || tok->kind == AR_TOK_EOF)
    {
        return 1;
    }
    if (ar__math_text_point(t, cur))
    {
        if (tok->kind == AR_TOK_TEXT)
        {
            return 1;
        }
        if (tok->kind == AR_TOK_START && !ar_span_is(tok->name, "mglyph") &&
            !ar_span_is(tok->name, "malignmark"))
        {
            return 1;
        }
    }
    if (t->doc->nodes[cur].ns == AR_NS_MATHML && ar__is(t, cur, "annotation-xml") &&
        tok->kind == AR_TOK_START && ar_span_is(tok->name, "svg"))
    {
        return 1;
    }
    if (ar__html_point(t, cur) && (tok->kind == AR_TOK_START || tok->kind == AR_TOK_TEXT))
    {
        return 1;
    }
    return 0;
}

/*
 * Insert a foreign element: adjust its name, adjust its attributes, and put it
 * in the namespace it belongs to.
 */
static ar_i32 ar__insert_foreign(ar__tree *t, const ar_token *tok, ar_ns ns, int foster)
{
    ar_i32 node = ar__node(t, AR_DOM_ELEMENT);
    ar_i32 k;

    if (node < 0)
    {
        return -1;
    }
    t->doc->nodes[node].ns = ns;
    t->doc->nodes[node].name =
        ar__keep(t, ns == AR_NS_SVG ? ar__fix_case(AR__SVG_TAGS, tok->name) : tok->name);

    if (tok->attr_count > 0 && t->doc->attr_count + tok->attr_count <= t->doc->attr_cap)
    {
        t->doc->nodes[node].attr_first = t->doc->attr_count;
        t->doc->nodes[node].attr_count = tok->attr_count;
        for (k = 0; k < tok->attr_count; ++k)
        {
            ar_span name = tok->attrs[k].name;
            int     ans = AR_ATTR_NS_NONE;
            ar_i32  j;

            for (j = 0; AR__FOREIGN_ATTRS[j].written; ++j)
            {
                if (ar_span_is(name, AR__FOREIGN_ATTRS[j].written))
                {
                    name.p = AR__FOREIGN_ATTRS[j].local;
                    name.n = (ar_u32)strlen(AR__FOREIGN_ATTRS[j].local);
                    ans = AR__FOREIGN_ATTRS[j].ns;
                    break;
                }
            }
            if (ans == AR_ATTR_NS_NONE)
            {
                name = ns == AR_NS_SVG ? ar__fix_case(AR__SVG_ATTRS, name)
                                       : ar__fix_case(AR__MATH_ATTRS, name);
            }
            t->doc->attrs[t->doc->attr_count].name = ar__keep(t, name);
            t->doc->attrs[t->doc->attr_count].value = ar__keep(t, tok->attrs[k].value);
            t->doc->attrs[t->doc->attr_count].ns = (ar_attr_ns)ans;
            ++t->doc->attr_count;
        }
    }
    else if (tok->attr_count > 0)
    {
        t->doc->overflowed = 1;
    }

    ar__insert_node(t, node, foster);
    ar__push(t, node);
    return node;
}

/*
 * The HTML start tags that break out of foreign content, §13.2.6.5.
 *
 * Not an arbitrary list: it is the block-level and formatting elements an
 * author is likely to leave open. A `<p>` inside `<svg>` almost always means
 * the `</svg>` is missing, and the specification's answer is to believe the
 * `<p>` and pop the SVG subtree rather than make an SVG element called p.
 */
static int ar__breaks_out(const ar__tree *t, const ar_token *tok)
{
    static const char *const OUT[] = {
        "b",      "big",  "blockquote", "body",  "br",   "center", "code",    "dd",   "div",
        "dl",     "dt",   "em",         "embed", "h1",   "h2",     "h3",      "h4",   "h5",
        "h6",     "head", "hr",         "i",     "img",  "li",     "listing", "menu", "meta",
        "nobr",   "ol",   "p",          "pre",   "ruby", "s",      "small",   "span", "strong",
        "strike", "sub",  "sup",        "table", "tt",   "u",      "ul",      "var",  0};
    ar_i32 i;

    (void)t; /* the list is a property of the tag, not of the stack */

    if (ar__name_in(tok->name, OUT))
    {
        return 1;
    }
    /* And `<font>`, but only when it carries one of the three attributes that
       make it a presentational HTML font tag rather than an SVG one. */
    if (ar_span_is(tok->name, "font"))
    {
        for (i = 0; i < tok->attr_count; ++i)
        {
            if (ar_span_is(tok->attrs[i].name, "color") || ar_span_is(tok->attrs[i].name, "face") ||
                ar_span_is(tok->attrs[i].name, "size"))
            {
                return 1;
            }
        }
    }
    return 0;
}

/*
 * The namespace new elements go into: the *adjusted* current node's.
 *
 * In a fragment with only the synthetic root on the stack that is the context
 * element, not the root -- and the root is an html element, so taking its
 * namespace put every child of an `svg path` context into the HTML namespace.
 * The tree looked right and every name in it was wrong.
 */
static ar_ns ar__adjusted_ns(const ar__tree *t)
{
    ar_i32 cur;

    if (t->ctx && t->open_n <= 2)
    {
        return t->ctx_ns;
    }
    cur = ar__current(t);
    return cur >= 0 ? t->doc->nodes[cur].ns : AR_NS_HTML;
}

static void ar__foreign(ar__tree *t, const ar_token *tok)
{
    ar_i32 cur = ar__current(t);
    ar_ns  ns = ar__adjusted_ns(t);

    (void)cur;

    switch (tok->kind)
    {
    case AR_TOK_TEXT:
        /* §13.2.6.5: a U+0000 here is the replacement character, not nothing.
           `<svg>\0filler` is three characters and a word, and the CDATA
           section that carries one says the same. */
        ar__insert_text_ex(t, tok->text, 0, AR__NUL_REPLACE);
        if (!ar__all_space(tok->text))
        {
            t->frameset_ok = 0;
        }
        return;

    case AR_TOK_COMMENT:
        ar__comment_node(t, tok, -1);
        return;

    case AR_TOK_DOCTYPE:
        t->doc->errors++;
        return;

    case AR_TOK_START:
        if (ar__breaks_out(t, tok))
        {
            t->doc->errors++;
            while (t->open_n > 1 && !ar__is_html(t, ar__current(t)) &&
                   !ar__math_text_point(t, ar__current(t)) && !ar__html_point(t, ar__current(t)))
            {
                ar__pop(t);
            }
            /*
             * The insertion mode, not the dispatcher.
             *
             * §13.2.6.5 says "reprocess the token according to the rules given
             * in the section corresponding to the current insertion mode", and
             * the difference is not stylistic. In a fragment with an `svg
             * path` context the adjusted current node is the context element,
             * so the dispatcher sends the token straight back here -- and the
             * breakout has nothing left to pop, because the only thing on the
             * stack is the synthetic root. That is an infinite loop, and it is
             * what foreign-fragment.dat hangs on.
             */
            ar__process_mode(t, tok);
            return;
        }
        {
            ar_i32 node = ar__insert_foreign(t, tok, ns, 0);

            if (node >= 0 && tok->self_closing)
            {
                /* In foreign content a self-closing tag really does close. */
                ar__pop(t);
            }
        }
        return;

    case AR_TOK_END:
        /*
         * `</br>` and `</p>` break out, §13.2.6.5, and they are the only two
         * end tags that do.
         *
         * Both are HTML tags an author writes without thinking, and both get
         * the start tag treatment: pop until the current node is an HTML
         * element or an integration point, then reprocess in HTML content. So
         * `<svg></p><foo>` is three siblings -- the svg, a paragraph, and an
         * unknown element -- rather than everything swallowed by the svg.
         *
         * The general end-tag loop below cannot produce that. It stops at the
         * first HTML element *below* the foreign one and processes the token
         * there, leaving the svg open, so the paragraph landed inside it.
         */
        if (ar_span_is(tok->name, "br") || ar_span_is(tok->name, "p"))
        {
            t->doc->errors++;
            while (t->open_n > 1 && !ar__is_html(t, ar__current(t)) &&
                   !ar__math_text_point(t, ar__current(t)) && !ar__html_point(t, ar__current(t)))
            {
                ar__pop(t);
            }
            ar__process_mode(t, tok);
            return;
        }

        /*
         * §13.2.6.5's end tag loop. Walk down the stack looking for a match on
         * the name, case-insensitively for the current node and by exact name
         * below it; pop through it if found. Reaching an HTML element means
         * the tag belongs to the insertion mode instead.
         */
        {
            ar_i32 i = t->open_n - 1;

            if (i < 1)
            {
                return;
            }
            if (!ar__span_eq(t->doc->nodes[t->open[i]].name, tok->name))
            {
                t->doc->errors++;
            }
            for (;;)
            {
                if (ar__span_eq(t->doc->nodes[t->open[i]].name, tok->name))
                {
                    while (t->open_n > i)
                    {
                        ar__pop(t);
                    }
                    return;
                }
                --i;
                if (i < 1)
                {
                    return;
                }
                if (ar__is_html(t, t->open[i]))
                {
                    ar__process_mode(t, tok);
                    return;
                }
            }
        }

    default:
        return;
    }
}

/*
 * Every token goes through here, and the only question is which set of rules
 * it belongs to: the insertion mode, or the foreign content rules.
 */
#define AR_HTML_REPROCESS 64

static void ar__process(ar__tree *t, const ar_token *tok)
{
    if (t->depth >= AR_HTML_REPROCESS)
    {
        /* Sixty-four is far past anything the specification asks for -- the
           longest real chain is a handful of modes -- so reaching it means a
           cycle, and dropping the token ends it without ending the parse. */
        t->doc->errors++;
        return;
    }
    ++t->depth;
    if (ar__use_insertion_mode(t, tok))
    {
        ar__process_mode(t, tok);
    }
    else
    {
        ar__foreign(t, tok);
    }
    --t->depth;
}

/*
 * The same net as ar__process, in the shape this function can take.
 *
 * Several modes call ar__process_mode directly rather than going through
 * ar__process, and a cycle between two of *those* is what hung `after head`
 * against `in head`. The switch below has nearly two hundred returns, so a
 * counter cannot live inside it -- it lives in a wrapper, and the switch is
 * what the wrapper calls.
 */
static void ar__process_switch(ar__tree *t, const ar_token *tok);

static void ar__process_mode(ar__tree *t, const ar_token *tok)
{
    if (t->depth >= AR_HTML_REPROCESS)
    {
        t->doc->errors++;
        return;
    }
    ++t->depth;
    ar__process_switch(t, tok);
    --t->depth;
}

static void ar__process_switch(ar__tree *t, const ar_token *tok)
{
    /*
     * A character token that is nothing but NULs is ignored outright, and
     * that means the *token*, not just its text.
     *
     * Dropping the bytes at insertion time was not enough: the token still
     * reached the insertion mode, and in `before head` or `after head`
     * anything that is not whitespace opens a body. So
     * `<html>\0<frameset>` got a body and the frameset had nowhere to go --
     * the NUL was invisible in the tree and decided its shape.
     *
     * Only here, not in ar__foreign: foreign content turns a NUL into U+FFFD
     * rather than ignoring it.
     */
    if (tok->kind == AR_TOK_TEXT && tok->text.n && ar__non_nul(tok->text) == 0)
    {
        t->doc->errors++;
        return;
    }

    /*
     * A `<?` construct the file ended in the middle of leaves no node.
     *
     * The tokenizer emits it -- the bogus comment state emits on EOF, and the
     * tokenizer suite checks that it does -- and dropping it is this layer's
     * job, the same division that makes a processing instruction a comment
     * token there and a node of its own here. `<body><?start data` gets a body
     * and nothing in it.
     */
    if (tok->kind == AR_TOK_COMMENT && tok->unterminated)
    {
        t->doc->errors++;
        return;
    }

    switch (t->mode)
    {
    case M_INITIAL:
        if (tok->kind == AR_TOK_DOCTYPE)
        {
            ar_i32 node = ar__node(t, AR_DOM_DOCTYPE);

            if (node >= 0)
            {
                t->doc->nodes[node].name = ar__keep(t, tok->name);
                ar__append(t, 0, node);
            }
            t->doc->doctype_public = ar__keep(t, tok->pub);
            t->doc->doctype_system = ar__keep(t, tok->sys);
            if (!tok->pub.p)
            {
                t->doc->doctype_public.p = 0;
            }
            if (!tok->sys.p)
            {
                t->doc->doctype_system.p = 0;
            }
            t->doc->quirks = ar__quirks_for(tok);
            t->mode = M_BEFORE_HTML;
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            /* The document, or the synthetic root when this is a
               fragment -- a fragment's answer is the root's children,
               so a comment parented on the document is invisible. */
            ar__comment_node(t, tok, t->ctx ? ar_dom_root(t->doc) : 0);
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            return;
        }
        /* No doctype at all. The specification's answer is quirks mode, and
           this is not a curiosity: it changes the box model for the whole
           document. */
        t->doc->quirks = AR_QUIRKS_YES;
        t->mode = M_BEFORE_HTML;
        ar__process(t, tok);
        return;

    case M_BEFORE_HTML:
        if (ar__early_end_tag_ignored(tok))
        {
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            /* The document, or the synthetic root when this is a
               fragment -- a fragment's answer is the root's children,
               so a comment parented on the document is invisible. */
            ar__comment_node(t, tok, t->ctx ? ar_dom_root(t->doc) : 0);
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "html"))
        {
            ar__insert_element(t, tok, 0);
            t->mode = M_BEFORE_HEAD;
            return;
        }
        ar__insert_implied(t, "html");
        t->mode = M_BEFORE_HEAD;
        ar__process(t, tok);
        return;

    case M_BEFORE_HEAD:
        if (ar__early_end_tag_ignored(tok))
        {
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, -1);
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "head"))
        {
            t->head = ar__insert_element(t, tok, 0);
            t->mode = M_IN_HEAD;
            return;
        }
        t->head = ar__insert_implied(t, "head");
        t->mode = M_IN_HEAD;
        ar__process(t, tok);
        return;

    case M_IN_HEAD:
        if (ar__early_end_tag_ignored(tok))
        {
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            ar__insert_text(t, tok->text, 0);
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, -1);
            return;
        }
        if (tok->kind == AR_TOK_START)
        {
            /*
             * The five void elements that are head content, and not the
             * eleven others that merely happen to be void.
             *
             * This had been `ar__is_void`, which is the whole void list, so
             * `<br>`, `<param>`, `<img>`, `<hr>` and the rest were inserted
             * into the head and the body never opened. It looked harmless --
             * a `<br>` in the head draws nothing either way -- until the
             * frameset-ok flag arrived: `<br>` clears the flag in `in body`
             * and clears nothing at all in `in head`, so `<br><frameset>`
             * built a frameset document and threw the `<br>` into the head on
             * the way. Six conformance cases, and the visible one is that
             * `<param><frameset>` put a param in the head.
             *
             * `basefont` and `bgsound` are here because they really are head
             * content, obsolete rather than absent, and `keygen` is not --
             * which is why it moved out with the others.
             */
            static const char *const HEAD_VOID[] = {"base", "basefont", "bgsound",
                                                    "link", "meta",     0};

            if (ar__name_in(tok->name, HEAD_VOID))
            {
                /* Fostered when a table sent it here: `in body` routes
                   head content to these rules, and a `<meta>` inside a
                   table belongs before the table like anything else.
                   In a real head there is nothing to foster past. */
                ar__insert_element(t, tok, 1);
                ar__pop(t);
                return;
            }
            /*
             * `noframes` belongs here too, as raw text, and leaving it out was
             * not merely a missing element: `after head` routes head content
             * back to these rules, and a tag these rules do not know is popped
             * straight back to `after head` -- which routed it here again.
             * That is an infinite loop, and `<noframes>` after a closed head
             * was all it took.
             */
            if (ar_span_is(tok->name, "title") || ar_span_is(tok->name, "style") ||
                ar_span_is(tok->name, "noframes") || ar_span_is(tok->name, "script"))
            {
                ar__insert_element(t, tok, 0);
                t->tok->state =
                    ar_span_is(tok->name, "script")
                        ? AR_HTML_SCRIPT
                        : (ar_span_is(tok->name, "title") ? AR_HTML_RCDATA : AR_HTML_RAWTEXT);
                t->original_mode = M_IN_HEAD;
                t->mode = M_TEXT;
                return;
            }
            /*
             * A noscript is not rawtext here, because there is no scripting in
             * areole and never will be. With scripting disabled the
             * specification parses its contents as ordinary markup and closes
             * it on anything that is not head content -- so
             * `<noscript><p>x</p></noscript>` is an empty noscript in the head
             * and a paragraph in the body, which is what a browser with
             * scripting switched off also produces.
             */
            if (ar_span_is(tok->name, "noscript"))
            {
                ar__insert_element(t, tok, 0);
                t->mode = M_IN_HEAD_NOSCRIPT;
                return;
            }
            /* A template is head content wherever it appears. */
            if (ar_span_is(tok->name, "template"))
            {
                ar__insert_element(t, tok, 0);
                ar__fmt_marker(t);
                t->original_mode = M_IN_HEAD;
                ar__tmpl_push(t, M_IN_TEMPLATE);
                t->mode = M_IN_TEMPLATE;
                return;
            }
        }
        if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "head"))
        {
            ar__pop(t);
            t->mode = M_AFTER_HEAD;
            return;
        }
        /* Anything else ends the head, which is why `</head>` is optional. */
        ar__pop(t);
        t->mode = M_AFTER_HEAD;
        ar__process(t, tok);
        return;

    case M_AFTER_HEAD:
        if (ar__early_end_tag_ignored(tok))
        {
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            ar__insert_text(t, tok->text, 0);
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, -1);
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "body"))
        {
            ar__insert_element(t, tok, 0);

            /* A document that says `<body>` has said what it is, so a
               `<frameset>` after it is dropped rather than allowed to throw
               the body away. `<body><frameset>` keeps the body. */
            t->frameset_ok = 0;
            t->mode = M_IN_BODY;
            return;
        }
        /* A frameset document has a frameset where its body would be. There is
           no layout for one -- the release document says parsed to the correct
           tree and then not laid out -- but the tree has to be right. */
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "frameset"))
        {
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_FRAMESET;
            return;
        }
        /*
         * Head content after the head has closed still goes *in* the head.
         *
         * `<head></head><title>X</title>` puts the title inside the head, and
         * the specification is explicit about the mechanism: push the head
         * element back onto the stack, run the `in head` rules, then take it
         * off again. Without that the title opened a body and landed in it,
         * which is a visibly different document.
         */
        if (tok->kind == AR_TOK_START && t->head >= 0)
        {
            static const char *const HEAD_CONTENT[] = {"base",     "basefont", "bgsound", "link",
                                                       "meta",     "noframes", "script",  "style",
                                                       "template", "title",    0};

            if (ar__name_in(tok->name, HEAD_CONTENT))
            {
                t->doc->errors++;
                ar__push(t, t->head);
                t->mode = M_IN_HEAD;
                ar__process_mode(t, tok);
                /*
                 * The head comes off again whatever happened. It was pushed
                 * only so the `in head` rules had somewhere to insert, and
                 * 13.2.6.4.5 removes it in the same breath -- before the
                 * element's own content is tokenized, not after.
                 *
                 * Leaving it on when the token opened a text element was the
                 * bug: `<head></head><style></style><!-- -->` put the comment
                 * inside the head, because `</style>` came back to a stack
                 * that still had one. `ar__pop_index` takes it out from under
                 * the style, which is exactly what it is for.
                 */
                ar__pop_index(t, t->head);
                if (t->mode == M_IN_HEAD)
                {
                    t->mode = M_AFTER_HEAD;
                }
                else if (t->original_mode == M_IN_HEAD)
                {
                    /* `in head` was borrowed; the mode to return to when the
                       text ends is the one we were really in. */
                    t->original_mode = M_AFTER_HEAD;
                }
                return;
            }
        }

        ar__insert_implied(t, "body");
        t->mode = M_IN_BODY;
        ar__process(t, tok);
        return;

    case M_TEXT:
        if (tok->kind == AR_TOK_TEXT)
        {
            ar__insert_text(t, tok->text, 0);
            return;
        }
        /* Any end tag, and the tokenizer only produces the matching one here
           because of the appropriate-end-tag rule. */
        ar__pop(t);
        t->tok->state = AR_HTML_DATA;
        t->mode = t->original_mode;
        return;

    case M_IN_TABLE:
        ar__in_table(t, tok);
        return;
    case M_IN_COLUMN_GROUP:
        ar__in_column_group(t, tok);
        return;
    case M_IN_SELECT:
        ar__in_select(t, tok);
        return;
    case M_IN_SELECT_IN_TABLE:
        ar__in_select_in_table(t, tok);
        return;
    case M_IN_TABLE_BODY:
        ar__in_table_body(t, tok);
        return;
    case M_IN_ROW:
        ar__in_row(t, tok);
        return;
    case M_IN_CELL:
        ar__in_cell(t, tok);
        return;
    case M_IN_CAPTION:
        ar__in_caption(t, tok);
        return;

    case M_IN_HEAD_NOSCRIPT:
        if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "noscript"))
        {
            ar__pop(t);
            t->mode = M_IN_HEAD;
            return;
        }
        if (tok->kind == AR_TOK_DOCTYPE)
        {
            /* Ignored, and that matters: falling through would pop the
               noscript, and the comment after it would land in the body. */
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_START &&
            (ar_span_is(tok->name, "head") || ar_span_is(tok->name, "noscript")))
        {
            t->doc->errors++;
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "html"))
        {
            /* A second `<html>` is `in body`'s business wherever it appears,
               and it is not "anything else" -- treating it as such popped the
               noscript and put everything after it in the body. */
            ar__in_body(t, tok);
            return;
        }
        if (tok->kind == AR_TOK_COMMENT || (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text)))
        {
            t->mode = M_IN_HEAD;
            ar__process(t, tok);
            t->mode = M_IN_HEAD_NOSCRIPT;
            return;
        }
        if (tok->kind == AR_TOK_START)
        {
            static const char *const ALLOWED[] = {"basefont", "bgsound", "link", "meta",
                                                  "noframes", "style",   0};

            if (ar__name_in(tok->name, ALLOWED))
            {
                ar__via_head(t, tok, M_IN_HEAD_NOSCRIPT);
                return;
            }
        }
        /*
         * Any other end tag is ignored, and `</br>` is the exception that
         * falls through to "anything else" below.
         *
         * The difference is visible in one token: `<noscript></p><!--c-->`
         * keeps the noscript open and puts the comment inside it, while
         * `<noscript></br><!--c-->` closes the noscript, opens a body, and
         * puts a `<br>` and the comment in it. Treating every end tag as
         * "anything else" got the second right and the first wrong.
         */
        if (tok->kind == AR_TOK_END && !ar_span_is(tok->name, "br"))
        {
            t->doc->errors++;
            return;
        }

        /* Anything else closes it and is reprocessed, which is what puts the
           paragraph in the body rather than inside the noscript. */
        t->doc->errors++;
        ar__pop(t);
        t->mode = M_IN_HEAD;
        ar__process(t, tok);
        return;

    case M_IN_TEMPLATE:
        /*
         * `in template`, §13.2.6.4.18, which is a *dispatcher* and not a mode
         * that content sits in.
         *
         * Each rule sets the insertion mode to something else and reprocesses
         * the token, so `<template><p>x</p>` runs `<p>` under `in body` and
         * then `</p>` under `in body` too -- the parser has left `in template`
         * by the time the end tag arrives. That is why "any other end tag is
         * ignored" is safe here and was not safe when this mode delegated to
         * `in body` while staying put: it saw every end tag in the template
         * and would have swallowed all of them.
         *
         * Coming back out is ar__reset_mode's job. It walks the stack and
         * finds the enclosing template, table, row or body, which is how a
         * nested template resumes the right rules without a stack of modes
         * being kept by hand.
         */
        if (tok->kind == AR_TOK_START)
        {
            static const char *const HEAD_CONTENT[] = {"base",     "basefont", "bgsound", "link",
                                                       "meta",     "noframes", "script",  "style",
                                                       "template", "title",    0};

            if (ar__name_in(tok->name, HEAD_CONTENT))
            {
                /* Head content does not change the template's mode: the
                   specification has no "pop, push" step for these, and a
                   `<style>` inside a template must not undo a `<tr>` before
                   it. */
                t->mode = M_IN_HEAD;
                ar__process_mode(t, tok);
                t->mode = M_IN_TEMPLATE;
                return;
            }
            if (ar_span_is(tok->name, "caption") || ar_span_is(tok->name, "colgroup") ||
                ar_span_is(tok->name, "tbody") || ar_span_is(tok->name, "tfoot") ||
                ar_span_is(tok->name, "thead"))
            {
                ar__tmpl_set(t, M_IN_TABLE);
                t->mode = M_IN_TABLE;
                ar__process_mode(t, tok);
                return;
            }
            if (ar_span_is(tok->name, "col"))
            {
                t->mode = M_IN_COLUMN_GROUP;
                ar__process_mode(t, tok);
                return;
            }
            if (ar_span_is(tok->name, "tr"))
            {
                ar__tmpl_set(t, M_IN_TABLE_BODY);
                t->mode = M_IN_TABLE_BODY;
                ar__process_mode(t, tok);
                return;
            }
            if (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th"))
            {
                ar__tmpl_set(t, M_IN_ROW);
                t->mode = M_IN_ROW;
                ar__process_mode(t, tok);
                return;
            }
            ar__tmpl_set(t, M_IN_BODY);
            t->mode = M_IN_BODY;
            ar__process_mode(t, tok);
            return;
        }

        if (tok->kind == AR_TOK_END)
        {
            if (ar_span_is(tok->name, "template"))
            {
                if (!ar__on_stack_named(t, "template"))
                {
                    t->doc->errors++;
                    return;
                }
                ar__implied_end_tags(t, 0);
                ar__pop_until(t, "template");
                ar__fmt_clear_to_marker(t);
                ar__reset_mode(t);
                return;
            }
            /* A template's contents are a fragment: there is nothing outside
               it for an end tag to name, so it is a parse error and dropped. */
            t->doc->errors++;
            return;
        }

        /* Text, comments and doctypes are `in body`'s business. */
        ar__in_body(t, tok);
        return;

    case M_IN_FRAMESET:
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "frameset"))
        {
            ar__insert_element(t, tok, 0);
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "frame"))
        {
            ar__insert_element(t, tok, 0);
            ar__pop(t);
            return;
        }
        if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "frameset"))
        {
            /* The fragment case, 13.2.6.4.20: when the current node is the
               root html element there is nothing outside it to close, so the
               token is a parse error and nothing else. Popping it left the
               synthetic root gone and every following `<frame>` with nowhere
               to be inserted. */
            if (t->ctx && t->open_n <= 2)
            {
                t->doc->errors++;
                return;
            }
            if (t->open_n > 2)
            {
                ar__pop(t);
            }
            /* The outermost one closes the frameset document. A nested one
               leaves an enclosing frameset behind and changes nothing -- and
               a fragment never leaves `in frameset` at all, because there is
               no document around it for the parser to be after. */
            if (!t->ctx && !ar__is(t, ar__current(t), "frameset"))
            {
                t->mode = M_AFTER_FRAMESET;
            }
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "noframes"))
        {
            ar__via_head(t, tok, M_IN_FRAMESET);
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, -1);
            return;
        }
        /* A frameset document has no body and nothing else belongs in it --
           but the whitespace between its frames is still in the tree. */
        if (tok->kind == AR_TOK_TEXT)
        {
            ar__keep_only_space(t, tok);
            return;
        }
        t->doc->errors++;
        return;

    case M_AFTER_FRAMESET:
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, -1);
            return;
        }
        if (tok->kind == AR_TOK_TEXT)
        {
            ar__keep_only_space(t, tok);
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "noframes"))
        {
            ar__via_head(t, tok, M_AFTER_FRAMESET);
            return;
        }
        if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "html"))
        {
            /* `after after frameset` differs only in where a comment goes: on
               the *document*, beside the html element rather than inside it.
               Putting it on the html element either way was one node out of
               place in four conformance cases. */
            t->mode = M_AFTER_AFTER_FRAMESET;
            return;
        }
        t->doc->errors++;
        return;

    case M_AFTER_AFTER_FRAMESET:
        if (tok->kind == AR_TOK_COMMENT)
        {
            /* The document, or the synthetic root when this is a
               fragment -- a fragment's answer is the root's children,
               so a comment parented on the document is invisible. */
            ar__comment_node(t, tok, t->ctx ? ar_dom_root(t->doc) : 0);
            return;
        }
        if (tok->kind == AR_TOK_TEXT)
        {
            ar__keep_only_space(t, tok);
            return;
        }
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "noframes"))
        {
            ar__via_head(t, tok, M_AFTER_AFTER_FRAMESET);
            return;
        }
        /* A second `<html>` is still the same html element, and its
           attributes are merged onto the one that exists -- 13.2.6.4.22
           routes it to `in body`, which is where that merge lives. Dropping
           it lost `<html a=b>` written after `</html>`, which is exactly the
           shape a template engine emits when it closes the document twice. */
        if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "html"))
        {
            ar__in_body(t, tok);
            return;
        }
        t->doc->errors++;
        return;

    case M_AFTER_BODY:
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, ar_dom_root(t->doc));
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            /* Whitespace after `</body>` goes back *into* the body rather than
               being dropped: `</body>
   <!--c-->` leaves the newline and the
               indent in the tree, which is what every browser shows. */
            ar__in_body(t, tok);
            return;
        }
        if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "html"))
        {
            t->mode = M_AFTER_AFTER_BODY;
            return;
        }
        /* Content after </body> goes back in the body, which is what every
           browser does with the stray text so many pages have there. */
        t->doc->errors++;
        t->mode = M_IN_BODY;
        ar__process(t, tok);
        return;

    case M_AFTER_AFTER_BODY:
        if (tok->kind == AR_TOK_COMMENT)
        {
            /* The document, or the synthetic root when this is a
               fragment -- a fragment's answer is the root's children,
               so a comment parented on the document is invisible. */
            ar__comment_node(t, tok, t->ctx ? ar_dom_root(t->doc) : 0);
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
            ar__in_body(t, tok);
            return;
        }
        t->doc->errors++;
        t->mode = M_IN_BODY;
        ar__process(t, tok);
        return;

    case M_IN_BODY:
    default:
        ar__in_body(t, tok);
        return;
    }
}

/* ------------------------------------------------------------------------
 * The entry point
 * ------------------------------------------------------------------------ */
static int ar__parse_core(ar_doc *doc, const char *bytes, ar_u32 len, char *scratch,
                          ar_u32 scratch_cap, const char *ctx, ar_ns ctx_ns)
{
    ar__tree    t;
    ar_html_tok tk;
    ar_token    tok;

    if (!doc || !doc->nodes || doc->node_cap < 4)
    {
        return 0;
    }

    doc->node_count = 0;
    doc->attr_count = 0;
    doc->text_used = 0;
    doc->errors = 0;
    doc->overflowed = 0;
    doc->quirks = AR_QUIRKS_NO;

    memset(&t, 0, sizeof t);
    t.doc = doc;
    t.tok = &tk;
    t.head = -1;
    t.form = -1;
    t.frameset_ok = 1;
    t.mode = M_INITIAL;
    t.ctx = ctx;
    t.ctx_ns = ctx_ns;

    ar_html_tok_init(&tk, bytes, len, scratch, scratch_cap);

    /* The document node is index 0 and is the bottom of the stack, so every
       "insert into the current node" has somewhere to go before <html>. */
    if (ar__node(&t, AR_DOM_DOCUMENT) != 0)
    {
        return 0;
    }
    ar__push(&t, 0);

    /*
     * Fragment parsing, §13.2.6.5.
     *
     * A synthetic `<html>` root is created and pushed, the insertion mode is
     * reset with the context element standing in for the bottom of the stack,
     * and the tokenizer is put into whatever state that element implies. None
     * of that is bookkeeping: the same bytes are a different document
     * depending on the context, and these three lines are where the difference
     * comes from.
     */
    if (ctx)
    {
        ar_token fake;
        ar_i32   root;

        memset(&fake, 0, sizeof fake);
        fake.kind = AR_TOK_START;
        fake.name.p = "html";
        fake.name.n = 4;
        root = ar__insert_element(&t, &fake, 0);
        if (root < 0)
        {
            return 0;
        }
        /* 13.2.6.5 step 4: a template context pushes `in template` onto the
           stack of template insertion modes, so that the mode reset below
           has something to take. Without it a fragment parsed with a
           template context pops a stack that was never pushed. */
        if (ctx_ns == AR_NS_HTML && ar__lit_is(ctx, "template"))
        {
            ar__tmpl_push(&t, M_IN_TEMPLATE);
        }
        ar__reset_mode(&t);

        /*
         * A `<title>` context makes the whole fragment RCDATA and a `<script>`
         * context makes it script data, which is why `a<b>` inside a title is
         * five characters of text rather than an element.
         *
         * `last_start` is deliberately *not* seeded from the context, and it
         * used to be. An "appropriate end tag token" is one whose name matches
         * the last start tag the tokenizer itself emitted, and in a fragment
         * the tokenizer has emitted none -- 13.2.6.5 switches the state and
         * says nothing about the last start tag, because there is no start tag
         * to be last. So a fragment in one of these states never leaves it,
         * which is the whole point: `</script>` inside a script fragment is
         * five more characters of script, not the end of one.
         *
         * Seeding it read as harmless because nothing else in the suite closes
         * its own context tag. One case does.
         */
        {
            ar_html_state s = AR_HTML_DATA;

            if (ctx_ns == AR_NS_HTML)
            {
                if (ar__lit_is(ctx, "title") || ar__lit_is(ctx, "textarea"))
                {
                    s = AR_HTML_RCDATA;
                }
                else if (ar__lit_is(ctx, "style") || ar__lit_is(ctx, "xmp") ||
                         ar__lit_is(ctx, "iframe") || ar__lit_is(ctx, "noembed") ||
                         ar__lit_is(ctx, "noframes"))
                {
                    s = AR_HTML_RAWTEXT;
                }
                else if (ar__lit_is(ctx, "script"))
                {
                    s = AR_HTML_SCRIPT;
                }
                else if (ar__lit_is(ctx, "plaintext"))
                {
                    s = AR_HTML_PLAINTEXT;
                }
            }
            tk.state = s;
        }
    }

    /*
     * The loop, with a guarantee rather than a hope.
     *
     * `ar_html_next` is required to consume input, and if it ever stops doing
     * so this loop runs forever -- which is exactly what `&#0` with a
     * one-byte scratch buffer did, and a hang is the most expensive failure
     * to diagnose because it produces no output to diagnose from.
     *
     * The tokenizer's own guarantee is the real fix and lives in
     * ar_html_token.c. This is the backstop: one comparison per token, and it
     * turns any future violation into a document that stops early and says
     * `overflowed` instead of a process that has to be killed. A backstop for
     * an invariant that is already enforced is cheap; a hang in a parser
     * reading untrusted input is not.
     */
    for (;;)
    {
        const char *before = tk.p;

        /* The tokenizer needs to know whether `<![CDATA[` opens a section or
           is a bogus comment, and only the stack can say. */
        tk.in_foreign = t.open_n > 1 && !ar__is_html(&t, ar__current(&t));

        if (!ar_html_next(&tk, &tok))
        {
            break;
        }

        /*
         * `<pre>`, `<listing>` and `<textarea>` swallow one newline of their
         * own, §13.2.6.4.7.
         *
         * The rule is about *the next token* -- "if the next token is a U+000A
         * LINE FEED character token, ignore it" -- so it is consumed here,
         * where the next token is, rather than in the insertion mode. Putting
         * it in `in body` would have missed `<textarea>` entirely, whose
         * content arrives in `text` mode.
         *
         * The flag is cleared by whatever token comes next whether or not it
         * begins with a newline, because the specification only ever looks at
         * one.
         *
         * A line feed and nothing else. The tokenizer has already turned a
         * carriage return into one -- §13.2.3.5 is applied where characters
         * are produced, in ar__clean and the data state's own loop -- so
         * `<pre>\rA` arrives here as `\nA` and needs no second rule. And a
         * `&#x000D;` must *not* be caught: a reference is decoded after the
         * input stream is preprocessed, so it is a real carriage return in
         * the tree and stays one.
         */
        if (t.drop_lf)
        {
            t.drop_lf = 0;
            if (tok.kind == AR_TOK_TEXT && tok.text.n && tok.text.p[0] == '\n')
            {
                ++tok.text.p;
                --tok.text.n;
            }
        }

        /*
         * Whitespace before the document starts is dropped one character at a
         * time, and areole's tokens are whole runs.
         *
         * `initial`, `before html` and `before head` all say "a character
         * token that is whitespace: ignore the token" and then hand anything
         * else to the next mode. The specification's tokens are one character
         * each, so a run of `\n]>` is three tokens and the newline is gone
         * before the `]` opens a body. Here it is one token, and testing the
         * whole run for whitespace answers no -- so the newline went into the
         * body with the rest.
         *
         * Trimming here rather than in the modes because all three want it and
         * the run then flows through the reprocessing chain already trimmed.
         * The mode read is the one before the token is processed, which is the
         * right one: these three modes can only be left *because* of the
         * non-whitespace part of this very run.
         */
        if (tok.kind == AR_TOK_TEXT &&
            (t.mode == M_INITIAL || t.mode == M_BEFORE_HTML || t.mode == M_BEFORE_HEAD))
        {
            while (tok.text.n && ar__space_char(tok.text.p[0]))
            {
                ++tok.text.p;
                --tok.text.n;
            }
            /* An empty run is left to fall through rather than skipped, so the
               loop's no-progress backstop still runs on every cycle. Every
               mode treats it as whitespace and ignores it. */
        }

        /*
         * The same split again, one mode later, where the whitespace is kept
         * rather than dropped.
         *
         * `in head` and `after head` both insert a whitespace character and
         * let anything else open a body. Character by character that puts the
         * space in `</style> --> x` inside the head and the rest in the body;
         * run by run the whole thing went to the body and the head lost a text
         * node every browser shows.
         */
        if (tok.kind == AR_TOK_TEXT && (t.mode == M_IN_HEAD || t.mode == M_AFTER_HEAD) &&
            tok.text.n && ar__space_char(tok.text.p[0]) && !ar__all_space(tok.text))
        {
            ar_span lead = tok.text;
            ar_u32  k = 0;

            while (k < lead.n && ar__space_char(lead.p[k]))
            {
                ++k;
            }
            lead.n = k;
            ar__insert_text(&t, lead, 0);
            tok.text.p += k;
            tok.text.n -= k;
        }

        ar__process(&t, &tok);

        /* Note where `before` is taken: across the whole cycle, not across
           ar__process, which moves nothing and would make this fire on every
           token. It did, on the first attempt, and truncated every document in
           the suite to a single node. */
        if (tk.p == before)
        {
            doc->overflowed = 1;
            break;
        }
    }

    /* A reference that did not fit is not a malformed document, it is a
       budget that was too small, and the caller has to be able to tell the
       difference. 0.9.0 acceptance criterion 7. */
    if (tk.scratch_full)
    {
        doc->overflowed = 1;
    }

    /*
     * The end of the file, which every insertion mode has a rule for and all
     * of them come to the same thing: a document has an html, a head and a
     * body whether or not anything was written.
     *
     * An empty file is still `html(head body)` in every browser, and areole
     * returned nothing at all until the tree corpus asked.
     */
    /*
     * A fragment has no implied html, head or body: its answer is the children
     * of the root, and adding a body would put them somewhere else.
     *
     * With one exception, and it is the specification's rather than a
     * convenience: a fragment whose *context* is `html` starts in `before
     * head`, so the same end-of-file walk that gives a document its head and
     * body gives one to this too. `innerHTML` on the html element returns
     * `<head></head><body></body>` in every browser, and an empty fragment
     * against `html` returned nothing at all here.
     */
    if (ctx)
    {
        if (ctx_ns == AR_NS_HTML && ar__lit_is(ctx, "html"))
        {
            if (t.head < 0)
            {
                t.head = ar__insert_implied(&t, "head");
                ar__pop(&t);
            }
            if (ar_dom_child_element(doc, ar_dom_root(doc), "frameset") < 0 &&
                ar_dom_child_element(doc, ar_dom_root(doc), "body") < 0)
            {
                /* Back to the root first, for the reason the document path
                   gives: `ar__insert_implied` inserts into the *current* node,
                   which after `<frameset><span>` is the frameset. */
                while (t.open_n > 2)
                {
                    ar__pop(&t);
                }
                ar__insert_implied(&t, "body");
            }
        }
        doc->errors += tk.errors;
        return !doc->overflowed;
    }

    /*
     * An empty file is a quirks document.
     *
     * The initial insertion mode sets quirks when it meets anything that is
     * not a doctype -- and a file with nothing in it never meets anything at
     * all, so the mode never ran and the document came out in standards
     * mode. Every browser reports BackCompat for it, which is what the
     * doctype corpus asked.
     */
    if (t.mode == M_INITIAL)
    {
        doc->quirks = AR_QUIRKS_YES;
    }

    /*
     * The condition is "unless this document has a frameset", not "unless the
     * insertion mode is early".
     *
     * It used to read `t.mode < M_IN_BODY`, which is true for the modes that
     * come before `in body` in the enumeration and false for every one after
     * it -- including M_TEXT, which is where a document ending in an
     * unterminated `<script>` or `<title>` stops. Those documents came out
     * with no body at all, which is a hundred and forty-one conformance
     * cases and every truncated file on the web.
     *
     * A frameset document genuinely has no body; nothing else is exempt.
     */
    {
        if (ar_dom_root(doc) < 0)
        {
            ar__insert_implied(&t, "html");
        }
        if (t.head < 0)
        {
            t.head = ar__insert_implied(&t, "head");
            ar__pop(&t);
        }
        if (ar_dom_child_element(doc, ar_dom_root(doc), "frameset") < 0 &&
            ar_dom_child_element(doc, ar_dom_root(doc), "body") < 0)
        {
            /*
             * Back to the html element first.
             *
             * The body is a child of html, and `ar__insert_implied` inserts
             * into the *current* node -- which after `<template><p>x</p>
             * </template>` is still the head, because a template is head
             * content. So the body ended up inside the head, which is a tree
             * no browser produces and which the corpus caught the moment a
             * template case was added.
             */
            while (t.open_n > 2)
            {
                ar__pop(&t);
            }
            ar__insert_implied(&t, "body");
        }
    }

    doc->errors += tk.errors;
    return !doc->overflowed;
}

int ar_html_parse(ar_doc *doc, const char *bytes, ar_u32 len, char *scratch, ar_u32 scratch_cap)
{
    return ar__parse_core(doc, bytes, len, scratch, scratch_cap, 0, AR_NS_HTML);
}

int ar_html_parse_fragment(ar_doc *doc, const char *bytes, ar_u32 len, const char *context,
                           ar_ns context_ns, char *scratch, ar_u32 scratch_cap)
{
    if (!context || !*context)
    {
        context = "div";
        context_ns = AR_NS_HTML;
    }
    return ar__parse_core(doc, bytes, len, scratch, scratch_cap, context, context_ns);
}

ar_i32 ar_dom_root(const ar_doc *doc)
{
    ar_i32 c;

    if (!doc || doc->node_count == 0)
    {
        return -1;
    }
    for (c = doc->nodes[0].first_child; c >= 0; c = doc->nodes[c].next_sibling)
    {
        if (doc->nodes[c].kind == AR_DOM_ELEMENT)
        {
            return c;
        }
    }
    return -1;
}

ar_i32 ar_dom_child_element(const ar_doc *doc, ar_i32 i, const char *tag)
{
    ar_i32 c;

    if (!doc || i < 0 || i >= doc->node_count)
    {
        return -1;
    }
    for (c = doc->nodes[i].first_child; c >= 0; c = doc->nodes[c].next_sibling)
    {
        if (doc->nodes[c].kind == AR_DOM_ELEMENT && ar_span_is(doc->nodes[c].name, tag))
        {
            return c;
        }
    }
    return -1;
}
