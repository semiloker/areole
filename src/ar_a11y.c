/*
 * ar_a11y.c -- the accessibility tree.
 *
 * A screen reader does not read the box tree. It reads a second tree, derived
 * from the first, in which every node has a role, a name, a value and a set of
 * states -- and in which the boxes that exist only to hold a background or to
 * fix up malformed markup are not there at all.
 *
 * This file computes that tree. It does not talk to any platform: MSAA and UI
 * Automation on Windows, AT-SPI on Linux and NSAccessibility on macOS are
 * shapes the backend adapts to, and the core never learns what an IAccessible
 * is. What the core owes them is a stable tree with correct names, and the
 * accessible-name algorithm is where correctness actually lives.
 *
 * ------------------------------------------------------------------------
 * Why the name is the hard part
 *
 * A role is a lookup. A name is an algorithm, and getting it wrong produces a
 * screen reader that says "button button button" -- which is the failure
 * everybody has heard and nobody can debug from the outside, because the page
 * looks right.
 *
 * ARIA gives the order and the order is the whole of it: `aria-labelledby`
 * first, because it names another element and that element's text wins over
 * anything local; then `aria-label`, an author's own string; then the
 * element's own labelling markup -- a `<label for>` or one wrapping it, a
 * `<summary>`, a `<legend>`; then the element's text content; then, for the
 * few controls that have one, an attribute like `alt`, `title` or `value`.
 *
 * Skipping a step is how a button labelled by an icon reads as its filename,
 * and taking them in the wrong order is how an `aria-label` that was added to
 * fix exactly that gets ignored.
 *
 * ------------------------------------------------------------------------
 * What is not in the tree
 *
 * Anonymous boxes, generated boxes, and anything inert.
 *
 * An anonymous box is one areole invented -- a row to hold cells that had no
 * row, a block to hold inline content that needed one. It corresponds to
 * nothing in the document and a reader that announced it would be announcing
 * this engine's internals. `ar-mark` and `ar-bar`, the boxes a checkbox and a
 * gauge are built from, are the same kind of thing: they are how the control
 * is drawn, not what it is.
 *
 * Inert is not merely hidden: it is switched off. A modal makes the rest of
 * the document inert, and a reader that walks into it is reading a page the
 * user cannot reach.
 */

#include "ar_a11y.h"
#include "ar_node.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Roles
 * ------------------------------------------------------------------------ */

/*
 * Tag to role, and the table is short because a role is only worth having when
 * it changes what a reader says or does.
 *
 * `<div>` and `<span>` have none on purpose. A generic role is noise: a reader
 * that announces "group" for every wrapper on the page buries the three things
 * that mattered. An element with no role is walked through to its children.
 */
static const struct
{
    const char *tag;
    ar_u8       role;
} AR__ROLE[] = {{"a", AR_ROLE_LINK},
                {"article", AR_ROLE_ARTICLE},
                {"aside", AR_ROLE_COMPLEMENTARY},
                {"button", AR_ROLE_BUTTON},
                {"details", AR_ROLE_GROUP},
                {"dialog", AR_ROLE_DIALOG},
                {"fieldset", AR_ROLE_GROUP},
                {"footer", AR_ROLE_CONTENTINFO},
                {"form", AR_ROLE_FORM},
                {"h1", AR_ROLE_HEADING},
                {"h2", AR_ROLE_HEADING},
                {"h3", AR_ROLE_HEADING},
                {"h4", AR_ROLE_HEADING},
                {"h5", AR_ROLE_HEADING},
                {"h6", AR_ROLE_HEADING},
                {"header", AR_ROLE_BANNER},
                {"img", AR_ROLE_IMAGE},
                {"li", AR_ROLE_LISTITEM},
                {"main", AR_ROLE_MAIN},
                {"meter", AR_ROLE_METER},
                {"nav", AR_ROLE_NAVIGATION},
                {"ol", AR_ROLE_LIST},
                {"option", AR_ROLE_OPTION},
                {"p", AR_ROLE_PARAGRAPH},
                {"progress", AR_ROLE_PROGRESSBAR},
                {"section", AR_ROLE_REGION},
                {"select", AR_ROLE_COMBOBOX},
                {"summary", AR_ROLE_BUTTON},
                {"table", AR_ROLE_TABLE},
                {"td", AR_ROLE_CELL},
                {"textarea", AR_ROLE_TEXTBOX},
                {"th", AR_ROLE_COLUMNHEADER},
                {"tr", AR_ROLE_ROW},
                {"ul", AR_ROLE_LIST}};

