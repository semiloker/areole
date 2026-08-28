/*
 * areole example 14 - an interface, written in HTML and CSS
 * SPDX-License-Identifier: MIT
 *
 *     example_interface                 the page, in a window
 *     example_interface page.html       a file from disk
 *     example_interface --selftest      what CI runs: parse, lay out, check
 *     example_interface --dump          one line per box, for a comparison
 *     example_interface --ppm out.ppm   the frame as an image, no window needed
 *     example_interface --html          write the page out
 *
 * Example 13 shows that a *document* renders. This one shows that an
 * *interface* does: a header bar, a sidebar, a row of figures, a deck of
 * cards whose footers line up, and a table -- with no C in this file
 * declaring a single box or a single coordinate.
 *
 * ------------------------------------------------------------------------
 * What makes it look like something, given what is missing
 *
 * There is no `font-family`, no `font-weight`, no `line-height`, no
 * `text-decoration`, no shadow, no gradient, and `border-radius` parses and
 * is then ignored. A page cannot be made handsome here by decorating it.
 *
 * What is left is what actually carries a layout: a grid, a type scale, one
 * accent colour, and space. That is the honest demonstration -- if the page
 * reads well it is because the *layout* is right, and the layout is the part
 * areole is claiming to have.
 *
 * Three deliberate choices follow from the gaps:
 *
 *   - Rules between sections are one-pixel boxes with a background, not
 *     borders. `border-width` is a single value for all four sides here, so
 *     an underline drawn as a border is a rectangle.
 *   - Colours are hex. There are no colour keywords in the parser -- `white`
 *     is not a colour, `#ffffff` is -- and that is the kind of thing a
 *     stylesheet fails silently on.
 *   - No selector list is longer than four. `AR_MAX_SEL_LIST` is 4 and a
 *     longer list is refused *whole*, without a diagnostic anyone sees. The
 *     first user-agent stylesheet written for this engine had lists of
 *     eighteen and twenty-four, both silently discarded, and the page laid
 *     out as flex because the rules that said otherwise had vanished.
 *
 * That last one is why `--selftest` asserts `ar_stylesheet_errors() == 0`
 * before it asserts anything about the layout. A stylesheet that fails does
 * not crash; it quietly stops.
 *
 * ------------------------------------------------------------------------
 * What the layout is actually exercising
 *
 * Every one of these is a release, and every one of them is reached from
 * markup rather than from a C call:
 *
 *   flex             0.8.0   the header bar and the sidebar
 *   grid             0.8.0   the shell, the figures, the card deck
 *   subgrid          0.8.2   the card footers, which line up across the row
 *                            although the bodies above them do not match
 *   aspect-ratio     0.8.1   the figure tiles
 *   tables           0.7.0   the run table, with no closing tags in the source
 *   scroll container 0.6.1   the content column
 *   position         0.6.0   the badge on the third card
 *   :hover           0.4.0   the navigation and the table rows
 *   :nth-child       0.4.0   the table striping
 *
 * The card deck is the one worth looking at twice. The three bodies are
 * different lengths, and the three footers sit on the same line anyway --
 * that is `grid-template-rows: subgrid`, and it is the layout problem card
 * decks have had on the web for twenty years.
 *
 * ------------------------------------------------------------------------
 * The twin
 *
 * `page.html` beside this file is written by `--html` and diffed in CI
 * against what this source produces, so the file you can open in a browser
 * and the bytes this example parses cannot drift apart. Every hand-written
 * twin in this repository has drifted; the generated ones cannot.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define WIN_W 1100
#define WIN_H 900

static unsigned char g_memory[AR_MEM_DOC(3072, 256 * 1024)];

static char   g_file[512 * 1024];
static ar_u32 g_file_n;

/*
 * A real face, because the type scale is most of the design.
 *
 * Without one the built-in 8x8 bitmap draws everything at eight pixels and
 * `font-size` moves the line boxes around text that never changes size -- so a
 * page whose hierarchy is entirely type reads as one flat block. The first
 * name that loads wins; if none of them is installed the page still lays out,
 * still passes its checks, and looks like a wireframe.
 *
 * `font-family` is parsed and ignored by the engine, so the face is chosen
 * here rather than in the stylesheet. That is a real limitation and this is
 * where it shows.
 */
