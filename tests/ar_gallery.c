/*
 * areole - the gallery renderer.
 * SPDX-License-Identifier: MIT
 *
 * areole's half of the demo gallery: one HTML file in, a picture and a table
 * of rectangles out.
 *
 *     ar_gallery demo.html --geometry          one line per element
 *     ar_gallery demo.html --ppm out.ppm       the render
 *     ar_gallery page.html --ppm out.ppm --scale 2
 *                                              drawn at twice the pixels, then shrunk
 *     ar_gallery page.html --script s.txt --frames dir
 *                                              the page used, a frame at a time
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

/* The picture at a render scale, before it is resampled to the view: four
   times the largest view's pixels, which is 2x of it or 4x of a quarter. */
static ar_u32 g_scaled[MAX_W * MAX_H * 4];

/* "2", "1.5", "0.5x" or "1500" as thousandths: a scale as a person types it. */
static ar_i32 parse_scale(const char *t)
{
    ar_i32 whole = 0, frac = 0, digits = 0;

    while (*t >= '0' && *t <= '9')
    {
        whole = whole * 10 + (*t++ - '0');
    }
    if (*t == '.')
    {
        ++t;
        while (*t >= '0' && *t <= '9' && digits < 3)
        {
            frac = frac * 10 + (*t++ - '0');
            ++digits;
        }
    }
    while (digits++ < 3)
    {
        frac *= 10;
    }
    if (whole >= 100) /* already thousandths */
    {
        return whole;
    }
    return whole * 1000 + frac;
}

/* Font files, one buffer each: a face keeps a pointer into its bytes. */
static unsigned char g_face_body[8u * 1024u * 1024u];
static unsigned char g_face_sans[8u * 1024u * 1024u];
static unsigned char g_face_mono[8u * 1024u * 1024u];
static unsigned char g_face_bold[8u * 1024u * 1024u];
static unsigned char g_face_italic[8u * 1024u * 1024u];
static unsigned char g_face_bold_italic[8u * 1024u * 1024u];
/* A fallback for what the primary face does not cover -- CJK, mostly -- and
   CJK collections are large: Yu Gothic is fourteen megabytes. */
static unsigned char g_face_fallback[24u * 1024u * 1024u];

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

/* ------------------------------------------------------------------------
 * --script: a page used, a frame at a time
 *
 * Every frame is written out, which is what tools/readme_shots.py turns into
 * the README's animation. One command a line, an element named by its id:
 *
 *     at 900 560          the pointer starts here, with no frame
 *     delay 16            each frame after this lasts 16 ms
 *     hold 800            the picture as it is, for 800 ms
 *     move #id            the pointer glides to the element's centre
 *     click #id [n]       glides there and clicks; n = 3 is a triple click
 *     clicktext Manager   the same, for the visible box whose text that is
 *     type Ada Byron      one character a frame
 *     key tab             tab, enter, space, up, down, left, right, home, end
 *     drag #id 650 300    press at 65% of its width, move to 30%, release
 *
 * A line starting "# " is a comment. Frames are <dir>/NNNN.ppm, and
 * <dir>/frames.txt lists each: its file, how long it lasts, and where the
 * pointer is and whether it is held -- the renderer draws no cursor, and an
 * animation without one is a page changing by itself.
 * ------------------------------------------------------------------------ */

typedef struct script_state
{
    ar_ctx     *c;
    ar_doc     *d;
    ar_surface *s;
    const char *dir;
    FILE       *list;
    ar_i32      frames;
    ar_i32      delay;
    ar_i32      x, y;
    int         down;
} script_state;

static char g_script[64 * 1024];

static int script_frame(script_state *st, ar_input *in, ar_i32 ms)
{
    char path[1100];

    in->mouse_x = st->x;
    in->mouse_y = st->y;
    in->mouse_inside = st->x >= 0 && st->y >= 0;
    in->mouse_down = st->down ? 1u : 0u;
    ar_frame_begin(st->c, in);
    ar_dom_build(st->c, st->d);
    ar_frame_end(st->c, st->s);
    sprintf(path, "%s/%04ld.ppm", st->dir, (long)st->frames);
    if (write_ppm(path) != 0)
    {
        return 1;
    }
    fprintf(st->list, "%04ld.ppm %ld %ld %ld %d\n", (long)st->frames, (long)ms, (long)st->x,
            (long)st->y, st->down);
    ++st->frames;
    return 0;
}

