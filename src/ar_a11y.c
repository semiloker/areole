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
#include "ar_edit.h"

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
    if (ar_span_is(type, "submit") || ar_span_is(type, "reset") || ar_span_is(type, "button") ||
        ar_span_is(type, "image") || ar_span_is(type, "file") || ar_span_is(type, "color"))
    {
        /* A file field and a colour field are buttons that open something:
           that is how every platform announces them, with the chosen file or
           colour as the value. */
        return AR_ROLE_BUTTON;
    }
    if (ar_span_is(type, "number"))
    {
        return AR_ROLE_SPINBUTTON;
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
 * Whether the document has been walked into boxes. Before the first walk every
 * node reads -1 and none of them is hidden; after one, -1 is a node the walk
 * did not build -- inside a closed `<details>`, under the head -- which nobody
 * can see and a name must not read out.
 */
static int ar__walked(const ar_doc *d)
{
    ar_i32 i;

    for (i = 0; i < d->node_count; ++i)
    {
        if (d->nodes[i].kind == AR_DOM_ELEMENT)
        {
            return d->nodes[i].box >= 0;
        }
    }
    return 0;
}

/*
 * Content that is never part of a name. A select's options and a textarea's
 * text are values, not labels, and a paragraph holding a field was named
 * "Address 12 Mill Lane" for it; script and style are not content at all.
 */
static int ar__never_named(const ar_doc *d, ar_i32 node)
{
    static const char *const TAGS[] = {"select",   "textarea", "option", "optgroup",
                                       "datalist", "script",   "style",  "template"};
    ar_i32                   k;

    for (k = 0; k < 8; ++k)
    {
        if (ar_span_is(d->nodes[node].name, TAGS[k]))
        {
            return 1;
        }
    }
    return 0;
}

/*
 * The text under an element, flattened, which is what "its content" means when
 * ARIA says to use it.
 *
 * Depth-limited rather than unbounded: a name is a sentence a person listens
 * to, and an element wrapping half a document has no useful name however far
 * this walks. Stopping is better than producing a paragraph.
 *
 * Below the element it was asked about, what is not rendered is skipped --
 * accname's "hidden and not referenced" -- so a closed `<details>` does not
 * read its secret out through whatever contains it. The element itself is
 * read whatever its state, because `aria-labelledby` may point at a hidden
 * one on purpose.
 */
static void ar__text_of(const ar_doc *d, ar_i32 node, char *buf, ar_u32 cap, ar_u32 *used,
                        int depth)
{
    ar_i32 child;

    if (node < 0 || depth > 8 || *used + 1 >= cap)
    {
        return;
    }
    if (depth > 0 && d->nodes[node].box < 0 && ar__walked(d))
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
    if (depth > 0 && (ar_a11y_role(d, node) == AR_ROLE_BUTTON || ar__never_named(d, node)))
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

/*
 * The roles whose content is their name, which accname calls "name from
 * content": the controls a label is written inside, a heading, a cell, an
 * option -- and here a paragraph and a list item, because MSAA has no text
 * interface and a static text item's name is how a reader gets its words.
 *
 * Not a group, a form, a region or a landmark. Those are named by markup or
 * not at all: a `<form>` was named with every word in it, which a reader
 * announces in full each time the focus enters it.
 */
static int ar__named_by_content(ar_u8 role)
{
    switch (role)
    {
    case AR_ROLE_NONE:
    case AR_ROLE_BUTTON:
    case AR_ROLE_LINK:
    case AR_ROLE_CHECKBOX:
    case AR_ROLE_RADIO:
    case AR_ROLE_OPTION:
    case AR_ROLE_HEADING:
    case AR_ROLE_PARAGRAPH:
    case AR_ROLE_LISTITEM:
    case AR_ROLE_ROW:
    case AR_ROLE_CELL:
    case AR_ROLE_COLUMNHEADER:
        return 1;
    default:
        return 0;
    }
}

/* The first child element with this tag, for the elements HTML names by one
   of their own children: a fieldset by its legend, a table by its caption. */
static ar_i32 ar__first_child_tag(const ar_doc *d, ar_i32 node, const char *tag)
{
    ar_i32 c;

    for (c = d->nodes[node].first_child; c >= 0; c = d->nodes[c].next_sibling)
    {
        if (d->nodes[c].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[c].name, tag))
        {
            return c;
        }
    }
    return -1;
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

    /* 3b. The child HTML names it by: a fieldset's legend, a table's caption
           -- HTML-AAM's step for each, ahead of content, which a group does
           not have a name from. */
    {
        ar_i32 by = -1;

        if (ar_span_is(d->nodes[node].name, "fieldset"))
        {
            by = ar__first_child_tag(d, node, "legend");
        }
        else if (ar_span_is(d->nodes[node].name, "table"))
        {
            by = ar__first_child_tag(d, node, "caption");
        }
        if (by >= 0)
        {
            ar__text_of(d, by, buf, cap, &used, 0);
            used = ar__trim(buf, used);
            if (used > 0)
            {
                return used;
            }
        }
    }

    /*
     * 4. The element's content -- but only where content is a label rather
     *    than a value, and only for a role named by its content at all. A
     *    button's text names it; a text field's text is what the user typed,
     *    and announcing that as the field's name is how a form comes to have
     *    five fields all called by whatever was last entered.
     */
    if (ar__named_by_content(role) && !ar_span_is(d->nodes[node].name, "input"))
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

    /* 6. What a browser writes on a submit or reset button that has no value
          of its own -- which is what the button says, so it is its name. */
    if (role == AR_ROLE_BUTTON && ar_span_is(d->nodes[node].name, "input") && used == 0)
    {
        ar_span type = ar_a11y_attr(d, node, "type");

        if (type.p && ar_span_is(type, "submit"))
        {
            ar__cat(buf, cap, &used, "Submit", 6);
        }
        else if (type.p && ar_span_is(type, "reset"))
        {
            ar__cat(buf, cap, &used, "Reset", 5);
        }
        else if (type.p && ar_span_is(type, "file"))
        {
            ar__cat(buf, cap, &used, "Choose file", 11);
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

/* ------------------------------------------------------------------------
 * The tree, flattened, for a backend to adapt
 *
 * Built from the document and the boxes it became, in the window between
 * ar_frame_end and the next ar_frame_begin -- the one moment both are whole.
 * The document says what each thing is; the box says whether it exists this
 * frame, where it is, and what state it is in.
 * ------------------------------------------------------------------------ */

/* Deeper than any document anybody reads. A node past it is not listed --
   stated rather than discovered, and a page nested 256 deep has problems a
   screen reader is not the first to notice. */
#define AR__A11Y_DEPTH 256

static int ar__is_stop_key(const ar_ctx *c, ar_u32 key)
{
    ar_i32 i;

    for (i = 0; i < c->focusable_prev_n; ++i)
    {
        if (c->focusables_prev[i] == key)
        {
            return 1;
        }
    }
    return 0;
}

static int ar__box_ok(const ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 b = d->nodes[node].box;

    return b >= 0 && b < c->node_count;
}

/* Is this a heading, and which: `<h1>` to `<h6>` are level 1 to 6, and
   `aria-level` overrides it the way `role` overrides a tag. */
static ar_u8 ar__heading_level(const ar_doc *d, ar_i32 node)
{
    ar_span name = d->nodes[node].name;
    ar_i32  lvl = ar_dom_attr_num(d, node, "aria-level", 0) / 1000;

    if (lvl >= 1 && lvl <= 9)
    {
        return (ar_u8)lvl;
    }
    if (name.n == 2 && (name.p[0] == 'h' || name.p[0] == 'H') && name.p[1] >= '1' &&
        name.p[1] <= '6')
    {
        return (ar_u8)(name.p[1] - '0');
    }
    return 0;
}

static int ar__input_is(const ar_doc *d, ar_i32 node, const char *type)
{
    ar_span t = ar_a11y_attr(d, node, "type");

    return ar_span_is(d->nodes[node].name, "input") && t.p && ar_span_is(t, type);
}

/* Is this node inside a select that is shut? Its options are then the
   select's business and not separately in the tree, which is what every
   platform does with a collapsed combobox. */
static int ar__in_shut_select(const ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 up;

    for (up = d->nodes[node].parent; up >= 0; up = d->nodes[up].parent)
    {
        if (d->nodes[up].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[up].name, "select"))
        {
            return !(ar__box_ok(c, d, up) && (c->nodes[d->nodes[up].box].state & AR_STATE_OPEN));
        }
    }
    return 0;
}

static ar_u32 ar__item_state(const ar_ctx *c, const ar_doc *d, ar_i32 node, ar_u8 role)
{
    const ar_node *n = &c->nodes[d->nodes[node].box];
    ar_u32         st = ar_a11y_state(n->state, c->focus_key != 0 && n->key == c->focus_key);

    if (ar__is_stop_key(c, n->key))
    {
        st |= AR_A11Y_FOCUSABLE;
    }
    if (ar_a11y_attr(d, node, "readonly").p)
    {
        st |= AR_A11Y_READONLY;
    }
    if (ar__input_is(d, node, "password"))
    {
        st |= AR_A11Y_PROTECTED;
    }
    if ((role == AR_ROLE_COMBOBOX || ar_span_is(d->nodes[node].name, "details")) &&
        !(n->state & AR_STATE_OPEN))
    {
        st |= AR_A11Y_COLLAPSED;
    }
    if (role == AR_ROLE_OPTION && (n->state & AR_STATE_CHECKED))
    {
        st = (st & ~(ar_u32)AR_A11Y_CHECKED) | AR_A11Y_SELECTED;
    }
    if (ar_rect_is_empty(ar_rect_intersect(n->rect, n->clip)))
    {
        st |= AR_A11Y_OFFSCREEN;
    }
    return st;
}

ar_i32 ar_a11y_tree(const ar_ctx *c, const ar_doc *d, ar_a11y_item *out, ar_i32 cap)
{
    ar_i32 anc[AR__A11Y_DEPTH];
    ar_i32 root, node, depth = 0, count = 0;

    if (!c || !d || d->node_count <= 0)
    {
        return 0;
    }
    root = ar_dom_root(d);
    if (root < 0)
    {
        return 0;
    }

    /*
     * Pre-order over the document, by its sibling links -- a loop rather than
     * recursion, so a document nested deeper than the stack is listed rather
     * than overflowed. `anc[k]` is the nearest listed ancestor-or-self of the
     * node at depth k, so each item's parent is a lookup, not a search.
     */
    node = root;
    for (;;)
    {
        int    descend = 1;
        ar_i32 parent_item = depth > 0 ? anc[depth - 1] : -1;

        anc[depth] = parent_item;
        if (d->nodes[node].kind == AR_DOM_ELEMENT)
        {
            if (!ar__box_ok(c, d, node))
            {
                /* No box this frame -- a shut details, the head, a script --
                   and so nothing a reader should be told is there. */
                descend = 0;
            }
            else
            {
                const ar_node *n = &c->nodes[d->nodes[node].box];
                ar_u8          role;

                if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE || (n->state & AR_STATE_INERT) ||
                    ar_a11y_attr(d, node, "aria-hidden").p)
                {
                    descend = 0;
                }
                else if ((role = ar_a11y_role(d, node)) != AR_ROLE_NONE &&
                         !(role == AR_ROLE_OPTION && ar__in_shut_select(c, d, node)))
                {
                    if (count < cap && out)
                    {
                        ar_a11y_item *it = &out[count];

                        it->node = node;
                        it->box = d->nodes[node].box;
                        it->parent = parent_item;
                        it->role = role;
                        it->level = role == AR_ROLE_HEADING ? ar__heading_level(d, node) : 0;
                        it->pad_[0] = 0;
                        it->pad_[1] = 0;
                        it->rect = n->rect;
                        it->state = ar__item_state(c, d, node, role);
                    }
                    anc[depth] = count;
                    ++count;
                }
            }
        }
        else
        {
            descend = 0;
        }

        if (descend && d->nodes[node].first_child >= 0 && depth + 1 < AR__A11Y_DEPTH)
        {
            node = d->nodes[node].first_child;
            ++depth;
            continue;
        }
        while (node != root && d->nodes[node].next_sibling < 0)
        {
            node = d->nodes[node].parent;
            --depth;
        }
        if (node == root || node < 0)
        {
            break;
        }
        node = d->nodes[node].next_sibling;
    }
    return count;
}

/* ------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------ */

static ar_u32 ar__put_num1000(char *buf, ar_u32 cap, ar_u32 used, ar_i32 v)
{
    char   tmp[16];
    ar_i32 k = 0, whole, frac;

    if (v < 0 && used < cap)
    {
        buf[used++] = '-';
        v = -v;
    }
    whole = v / 1000;
    frac = v % 1000;
    do
    {
        tmp[k++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole > 0);
    while (k > 0 && used < cap)
    {
        buf[used++] = tmp[--k];
    }
    if (frac && used + 1 < cap)
    {
        buf[used++] = '.';
        buf[used++] = (char)('0' + frac / 100);
        if (frac % 100 && used < cap)
        {
            buf[used++] = (char)('0' + (frac / 10) % 10);
            if (frac % 10 && used < cap)
            {
                buf[used++] = (char)('0' + frac % 10);
            }
        }
    }
    return used;
}

ar_u32 ar_a11y_value(const ar_ctx *c, const ar_doc *d, ar_i32 node, char *buf, ar_u32 cap)
{
    ar_u32 used = 0;
    ar_u8  role;
    ar_u32 key;

    if (!c || !d || node < 0 || node >= d->node_count || !buf || cap == 0)
    {
        return 0;
    }
    buf[0] = 0;
    role = ar_a11y_role(d, node);
    key = ar_ctl_key_of(c, node);

    if (role == AR_ROLE_TEXTBOX || role == AR_ROLE_SEARCHBOX || role == AR_ROLE_SPINBUTTON ||
        ar__input_is(d, node, "file"))
    {
        const char *p = 0;
        ar_u32      n = 0;

        if (!ar_field_value_of(c, key, &p, &n))
        {
            /* Untouched: a textarea's value is its content, an input's is its
               attribute. */
            ar_span v = ar_a11y_attr(d, node, "value");

            if (ar_span_is(d->nodes[node].name, "textarea"))
            {
                ar_i32 ch = d->nodes[node].first_child;

                v.p = 0;
                v.n = 0;
                if (ch >= 0 && d->nodes[ch].kind == AR_DOM_TEXT)
                {
                    v = d->nodes[ch].text;
                }
            }
            p = v.p;
            n = v.p ? v.n : 0;
        }
        if (ar__input_is(d, node, "password"))
        {
            /* One bullet a cluster, never the characters: the value of a
               password field is its length, as far as anybody listening is
               concerned. */
            ar_u32 k = ar_cluster_count(p ? p : "", n);

            while (k-- > 0 && used + 3 < cap)
            {
                buf[used++] = (char)0xE2;
                buf[used++] = (char)0x80;
                buf[used++] = (char)0xA2;
            }
        }
        else
        {
            ar__cat(buf, cap, &used, p ? p : "", n);
        }
    }
    else if (role == AR_ROLE_SLIDER)
    {
        ar_i32 lo = ar_dom_attr_num(d, node, "min", 0);
        ar_i32 hi = ar_dom_attr_num(d, node, "max", 100000);
        ar_i32 v = ar_ctl_value(c, key, ar_dom_attr_num(d, node, "value", lo + (hi - lo) / 2));

        used = ar__put_num1000(buf, cap - 1, used, v);
    }
    else if (role == AR_ROLE_COMBOBOX)
    {
        ar_i32 opt = ar_dom_select_option(c, d, node);

        if (opt >= 0)
        {
            ar__text_of(d, opt, buf, cap, &used, 0);
            used = ar__trim(buf, used);
        }
    }
    else if (role == AR_ROLE_PROGRESSBAR || role == AR_ROLE_METER)
    {
        int    meter = role == AR_ROLE_METER;
        ar_i32 lo = meter ? ar_dom_attr_num(d, node, "min", 0) : 0;
        ar_i32 hi = ar_dom_attr_num(d, node, "max", meter ? 1000 : 1000);
        ar_i32 v = ar_dom_attr_num(d, node, "value", lo);
        ar_i32 pct = hi > lo ? (v - lo) * 100 / (hi - lo) : 0;

        if (!meter && !ar_a11y_attr(d, node, "value").p)
        {
            return 0; /* indeterminate: there is no number to say */
        }
        used = ar__put_num1000(buf, cap - 2, used, (pct < 0 ? 0 : pct > 100 ? 100 : pct) * 1000);
        buf[used++] = '%';
    }
    else if (ar__input_is(d, node, "color"))
    {
        static const char HEX[] = "0123456789abcdef";
        ar_span           v = ar_a11y_attr(d, node, "value");
        ar_i32            rgb = 0, i;

        if (v.p && v.n == 7 && v.p[0] == '#')
        {
            for (i = 1; i < 7; ++i)
            {
                char ch = v.p[i];
                int  h = (ch >= '0' && ch <= '9')   ? ch - '0'
                         : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                         : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                                    : 0;

                rgb = rgb * 16 + h;
            }
        }
        rgb = ar_ctl_value(c, key, rgb);
        if (cap > 7)
        {
            buf[used++] = '#';
            for (i = 20; i >= 0; i -= 4)
            {
                buf[used++] = HEX[(rgb >> i) & 15];
            }
        }
    }
    else if (role == AR_ROLE_LINK)
    {
        ar_span href = ar_a11y_attr(d, node, "href");

        ar__cat(buf, cap, &used, href.p ? href.p : "", href.p ? href.n : 0);
    }
    if (used >= cap)
    {
        used = cap - 1;
    }
    buf[used] = 0;
    return used;
}

/* ------------------------------------------------------------------------
 * What an assistive tool can do
 * ------------------------------------------------------------------------ */

int ar_a11y_activate(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_u32 key;
    ar_i32 i;

    if (!c || !d || node < 0 || node >= d->node_count || !ar__box_ok(c, d, node))
    {
        return 0;
    }
    key = c->nodes[d->nodes[node].box].key;
    for (i = 0; i < c->control_n; ++i)
    {
        if (c->control_key[i] == key)
        {
            /* Fired at the end of the next frame, through the same path a
               click takes -- so a reader's "press" and a mouse's cannot do
               different things. */
            c->synth_fire = key;
            return 1;
        }
    }
    return 0;
}

int ar_a11y_focus(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_u32 key;

    if (!c || !d || node < 0 || node >= d->node_count || !ar__box_ok(c, d, node))
    {
        return 0;
    }
    key = c->nodes[d->nodes[node].box].key;
    if (!ar__is_stop_key(c, key))
    {
        return 0;
    }
    c->focus_key = key;
    c->focus_visible = 1; /* moved without a pointer, so it is drawn */
    c->focus_moved = 1;
    return 1;
}
