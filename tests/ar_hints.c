/*
 * areole - the presentational-hint corpus.
 * SPDX-License-Identifier: MIT
 *
 * What HTML's legacy attributes compute to, checked against a browser rather
 * than against my reading of the rendering section.
 *
 *     ar_hints --dump                       areole's computed values
 *     ar_hints --html > tests/hints.html    the browser twin
 *     python tools/compare_computed.py --run ./build/ar_hints.exe \
 *            tests/hints.html
 *
 * ------------------------------------------------------------------------
 * Computed values, not rectangles
 *
 * 0.9.1's second acceptance criterion asks whether the *mapping* is right --
 * whether `<td bgcolor=red>` means what a browser thinks it means -- and a
 * rectangle answers a different question. Two engines can agree on every box
 * on a page and disagree about every colour on it, and half of these
 * attributes are colours.
 *
 * It also avoids a comparison this engine cannot win. A border in areole
 * reserves no space (see ar_used_size), and in a browser it reserves as much
 * as `border-style` lets it -- so `<table border=3>` is a three-pixel
 * difference in every rectangle for a reason that has nothing to do with the
 * attribute. The computed value is the thing under test and the thing both
 * engines can state.
 *
 * ------------------------------------------------------------------------
 * Why the twin is generated
 *
 * Every hand-written twin in this tree has drifted from the C source it was
 * meant to mirror, and the corpus went on reporting agreement about cases that
 * were no longer the same cases. `--html` writes it, and CI diffs the file
 * against what `--html` produces, so it cannot.
 */
#include "ar_internal.h"
#include "ar_html.h"
#include "ar_node.h"
#include "ar_css.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 420

static unsigned char g_memory[AR_MEM_DOC(512, 64u * 1024u)];
static ar_u32        g_pixels[CASE_W * 200];

/*
 * One case: a fragment, and which properties of `#x` inside it to report.
 *
 * `#x` rather than "the first element" because half of these attributes are
 * written on a table and land on a cell, so the element under test is rarely
 * the one carrying the attribute -- which is the part of the mapping most
 * likely to be wrong and was wrong in the first draft.
 */
typedef struct
{
    const char *name;
    const char *css;  /* an author stylesheet, or "" */
    const char *html; /* the fragment, with one element marked id="x" */
    const char *props;
} ar__case;

/*
 * The corpus.
 *
 * Grouped by what each group is for rather than by attribute, because the
 * groups fail for different reasons: a mapping group fails when an attribute
 * means the wrong thing, an ordering group when it means the right thing in
 * the wrong band of the cascade, and a parsing group when HTML's own rules for
 * reading a value are not HTML's.
 */
