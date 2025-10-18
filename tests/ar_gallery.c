/*
 * areole - the gallery renderer.
 * SPDX-License-Identifier: MIT
 *
 * areole's half of the demo gallery: one HTML file in, a picture and a table
 * of rectangles out.
 *
 *     ar_gallery demo.html --geometry          one line per element
 *     ar_gallery demo.html --ppm out.ppm       the render
 *
 * tools/gallery.py drives both halves and compares them. See
 * docs/roadmap/example-gallery.md for what a demo is and why there is one file
 * per feature rather than one page per subject.
 *
 * ------------------------------------------------------------------------
 * Elements, not boxes
 *
 * ar_dom_build gives every text node a box of its own, and a table gets
 * anonymous rows and cells that no element asked for. A browser's DOM has
 * neither, so the dump skips both and numbers what is left as the document's
 * own tree -- `0/1/0` is the first child of the second child of the root.
 *
 * That is not a weaker comparison. If a generated box is wrong, the declared
 * ones land somewhere else, and the declared ones are what is compared.
 *
 * ------------------------------------------------------------------------
 * Why the viewport is fixed
 *
 * 800 by 600, stated here and in the twin, because a demo whose geometry
 * depends on the window is a demo that fails on a different machine. The
 * gallery's third rule says deterministic and this is most of what that
 * means.
 */
#include "ar_internal.h"
#include "ar_html.h"
#include "ar_node.h"
#include "ar_css.h"

#include <stdio.h>
#include <string.h>

/* The gallery's size, which --size can change for a page that is not a demo
   -- the versus tool renders whole forms at the size of a browser window. */
#define MAX_W 1600
#define MAX_H 1200
static ar_i32 VIEW_W = 800;
static ar_i32 VIEW_H = 600;

/* Generous, because a demo is small and the cost of being wrong about it is a
   corpus entry that silently truncates. The extra eight megabytes are glyph
   atlases, for the faces --font, --sans and --mono load. */
static unsigned char g_memory[AR_MEM_DOC(8192, 2u * 1024u * 1024u) + 8u * 1024u * 1024u];
static ar_u32        g_pixels[MAX_W * MAX_H];

/* Font files, one buffer each: a face keeps a pointer into its bytes. */
static unsigned char g_face_body[8u * 1024u * 1024u];
static unsigned char g_face_sans[8u * 1024u * 1024u];
static unsigned char g_face_mono[8u * 1024u * 1024u];
static unsigned char g_face_bold[8u * 1024u * 1024u];

static ar_u32 read_face(const char *path, unsigned char *buf, ar_u32 cap)
{
    FILE  *f = fopen(path, "rb");
    ar_u32 n;

    if (!f)
    {
        printf("# cannot open %s\n", path);
        return 0;
    }
    n = (ar_u32)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}
static char   g_file[512 * 1024];
static ar_u32 g_file_n;

static int read_file(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f)
    {
        printf("# cannot open %s\n", path);
        return 0;
    }
    g_file_n = (ar_u32)fread(g_file, 1, sizeof g_file - 1, f);
    fclose(f);
    g_file[g_file_n] = 0;
    return 1;
}

/* ------------------------------------------------------------------------
 * The tree the document declared
 * ------------------------------------------------------------------------ */

#define MAX_NODES 4096

static ar_i32 g_index[MAX_NODES];
static ar_i32 g_count[MAX_NODES];

/*
 * A box that stands for an element: not generated, and not a text run.
 *
 * `ar_node_text` returns "" and never null for a box with no text, so the test
 * is on the first character. Comparing the pointer to null is always false and
 * made every box a text run, which printed a dump with no boxes in it at all.
 */
static int is_element(const ar_ctx *c, ar_i32 i)
{
    return !ar_node_generated(c, i) && ar_node_text(c, i)[0] == 0;
}

/* The nearest ancestor that is one. An anonymous box is transparent: to the
   document, its children are its parent's. */
