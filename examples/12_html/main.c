/*
 * areole example 12 - the HTML tree corpus
 * SPDX-License-Identifier: MIT
 *
 * What tree a document produces, checked against a browser rather than against
 * my reading of §13.2.6.
 *
 *     example_html --dump
 *     example_html --html > examples/12_html/cases.html
 *     python tools/compare_trees.py --run ./build/example_html.exe \
 *            examples/12_html/cases.html
 *
 * ------------------------------------------------------------------------
 * Why this compares trees and not rectangles
 *
 * Every other corpus here compares geometry, because every other corpus is
 * about layout. This one is about the parser, and the parser's output is a
 * tree -- so comparing rectangles would measure areole's user-agent stylesheet
 * against a browser's, which is a different and much later question.
 *
 * A tree is compared as one string per case: `html(head body(p(#) p(#)))`,
 * where `#` is a text node. Whole-shape rather than a property at a time,
 * because a check that asks whether one substring precedes another accepts
 * almost anything -- which the grid corpus learned the hard way when a loose
 * assertion passed on a tree with the table's contents inside an emphasis.
 *
 * ------------------------------------------------------------------------
 * What this stands in for
 *
 * 0.9.0's acceptance criterion is the html5lib tree-construction suite at 98%.
 * html5lib-tests is not vendored -- it would have to be, with its licence in
 * THIRDPARTY.md, and nothing is fetched at build time -- so this is the same
 * question asked of the browser that is on the machine.
 *
 * It is weaker in one specific way and the difference is worth naming: a suite
 * states the expected tree, so it is a fixed target. A browser is a second
 * implementation, so a case where areole and the browser are both wrong in the
 * same way passes here and would fail there. In exchange it covers whatever
 * anybody writes down, immediately, and needs no network.
 *
 * ------------------------------------------------------------------------
 * The twin is generated
 *
 * `--html` prints the browser side. Every hand-written twin in this repository
 * has drifted from its C side -- tour.html stops at 0.6.0 while main.c has
 * fifteen pages -- and CI diffs the committed file against a fresh one.
 */
#include "ar_html.h"
#include "areole.h"

#include <stdio.h>
#include <string.h>

static ar_dom_node g_nodes[1024];
static ar_attr     g_attrs[256];
static char        g_text[16384];
static char        g_scratch[4096];
static ar_doc      g_doc;
static char        g_shape[2048];

/*
 * The cases.
 *
 * Grouped by what they are about, and every one of them is markup somebody has
 * written by accident. A parser that handles only well-formed input passes
 * none of these and looks fine doing it.
 */