#define ATLAS_BYTES (512u * 1024u)
#define MAX_PX      48

static const char *const FACES[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/calibri.ttf",
                                    "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/tahoma.ttf", 0};

static unsigned char g_font[4 * 1024 * 1024];

/*
 * The page, in parts.
 *
 * C90 guarantees only 509 characters in a string literal and adjacent
 * literals count as one, so this is an array rather than one constant. The
 * same shape src/ar_ua_css.c uses, for the same reason.
 */
static const char *const PAGE[] = {
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<title>areole &mdash; an interface</title>\n"
    "<style>\n",

    /* The ground. `body` has an 8px margin from the user-agent stylesheet and
       a full-bleed header needs it gone. */
    "  html, body { background: #f4f2ee; color: #14161a; }\n"
    "  body { margin: 0px; font-size: 15px; box-sizing: border-box; }\n"
    "  .rule { height: 1px; background: #e2ded7; }\n",

    /* The header bar. */
    "  .bar { display: flex; align-items: center; padding: 16px 28px;\n"
    "         justify-content: space-between; background: #ffffff; }\n"
    "  .brand { display: flex; align-items: center; gap: 11px; }\n"
    "  .mark { width: 20px; height: 20px; background: #c2410c; }\n"
    "  .name { font-size: 17px; }\n",

    "  .nav { display: flex; gap: 2px; }\n"
    "  .nav a { display: inline-block; padding: 7px 13px; color: #6b6862; }\n"
    "  .nav a:hover { background: #fdf0e8; color: #c2410c; }\n"
    "  .nav a.on { color: #14161a; background: #f4f2ee; }\n",

    /*
     * The shell: a fixed sidebar and a content column that scrolls.
     *
     * The row is stated as well as the height. Without it the single implicit
     * row sizes to its content, the content is taller than the shell, and both
     * columns overflow instead of the right one scrolling -- a grid row is
     * `auto` until somebody says otherwise, and `height` on the container is
     * not somebody saying otherwise.
     */
    "  .shell { display: grid; grid-template-columns: 232px 1fr;\n"
    "           grid-template-rows: 833px; height: 833px; }\n"
    "  .side { background: #ffffff; padding: 22px 16px;\n"
    "          display: flex; flex-direction: column; gap: 3px; }\n"
    "  .side h4 { font-size: 11px; color: #9c968c; padding: 8px 10px 4px; }\n",

    "  .side a { display: block; padding: 8px 10px; color: #4a4740; }\n"
    "  .side a:hover { background: #f4f2ee; color: #14161a; }\n"
    "  .side a.on { background: #fdf0e8; color: #c2410c; }\n"
    "  .main { overflow-y: auto; padding: 26px 34px 40px; }\n",

    /* Type. */
    "  h1 { font-size: 30px; margin: 0px 0px 8px; }\n"
    "  .lede { color: #6b6862; margin: 0px 0px 20px; }\n"
    "  h2 { font-size: 13px; color: #9c968c; margin: 22px 0px 10px; }\n",

    /* Four figures. `aspect-ratio` gives the tiles their shape rather than a
       height, so they keep it as the column widths change. */
    "  .figures { display: grid; grid-template-columns: repeat(4, 1fr);\n"
    "             gap: 14px; }\n"
    "  .fig { background: #ffffff; padding: 14px 16px 0px;\n"
    "         aspect-ratio: 16 / 9; }\n"
    "  .fig .k { font-size: 11px; color: #9c968c; }\n"
    "  .fig .v { font-size: 27px; margin: 6px 0px 0px; }\n"
    "  .fig .d { font-size: 12px; color: #2e7d32; margin: 2px 0px 0px; }\n"
    "  .fig .down { color: #c2410c; }\n",

    /*
     * The card deck, and the reason this example exists.
     *
     * The parent declares three rows; each card spans all three and takes
     * them as its own with `grid-template-rows: subgrid`. So the three
     * footers sit on one line although the bodies above them are three
     * different lengths -- which is what a deck of cards has wanted since
     * cards were invented.
     */
    "  .cards { display: grid; grid-template-columns: repeat(3, 1fr);\n"
    "           grid-template-rows: auto auto auto; gap: 14px; }\n"
    "  .card { grid-row: span 3; display: grid;\n"
    "          grid-template-rows: subgrid; background: #ffffff;\n"
    "          position: relative; }\n",

    "  .card h3 { font-size: 16px; margin: 0px; padding: 18px 18px 0px; }\n"
    "  .card .body { padding: 8px 18px 14px; }\n"
    "  .card .body div { color: #6b6862; padding: 2px 0px; }\n"
    "  .card .foot { padding: 12px 18px; background: #faf8f5;\n"
    "                color: #6b6862; font-size: 13px; }\n",

    /* One badge, placed by `position: absolute` against the card it sits on
       -- the card is `position: relative`, so it is the containing block. */
    "  .badge { position: absolute; top: 14px; right: 14px;\n"
    "           background: #c2410c; color: #ffffff; font-size: 11px;\n"
    "           padding: 3px 8px; }\n",

    /* The table. Striping is `:nth-child(even)`, which is two bits settled at
       declare time rather than a modulo per box. */
    "  table { width: 100%; border-collapse: collapse;\n"
    "          background: #ffffff; }\n"
    "  th { text-align: left; font-size: 11px; color: #9c968c;\n"
    "       padding: 10px 16px; }\n"
    "  td { padding: 11px 16px; color: #3a352d; }\n",

    "  tbody tr:nth-child(even) { background: #faf8f5; }\n"
    "  tbody tr:hover { background: #fdf0e8; }\n"
    "  td.ok { color: #2e7d32; }\n"
    "  td.no { color: #c2410c; }\n"
    "</style>\n"
    "</head>\n",

    /* -------------------------------------------------------------- body -- */
    "<body>\n"
    "<div class=\"bar\">\n"
    "  <div class=\"brand\">\n"
    "    <div class=\"mark\"></div>\n"
    "    <div class=\"name\">areole</div>\n"
    "  </div>\n"
    "  <div class=\"nav\">\n"
    "    <a class=\"on\">Overview</a>\n"
    "    <a>Scenes</a>\n"
    "    <a>Corpora</a>\n"
    "    <a>Settings</a>\n"
    "  </div>\n"
    "</div>\n"
    "<div class=\"rule\"></div>\n",

    "<div class=\"shell\">\n"
    "  <div class=\"side\">\n"
    "    <h4>MEASURE</h4>\n"
    "    <a class=\"on\">Overview</a>\n"
    "    <a>Benchmarks</a>\n"
    "    <a>Comparison</a>\n"
    "    <h4>CONFORM</h4>\n"
    "    <a>Tokenizer</a>\n"
    "    <a>Tree construction</a>\n"
    "    <a>Browser corpora</a>\n"
    "  </div>\n",

    "  <div class=\"main\">\n"
    "    <h1>Overview</h1>\n"
    "    <p class=\"lede\">Every rectangle came out of a stylesheet "
    "&mdash; there is not one coordinate in the C that draws it.</p>\n",

    "    <h2>THIS RUN</h2>\n"
    "    <div class=\"figures\">\n"
    "      <div class=\"fig\">\n"
    "        <div class=\"k\">TREE CONSTRUCTION</div>\n"
    "        <p class=\"v\">98.0%</p>\n"
    "        <p class=\"d\">+12.5 this release</p>\n"
    "      </div>\n"
    "      <div class=\"fig\">\n"
    "        <div class=\"k\">TOKENIZER</div>\n"
    "        <p class=\"v\">100%</p>\n"
    "        <p class=\"d\">7,026 of 7,026</p>\n"
    "      </div>\n",

    "      <div class=\"fig\">\n"
    "        <div class=\"k\">PARSE</div>\n"
    "        <p class=\"v\">40.0</p>\n"
    "        <p class=\"d down\">MB/s, was 42.2</p>\n"
    "      </div>\n"
    "      <div class=\"fig\">\n"
    "        <div class=\"k\">HEAP AFTER INIT</div>\n"
    "        <p class=\"v\">0</p>\n"
    "        <p class=\"d\">allocations, always</p>\n"
    "      </div>\n"
    "    </div>\n",

    /*
     * Three bodies of three different heights, three footers on one line.
     *
     * The bodies are lists of short lines rather than paragraphs, and that is
     * a limitation showing rather than a design choice. Two of them, both
     * named at the top of this file: prose inside a *nested* grid's items
     * wraps at the wrong width, and a paragraph that wraps does not tell the
     * block below it how tall it became. Every line of text on this page fits
     * on one line for the second reason, which is also why the lede is one
     * sentence rather than three.
     */
    "    <h2>WHAT MOVED</h2>\n"
    "    <div class=\"cards\">\n"
    "      <div class=\"card\">\n"
    "        <h3>The select modes</h3>\n"
    "        <div class=\"body\">\n"
    "          <div>in select</div>\n"
    "          <div>in select in table</div>\n"
    "          <div>select scope</div>\n"
    "          <div>the relaxation</div>\n"
    "        </div>\n"
    "        <div class=\"foot\">17 cases</div>\n"
    "      </div>\n",

    "      <div class=\"card\">\n"
    "        <h3>Noah&rsquo;s Ark</h3>\n"
    "        <div class=\"body\">\n"
    "          <div>three of a kind</div>\n"
    "          <div>and no more</div>\n"
    "        </div>\n"
    "        <div class=\"foot\">5 cases</div>\n"
    "      </div>\n",

    "      <div class=\"card\">\n"
    "        <div class=\"badge\">FIXED</div>\n"
    "        <h3>An unwritten field</h3>\n"
    "        <div class=\"body\">\n"
    "          <div>one namespace unset</div>\n"
    "          <div>one table never cleared</div>\n"
    "          <div>a score that moved</div>\n"
    "        </div>\n"
    "        <div class=\"foot\">10 cases</div>\n"
    "      </div>\n"
    "    </div>\n",

    /* Written the way a real page is: no closing tags on the rows or cells.
       The parser supplies them, which is most of what §13.2.6 is for. */
    "    <h2>CORPORA</h2>\n"
    "    <table>\n"
    "      <thead>\n"
    "        <tr><th>CORPUS<th>DOCUMENTS<th>AGREEING<th>STATE\n"
    "      <tbody>\n"
    "        <tr><td>block<td>168<td>168<td class=\"ok\">exact\n"
    "        <tr><td>snap<td>960<td>960<td class=\"ok\">exact\n",

    "        <tr><td>sticky<td>355<td>355<td class=\"ok\">exact\n"
    "        <tr><td>html trees<td>183<td>183<td class=\"ok\">exact\n"
    "        <tr><td>grid<td>218<td>217<td class=\"no\">one, named\n"
    "        <tr><td>table<td>624<td>416<td class=\"no\">not gated\n"
    "    </table>\n"
    "  </div>\n"
    "</div>\n"
    "</body>\n"
    "</html>\n",

    0};

