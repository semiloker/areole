/*
 * areole - the shared half of the computed-value corpora.
 * SPDX-License-Identifier: MIT
 *
 * Two corpora ask the same kind of question and so share everything but their
 * cases: `ar_hints` asks what HTML's legacy attributes compute to, and
 * `ar_elements` asks what the user-agent stylesheet computes to for every
 * element. Both are answered by `getComputedStyle` on the other side.
 *
 * A file that includes this defines CASES and CASE_N, and calls
 * ar__corpus_main. Everything here is static, so each of the two gets its own
 * copy and neither can reach into the other's.
 *
 * ------------------------------------------------------------------------
 * Computed values, not rectangles
 *
 * A rectangle answers a different question. Two engines can agree on every box
 * on a page and disagree about every colour on it, and a corpus about defaults
 * is half colours and font weights. It also avoids a comparison this engine
 * cannot win: a border in areole reserves no space (see ar_used_size) and in a
 * browser it reserves as much as `border-style` allows, so a bordered box is
 * wrong in every rectangle for a reason that has nothing to do with the thing
 * under test.
 *
 * `width` and `height` are the exception and are read from the rectangle,
 * because that is what `getComputedStyle` returns for them -- a used value.
 * The padding is taken back off, since a browser reports the content box and
 * areole's rectangle is the border box.
 *
 * ------------------------------------------------------------------------
 * Why the twin is generated
 *
 * Every hand-written twin in this tree has drifted from the C source it was
 * meant to mirror, and the corpus went on reporting agreement about cases that
 * were no longer the same cases. `--html` writes it, and CI diffs the file
 * against what `--html` produces, so it cannot.
 */
#ifndef AR_COMPUTED_H
#define AR_COMPUTED_H

#include "ar_internal.h"
#include "ar_html.h"
#include "ar_node.h"
#include "ar_css.h"

#include <stdio.h>
#include <string.h>

#define CASE_W 420

/*
 * One case: a fragment, and which properties of `#x` inside it to report.
 *
 * `#x` rather than "the first element" because half of HTML's presentational
 * attributes are written on a table and land on a cell, so the element under
 * test is often not the one carrying the thing being tested -- which is the
 * part of a mapping most likely to be wrong, and was.
 *
 * `props` is a space-separated list, because an element's defaults are a
 * handful of properties at once and asking for them one case at a time would
 * mean parsing the same fragment seven times.
 */
typedef struct
{
    const char *name;
    const char *css;  /* an author stylesheet, or "" */
    const char *html; /* the fragment, with one element marked id="x" */
    const char *props;
} ar__case;

/*
 * Whether this corpus's documents are in quirks mode.
 *
 * A mode belongs to a document and the twin is one document, so a corpus is
 * all of one or all of the other. That is why the quirks cases are their own
 * binary and their own twin rather than a flag on a case: forty cases sharing
 * a page share its doctype, and there is no way to give one of them a
 * different one short of an iframe, which loads after the dump has run.
 */
static int ar__quirks_mode = 0;

static unsigned char g_memory[AR_MEM_DOC(512, 64u * 1024u)];
static ar_u32        g_pixels[CASE_W * 200];
static char          g_html[8192];
static ar_u32        g_html_n;

/*
 * The fragment, wrapped so the parser has a document to build.
 *
 * `body` is given the case width and no margin, because the twin's container
 * is a plain div and a percentage has to resolve against the same number on
 * both sides.
 */
static void ar__build_source(const ar__case *k)
{
    g_html_n = 0;
    g_html[0] = 0;
    /* No doctype at all is what puts a document in quirks mode -- the mode is
       read from the doctype and its absence is the loudest thing it can say. */
    strcat(g_html, ar__quirks_mode ? "<html><head><style>" : "<!doctype html><html><head><style>");
    strcat(g_html, "body { width:");
    sprintf(g_html + strlen(g_html), "%dpx", CASE_W);
    strcat(g_html, "; margin:0px; padding:0px; }");
    strcat(g_html, k->css);
    strcat(g_html, "</style></head><body>");
    strcat(g_html, k->html);
    strcat(g_html, "</body></html>");
    g_html_n = (ar_u32)strlen(g_html);
}

/* The box whose id is `x`. Compared by hash, which is what the selector
   machinery stores -- there is no name on a box to compare against. */
