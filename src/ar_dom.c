/*
 * areole - the document, into the box tree.
 * SPDX-License-Identifier: MIT
 *
 * The parser builds a tree of elements; the layout engine lays out a tree of
 * boxes. This is the walk between them, and it is the piece that makes any of
 * 0.9.0 visible: without it `ar_html_parse` builds a document nothing renders.
 *
 * ------------------------------------------------------------------------
 * Why it goes through ar_begin rather than building boxes directly
 *
 * `ar_begin` and `ar_text` are the front end every other part of areole uses,
 * and they already do the three things this walk would otherwise have to
 * repeat: resolve the style, assign the stable key that hover and damage
 * tracking are built on, and keep the pre-order invariant five layout passes
 * depend on.
 *
 * So HTML is a second *front end* rather than a second box builder, which is
 * what the release document means by "both build the same box tree". A bug
 * fixed in one is fixed in both.
 *
 * ------------------------------------------------------------------------
 * The selector string
 *
 * `ar_begin` takes the same syntax a stylesheet does -- `div.card#first` --
 * and an element's tag, class and id are exactly that. So the walk spells one
 * out per element into a small buffer, which `ar_begin` consumes immediately;
 * it hashes the parts and keeps no pointer, so the buffer does not outlive the
 * call.
 *
 * A class attribute holding more names than the selector syntax carries is
 * truncated rather than refused, and the count says so.
 */
#include "ar_html.h"
#include "ar_node.h"

#include <string.h>

/* Long enough for `tag` plus four classes plus an id at the lengths real
   markup uses. Anything past it is dropped, which is a visibly unstyled box
   rather than an overrun. */
#define AR_DOM_SEL 192

static ar_span ar__attr_of(const ar_doc *d, ar_i32 node, const char *name)
{
    ar_span none;
    ar_i32  i;

    none.p = 0;
    none.n = 0;
    if (node < 0 || d->nodes[node].attr_first < 0)
    {
        return none;
    }
    for (i = 0; i < d->nodes[node].attr_count; ++i)
    {
        const ar_attr *a = &d->attrs[d->nodes[node].attr_first + i];

        if (ar_span_is(a->name, name))
        {
            return a->value;
        }
    }
    return none;
}

static void ar__put(char *buf, ar_u32 *used, char c)
{
    if (*used + 1 < AR_DOM_SEL)
    {
        buf[(*used)++] = c;
    }
}

static void ar__put_span(char *buf, ar_u32 *used, ar_span s)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        ar__put(buf, used, s.p[i]);
    }
}

/*
 * `tag.a.b#id`, in the order ar_begin's parser expects.
 *
 * The class attribute is a space-separated list and the selector syntax spells
 * each one with a dot, so the spaces become dots.
 */
static void ar__selector(const ar_doc *d, ar_i32 node, char *buf)
{
    ar_u32  used = 0;
    ar_span klass = ar__attr_of(d, node, "class");
    ar_span id = ar__attr_of(d, node, "id");

    ar__put_span(buf, &used, d->nodes[node].name);

    if (klass.n > 0)
    {
        ar_u32 i;
        int    open = 0;

        for (i = 0; i < klass.n; ++i)
        {
            char c = klass.p[i];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f')
            {
                open = 0;
                continue;
            }
            if (!open)
            {
                ar__put(buf, &used, '.');
                open = 1;
            }
            ar__put(buf, &used, c);
        }
    }
    if (id.n > 0)
    {
        ar__put(buf, &used, '#');
        ar__put_span(buf, &used, id);
    }
    buf[used] = 0;
}

/*
 * Whitespace-only text between block-level elements is not content.
 *
 * `<ul>\n  <li>a</li>\n</ul>` has three text nodes in it that a browser drops
 * on the floor, and every hand-written document is full of them. Keeping them
 * would put an empty box between every pair of list items.
 *
 * This is a simplification of the specification's rule, which is about inline
 * formatting contexts rather than about the text itself, and it is right for
 * everything except `white-space: pre` -- which is 0.5.1 and is named in
 * CSS_REFERENCE as absent.
 */
static int ar__ignorable(ar_span s)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        char c = s.p[i];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f')
        {
            return 0;
        }
    }
    return 1;
}

static void ar__walk(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    char   sel[AR_DOM_SEL];
    ar_i32 child;

    if (node < 0)
    {
        return;
    }

    if (d->nodes[node].kind == AR_DOM_TEXT)
    {
        if (!ar__ignorable(d->nodes[node].text) && d->nodes[node].text.p)
        {
            /* The text is NUL-terminated in the document's own buffer, which
               is why ar_html_tree.c stores it there rather than leaving it a
               span of the input. */
            ar_text(c, "span", d->nodes[node].text.p);
        }
        return;
    }
    if (d->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return; /* comments and the doctype generate no box */
    }

    ar__selector(d, node, sel);
    ar_begin(c, sel);
    for (child = d->nodes[node].first_child; child >= 0; child = d->nodes[child].next_sibling)
    {
        ar__walk(c, d, child);
    }
    ar_end(c);
}

void ar_dom_build(ar_ctx *c, const ar_doc *d)
{
    if (!c || !d)
    {
        return;
    }
    ar__walk(c, d, ar_dom_root(d));
}

/*
 * Every `<style>` element's text, handed to the stylesheet parser in tree
 * order.
 *
 * Tree order is cascade order: two rules of equal specificity are decided by
 * which came last, so the sheets have to arrive in the order the document
 * declares them. That is why this is a walk rather than a search for the first
 * one.
 *
 * A `<style>` element holds exactly one text child, because the tokenizer put
 * the whole element in RAWTEXT -- so there is no reassembly to do here, which
 * there would be if `<` inside a stylesheet had been read as markup.
 *
 * Not handled: `<link rel=stylesheet>`, which needs a resource the embedder
 * has to fetch, and there is no networking here by design. The release
 * document gives that a callback and 0.9.1 is where it lands.
 */
static ar_i32 ar__collect_styles(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 found = 0;
    ar_i32 child;

    if (node < 0)
    {
        return 0;
    }
    if (d->nodes[node].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[node].name, "style"))
    {
        ar_i32 text = d->nodes[node].first_child;

        if (text >= 0 && d->nodes[text].kind == AR_DOM_TEXT && d->nodes[text].text.p &&
            d->nodes[text].text.n > 0)
        {
            ar_stylesheet(c, d->nodes[text].text.p);
            ++found;
        }
        return found;
    }
    for (child = d->nodes[node].first_child; child >= 0; child = d->nodes[child].next_sibling)
    {
        found += ar__collect_styles(c, d, child);
    }
    return found;
}

ar_i32 ar_doc_stylesheets(ar_ctx *c, const ar_doc *d)
{
    if (!c || !d || d->node_count == 0)
    {
        return 0;
    }
    return ar__collect_styles(c, d, 0);
}