static ar_i32 element_parent(const ar_ctx *c, ar_i32 i)
{
    ar_i32 at = ar_node_parent(c, i);

    while (at >= 0 && !is_element(c, at))
    {
        at = ar_node_parent(c, at);
    }
    return at;
}

static void number_nodes(const ar_ctx *c)
{
    ar_i32 n = ar_node_count(c);
    ar_i32 i;

    for (i = 0; i < n && i < MAX_NODES; ++i)
    {
        g_index[i] = -1;
        g_count[i] = 0;
    }
    /* Pre-order, so a parent is numbered before its children. */
    for (i = 0; i < n && i < MAX_NODES; ++i)
    {
        ar_i32 p;

        if (!is_element(c, i))
        {
            continue;
        }
        p = element_parent(c, i);
        if (p < 0)
        {
            g_index[i] = 0;
            continue;
        }
        g_index[i] = g_count[p]++;
    }
}

static void path_of(const ar_ctx *c, ar_i32 i, char *out)
{
    ar_i32 stack[64];
    ar_i32 depth = 0;
    ar_i32 at = i;
    char  *w = out;

    while (at >= 0 && depth < 64)
    {
        stack[depth++] = at;
        at = element_parent(c, at);
    }
    while (depth > 0)
    {
        --depth;
        if (w != out)
        {
            *w++ = '/';
        }
        sprintf(w, "%ld", (long)g_index[stack[depth]]);
        while (*w)
        {
            ++w;
        }
    }
    *w = 0;
}

/* ------------------------------------------------------------------------
 * The picture
 * ------------------------------------------------------------------------ */