#define AR__ROLE_N ((ar_i32)(sizeof AR__ROLE / sizeof AR__ROLE[0]))

/*
 * `<input>` is not one element, it is fourteen, and its role comes from `type`.
 *
 * A checkbox and a text field share a tag and share nothing else: they are
 * announced differently, operated differently, and carry different states. A
 * table keyed on the tag alone would give both of them "textbox", which is the
 * single most misleading thing this file could say.
 */
static ar_u8 ar__input_role(ar_span type)
{
    if (!type.p || type.n == 0)
    {
        return AR_ROLE_TEXTBOX; /* no type is `text`, which is the default */
    }
    if (ar_span_is(type, "checkbox"))
    {
        return AR_ROLE_CHECKBOX;
    }
    if (ar_span_is(type, "radio"))
    {
        return AR_ROLE_RADIO;
    }
    if (ar_span_is(type, "range"))
    {
        return AR_ROLE_SLIDER;
    }
    if (ar_span_is(type, "submit") || ar_span_is(type, "reset") || ar_span_is(type, "button"))
    {
        return AR_ROLE_BUTTON;
    }
    if (ar_span_is(type, "hidden"))
    {
        return AR_ROLE_NONE;
    }
    if (ar_span_is(type, "search"))
    {
        return AR_ROLE_SEARCHBOX;
    }
    return AR_ROLE_TEXTBOX;
}

ar_u8 ar_a11y_role(const ar_doc *d, ar_i32 node)
{
    ar_span name;
    ar_span role;
    ar_i32  lo = 0, hi = AR__ROLE_N;

    if (!d || node < 0)
    {
        return AR_ROLE_NONE;
    }
    name = d->nodes[node].name;

    /*
     * `role=` wins over the tag, which is the whole point of the attribute:
     * an author who writes `<div role="button">` has told the reader what the
     * thing is, and an engine that prefers the tag has ignored the one piece
     * of information that was put there deliberately.
     */
    role = ar_a11y_attr(d, node, "role");
    if (role.p && role.n > 0)
    {
        ar_i32 i;

        for (i = 0; i < AR__ROLE_N; ++i)
        {
            if (ar_span_is(role, AR__ROLE[i].tag))
            {
                return AR__ROLE[i].role;
            }
        }
        if (ar_span_is(role, "button"))
        {
            return AR_ROLE_BUTTON;
        }
        if (ar_span_is(role, "checkbox"))
        {
            return AR_ROLE_CHECKBOX;
        }
        if (ar_span_is(role, "heading"))
        {
            return AR_ROLE_HEADING;
        }
        /* An unknown role is not a reason to fall back to the tag: the author
           meant something by it, and announcing the tag instead is announcing
           something they deliberately overrode. */
        return AR_ROLE_NONE;
    }

    if (ar_span_is(name, "input"))
    {
        return ar__input_role(ar_a11y_attr(d, node, "type"));
    }

    /* Bisected, so the table stays a table rather than a chain of compares.
       ar_test asserts the ordering. */
    while (lo < hi)
    {
        ar_i32 mid = (lo + hi) / 2;
        int    cmp = ar_span_cmp(name, AR__ROLE[mid].tag);

        if (cmp == 0)
        {
            /* A link is only a link with an href. Without one it is an anchor,
               which is markup and not a control, and announcing it as a link
               sends somebody to press Enter on nothing. */
            if (AR__ROLE[mid].role == AR_ROLE_LINK && !ar_a11y_attr(d, node, "href").p)
            {
                return AR_ROLE_NONE;
            }
            return AR__ROLE[mid].role;
        }
        if (cmp < 0)
        {
            hi = mid;
        }
        else
        {
            lo = mid + 1;
        }
    }
    return AR_ROLE_NONE;
}

/* ------------------------------------------------------------------------
 * Names
 * ------------------------------------------------------------------------ */

/* Append, never overflowing, and never leaving the buffer unterminated. */
static void ar__cat(char *buf, ar_u32 cap, ar_u32 *used, const char *p, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i < n && *used + 1 < cap; ++i)
    {
        buf[(*used)++] = p[i];
    }
    buf[*used] = 0;
}

