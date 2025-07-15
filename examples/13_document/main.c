/*
 * areole example 13 - a document
 * SPDX-License-Identifier: MIT
 *
 *     example_document                 the built-in article, in a window
 *     example_document page.html       a file from disk
 *     example_document --selftest      what CI runs: parse, lay out, check
 *     example_document --dump          one line per box, for a comparison
 *     example_document --html          write the sample document out
 *
 * 0.9.0's demonstration. Every other example declares its boxes by calling
 * ar_begin and ar_text; this one reads HTML and does not declare anything.
 *
 * ------------------------------------------------------------------------
 * What it is showing
 *
 * Four calls, and the rest of the engine is the one the other twelve examples
 * use:
 *
 *     ar_ua_stylesheet(c);          the default style for every element
 *     d = ar_html_parse_into(...);  bytes to a document
 *     ar_doc_stylesheets(c, d);     the document's own <style>, in cascade order
 *     ar_dom_build(c, d);           the document to the box tree
 *
 * The third and fourth are the interesting ones. `ar_doc_stylesheets` is a
 * walk rather than a search because tree order *is* cascade order, and
 * `ar_dom_build` goes through ar_begin and ar_text rather than building boxes
 * directly -- so style resolution, the stable keys hover and damage tracking
 * depend on, and the pre-order invariant the layout passes require are the
 * ones every other caller already gets. HTML is a second front end, not a
 * second engine.
 *
 * ------------------------------------------------------------------------
 * Why the sample document is inline, and also a file
 *
 * Inline, so the example runs from a clean checkout with no arguments and no
 * file to find, and so `--selftest` in CI is testing a document that cannot go
 * missing. A path on the command line reads a different one instead.
 *
 * And `document.html` beside this file, written by `--html` and checked in CI
 * against what this source produces -- the same twin discipline the layout
 * corpora use. It exists so the document can be opened in a browser next to
 * the window this draws, which is the only way to look at "it reads HTML" and
 * see whether it is true.
 *
 * Generated rather than written twice, because two copies of a document
 * disagree eventually and the disagreement is invisible until somebody is
 * comparing screenshots and wondering which one is wrong.
 *
 * It is deliberately ordinary: headings, paragraphs with inline markup, a
 * list, a table, a blockquote, an entity or two. Nothing here is chosen to
 * flatter the parser.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define WIN_W 900
#define WIN_H 700

/*
 * Boxes for the tree, and bytes for the document.
 *
 * AR_MEM_DOC is the arena sized for both: the box budget as usual, plus what
 * ar_html_parse_into may spend. A document is per-parse rather than per-frame,
 * so it cannot come out of the box budget -- a caller that asked for two
 * thousand boxes must still get two thousand.
 */
static unsigned char g_memory[AR_MEM_DOC(2048, 256 * 1024)];

/* A file, when one is named. Static because this process allocates once. */
static char   g_file[512 * 1024];
static ar_u32 g_file_n;

/*
 * The sample. Split because C90 guarantees only 509 characters in a string
 * literal and adjacent literals count as one.
 */
static const char *const DOC_HEAD =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<title>A document</title>\n"
    "<style>\n"
    "  body { padding: 32px 40px; background: #fdfcf9; color: #23211d; }\n"
    "  h1   { font-size: 30px; color: #1a1815; }\n"
    "  h2   { font-size: 20px; color: #3a352d; padding-top: 14px; }\n"
    "  p    { padding-bottom: 10px; }\n"
    "</style>\n"
    "</head>\n";

static const char *const DOC_BODY =
    "<body>\n"
    "<h1>Reading a document</h1>\n"
    "<p>This page was not declared by any C in this example. It is "
    "<b>parsed</b> from HTML, styled by the user-agent stylesheet and the "
    "<i>&lt;style&gt;</i> element above it, and laid out by the same engine "
    "every other example uses.</p>\n"
    "<h2>What the parser does with it</h2>\n"
    "<p>Optional end tags are supplied, misnested markup is repaired, and "
    "character references such as &amp; and &eacute; and &mdash; become the "
    "characters they name.</p>\n";

