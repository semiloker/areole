/*
 * areole - the public header, used the way an embedder uses it.
 * SPDX-License-Identifier: MIT
 *
 * This file includes areole.h and nothing else, and its CMake target does not
 * have src/ on the include path. That is the whole test: if any of it stops
 * compiling, something that an application needs has drifted back out of the
 * installed header and into the private one.
 *
 * It exists because that had happened. 0.9.0 shipped five commits of parser --
 * tokenizer, tree construction, encoding, a user-agent stylesheet, a document
 * that lays out -- with every one of its types and functions declared in
 * src/ar_html.h, which is not installed. Everything worked; the tests passed;
 * the corpora agreed with a browser. Nobody outside the repository could parse
 * a document, because the only way to name ar_doc was to reach into src/.
 *
 * ar_test could not have caught it: ar_test includes src/ar_internal.h on
 * purpose, so it sees everything whether or not it is public. A gate for "is
 * this reachable from outside" has to be compiled from outside.
 */
#include "areole.h"

#include <stdio.h>
#include <string.h>

static char        g_mem[AR_MEM_DOC(2000, 64 * 1024)];
static ar_dom_node g_nodes[512];
static ar_attr     g_attrs[128];
static char        g_text[8192];
static char        g_scratch[512];

int main(void)
{
    static const char SRC[] = "<!DOCTYPE html><html><head><style>p{width:120px;}</style></head>"
                              "<body><p>caf&eacute; &amp; cr&egrave;me</p></body></html>";
    ar_doc            doc;
    ar_ctx           *c;
    ar_surface        surf;
    ar_input          in;
    ar_doc           *d2;
    ar_encoding       enc;
    ar_u32            skip = 0;
    ar_i32            root;
    static ar_u32     pixels[64 * 64];

    /* 1. The caller-storage form. */
    memset(&doc, 0, sizeof doc);
    doc.nodes = g_nodes;
    doc.node_cap = (ar_i32)(sizeof g_nodes / sizeof g_nodes[0]);
    doc.attrs = g_attrs;
    doc.attr_cap = (ar_i32)(sizeof g_attrs / sizeof g_attrs[0]);
    doc.text = g_text;
    doc.text_cap = (ar_u32)sizeof g_text;
    if (!ar_html_parse(&doc, SRC, (ar_u32)(sizeof SRC - 1), g_scratch, (ar_u32)sizeof g_scratch))
    {
        printf("FAIL: parse overflowed\n");
        return 1;
    }
    root = ar_dom_root(&doc);
    if (root < 0 || !ar_span_is(doc.nodes[root].name, "html"))
    {
        printf("FAIL: no html element\n");
        return 1;
    }
    if (ar_dom_child_element(&doc, root, "body") < 0)
    {
        printf("FAIL: no body\n");
        return 1;
    }
    if (doc.quirks != AR_QUIRKS_NO)
    {
        printf("FAIL: a full doctype should not be quirks\n");
        return 1;
    }

    /* 2. The encoding functions. */
    enc = ar_encoding_sniff("\357\273\277<p>x", 8, &skip);
    if (enc != AR_ENC_UTF8 || skip != 3)
    {
        printf("FAIL: BOM sniffing\n");
        return 1;
    }

    /* 3. The arena form, plus the whole pipeline: sheet, document, boxes. */
    c = ar_init_ex(g_mem, (ar_u32)sizeof g_mem, 256, 64 * 1024);
    if (!c)
    {
        printf("FAIL: ar_init_ex\n");
        return 1;
    }
    ar_ua_stylesheet(c);
    d2 = ar_html_parse_into(c, SRC, (ar_u32)(sizeof SRC - 1));
    if (!d2 || d2->overflowed)
    {
        printf("FAIL: ar_html_parse_into\n");
        return 1;
    }
    if (ar_doc_stylesheets(c, d2) != 1)
    {
        printf("FAIL: the document's own <style> was not collected\n");
        return 1;
    }

    memset(&surf, 0, sizeof surf);
    surf.pixels = pixels;
    surf.w = 64;
    surf.h = 64;
    surf.stride = 64;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    ar_frame_begin(c, &in);
    ar_dom_build(c, d2);
    ar_frame_end(c, &surf);
    ar_frame_presented(c);

    if (ar_node_count(c) < 4)
    {
        printf("FAIL: the document produced no boxes\n");
        return 1;
    }

    printf("ok: %ld dom nodes, %ld boxes, areole %s\n", (long)doc.node_count,
           (long)ar_node_count(c), ar_version());
    return 0;
}
