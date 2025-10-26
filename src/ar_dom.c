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
 * A class or id name as the selector string can carry it.
 *
 * HTML allows almost anything in either, and the selector syntax stops a name
 * at the first character that is not an identifier's. The HTML standard's own
 * ids are `syntax:the-xhtml-syntax`, the RFC's `section-1.1`, a Tailwind class
 * `md:flex`: each ended its name early -- `#syntax`, colliding with the real
 * `#syntax` on the page -- and the `:` ended the compound outright, so every
 * class after it was lost, `.ar-link` among them, and a link drew as plain
 * text. Anything else becomes `_`, which keeps the name whole and distinct; a
 * stylesheet naming it needs CSS escapes, which the parser does not read yet.
 */
static void ar__put_name(char *buf, ar_u32 *used, ar_u32 cap, const char *p, ar_u32 n)
{
    ar_u32 i;

    for (i = 0; i < n; ++i)
    {
        char ch = p[i];

        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || (unsigned char)ch >= 0x80))
        {
            ch = '_';
        }
        ar__put(buf, used, cap, ch);
    }
}

/*
 * `tag.a.b#id`, in the order ar_begin's parser expects.
 *
 * The class attribute is a space-separated list and the selector syntax spells
 * each one with a dot, so the spaces become dots.
 */
/* Append a literal to a selector already built, if it fits. Silent when it
   does not: a selector too long to hold its own class is a document with a
   pathological class list, and dropping the tail styles the box plainly
   rather than failing to build it. */