static const char *const DOC_LIST =
    "<ul>\n"
    "  <li>A list whose items never close\n"
    "  <li>because the parser closes them\n"
    "  <li>which is what the specification asks for\n"
    "</ul>\n"
    "<h2>A table</h2>\n"
    "<table>\n"
    "  <tr><td>rows<td>and cells\n"
    "  <tr><td>with no<td>closing tags\n"
    "</table>\n"
    "<blockquote><p>A quotation, indented by the user-agent stylesheet and by "
    "nothing in this file.</p></blockquote>\n"
    "</body>\n"
    "</html>\n";

static char   g_doc[4096];
static ar_u32 g_doc_n;

static void build_sample(void)
{
    g_doc_n = 0;
    g_doc[0] = 0;
    strcat(g_doc, DOC_HEAD);
    strcat(g_doc, DOC_BODY);
    strcat(g_doc, DOC_LIST);
    g_doc_n = (ar_u32)strlen(g_doc);
}

static int read_file(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
    {
        printf("cannot open %s\n", path);
        return 0;
    }
    g_file_n = (ar_u32)fread(g_file, 1, sizeof g_file - 1u, f);
    fclose(f);
    g_file[g_file_n] = 0;
    return 1;
}

/*
 * One frame.
 *
 * The document is parsed once at startup and rebuilt into boxes every frame,
 * which is the same shape every other example has: the tree is declared fresh
 * and damage tracking decides what actually gets painted.
 */
static void frame(ar_ctx *c, const ar_doc *d, const ar_input *in, ar_surface *surface)
{
    ar_frame_begin(c, in);
    ar_dom_build(c, d);
    ar_frame_end(c, surface);
}

static int selftest(ar_ctx *c, ar_doc *d)
{
    ar_surface    surface;
    ar_input      in;
    static ar_u32 pixels[WIN_W * WIN_H];
    ar_i32        i;
    ar_i32        boxes;
    ar_i32        with_text = 0;
    ar_i32        wide = 0;
    int           bad = 0;

    memset(&surface, 0, sizeof surface);
    surface.pixels = pixels;
    surface.w = WIN_W;
    surface.h = WIN_H;
    surface.stride = WIN_W;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    frame(c, d, &in, &surface);
    boxes = ar_node_count(c);

    /* The document parsed whole. A truncated one says so rather than
       pretending, which is 0.9.0 acceptance criterion 7. */
    if (d->overflowed)
    {
        printf("FAIL  the document did not fit its budget\n");
        ++bad;
    }
    else
    {
        printf("ok    parsed whole: %ld nodes, %ld attributes, %lu bytes of text\n",
               (long)d->node_count, (long)d->attr_count, (unsigned long)d->text_used);
    }

    if (boxes < 20)
    {
        printf("FAIL  only %ld boxes; the document should be larger than that\n", (long)boxes);
        ++bad;
    }
    else
    {
        printf("ok    %ld boxes from %lu bytes of HTML\n", (long)boxes, (unsigned long)g_doc_n);
    }

    /*
     * Something was laid out, and laid out *across* the window rather than
     * collapsed into a corner. A parser that produced a tree of empty boxes
     * would pass a count check and fail this one.
     */
    for (i = 0; i < boxes; ++i)
    {
        ar_rect r = ar_node_rect(c, i);

        if (ar_node_text(c, i)[0] != 0)
        {
            ++with_text;
        }
        if (r.w > WIN_W / 2)
        {
            ++wide;
        }
    }
    if (with_text < 10)
    {
        printf("FAIL  only %ld boxes carry text\n", (long)with_text);
        ++bad;
    }
    else
    {
        printf("ok    %ld boxes carry text\n", (long)with_text);
    }
    if (wide < 3)
    {
        printf("FAIL  only %ld boxes are wider than half the window\n", (long)wide);
        ++bad;
    }
    else
    {
        printf("ok    %ld boxes fill the width, so the block layout ran\n", (long)wide);
    }

    /* Two identical frames are identical: the damage tracking that every other
       example relies on is not disturbed by rebuilding from a document. */
    {
        ar_u32 before = 0;
        ar_u32 after = 0;

        ar_frame_presented(c);
        frame(c, d, &in, &surface);
        for (i = 0; i < WIN_W * WIN_H; i += 977)
        {
            before += pixels[i];
        }
        ar_frame_presented(c);
        frame(c, d, &in, &surface);
        for (i = 0; i < WIN_W * WIN_H; i += 977)
        {
            after += pixels[i];
        }
        if (before != after)
        {
            printf("FAIL  two identical frames painted differently\n");
            ++bad;
        }
        else
        {
            printf("ok    two identical frames are identical\n");
        }
    }

    printf("%s\n", bad ? "selftest FAILED" : "selftest passed");
    return bad ? 1 : 0;
}