/* One frame with nothing happening in it but the pointer, if it moved. */
static int script_idle(script_state *st, ar_i32 ms)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    return script_frame(st, &in, ms);
}

/* The pointer to (tx, ty), eased, over ten frames. */
static int script_glide(script_state *st, ar_i32 tx, ar_i32 ty)
{
    ar_i32 x0 = st->x, y0 = st->y, k;

    for (k = 1; k <= 10; ++k)
    {
        ar_i32 t = k * 100;                                /* thousandths */
        ar_i32 e = (t * t / 1000) * (3000 - 2 * t) / 1000; /* smoothstep */

        st->x = x0 + (tx - x0) * e / 1000;
        st->y = y0 + (ty - y0) * e / 1000;
        if (script_idle(st, st->delay))
        {
            return 1;
        }
    }
    return 0;
}

static int script_rect_of_id(const script_state *st, const char *id, ar_rect *out)
{
    ar_u32 n = (ar_u32)strlen(id);
    ar_i32 i;

    for (i = 0; i < st->d->node_count; ++i)
    {
        ar_span a = ar_a11y_attr(st->d, i, "id");

        if (a.p && a.n == n && memcmp(a.p, id, n) == 0 && st->d->nodes[i].box >= 0)
        {
            *out = ar_node_rect(st->c, st->d->nodes[i].box);
            return 1;
        }
    }
    return 0;
}

/* The last visible box whose own text is `text`: an open list's row is later
   in the tree than the closed face showing the same words. */
static int script_rect_of_text(const script_state *st, const char *text, ar_rect *out)
{
    ar_i32 i;
    int    found = 0;

    for (i = 0; i < ar_node_count(st->c); ++i)
    {
        const char *t = ar_node_text(st->c, i);
        ar_rect     r = ar_node_rect(st->c, i);

        if (t && strcmp(t, text) == 0 && r.w > 0 && r.h > 0)
        {
            *out = r;
            found = 1;
        }
    }
    return found;
}

static int script_click(script_state *st, ar_rect r, ar_i32 times)
{
    ar_i32   k;
    ar_input in;

    if (script_glide(st, r.x + r.w / 2, r.y + r.h / 2))
    {
        return 1;
    }
    for (k = 1; k <= times; ++k)
    {
        memset(&in, 0, sizeof in);
        in.mouse_pressed = 1;
        in.clicks = (ar_u32)k;
        st->down = 1;
        if (script_frame(st, &in, st->delay * 4))
        {
            return 1;
        }
        memset(&in, 0, sizeof in);
        in.mouse_released = 1;
        st->down = 0;
        if (script_frame(st, &in, st->delay * 2))
        {
            return 1;
        }
    }
    return 0;
}

static ar_u32 script_key(const char *name)
{
    static const char *const NAME[] = {"tab",  "enter", "space", "up", "down",
                                       "left", "right", "home",  "end"};
    static const ar_u32 KEY[] = {AR_KEY_TAB,  AR_KEY_ENTER, AR_KEY_SPACE, AR_KEY_UP, AR_KEY_DOWN,
                                 AR_KEY_LEFT, AR_KEY_RIGHT, AR_KEY_HOME,  AR_KEY_END};
    ar_i32              k;

    for (k = 0; k < 9; ++k)
    {
        if (strcmp(name, NAME[k]) == 0)
        {
            return KEY[k];
        }
    }
    return 0;
}