static const ar__case CASES[] = {
    /* -- the mapping: an attribute and the declaration it becomes -------- */
    /*
     * `<body bgcolor>` and `<body text>` are not here, and the reason is the
     * corpus rather than the mapping: these forty cases share one page, and a
     * document has one body. Rewritten as a div in the twin -- which is what
     * the first version did -- a browser correctly ignores both attributes,
     * because they belong to body alone, and the case then reports a
     * disagreement about the twin rather than about areole. Both are checked
     * in tests/ar_test.c instead, where every scene is its own document.
     */
    {"bgcolor-table", "", "<table id=\"x\" bgcolor=\"red\"><tr><td>t</td></tr></table>",
     "background-color"},
    {"bgcolor-tr", "", "<table><tr id=\"x\" bgcolor=\"red\"><td>t</td></tr></table>",
     "background-color"},
    {"bgcolor-td", "", "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>",
     "background-color"},
    {"bgcolor-th", "", "<table><tr><th id=\"x\" bgcolor=\"red\">t</th></tr></table>",
     "background-color"},
    {"font-color", "", "<font id=\"x\" color=\"green\">t</font>", "color"},

    {"width-img", "", "<img id=\"x\" width=\"200\">", "width"},
    {"height-img", "", "<img id=\"x\" height=\"90\">", "height"},
    {"width-table", "", "<table id=\"x\" width=\"300\"><tr><td>t</td></tr></table>", "width"},
    {"width-td", "", "<table><tr><td id=\"x\" width=\"120\">t</td></tr></table>", "width"},
    {"width-col", "",
     "<table><colgroup><col id=\"x\" width=\"70\"></colgroup><tr><td>t</td></tr></table>", "width"},
    {"width-hr", "", "<hr id=\"x\" width=\"150\">", "width"},

    {"cellspacing", "", "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>",
     "border-spacing"},
    {"cellpadding-lands-on-the-cell", "",
     "<table cellpadding=\"8\"><tr><td id=\"x\">t</td></tr></table>", "padding-top"},
    {"border-table", "", "<table id=\"x\" border=\"3\"><tr><td>t</td></tr></table>",
     "border-top-width"},
    {"border-gives-cells-one-pixel", "", "<table border=\"3\"><tr><td id=\"x\">t</td></tr></table>",
     "border-top-width"},
    {"border-zero-is-no-border", "", "<table border=\"0\"><tr><td id=\"x\">t</td></tr></table>",
     "border-top-width"},
    {"hspace-img", "", "<img id=\"x\" hspace=\"12\">", "margin-left"},
    {"vspace-img", "", "<img id=\"x\" vspace=\"6\">", "margin-top"},

    {"align-left", "", "<p id=\"x\" align=\"left\">t</p>", "text-align"},
    {"align-right", "", "<p id=\"x\" align=\"right\">t</p>", "text-align"},
    {"align-center", "", "<p id=\"x\" align=\"center\">t</p>", "text-align"},
    {"align-middle-is-center", "", "<p id=\"x\" align=\"middle\">t</p>", "text-align"},
    /* A known divergence rather than a bug: this engine has three text-align
       values and `justify` is not one of them, so the attribute maps to
       nothing and the property stays `left`. Kept in the corpus so the day it
       arrives, the case is already here. */
    {"align-justify", "", "<p id=\"x\" align=\"justify\">t</p>", "text-align"},
    {"align-td", "", "<table><tr><td id=\"x\" align=\"right\">t</td></tr></table>", "text-align"},
    {"align-nonsense-is-nothing", "", "<p id=\"x\" align=\"sideways\">t</p>", "text-align"},

    /* -- the cascade: which band each thing sits in ---------------------- */
    {"hint-beats-the-user-agent", "",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"author-beats-hint", "table { border-spacing:4px; }",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"author-beats-hint-on-the-cell", "td { padding:3px; }",
     "<table cellpadding=\"8\"><tr><td id=\"x\">t</td></tr></table>", "padding-top"},
    {"a-sheet-that-says-nothing-leaves-the-hint", "p { color:#112233; }",
     "<table id=\"x\" cellspacing=\"9\"><tr><td>t</td></tr></table>", "border-spacing"},
    {"style-beats-hint", "", "<img id=\"x\" width=\"200\" style=\"width:50px\">", "width"},
    {"style-leaves-the-other-attribute", "",
     "<img id=\"x\" width=\"200\" height=\"90\" style=\"width:50px\">", "height"},
    {"important-beats-style", "p { color:#00ff00 !important; }",
     "<p id=\"x\" style=\"color:#ff0000\">t</p>", "color"},
    {"important-style-beats-important", "p { color:#00ff00 !important; }",
     "<p id=\"x\" style=\"color:#ff0000 !important\">t</p>", "color"},
    {"style-beats-the-most-specific-selector", "p#x.a.b { width:20px; }",
     "<p id=\"x\" class=\"a b\" style=\"width:30px\">t</p>", "width"},

    /* -- HTML's own value parsing, which is not CSS's -------------------- */
    {"colour-keyword", "", "<table><tr><td id=\"x\" bgcolor=\"red\">t</td></tr></table>",
     "background-color"},
    {"colour-hash", "", "<table><tr><td id=\"x\" bgcolor=\"#ff0000\">t</td></tr></table>",
     "background-color"},
    {"colour-bare-hex", "", "<table><tr><td id=\"x\" bgcolor=\"ff0000\">t</td></tr></table>",
     "background-color"},
    {"colour-short-hash", "", "<table><tr><td id=\"x\" bgcolor=\"#f00\">t</td></tr></table>",
     "background-color"},
    {"length-pixels", "", "<img id=\"x\" width=\"200\">", "width"},
    {"length-percent", "", "<div style=\"width:400px\"><img id=\"x\" width=\"50%\"></div>",
     "width"},
    {"length-auto-is-not-a-length", "", "<img id=\"x\" width=\"auto\" height=\"77\">", "height"},
    {"length-trailing-junk", "", "<img id=\"x\" width=\"200abc\">", "width"},
    {"length-empty", "", "<img id=\"x\" width=\"\" height=\"55\">", "height"}};