static void dump(ar_ctx *c, ar_doc *d)
{
    ar_surface    surface;
    ar_input      in;
    static ar_u32 pixels[WIN_W * WIN_H];
    ar_i32        i;

    memset(&surface, 0, sizeof surface);
    surface.pixels = pixels;
    surface.w = WIN_W;
    surface.h = WIN_H;
    surface.stride = WIN_W;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    frame(c, d, &in, &surface);

    printf("# areole %s viewport %dx%d\n", ar_version(), WIN_W, WIN_H);
    for (i = 0; i < ar_node_count(c); ++i)
    {
        ar_rect r = ar_node_rect(c, i);

        printf("%ld %ld %ld %ld %ld\n", (long)i, (long)r.x, (long)r.y, (long)r.w, (long)r.h);
    }
}

int main(int argc, char **argv)
{
    ar_ctx     *c;
    ar_doc     *d;
    const char *bytes;
    ar_u32      len;
    const char *path = 0;
    int         want_selftest = 0;
    int         want_dump = 0;
    int         want_html = 0;
    int         k;

    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--selftest") == 0)
        {
            want_selftest = 1;
        }
        else if (strcmp(argv[k], "--dump") == 0)
        {
            want_dump = 1;
        }
        else if (strcmp(argv[k], "--html") == 0)
        {
            want_html = 1;
        }
        else
        {
            path = argv[k];
        }
    }

    build_sample();
    if (want_html)
    {
        /* Byte for byte what the parser is handed, so the file and the test
           cannot drift. */
        fwrite(g_doc, 1, g_doc_n, stdout);
        return 0;
    }
    bytes = g_doc;
    len = g_doc_n;
    if (path)
    {
        if (!read_file(path))
        {
            return 2;
        }
        bytes = g_file;
        len = g_file_n;
    }

    /*
     * ar_init_ex rather than ar_init: the second figure is what the parser may
     * spend, fixed here and never grown. Nothing allocates after this line.
     */
    c = ar_init_ex(g_memory, (ar_u32)sizeof g_memory, 256, 256 * 1024);
    if (!c)
    {
        printf("the arena is too small\n");
        return 2;
    }

    /* Before the first frame, both of them: a frame reserves the whole box
       budget from the other end of the arena and does not release it until the
       next ar_frame_begin. */
    ar_ua_stylesheet(c);
    d = ar_html_parse_into(c, bytes, len);
    if (!d)
    {
        printf("the document did not fit\n");
        return 2;
    }
    ar_doc_stylesheets(c, d);

    if (want_selftest)
    {
        return selftest(c, d);
    }
    if (want_dump)
    {
        dump(c, d);
        return 0;
    }

    {
        ar_win *win = ar_win_open("areole - a document", WIN_W, WIN_H);

        if (!win)
        {
            printf("could not open a window\n");
            return 1;
        }
        ar_set_clock(c, ar_time_us);
        printf("areole %s: %ld nodes, %ld attributes, %lu bytes of text\n", ar_version(),
               (long)d->node_count, (long)d->attr_count, (unsigned long)d->text_used);

        while (ar_win_pump(win))
        {
            ar_i32 region;

            frame(c, d, ar_win_input(win), ar_win_surface(win));

            /*
             * Present the damaged regions rather than the window. The document
             * does not change between frames, so after the first one this is
             * usually nothing at all -- which is the whole reason damage
             * tracking exists and is as true for a parsed document as for a
             * hand-declared tree.
             */
            for (region = 0; region < ar_damage_count(c); ++region)
            {
                ar_win_present(win, ar_damage_rect(c, region));
            }
            ar_frame_presented(c);

            if (ar_needs_redraw(c))
            {
                ar_win_wake(win);
            }
        }
        ar_win_close(win);
    }
    return 0;
}