/* One command. Returns 0, or 1 with the reason printed. */
static int script_line(script_state *st, char *line, ar_i32 number)
{
    char     cmd[16], arg[256];
    char    *rest;
    long     a = 0, b = 0;
    ar_rect  r;
    ar_input in;

    cmd[0] = arg[0] = 0;
    if (sscanf(line, "%15s", cmd) != 1 || cmd[0] == 0 || (cmd[0] == '#' && cmd[1] == 0))
    {
        return 0;
    }
    rest = line + strlen(cmd);
    while (*rest == ' ' || *rest == '\t')
    {
        ++rest;
    }

    if (strcmp(cmd, "at") == 0 && sscanf(rest, "%ld %ld", &a, &b) == 2)
    {
        st->x = (ar_i32)a;
        st->y = (ar_i32)b;
        return 0;
    }
    if (strcmp(cmd, "delay") == 0 && sscanf(rest, "%ld", &a) == 1 && a > 0)
    {
        st->delay = (ar_i32)a;
        return 0;
    }
    if (strcmp(cmd, "hold") == 0 && sscanf(rest, "%ld", &a) == 1 && a > 0)
    {
        return script_idle(st, (ar_i32)a);
    }
    if ((strcmp(cmd, "move") == 0 || strcmp(cmd, "click") == 0 || strcmp(cmd, "drag") == 0) &&
        sscanf(rest, "#%255s", arg) == 1)
    {
        if (!script_rect_of_id(st, arg, &r))
        {
            printf("# script line %ld: no element has the id %s\n", (long)number, arg);
            return 1;
        }
        if (strcmp(cmd, "move") == 0)
        {
            return script_glide(st, r.x + r.w / 2, r.y + r.h / 2);
        }
        if (strcmp(cmd, "click") == 0)
        {
            a = 1;
            sscanf(rest, "#%*s %ld", &a);
            return script_click(st, r, a > 0 && a <= 3 ? (ar_i32)a : 1);
        }
        if (sscanf(rest, "#%*s %ld %ld", &a, &b) == 2)
        {
            ar_i32 k, y = r.y + r.h / 2;
            ar_i32 x0 = r.x + (ar_i32)(r.w * a / 1000), x1 = r.x + (ar_i32)(r.w * b / 1000);

            if (script_glide(st, x0, y))
            {
                return 1;
            }
            memset(&in, 0, sizeof in);
            in.mouse_pressed = 1;
            st->down = 1;
            if (script_frame(st, &in, st->delay * 4))
            {
                return 1;
            }
            for (k = 1; k <= 16; ++k)
            {
                st->x = x0 + (x1 - x0) * k / 16;
                if (script_idle(st, st->delay))
                {
                    return 1;
                }
            }
            memset(&in, 0, sizeof in);
            in.mouse_released = 1;
            st->down = 0;
            return script_frame(st, &in, st->delay * 2);
        }
    }
    if (strcmp(cmd, "clicktext") == 0 && *rest)
    {
        if (!script_rect_of_text(st, rest, &r))
        {
            printf("# script line %ld: no visible box says \"%s\"\n", (long)number, rest);
            return 1;
        }
        return script_click(st, r, 1);
    }
    if (strcmp(cmd, "type") == 0 && *rest)
    {
        while (*rest)
        {
            char   ch[8];
            ar_u32 n = 1;

            /* One character, which is one to four bytes of UTF-8. */
            while (n < 4 && ((unsigned char)rest[n] & 0xC0u) == 0x80u)
            {
                ++n;
            }
            memcpy(ch, rest, n);
            ch[n] = 0;
            rest += n;
            memset(&in, 0, sizeof in);
            in.text = ch;
            in.text_len = n;
            if (script_frame(st, &in, st->delay * 4))
            {
                return 1;
            }
        }
        return 0;
    }
    if (strcmp(cmd, "key") == 0 && sscanf(rest, "%255s", arg) == 1 && script_key(arg))
    {
        memset(&in, 0, sizeof in);
        in.keys_pressed = script_key(arg);
        return script_frame(st, &in, st->delay * 8);
    }
    printf("# script line %ld: cannot read \"%s\"\n", (long)number, line);
    return 1;
}

static int run_script(ar_ctx *c, ar_doc *d, ar_surface *s, const char *script, const char *dir)
{
    FILE        *f = fopen(script, "rb");
    script_state st;
    char         path[1100];
    char        *line, *end;
    ar_u32       n;
    ar_i32       number = 0;
    int          bad = 0;

    if (!f || strlen(dir) > 1000)
    {
        printf("# cannot read %s\n", script);
        return 2;
    }
    n = (ar_u32)fread(g_script, 1, sizeof g_script - 1, f);
    fclose(f);
    g_script[n] = 0;

    memset(&st, 0, sizeof st);
    st.c = c;
    st.d = d;
    st.s = s;
    st.dir = dir;
    st.delay = 16;
    st.x = -1;
    st.y = -1;
    sprintf(path, "%s/frames.txt", dir);
    st.list = fopen(path, "w");
    if (!st.list)
    {
        printf("# cannot write %s\n", path);
        return 2;
    }

    /* Two frames nobody sees: the first lays the page out, the second is the
       first one a pointer could have landed on. */
    {
        ar_input in;

        memset(&in, 0, sizeof in);
        in.mouse_x = -1;
        in.mouse_y = -1;
        ar_frame_begin(c, &in);
        ar_dom_build(c, d);
        ar_frame_end(c, s);
        ar_frame_begin(c, &in);
        ar_dom_build(c, d);
        ar_frame_end(c, s);
    }

    for (line = g_script; *line && !bad; line = end)
    {
        end = line;
        while (*end && *end != '\n')
        {
            ++end;
        }
        if (*end)
        {
            *end++ = 0;
        }
        if (end - line >= 2 && end[-2] == '\r')
        {
            end[-2] = 0;
        }
        n = (ar_u32)strlen(line);
        if (n > 0 && line[n - 1] == '\r')
        {
            line[n - 1] = 0;
        }
        bad = script_line(&st, line, ++number);
    }
    fclose(st.list);
    printf("# %ld frames in %s\n", (long)st.frames, dir);
    return bad;
}