/*
 * The text under an element, flattened, which is what "its content" means when
 * ARIA says to use it.
 *
 * Depth-limited rather than unbounded: a name is a sentence a person listens
 * to, and an element wrapping half a document has no useful name however far
 * this walks. Stopping is better than producing a paragraph.
 */
static void ar__text_of(const ar_doc *d, ar_i32 node, char *buf, ar_u32 cap, ar_u32 *used,
                        int depth)
{
    ar_i32 child;

    if (node < 0 || depth > 8 || *used + 1 >= cap)
    {
        return;
    }
    if (d->nodes[node].kind == AR_DOM_TEXT)
    {
        ar_span t = d->nodes[node].text;

        if (t.p)
        {
            /* One space between runs, so `<b>Save</b> file` is "Save file"
               and not "Savefile". */
            if (*used > 0 && buf[*used - 1] != ' ')
            {
                ar__cat(buf, cap, used, " ", 1);
            }
            ar__cat(buf, cap, used, t.p, t.n);
        }
        return;
    }
    if (d->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return;
    }
    /* A nested control's own text is not its container's name: a button inside
       a list item labels the button. */
    if (depth > 0 && ar_a11y_role(d, node) == AR_ROLE_BUTTON)
    {
        return;
    }
    for (child = d->nodes[node].first_child; child >= 0; child = d->nodes[child].next_sibling)
    {
        ar__text_of(d, child, buf, cap, used, depth + 1);
    }
}

/* The element whose `id` matches, searched from the root because
   `aria-labelledby` may name anything in the document. */
static ar_i32 ar__by_id(const ar_doc *d, ar_span id)
{
    ar_i32 i;

    if (!id.p || id.n == 0)
    {
        return -1;
    }
    for (i = 0; i < d->node_count; ++i)
    {
        if (d->nodes[i].kind == AR_DOM_ELEMENT)
        {
            ar_span other = ar_a11y_attr(d, i, "id");

            if (other.p && other.n == id.n && memcmp(other.p, id.p, id.n) == 0)
            {
                return i;
            }
        }
    }
    return -1;
}

/*
 * A `<label>` for this control: one naming it by `for`, or one wrapping it.
 *
 * Both spellings are in real markup and neither is rare. Supporting only the
 * explicit one leaves every `<label>Name <input></label>` on the web unnamed,
 * which is the commoner of the two by some distance.
 */
static ar_i32 ar__label_for(const ar_doc *d, ar_i32 node)
{
    ar_span id = ar_a11y_attr(d, node, "id");
    ar_i32  i;

    if (id.p && id.n > 0)
    {
        for (i = 0; i < d->node_count; ++i)
        {
            if (d->nodes[i].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[i].name, "label"))
            {
                ar_span f = ar_a11y_attr(d, i, "for");

                if (f.p && f.n == id.n && memcmp(f.p, id.p, id.n) == 0)
                {
                    return i;
                }
            }
        }
    }
    for (i = d->nodes[node].parent; i >= 0; i = d->nodes[i].parent)
    {
        if (ar_span_is(d->nodes[i].name, "label"))
        {
            return i;
        }
    }
    return -1;
}

/*
 * Trim the ends, and collapse nothing in the middle.
 *
 * A name is spoken, and `" Save "` and `"Save"` are the same word to a
 * listener and different strings to everything that compares them -- a test, a
 * cache, a platform that dedupes announcements. The whitespace comes from the
 * markup and from joining text runs, neither of which the author meant as part
 * of the name.
 */
static int ar__space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static ar_u32 ar__trim(char *buf, ar_u32 used)
{
    ar_u32 lead = 0;

    while (used > 0 && ar__space(buf[used - 1]))
    {
        --used;
    }
    while (lead < used && ar__space(buf[lead]))
    {
        ++lead;
    }
    if (lead > 0)
    {
        ar_u32 i;

        for (i = 0; i + lead < used; ++i)
        {
            buf[i] = buf[i + lead];
        }
        used -= lead;
    }
    buf[used] = 0;
    return used;
}