static void ar__put_lit(char *buf, const char *lit)
{
    ar_u32 used = 0;
    ar_u32 i = 0;

    while (buf[used])
    {
        ++used;
    }
    while (lit[i] && used + 1 < AR_DOM_SEL)
    {
        buf[used++] = lit[i++];
    }
    buf[used] = 0;
}

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
            ar__put_name(buf, &used, AR_DOM_SEL, &c, 1);
        }
    }
    if (id.n > 0)
    {
        ar__put(buf, &used, AR_DOM_SEL, '#');
        ar__put_name(buf, &used, AR_DOM_SEL, id.p, id.n);
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
 * `colspan="2"` -> `colspan:2`, with no unit.
 *
 * A span is a count and not a length, and `colspan:2px` is not a thing anyone
 * can write in CSS. It happens to *work* -- the parser stores the number and
 * the layout reads it without asking what unit came with it -- which is why
 * this is a matter of the declaration saying what it means rather than a bug
 * with a symptom. A declaration that is only accidentally correct is one that
 * stops being correct the day the unit starts mattering.
 *
 * Zero and one are written like any other number and mean what they say. A
 * value that is not a number at all writes nothing, for the same reason a
 * length that is not a length does.
 */
static void ar__hint_count(char *buf, ar_u32 *used, const char *prop, ar_span v)
{
    ar_i32 n = 0;
    int    pct = 0;

    if (v.n == 0 || ar__legacy_len(v, &n, &pct) == 0 || pct)
    {
        return;
    }
    ar__put_str(buf, used, AR_DOM_HINTS, prop);
    ar__put(buf, used, AR_DOM_HINTS, ':');
    ar__put_num(buf, used, AR_DOM_HINTS, n);
    ar__put(buf, used, AR_DOM_HINTS, ';');
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
    /* `justify` is left out on purpose. This engine has three text-align
       values and that is not one of them, so writing it would be a
       declaration the style parser refuses -- counted in the sheet's error
       tally, which is what says whether a page's CSS is broken. The
       attribute maps to nothing until the value exists. */
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
static ar_i32 ar__attr_num(const ar_doc *d, ar_i32 node, const char *name, ar_i32 dflt);

static const char *ar__hints(const ar_doc *d, ar_i32 node, char *buf)
{
    ar_span name = d->nodes[node].name;
    ar_u32  used = 0;
    int     table = ar_span_is(name, "table");
    int     cell = ar_span_is(name, "td") || ar_span_is(name, "th");
    int row = ar_span_is(name, "tr") || ar_span_is(name, "thead") || ar_span_is(name, "tbody") ||
              ar_span_is(name, "tfoot");
    /* `svg` belongs on this list and was missing from it: a drawing states its
       size in attributes exactly as an image does, and with no rule and no
       hint it came out the full width of the page and none of its height. */
    int sized = table || cell || ar_span_is(name, "img") || ar_span_is(name, "col") ||
                ar_span_is(name, "hr") || ar_span_is(name, "canvas") || ar_span_is(name, "video") ||
                ar_span_is(name, "iframe") || ar_span_is(name, "embed") ||
                ar_span_is(name, "object") || ar_span_is(name, "svg");

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

        /*
         * `colspan` and `rowspan` first, which are not presentational at all.
         *
         * They are structure: they say which cells of the grid this one
         * occupies, and no stylesheet has ever been able to say it. They are
         * mapped here because this is where an attribute becomes a declaration
         * and areole carries both as properties -- and because they were
         * mapped nowhere at all, so a `<table>` written in HTML had every span
         * silently ignored. A two-row `rowspan` laid out as one cell of one
         * row, and the row below it started in the column the span was
         * holding.
         *
         * Being in the hint band means a stylesheet could overrule them, which
         * no stylesheet will: there is no CSS spelling of `colspan` for an
         * author to have written one in.
         */
        ar__hint_count(buf, &used, "colspan", ar__attr_of(d, node, "colspan"));
        ar__hint_count(buf, &used, "rowspan", ar__attr_of(d, node, "rowspan"));

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
    /*
     * The attributes that size a control, by the rule a browser follows --
     * measured, not derived: a text field's content is 29 pixels plus seven a
     * character of `size`; a number field with both limits is as wide as the
     * longer limit, at 44 plus seven a character; a textarea is 7.33 pixels a
     * column plus fifteen for a scrollbar, and fifteen a row. Hints, so an
     * author's width still wins.
     */
    if (ar_span_is(name, "input"))
    {
        ar_span type = ar__attr_of(d, node, "type");
        ar_span lo = ar__attr_of(d, node, "min");
        ar_span hi = ar__attr_of(d, node, "max");
        ar_span size = ar__attr_of(d, node, "size");

        if (type.p && ar_span_is(type, "number") && lo.p && hi.p)
        {
            ar_span step = ar__attr_of(d, node, "step");
            ar_u32  n = lo.n > hi.n ? lo.n : hi.n;
            ar_u32  k;

            for (k = 0; step.p && k < step.n; ++k)
            {
                if (step.p[k] == '.')
                {
                    n += step.n - k; /* the point and the digits after it */
                    break;
                }
            }
            ar__put_str(buf, &used, AR_DOM_HINTS, "width:");
            ar__put_num(buf, &used, AR_DOM_HINTS, 44 + 7 * (ar_i32)n);
            ar__put_str(buf, &used, AR_DOM_HINTS, "px;");
        }
        else if (size.p && (!type.p || ar_span_is(type, "text") || ar_span_is(type, "password") ||
                            ar_span_is(type, "search") || ar_span_is(type, "email") ||
                            ar_span_is(type, "url") || ar_span_is(type, "tel")))
        {
            ar_i32 sz = ar__attr_num(d, node, "size", 20000) / 1000;

            if (sz > 0 && sz < 1000)
            {
                ar__put_str(buf, &used, AR_DOM_HINTS, "width:");
                ar__put_num(buf, &used, AR_DOM_HINTS, 29 + 7 * sz);
                ar__put_str(buf, &used, AR_DOM_HINTS, "px;");
            }
        }
    }
    if (ar_span_is(name, "textarea"))
    {
        ar_i32 cols = ar__attr_num(d, node, "cols", 0) / 1000;
        ar_i32 rows = ar__attr_num(d, node, "rows", 0) / 1000;

        if (cols > 0 && cols < 1000)
        {
            ar__put_str(buf, &used, AR_DOM_HINTS, "width:");
            ar__put_num(buf, &used, AR_DOM_HINTS, (22 * cols + 45) / 3);
            ar__put_str(buf, &used, AR_DOM_HINTS, "px;");
        }
        if (rows > 0 && rows < 1000)
        {
            ar__put_str(buf, &used, AR_DOM_HINTS, "height:");
            ar__put_num(buf, &used, AR_DOM_HINTS, 15 * rows);
            ar__put_str(buf, &used, AR_DOM_HINTS, "px;");
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

/*
 * Whether this element is a tab stop.
 *
 * Two ways in, and the specification gives them in this order. `tabindex`
 * decides outright when it is present -- a negative value means focusable by
 * script and by click but never by Tab, which is the whole reason the
 * attribute takes a number rather than a boolean. Otherwise the element type
 * decides, and the list is short because it is the list of things that do
 * something when you press them.
 *
 * `<a>` without `href` is not a link and not a tab stop, which is an old rule
 * that still catches people: an anchor used as a scroll target is markup, not
 * a control, and putting it in the tab order makes a page unusable by keyboard
 * long before it makes it accessible.
 *
 * Order within the document, not within `tabindex`. A positive tabindex is
 * supposed to sort ahead of everything else and it does not here yet -- said
 * plainly rather than left to be discovered, because a page that relies on it
 * will Tab in the wrong order rather than not at all.
 */
static int ar__focusable_element(const ar_doc *d, ar_i32 node)
{
    ar_span ti = ar__attr_of(d, node, "tabindex");
    ar_span name = d->nodes[node].name;

    /*
     * A disabled control is not a tab stop, and that comes before `tabindex`.
     *
     * Not an ordering detail: `tabindex="0"` on a disabled field is real
     * markup -- somebody set the index and disabled it later -- and honouring
     * the index there puts a control in the tab order that cannot be operated
     * when it is reached. Every browser drops it, and this example advertised
     * the rule in its own label before the engine implemented it, which is how
     * it got noticed.
     */
    if (ar__attr_of(d, node, "disabled").p)
    {
        return 0;
    }

    if (ti.p && ti.n > 0)
    {
        /* A leading `-` is the only part that matters: -1 and -37 mean the
           same thing, and anything else is a stop. */
        return ti.p[0] != '-';
    }

    if (ar_span_is(name, "a") || ar_span_is(name, "area"))
    {
        ar_span href = ar__attr_of(d, node, "href");

        return href.p != 0;
    }
    if (ar_span_is(name, "button") || ar_span_is(name, "select") || ar_span_is(name, "textarea") ||
        ar_span_is(name, "summary"))
    {
        return 1;
    }
    if (ar_span_is(name, "input"))
    {
        /* Every input but the hidden one, which has no box at all and would be
           a tab stop nobody can see. */
        ar_span type = ar__attr_of(d, node, "type");

        return !(type.p && ar_span_is(type, "hidden"));
    }
    return 0;
}

/*
 * The states a control is born with, read from the markup.
 *
 * `checked` and `disabled` are boolean attributes: present means true whatever
 * the value says, so `disabled="false"` disables. That is HTML and it surprises
 * people, and writing the check any other way is how an engine ends up
 * disagreeing with every browser about one line of somebody's markup.
 *
 * `:enabled` is not merely the absence of `:disabled`. Neither matches an
 * element that cannot be disabled at all -- a paragraph is not an enabled
 * paragraph -- so both bits come from the same short list of elements and the
 * list is the point.
 */
static ar_u32 ar__markup_state(const ar_doc *d, ar_i32 node)
{
    ar_span name = d->nodes[node].name;
    ar_u32  st = 0;
    int     formish;

    formish = ar_span_is(name, "input") || ar_span_is(name, "button") ||
              ar_span_is(name, "select") || ar_span_is(name, "textarea") ||
              ar_span_is(name, "option") || ar_span_is(name, "optgroup") ||
              ar_span_is(name, "fieldset");

    if (formish)
    {
        st |= ar__attr_of(d, node, "disabled").p ? AR_STATE_DISABLED : AR_STATE_ENABLED;
    }

    /*
     * `checked` in the markup is the *default* checkedness, not the state. A
     * user who clicks the box changes the state and not the attribute, which
     * is why `:checked` is a pseudo-class and `[checked]` is a different
     * question -- and the difference is a mistake people make once.
     *
     * Nothing here can change it yet, so for now the two agree. When
     * activation lands, this becomes the initial value and the slot carries
     * what happened since.
     */
    if (ar_span_is(name, "input") && ar__attr_of(d, node, "checked").p)
    {
        st |= AR_STATE_CHECKED;
    }
    if (ar_span_is(name, "option") && ar__attr_of(d, node, "selected").p)
    {
        st |= AR_STATE_CHECKED;
    }
    if (ar_span_is(name, "details") && ar__attr_of(d, node, "open").p)
    {
        st |= AR_STATE_OPEN;
    }
    return st;
}

/*
 * Which controls activate, and how.
 *
 * The list is short because it is the list of things that do something when
 * you press them and need no caret to do it. Text fields are absent on
 * purpose: they take a keystroke rather than an activation, and that is the
 * editing subsystem rather than this one.
 */
/*
 * A number attribute, in thousandths, defaulting when it is absent or junk.
 *
 * Thousandths because `value="0.7"` is the common way to write a progress bar
 * and an integer parse would make it zero -- which is a full bar reading empty
 * rather than an error anybody notices.
 */
static ar_i32 ar__attr_num(const ar_doc *d, ar_i32 node, const char *name, ar_i32 dflt)
{
    ar_span a = ar__attr_of(d, node, name);
    ar_i32  whole = 0, frac = 0, digits = 0, sign = 1;
    ar_u32  i = 0;
    int     any = 0;

    if (!a.p || a.n == 0)
    {
        return dflt;
    }
    while (i < a.n && (a.p[i] == ' ' || a.p[i] == '\t'))
    {
        ++i;
    }
    if (i < a.n && (a.p[i] == '-' || a.p[i] == '+'))
    {
        sign = a.p[i] == '-' ? -1 : 1;
        ++i;
    }
    while (i < a.n && a.p[i] >= '0' && a.p[i] <= '9')
    {
        if (whole < 100000)
        {
            whole = whole * 10 + (a.p[i] - '0');
        }
        ++i;
        any = 1;
    }
    if (i < a.n && a.p[i] == '.')
    {
        ++i;
        while (i < a.n && a.p[i] >= '0' && a.p[i] <= '9')
        {
            if (digits < 3)
            {
                frac = frac * 10 + (a.p[i] - '0');
                ++digits;
            }
            ++i;
            any = 1;
        }
    }
    while (digits < 3)
    {
        frac *= 10;
        ++digits;
    }
    return any ? sign * (whole * 1000 + frac) : dflt;
}

/*
 * How full a `<progress>` or `<meter>` is, as a percentage.
 *
 * `<progress>` with no `value` is *indeterminate* and not empty -- a bar that
 * is waiting rather than one at zero. There is no animation here to say so, so
 * it reads as empty and is written down rather than pretended about.
 *
 * `<meter>` has `min` as well, which `<progress>` does not: a meter measures a
 * range and a progress bar counts from nothing.
 */
/*
 * Whether this `<option>` is the one its `<select>` is showing.
 *
 * `selected` decides when any sibling carries it; otherwise the first option
 * does, which is what a browser shows and what almost every document is. Both
 * halves are needed: reading only `selected` leaves a plain `<select>` blank,
 * and reading only "first" ignores the attribute wherever it appears.
 */
static int ar__chosen_option(const ar_doc *d, ar_i32 node)
{
    ar_i32 up = d->nodes[node].parent;
    ar_i32 sib;
    ar_i32 first = -1;

    if (ar__attr_of(d, node, "selected").p)
    {
        return 1;
    }
    if (up < 0)
    {
        return 0;
    }
    for (sib = d->nodes[up].first_child; sib >= 0; sib = d->nodes[sib].next_sibling)
    {
        if (d->nodes[sib].kind != AR_DOM_ELEMENT || !ar_span_is(d->nodes[sib].name, "option"))
        {
            continue;
        }
        if (first < 0)
        {
            first = sib;
        }
        if (ar__attr_of(d, sib, "selected").p)
        {
            return 0; /* somebody else is chosen, and it is not this one */
        }
    }
    return first == node;
}

/*
 * The marker on a list item, as a box.
 *
 * A marker is a box here rather than something the paint pass draws, for the
 * same reason a checkbox's tick is: everything a marker needs -- a colour that
 * inherits, a size in `em`, text measurement for a number, a rounded corner
 * for a bullet -- is already true of boxes and would have to be written again
 * inside the painter. `::marker` and counters are the general machinery and
 * are still 0.5.3; this is the two cases every document actually contains.
 *
 * It is placed by a negative left margin, which is exactly what
 * `list-style-position: outside` means: the marker starts in the padding the
 * list already reserves, the first line's text starts at the content edge, and
 * a line that wraps starts there too -- without the marker taking part in the
 * wrap. No positioning scheme and no new layout path.
 *
 * A bullet is drawn and a number is typed, and that split is forced: the
 * built-in face is ASCII 32 to 126 and renders everything else as `?`, so
 * U+2022 would be a question mark on any build without a TrueType face. A
 * disc is a box with a radius, which is now a thing this engine can draw.
 */
static void ar__list_marker(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 up, depth = 0, ordered = 0;
    ar_i32 index;

    if (!ar_span_is(d->nodes[node].name, "li"))
    {
        return;
    }

    /* Which list this belongs to, and how deep it is nested. A bare `<li>`
       with no list around it is still a list item with a disc, which is what
       a browser does and what the tag means on its own. */
    for (up = d->nodes[node].parent; up >= 0; up = d->nodes[up].parent)
    {
        if (d->nodes[up].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        if (ar_span_is(d->nodes[up].name, "ol"))
        {
            if (depth == 0)
            {
                ordered = 1;
            }
            ++depth;
        }
        else if (ar_span_is(d->nodes[up].name, "ul") || ar_span_is(d->nodes[up].name, "menu") ||
                 ar_span_is(d->nodes[up].name, "dir"))
        {
            ++depth;
        }
    }

    if (!ordered)
    {
        /*
         * disc, then circle, then square, by nesting depth -- the
         * specification's own sequence, and the thing that makes a nested list
         * readable without indentation alone having to carry it.
         */
        const char *sel = "ar-bullet";

        if (depth == 2)
        {
            sel = "ar-bullet.ar-circle";
        }
        else if (depth >= 3)
        {
            sel = "ar-bullet.ar-square";
        }
        ar_begin(c, sel);
        ar_end(c);
        return;
    }

    /*
     * The number, which is counted rather than read: `start` on the list and
     * `value` on the item are the two ways a document overrides it, and both
     * are common enough in real markup to be worth the twenty lines -- an
     * ordered list that restarts at 1 halfway down is a wrong document, not a
     * styling difference.
     */
    {
        ar_i32  own = ar__attr_num(d, node, "value", 0);
        ar_span v = ar__attr_of(d, node, "value");

        if (v.p && v.n > 0)
        {
            index = own / 1000; /* ar__attr_num is fixed point, thousandths */
        }
        else
        {
            ar_i32  sib;
            ar_i32  start = 1;
            ar_span st;

            up = d->nodes[node].parent;
            st = up >= 0 ? ar__attr_of(d, up, "start") : ar__attr_of(d, node, "nosuch");
            if (st.p && st.n > 0)
            {
                start = ar__attr_num(d, up, "start", 1000) / 1000;
            }
            index = start;
            for (sib = up >= 0 ? d->nodes[up].first_child : -1; sib >= 0 && sib != node;
                 sib = d->nodes[sib].next_sibling)
            {
                if (d->nodes[sib].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[sib].name, "li"))
                {
                    ++index;
                }
            }
        }
    }

    {
        char   buf[16];
        ar_u32 used = 0;
        ar_i32 t = index < 0 ? -index : index;
        char   digits[12];
        ar_i32 nd = 0;

        if (t == 0)
        {
            digits[nd++] = '0';
        }
        while (t > 0 && nd < 11)
        {
            digits[nd++] = (char)('0' + (t % 10));
            t /= 10;
        }
        if (index < 0)
        {
            buf[used++] = '-';
        }
        while (nd > 0)
        {
            buf[used++] = digits[--nd];
        }
        buf[used++] = '.';
        buf[used] = 0;

        /* The marker carries the text itself rather than wrapping a child
           that does. A box with its own text has its own baseline, so it
           sits on the line the item sits on; a box whose text is a child
           is an inline-block with no baseline of its own and takes its
           bottom margin edge instead, which lifted every number about four
           pixels above the word beside it. */
        ar_text_kept(c, "ar-marker", buf, used);
    }
}

static ar_i32 ar__gauge_pct(const ar_doc *d, ar_i32 node, int is_meter)
{
    ar_i32 lo = is_meter ? ar__attr_num(d, node, "min", 0) : 0;
    ar_i32 hi = ar__attr_num(d, node, "max", 1000);
    ar_i32 v = ar__attr_num(d, node, "value", lo);
    ar_i32 span;

    if (hi <= lo)
    {
        return 0;
    }
    if (v < lo)
    {
        v = lo;
    }
    if (v > hi)
    {
        v = hi;
    }
    span = hi - lo;
    return ((v - lo) * 100 + span / 2) / span;
}

/*
 * What sort of control an element is, by its markup alone.
 *
 * A `<button>` submits unless it says otherwise, which surprises people and is
 * the HTML rule: `type` defaults to `submit`, so a button inside a form with
 * no type submits it. `<input>` is fourteen elements wearing one tag, and the
 * type decides which.
 */
static ar_u8 ar__control_kind(const ar_doc *d, ar_i32 node)
{
    ar_span name = d->nodes[node].name;

    if (ar_span_is(name, "button"))
    {
        ar_span type = ar__attr_of(d, node, "type");

        if (type.p && ar_span_is(type, "reset"))
        {
            return AR_CTL_RESET;
        }
        if (type.p && ar_span_is(type, "button"))
        {
            return AR_CTL_BUTTON;
        }
        return AR_CTL_SUBMIT;
    }
    if (ar_span_is(name, "summary"))
    {
        return AR_CTL_SUMMARY;
    }
    if (ar_span_is(name, "details"))
    {
        return AR_CTL_DETAILS;
    }
    if (ar_span_is(name, "select"))
    {
        return AR_CTL_SELECT;
    }
    if (ar_span_is(name, "textarea"))
    {
        return AR_CTL_TEXT;
    }
    if (ar_span_is(name, "label"))
    {
        return AR_CTL_LABEL;
    }
    if (ar_span_is(name, "input"))
    {
        ar_span type = ar__attr_of(d, node, "type");

        if (!type.p)
        {
            return AR_CTL_TEXT;
        }
        if (ar_span_is(type, "checkbox"))
        {
            return AR_CTL_CHECKBOX;
        }
        if (ar_span_is(type, "radio"))
        {
            return AR_CTL_RADIO;
        }
        if (ar_span_is(type, "submit") || ar_span_is(type, "image"))
        {
            return AR_CTL_SUBMIT;
        }
        if (ar_span_is(type, "reset"))
        {
            return AR_CTL_RESET;
        }
        if (ar_span_is(type, "button"))
        {
            return AR_CTL_BUTTON;
        }
        if (ar_span_is(type, "range"))
        {
            return AR_CTL_RANGE;
        }
        if (ar_span_is(type, "color"))
        {
            return AR_CTL_COLOR;
        }
        if (ar_span_is(type, "file"))
        {
            return AR_CTL_FILE;
        }
        if (ar_span_is(type, "hidden"))
        {
            return AR_CTL_NONE;
        }
        /* Everything else an `<input>` can be is a text field: text, password,
           search, email, url, tel, number. They differ in what they accept and
           in how they draw, which is what ar__field_flags says. */
        return AR_CTL_TEXT;
    }
    return AR_CTL_NONE;
}

static int ar__is_input_type(const ar_doc *d, ar_i32 node, const char *type)
{
    ar_span t = ar__attr_of(d, node, "type");

    return ar_span_is(d->nodes[node].name, "input") && t.p && ar_span_is(t, type);
}

/*
 * Whether a control is disabled, which is more than its own attribute.
 *
 * A disabled `<fieldset>` disables everything inside it -- except what is in
 * its first `<legend>`, which is where a checkbox that switches the group on
 * and off lives. An option is disabled by a disabled `<optgroup>` around it.
 */
static int ar__is_disabled(const ar_doc *d, ar_i32 node)
{
    ar_i32 up, below = node;

    if (ar__attr_of(d, node, "disabled").p)
    {
        return 1;
    }
    for (up = d->nodes[node].parent; up >= 0; below = up, up = d->nodes[up].parent)
    {
        if (d->nodes[up].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        if (ar_span_is(d->nodes[up].name, "optgroup") && ar__attr_of(d, up, "disabled").p)
        {
            return 1;
        }
        if (ar_span_is(d->nodes[up].name, "fieldset") && ar__attr_of(d, up, "disabled").p)
        {
            ar_i32 ch;

            /* `below` is the fieldset's child on the way down: if it is the
               first legend, this control is exempt. */
            for (ch = d->nodes[up].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
            {
                if (d->nodes[ch].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[ch].name, "legend"))
                {
                    break;
                }
            }
            if (ch != below)
            {
                return 1;
            }
        }
    }
    return 0;
}

static ar_i32 ar__by_id(const ar_doc *d, ar_span id)
{
    ar_i32 i;

    if (!id.p || id.n == 0)
    {
        return -1;
    }
    for (i = 0; i < d->node_count; ++i)
    {
        ar_span v;

        if (d->nodes[i].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        v = ar__attr_of(d, i, "id");
        if (v.p && v.n == id.n && memcmp(v.p, id.p, id.n) == 0)
        {
            return i;
        }
    }
    return -1;
}

/* What a label can point at, from the specification's list of labelable
   elements. A hidden input is not one: there is nothing to click. */
static int ar__labelable(const ar_doc *d, ar_i32 node)
{
    ar_span name;

    if (node < 0 || d->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return 0;
    }
    name = d->nodes[node].name;
    if (ar_span_is(name, "input"))
    {
        return !ar__is_input_type(d, node, "hidden");
    }
    return ar_span_is(name, "button") || ar_span_is(name, "select") ||
           ar_span_is(name, "textarea") || ar_span_is(name, "meter") ||
           ar_span_is(name, "progress") || ar_span_is(name, "output");
}

static ar_i32 ar__first_labelable(const ar_doc *d, ar_i32 node)
{
    ar_i32 ch;

    for (ch = d->nodes[node].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        ar_i32 hit;

        if (ar__labelable(d, ch))
        {
            return ch;
        }
        if (d->nodes[ch].kind == AR_DOM_ELEMENT && (hit = ar__first_labelable(d, ch)) >= 0)
        {
            return hit;
        }
    }
    return -1;
}

/* The control a `<label>` labels: its `for`, or the first labelable thing
   inside it. The two spellings are equally common and only supporting the
   explicit one leaves most of the web's checkboxes unlabelled. */
static ar_i32 ar__label_target(const ar_doc *d, ar_i32 node)
{
    ar_span f = ar__attr_of(d, node, "for");

    if (f.p)
    {
        ar_i32 t = ar__by_id(d, f);

        return ar__labelable(d, t) ? t : -1;
    }
    return ar__first_labelable(d, node);
}

/* The first text child, which is a textarea's value and an option's label --
   one node, because the tree builder appends adjacent text to the node
   already there. */
static ar_span ar__first_text(const ar_doc *d, ar_i32 node)
{
    ar_span none;
    ar_i32  ch;

    none.p = 0;
    none.n = 0;
    for (ch = d->nodes[node].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        if (d->nodes[ch].kind == AR_DOM_TEXT)
        {
            return d->nodes[ch].text;
        }
    }
    return none;
}

/* What a field is and what its limits are, from its markup. */
static ar_u32 ar__field_flags(const ar_doc *d, ar_i32 node, int disabled)
{
    ar_u32 f = 0;

    if (ar_span_is(d->nodes[node].name, "textarea"))
    {
        f |= AR_FIELD_MULTI;
    }
    if (ar__is_input_type(d, node, "password"))
    {
        f |= AR_FIELD_PASSWORD;
    }
    if (ar__is_input_type(d, node, "number"))
    {
        f |= AR_FIELD_NUMBER;
    }
    if (ar__attr_of(d, node, "readonly").p)
    {
        f |= AR_FIELD_READONLY;
    }
    if (disabled)
    {
        f |= AR_FIELD_DISABLED;
    }
    return f;
}

/* A slider's limits and starting value, in thousandths. `min` 0, `max` 100,
   `step` 1 and a value halfway when none is given -- the specification's
   defaults, and why an empty `<input type=range>` sits in the middle. */
static void ar__range_limits(const ar_doc *d, ar_i32 node, ar_i32 *lo, ar_i32 *hi, ar_i32 *step,
                             ar_i32 *value)
{
    ar_i32 v;

    *lo = ar__attr_num(d, node, "min", 0);
    *hi = ar__attr_num(d, node, "max", 100000);
    *step = ar__attr_num(d, node, "step", 1000);
    if (*step <= 0)
    {
        *step = 1000;
    }
    if (*hi < *lo)
    {
        *hi = *lo;
    }
    v = ar__attr_num(d, node, "value", *lo + (*hi - *lo) / 2);
    if (v < *lo)
    {
        v = *lo;
    }
    if (v > *hi)
    {
        v = *hi;
    }
    v = *lo + (v - *lo + *step / 2) / *step * *step;
    *value = v > *hi ? *hi : v;
}

static int ar__hexval(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

/* A colour field's value: `#rrggbb` exactly, and black for anything else,
   which is the specification's sanitisation rather than a guess. */
static ar_i32 ar__color_value(const ar_doc *d, ar_i32 node)
{
    ar_span v = ar__attr_of(d, node, "value");
    ar_i32  rgb = 0, i;

    if (!v.p || v.n != 7 || v.p[0] != '#')
    {
        return 0;
    }
    for (i = 1; i < 7; ++i)
    {
        int h = ar__hexval(v.p[i]);

        if (h < 0)
        {
            return 0;
        }
        rgb = rgb * 16 + h;
    }
    return rgb;
}

static void ar__put_hex(char *buf, ar_u32 *used, ar_u32 cap, ar_i32 rgb)
{
    static const char HEX[] = "0123456789abcdef";
    ar_i32            shift;

    ar__put(buf, used, cap, '#');
    for (shift = 20; shift >= 0; shift -= 4)
    {
        ar__put(buf, used, cap, HEX[(rgb >> shift) & 15]);
    }
}

/* `part` as a whole per cent of `whole`, without the multiply overflowing for
   a range of millions -- which in thousandths is a range of billions. */
static ar_i32 ar__percent_of(ar_i32 part, ar_i32 whole)
{
    if (whole <= 0)
    {
        return 0;
    }
    if (whole > 20000000)
    {
        return part / (whole / 100);
    }
    return part * 100 / whole;
}

/* `prop:N%`, for a slider's fill and thumb -- written as an inline style so it
   lands in the cascade like any other width, which is how a gauge's bar
   already works. */
static void ar__style_pct(char *buf, const char *prop, ar_i32 pct)
{
    ar_u32 used = 0;

    ar__put_str(buf, &used, 32, prop);
    ar__put(buf, &used, 32, ':');
    ar__put_num(buf, &used, 32, pct);
    ar__put(buf, &used, 32, '%');
    buf[used] = 0;
}

/* How many options a select has and which the markup chooses: the *last* one
   marked `selected`, which is the specification's rule for a single select
   given several, and the first when none is. */
static void ar__select_info(const ar_doc *d, ar_i32 node, ar_i32 *count, ar_i32 *chosen)
{
    ar_i32 ch;

    for (ch = d->nodes[node].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        if (d->nodes[ch].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        if (ar_span_is(d->nodes[ch].name, "option"))
        {
            if (ar__attr_of(d, ch, "selected").p)
            {
                *chosen = *count;
            }
            ++*count;
        }
        else if (ar_span_is(d->nodes[ch].name, "optgroup"))
        {
            ar__select_info(d, ch, count, chosen);
        }
    }
}

static void ar__walk(ar_ctx *c, ar_doc *d, ar_i32 node, int pre);

/*
 * One option, as a row of an open list or as the closed select's face.
 *
 * Built here rather than by the general walk because whether it is the chosen
 * one is not in its markup any more: it is the select's value, which the user
 * may have changed. The class is the same `.ar-chosen` the closed face has
 * always used, so the sheet's rules carry over.
 */
static void ar__emit_option(ar_ctx *c, ar_doc *d, ar_i32 opt, int pre, int chosen, int row,
                            ar_u32 select_key, ar_i32 index)
{
    char   sel[AR_DOM_SEL];
    char   style[AR_DOM_STYLE];
    char   hints[AR_DOM_HINTS];
    ar_u32 st;
    ar_i32 ch;

    ar__selector(d, opt, sel);
    if (chosen)
    {
        ar__put_lit(sel, ".ar-chosen");
    }
    st = ar__markup_state(d, opt) & ~(ar_u32)AR_STATE_CHECKED;
    if (chosen)
    {
        st |= AR_STATE_CHECKED;
    }
    if (st)
    {
        ar_state_next(c, st);
    }
    if (row && !ar__is_disabled(d, opt))
    {
        ar_control_next(c, AR_CTL_OPTION, select_key, opt, index);
    }
    {
        const char *h = ar__hints(d, opt, hints);

        ar_begin_hinted(c, sel, h, ar__inline_style(d, opt, style));
    }
    if (row || d->nodes[opt].box < 0)
    {
        d->nodes[opt].box = ar_node_count(c) - 1;
    }
    for (ch = d->nodes[opt].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        ar__walk(c, d, ch, pre);
    }
    ar_end(c);
}

/* A select's options, in order, through any optgroups -- as its closed face
   (`row` zero: every option a box, the chosen one shown) or as the rows of
   its open list. */
static void ar__walk_select(ar_ctx *c, ar_doc *d, ar_i32 node, int pre, ar_i32 cur, ar_u32 key,
                            int row, ar_i32 *index)
{
    ar_i32 ch;

    for (ch = d->nodes[node].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        if (d->nodes[ch].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[ch].name, "option"))
        {
            ar__emit_option(c, d, ch, pre, *index == cur, row, key, *index);
            ++*index;
        }
        else if (d->nodes[ch].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[ch].name, "optgroup"))
        {
            char sel[AR_DOM_SEL];

            ar__selector(d, ch, sel);
            ar_begin(c, sel);
            if (!row)
            {
                d->nodes[ch].box = ar_node_count(c) - 1;
            }
            else
            {
                /* An open list labels each group, which is all an optgroup is
                   for -- its label is an attribute, not content. */
                ar_span lab = ar__attr_of(d, ch, "label");

                if (lab.p && lab.n > 0)
                {
                    ar_text_kept(c, "ar-group", lab.p, lab.n);
                }
            }
            ar__walk_select(c, d, ch, pre, cur, key, row, index);
            ar_end(c);
        }
        else if (!row)
        {
            ar__walk(c, d, ch, pre);
        }
    }
}

/* The sixteen colours a palette offers: the VGA sixteen, which every desktop
   has offered as its basic colours since there was a desktop. */
static const ar_i32 AR__PALETTE[16] = {0x000000, 0x808080, 0xC0C0C0, 0xFFFFFF, 0x800000, 0xFF0000,
                                       0x808000, 0xFFFF00, 0x008000, 0x00FF00, 0x008080, 0x00FFFF,
                                       0x000080, 0x0000FF, 0x800080, 0xFF00FF};

static void ar__walk(ar_ctx *c, ar_doc *d, ar_i32 node, int pre)
{
    char   sel[AR_DOM_SEL];
    char   style[AR_DOM_STYLE];
    char   hints[AR_DOM_HINTS];
    ar_i32 child;
    ar_u8  kind = AR_CTL_NONE;
    ar_i32 cval = 0;     /* the control's default value: a slider's, a colour's */
    ar_i32 ckey_val = 0; /* and its live one, read once the box exists */
    int    disabled = 0;

    /*
     * The element's own key and open state, taken the moment its box exists.
     *
     * ar_box_is_open and ar_box_key answer for the box most recently opened,
     * and every child built after that -- a select's face, a colour's swatch,
     * a file field's button -- is a more recent box. Asked afterwards they
     * answered for the child, so a select never opened its list and a colour
     * field never showed its palette, with every rule and every click right.
     */
    int    self_open = 0;
    ar_u32 self_key = 0;

    if (node < 0)
    {
        return;
    }

    if (d->nodes[node].kind == AR_DOM_TEXT)
    {
        /*
         * A whitespace-only text node is dropped between blocks and kept
         * between inlines.
         *
         * `<ul>\n  <li>a</li>\n</ul>` has newlines a browser drops on the
         * floor and every hand-written document is full of them. But
         * `<small>a</small> <strong>b</strong>` has a space that is the only
         * thing separating two words, and dropping it renders
         * "smallstrong" -- which is what this did until a plain HTML page was
         * rendered and looked at.
         */
        if ((!ar__ignorable(d->nodes[node].text) || ar_last_child_is_inline(c)) &&
            d->nodes[node].text.p)
        {
            /* The text is NUL-terminated in the document's own buffer, which
               is why ar_html_tree.c stores it there rather than leaving it a
               span of the input. */
            if (!pre)
            {
                ar__collapse(d, &d->nodes[node].text);
            }
            ar_text(c, "span", d->nodes[node].text.p);
            d->nodes[node].box = ar_node_count(c) - 1;
        }
        return;
    }
    if (d->nodes[node].kind != AR_DOM_ELEMENT)
    {
        return; /* comments and the doctype generate no box */
    }

    ar__selector(d, node, sel);
    {
        /* Both lists are built before the box is opened, because ar_begin
           copies them and neither buffer survives this frame's recursion. */
        const char *h = ar__hints(d, node, hints);

        ar_u32 st = ar__markup_state(d, node);

        kind = ar__control_kind(d, node);
        disabled = kind != AR_CTL_NONE && ar__is_disabled(d, node);

        /*
         * A class per input type, because there are no attribute selectors in
         * this engine and the user-agent sheet has to be able to tell a
         * checkbox from a text field.
         *
         * `input[type=checkbox]` is how every other engine writes this rule.
         * Adding attribute selectors to get there is 0.4.6's work and a good
         * deal more than a checkbox needs, so the hook is a synthetic class
         * with a reserved prefix. It is visible to author stylesheets, which
         * is the honest cost: `.ar-checkbox` will match, and is documented
         * rather than hidden, because a name nobody is told about is a name
         * somebody discovers.
         */
        if (kind == AR_CTL_CHECKBOX)
        {
            ar__put_lit(sel, ".ar-checkbox");
        }
        else if (kind == AR_CTL_RADIO)
        {
            ar__put_lit(sel, ".ar-radio");
        }
        else if (AR_CTL_IS_BUTTON(kind) && ar_span_is(d->nodes[node].name, "input"))
        {
            /* `<input type="submit">` is a button wearing an input's tag, and
               it has to be told apart from a text field for the same reason a
               checkbox does: a field has a default width of twenty characters
               and a button is as wide as its label. Without this, every
               submit button on every form came out eleven ems wide. */
            ar__put_lit(sel, ".ar-button");
        }
        else if (kind == AR_CTL_RANGE)
        {
            ar__put_lit(sel, ".ar-range");
        }
        else if (kind == AR_CTL_COLOR)
        {
            ar__put_lit(sel, ".ar-color");
        }
        else if (kind == AR_CTL_FILE)
        {
            ar__put_lit(sel, ".ar-file");
        }
        else if (ar__is_input_type(d, node, "hidden"))
        {
            /* A hidden input has no box in any browser. It drew as an empty
               eleven-em field here, because nothing said otherwise. */
            ar__put_lit(sel, ".ar-hidden");
        }

        /*
         * The `hidden` attribute, on anything: html.css's `[hidden] { display:
         * none }`, spelled as the same class. Put beside a browser, nasa.gov
         * drew its dropdown menus open -- the submenus and the megamenu are
         * five elements marked `hidden` -- because nothing read it. A user-
         * agent rule, so an author's `display` still wins, as it does in a
         * browser; `until-found` hides too, until find-in-page exists.
         */
        if (ar__attr_of(d, node, "hidden").p)
        {
            ar__put_lit(sel, ".ar-hidden");
        }

        /*
         * `:link`, spelled as a class for the same reason.
         *
         * An `<a>` is only a link when it has an `href` -- an anchor without
         * one is a name for somewhere on the page and has never been styled
         * like a link by anything. That distinction is an attribute selector
         * in every other engine and there are none here, so it is a synthetic
         * class like the input types above.
         */
        if (ar_span_is(d->nodes[node].name, "a") && ar__attr_of(d, node, "href").p)
        {
            ar__put_lit(sel, ".ar-link");
        }

        /*
         * An option outside a select -- in a datalist -- still takes its
         * chosen class from the markup. Inside one, ar__walk_select decides,
         * because there the select's value does.
         */
        if (ar_span_is(d->nodes[node].name, "option") && ar__chosen_option(d, node))
        {
            ar__put_lit(sel, ".ar-chosen");
        }

        /* The first legend of a fieldset is drawn on the fieldset's border,
           which the layout and the painter learn from this bit. */
        if (ar_span_is(d->nodes[node].name, "legend") && d->nodes[node].parent >= 0 &&
            ar_span_is(d->nodes[d->nodes[node].parent].name, "fieldset"))
        {
            ar_i32 sib;

            for (sib = d->nodes[d->nodes[node].parent].first_child; sib >= 0;
                 sib = d->nodes[sib].next_sibling)
            {
                if (d->nodes[sib].kind == AR_DOM_ELEMENT &&
                    ar_span_is(d->nodes[sib].name, "legend"))
                {
                    break;
                }
            }
            if (sib == node)
            {
                st |= AR_STATE_LEGEND;
            }
        }

        if (st)
        {
            ar_state_next(c, st);
        }

        /*
         * Register the control, unless it is disabled -- a disabled control
         * takes no click and no key, and the cheapest way to make sure of that
         * is for the end of the frame never to hear of it. A text field is the
         * exception: it still needs its flags, so a disabled password still
         * draws bullets, and it can never be focused to be typed into.
         */
        if (kind != AR_CTL_NONE && (!disabled || kind == AR_CTL_TEXT))
        {
            ar_u32 group = (ar_u32)node;
            ar_i32 val = 0;
            int    reg = 1;

            if (kind == AR_CTL_RADIO)
            {
                /* A radio's group is the hash of its `name`, and a radio with
                   no name is a group of one -- which is what a browser does
                   and is less surprising than making every unnamed radio
                   fight the others. */
                ar_span nm = ar__attr_of(d, node, "name");

                if (nm.p)
                {
                    group = ar_hash(nm.p, nm.n);
                }
            }
            else if (kind == AR_CTL_SELECT)
            {
                ar_i32 count = 0, chosen = 0;

                ar__select_info(d, node, &count, &chosen);
                val = (count << 16) | (chosen & 0xFFFF);
            }
            else if (kind == AR_CTL_RANGE)
            {
                ar_i32 lo, hi, step;

                ar__range_limits(d, node, &lo, &hi, &step, &cval);
                ar_field_next(c, 0, lo, hi, step, 0);
                val = cval;
            }
            else if (kind == AR_CTL_COLOR)
            {
                cval = ar__color_value(d, node);
                val = cval;
            }
            else if (kind == AR_CTL_LABEL)
            {
                val = ar__label_target(d, node);
                reg = val >= 0;
            }
            else if (kind == AR_CTL_TEXT)
            {
                /*
                 * `value` is where a text field starts, and the buffer is where
                 * it goes after anybody types; a textarea starts from its
                 * content instead. Read once, when the field is first focused.
                 */
                ar_span v = ar_span_is(d->nodes[node].name, "textarea")
                                ? ar__first_text(d, node)
                                : ar__attr_of(d, node, "value");

                ar_field_next(c, ar__field_flags(d, node, disabled),
                              ar__attr_num(d, node, "min", -2000000000),
                              ar__attr_num(d, node, "max", 2000000000),
                              ar__attr_num(d, node, "step", 1000),
                              ar__attr_num(d, node, "maxlength", 0) / 1000);
                ar_value_next(c, v.p, v.n);
            }
            if (reg)
            {
                ar_control_next(c, kind, group, node, val);
            }
        }
        ar_begin_hinted(c, sel, h, ar__inline_style(d, node, style));
        d->nodes[node].box = ar_node_count(c) - 1;
    }
    self_open = ar_box_is_open(c);
    self_key = ar_box_key(c);

    /*
     * Whether the text inside keeps its spaces, from the computed style and no
     * longer from the tag.
     *
     * The element's style is resolved by the time its children are walked,
     * which is what makes this possible at all -- and doing it from the tag
     * was wrong in both directions: `white-space: pre` on a div collapsed
     * anyway, and `white-space: normal` on a `<pre>` did not. The user-agent
     * sheet says `pre { white-space: pre }` now, so the tag still decides by
     * default and an author can say otherwise.
     */
    pre = !AR_WS_COLLAPSES(ar_box_white_space(c));

    /* After the box exists, because ar_focusable marks the box most recently
       begun -- and before the children, so the tab order is document order. */
    if (ar__focusable_element(d, node))
    {
        ar_focusable(c, ar__attr_num(d, node, "tabindex", 0) / 1000);
    }

    /* A text field's contents are a box like any other text, so that they are
       measured, laid out and painted by the machinery that already does all
       three -- rather than by a special case that would have to learn them. */
    if (kind == AR_CTL_TEXT)
    {
        ar_span v = ar_span_is(d->nodes[node].name, "textarea") ? ar__first_text(d, node)
                                                                : ar__attr_of(d, node, "value");
        ar_span ph = ar__attr_of(d, node, "placeholder");

        ar_field_child(c, v.p, v.n, ph.p, ph.n);
    }

    /*
     * A push button's label is its `value`, and an `<input>` has no children
     * to put it in.
     *
     * `<button>Send</button>` carries its label as content and needs nothing
     * here; `<input type="submit" value="Send">` carries it as an attribute,
     * and without this the button draws as an empty box. With no value a
     * submit button says "Submit" and a reset button "Reset", which is what
     * every browser writes on them.
     */
    if (AR_CTL_IS_BUTTON(kind) && ar_span_is(d->nodes[node].name, "input"))
    {
        ar_span v = ar__attr_of(d, node, "value");

        if (!v.p && kind == AR_CTL_SUBMIT && ar__is_input_type(d, node, "image"))
        {
            v = ar__attr_of(d, node, "alt");
        }
        if (v.p && v.n > 0)
        {
            ar_text_kept(c, "ar-value", v.p, v.n);
        }
        else if (!v.p && kind == AR_CTL_SUBMIT)
        {
            ar_text(c, "ar-value", "Submit");
        }
        else if (!v.p && kind == AR_CTL_RESET)
        {
            ar_text(c, "ar-value", "Reset");
        }
    }

    /*
     * A checkbox and a radio each get one child box, which is the mark.
     *
     * A real box rather than a painted glyph, because that is what makes it
     * styleable -- the whole argument for building controls out of boxes at
     * all. It carries the same checked state as its parent so the sheet can
     * say `.ar-mark:checked`, rather than needing a combinator that reaches
     * from a parent's state to a child.
     */
    if (kind == AR_CTL_CHECKBOX || kind == AR_CTL_RADIO)
    {
        /* A tick for a checkbox and a dot for a radio, which is the difference
           a browser draws -- the tick is a shape the painter knows by tag, as
           the summary's triangle is. */
        ar_state_next(c, ar_box_is_checked(c) ? AR_STATE_CHECKED : 0);
        ar_begin(c, kind == AR_CTL_CHECKBOX ? "ar-tick" : "ar-mark");
        ar_end(c);
    }

    /* A select's arrow, and a textarea's resize grip: both drawn by the
       painter, both placed by the sheet in a corner of the control. */
    if (kind == AR_CTL_SELECT)
    {
        ar_begin(c, "ar-chev");
        ar_end(c);
    }
    if (kind == AR_CTL_TEXT && ar_span_is(d->nodes[node].name, "textarea"))
    {
        ar_begin(c, "ar-grip");
        ar_end(c);
    }

    /*
     * A slider is a track with a fill, and a rail with a thumb on it.
     *
     * The thumb is placed with `left: N%` on a relatively positioned box,
     * inside a rail one thumb narrower than the track -- so at 0% the thumb's
     * left edge is the track's and at 100% its right edge is, and its centre
     * is never off the end. Both are inline styles, so an author can restyle
     * the parts without the value losing its meaning.
     */
    if (kind == AR_CTL_RANGE)
    {
        ar_i32 lo, hi, step, def;
        char   buf[32];
        ar_i32 pct;

        ar__range_limits(d, node, &lo, &hi, &step, &def);
        ckey_val = ar_ctl_value(c, self_key, def);
        pct = ar__percent_of(ckey_val - lo, hi - lo);
        if (pct < 0)
        {
            pct = 0;
        }
        if (pct > 100)
        {
            pct = 100;
        }
        ar_begin(c, "ar-track");
        ar__style_pct(buf, "width", pct);
        ar_begin_styled(c, "ar-fill", buf);
        ar_end(c);
        ar_end(c);
        ar_begin(c, "ar-rail");
        ar__style_pct(buf, "left", pct);
        ar_begin_styled(c, "ar-thumb", buf);
        ar_end(c);
        ar_end(c);
    }

    /* A colour field is a swatch of its colour, and a palette of sixteen when
       it is open -- an engine with no platform cannot open the system's colour
       dialog, and a palette is what one offers first anyway. */
    if (kind == AR_CTL_COLOR)
    {
        char   buf[32];
        ar_u32 used = 0;
        ar_u32 owner = self_key;

        ckey_val = ar_ctl_value(c, self_key, cval);
        ar__put_str(buf, &used, sizeof buf, "background:");
        ar__put_hex(buf, &used, sizeof buf, ckey_val);
        buf[used] = 0;
        ar_begin_styled(c, "ar-swatch", buf);
        ar_end(c);
        if (self_open)
        {
            ar_i32 k;

            ar_begin(c, "ar-palette");
            for (k = 0; k < 16; ++k)
            {
                used = 0;
                ar__put_str(buf, &used, sizeof buf, "background:");
                ar__put_hex(buf, &used, sizeof buf, AR__PALETTE[k]);
                buf[used] = 0;
                ar_control_next(c, AR_CTL_SWATCH, owner, -1, AR__PALETTE[k]);
                ar_state_next(c, AR__PALETTE[k] == ckey_val ? AR_STATE_CHECKED : 0);
                ar_begin_styled(c, "ar-chip", buf);
                ar_end(c);
            }
            ar_end(c);
        }
    }

    /* A file field is a button and the name of what was chosen -- the name,
       because without a network that is all a file input submits. */
    if (kind == AR_CTL_FILE)
    {
        const char *name = 0;
        ar_u32      len = 0;

        ar_begin(c, "ar-pick");
        ar_text(c, "ar-value", "Choose File");
        ar_end(c);
        if (ar_field_value_of(c, self_key, &name, &len) && len > 0)
        {
            ar_text_kept(c, "ar-name", name, len);
        }
        else
        {
            ar_text(c, "ar-name", "No file chosen");
        }
    }

    ar__list_marker(c, d, node);

    /*
     * A `<summary>`'s disclosure triangle.
     *
     * A glyph in a browser and not one here -- the built-in face is ASCII, so
     * U+25B8 would draw as a question mark. It is a box the painter knows by
     * tag; see the note in ar__paint_boxes.
     *
     * It points right when its details is shut and down when it is open,
     * which is the only thing on the page that says which way it will go. The
     * open state is the parent's, and the walk is inside the details by the
     * time it reaches the summary, so ar_box_is_open answers for the box
     * being built rather than for its parent -- which is why the state is read
     * from the parent's box instead.
     */
    if (ar_span_is(d->nodes[node].name, "summary"))
    {
        ar_i32 up = d->nodes[node].parent;
        int    open = 0;

        if (up >= 0 && d->nodes[up].box >= 0 && d->nodes[up].box < ar_node_count(c))
        {
            open = ar_box_state(c, d->nodes[up].box, AR_STATE_OPEN);
        }
        else
        {
            open = up >= 0 && ar__attr_of(d, up, "open").p != 0;
        }
        ar_begin(c, open ? "ar-tri-d" : "ar-tri-r");
        ar_end(c);
    }

    /*
     * A `<progress>` or `<meter>` is a track with a bar in it, and the bar is
     * a box whose width is the value.
     *
     * The percentage is written as an inline style rather than resolved here,
     * so that the width lands in the cascade like any other and an author can
     * override the track around it without the bar losing its meaning.
     */
    {
        ar_span gname = d->nodes[node].name;
        int     meter = ar_span_is(gname, "meter");

        if (meter || ar_span_is(gname, "progress"))
        {
            char bar[32];

            /* A track with a fill, the same pill a slider is drawn as. */
            ar__style_pct(bar, "width", ar__gauge_pct(d, node, meter));
            ar_begin(c, "ar-track");
            ar_begin_styled(c, "ar-fill", bar);
            ar_end(c);
            ar_end(c);
        }
    }

    if (kind == AR_CTL_TEXT && ar_span_is(d->nodes[node].name, "textarea"))
    {
        /* A textarea's content is its value, already drawn by the field's
           own text box -- walking it as well would draw it twice, once as
           text nobody can edit. */
    }
    else if (kind == AR_CTL_SELECT)
    {
        /*
         * A select's face: every option a box, the one it is showing marked.
         * And when it is open, its list, as rows a click chooses from --
         * absolutely positioned under it by the sheet, and part of it, so a
         * press on a row lands inside the select and does not close it.
         */
        ar_i32 count = 0, chosen = 0, index = 0;
        ar_u32 key = self_key;
        ar_i32 cur;

        ar__select_info(d, node, &count, &chosen);
        cur = ar_ctl_value(c, self_key, chosen);
        ar__walk_select(c, d, node, pre, cur, key, 0, &index);
        if (self_open && !disabled)
        {
            index = 0;
            ar_begin(c, "ar-listbox");
            ar__walk_select(c, d, node, pre, cur, key, 1, &index);
            ar_end(c);
        }
    }
    else
    {
        /*
         * A closed `<details>` shows its summary and nothing else.
         *
         * Done by not building the boxes rather than by hiding them, which is
         * the difference between a collapsed section costing nothing and
         * costing a styled, laid-out subtree that is then not painted. A
         * document whose every section is collapsed is the case this is for.
         *
         * `display:none` in the user-agent sheet would be the other way, and
         * it cannot reach here: whether it is open is a state this frame
         * settles, and the sheet is parsed once.
         */
        int closed = ar_span_is(d->nodes[node].name, "details") && !self_open;

        for (child = d->nodes[node].first_child; child >= 0; child = d->nodes[child].next_sibling)
        {
            if (closed && !(d->nodes[child].kind == AR_DOM_ELEMENT &&
                            ar_span_is(d->nodes[child].name, "summary")))
            {
                continue;
            }
            ar__walk(c, d, child, pre);
        }
    }
    (void)ckey_val;
    ar_end(c);
}

void ar_dom_build(ar_ctx *c, ar_doc *d)
{
    ar_i32 i;

    if (!c || !d)
    {
        return;
    }
    /* Every node starts the frame boxless; the walk writes the ones it builds.
       A node it skips -- inside a closed details, the head -- reads -1, which
       is what the accessibility tree and a form's submission need to know. */
    for (i = 0; i < d->node_count; ++i)
    {
        d->nodes[i].box = -1;
    }
    ar_frame_doc(c, d);
    ar__walk(c, d, ar_dom_root(d), 0);
}

/* ------------------------------------------------------------------------
 * Forms
 * ------------------------------------------------------------------------ */

ar_i32 ar_dom_attr_num(const ar_doc *d, ar_i32 node, const char *name, ar_i32 dflt)
{
    return ar__attr_num(d, node, name, dflt);
}

/* The form a control belongs to: its `form` attribute's, or the nearest form
   around it. The attribute is how a button outside the form's markup submits
   it, and it is common enough in real pages to be worth the lookup. */
ar_i32 ar_dom_form_of(const ar_doc *d, ar_i32 node)
{
    ar_span f;
    ar_i32  up;

    if (!d || node < 0 || node >= d->node_count)
    {
        return -1;
    }
    f = ar__attr_of(d, node, "form");
    if (f.p)
    {
        ar_i32 t = ar__by_id(d, f);

        return (t >= 0 && ar_span_is(d->nodes[t].name, "form")) ? t : -1;
    }
    for (up = d->nodes[node].parent; up >= 0; up = d->nodes[up].parent)
    {
        if (d->nodes[up].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[up].name, "form"))
        {
            return up;
        }
    }
    return -1;
}

static int ar__in_form(const ar_doc *d, ar_i32 node, ar_i32 form)
{
    return ar_dom_form_of(d, node) == form;
}

/* The form's default button: its first submit button in tree order, -1 for
   none, -2 when that button is disabled -- which blocks implicit submission
   rather than passing it on to the next one. */
ar_i32 ar_dom_first_submit(const ar_doc *d, ar_i32 form)
{
    ar_i32 i;

    for (i = 0; i < d->node_count; ++i)
    {
        if (d->nodes[i].kind != AR_DOM_ELEMENT || ar__control_kind(d, i) != AR_CTL_SUBMIT ||
            !ar__in_form(d, i, form))
        {
            continue;
        }
        return ar__is_disabled(d, i) ? -2 : i;
    }
    return -1;
}

/*
 * Put a form back the way its markup had it.
 *
 * Not by writing the markup's values back, but by forgetting what the user
 * did: the TOUCHED bit, the typed text, the chosen option. Every control
 * already reads its markup when nothing has been done to it, so forgetting is
 * the whole of resetting -- and it cannot disagree with first load.
 */
void ar_dom_form_reset(ar_ctx *c, const ar_doc *d, ar_i32 form)
{
    ar_i32 i;

    for (i = 0; i < d->node_count; ++i)
    {
        ar_u32   key;
        ar_slot *slot;

        if (d->nodes[i].kind != AR_DOM_ELEMENT || ar__control_kind(d, i) == AR_CTL_NONE ||
            !ar__in_form(d, i, form))
        {
            continue;
        }
        key = ar_ctl_key_of(c, i);
        if (key == 0)
        {
            continue;
        }
        ar_ctl_forget(c, key);
        slot = ar_ctx_slot(c, key);
        if (slot)
        {
            slot->flags = (ar_u8)(slot->flags & ~(ar_u8)(AR_SLOT_TOUCHED | AR_SLOT_CHECKED));
        }
    }
}

/* application/x-www-form-urlencoded, one byte at a time: letters, digits and
   `*-._` as they are, a space as `+`, everything else as %XX. The length is
   counted past `cap`, so the caller learns how much room it needs. */
static void ar__enc(char *buf, ar_u32 cap, ar_u32 *used, const char *p, ar_u32 n)
{
    static const char HEX[] = "0123456789ABCDEF";
    ar_u32            i;

    for (i = 0; i < n; ++i)
    {
        unsigned char ch = (unsigned char)p[i];

        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '*' || ch == '-' || ch == '.' || ch == '_')
        {
            if (*used < cap)
            {
                buf[*used] = (char)ch;
            }
            ++*used;
        }
        else if (ch == ' ')
        {
            if (*used < cap)
            {
                buf[*used] = '+';
            }
            ++*used;
        }
        else
        {
            if (*used + 2 < cap)
            {
                buf[*used] = '%';
                buf[*used + 1] = HEX[ch >> 4];
                buf[*used + 2] = HEX[ch & 15];
            }
            *used += 3;
        }
    }
}

/* A textarea's newlines go out as CRLF, which is the specification's
   normalisation for a submitted value and what every server expects. */
static void ar__enc_lines(char *buf, ar_u32 cap, ar_u32 *used, const char *p, ar_u32 n)
{
    ar_u32 i, from = 0;

    for (i = 0; i < n; ++i)
    {
        if (p[i] == '\n')
        {
            ar__enc(buf, cap, used, p + from, i - from);
            ar__enc(buf, cap, used, "\r\n", 2);
            from = i + 1;
        }
    }
    ar__enc(buf, cap, used, p + from, n - from);
}

static void ar__pair(char *buf, ar_u32 cap, ar_u32 *used, ar_span name, const char *v, ar_u32 n,
                     int lines)
{
    if (*used > 0)
    {
        if (*used < cap)
        {
            buf[*used] = '&';
        }
        ++*used;
    }
    ar__enc(buf, cap, used, name.p, name.n);
    if (*used < cap)
    {
        buf[*used] = '=';
    }
    ++*used;
    if (lines)
    {
        ar__enc_lines(buf, cap, used, v, n);
    }
    else
    {
        ar__enc(buf, cap, used, v, n);
    }
}

/* Whether a checkbox or radio is checked now: what the user left, if they
   touched it, and the markup if not. */
static int ar__checked_now(const ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_u32         key = ar_ctl_key_of(c, node);
    const ar_slot *slot = key ? ar_ctx_slot_find(c, key) : 0;

    if (slot && (slot->flags & AR_SLOT_TOUCHED))
    {
        return (slot->flags & AR_SLOT_CHECKED) != 0;
    }
    return ar__attr_of(d, node, "checked").p != 0;
}

/* The option a select is showing, by index through its optgroups. */
static ar_i32 ar__nth_option(const ar_doc *d, ar_i32 node, ar_i32 *n)
{
    ar_i32 ch;

    for (ch = d->nodes[node].first_child; ch >= 0; ch = d->nodes[ch].next_sibling)
    {
        if (d->nodes[ch].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        if (ar_span_is(d->nodes[ch].name, "option"))
        {
            if (*n == 0)
            {
                return ch;
            }
            --*n;
        }
        else if (ar_span_is(d->nodes[ch].name, "optgroup"))
        {
            ar_i32 hit = ar__nth_option(d, ch, n);

            if (hit >= 0)
            {
                return hit;
            }
        }
    }
    return -1;
}

ar_i32 ar_dom_select_option(const ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 count = 0, chosen = 0, idx;

    ar__select_info(d, node, &count, &chosen);
    idx = ar_ctl_value(c, ar_ctl_key_of(c, node), chosen);
    return ar__nth_option(d, node, &idx);
}

/* A text control's value now: the buffer or the pool if it has been touched,
   the markup if not. */
static void ar__text_now(const ar_ctx *c, const ar_doc *d, ar_i32 node, const char **p, ar_u32 *n)
{
    ar_span v;

    if (ar_field_value_of(c, ar_ctl_key_of(c, node), p, n))
    {
        return;
    }
    v = ar_span_is(d->nodes[node].name, "textarea") ? ar__first_text(d, node)
                                                    : ar__attr_of(d, node, "value");
    *p = v.p ? v.p : "";
    *n = v.p ? v.n : 0;
}

ar_u32 ar_form_encode(const ar_ctx *c, ar_i32 form, ar_i32 submitter, char *buf, ar_u32 cap)
{
    const ar_doc *d;
    ar_u32        used = 0;
    ar_i32        i;

    if (!c || !c->frame_doc || form < 0)
    {
        return 0;
    }
    d = c->frame_doc;
    if (!buf)
    {
        cap = 0;
    }

    /* Tree order is submission order, which servers that read repeated names
       as a list depend on. */
    for (i = 0; i < d->node_count; ++i)
    {
        ar_u8   kind;
        ar_span name;

        if (d->nodes[i].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        name = ar__attr_of(d, i, "name");
        if (!name.p || name.n == 0)
        {
            continue;
        }
        kind = ar__control_kind(d, i);
        if (!ar__in_form(d, i, form) || ar__is_disabled(d, i))
        {
            continue;
        }
        if (kind == AR_CTL_NONE && !ar__is_input_type(d, i, "hidden"))
        {
            continue;
        }

        if (kind == AR_CTL_CHECKBOX || kind == AR_CTL_RADIO)
        {
            ar_span v = ar__attr_of(d, i, "value");

            if (ar__checked_now(c, d, i))
            {
                ar__pair(buf, cap, &used, name, v.p ? v.p : "on", v.p ? v.n : 2u, 0);
            }
        }
        else if (AR_CTL_IS_BUTTON(kind))
        {
            ar_span v = ar__attr_of(d, i, "value");

            if (i == submitter && kind == AR_CTL_SUBMIT)
            {
                ar__pair(buf, cap, &used, name, v.p ? v.p : "", v.n, 0);
            }
        }
        else if (kind == AR_CTL_SELECT)
        {
            ar_i32 opt = ar_dom_select_option(c, d, i);

            if (opt >= 0)
            {
                ar_span v = ar__attr_of(d, opt, "value");

                if (!v.p)
                {
                    v = ar__first_text(d, opt);
                }
                ar__pair(buf, cap, &used, name, v.p ? v.p : "", v.p ? v.n : 0u, 0);
            }
        }
        else if (kind == AR_CTL_RANGE)
        {
            ar_i32 lo, hi, step, def;
            char   num[24];
            ar_u32 nn = 0;
            ar_i32 v;

            ar__range_limits(d, i, &lo, &hi, &step, &def);
            v = ar_ctl_value(c, ar_ctl_key_of(c, i), def);
            /* Whole numbers when the step is, which is what a browser sends. */
            if (v < 0)
            {
                num[nn++] = '-';
                v = -v;
            }
            ar__put_num(num, &nn, sizeof num, v / 1000);
            if (v % 1000)
            {
                ar_i32 f = v % 1000;

                num[nn++] = '.';
                num[nn++] = (char)('0' + f / 100);
                if (f % 100)
                {
                    num[nn++] = (char)('0' + (f / 10) % 10);
                    if (f % 10)
                    {
                        num[nn++] = (char)('0' + f % 10);
                    }
                }
            }
            ar__pair(buf, cap, &used, name, num, nn, 0);
        }
        else if (kind == AR_CTL_COLOR)
        {
            char   hex[8];
            ar_u32 nn = 0;

            ar__put_hex(hex, &nn, sizeof hex,
                        ar_ctl_value(c, ar_ctl_key_of(c, i), ar__color_value(d, i)));
            ar__pair(buf, cap, &used, name, hex, nn, 0);
        }
        else if (kind == AR_CTL_FILE)
        {
            const char *p = "";
            ar_u32      n = 0;

            ar_field_value_of(c, ar_ctl_key_of(c, i), &p, &n);
            ar__pair(buf, cap, &used, name, p, n, 0);
        }
        else if (kind == AR_CTL_TEXT || ar__is_input_type(d, i, "hidden"))
        {
            const char *p;
            ar_u32      n;

            ar__text_now(c, d, i, &p, &n);
            ar__pair(buf, cap, &used, name, p, n, ar_span_is(d->nodes[i].name, "textarea"));
        }
    }
    if (cap > 0)
    {
        buf[used < cap ? used : cap - 1] = 0;
    }
    return used;
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
 * `<link rel=stylesheet>` is handled too, through the callback the embedder
 * sets with ar_set_stylesheet_loader. There is no networking and no file IO
 * here, by design, so the bytes have to come from somebody who has them --
 * and a link with no loader behind it is counted rather than ignored.
 */
/*
 * Whether this `rel` names a stylesheet.
 *
 * A space-separated list of keywords, matched case-insensitively, because
 * `rel="stylesheet"` and `rel="STYLESHEET"` and `rel="alternate stylesheet"`
 * are all in real markup and the first two mean the same thing. The third does
 * not -- an alternate sheet is one the reader may choose and is not applied
 * until they do -- so it is refused rather than loaded.
 */
static int ar__rel_is_stylesheet(ar_span rel)
{
    ar_u32 i = 0;
    int    saw_sheet = 0;
    int    saw_alt = 0;

    while (i < rel.n)
    {
        ar_u32 start;

        while (i < rel.n && ar__space(rel.p[i]))
        {
            ++i;
        }
        start = i;
        while (i < rel.n && !ar__space(rel.p[i]))
        {
            ++i;
        }
        if (i > start)
        {
            ar_span word;

            word.p = rel.p + start;
            word.n = i - start;
            if (ar_span_is(word, "stylesheet"))
            {
                saw_sheet = 1;
            }
            else if (ar_span_is(word, "alternate"))
            {
                saw_alt = 1;
            }
        }
    }
    return saw_sheet && !saw_alt;
}

/*
 * One `<link rel=stylesheet>`, handed to whoever can fetch it.
 *
 * Returns 1 if a sheet was parsed. A link with no loader, no href or a loader
 * that declined is counted as skipped instead -- a number a caller can ask
 * for, because a page whose design is in one external sheet renders as
 * unstyled text either way and the difference matters.
 */
static int ar__collect_link(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    char        href[512];
    ar_span     rel = ar__attr_of(d, node, "rel");
    ar_span     h = ar__attr_of(d, node, "href");
    const char *css;
    ar_u32      used = 0;

    if (!ar__rel_is_stylesheet(rel))
    {
        return 0; /* not a stylesheet link at all, so nothing was skipped */
    }
    if (!c->link_load || h.n == 0)
    {
        ++c->links_skipped;
        return 0;
    }
    ar__put_span(href, &used, (ar_u32)sizeof href, h);
    href[used] = 0;

    css = c->link_load(c->link_user, href);
    if (!css)
    {
        ++c->links_skipped;
        return 0;
    }
    ar_stylesheet(c, css);
    return 1;
}

static ar_i32 ar__collect_styles(ar_ctx *c, const ar_doc *d, ar_i32 node)
{
    ar_i32 found = 0;
    ar_i32 child;

    if (node < 0)
    {
        return 0;
    }
    if (d->nodes[node].kind == AR_DOM_ELEMENT && ar_span_is(d->nodes[node].name, "link"))
    {
        /* In the same walk as `<style>` and not in a pass of its own, because
           the two interleave: `<link>` then `<style>` then `<link>` is three
           sheets in that order, and the order is the cascade. */
        return ar__collect_link(c, d, node);
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

/*
 * The user-agent rules that apply only in quirks mode.
 *
 * One rule, and it is not a curiosity. A `<table>` in quirks mode does not
 * inherit the font its container set: a page that says `body { font-size:30px }`
 * gets a table at sixteen, which is what browsers did before CSS and still do
 * for a document that asks for quirks. A page written that way and rendered
 * with the table inherited is wrong everywhere there is a table, which on the
 * old web is most pages.
 *
 * `font-family` belongs here too and is missing because there is no such
 * property yet.
 */
static const char AR__QUIRKS_CSS[] = "table { font-size:16px; }";

ar_i32 ar_doc_stylesheets(ar_ctx *c, const ar_doc *d)
{
    if (!c || !d || d->node_count == 0)
    {
        return 0;
    }

    /*
     * The doctype decides two things about how the page's own stylesheets are
     * read, and both are decided here because here is the first moment both
     * the document and the sheets are in hand.
     *
     * The quirks rules are bracketed as the user agent's so they sort into the
     * user-agent band -- above nothing and below everything the page says,
     * which is where a default belongs. Written after the main sheet rather
     * than inside it because the mode is not known when that one is loaded.
     */
    c->links_skipped = 0;
    if (d->quirks == AR_QUIRKS_YES)
    {
        ar_sheet_begin_ua(&c->sheet);
        ar_stylesheet(c, AR__QUIRKS_CSS);
        ar_sheet_mark_ua(&c->sheet);
    }
    ar_sheet_set_strict_lengths(&c->sheet, d->quirks != AR_QUIRKS_YES);

    {
        ar_i32 n = ar__collect_styles(c, d, 0);

        /* Back to lenient, so the next thing parsed -- an interface's sheet,
           another document's -- is not styled by this document's doctype. */
        ar_sheet_set_strict_lengths(&c->sheet, 0);
        return n;
    }
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
