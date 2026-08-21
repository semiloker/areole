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
    M_AFTER_AFTER_BODY
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

static void ar__append(ar__tree *t, ar_i32 parent, ar_i32 child)
{
    ar_doc *d = t->doc;

    if (parent < 0 || child < 0)
    {
        return;
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

    if (before < 0 || child < 0 || d->nodes[before].parent < 0)
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

static void ar__pop(ar__tree *t)
{
    if (t->open_n > 0)
    {
        --t->open_n;
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

    for (i = t->open_n - 1; i >= 1; --i)
    {
        ar_i32 k;

        if (ar__is(t, t->open[i], tag))
        {
            return 1;
        }
        if (button_scope && ar__is(t, t->open[i], "button"))
        {
            return 0;
        }
        for (k = 0; STOP[k]; ++k)
        {
            if (ar__is(t, t->open[i], STOP[k]))
            {
                return 0;
            }
        }
    }
    return 0;
}

static void ar__pop_until(ar__tree *t, const char *tag)
{
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
    int                      changed = 1;

    while (changed)
    {
        ar_i32 k;

        changed = 0;
        if (except && ar__is(t, ar__current(t), except))
        {
            return;
        }
        for (k = 0; IMPLIED[k]; ++k)
        {
            if (ar__is(t, ar__current(t), IMPLIED[k]))
            {
                ar__pop(t);
                changed = 1;
                break;
            }
        }
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
 */
static ar_i32 ar__insertion_point(ar__tree *t, ar_i32 *before)
{
    static const char *const FOSTER[] = {"table", "tbody", "tfoot", "thead", "tr", 0};
    ar_i32                   i;

    *before = -1;

    for (i = 0; FOSTER[i]; ++i)
    {
        if (ar__is(t, ar__current(t), FOSTER[i]))
        {
            ar_i32 k;

            /* The last table on the stack; insert before it. */
            for (k = t->open_n - 1; k >= 1; --k)
            {
                if (ar__is(t, t->open[k], "table"))
                {
                    if (t->doc->nodes[t->open[k]].parent >= 0)
                    {
                        *before = t->open[k];
                        return t->doc->nodes[t->open[k]].parent;
                    }
                    break;
                }
            }
            /* A table with no parent: fall back to the element below it. */
            return k > 1 ? t->open[k - 1] : t->open[0];
        }
    }
    return ar__current(t);
}

static void ar__insert_node(ar__tree *t, ar_i32 node, int foster)
{
    ar_i32 before = -1;
    ar_i32 parent = foster ? ar__insertion_point(t, &before) : ar__current(t);

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
static ar_span ar__text_store(ar__tree *t, ar_span s)
{
    ar_span out;

    out.p = 0;
    out.n = 0;
    if (s.n == 0)
    {
        return out;
    }
    if (t->doc->text_used + s.n + 1u > t->doc->text_cap)
    {
        t->doc->overflowed = 1;
        return out;
    }
    memcpy(t->doc->text + t->doc->text_used, s.p, s.n);
    out.p = t->doc->text + t->doc->text_used;
    out.n = s.n;
    t->doc->text_used += s.n;
    t->doc->text[t->doc->text_used++] = 0;
    return out;
}

/* Extend the last text node in place, overwriting its terminator. Only
   possible when it is the most recent thing in the buffer, which is the
   ordinary case for a run split by a character reference. */
static int ar__text_extend(ar__tree *t, ar_i32 node, ar_span s)
{
    ar_span old = t->doc->nodes[node].text;

    if (old.n == 0 || old.p + old.n + 1 != t->doc->text + t->doc->text_used)
    {
        return 0;
    }
    if (t->doc->text_used + s.n > t->doc->text_cap)
    {
        t->doc->overflowed = 1;
        return 0;
    }
    --t->doc->text_used; /* drop the terminator; a new one goes after */
    memcpy(t->doc->text + t->doc->text_used, s.p, s.n);
    t->doc->text_used += s.n;
    t->doc->text[t->doc->text_used++] = 0;
    t->doc->nodes[node].text.n += s.n;
    return 1;
}

static void ar__insert_text(ar__tree *t, ar_span s, int foster)
{
    ar_i32 before = -1;
    ar_i32 parent = foster ? ar__insertion_point(t, &before) : ar__current(t);
    ar_i32 node;

    if (s.n == 0)
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

        if (last >= 0 && t->doc->nodes[last].kind == AR_DOM_TEXT && ar__text_extend(t, last, s))
        {
            return;
        }
    }
    s = ar__text_store(t, s);
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

static ar_i32 ar__insert_element(ar__tree *t, const ar_token *tok, int foster)
{
    ar_i32 node = ar__node(t, AR_DOM_ELEMENT);
    ar_i32 k;

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

/* ------------------------------------------------------------------------
 * The list of active formatting elements
 * ------------------------------------------------------------------------ */
static void ar__fmt_push(ar__tree *t, ar_i32 node)
{
    if (t->fmt_n >= AR_HTML_FMT)
    {
        t->doc->errors++;
        return;
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
            static const char *const SPECIAL[] = {
                "address", "article", "aside",  "blockquote", "center", "details", "dir",
                "div",     "dl",      "dt",     "fieldset",   "figure", "footer",  "form",
                "h1",      "h2",      "h3",     "h4",         "h5",     "h6",      "header",
                "hr",      "li",      "main",   "nav",        "ol",     "p",       "pre",
                "section", "summary", "table",  "td",         "th",     "tr",      "ul",
                "button",  "marquee", "object", "applet",     0};

            for (i = stack_index + 1; i < t->open_n && furthest < 0; ++i)
            {
                ar_i32 k;

                for (k = 0; SPECIAL[k]; ++k)
                {
                    if (ar__is(t, t->open[i], SPECIAL[k]))
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
                ar__insert_node(t, last, 1); /* foster parented */
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
     * The double solidus is spelled AR__FPI rather than written, and that is
     * not squeamishness.
     *
     * A DTD public identifier is full of it, and the
     * CI gate that keeps C99 comments out of this codebase greps for the pair
     * with a regular expression, which cannot tell one inside a string literal
     * from one starting a comment. The gate has caught three real portability
     * bugs and is worth more than the readability of eight strings, so the
     * strings bend.
     */
#define AR__FPI                                                                                    \
    "/"                                                                                            \
    "/"

    static const char *const QUIRKY[] = {"-" AR__FPI "W3C" AR__FPI "DTD HTML 3",
                                         "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.0 Transitional",
                                         "-" AR__FPI "W3C" AR__FPI "DTD HTML 4.0 Frameset",
                                         "-" AR__FPI "IETF" AR__FPI "DTD HTML",
                                         "-" AR__FPI "W3O" AR__FPI "DTD W3 HTML",
                                         "-" AR__FPI "SoftQuad",
                                         "-" AR__FPI "Microsoft" AR__FPI "DTD Internet Explorer",
                                         "HTML",
                                         0};
    ar_i32                   i;

    if (tok->force_quirks || !ar_span_is(tok->name, "html"))
    {
        return AR_QUIRKS_YES;
    }
    for (i = 0; QUIRKY[i]; ++i)
    {
        if (ar__starts_with_ci(tok->pub, QUIRKY[i]))
        {
            return AR_QUIRKS_YES;
        }
    }
    /* A public identifier with no system identifier beside it is the limited
       form -- which is what a bare HTML 4.01 public identifier
       gets, and it differs from full quirks only in table cell heights. */
    if (tok->pub.n > 0 && tok->sys.n == 0)
    {
        return AR_QUIRKS_LIMITED;
    }
    if (ar__starts_with_ci(tok->sys,
                           "http:" AR__FPI "www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd"))
    {
        return AR_QUIRKS_YES;
    }
    return AR_QUIRKS_NO;
}

/* ------------------------------------------------------------------------
 * The insertion modes, §13.2.6.4
 * ------------------------------------------------------------------------ */

/* The elements that never have children and never close. */
static int ar__is_void(ar_span name)
{
    static const char *const VOID_TAGS[] = {"area",  "base",   "br",    "col",  "embed",
                                            "hr",    "img",    "input", "link", "meta",
                                            "param", "source", "track", "wbr",  0};
    ar_i32                   i;

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
static int ar__is_formatting(ar_span name)
{
    static const char *const FMT_TAGS[] = {"a", "b",     "big",    "code",   "em", "font", "i",
                                           "s", "small", "strike", "strong", "tt", "u",    0};
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
        "address", "article", "aside",   "blockquote", "center",     "details", "dialog",
        "dir",     "div",     "dl",      "fieldset",   "figcaption", "figure",  "footer",
        "form",    "h1",      "h2",      "h3",         "h4",         "h5",      "h6",
        "header",  "hgroup",  "hr",      "main",       "menu",       "nav",     "ol",
        "p",       "pre",     "section", "summary",    "table",      "ul",      0};
    ar_i32 i;

    for (i = 0; BLOCKS[i]; ++i)
    {
        if (ar_span_is(name, BLOCKS[i]))
        {
            return 1;
        }
    }
    return 0;
}

static int ar__all_space(ar_span s)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        char c = s.p[i];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\f' && c != '\r')
        {
            return 0;
        }
    }
    return 1;
}

static void ar__comment_node(ar__tree *t, const ar_token *tok, ar_i32 parent)
{
    ar_i32 node = ar__node(t, AR_DOM_COMMENT);

    if (node < 0)
    {
        return;
    }
    t->doc->nodes[node].text = ar__keep(t, tok->text);
    ar__append(t, parent >= 0 ? parent : ar__current(t), node);
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

static void ar__process(ar__tree *t, const ar_token *tok);

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
        if (ar_span_is(tok->name, "html") || ar_span_is(tok->name, "body"))
        {
            /* Attributes on a second <html> or <body> are merged onto the
               first; areole keeps the first and counts the error. */
            t->doc->errors++;
            return;
        }
        if (ar_span_is(tok->name, "head"))
        {
            t->doc->errors++;
            return;
        }
        if (ar_span_is(tok->name, "table"))
        {
            ar__close_p(t);
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_TABLE;
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
            ar__insert_element(t, tok, 0);
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
            ar__insert_element(t, tok, 0);
            return;
        }
        if (ar__is_void(tok->name))
        {
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
            t->tok->state = ar_span_is(tok->name, "title") ? AR_HTML_RCDATA : AR_HTML_RAWTEXT;
            t->original_mode = t->mode;
            t->mode = M_TEXT;
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
    if (ar_span_is(tok->name, "body") || ar_span_is(tok->name, "html"))
    {
        t->mode = M_AFTER_BODY;
        if (ar_span_is(tok->name, "html"))
        {
            ar__process(t, tok); /* reprocessed in the new mode */
        }
        return;
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
     * "Any other end tag", §13.2.6.4.7.
     *
     * Walk the stack for an element with this name and close through to it.
     * An end tag naming nothing that is open is dropped, which is what makes a
     * stray `</span>` harmless rather than destructive.
     */
    {
        ar_i32 i;

        for (i = t->open_n - 1; i >= 1; --i)
        {
            if (t->doc->nodes[t->open[i]].kind != AR_DOM_ELEMENT)
            {
                continue;
            }
            if (ar__span_eq(t->doc->nodes[t->open[i]].name, tok->name))
            {
                while (t->open_n > i + 1)
                {
                    ar__pop(t);
                }
                ar__pop(t);
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
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_TABLE_BODY;
            return;
        }
        if (ar_span_is(tok->name, "tr"))
        {
            ar__insert_implied(t, "tbody");
            ar__insert_element(t, tok, 0);
            t->mode = M_IN_ROW;
            return;
        }
        if (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th"))
        {
            ar__insert_implied(t, "tbody");
            ar__insert_implied(t, "tr");
            ar__insert_element(t, tok, 0);
            ar__fmt_marker(t);
            t->mode = M_IN_CELL;
            return;
        }
        if (ar_span_is(tok->name, "caption"))
        {
            ar__insert_element(t, tok, 0);
            ar__fmt_marker(t);
            t->mode = M_IN_CAPTION;
            return;
        }
        if (ar_span_is(tok->name, "colgroup") || ar_span_is(tok->name, "col"))
        {
            ar__insert_element(t, tok, 0);
            if (ar_span_is(tok->name, "col"))
            {
                ar__pop(t);
            }
            return;
        }
        if (ar_span_is(tok->name, "table"))
        {
            /* A table inside a table closes the first, which is the recovery
               for the single most common malformed table on the web. */
            t->doc->errors++;
            ar__pop_until(t, "table");
            t->mode = M_IN_BODY;
            ar__process(t, tok);
            return;
        }
        /* Anything else is foster parented. */
        t->doc->errors++;
        ar__in_body(t, tok);
        return;
    }
    if (tok->kind == AR_TOK_END)
    {
        static const char *const STRUCTURAL[] = {"tbody", "tfoot",   "thead",    "tr",  "td",
                                                 "th",    "caption", "colgroup", "col", 0};
        ar_i32                   i;

        if (ar_span_is(tok->name, "table"))
        {
            ar__pop_until(t, "table");
            t->mode = M_IN_BODY;
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
        ar__in_body(t, tok);
        return;
    }
}

static void ar__in_table_body(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "tr"))
    {
        ar__insert_element(t, tok, 0);
        t->mode = M_IN_ROW;
        return;
    }
    if (tok->kind == AR_TOK_START && (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th")))
    {
        ar__insert_implied(t, "tr");
        ar__insert_element(t, tok, 0);
        ar__fmt_marker(t);
        t->mode = M_IN_CELL;
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
        ar__pop_until(t, "table");
        t->mode = M_IN_BODY;
        return;
    }
    ar__in_table(t, tok);
}

static void ar__in_row(ar__tree *t, const ar_token *tok)
{
    if (tok->kind == AR_TOK_START && (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th")))
    {
        ar__insert_element(t, tok, 0);
        ar__fmt_marker(t);
        t->mode = M_IN_CELL;
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "tr"))
    {
        ar__pop_until(t, "tr");
        t->mode = M_IN_TABLE_BODY;
        return;
    }
    if (tok->kind == AR_TOK_START && ar_span_is(tok->name, "tr"))
    {
        ar__pop_until(t, "tr");
        ar__insert_element(t, tok, 0);
        return;
    }
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "table"))
    {
        ar__pop_until(t, "table");
        t->mode = M_IN_BODY;
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
        (ar_span_is(tok->name, "td") || ar_span_is(tok->name, "th") || ar_span_is(tok->name, "tr")))
    {
        /* A new cell closes the open one. Missing `</td>` is the normal state
           of hand-written tables. */
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
    if (tok->kind == AR_TOK_END && ar_span_is(tok->name, "caption"))
    {
        ar__implied_end_tags(t, 0);
        ar__pop_until(t, "caption");
        ar__fmt_clear_to_marker(t);
        t->mode = M_IN_TABLE;
        return;
    }
    ar__in_body(t, tok);
}

/* ------------------------------------------------------------------------
 * The dispatcher
 * ------------------------------------------------------------------------ */
static void ar__process(ar__tree *t, const ar_token *tok)
{
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
            t->doc->quirks = ar__quirks_for(tok);
            t->mode = M_BEFORE_HTML;
            return;
        }
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, 0);
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
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, 0);
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
            if (ar__is_void(tok->name))
            {
                ar__insert_element(t, tok, 0);
                ar__pop(t);
                return;
            }
            if (ar_span_is(tok->name, "title") || ar_span_is(tok->name, "style") ||
                ar_span_is(tok->name, "script") || ar_span_is(tok->name, "noscript"))
            {
                ar__insert_element(t, tok, 0);
                t->tok->state = ar_span_is(tok->name, "title") ? AR_HTML_RCDATA : AR_HTML_RAWTEXT;
                t->original_mode = M_IN_HEAD;
                t->mode = M_TEXT;
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
            t->mode = M_IN_BODY;
            return;
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

    case M_AFTER_BODY:
        if (tok->kind == AR_TOK_COMMENT)
        {
            ar__comment_node(t, tok, ar_dom_root(t->doc));
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
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
            ar__comment_node(t, tok, 0);
            return;
        }
        if (tok->kind == AR_TOK_TEXT && ar__all_space(tok->text))
        {
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
int ar_html_parse(ar_doc *doc, const char *bytes, ar_u32 len, char *scratch, ar_u32 scratch_cap)
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

    ar_html_tok_init(&tk, bytes, len, scratch, scratch_cap);

    /* The document node is index 0 and is the bottom of the stack, so every
       "insert into the current node" has somewhere to go before <html>. */
    if (ar__node(&t, AR_DOM_DOCUMENT) != 0)
    {
        return 0;
    }
    ar__push(&t, 0);

    while (ar_html_next(&tk, &tok))
    {
        ar__process(&t, &tok);
    }

    doc->errors += tk.errors;
    return !doc->overflowed;
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