static ar_i32 ar__find_x(const ar_ctx *c)
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
 * `getComputedStyle` says `rgb(255, 0, 0)`, and `rgba(0, 0, 0, 0)` for
 * transparent. Both sides have to spell it the same way or every colour case
 * fails for punctuation.
 */
static void ar__put_color(char *out, ar_u32 v)
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
        sprintf(out, "rgba(%lu, %lu, %lu, 0)", (unsigned long)r, (unsigned long)g,
                (unsigned long)b);
        return;
    }
    sprintf(out, "rgba(%lu, %lu, %lu, %lu)", (unsigned long)r, (unsigned long)g, (unsigned long)b,
            (unsigned long)a);
}

enum
{
    AR__V_LEN = 0, /* a length, printed with px */
    AR__V_COLOR,
    AR__V_DISPLAY,
    AR__V_ALIGN,
    AR__V_WEIGHT,
    AR__V_SLANT,
    AR__V_USED_W,
    AR__V_USED_H
};

static const struct
{
    const char *name;
    ar_i32      prop;
    int         kind;
} AR__PROPS[] = {{"background-color", AR_P_BACKGROUND, AR__V_COLOR},
                 {"color", AR_P_COLOR, AR__V_COLOR},
                 {"width", AR_P_WIDTH, AR__V_USED_W},
                 {"height", AR_P_HEIGHT, AR__V_USED_H},
                 {"border-spacing", AR_P_BORDER_SPACING, AR__V_LEN},
                 {"padding-top", AR_P_PAD_TOP, AR__V_LEN},
                 {"padding-left", AR_P_PAD_LEFT, AR__V_LEN},
                 {"margin-left", AR_P_MARGIN_LEFT, AR__V_LEN},
                 {"margin-right", AR_P_MARGIN_RIGHT, AR__V_LEN},
                 {"margin-top", AR_P_MARGIN_TOP, AR__V_LEN},
                 {"margin-bottom", AR_P_MARGIN_BOTTOM, AR__V_LEN},
                 {"font-size", AR_P_FONT_SIZE, AR__V_LEN},
                 {"border-top-width", AR_P_BORDER_WIDTH, AR__V_LEN},
                 {"text-align", AR_P_TEXT_ALIGN, AR__V_ALIGN},
                 {"font-weight", AR_P_FONT_WEIGHT, AR__V_WEIGHT},
                 {"font-style", AR_P_FONT_STYLE, AR__V_SLANT},
                 {"display", AR_P_DISPLAY, AR__V_DISPLAY}};

#define AR__PROPS_N ((int)(sizeof AR__PROPS / sizeof AR__PROPS[0]))

static const char *ar__align_name(ar_i32 v)
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

static const char *ar__display_name(ar_i32 v)
{
    switch (v)
    {
    case AR_DISPLAY_NONE:
        return "none";
    case AR_DISPLAY_BLOCK:
        return "block";
    case AR_DISPLAY_FLEX:
        return "flex";
    case AR_DISPLAY_INLINE_BLOCK:
        return "inline-block";
    case AR_DISPLAY_INLINE:
        return "inline";
    case AR_DISPLAY_TABLE:
        return "table";
    case AR_DISPLAY_TABLE_ROW_GROUP:
        return "table-row-group";
    case AR_DISPLAY_TABLE_HEADER_GROUP:
        return "table-header-group";
    case AR_DISPLAY_TABLE_FOOTER_GROUP:
        return "table-footer-group";
    case AR_DISPLAY_TABLE_ROW:
        return "table-row";
    case AR_DISPLAY_TABLE_CELL:
        return "table-cell";
    case AR_DISPLAY_TABLE_COLUMN_GROUP:
        return "table-column-group";
    case AR_DISPLAY_TABLE_COLUMN:
        return "table-column";
    case AR_DISPLAY_TABLE_CAPTION:
        return "table-caption";
    case AR_DISPLAY_GRID:
        return "grid";
    case AR_DISPLAY_CONTENTS:
        return "contents";
    case AR_DISPLAY_LIST_ITEM:
        return "list-item";
    default:
        return "?";
    }
}