int main(int argc, char **argv)
{
    const char *path = 0;
    const char *ppm_path = 0;
    const char *script_path = 0, *frames_dir = 0;
    ar_i32      scale = 1000;
    const char *font_body = 0, *font_sans = 0, *font_mono = 0, *font_bold = 0;
    const char *font_italic = 0, *font_bold_italic = 0, *font_fallback = 0;
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
        else if (strcmp(argv[k], "--script") == 0 && k + 1 < argc)
        {
            script_path = argv[++k];
        }
        else if (strcmp(argv[k], "--frames") == 0 && k + 1 < argc)
        {
            frames_dir = argv[++k];
        }
        else if (strcmp(argv[k], "--scale") == 0 && k + 1 < argc)
        {
            scale = parse_scale(argv[++k]);
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
        else if (strcmp(argv[k], "--italic") == 0 && k + 1 < argc)
        {
            font_italic = argv[++k];
        }
        else if (strcmp(argv[k], "--bold-italic") == 0 && k + 1 < argc)
        {
            font_bold_italic = argv[++k];
        }
        else if (strcmp(argv[k], "--fallback") == 0 && k + 1 < argc)
        {
            font_fallback = argv[++k];
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
               "#        [--size WxH] [--font body.ttf] [--sans sans.ttf] [--mono mono.ttf]\n"
               "#        [--bold bold.ttf] [--italic italic.ttf] [--bold-italic bi.ttf]\n"
               "#        [--fallback cjk.ttc] [--script s.txt --frames dir]\n");
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
        if (font_fallback &&
            (n = read_face(font_fallback, g_face_fallback, sizeof g_face_fallback)) > 0 &&
            !ar_font_add(c, g_face_fallback, n))
        {
            printf("# %s did not load as a fallback\n", font_fallback);
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
        if (font_italic && (n = read_face(font_italic, g_face_italic, sizeof g_face_italic)) > 0)
        {
            ar_font_load_styled(c, g_face_italic, n, 400, 1);
        }
        if (font_bold_italic &&
            (n = read_face(font_bold_italic, g_face_bold_italic, sizeof g_face_bold_italic)) > 0)
        {
            ar_font_load_styled(c, g_face_bold_italic, n, 700, 1);
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
    if (scale != 1000)
    {
        ar_set_render_scale(c, scale);
        scale = ar_render_scale(c);
        s.pixels = g_scaled;
        s.w = (VIEW_W * scale + 999) / 1000;
        s.h = (VIEW_H * scale + 999) / 1000;
        s.stride = s.w;
        if ((long)s.w * (long)s.h > (long)(sizeof g_scaled / sizeof g_scaled[0]))
        {
            printf("# %ldx%ld at that scale does not fit\n", (long)s.w, (long)s.h);
            return 2;
        }
    }
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

    if (script_path && frames_dir)
    {
        return run_script(c, d, &s, script_path, frames_dir);
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
        /* At a render scale the picture is resampled to the view first, the
           way a window shows it: ar_surface_resample is what ar_win_present
           calls. */
        if (scale != 1000)
        {
            ar_surface view;

            view.pixels = g_pixels;
            view.w = VIEW_W;
            view.h = VIEW_H;
            view.stride = VIEW_W;
            ar_surface_resample(&s, &view, ar_rect_make(0, 0, VIEW_W, VIEW_H), scale);
        }
        return write_ppm(ppm_path);
    }
    return 0;
}
