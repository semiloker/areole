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

/* How much of a `style=""` attribute is kept. Long enough for the inline
   styles real markup carries -- a colour, a width, a couple of margins -- and
   truncation cuts back to the last complete declaration rather than leaving
   half of one, so what survives is always something the parser can read. */
#define AR_DOM_STYLE 256

/* And for the declarations built from an element's legacy attributes. Every
   one of them is short -- a colour, a length, a keyword -- and no element
   carries more than a handful. */
#define AR_DOM_HINTS 160

static void ar__put(char *buf, ar_u32 *used, ar_u32 cap, char c)
{
    if (*used + 1 < cap)
    {
        buf[(*used)++] = c;
    }
}

static void ar__put_span(char *buf, ar_u32 *used, ar_u32 cap, ar_span s)
{
    ar_u32 i;

    for (i = 0; i < s.n; ++i)
    {
        ar__put(buf, used, cap, s.p[i]);
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

    ar__put_span(buf, &used, AR_DOM_SEL, d->nodes[node].name);

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
                ar__put(buf, &used, AR_DOM_SEL, '.');
                open = 1;
            }
            ar__put(buf, &used, AR_DOM_SEL, c);
        }
    }
    if (id.n > 0)
    {
        ar__put(buf, &used, AR_DOM_SEL, '#');
        ar__put_span(buf, &used, AR_DOM_SEL, id);
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

static int ar__space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/*
 * `white-space: normal`, which is what every element in an HTML document has
 * until a stylesheet says otherwise: a run of whitespace is one space, and a
 * newline is whitespace rather than a line break.
 *
 * This is the front end's job rather than the tokenizer's. The tree has to
 * keep the bytes exactly -- html5lib compares text node by node and would fail
 * 1,884 cases the moment a newline went missing -- so the collapsing happens
 * here, on the way into a box, where CSS says it happens.
 *
 * Without it a document is a good deal taller than the browser renders it,
 * because markup is *written* with newlines: `<td>exact` followed by a newline
 * and the next row's indentation is one line of text in a browser and was two
 * here. On the interface example that made every table row 62 pixels where
 * Edge draws 39.
 *
 * Collapsed *in place*, in the document's own text buffer, because `ar_text`
 * keeps the pointer it is given rather than copying: a shared scratch buffer
 * would leave every span on the page pointing at whatever was collapsed last,
 * which is exactly what the first attempt did. Collapsing only ever shortens,
 * so it fits where it stands, and it is idempotent -- which it has to be,
 * since `ar_dom_build` runs every frame.
 *
 * That is why the document is not const here. Call `ar_doc_stylesheets`
 * before this, not after, which is the order both examples already use.
 */
static void ar__collapse(ar_doc *d, ar_span *text)
{
    char  *src;
    ar_u32 n = 0;
    ar_u32 i = 0;

    /*
     * Only text the tree builder copied into the document's own buffer, which
     * is every text node -- but the check is here rather than assumed, because
     * a span that still points into the caller's bytes must not be written to.
     * The offset is taken from `d->text`, which is not const, so no cast
     * throws away a qualifier the compiler is right to defend.
     */
    if (!text->p || !d->text || text->p < d->text || text->p >= d->text + d->text_cap)
    {
        return;
    }
    src = d->text + (text->p - d->text);

    while (src[i])
    {
        if (ar__space(src[i]))
        {
            while (src[i] && ar__space(src[i]))
            {
                ++i;
            }
            /*
             * Not trimmed at either end, and that is deliberate: the space
             * between `<b>bold</b>` and `<i>italic</i>` is its own text node,
             * and trimming it runs the two words together. A trailing space at
             * the end of a line costs nothing anybody can see.
             */
            src[n++] = ' ';
            continue;
        }
        src[n++] = src[i++];
    }
    src[n] = 0;
    text->n = n;
}

/* The elements whose contents keep their whitespace. `white-space` is not a
   property here yet, so the list is by name -- which is what the user-agent
   stylesheet would say if it could. */
static int ar__preformatted(ar_span name)
{
    return ar_span_is(name, "pre") || ar_span_is(name, "textarea") || ar_span_is(name, "listing") ||
           ar_span_is(name, "xmp") || ar_span_is(name, "plaintext");
}

/*
 * An element's `style` attribute, as a NUL-terminated declaration list.
 *
 * The attribute's value is a span into the document rather than a string, so
 * it has to be copied somewhere before the style engine can be handed it. The
 * caller's buffer is a stack one, which is why ar_begin_styled copies again --
 * this one is gone as soon as the element's children have been walked.
 *
 * Truncation cuts back to the last semicolon, so a `style` longer than the
 * buffer loses whole declarations rather than ending mid-value. A value cut in
 * half is not merely dropped: `width: 40p` is a parse error that the sheet
 * counts, and `color: #ff000` is a different colour.
 *
 * Returns the buffer, or null if there was nothing to copy.
 */
/* ------------------------------------------------------------------------
 * Presentational hints
 *
 * The attributes HTML had before it had CSS: `<td bgcolor=red>`,
 * `<table border=1 cellspacing=4>`, `<font size=5>`, `<p align=center>`,
 * `<img width=200>`. The specification defines each of them as a declaration,
 * and every browser still obeys them, because a great deal of the web was
 * written before 1998 and has not been touched since.
 *
 * They are a **band of the cascade**, not a `style` attribute. Above the
 * user-agent stylesheet, below every author rule -- so a page that says
 * `td { background: white }` gets white however many `bgcolor`s the markup
 * carries, and a page that says nothing gets the markup's colour. Getting that
 * order wrong in either direction is visible on real documents: hints below
 * the UA sheet do nothing at all, and hints above the author's make a
 * restyled table impossible.
 * ------------------------------------------------------------------------ */

/* The digits of a legacy length: `width="200"` is pixels, `width="50%"` is a
   percentage, and anything else is not a length at all. Returns the number of
   characters used, or 0 -- and `pct` says which of the two it was. */
static ar_u32 ar__legacy_len(ar_span v, ar_i32 *out, int *pct)
{
    ar_u32 i = 0;
    ar_i32 n = 0;

    *pct = 0;
    while (i < v.n && (v.p[i] == ' ' || v.p[i] == '\t' || v.p[i] == '\n' || v.p[i] == '\r'))
    {
        ++i;
    }
    if (i >= v.n || v.p[i] < '0' || v.p[i] > '9')
    {
        return 0;
    }
    while (i < v.n && v.p[i] >= '0' && v.p[i] <= '9')
    {
        if (n < 100000)
        {
            n = n * 10 + (v.p[i] - '0');
        }
        ++i;
    }
    if (i < v.n && v.p[i] == '%')
    {
        *pct = 1;
        ++i;
    }
    *out = n;
    return i;
}

static void ar__put_str(char *buf, ar_u32 *used, ar_u32 cap, const char *s)
{
    while (*s)
    {
        ar__put(buf, used, cap, *s++);
    }
}

static void ar__put_num(char *buf, ar_u32 *used, ar_u32 cap, ar_i32 n)
{
    char   tmp[12];
    ar_i32 i = 0;

    if (n <= 0)
    {
        ar__put(buf, used, cap, '0');
        return;
    }
    while (n > 0 && i < 11)
    {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (i > 0)
    {
        ar__put(buf, used, cap, tmp[--i]);
    }
}

/*
 * `width="200"` -> `width:200px`, `width="50%"` -> `width:50%`.
 *
 * A value that is not a legacy length writes nothing rather than writing
 * something the parser will reject: `width="auto"` is not a hint, it is
 * markup a browser ignores, and turning it into a parse error would make the
 * sheet's error tally lie about the page.
 */
static void ar__hint_len(char *buf, ar_u32 *used, const char *prop, ar_span v)
{
    ar_i32 n = 0;
    int    pct = 0;

    if (v.n == 0 || ar__legacy_len(v, &n, &pct) == 0)
    {
        return;
    }
    ar__put_str(buf, used, AR_DOM_HINTS, prop);
    ar__put(buf, used, AR_DOM_HINTS, ':');
    ar__put_num(buf, used, AR_DOM_HINTS, n);
    ar__put_str(buf, used, AR_DOM_HINTS, pct ? "%;" : "px;");
}

/*
 * A legacy colour, which is not a CSS colour.
 *
 * `bgcolor=red`, `bgcolor="#f00"` and `bgcolor=FF0000` are all legal HTML and
 * all mean the same thing. Only the second is legal CSS, so this is a
 * translation and not a copy -- and it is HTML's job rather than the style
 * parser's, because these are HTML's own rules and apply to nothing else.
 *
 * What is handled: the sixteen colour keywords HTML names, a hash colour of
 * three or six digits, and a bare hex triple or sextet with the hash left off.
 * That is what markup contains. The specification's full algorithm goes
 * further -- it pads, truncates and reinterprets anything at all into a
 * colour, so `bgcolor="hello world"` is a real colour in a browser -- and the
 * rest of it is deliberately not here: it turns typing mistakes into colours,
 * and a page relying on that is not a page this engine has to match.
 *
 * Anything not recognised writes nothing at all, rather than writing a value
 * the CSS parser will refuse. A refusal would be counted in the sheet's error
 * tally, and that tally is what tells anyone whether a page's *CSS* is broken.
 */
static const char *const AR__HTML_COLORS[] = {
    "black",   "#000000", "silver",  "#c0c0c0", "gray",    "#808080", "white",
    "#ffffff", "maroon",  "#800000", "red",     "#ff0000", "purple",  "#800080",
    "fuchsia", "#ff00ff", "green",   "#008000", "lime",    "#00ff00", "olive",
    "#808000", "yellow",  "#ffff00", "navy",    "#000080", "blue",    "#0000ff",
    "teal",    "#008080", "aqua",    "#00ffff", 0,         0};

static void ar__hint_color(char *buf, ar_u32 *used, const char *prop, ar_span v)
{
    ar_u32 i;
    int    hex;

    if (v.n == 0)
    {
        return;
    }
    for (i = 0; AR__HTML_COLORS[i]; i += 2)
    {
        if (ar_span_is(v, AR__HTML_COLORS[i]))
        {
            ar__put_str(buf, used, AR_DOM_HINTS, prop);
            ar__put(buf, used, AR_DOM_HINTS, ':');
            ar__put_str(buf, used, AR_DOM_HINTS, AR__HTML_COLORS[i + 1]);
            ar__put(buf, used, AR_DOM_HINTS, ';');
            return;
        }
    }

    hex = 1;
    for (i = (v.p[0] == '#' ? 1u : 0u); i < v.n; ++i)
    {
        char c = v.p[i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        {
            hex = 0;
            break;
        }
    }
    if (!hex)
    {
        return;
    }
    i = v.p[0] == '#' ? v.n - 1u : v.n;
    if (i != 3 && i != 6)
    {
        return;
    }
    ar__put_str(buf, used, AR_DOM_HINTS, prop);
    ar__put(buf, used, AR_DOM_HINTS, ':');
    if (v.p[0] != '#')
    {
        ar__put(buf, used, AR_DOM_HINTS, '#');
    }
    ar__put_span(buf, used, AR_DOM_HINTS, v);
    ar__put(buf, used, AR_DOM_HINTS, ';');
}

/* `align` is text-align on a block and on a cell, and the two extra values
   HTML has for it are spellings of the two CSS has. */
static void ar__hint_align(char *buf, ar_u32 *used, ar_span v)
{
    const char *css = 0;

    if (ar_span_is(v, "left"))
    {
        css = "left";
    }
    else if (ar_span_is(v, "right"))
    {
        css = "right";
    }
    else if (ar_span_is(v, "center") || ar_span_is(v, "middle"))
    {
        css = "center";
    }
    else if (ar_span_is(v, "justify"))
    {
        css = "justify";
    }
    if (css)
    {
        ar__put_str(buf, used, AR_DOM_HINTS, "text-align:");
        ar__put_str(buf, used, AR_DOM_HINTS, css);
        ar__put(buf, used, AR_DOM_HINTS, ';');
    }
}

/*
 * Every presentational hint this element carries, as a declaration list.
 *
 * Which attributes are hints depends on the element -- `border` is a border on
 * a table and on an image and nothing at all on a `<div>`, and `width` is a
 * width on the handful of elements that ever had it. Attributes are read by
 * name rather than walked, because an element has few of these and the walk is
 * over every attribute it has.
 */
static const char *ar__hints(const ar_doc *d, ar_i32 node, char *buf)
{
    ar_span name = d->nodes[node].name;
    ar_u32  used = 0;
    int     table = ar_span_is(name, "table");
    int     cell = ar_span_is(name, "td") || ar_span_is(name, "th");
    int row = ar_span_is(name, "tr") || ar_span_is(name, "thead") || ar_span_is(name, "tbody") ||
              ar_span_is(name, "tfoot");
    int sized = table || cell || ar_span_is(name, "img") || ar_span_is(name, "col") ||
                ar_span_is(name, "hr") || ar_span_is(name, "canvas") || ar_span_is(name, "video") ||
                ar_span_is(name, "iframe") || ar_span_is(name, "embed") ||
                ar_span_is(name, "object");

    if (table || cell || row || ar_span_is(name, "body"))
    {
        ar__hint_color(buf, &used, "background", ar__attr_of(d, node, "bgcolor"));
    }
    if (ar_span_is(name, "body"))
    {
        ar__hint_color(buf, &used, "color", ar__attr_of(d, node, "text"));
    }
    if (ar_span_is(name, "font"))
    {
        ar__hint_color(buf, &used, "color", ar__attr_of(d, node, "color"));
    }
    if (sized)
    {
        ar__hint_len(buf, &used, "width", ar__attr_of(d, node, "width"));
        ar__hint_len(buf, &used, "height", ar__attr_of(d, node, "height"));
    }
    if (table)
    {
        /* `border=1` is a one-pixel border on the table, and `border=0` is the
           way a page that used tables for layout said so. */
        ar__hint_len(buf, &used, "border-width", ar__attr_of(d, node, "border"));
        ar__hint_len(buf, &used, "border-spacing", ar__attr_of(d, node, "cellspacing"));
    }
    if (cell)
    {
        /*
         * `cellpadding` and `border` are written on the *table* and land on
         * its cells, which is the one hint that is not about the element
         * carrying it. `<table cellpadding=8>` is how every table on the old
         * web set its padding, and reading the attribute off the cell -- where
         * it never appears -- would have made this whole mapping look like it
         * worked while doing nothing.
         *
         * The table is found by walking up rather than by remembering it,
         * because a cell is three or four links below its table and the walk
         * happens once per cell.
         */
        ar_i32 up = d->nodes[node].parent;

        while (up >= 0 && !ar_span_is(d->nodes[up].name, "table"))
        {
            up = d->nodes[up].parent;
        }
        if (up >= 0)
        {
            ar_i32  px = 0;
            int     pct = 0;
            ar_span b = ar__attr_of(d, up, "border");

            ar__hint_len(buf, &used, "padding", ar__attr_of(d, up, "cellpadding"));
            /* And a table with a border gives its cells one pixel, whatever
               number it asked for itself -- which is what `border=5` looks
               like in a browser and why it does not look like five. */
            if (b.n > 0 && ar__legacy_len(b, &px, &pct) > 0 && px > 0)
            {
                ar__put_str(buf, &used, AR_DOM_HINTS, "border-width:1px;");
            }
        }
    }
    if (ar_span_is(name, "img"))
    {
        ar__hint_len(buf, &used, "border-width", ar__attr_of(d, node, "border"));
        ar__hint_len(buf, &used, "margin-left", ar__attr_of(d, node, "hspace"));
        ar__hint_len(buf, &used, "margin-top", ar__attr_of(d, node, "vspace"));
    }
    ar__hint_align(buf, &used, ar__attr_of(d, node, "align"));

    buf[used] = 0;
    return used > 0 ? buf : 0;
}

static const char *ar__inline_style(const ar_doc *d, ar_i32 node, char *buf)
{
    ar_span style = ar__attr_of(d, node, "style");
    ar_u32  used = 0;

    if (style.n == 0)
    {
        return 0;
    }
    ar__put_span(buf, &used, AR_DOM_STYLE, style);
    if (used < style.n)
    {
        while (used > 0 && buf[used - 1] != ';')
        {
            --used;
        }
    }
    buf[used] = 0;
    return used > 0 ? buf : 0;
}

static void ar__walk(ar_ctx *c, ar_doc *d, ar_i32 node, int pre)
{
    char   sel[AR_DOM_SEL];
    char   style[AR_DOM_STYLE];
    char   hints[AR_DOM_HINTS];
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
            if (!pre)
            {
                ar__collapse(d, &d->nodes[node].text);
            }
            ar_text(c, "span", d->nodes[node].text.p);
        }
        return;
    }
    if (d->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return; /* comments and the doctype generate no box */
    }

    if (ar__preformatted(d->nodes[node].name))
    {
        pre = 1;
    }

    ar__selector(d, node, sel);
    {
        /* Both lists are built before the box is opened, because ar_begin
           copies them and neither buffer survives this frame's recursion. */
        const char *h = ar__hints(d, node, hints);

        ar_begin_hinted(c, sel, h, ar__inline_style(d, node, style));
    }
    for (child = d->nodes[node].first_child; child >= 0; child = d->nodes[child].next_sibling)
    {
        ar__walk(c, d, child, pre);
    }
    ar_end(c);
}

void ar_dom_build(ar_ctx *c, ar_doc *d)
{
    if (!c || !d)
    {
        return;
    }
    ar__walk(c, d, ar_dom_root(d), 0);
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

/* ------------------------------------------------------------------------
 * A document in the context's own arena
 *
 * The release document specifies `ar_html_parse(ar_ctx *, ...)`, and the
 * reason it took caller storage until now is a real question rather than an
 * oversight: a document is per-*parse*, not per-frame, so it belongs in the
 * persistent half of the arena -- and every byte of the persistent half beyond
 * AR_MEM_FIXED was promised to the box budget by AR_MEM.
 *
 * Taking it anyway would mean a caller that asked for two thousand boxes
 * quietly getting fewer, which is exactly the class of failure this library
 * refuses everywhere else.
 *
 * So the caller asks: AR_MEM_DOC(boxes, bytes) sizes the block and `budget`
 * says how much of it the document may have. Nothing is silent and nothing is
 * borrowed.
 * ------------------------------------------------------------------------ */

/*
 * How the budget is divided.
 *
 * Nodes dominate a document -- an element is 72 bytes and its text is usually
 * shorter than its markup -- and attributes are the smallest share. These are
 * measured against ordinary pages rather than derived: a document that is all
 * text and no structure wastes the node share, and one that is all structure
 * runs out of text first. Both fail cleanly and say which.
 */
#define AR_DOC_NODE_SHARE 45u /* per cent */
#define AR_DOC_ATTR_SHARE 15u
#define AR_DOC_TEXT_SHARE 30u
/* and the remaining 10% is the tokenizer's scratch */

ar_doc *ar_html_parse_into(ar_ctx *c, const char *bytes, ar_u32 len)
{
    ar_doc     *d;
    ar_u32      node_bytes, attr_bytes, text_bytes, scratch_bytes;
    ar_u32      skip = 0;
    ar_encoding enc;
    char       *scratch;
    ar_u32      budget;

    if (!c || !bytes)
    {
        return 0;
    }
    budget = c->doc_budget;
    if (budget < 4096u)
    {
        return 0;
    }

    d = (ar_doc *)ar_arena_persist(&c->arena, (ar_u32)sizeof(ar_doc));
    if (!d)
    {
        return 0;
    }
    memset(d, 0, sizeof *d);

    /*
     * The encoding, before anything else reads a byte.
     *
     * A byte order mark is not content and is skipped; anything that is not
     * already UTF-8 is decoded into arena space that outlives the document,
     * because tag names and attribute values stay spans of it.
     */
    enc = ar_encoding_sniff(bytes, len, &skip);
    bytes += skip;
    len -= skip;

    if (enc != AR_ENC_UTF8)
    {
        /* Worst case is two bytes out per byte in, which windows-1252 hits on
           every character above 0x7F. */
        ar_u32 need = len * 2u + 4u;
        char  *decoded;

        if (need >= budget)
        {
            d->overflowed = 1;
            return d;
        }
        decoded = (char *)ar_arena_persist(&c->arena, need);
        if (!decoded)
        {
            d->overflowed = 1;
            return d;
        }
        len = ar_encoding_decode(enc, bytes, len, decoded, need);
        bytes = decoded;
        budget -= need;
    }

    node_bytes = budget * AR_DOC_NODE_SHARE / 100u;
    attr_bytes = budget * AR_DOC_ATTR_SHARE / 100u;
    text_bytes = budget * AR_DOC_TEXT_SHARE / 100u;
    scratch_bytes = budget - node_bytes - attr_bytes - text_bytes;

    d->nodes = (ar_dom_node *)ar_arena_persist(&c->arena, node_bytes);
    d->attrs = (ar_attr *)ar_arena_persist(&c->arena, attr_bytes);
    d->text = (char *)ar_arena_persist(&c->arena, text_bytes);
    scratch = (char *)ar_arena_persist(&c->arena, scratch_bytes);

    if (!d->nodes || !d->attrs || !d->text || !scratch)
    {
        d->overflowed = 1;
        return d;
    }
    d->node_cap = (ar_i32)(node_bytes / sizeof(ar_dom_node));
    d->attr_cap = (ar_i32)(attr_bytes / sizeof(ar_attr));
    d->text_cap = text_bytes;

    ar_html_parse(d, bytes, len, scratch, scratch_bytes);
    return d;
}