static const struct
{
    const char *name;
    const char *src;
} CASES[] = {
    /* ------------------------------------------------ the optional tags -- */
    {"bare-text", "hello"},
    {"bare-p", "<p>hi</p>"},
    {"explicit-skeleton", "<html><head></head><body><p>hi</p></body></html>"},
    {"head-implied", "<title>T</title><p>b"},
    {"body-implied", "<html><p>b"},
    {"text-before-head", "x<p>y"},

    /* ------------------------------------------------ closing themselves -- */
    {"p-closes-p", "<p>a<p>b"},
    {"div-closes-p", "<p>a<div>b</div>"},
    {"p-does-not-close-div", "<div>a<p>b</p></div>"},
    {"li-closes-li", "<ul><li>a<li>b</ul>"},
    {"li-nested-list", "<ul><li>a<ul><li>b</ul></ul>"},
    {"dl-dt-dd", "<dl><dt>a<dd>b<dt>c<dd>d</dl>"},
    {"heading-then-p", "<h1>a</h1><p>b"},
    {"unclosed-everything", "<div><p><span>x"},

    /* ------------------------------------------------------- formatting -- */
    {"plain-b", "<b>x</b>"},
    {"b-across-p", "<p><b>bold</p>text"},
    {"b-keeps-p-inside", "<b>one<p>two</p></b>"},
    {"misnest-bi", "<b><i>x</b>y</i>"},
    {"misnest-ib", "<i><b>x</i>y</b>"},
    {"misnest-with-p", "<b><i><p></b></i>"},
    {"misnest-p-block", "<b>1<p>2</b>3</p>"},
    {"misnest-three-deep", "<b>1<i>2<em>3</b>4</em>5</i>"},
    {"misnest-em-i", "<b><em><i>q</b>r</i></em>"},
    {"nested-a", "<a><b>1</a>2</b>"},
    {"a-closes-a", "<a>one<a>two"},
    {"formatting-chain", "<b><b><b>x</b>"},

    /* ----------------------------------------------------------- tables -- */
    {"table-implied-tbody", "<table><tr><td>a</table>"},
    {"table-explicit", "<table><tbody><tr><td>a</td></tr></tbody></table>"},
    {"table-cells-unclosed", "<table><tr><td>a<td>b</table>"},
    {"table-rows-unclosed", "<table><tr><td>a<tr><td>b</table>"},
    {"table-foster-em", "<table><em>x</em><tr><td>y</table>"},
    {"table-foster-text", "<table>stray<tr><td>y</table>"},
    {"table-thead-tfoot", "<table><thead><tr><td>h<tbody><tr><td>b</table>"},
    {"table-caption", "<table><caption>c</caption><tr><td>a</table>"},
    {"table-in-table", "<table><tr><td><table><tr><td>inner</table></table>"},
    {"td-orphan", "<td>orphan"},
    {"tr-orphan", "<tr><td>orphan"},

    /* ------------------------------------------------- text and comments -- */
    {"comment-in-body", "<p>a<!-- c -->b"},
    {"comment-before-html", "<!-- c --><p>a"},
    {"doctype", "<!DOCTYPE html><p>a"},
    {"doctype-legacy", "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01//EN\"><p>a"},
    {"entity-in-text", "<p>a&amp;b"},
    {"entity-unknown", "<p>a&nosuch;b"},
    {"bare-ampersand", "<p>AT&T"},
    {"numeric-entity", "<p>&#65;&#x42;"},
    {"cp1252-entity", "<p>&#147;q&#148;"},

    /* --------------------------------------------------------- raw text -- */
    {"style-holds-markup", "<style>a{content:\"<b>\";}</style><p>after"},
    {"title-holds-markup", "<title>a<b>c</title><p>after"},
    {"textarea-holds-markup", "<textarea><p>x</textarea><p>after"},

    /* ------------------------------------------------------- attributes -- */
    {"attributes", "<div id=\"a\" class='b c' hidden>x</div>"},
    {"attr-unquoted", "<div id=a class=b>x</div>"},
    {"end-tag-attrs", "<p>a</p class=x>"},
    {"self-closing", "<p>a<br/>b"},

    /* ------------------------------------------------------- the ragged -- */
    {"stray-end-tags", "</p></div></b><p>a"},
    {"bogus-pi", "<?php echo 1; ?><p>a"},
    {"empty-end-tag", "a</>b"},
    {"lone-lt", "a < b"},
    {"after-body", "<body><p>a</body>trailing"},
    {"empty", ""}};

#define CASE_COUNT ((ar_i32)(sizeof CASES / sizeof CASES[0]))

static void shape(ar_i32 i, ar_u32 *used)
{
    ar_i32 c;

    if (i < 0 || *used + 2 >= sizeof g_shape)
    {
        return;
    }
    if (g_doc.nodes[i].kind == AR_DOM_TEXT)
    {
        g_shape[(*used)++] = '#';
        return;
    }
    if (g_doc.nodes[i].kind == AR_DOM_COMMENT)
    {
        g_shape[(*used)++] = '!';
        return;
    }
    if (g_doc.nodes[i].kind != AR_DOM_ELEMENT)
    {
        return; /* the doctype is not in the shape; both sides agree it is
                   there and neither renders it */
    }
    {
        ar_u32 k;

        for (k = 0; k < g_doc.nodes[i].name.n && *used + 1 < sizeof g_shape; ++k)
        {
            g_shape[(*used)++] = g_doc.nodes[i].name.p[k];
        }
    }
    if (g_doc.nodes[i].first_child < 0)
    {
        return;
    }
    g_shape[(*used)++] = '(';
    for (c = g_doc.nodes[i].first_child; c >= 0; c = g_doc.nodes[c].next_sibling)
    {
        if (c != g_doc.nodes[i].first_child && *used + 1 < sizeof g_shape)
        {
            g_shape[(*used)++] = ' ';
        }
        shape(c, used);
    }
    if (*used + 1 < sizeof g_shape)
    {
        g_shape[(*used)++] = ')';
    }
}

