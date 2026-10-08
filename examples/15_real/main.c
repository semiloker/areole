/*
 * Ten documents saved from the web, laid out by areole.
 *
 *     example_real                    every document, one line each
 *     example_real --selftest         every document, and what it cost
 *     example_real <file>             one document in a window
 *     example_real <file> --dump      one line per box, for a comparison
 *     example_real <file> --ppm out.ppm    the frame as an image
 *
 * ------------------------------------------------------------------------
 * Why this exists
 *
 * 0.9.0 acceptance criterion 9: ten real-world documents, saved from the web,
 * rendered recognisably against a browser screenshot -- **assessed by eye**,
 * because pixel identity is not achievable and a gate that pretended otherwise
 * would be measuring the wrong thing.
 *
 * The criterion asks for *real* markup on purpose. Real markup is malformed in
 * ways hand-written markup never is: an unclosed `<p>` before a `<div>`, a
 * `</td>` with no cell, a `<font>` inside a `<table>` inside a `<form>` that
 * opened three elements ago. Every corpus in this repository before this one
 * was written by the same person who wrote the engine, and agreed with it by
 * construction. These ten do not know it exists.
 *
 * It is worth saying what happened the last time one real page was tried.
 * `examples/14_interface` is 5,755 bytes of HTML and CSS and it found eleven
 * engine bugs in an afternoon -- more than four releases of hand-declared box
 * trees had. Heights that never swept upward, tracks and rows sized from
 * unwrapped text, `:hover` that never matched a parent, whitespace that never
 * collapsed, fragments that did not follow their box. **Reading documents is
 * the test this engine had been missing**, and ten of them is the cheapest
 * thing in the release.
 *
 * ------------------------------------------------------------------------
 * What was changed in the saved files, and what was not
 *
 * `tools/save_page.py` records it exactly and can be re-run. In short: scripts,
 * external stylesheets and images are removed, and **nothing else is**.
 *
 * Each of those three is removed so the comparison stays honest rather than to
 * make the pictures agree. areole runs no scripts, has no `<link
 * rel=stylesheet>` until 0.9.1 and no images until 0.11.0; leaving them in
 * means the browser lays out a different document and the difference measures
 * the missing feature instead of the layout. What is left is what both engines
 * can be asked the same question about: the document's own `<style>` blocks,
 * its inline styles, its structure, and each engine's default stylesheet.
 *
 * The tag soup is kept exactly as served. That is the part being tested.
 *
 * ------------------------------------------------------------------------
 * The budget, which is the honest part of this file
 *
 * Every other example fixes a small arena to prove the engine fits in one.
 * This one cannot: the largest document here is 596 KB and produces tens of
 * thousands of boxes, and refusing it would be measuring the arena rather than
 * the parser. So the budget is large and stated, and `--selftest` prints what
 * each document actually spent -- which is the number that says whether the
 * Pentium II figures in the release document are reachable, and the reason
 * `overflowed` is checked before anything else.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define WIN_W 1100
#define WIN_H 900

/*
 * Sized for the corpus, from what the corpus actually costs.
 *
 * Measured, not guessed -- the first attempt was a megabyte and five of the ten
 * did not fit. The largest document here is the CSS article at 596 KB, and it
 * comes to **12,316 nodes and 10,004 boxes**; the most text is RFC 2616 at
 * 431 KB, which is almost all of a 514 KB file because a specification is text
 * and nothing else. Nine megabytes and twenty-four thousand boxes clears both with
 * room -- five did not, by one document, and the glyph atlas shares it, and `--selftest` prints the
 * real figures so this comment can be checked rather than believed.
 *
 * Every other example fixes a small arena on purpose, to show the engine fits
 * in one. This one cannot: refusing a real document would be measuring the
 * arena rather than the parser.
 */
static unsigned char g_memory[AR_MEM_DOC(24000, 9u * 1024 * 1024)];

static char   g_file[1024 * 1024];
static ar_u32 g_file_n;

