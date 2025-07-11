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

    /* ------------------- foster parenting meets the adoption agency -- */
    /*
     * The gap the corpus had: it held foster-parenting cases, and it held
     * adoption-agency cases, and nothing where the agency's own step 4.14 has
     * to foster parent. That step relocates a node into the common ancestor
     * and has to be told where -- the specification calls it the override
     * target -- and without it the destination was worked out again from the
     * current node, which by then is the node being moved.
     *
     * `<table><em><p>x</em` came out with the paragraph missing from the tree
     * altogether, at every document size. ar_fuzz found it; these keep it
     * found.
     */
    {"foster-agency-p", "<table><em><p>x</em"},
    {"foster-p-into-em", "<table><em><p>x"},
    {"foster-text-into-em", "<table><em>y"},
    {"foster-b-into-em", "<table><em><b>z"},
    {"foster-agency-moves-p", "<table><b><p>x</b>"},
    {"foster-agency-moves-div", "<table><em><div>x</em>"},
    {"agency-em-wraps-table", "<em><table><p>x</em>"},
    {"foster-agency-closed", "<table><em><p>x</p></em>"},
    {"agency-without-a-table", "<div><em><p>x</em>"},

    /* ------------------------------------------- a tag that never ended -- */
    /*
     * §13.2.5.10 and the eight states after it all say the same thing about
     * end of file: emit an end-of-file token. Not the tag -- the tag is
     * dropped, attributes and all.
     *
     * Emitting it instead looks harmless and is not. `<table><em><p>x</em`
     * ends in an unterminated end tag, and taking it seriously runs the
     * adoption agency: the paragraph is relocated out of the emphasis and a
     * clone of the emphasis appears inside it. One case in this corpus
     * disagreed with the browser and every neighbouring case agreed, which is
     * what said the fault was in the tokenizer and not in the agency.
     */
    {"eof-in-start-tag", "<p>a<div"},
    {"eof-in-end-tag", "<b>x</b"},
    {"eof-in-tag-name", "<p>a<sec"},
    {"eof-before-attr", "<div id"},
    {"eof-in-attr-name", "<div clas"},
    {"eof-in-attr-value", "<div class=\"a"},
    {"eof-in-attr-value-unquoted", "<div class=a"},
    {"eof-after-solidus", "<br/"},
    {"eof-in-agency-end-tag", "<b><p>x</b"},
    {"foster-agency-b-td", "<table><b><td><i></b>"},
    {"foster-agency-anchor", "<table><a><tr><td><a>x</a>"},
    {"foster-agency-b-outside", "<b><table><p></b><tr><td>"},
    {"foster-agency-caption", "<table><caption><b><p></b></caption>"},
    {"foster-agency-tbody", "<table><tbody><em><tr><td><p></em>"},
    {"foster-agency-nested", "<b><i><table><p></b></i>"},
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
    {"empty", ""},

    /* ------------------------------------------- the modes not built yet -- */
    {"select-option", "<select><option>a<option>b</select>"},
    {"select-optgroup", "<select><optgroup label=x><option>a</select>"},
    {"select-stray-div", "<select><div>x</div><option>a</select>"},
    {"select-in-table", "<table><select><option>a</select></table>"},
    {"template-basic", "<template><p>x</p></template>"},
    {"template-in-table", "<table><template><tr><td>a</template></table>"},
    {"svg-inline", "<p>a<svg><circle/></svg>b"},
    {"math-inline", "<p><math><mi>x</mi></math>"},
    {"frameset", "<frameset><frame></frameset>"},

    /* -------------------------------------------- more of the ordinary -- */
    {"form-unclosed", "<form><p>a</form>"},
    {"button-nesting", "<button>a<button>b"},
    {"nobr-nesting", "<nobr>a<nobr>b"},
    {"image-is-img", "<image src=x>"},
    {"ruby", "<ruby>a<rt>b</ruby>"},
    {"iframe-rawtext", "<iframe><p>x</iframe><p>after"},
    {"noscript-in-body", "<noscript><p>x</p></noscript>"},
    {"xmp-rawtext", "<xmp><b>x</xmp>"},
    {"plaintext-eats-all", "<plaintext><b>x"},
    {"hr-in-p", "<p>a<hr>b"},
    {"address-closes-p", "<p>a<address>b"},
    {"nested-forms", "<form><form><p>a"},
    {"option-closes-option", "<option>a<option>b"},
    {"h1-closes-h2", "<h1>a<h2>b"},

    /* ------------------------------------ svg, math, and template content -- */
    /*
     * Foreign content is decided by *where* an element sits, not by what it
     * says, so these check the four things that get that wrong: the case
     * corrections SVG needs back after the tokenizer lowercased them, the
     * integration points where HTML resumes, the breakout list that pops the
     * whole subtree when an HTML block element appears, and the self-closing
     * tag that really does close.
     *
     * A template's contents are a separate fragment in the tree. The browser
     * twin reads them through `el.content`, which is why the shape function
     * walks through the fragment without printing it -- same tree, and the two
     * sides state it differently.
     */
    {"svg-basic", "<svg><circle/></svg>"},
    {"svg-case-fixed", "<svg><foreignobject><div>x</div></foreignobject></svg>"},
    {"svg-nested-html", "<div><svg><foreignObject><p>a</p></foreignObject></svg></div>"},
    {"svg-breakout-p", "<svg><p>a"},
    {"svg-breakout-table", "<svg><g><table><tr><td>x"},
    {"svg-self-closing", "<svg><g/><g/></svg>after"},
    {"svg-in-table", "<table><svg><g></g></svg><tr><td>y</table>"},
    {"svg-title-is-not-html", "<svg><title>a<b>c</b></title></svg>"},
    {"svg-desc-integration", "<p><svg><desc><p>x"},
    {"math-basic", "<math><mi>x</mi></math>"},
    {"math-text-integration", "<math><mtext><b>bold</b></mtext></math>"},
    {"math-breakout", "<math><mi>a<p>b"},
    {"math-annotation-html",
     "<math><annotation-xml encoding=\"text/html\"><p>x</p></annotation-xml></math>"},
    {"math-annotation-svg", "<math><annotation-xml><svg><g></g></svg></annotation-xml></math>"},
    {"svg-then-html", "<svg></svg><p>after"},
    {"math-then-html", "<math></math><p>after"},
    {"svg-unclosed", "<div><svg><g>x</div>y"},
    {"nested-svg-math", "<svg><foreignObject><math><mi>i</mi></math></foreignObject></svg>"},
    {"template-content-nested", "<template><div><template><p>x</p></template></div></template>"},
    {"template-text", "<template>plain text</template>"}};

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
    /*
     * A template's content fragment is transparent here.
     *
     * The browser twin reads a template through `el.content`, which hands back
     * the fragment's children directly, so the two sides agree on the shape
     * only if this walks through the fragment without printing it. The
     * html5lib serialisation in tests/ar_html5lib.c does print it, because
     * that format shows it -- same tree, two conventions.
     */
    if (g_doc.nodes[i].kind == AR_DOM_FRAGMENT)
    {
        ar_i32 k;

        for (k = g_doc.nodes[i].first_child; k >= 0; k = g_doc.nodes[k].next_sibling)
        {
            shape(k, used);
        }
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
    printf("  /* A template keeps its children in a DocumentFragment rather than in\n");
    printf("     childNodes. areole has no fragment -- the children are on the element\n");
    printf("     -- so the comparison looks through .content here and asks both sides\n");
    printf("     the same question. Without it every template case disagrees for a\n");
    printf("     reason that is a model difference rather than a bug. */\n");
    printf("  const kids0 = el.content ? el.content.childNodes : el.childNodes;\n");
    /* localName, not nodeName.toLowerCase(): an SVG element's name is
       case-sensitive and `foreignObject` keeping its capital F is exactly what
       the corpus is checking. Lowercasing it here would make the two sides
       agree by destroying the difference. */
    printf("  let s = el.localName;\n");
    printf("  const kids = [];\n");
    printf("  for (const c of kids0) {\n");
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