static const char *tree_of(const char *src)
{
    ar_u32 used = 0;

    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_nodes;
    g_doc.node_cap = (ar_i32)(sizeof g_nodes / sizeof g_nodes[0]);
    g_doc.attrs = g_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_attrs / sizeof g_attrs[0]);
    g_doc.text = g_text;
    g_doc.text_cap = (ar_u32)sizeof g_text;

    ar_html_parse(&g_doc, src, (ar_u32)strlen(src), g_scratch, (ar_u32)sizeof g_scratch);
    shape(ar_dom_root(&g_doc), &used);
    g_shape[used] = 0;
    return g_shape;
}

static int run_dump(void)
{
    ar_i32 k;

    printf("# areole %s  html tree corpus\n", ar_version());
    for (k = 0; k < CASE_COUNT; ++k)
    {
        printf("# case %s\n", CASES[k].name);
        printf("%s\n", tree_of(CASES[k].src));
    }
    return 0;
}

/* The source, as a JavaScript string literal. Only three characters need
   escaping for that, and the corpus contains all three. */
static void emit_js_string(const char *s)
{
    while (*s)
    {
        if (*s == '\\' || *s == '"')
        {
            putchar('\\');
            putchar(*s);
        }
        else if (*s == '\n')
        {
            putchar('\\');
            putchar('n');
        }
        else
        {
            putchar(*s);
        }
        ++s;
    }
}

static int run_html(void)
{
    ar_i32 k;

    printf("<!DOCTYPE html>\n<meta charset=\"utf-8\">\n");
    printf("<title>areole html tree corpus %s the browser twin</title>\n\n", "\xe2\x80\x94");
    printf("<!--\n");
    printf("  GENERATED by examples/12_html/main.c. Do not edit.\n\n");
    printf("      ./build/example_html --html > examples/12_html/cases.html\n\n");
    printf("  The same documents as the C corpus, handed to the browser's own parser\n");
    printf("  through DOMParser, so both sides answer the same question about the same\n");
    printf("  bytes:\n\n");
    printf("      python tools/compare_trees.py --run ./build/example_html.exe \\\n");
    printf("             examples/12_html/cases.html\n");
    printf("-->\n\n");

    printf("<textarea id=\"out\" style=\"width:100%%;height:70vh\"></textarea>\n\n");
    printf("<script>\n");
    printf("const CASES = [\n");
    for (k = 0; k < CASE_COUNT; ++k)
    {
        printf("  [\"%s\", \"", CASES[k].name);
        emit_js_string(CASES[k].src);
        printf("\"],\n");
    }
    printf("];\n\n");
    printf("/* The same shape the C side prints: element names, '#' for a text node,\n");
    printf("   '!' for a comment, and the doctype left out because neither side\n");
    printf("   renders it. Empty text nodes are skipped on both sides. */\n");
    printf("function shape(el) {\n");
    printf("  if (el.nodeType === 3) return el.data.length ? '#' : '';\n");
    printf("  if (el.nodeType === 8) return '!';\n");
    printf("  if (el.nodeType !== 1) return '';\n");
    printf("  let s = el.nodeName.toLowerCase();\n");
    printf("  const kids = [];\n");
    printf("  for (const c of el.childNodes) {\n");
    printf("    const t = shape(c);\n");
    printf("    if (t) kids.push(t);\n");
    printf("  }\n");
    printf("  return kids.length ? s + '(' + kids.join(' ') + ')' : s;\n");
    printf("}\n\n");
    printf("const out = ['# browser ' + navigator.userAgent.replace(/\\s+/g, ' ')];\n");
    printf("for (const [name, src] of CASES) {\n");
    printf("  const d = new DOMParser().parseFromString(src, 'text/html');\n");
    printf("  out.push('# case ' + name);\n");
    printf("  out.push(shape(d.documentElement));\n");
    printf("}\n");
    printf("const box = document.getElementById('out');\n");
    printf("box.value = out.join('\\n');\n");
    printf("box.textContent = out.join('\\n');   /* so --dump-dom sees it */\n");
    printf("</script>\n");
    return 0;
}

/*
 * No window. This corpus has nothing to look at -- its output is a tree per
 * case, and a window showing sixty strings is a worse way to read sixty
 * strings than a terminal is. The other corpora draw because their subject is
 * where boxes land.
 */
int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--html") == 0)
    {
        return run_html();
    }
    return run_dump();
}