/* One property, in the spelling a browser uses. */
static void ar__value_of(const ar_ctx *c, ar_i32 node, const char *prop, char *out)
{
    const ar_style *st = &c->nodes[node].style;
    int             i;

    out[0] = 0;
    for (i = 0; i < AR__PROPS_N; ++i)
    {
        if (strcmp(AR__PROPS[i].name, prop) != 0)
        {
            continue;
        }
        switch (AR__PROPS[i].kind)
        {
        case AR__V_COLOR:
            ar__put_color(out, (ar_u32)AR_WIDE(st, AR__PROPS[i].prop));
            return;
        case AR__V_ALIGN:
            strcpy(out, ar__align_name(st->v[AR_P_TEXT_ALIGN]));
            return;
        case AR__V_DISPLAY:
            strcpy(out, ar__display_name(st->v[AR_P_DISPLAY]));
            return;
        case AR__V_WEIGHT:
            sprintf(out, "%ld", (long)st->v[AR_P_FONT_WEIGHT]);
            return;
        case AR__V_SLANT:
            strcpy(out, st->v[AR_P_FONT_STYLE] == AR_FONT_STYLE_ITALIC ? "italic" : "normal");
            return;
        /*
         * The content box, because that is what `getComputedStyle` returns for
         * `width` -- a used value with the padding taken back off. areole's
         * rectangle is the border box, so the two agreed on every element with
         * no padding and differed by exactly two on the one that had it.
         *
         * Nothing is subtracted for the border: a border in areole reserves no
         * space and is not in the rectangle to begin with.
         */
        case AR__V_USED_W:
            sprintf(out, "%ldpx",
                    (long)(ar_node_rect(c, node).w - st->v[AR_P_PAD_LEFT] - st->v[AR_P_PAD_RIGHT]));
            return;
        case AR__V_USED_H:
            sprintf(out, "%ldpx",
                    (long)(ar_node_rect(c, node).h - st->v[AR_P_PAD_TOP] - st->v[AR_P_PAD_BOTTOM]));
            return;
        default:
            /* `auto` is a value and not a number. A browser says `auto` for a
               margin that has none, and printing the zero underneath it would
               make two different things look like one. */
            if (st->unit[AR__PROPS[i].prop] == AR_UNIT_AUTO)
            {
                strcpy(out, "auto");
                return;
            }
            sprintf(out, "%ldpx", (long)st->v[AR__PROPS[i].prop]);
            return;
        }
    }
    strcpy(out, "?");
}

/* The next space-separated word of `props`, or 0 when there are none left. */
static const char *ar__next_prop(const char *at, char *out)
{
    int n = 0;

    while (*at == ' ')
    {
        ++at;
    }
    if (!*at)
    {
        return 0;
    }
    while (*at && *at != ' ' && n < 40)
    {
        out[n++] = *at++;
    }
    out[n] = 0;
    return at;
}

static int ar__dump_one(const ar__case *k)
{
    ar_surface  s;
    ar_input    in;
    ar_ctx     *c;
    ar_doc     *d;
    ar_i32      x;
    const char *at;
    char        prop[48];
    char        value[64];

    ar__build_source(k);

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
    x = ar__find_x(c);
    if (x < 0)
    {
        printf("%s ?\n", k->props);
        return 1;
    }
    at = k->props;
    while ((at = ar__next_prop(at, prop)) != 0)
    {
        ar__value_of(c, x, prop, value);
        printf("%s %s\n", prop, value);
    }
    return 0;
}

static int ar__dump(const ar__case *cases, int n)
{
    int k, bad = 0;

    printf("# areole %s\n", ar_version());
    for (k = 0; k < n; ++k)
    {
        bad += ar__dump_one(&cases[k]);
    }
    return bad;
}

/* ------------------------------------------------------------------------
 * The twin
 * ------------------------------------------------------------------------ */

static void ar__put_escaped(const char *s)
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