/*
 * A real face, for the same reason the interface example loads one.
 *
 * `font-family` is parsed and ignored -- selecting between families needs a
 * font database, which is 0.2.1's -- so the face is chosen here and every
 * element gets it. Without one the corpus renders in the 8x8 bitmap fallback,
 * and then the comparison is measuring the font rather than the layout: the
 * question this corpus asks is whether the *document* comes out looking like
 * the document, and a page set in a bitmap face cannot answer it either way.
 *
 * A serif face on purpose. Nine of the ten documents are prose or a
 * specification and a browser sets an unstyled page in its serif default, so
 * this is the closer comparison; the sans faces follow it for machines without
 * one.
 */
static const char *const FACES[] = {"C:/Windows/Fonts/georgia.ttf", "C:/Windows/Fonts/times.ttf",
                                    "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
                                    0};

#define ATLAS_BYTES (2u * 1024u * 1024u)
#define MAX_PX      64

static unsigned char g_font[8 * 1024 * 1024];

static int load_face(ar_ctx *c)
{
    ar_i32 i;

    for (i = 0; FACES[i]; ++i)
    {
        FILE  *f = fopen(FACES[i], "rb");
        ar_u32 n;

        if (!f)
        {
            continue;
        }
        n = (ar_u32)fread(g_font, 1, sizeof g_font, f);
        fclose(f);
        if (n && ar_font_load(c, g_font, n, ATLAS_BYTES, MAX_PX))
        {
            return 1;
        }
    }
    return 0;
}

/* The corpus, in the order it is reported. Paths are relative to the tree
   root, which is where every other example is run from. */
static const char *const CORPUS[] = {
    "examples/15_real/w3c-css21-tables.html",  "examples/15_real/whatwg-syntax.html",
    "examples/15_real/weather-gov.html",       "examples/15_real/nasa.html",
    "examples/15_real/mdn-table.html",         "examples/15_real/mdn-flex.html",
    "examples/15_real/wikipedia-ja-html.html", "examples/15_real/wikipedia-status-codes.html",
    "examples/15_real/wikipedia-css.html",     "examples/15_real/rfc2616.html"};

#define CORPUS_N ((int)(sizeof CORPUS / sizeof CORPUS[0]))

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

static void frame(ar_ctx *c, ar_doc *d, const ar_input *in, ar_surface *surface)
{
    ar_frame_begin(c, in);
    ar_dom_build(c, d);
    ar_frame_end(c, surface);
}

/*
 * A fresh context per document.
 *
 * The document arena is spent by the parse and not returned, so ten documents
 * through one context would exhaust it and report a capacity that is an
 * artefact of the harness rather than of the parser. Re-initialising the same
 * buffer is the supported way to say "start again", and it still allocates
 * nothing.
 */
static ar_ctx *open_document(const char *path, ar_doc **out)
{
    ar_ctx *c;

    if (!read_file(path))
    {
        return 0;
    }
    c = ar_init_ex(g_memory, (ar_u32)sizeof g_memory, 4096, 9u * 1024u * 1024u);
    if (!c)
    {
        printf("the arena is too small\n");
        return 0;
    }
    /* Before the first frame: ar_font_load wants the arena to itself. */
    load_face(c);
    ar_ua_stylesheet(c);
    *out = ar_html_parse_into(c, g_file, g_file_n);
    if (!*out)
    {
        printf("%s did not fit\n", path);
        return 0;
    }
    ar_doc_stylesheets(c, *out);
    return c;
}

static const char *base(const char *path)
{
    const char *s = strrchr(path, '/');

    return s ? s + 1 : path;
}

static int report(int quiet)
{
    ar_surface    surface;
    ar_input      in;
    static ar_u32 pixels[WIN_W * WIN_H];
    int           k;
    int           bad = 0;

    memset(&surface, 0, sizeof surface);
    surface.pixels = pixels;
    surface.w = WIN_W;
    surface.h = WIN_H;
    surface.stride = WIN_W;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    if (!quiet)
    {
        printf("%-30s %8s %8s %8s %9s\n", "document", "bytes", "nodes", "boxes", "text");
    }

    for (k = 0; k < CORPUS_N; ++k)
    {
        ar_doc *d = 0;
        ar_ctx *c = open_document(CORPUS[k], &d);

        if (!c)
        {
            ++bad;
            continue;
        }
        frame(c, d, &in, &surface);

        /*
         * Criterion 7 again, and the reason it is checked here rather than
         * assumed: a document that does not fit has to say so. A truncated
         * tree that renders anyway is the failure mode worth catching, because
         * it looks like a page.
         */
        if (d->overflowed)
        {
            printf("FAIL  %-24s did not fit its budget\n", base(CORPUS[k]));
            ++bad;
            continue;
        }
        if (ar_node_count(c) <= 1)
        {
            printf("FAIL  %-24s produced no boxes\n", base(CORPUS[k]));
            ++bad;
            continue;
        }

        printf("%-30s %8lu %8ld %8ld %9lu\n", base(CORPUS[k]), (unsigned long)g_file_n,
               (long)d->node_count, (long)ar_node_count(c), (unsigned long)d->text_used);
    }

    if (!quiet)
    {
        printf("\n%d of %d documents parsed and laid out\n", CORPUS_N - bad, CORPUS_N);
    }
    return bad;
}