static int write_ppm(const char *path)
{
    FILE  *out;
    ar_i32 i;

    /* "wb". Text mode turns every 0x0A in the pixel data into 0x0D 0x0A and
       the file is silently corrupt. */
    out = fopen(path, "wb");
    if (!out)
    {
        printf("# cannot write %s\n", path);
        return 1;
    }
    fprintf(out, "P6\n%d %d\n255\n", VIEW_W, VIEW_H);
    for (i = 0; i < VIEW_W * VIEW_H; ++i)
    {
        ar_u32 p = g_pixels[i];

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
    const char *font_body = 0, *font_sans = 0, *font_mono = 0, *font_bold = 0;
    int         want_geometry = 0;
    int         want_ids = 0;
    int         k;

    ar_surface s;
    ar_input   in;
    ar_ctx    *c;
    ar_doc    *d;

    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--geometry") == 0)
        {
            want_geometry = 1;
        }
        else if (strcmp(argv[k], "--ppm") == 0 && k + 1 < argc)
        {
            ppm_path = argv[++k];
        }
        else if (strcmp(argv[k], "--ids") == 0)
        {
            want_ids = 1;
        }
        else if (strcmp(argv[k], "--font") == 0 && k + 1 < argc)
        {
            font_body = argv[++k];
        }
        else if (strcmp(argv[k], "--sans") == 0 && k + 1 < argc)
        {
            font_sans = argv[++k];
        }
        else if (strcmp(argv[k], "--mono") == 0 && k + 1 < argc)
        {
            font_mono = argv[++k];
        }
        else if (strcmp(argv[k], "--bold") == 0 && k + 1 < argc)
        {
            font_bold = argv[++k];
        }
        else if (strcmp(argv[k], "--size") == 0 && k + 1 < argc)
        {
            long w = 0, h = 0;

            if (sscanf(argv[++k], "%ldx%ld", &w, &h) == 2 && w > 0 && h > 0 && w <= MAX_W &&
                h <= MAX_H)
            {
                VIEW_W = (ar_i32)w;
                VIEW_H = (ar_i32)h;
            }
        }
        else
        {
            path = argv[k];
        }
    }
    if (!path)
    {
        printf("# usage: ar_gallery demo.html [--geometry] [--ids] [--ppm out.ppm]\n"
               "#        [--size WxH] [--font body.ttf] [--sans sans.ttf] [--mono mono.ttf]\n");
        return 2;
    }
    if (!read_file(path))
    {
        return 2;
    }

    c = ar_init_ex(g_memory, (ar_u32)sizeof g_memory, 2048, 2u * 1024u * 1024u);
    if (!c)
    {
        printf("# the arena is too small\n");
        return 2;
    }
    /* Faces before anything else, because a frame reserves the arena's other
       end and an atlas has to come out of the persistent half. The gallery's
       demos load none and draw in the built-in face, as they always have. */
    if (font_body)
    {
        ar_u32 n = read_face(font_body, g_face_body, sizeof g_face_body);

        if (!n || !ar_font_load(c, g_face_body, n, 1024u * 1024u, 64))
        {
            printf("# %s did not load\n", font_body);
        }
        if (font_sans && (n = read_face(font_sans, g_face_sans, sizeof g_face_sans)) > 0)
        {
            ar_font_load_sans(c, g_face_sans, n);
        }
        if (font_mono && (n = read_face(font_mono, g_face_mono, sizeof g_face_mono)) > 0)
        {
            ar_font_load_mono(c, g_face_mono, n);
        }
        if (font_bold && (n = read_face(font_bold, g_face_bold, sizeof g_face_bold)) > 0)
        {
            ar_font_load_styled(c, g_face_bold, n, 700, 0);
        }
    }
    ar_ua_stylesheet(c);
    d = ar_html_parse_into(c, g_file, g_file_n);
    if (!d)
    {
        printf("# %s did not fit\n", path);
        return 2;
    }
    ar_doc_stylesheets(c, d);

    memset(&s, 0, sizeof s);
    s.pixels = g_pixels;
    s.w = VIEW_W;
    s.h = VIEW_H;
    s.stride = VIEW_W;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    {
        /* One frame, so there is no previous viewport to take this from and
           every `@media (min-width: ...)` would be false without it. */
        ar_media m;

        m.width = VIEW_W;
        m.height = VIEW_H;
        m.resolution = 1000;
        ar_set_media(c, &m);
    }

    ar_frame_begin(c, &in);
    ar_dom_build(c, d);
    ar_frame_end(c, &s);

    if (want_geometry)
    {
        ar_i32 i;
        ar_i32 n = ar_node_count(c);

        printf("# areole %s viewport %dx%d\n", ar_version(), VIEW_W, VIEW_H);
        /* Said rather than assumed: a demo that outgrew the arena would
           otherwise report a short tree as a complete one. */
        printf("# nodes %ld boxes %ld truncated %d\n", (long)d->node_count, (long)n,
               d->overflowed || ar_overflowed(c) ? 1 : 0);
        number_nodes(c);
        for (i = 0; i < n && i < MAX_NODES; ++i)
        {
            char    p[256];
            ar_rect r;

            if (!is_element(c, i))
            {
                continue;
            }
            r = ar_node_rect(c, i);
            path_of(c, i, p);
            printf("%s %ld %ld %ld %ld\n", p, (long)r.x, (long)r.y, (long)r.w, (long)r.h);
        }
    }

    /* Every element with an id, by the box its node became: how the versus
       tool lines the two engines up when one of them builds boxes the other
       does not -- a field's text box, a checkbox's mark. */
    if (want_ids)
    {
        ar_i32 i;

        printf("# areole %s viewport %dx%d\n", ar_version(), (int)VIEW_W, (int)VIEW_H);
        for (i = 0; i < d->node_count; ++i)
        {
            ar_span id = ar_a11y_attr(d, i, "id");
            ar_rect r;

            if (!id.p || id.n == 0 || d->nodes[i].box < 0)
            {
                continue;
            }
            r = ar_node_rect(c, d->nodes[i].box);
            printf("#%.*s %ld %ld %ld %ld\n", (int)id.n, id.p, (long)r.x, (long)r.y, (long)r.w,
                   (long)r.h);
        }
    }

    if (ppm_path)
    {
        return write_ppm(ppm_path);
    }
    return 0;
}