#define CASE_N ((int)(sizeof CASES / sizeof CASES[0]))

/* ------------------------------------------------------------------------
 * areole's side
 * ------------------------------------------------------------------------ */

static char   g_html[8192];
static ar_u32 g_html_n;

/*
 * The fragment, wrapped so the parser has a document to build.
 *
 * `body` is given the case width and no margin, because the twin's container
 * is a plain div and a percentage has to resolve against the same number on
 * both sides. `length-percent` is the case that made this matter.
 */
static void build_source(const ar__case *k)
{
    g_html_n = 0;
    g_html[0] = 0;
    strcat(g_html, "<!doctype html><html><head><style>");
    strcat(g_html, "body { width:");
    sprintf(g_html + strlen(g_html), "%dpx", CASE_W);
    strcat(g_html, "; margin:0px; padding:0px; }");
    strcat(g_html, k->css);
    strcat(g_html, "</style></head>");
    if (strncmp(k->html, "<body", 5) == 0)
    {
        strcat(g_html, k->html);
    }
    else
    {
        strcat(g_html, "<body>");
        strcat(g_html, k->html);
        strcat(g_html, "</body>");
    }
    strcat(g_html, "</html>");
    g_html_n = (ar_u32)strlen(g_html);
}

/* The box whose id is `x`. Compared by hash, which is what the selector
   machinery stores -- there is no name on a box to compare against. */
static ar_i32 find_x(const ar_ctx *c)
{
    ar_u32 want = ar_hash("x", 1u);
    ar_i32 i;

    for (i = 0; i < ar_node_count(c); ++i)
    {
        if (c->nodes[i].sel_id == want)
        {
            return i;
        }
    }
    return -1;
}

/*
 * A colour as a browser states it.
 *
 * `getComputedStyle` says `rgb(255, 0, 0)` and `rgba(0, 0, 0, 0)` for
 * transparent, and both sides have to spell it the same way or every colour
 * case fails for punctuation. Fully opaque is `rgb(...)` and anything else is
 * `rgba(...)`, which is the rule Chrome follows.
 */
static void put_color(char *out, ar_u32 v)
{
    ar_u32 a = (v >> 24) & 0xFFu;
    ar_u32 r = (v >> 16) & 0xFFu;
    ar_u32 g = (v >> 8) & 0xFFu;
    ar_u32 b = v & 0xFFu;

    if (a == 255u)
    {
        sprintf(out, "rgb(%lu, %lu, %lu)", (unsigned long)r, (unsigned long)g, (unsigned long)b);
        return;
    }
    if (a == 0u)
    {
        /* Chrome prints the alpha channel of a fully transparent colour as
           `0`, and every other alpha as a decimal. Nothing here sets a
           fractional alpha, so this is the only form needed. */
        sprintf(out, "rgba(%lu, %lu, %lu, 0)", (unsigned long)r, (unsigned long)g,
                (unsigned long)b);
        return;
    }
    sprintf(out, "rgba(%lu, %lu, %lu, %lu)", (unsigned long)r, (unsigned long)g, (unsigned long)b,
            (unsigned long)a);
}

static const struct
{
    const char *name;
    ar_i32      prop;
    int         color;
} PROPS[] = {{"background-color", AR_P_BACKGROUND, 1},
             {"color", AR_P_COLOR, 1},
             {"width", AR_P_WIDTH, 0},
             {"height", AR_P_HEIGHT, 0},
             {"border-spacing", AR_P_BORDER_SPACING, 0},
             {"padding-top", AR_P_PAD_TOP, 0},
             {"margin-left", AR_P_MARGIN_LEFT, 0},
             {"margin-top", AR_P_MARGIN_TOP, 0},
             {"text-align", AR_P_TEXT_ALIGN, 0},
             {"border-top-width", AR_P_BORDER_WIDTH, 0}};