static void dump(ar_ctx *c)
{
    ar_i32 i;

    printf("# areole %s viewport %dx%d\n", ar_version(), WIN_W, WIN_H);
    for (i = 0; i < ar_node_count(c); ++i)
    {
        ar_rect r = ar_node_rect(c, i);

        printf("%ld %ld %ld %ld %ld\n", (long)i, (long)r.x, (long)r.y, (long)r.w, (long)r.h);
    }
}

/*
 * The frame as a PPM.
 *
 * "wb", and on this platform that decides whether the image is an image:
 * stdout is in text mode, so every 0x0A byte inside a pixel becomes 0x0D 0x0A
 * and everything after it walks one byte sideways. It does not look like a
 * corrupted file, it looks like the engine rotated the colour channels.
 */
static int ppm(ar_ctx *c, ar_doc *d, const char *path)
{
    FILE         *out;
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

    out = fopen(path, "wb");
    if (!out)
    {
        printf("cannot write %s\n", path);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", WIN_W, WIN_H);
    for (i = 0; i < WIN_W * WIN_H; ++i)
    {
        ar_u32 p = pixels[i];

        fputc((int)((p >> 16) & 0xFFu), out);
        fputc((int)((p >> 8) & 0xFFu), out);
        fputc((int)(p & 0xFFu), out);
    }
    fclose(out);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = 0;
    const char *ppm_path = 0;
    int         want_selftest = 0;
    int         want_dump = 0;
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
        else if (strcmp(argv[k], "--ppm") == 0 && k + 1 < argc)
        {
            ppm_path = argv[++k];
        }
        else
        {
            path = argv[k];
        }
    }

    if (!path)
    {
        int bad = report(want_selftest);

        if (want_selftest)
        {
            printf("%s\n", bad ? "selftest FAILED" : "selftest passed");
        }
        return bad ? 1 : 0;
    }

    {
        ar_doc *d = 0;
        ar_ctx *c = open_document(path, &d);

        if (!c)
        {
            return 2;
        }
        if (ppm_path)
        {
            return ppm(c, d, ppm_path);
        }
        if (want_dump)
        {
            ar_surface    surface;
            ar_input      in;
            static ar_u32 pixels[WIN_W * WIN_H];

            memset(&surface, 0, sizeof surface);
            surface.pixels = pixels;
            surface.w = WIN_W;
            surface.h = WIN_H;
            surface.stride = WIN_W;
            memset(&in, 0, sizeof in);
            in.mouse_x = -1;
            in.mouse_y = -1;
            frame(c, d, &in, &surface);
            dump(c);
            return 0;
        }

        {
            ar_win *win = ar_win_open("areole - a real document", WIN_W, WIN_H);

            if (!win)
            {
                printf("could not open a window\n");
                return 1;
            }
            ar_set_clock(c, ar_time_us);
            printf("areole %s: %s, %ld nodes, %ld boxes\n", ar_version(), base(path),
                   (long)d->node_count, (long)ar_node_count(c));
            while (ar_win_pump(win))
            {
                ar_i32 region;

                frame(c, d, ar_win_input(win), ar_win_surface(win));
                for (region = 0; region < ar_damage_count(c); ++region)
                {
                    ar_win_present(win, ar_damage_rect(c, region));
                }
                ar_frame_presented(c);
            }
            ar_win_close(win);
        }
    }
    return 0;
}