static char   g_doc[8192];
static ar_u32 g_doc_n;

static void build_page(void)
{
    ar_i32 i;

    g_doc[0] = 0;
    for (i = 0; PAGE[i]; ++i)
    {
        ar_u32 k = (ar_u32)strlen(PAGE[i]);

        if (g_doc_n + k < sizeof g_doc - 1u)
        {
            memcpy(g_doc + g_doc_n, PAGE[i], k);
            g_doc_n += k;
        }
    }
    g_doc[g_doc_n] = 0;
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

static void frame(ar_ctx *c, const ar_doc *d, const ar_input *in, ar_surface *surface)
{
    ar_frame_begin(c, in);
    ar_dom_build(c, d);
    ar_frame_end(c, surface);
}

/* The first box whose text is exactly `want`, or -1. The inspection API has no
   way to ask for a class, so the labels on the page are the handles. */
static ar_i32 box_with_text(ar_ctx *c, const char *want)
{
    ar_i32 i;

    for (i = 0; i < ar_node_count(c); ++i)
    {
        if (strcmp(ar_node_text(c, i), want) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int selftest(ar_ctx *c, ar_doc *d)
{
    ar_surface    surface;
    ar_input      in;
    static ar_u32 pixels[WIN_W * WIN_H];
    ar_i32        boxes;
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

    if (d->overflowed)
    {
        printf("FAIL  the page did not fit its budget\n");
        ++bad;
    }

    /*
     * The stylesheet parsed, all of it.
     *
     * This is first because it is the failure that does not look like one. A
     * rule the parser refuses is dropped whole and silently -- a selector list
     * of five, a property that does not exist, a colour keyword instead of a
     * hex triple -- and the page still lays out, just wrongly. Nothing else
     * below would necessarily notice.
     */
    if (ar_stylesheet_errors(c) != 0)
    {
        printf("FAIL  %lu stylesheet rules were refused; the page is not the one written\n",
               (unsigned long)ar_stylesheet_errors(c));
        ++bad;
    }
    else
    {
        printf("ok    the stylesheet parsed with no rule refused\n");
    }

    if (boxes < 80)
    {
        printf("FAIL  only %ld boxes\n", (long)boxes);
        ++bad;
    }
    else
    {
        printf("ok    %ld boxes from %lu bytes of HTML and CSS\n", (long)boxes,
               (unsigned long)g_doc_n);
    }

    /*
     * The grid ran: the sidebar and the content are beside each other rather
     * than stacked. A page whose grid declaration had been dropped would still
     * draw, in one column, and every count check above would pass.
     */
    {
        ar_i32 side = box_with_text(c, "MEASURE");
        ar_i32 head = box_with_text(c, "Overview");

        if (side < 0 || head < 0)
        {
            printf("FAIL  could not find the sidebar and the heading\n");
            ++bad;
        }
        else
        {
            ar_rect s = ar_node_rect(c, side);
            ar_rect h = ar_node_rect(c, head);

            if (h.x <= s.x + s.w)
            {
                printf("FAIL  the content is not beside the sidebar: side x=%ld w=%ld, "
                       "content x=%ld\n",
                       (long)s.x, (long)s.w, (long)h.x);
                ++bad;
            }
            else
            {
                printf("ok    the grid put the content beside the sidebar, not under it\n");
            }
        }
    }

    /* The four figures are one row: same top, four different lefts. */
    {
        static const char *const K[] = {"TREE CONSTRUCTION", "TOKENIZER", "PARSE",
                                        "HEAD" /* unused */};
        ar_i32                   a = box_with_text(c, K[0]);
        ar_i32                   b = box_with_text(c, K[1]);
        ar_i32                   e = box_with_text(c, K[2]);

        if (a < 0 || b < 0 || e < 0)
        {
            printf("FAIL  could not find the figure labels\n");
            ++bad;
        }
        else
        {
            ar_rect ra = ar_node_rect(c, a);
            ar_rect rb = ar_node_rect(c, b);
            ar_rect re = ar_node_rect(c, e);

            if (ra.y != rb.y || rb.y != re.y || !(ra.x < rb.x && rb.x < re.x))
            {
                printf("FAIL  the figures are not one row: y %ld %ld %ld, x %ld %ld %ld\n",
                       (long)ra.y, (long)rb.y, (long)re.y, (long)ra.x, (long)rb.x, (long)re.x);
                ++bad;
            }
            else
            {
                printf("ok    the four figures are one row, left to right\n");
            }
        }
    }

    /*
     * Subgrid, which is the whole point of the card deck: three footers on one
     * line although the three bodies above them are three different lengths.
     *
     * Without subgrid each card would size its own rows and the footers would
     * sit at three different heights -- which is what a card deck looks like
     * when it is wrong, and it is not obviously wrong until you see it right.
     */
    {
        ar_i32 f1 = box_with_text(c, "17 cases");
        ar_i32 f2 = box_with_text(c, "5 cases");
        ar_i32 f3 = box_with_text(c, "10 cases");

        if (f1 < 0 || f2 < 0 || f3 < 0)
        {
            printf("FAIL  could not find the three card footers\n");
            ++bad;
        }
        else
        {
            ar_rect r1 = ar_node_rect(c, f1);
            ar_rect r2 = ar_node_rect(c, f2);
            ar_rect r3 = ar_node_rect(c, f3);

            if (r1.y != r2.y || r2.y != r3.y)
            {
                printf("FAIL  subgrid did not align the card footers: y %ld %ld %ld\n", (long)r1.y,
                       (long)r2.y, (long)r3.y);
                ++bad;
            }
            else
            {
                printf("ok    subgrid put the three card footers on one line, y=%ld\n", (long)r1.y);
            }
        }
    }

    /* The table laid out as a table: four columns across one row. */
    {
        ar_i32 a = box_with_text(c, "CORPUS");
        ar_i32 b = box_with_text(c, "DOCUMENTS");

        if (a < 0 || b < 0)
        {
            printf("FAIL  could not find the table headings\n");
            ++bad;
        }
        else
        {
            ar_rect ra = ar_node_rect(c, a);
            ar_rect rb = ar_node_rect(c, b);

            if (ra.y != rb.y || rb.x <= ra.x)
            {
                printf("FAIL  the table headings are not side by side\n");
                ++bad;
            }
            else
            {
                printf("ok    the table formatting context put the headings in a row\n");
            }
        }
    }

    /* Two identical frames are identical. */
    {
        ar_u32 before = 0;
        ar_u32 after = 0;
        ar_i32 i;

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

/*
 * The frame as a PPM on stdout, so the page can be looked at without a window.
 *
 * P6 because it is six lines of code and no library: a header and the pixels.
 * The surface is 32-bit 0xAARRGGBB and PPM wants three bytes in RGB order, so
 * the only work is the shuffle.
 *
 * It exists because "it looks right" is a claim nobody can check from a list
 * of rectangles, and a window is no use on a machine with no display -- which
 * is every machine CI runs on.
 */
static void ppm(ar_ctx *c, ar_doc *d, const char *path)
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

    /*
     * "wb", and on this platform it decides whether the image is an
     * image. stdout is in text mode, so every 0x0A byte inside a pixel
     * becomes 0x0D 0x0A and everything after it walks one byte sideways.
     * It does not look like a corrupted file -- it looks like the engine
     * rotated the colour channels, because #c2410c comes back as
     * (0c, c2, 41), which is a very convincing bug in something else.
     */
    out = fopen(path, "wb");
    if (!out)
    {
        printf("cannot write %s\n", path);
        return;
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
    const char *ppm_path = 0;
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
        else if (strcmp(argv[k], "--ppm") == 0 && k + 1 < argc)
        {
            ppm_path = argv[++k];
        }
        else
        {
            path = argv[k];
        }
    }

    build_page();
    if (want_html)
    {
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

    c = ar_init_ex(g_memory, (ar_u32)sizeof g_memory, 256, 256 * 1024);
    if (!c)
    {
        printf("the arena is too small\n");
        return 2;
    }

    /* All three before the first frame: a frame reserves the whole box budget
       from the other end of the arena and does not release it until the next
       ar_frame_begin, and ar_font_load wants the arena to itself. */
    load_face(c);
    ar_ua_stylesheet(c);
    d = ar_html_parse_into(c, bytes, len);
    if (!d)
    {
        printf("the page did not fit\n");
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
    if (ppm_path)
    {
        ppm(c, d, ppm_path);
        return 0;
    }

    {
        ar_win *win = ar_win_open("areole - an interface", WIN_W, WIN_H);

        if (!win)
        {
            printf("could not open a window\n");
            return 1;
        }
        ar_set_clock(c, ar_time_us);
        printf("areole %s: %ld nodes, %lu bytes, %lu stylesheet errors\n", ar_version(),
               (long)d->node_count, (unsigned long)g_doc_n, (unsigned long)ar_stylesheet_errors(c));

        while (ar_win_pump(win))
        {
            ar_i32 region;

            frame(c, d, ar_win_input(win), ar_win_surface(win));
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