static void ar__emit_html(const ar__case *cases, int n, const char *title, const char *binary)
{
    int k;

    if (!ar__quirks_mode)
    {
        printf("<!DOCTYPE html>\n");
    }
    printf("<meta charset=\"utf-8\">\n");
    printf("<title>areole %s \xe2\x80\x94 the browser twin</title>\n\n", title);
    printf("<!--\n  GENERATED by %s --html. Do not edit; CI diffs it.\n\n", binary);
    printf("      python tools/compare_computed.py --run ./build/%s.exe \\\n", binary);
    printf("             tests/%s.html\n\n", binary + 3);
    printf("  Each case is a container the size of areole's body, holding the same\n");
    printf("  fragment, with the same author stylesheet scoped to it. The reported\n");
    printf("  values are getComputedStyle of the element marked id=x.\n");
    if (ar__quirks_mode)
    {
        printf("\n  NO DOCTYPE, on purpose: that is what puts this page in quirks mode,\n");
        printf("  which is the whole of what it is testing. document.compatMode has to\n");
        printf("  say BackCompat or every case here is measuring the wrong thing, so\n");
        printf("  the dump says which mode it was read in.\n");
    }
    printf("-->\n\n");
    printf("<style>\n");
    /* No font-family, and that is not cosmetic. A browser's `medium` is
       sixteen pixels for a proportional face and thirteen for a monospace one,
       and quirks mode resets a table's font to `medium` -- so a twin styled in
       monospace reported a thirteen-pixel table and areole's sixteen looked
       like the bug. The page under test has no font of its own on either
       side. */
    printf("  body { margin:0; }\n");
    printf("  .case { width:%dpx; margin:0; padding:0; }\n", CASE_W);
    printf("  textarea#out { display:block; width:100%%; height:240px; }\n");
    printf("</style>\n\n<div id=\"cases\"></div>\n<textarea id=\"out\" readonly></textarea>\n\n");

    printf("<script>\nconst CASES = [\n");
    for (k = 0; k < n; ++k)
    {
        printf("  ['");
        ar__put_escaped(cases[k].name);
        printf("', '");
        ar__put_escaped(cases[k].css);
        printf("', '");
        ar__put_escaped(cases[k].html);
        printf("', '");
        ar__put_escaped(cases[k].props);
        printf("'],\n");
    }
    printf("];\n\n");

    printf("const host = document.getElementById('cases');\n");
    printf("const out = [];\n");
    printf("for (const [name, css, html, props] of CASES) {\n");
    printf("  const box = document.createElement('div');\n");
    printf("  box.className = 'case';\n");
    printf("  box.dataset.page = name;\n");
    printf("  if (css) {\n");
    printf("    /* Scoped, so every case's stylesheet can share one page. */\n");
    printf("    const st = document.createElement('style');\n");
    printf("    st.textContent = css.replace(/(^|\\})\\s*([^{}]+)\\{/g,\n");
    printf("      (m, brace, sel) => brace + sel.split(',')\n");
    printf("        .map(s => '[data-page=\"' + name + '\"] ' + s.trim()).join(',') + '{');\n");
    printf("    box.appendChild(st);\n");
    printf("  }\n");
    printf("  box.insertAdjacentHTML('beforeend', html);\n");
    printf("  host.appendChild(box);\n");
    printf("}\n\n");
    printf("for (const [name, css, html, props] of CASES) {\n");
    printf("  const box = host.querySelector('[data-page=\"' + name + '\"]');\n");
    printf("  const el = box.querySelector('#x');\n");
    printf("  out.push('# case ' + name);\n");
    printf("  for (const prop of props.split(/\\s+/).filter(Boolean)) {\n");
    printf("    out.push(prop + ' ' +\n");
    printf("      (el ? getComputedStyle(el).getPropertyValue(prop) : '?'));\n");
    printf("  }\n");
    printf("}\n\n");
    if (ar__quirks_mode)
    {
        /* Reported rather than assumed. A twin that quietly slipped into
           standards mode would agree with areole about nothing and say
           nothing about why. */
        printf("out.unshift('# mode ' + document.compatMode);\n");
    }
    printf("const t = document.getElementById('out');\n");
    printf("t.value = out.join('\\n');\n");
    printf("t.textContent = out.join('\\n');\n");
    printf("</script>\n");
}

static int ar__corpus_main(int argc, char **argv, const ar__case *cases, int n, const char *title,
                           const char *binary, int quirks)
{
    int k;

    ar__quirks_mode = quirks;
    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--html") == 0)
        {
            ar__emit_html(cases, n, title, binary);
            return 0;
        }
    }
    return ar__dump(cases, n) ? 1 : 0;
}

#endif /* AR_COMPUTED_H */