ar_u32 ar_a11y_name(const ar_doc *d, ar_i32 node, char *buf, ar_u32 cap)
{
    ar_u32 used = 0;
    ar_u8  role;

    if (!d || node < 0 || !buf || cap == 0)
    {
        return 0;
    }
    buf[0] = 0;
    role = ar_a11y_role(d, node);

    /*
     * The order below is the algorithm, and the order is the whole of it.
     * Skipping a step is how a button labelled by an icon reads as its
     * filename; taking them out of order is how the `aria-label` somebody
     * added to fix exactly that gets ignored.
     */

    /* 1. aria-labelledby, which names another element and outranks anything
          local -- including an aria-label on this same element. */
    {
        ar_span by = ar_a11y_attr(d, node, "aria-labelledby");

        if (by.p && by.n > 0)
        {
            ar_i32 other = ar__by_id(d, by);

            if (other >= 0)
            {
                ar__text_of(d, other, buf, cap, &used, 0);
                used = ar__trim(buf, used);
                if (used > 0)
                {
                    return used;
                }
            }
        }
    }

    /* 2. aria-label, the author's own string. */
    {
        ar_span lbl = ar_a11y_attr(d, node, "aria-label");

        if (lbl.p && lbl.n > 0)
        {
            ar__cat(buf, cap, &used, lbl.p, lbl.n);
            return ar__trim(buf, used);
        }
    }

    /* 3. The element's own labelling markup. */
    {
        ar_i32 lab = ar__label_for(d, node);

        if (lab >= 0)
        {
            ar__text_of(d, lab, buf, cap, &used, 0);
            used = ar__trim(buf, used);
            if (used > 0)
            {
                return used;
            }
        }
    }

    /*
     * 4. The element's content -- but only where content is a label rather
     *    than a value. A button's text names it; a text field's text is what
     *    the user typed, and announcing that as the field's name is how a form
     *    comes to have five fields all called by whatever was last entered.
     */
    if (role != AR_ROLE_TEXTBOX && role != AR_ROLE_SEARCHBOX)
    {
        ar__text_of(d, node, buf, cap, &used, 0);
        used = ar__trim(buf, used);
        if (used > 0)
        {
            return used;
        }
    }

    /* 5. The attributes that stand in for content on elements that have none:
          `alt` on an image, `title` on anything, `value` on a push button. */
    {
        static const char *ATTRS[3];
        ar_i32             i;

        ATTRS[0] = "alt";
        ATTRS[1] = "value";
        ATTRS[2] = "title";
        for (i = 0; i < 3; ++i)
        {
            ar_span a = ar_a11y_attr(d, node, ATTRS[i]);

            /* `value` names a push button and never a text field, where it is
               the contents. */
            if (i == 1 && role != AR_ROLE_BUTTON)
            {
                continue;
            }
            if (a.p && a.n > 0)
            {
                ar__cat(buf, cap, &used, a.p, a.n);
                return ar__trim(buf, used);
            }
        }
    }

    return ar__trim(buf, used);
}

/* ------------------------------------------------------------------------
 * States
 * ------------------------------------------------------------------------ */

ar_u32 ar_a11y_state(ar_u32 box_state, int focused)
{
    ar_u32 out = 0;

    if (box_state & AR_STATE_CHECKED)
    {
        out |= AR_A11Y_CHECKED;
    }
    if (box_state & AR_STATE_DISABLED)
    {
        out |= AR_A11Y_DISABLED;
    }
    if (box_state & AR_STATE_OPEN)
    {
        out |= AR_A11Y_EXPANDED;
    }
    if (box_state & AR_STATE_INERT)
    {
        out |= AR_A11Y_DISABLED;
    }
    if (focused)
    {
        out |= AR_A11Y_FOCUSED;
    }
    return out;
}

/* ------------------------------------------------------------------------
 * Two small things this file needs and ar_dom.c keeps to itself
 * ------------------------------------------------------------------------ */

ar_span ar_a11y_attr(const ar_doc *d, ar_i32 node, const char *name)
{
    ar_span none;
    ar_i32  i;

    none.p = 0;
    none.n = 0;
    if (!d || node < 0 || d->nodes[node].attr_first < 0)
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

int ar_span_cmp(ar_span a, const char *lit)
{
    ar_u32 i;

    for (i = 0; i < a.n; ++i)
    {
        unsigned char x = (unsigned char)a.p[i];
        unsigned char y = (unsigned char)lit[i];

        if (x >= 'A' && x <= 'Z')
        {
            x = (unsigned char)(x + 32);
        }
        if (lit[i] == 0)
        {
            return 1;
        }
        if (x != y)
        {
            return x < y ? -1 : 1;
        }
    }
    return lit[a.n] == 0 ? 0 : -1;
}