#define PROPS_N ((int)(sizeof PROPS / sizeof PROPS[0]))

static const char *align_name(ar_i32 v)
{
    if (v == AR_TEXT_ALIGN_CENTER)
    {
        return "center";
    }
    if (v == AR_TEXT_ALIGN_RIGHT)
    {
        return "right";
    }
    return "left";
}

/*
 * One property, in the spelling a browser uses.
 *
 * `width` is the resolved rectangle rather than the stated value, because that
 * is what `getComputedStyle` returns for it -- a used value, not a specified
 * one. `50%` is the case that makes the difference visible, and it is one of
 * the two cases the whole percent branch exists for.
 */
static void value_of(const ar_ctx *c, ar_i32 node, const char *prop, char *out)
{
    int i;

    out[0] = 0;
    for (i = 0; i < PROPS_N; ++i)
    {
        if (strcmp(PROPS[i].name, prop) != 0)
        {
            continue;
        }
        if (PROPS[i].color)
        {
            put_color(out, (ar_u32)AR_WIDE(&c->nodes[node].style, PROPS[i].prop));
            return;
        }
        if (PROPS[i].prop == AR_P_TEXT_ALIGN)
        {
            strcpy(out, align_name(c->nodes[node].style.v[AR_P_TEXT_ALIGN]));
            return;
        }
        /*
         * The content box, because that is what `getComputedStyle` returns for
         * `width` on a box that is not `border-box` sized -- a used value with
         * the padding taken back off. areole's rectangle is the border box, so
         * the two agreed on every element that had no padding and differed by
         * exactly two on the one that did: a `<td>`, which html.css gives a
         * pixel of padding on each side.
         *
         * Nothing is subtracted for the border, because a border in areole
         * reserves no space and is not in the rectangle to begin with.
         */
        if (PROPS[i].prop == AR_P_WIDTH)
        {
            const ar_style *st = &c->nodes[node].style;

            sprintf(out, "%ldpx",
                    (long)(ar_node_rect(c, node).w - st->v[AR_P_PAD_LEFT] - st->v[AR_P_PAD_RIGHT]));
            return;
        }
        if (PROPS[i].prop == AR_P_HEIGHT)
        {
            const ar_style *st = &c->nodes[node].style;

            sprintf(out, "%ldpx",
                    (long)(ar_node_rect(c, node).h - st->v[AR_P_PAD_TOP] - st->v[AR_P_PAD_BOTTOM]));
            return;
        }
        sprintf(out, "%ldpx", (long)c->nodes[node].style.v[PROPS[i].prop]);
        return;
    }
    strcpy(out, "?");
}

static int dump_one(const ar__case *k)
{
    ar_surface s;
    ar_input   in;
    ar_ctx    *c;
    ar_doc    *d;
    ar_i32     x;
    char       value[64];

    build_source(k);

    c = ar_init_ex(g_memory, (ar_u32)sizeof g_memory, 512, 64u * 1024u);
    if (!c)
    {
        printf("# case %s\n# the arena is too small\n", k->name);
        return 1;
    }
    ar_ua_stylesheet(c);
    d = ar_html_parse_into(c, g_html, g_html_n);
    if (!d)
    {
        printf("# case %s\n# did not parse\n", k->name);
        return 1;
    }
    ar_doc_stylesheets(c, d);

    memset(&s, 0, sizeof s);
    s.pixels = g_pixels;
    s.w = CASE_W;
    s.h = 200;
    s.stride = CASE_W;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    ar_frame_begin(c, &in);
    ar_dom_build(c, d);
    ar_frame_end(c, &s);

    printf("# case %s\n", k->name);
    x = find_x(c);
    if (x < 0)
    {
        printf("%s ?\n", k->props);
        return 1;
    }
    value_of(c, x, k->props, value);
    printf("%s %s\n", k->props, value);
    return 0;
}

static int dump(void)
{
    int k, bad = 0;

    printf("# areole %s\n", ar_version());
    for (k = 0; k < CASE_N; ++k)
    {
        bad += dump_one(&CASES[k]);
    }
    return bad;
}

/* ------------------------------------------------------------------------
 * The twin
 * ------------------------------------------------------------------------ */

static void put_escaped(const char *s)
{
    for (; *s; ++s)
    {
        if (*s == '<')
        {
            fputs("\\u003c", stdout);
        }
        else if (*s == '\\' || *s == '\'')
        {
            fputc('\\', stdout);
            fputc(*s, stdout);
        }
        else
        {
            fputc(*s, stdout);
        }
    }
}

static void emit_html(void)
{
    int k;

    printf("<!DOCTYPE html>\n<meta charset=\"utf-8\">\n");
    printf("<title>areole presentational hints \xe2\x80\x94 the browser twin</title>\n\n");
    printf("<!--\n  GENERATED by ar_hints --html. Do not edit; CI diffs it.\n\n");
    printf("      python tools/compare_computed.py --run ./build/ar_hints.exe \\\n");
    printf("             tests/hints.html\n\n");
    printf("  Each case is a container the size of areole's body, holding the same\n");
    printf("  fragment, with the same author stylesheet scoped to it. The reported\n");
    printf("  value is getComputedStyle of the element marked id=x.\n-->\n\n");
    printf("<style>\n");
    printf("  body { margin:0; font:16px/1.2 monospace; }\n");
    printf("  .case { width:%dpx; margin:0; padding:0; }\n", CASE_W);
    printf("  textarea#out { display:block; width:100%%; height:240px; }\n");
    printf("</style>\n\n<div id=\"cases\"></div>\n<textarea id=\"out\" readonly></textarea>\n\n");

    printf("<script>\nconst CASES = [\n");
    for (k = 0; k < CASE_N; ++k)
    {
        printf("  ['");
        put_escaped(CASES[k].name);
        printf("', '");
        put_escaped(CASES[k].css);
        printf("', '");
        put_escaped(CASES[k].html);
        printf("', '");
        put_escaped(CASES[k].props);
        printf("'],\n");
    }
    printf("];\n\n");

    printf("const host = document.getElementById('cases');\n");
    printf("const out = [];\n");
    printf("for (const [name, css, html, prop] of CASES) {\n");
    printf("  const box = document.createElement('div');\n");
    printf("  box.className = 'case';\n");
    printf("  box.dataset.page = name;\n");
    printf("  if (css) {\n");
    printf("    /* Scoped, so forty stylesheets can share one page. */\n");
    printf("    const st = document.createElement('style');\n");
    printf("    st.textContent = css.replace(/(^|\\})\\s*([^{}]+)\\{/g,\n");
    printf("      (m, brace, sel) => brace + sel.split(',')\n");
    printf("        .map(s => '[data-page=\"' + name + '\"] ' + s.trim()).join(',') + '{');\n");
    printf("    box.appendChild(st);\n");
    printf("  }\n");
    printf("  box.insertAdjacentHTML('beforeend', html);\n");
    printf("  host.appendChild(box);\n");
    printf("}\n\n");
    printf("for (const [name, css, html, prop] of CASES) {\n");
    printf("  const box = host.querySelector('[data-page=\"' + name + '\"]');\n");
    printf("  const el = box.querySelector('#x') ||\n");
    printf("             (box.firstElementChild && box.firstElementChild.id === 'x'\n");
    printf("               ? box.firstElementChild : null);\n");
    printf("  out.push('# case ' + name);\n");
    printf("  out.push(prop + ' ' + (el ? getComputedStyle(el).getPropertyValue(prop) : '?'));\n");
    printf("}\n\n");
    printf("const t = document.getElementById('out');\n");
    printf("t.value = out.join('\\n');\n");
    printf("t.textContent = out.join('\\n');\n");
    printf("</script>\n");
}

int main(int argc, char **argv)
{
    int k;

    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--html") == 0)
        {
            emit_html();
            return 0;
        }
    }
    return dump() ? 1 : 0;
}
