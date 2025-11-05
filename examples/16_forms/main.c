/*
 * areole example 16 - a form you can actually fill in
 * SPDX-License-Identifier: MIT
 *
 *     example_forms              open a window and use it
 *     example_forms --dump       print what the engine thinks it has
 *     example_forms --selftest   drive it without a window and check
 *     example_forms --html       print the page, which is form.html
 *     example_forms --ppm F      draw it, focused, into an image
 *     example_forms --ppm-open F the same, with the select's list open
 *
 * The page is examples/16_forms/form.html and it has no stylesheet. What it
 * looks like is the user-agent sheet's doing, and tools/versus.py puts it
 * beside Edge to show how close that is.
 *
 * ------------------------------------------------------------------------
 * What this is for
 *
 * Every example before this one renders. This one is the first that can be
 * *used*: Tab moves between controls, Space and Enter operate them, and a text
 * field takes what is typed into it. That is 0.10.0's reason for existing, and
 * the difference between a viewer and an interface.
 *
 * It is a form rather than a gallery of widgets because the interesting bugs
 * are between controls and not inside them. Tab order, focus leaving a field
 * and coming back to find its text, a radio group excluding itself, a
 * `<details>` opening without moving everything below it -- none of those are
 * visible in a page with one control on it.
 *
 * ------------------------------------------------------------------------
 * What to try, and what to watch
 *
 *   Tab / Shift+Tab   moves the focus, and *draws a ring*
 *   click             moves the focus and draws no ring
 *
 * That pair is the whole of `:focus-visible`, and it is the thing worth
 * looking at first. A click focuses the field so typing arrives, and shows no
 * ring, because you know where you just clicked. A Tab shows one, because you
 * might not. Pages ship `outline: none` because engines used to draw both.
 *
 *   Space / Enter     operates the focused control
 *   typing            goes into the focused text field
 *   arrows, Home, End move the caret; Shift extends the selection
 *   Ctrl+A / Z / Y    select all, undo, redo
 *
 * Type something into the first field, Tab away, Tab back: the text is still
 * there. Then press Ctrl+Z: nothing happens, because the undo history does not
 * survive leaving a field. That is a real limitation, named in ar_node.h
 * beside the buffer rather than hidden, and this example is where you can see
 * it rather than read about it.
 *
 * And the controls 0.10.0 finished: a textarea that wraps and scrolls, a
 * password that draws bullets, a number field Up and Down step, a select that
 * opens on Space and chooses with the arrows, a slider, a colour field with a
 * palette, a file field that asks Windows for a file, a label that ticks its
 * checkbox, and a submit button that hands the form to this program -- which
 * prints what a server would have been sent.
 *
 * Run Narrator (Ctrl+Win+Enter) and Tab through it: every control should
 * announce its role, its name from its label, and its value. That is written
 * and not yet heard -- see README.md beside this file.
 */
#include "areole.h"
#include "areole_win32.h"

#include <stdio.h>
#include <string.h>

#define WIN_W 800
#define WIN_H 860

/*
 * The fonts a browser uses for a page with no stylesheet, on Windows: Times
 * New Roman for the text, its bold for the heading, Arial for the controls and
 * Consolas for monospace -- the families Edge reports through
 * getComputedStyle. Elsewhere, DejaVu stands in for all three, which is a
 * different picture and an honest one.
 */
static const char *const FACE_BODY[] = {"C:/Windows/Fonts/times.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf", 0};
static const char *const FACE_BOLD[] = {"C:/Windows/Fonts/timesbd.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf", 0};
static const char *const FACE_SANS[] = {"C:/Windows/Fonts/arial.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0};
static const char *const FACE_MONO[] = {"C:/Windows/Fonts/consola.ttf",
                                        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 0};

/* One buffer a face: a face keeps a pointer into its bytes. */
static unsigned char g_font_body[4 * 1024 * 1024];
static unsigned char g_font_bold[4 * 1024 * 1024];
static unsigned char g_font_sans[4 * 1024 * 1024];
static unsigned char g_font_mono[4 * 1024 * 1024];

/* The atlas comes out of the same arena as everything else, so the block
   has to be big enough for it -- a face that will not load is a face that
   was never given room, which reads as "no font found" and is not. */
#define ATLAS_BYTES (512u * 1024u)
#define MAX_PX      48

/* The glyph atlas grows with the square of the render scale (--scale), so the
   block has room for one drawn at several times the window's pixels. */
static unsigned char g_mem[AR_MEM_DOC(1536, 64 * 1024) + 16 * 1024 * 1024];
static ar_i32        g_scale = 1000;

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

static ar_doc *g_doc;

/* A clock the selftest owns, so a blink can be stepped to rather than waited
   for. The window uses the real one. */
static ar_u32 g_fake_us = 1000000u;

static ar_u32 fake_clock(void)
{
    return g_fake_us;
}

/*
 * The document is examples/16_forms/form.html, embedded -- not a copy written
 * for this program, the same file, so that what the window shows, what the
 * selftest drives and what tools/versus.py puts beside a browser are one page.
 * `--html` prints it back out, and the build can diff the two.
 *
 * In parts, because C90 caps a string literal at 509 characters; adjacent
 * literals concatenate, and the commas are what make these separate.
 */
static const char *const DOC[] = {
    "<!doctype html>\n"
    "<html><head><meta charset=\"utf-8\"><title>Delivery</title></head>\n"
    "<body>\n"
    "<h1 id=\"title\">Delivery</h1>\n"
    "<form id=\"order\" action=\"/order\" method=\"post\">\n"
    "<p id=\"p-name\"><label id=\"l-name\" for=\"name\">Name</label>\n"
    "<input id=\"name\" name=\"name\" type=\"text\" value=\"\"></p>\n"
    "<p id=\"p-note\"><label id=\"l-note\" for=\"note\">Note for the driver</label>\n"
    "<input id=\"note\" name=\"note\" type=\"text\" value=\"leave at the door\"></p>\n",

    "<p id=\"p-pin\"><label id=\"l-pin\" for=\"pin\">Door PIN</label>\n"
    "<input id=\"pin\" name=\"pin\" type=\"password\" value=\"1234\" maxlength=\"8\"></p>\n"
    "<p id=\"p-qty\"><label id=\"l-qty\" for=\"qty\">How many</label>\n"
    "<input id=\"qty\" name=\"qty\" type=\"number\" value=\"2\" min=\"1\" max=\"9\"></p>\n"
    "<p id=\"p-addr\"><label id=\"l-addr\" for=\"addr\">Address</label>\n"
    "<textarea id=\"addr\" name=\"addr\">12 Mill Lane</textarea></p>\n"
    "<p id=\"p-size\"><label id=\"l-size\" for=\"size\">Box size</label>\n"
    "<select id=\"size\" name=\"size\">\n",

    "<option value=\"s\">Small</option>\n"
    "<option value=\"m\" selected>Medium</option>\n"
    "<optgroup label=\"Bulky\">\n"
    "<option value=\"l\">Large</option>\n"
    "<option value=\"xl\">Extra large</option>\n"
    "</optgroup></select></p>\n"
    "<p id=\"p-tip\"><label id=\"l-tip\" for=\"tip\">Tip</label>\n"
    "<input id=\"tip\" name=\"tip\" type=\"range\" min=\"0\" max=\"20\" value=\"5\"></p>\n"
    "<p id=\"p-ribbon\"><label id=\"l-ribbon\" for=\"ribbon\">Ribbon colour</label>\n"
    "<input id=\"ribbon\" name=\"ribbon\" type=\"color\" value=\"#c02040\"></p>\n",

    "<p id=\"p-photo\"><label id=\"l-photo\" for=\"photo\">Photo of the door</label>\n"
    "<input id=\"photo\" name=\"photo\" type=\"file\"></p>\n"
    "<fieldset id=\"when\"><legend id=\"when-legend\">When</legend>\n"
    "<p id=\"p-soon\"><input id=\"soon\" type=\"radio\" name=\"when\" value=\"soon\" checked>\n"
    "<label id=\"l-soon\" for=\"soon\">As soon as possible</label></p>\n"
    "<p id=\"p-evening\"><input id=\"evening\" type=\"radio\" name=\"when\" value=\"evening\">\n"
    "<label id=\"l-evening\" for=\"evening\">This evening</label></p>\n",

    "<p id=\"p-weekend\"><input id=\"weekend\" type=\"radio\" name=\"when\" value=\"weekend\">\n"
    "<label id=\"l-weekend\" for=\"weekend\">At the weekend</label></p>\n"
    "</fieldset>\n"
    "<p id=\"p-gift\"><input id=\"gift\" name=\"gift\" type=\"checkbox\">\n"
    "<label id=\"l-gift\" for=\"gift\">Wrap it as a gift</label></p>\n"
    "<p id=\"p-news\"><label id=\"l-news\"><input id=\"news\" name=\"news\" type=\"checkbox\" "
    "checked>\n"
    "Email me about offers</label></p>\n"
    "<details id=\"more\"><summary id=\"more-summary\">Delivery instructions</summary>\n",

    "<p>The gate code is 4417.</p></details>\n"
    "<p id=\"p-packing\"><label id=\"l-packing\" for=\"packing\">Packing</label>\n"
    "<progress id=\"packing\" value=\"0.6\" max=\"1\"></progress></p>\n"
    "<p id=\"p-fragile\"><label id=\"l-fragile\" for=\"fragile\">Fragility</label>\n"
    "<meter id=\"fragile\" min=\"0\" max=\"10\" value=\"7\"></meter></p>\n"
    "<input type=\"hidden\" name=\"src\" value=\"example16\">\n"
    "<p id=\"p-buttons\"><button id=\"send\" name=\"go\" value=\"send\">Place order</button>\n",

    "<input id=\"clear\" type=\"reset\" value=\"Start again\"></p>\n"
    "</form>\n"
    "<p id=\"p-off\"><input id=\"off\" type=\"text\" value=\"not editable\" disabled>\n"
    "<label id=\"l-off\" for=\"off\">A disabled field is not a tab stop</label></p>\n"
    "</body></html>\n",
};

#define DOC_N ((int)(sizeof DOC / sizeof DOC[0]))

/*
 * No stylesheet.
 *
 * The first version of this example carried a page of CSS -- spacing, two
 * columns, control sizes -- and the form looked like a form only because of
 * it. With the sheet taken away it looked nothing like a browser's, which is
 * the comparison that matters: an engine is judged by what it does with a
 * plain page. So there is none, and the controls look the way they do because
 * the user-agent sheet was measured against Edge until they matched.
 */
static ar_u32 read_first(const char *const *paths, unsigned char *buf, ar_u32 cap)
{
    ar_i32 i;

    for (i = 0; paths[i]; ++i)
    {
        FILE  *f = fopen(paths[i], "rb");
        ar_u32 n;

        if (!f)
        {
            continue;
        }
        n = (ar_u32)fread(buf, 1, cap, f);
        fclose(f);
        if (n)
        {
            return n;
        }
    }
    return 0;
}

static int load_face(ar_ctx *c)
{
    ar_u32 n = read_first(FACE_BODY, g_font_body, sizeof g_font_body);

    /* Glyphs are rasterized at the render scale, so the largest one is that
       many times larger, and so is every one in the atlas. */
    ar_u32 atlas = ATLAS_BYTES / 1000u * (ar_u32)g_scale / 1000u * (ar_u32)g_scale;

    atlas = atlas < ATLAS_BYTES           ? ATLAS_BYTES
            : atlas > 12u * 1024u * 1024u ? 12u * 1024u * 1024u
                                          : atlas;
    if (!n ||
        !ar_font_load(c, g_font_body, n, atlas, MAX_PX * (g_scale > 1000 ? g_scale : 1000) / 1000))
    {
        return 0;
    }
    if ((n = read_first(FACE_BOLD, g_font_bold, sizeof g_font_bold)) > 0)
    {
        ar_font_load_styled(c, g_font_bold, n, 700, 0);
    }
    if ((n = read_first(FACE_SANS, g_font_sans, sizeof g_font_sans)) > 0)
    {
        ar_font_load_sans(c, g_font_sans, n);
    }
    if ((n = read_first(FACE_MONO, g_font_mono, sizeof g_font_mono)) > 0)
    {
        ar_font_load_mono(c, g_font_mono, n);
    }
    return 1;
}

static void build(ar_ctx *c)
{
    ar_ua_stylesheet(c);
}

static void frame(ar_ctx *c, const ar_input *in, ar_surface *s)
{
    ar_frame_begin(c, in);
    ar_dom_build(c, g_doc);
    ar_frame_end(c, s);
}

static const char *role_name(ar_u8 role)
{
    static const char *ROLE[] = {
        "none",          "button",   "link",    "checkbox",   "radio",  "textbox", "search",
        "slider",        "combobox", "option",  "heading",    "para",   "list",    "listitem",
        "table",         "row",      "cell",    "colhead",    "image",  "dialog",  "progress",
        "meter",         "group",    "form",    "main",       "nav",    "banner",  "contentinfo",
        "complementary", "region",   "article", "spinbutton", "listbox"};

    return role < (ar_u8)(sizeof ROLE / sizeof ROLE[0]) ? ROLE[role] : "?";
}

/* The states a reader would say, spelled out for the dump. */
static void state_words(ar_u32 st, char *out)
{
    out[0] = 0;
    if (st & AR_A11Y_FOCUSED)
    {
        strcat(out, " focused");
    }
    if (st & AR_A11Y_CHECKED)
    {
        strcat(out, " checked");
    }
    if (st & AR_A11Y_SELECTED)
    {
        strcat(out, " selected");
    }
    if (st & AR_A11Y_EXPANDED)
    {
        strcat(out, " expanded");
    }
    if (st & AR_A11Y_COLLAPSED)
    {
        strcat(out, " collapsed");
    }
    if (st & AR_A11Y_DISABLED)
    {
        strcat(out, " unavailable");
    }
    if (st & AR_A11Y_READONLY)
    {
        strcat(out, " read-only");
    }
    if (st & AR_A11Y_PROTECTED)
    {
        strcat(out, " protected");
    }
}

/*
 * What a screen reader would be told, printed.
 *
 * The point of the accessibility tree is that it is checkable without a screen
 * reader, and this is where that pays off: a name that comes out wrong is
 * visible here as a line of text rather than as somebody's report that the
 * form says "edit edit edit". It is the tree a backend adapts -- the same
 * array ar_win32_a11y.c hands to Narrator.
 */
static void dump(ar_ctx *c)
{
    static ar_a11y_item items[256];
    ar_i32              n = ar_a11y_tree(c, g_doc, items, 256);
    ar_i32              i;

    printf("areole %s -- the accessibility tree of example 16\n\n", ar_version());
    for (i = 0; i < n && i < 256; ++i)
    {
        char   name[128], value[128], st[96];
        ar_i32 depth = 0, up = items[i].parent;

        while (up >= 0)
        {
            ++depth;
            up = items[up].parent;
        }
        ar_a11y_name(g_doc, items[i].node, name, sizeof name);
        ar_a11y_value(c, g_doc, items[i].node, value, sizeof value);
        state_words(items[i].state, st);
        printf("  %*s%-11s %s%s%s%s%s\n", (int)(depth * 2), "", role_name(items[i].role),
               name[0] ? name : "(unnamed)", value[0] ? " = \"" : "", value, value[0] ? "\"" : "",
               st);
    }
    printf("\n  %d nodes, %d tab stops\n", (int)n, (int)ar_tab_stops(c));
}

/*
 * The form, rendered to a file, so it can be looked at.
 *
 * Every other check in this file is about behaviour -- what the focus does,
 * what the buffer holds, what a reader would be told. None of them can see
 * whether the checkbox has a mark in it or whether the text sits inside the
 * field, and "the tests pass" is not an answer to "does it look right".
 */
static void ppm(ar_ctx *c, const char *path, int open_select)
{
    FILE         *out;
    ar_surface    surf;
    ar_input      in;
    static ar_u32 pixels[WIN_W * WIN_H];
    ar_i32        i;

    memset(&surf, 0, sizeof surf);
    surf.pixels = pixels;
    surf.w = WIN_W;
    surf.h = WIN_H;
    surf.stride = WIN_W;
    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;

    /* Two frames, then a Tab, then two more: the first settles the tree, and
       the Tab is there so the focus ring is in the picture -- a screenshot of
       an untouched form cannot show the thing this release is about. */
    frame(c, &in, &surf);
    frame(c, &in, &surf);
    in.keys_pressed = AR_KEY_TAB;
    frame(c, &in, &surf);
    in.keys_pressed = 0;
    frame(c, &in, &surf);

    /* Tab on to the select and open it: name, note, PIN, quantity, address,
       then the select -- which Space opens on the frame after. */
    if (open_select)
    {
        int k;

        for (k = 0; k < 5; ++k)
        {
            in.keys_pressed = AR_KEY_TAB;
            frame(c, &in, &surf);
        }
        in.keys_pressed = AR_KEY_SPACE;
        frame(c, &in, &surf);
        in.keys_pressed = AR_KEY_DOWN;
        frame(c, &in, &surf);
        in.keys_pressed = 0;
        frame(c, &in, &surf);
        frame(c, &in, &surf);
    }

    /* "wb", and on this platform it decides whether the image is an image:
       stdout in text mode turns every 0x0A inside a pixel into 0x0D 0x0A and
       everything after walks one byte sideways -- which looks like rotated
       colour channels rather than a corrupted file. */
    out = fopen(path, "wb");
    if (!out)
    {
        printf("cannot write %s\n", path);
        return;
    }
    fprintf(out, "P6\n%d %d\n255\n", WIN_W, WIN_H);
    for (i = 0; i < WIN_W * WIN_H; ++i)
    {
        ar_u32 px = pixels[i];

        fputc((int)((px >> 16) & 0xFFu), out);
        fputc((int)((px >> 8) & 0xFFu), out);
        fputc((int)(px & 0xFFu), out);
    }
    fclose(out);
    printf("wrote %s\n", path);
}

/*
 * How many pixels of one colour are inside a rectangle.
 *
 * The whole of the pixel gate below, and deliberately the smallest thing that
 * could be: counting is enough to say a ring is there, and comparing against a
 * golden image would make this example the first that cannot be run on a
 * machine without one.
 */
static ar_i32 count_px(const ar_surface *s, ar_rect r, ar_u32 rgb)
{
    ar_i32 x, y, n = 0;

    for (y = r.y; y < r.y + r.h; ++y)
    {
        if (y < 0 || y >= s->h)
        {
            continue;
        }
        for (x = r.x; x < r.x + r.w; ++x)
        {
            if (x >= 0 && x < s->w && (s->pixels[y * s->stride + x] & 0xFFFFFFu) == rgb)
            {
                ++n;
            }
        }
    }
    return n;
}

/*
 * The check no assertion about state could make.
 *
 * 0.10.0 shipped fourteen commits of focus, controls and editing with 1,860
 * assertions behind them, every one about state or geometry, and the focus
 * ring did not appear on the screen. Three separate faults, and all three were
 * invisible to a test that asks the engine what it thinks:
 *
 *   - `outline-width` and `outline-color` were not in ar_paint_digest, so a
 *     frame where only the ring changed produced no damage and never painted;
 *   - damage was the border box, and an outline is drawn outside it, so the
 *     repaint that did happen clipped the ring away;
 *   - `outline-color` was not in the list that resolves a system colour, so
 *     AccentColor arrived at the paint pass as the index 17, whose alpha is
 *     zero.
 *
 * Every one of them is a list that has to grow when a property joins a pass.
 * None of them can be caught by asking; all three are caught by looking. So
 * this counts pixels: the ring's own band must be full of AccentColor and the
 * box just inside it must have none, which is what makes it a *ring* and not a
 * filled rectangle or a border.
 */
static int ring_check(ar_ctx *c, ar_surface *s, int want_ring, const char **why)
{
    const ar_u32 ACCENT = 0x0078D7u; /* AccentColor, light scheme */
    const ar_i32 W = 2;              /* the user-agent sheet's outline-width */
    ar_i32       i = ar_focus_node(c);
    ar_rect      box, out;
    ar_i32       band, inside;

    if (i < 0)
    {
        *why = "nothing is focused";
        return 0;
    }
    box = ar_node_rect(c, i);
    out = ar_rect_make(box.x - W, box.y - W, box.w + 2 * W, box.h + 2 * W);

    /* The band is the difference of the two rectangles, so this is the area of
       one minus the area of the other -- no second walk, and it states the
       arithmetic the ring has to satisfy rather than a number somebody read
       off an image once. */
    band = count_px(s, out, ACCENT) - count_px(s, box, ACCENT);
    inside = count_px(s, box, ACCENT);

    if (!want_ring)
    {
        *why = "a mouse focus draws no ring";
        return band == 0;
    }

    *why = "a key focus draws a full ring, and only a ring";
    if (band != out.w * out.h - box.w * box.h)
    {
        *why = "the ring is missing or incomplete";
        return 0;
    }
    /* A field is white and holds no accent of its own, so anything here is the
       ring having been drawn as a filled rectangle behind the box instead of
       four rectangles around it. */
    if (inside != 0)
    {
        *why = "the ring bled into the box";
        return 0;
    }
    return 1;
}

/* Every pixel the last frame presented, summed over its damage regions --
   what a blink costs, which 0.10.0 says must be under two thousand. */
static ar_i32 presented_px(ar_ctx *c)
{
    ar_i32 k, px = 0;

    for (k = 0; k < ar_damage_count(c); ++k)
    {
        ar_rect r = ar_damage_rect(c, k);

        px += r.w * r.h;
    }
    return px;
}

static ar_i32 node_by_id(const char *id)
{
    ar_i32 i;
    size_t n = strlen(id);

    for (i = 0; i < g_doc->node_count; ++i)
    {
        ar_span v = ar_a11y_attr(g_doc, i, "id");

        if (v.p && v.n == n && memcmp(v.p, id, n) == 0)
        {
            return i;
        }
    }
    return -1;
}

static int value_is(ar_ctx *c, const char *id, const char *want)
{
    char   buf[256];
    ar_u32 n = ar_a11y_value(c, g_doc, node_by_id(id), buf, sizeof buf);

    return n == strlen(want) && memcmp(buf, want, n) == 0;
}

/*
 * The same thing a person would do, without a window.
 *
 * Every check here is one the demo is for: Tab reaches the fields in order,
 * typing lands in the focused one, the text survives leaving and returning, a
 * radio group excludes itself, and the form is handed over when it is sent.
 * If this passes, the window will behave.
 */
static int selftest(ar_ctx *c, ar_surface *s)
{
    ar_input in;
    int      fail = 0;

/*
 * `typed` and not `text`, because a macro parameter replaces every matching
 * token in the body -- including the one in `in.text`, which then expands to
 * `in.(0)` and fails at the numeric constant rather than at the name. Worth a
 * sentence: the error message points at a line that is written correctly.
 */
#define STEP(typed, keys)                                                                          \
    do                                                                                             \
    {                                                                                              \
        const char *tp = (typed);                                                                  \
                                                                                                   \
        memset(&in, 0, sizeof in);                                                                 \
        in.mouse_x = -1;                                                                           \
        in.mouse_y = -1;                                                                           \
        in.text = tp;                                                                              \
        in.text_len = tp ? (ar_u32)strlen(tp) : 0u;                                                \
        in.keys_pressed = (keys);                                                                  \
        frame(c, &in, s);                                                                          \
    } while (0)

#define WANT(cond, what)                                                                           \
    do                                                                                             \
    {                                                                                              \
        printf("  %-4s %s\n", (cond) ? "ok" : "FAIL", what);                                       \
        if (!(cond))                                                                               \
        {                                                                                          \
            fail = 1;                                                                              \
        }                                                                                          \
    } while (0)

/* Tab to the element with this id, by pressing Tab until the focus is there:
   the way a person gets anywhere on a form without a mouse. */
#define TAB_TO(id)                                                                                 \
    do                                                                                             \
    {                                                                                              \
        int guard_;                                                                                \
                                                                                                   \
        for (guard_ = 0; guard_ < 40; ++guard_)                                                    \
        {                                                                                          \
            ar_i32 box_ = g_doc->nodes[node_by_id(id)].box;                                        \
                                                                                                   \
            if (box_ >= 0 && ar_focus_node(c) == box_)                                             \
            {                                                                                      \
                break;                                                                             \
            }                                                                                      \
            STEP(0, AR_KEY_TAB);                                                                   \
        }                                                                                          \
    } while (0)

/* A click in the middle of an element, with the hover frame a press needs. */
#define CLICK(id)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        ar_rect r_ = ar_node_rect(c, g_doc->nodes[node_by_id(id)].box);                            \
                                                                                                   \
        memset(&in, 0, sizeof in);                                                                 \
        in.mouse_x = r_.x + r_.w / 2;                                                              \
        in.mouse_y = r_.y + r_.h / 2;                                                              \
        in.mouse_inside = 1;                                                                       \
        frame(c, &in, s);                                                                          \
        in.mouse_pressed = 1;                                                                      \
        in.mouse_down = 1;                                                                         \
        frame(c, &in, s);                                                                          \
        in.mouse_pressed = 0;                                                                      \
        in.mouse_down = 0;                                                                         \
        in.mouse_released = 1;                                                                     \
        frame(c, &in, s);                                                                          \
        in.mouse_released = 0;                                                                     \
        in.mouse_inside = 0;                                                                       \
        in.mouse_x = -1;                                                                           \
        in.mouse_y = -1;                                                                           \
        frame(c, &in, s);                                                                          \
    } while (0)

    printf("areole %s -- example 16 selftest\n\n", ar_version());

    STEP(0, 0);
    WANT(!ar_has_focus(c), "a form starts with nothing focused");

    /*
     * A second settling frame, and the pixel gate below does not work without
     * it.
     *
     * The first frame of a document damages the whole surface, so *everything*
     * repaints whether or not anything asked it to -- which means a Tab on the
     * second frame draws a correct ring even with damage tracking completely
     * broken. Stubbing the digest fix out left this gate green until this line
     * existed. A gate that only ever runs against a full repaint cannot see
     * the class of bug that cost this release its focus ring.
     */
    STEP(0, 0);

    STEP(0, AR_KEY_TAB);
    WANT(ar_has_focus(c), "tab focuses something");
    WANT(ar_focus_is_visible(c), "and a key-driven focus draws its ring");

    /*
     * And the ring is on the screen, which is a different claim.
     *
     * The two checks above passed through every one of the three faults that
     * kept the ring off the surface for the whole release. This is the one
     * that could not.
     */
    {
        const char *why = "";
        int         ok = ring_check(c, s, 1, &why);

        WANT(ok, why);
    }

    /*
     * And the other half of the pair, which is the whole of `:focus-visible`.
     *
     * A click focuses the field so typing arrives and draws *no* ring, because
     * you know where you just clicked.
     */
    {
        ar_rect     box = ar_node_rect(c, ar_focus_node(c));
        const char *why = "";
        int         ok;

        memset(&in, 0, sizeof in);
        in.mouse_x = box.x + box.w / 2;
        in.mouse_y = box.y + box.h / 2;
        in.mouse_inside = 1;

        /* One frame with the cursor there and no button, because a press
           focuses whatever is under the *hover* chain and hover resolves from
           the previous frame's tree -- so a press on the first frame the
           cursor exists finds nothing under it and clears the focus. */
        frame(c, &in, s);

        in.mouse_pressed = 1;
        in.mouse_down = 1;
        frame(c, &in, s);
        in.mouse_pressed = 0;
        in.mouse_down = 0;
        in.mouse_released = 1;
        frame(c, &in, s);

        WANT(ar_has_focus(c), "a click focuses too");
        WANT(!ar_focus_is_visible(c), "but says its focus should not be drawn");
        ok = ring_check(c, s, 0, &why);
        WANT(ok, why);
    }

    /* Back to the keyboard, so the field the checks below type into is the one
       the Tab above chose rather than whatever the click landed on. */
    STEP(0, AR_KEY_TAB_BACK);
    STEP(0, AR_KEY_TAB);

    STEP("Ada", 0);
    WANT(value_is(c, "name", "Ada"), "typing reaches the first field");

    STEP(0, AR_KEY_TAB);
    WANT(value_is(c, "note", "leave at the door"),
         "the second field starts from its value attribute");

    STEP(0, AR_KEY_TAB_BACK);
    WANT(value_is(c, "name", "Ada"), "and going back finds what was typed, not the markup");

    /* The caret moves by cluster and the selection has a direction. */
    STEP(0, AR_KEY_LEFT | AR_KEY_SHIFT);
    STEP("i", 0);
    WANT(value_is(c, "name", "Adi"), "shift-left selects and typing replaces");

    /*
     * A blink is the caret's column and nothing else.
     *
     * 0.10.0's seventh criterion, measured on the surface: a frame in which
     * only the blink phase moved presents fewer than two thousand pixels. Run
     * against a clock this test owns, stepped by exactly one blink, because
     * waiting for a real one would make the selftest take half a second and
     * still not know which phase it was in.
     */
    {
        ar_i32 px;

        ar_set_clock(c, fake_clock);
        STEP(0, AR_KEY_END);
        STEP(0, 0);
        g_fake_us += 530000u;
        STEP(0, 0);
        px = presented_px(c);
        WANT(px > 0 && px < 2000, "a blink repaints the caret and nothing else");
        printf("       (%d pixels presented for one blink)\n", (int)px);
        g_fake_us += 530000u;
        STEP(0, 0);
        WANT(ar_caret_wait_us(c) > 0 && ar_caret_wait_us(c) <= 530000u,
             "and says how long until the next one");
    }

    /* A password draws one mask per character and never the characters. */
    TAB_TO("pin");
    {
        ar_i32      box = g_doc->nodes[node_by_id("pin")].box;
        const char *shown = ar_node_text(c, box + 1);

        WANT(strstr(shown, "1234") == 0 && strlen(shown) >= 4,
             "a password draws masks, not its text");
        WANT(value_is(c, "pin", "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2"),
             "and tells a reader its length, not its characters");
    }

    /* Up and Down step a number, within its limits. */
    TAB_TO("qty");
    STEP(0, AR_KEY_UP);
    WANT(value_is(c, "qty", "3"), "up steps a number field");
    STEP(0, AR_KEY_DOWN);
    STEP(0, AR_KEY_DOWN);
    STEP(0, AR_KEY_DOWN);
    STEP(0, AR_KEY_DOWN);
    WANT(value_is(c, "qty", "1"), "and down stops at its minimum");

    /* A textarea takes Enter as a newline and keeps its content as its value. */
    TAB_TO("addr");
    WANT(value_is(c, "addr", "12 Mill Lane"), "a textarea starts from its content");
    STEP(0, AR_KEY_END | AR_KEY_CTRL);
    STEP(0, AR_KEY_ENTER);
    STEP("Leeds", 0);
    WANT(value_is(c, "addr", "12 Mill Lane\nLeeds"), "and Enter in it is a newline");

    /* A select opens on Space, the arrows choose, Enter closes. */
    TAB_TO("size");
    WANT(value_is(c, "size", "Medium"), "a select shows its selected option");
    /* Activation lands on the next frame, as hover and focus do: the frame
       that sees the key has already styled the select as shut. */
    STEP(0, AR_KEY_SPACE);
    STEP(0, 0);
    {
        ar_a11y_item items[128];
        ar_i32       n = ar_a11y_tree(c, g_doc, items, 128), k, open = 0, options = 0;

        for (k = 0; k < n && k < 128; ++k)
        {
            if (items[k].node == node_by_id("size") && (items[k].state & AR_A11Y_EXPANDED))
            {
                open = 1;
            }
            if (items[k].role == AR_ROLE_OPTION)
            {
                ++options;
            }
        }
        WANT(open, "space opens it");
        WANT(options == 4, "and its four options are in the tree while it is open");
    }
    STEP(0, AR_KEY_DOWN);
    WANT(value_is(c, "size", "Large"), "down chooses the next option, through the optgroup");
    STEP(0, AR_KEY_ENTER);
    WANT(value_is(c, "size", "Large"), "enter closes it on that choice");

    /* A slider steps with the arrows. */
    TAB_TO("tip");
    STEP(0, AR_KEY_RIGHT);
    STEP(0, AR_KEY_RIGHT);
    WANT(value_is(c, "tip", "7"), "the arrows step a slider");
    STEP(0, AR_KEY_END);
    WANT(value_is(c, "tip", "20"), "and End takes it to its maximum");

    /* A colour field opens a palette, and a chip in it is a choice. */
    TAB_TO("ribbon");
    STEP(0, AR_KEY_ENTER);
    STEP(0, 0);
    {
        ar_i32 i, chip = -1;
        ar_i32 field = g_doc->nodes[node_by_id("ribbon")].box;

        /* A chip is a box whose parent -- the palette -- is a child of the
           colour field. The last one is the sixteenth colour, magenta. */
        for (i = 0; i < ar_node_count(c); ++i)
        {
            ar_i32 up = ar_node_parent(c, i);

            if (up >= 0 && ar_node_parent(c, up) == field && ar_node_rect(c, i).w == 16)
            {
                chip = i;
            }
        }
        WANT(chip >= 0, "enter opens the colour palette");
        if (chip >= 0)
        {
            ar_rect r = ar_node_rect(c, chip);

            memset(&in, 0, sizeof in);
            in.mouse_x = r.x + 8;
            in.mouse_y = r.y + 8;
            in.mouse_inside = 1;
            frame(c, &in, s);
            in.mouse_pressed = 1;
            in.mouse_down = 1;
            frame(c, &in, s);
            in.mouse_pressed = 0;
            in.mouse_down = 0;
            in.mouse_released = 1;
            frame(c, &in, s);
            STEP(0, 0);
        }
        WANT(value_is(c, "ribbon", "#ff00ff"), "and a click on a chip is the new colour");
    }

    /* A file field asks the embedder, and shows and submits the answer. */
    TAB_TO("photo");
    STEP(0, AR_KEY_SPACE);
    WANT(ar_file_wanted(c) == node_by_id("photo"), "a file field asks for a file");
    ar_file_chosen(c, node_by_id("photo"), "door.jpg", 8);
    STEP(0, 0);
    WANT(value_is(c, "photo", "door.jpg"), "and holds the name it was given");

    /* A label passes its click to its control, both ways it can be written. */
    CLICK("gift");
    {
        ar_i32 lab = -1, i;

        for (i = 0; i < g_doc->node_count; ++i)
        {
            ar_span f = ar_a11y_attr(g_doc, i, "for");

            if (f.p && f.n == 4 && memcmp(f.p, "gift", 4) == 0)
            {
                lab = i;
            }
        }
        WANT(ar_a11y_tree(c, g_doc, 0, 0) > 0, "the tree is there to ask");
        {
            ar_a11y_item items[128];
            ar_i32       n = ar_a11y_tree(c, g_doc, items, 128), k;
            int          checked = 0;

            for (k = 0; k < n && k < 128; ++k)
            {
                if (items[k].node == node_by_id("gift"))
                {
                    checked = (items[k].state & AR_A11Y_CHECKED) != 0;
                }
            }
            WANT(checked, "a click on the box ticks it");
        }
        if (lab >= 0)
        {
            ar_rect      r = ar_node_rect(c, g_doc->nodes[lab].box);
            ar_a11y_item items[128];
            ar_i32       n, k;
            int          checked = 1;

            memset(&in, 0, sizeof in);
            in.mouse_x = r.x + r.w / 2;
            in.mouse_y = r.y + r.h / 2;
            in.mouse_inside = 1;
            frame(c, &in, s);
            in.mouse_pressed = 1;
            in.mouse_down = 1;
            frame(c, &in, s);
            in.mouse_pressed = 0;
            in.mouse_down = 0;
            in.mouse_released = 1;
            frame(c, &in, s);
            STEP(0, 0);
            n = ar_a11y_tree(c, g_doc, items, 128);
            for (k = 0; k < n && k < 128; ++k)
            {
                if (items[k].node == node_by_id("gift"))
                {
                    checked = (items[k].state & AR_A11Y_CHECKED) != 0;
                }
            }
            WANT(!checked, "and a click on its label unticks it again");
        }
    }

    /* The radio group is one tab stop, and the arrows move within it. */
    {
        ar_i32 before = ar_tab_stops(c);

        TAB_TO("soon");
        STEP(0, AR_KEY_DOWN);
        STEP(0, 0);
        WANT(ar_focus_node(c) == g_doc->nodes[node_by_id("evening")].box,
             "down moves the focus to the next radio");
        WANT(before == 15, "and the three radios are one tab stop among fifteen");
        printf("       (%d tab stops)\n", (int)before);
    }

    /*
     * Sent: the form is handed to this program, encoded the way a server
     * expects it, with what the user did rather than what the markup said.
     */
    TAB_TO("send");
    STEP(0, AR_KEY_ENTER);
    {
        ar_i32 by = -2;
        ar_i32 form = ar_form_submitted(c, &by);
        char   data[1024];
        ar_u32 n = form >= 0 ? ar_form_encode(c, form, by, data, sizeof data) : 0;

        WANT(form == node_by_id("order"), "enter on the button submits the form");
        WANT(by == node_by_id("send"), "and says which button did it");
        if (n > 0)
        {
            printf("       %s\n", data);
        }
        WANT(n > 0 && strstr(data, "name=Adi") && strstr(data, "when=evening") &&
                 strstr(data, "addr=12+Mill+Lane%0D%0ALeeds") && strstr(data, "size=l") &&
                 strstr(data, "tip=20") && strstr(data, "ribbon=%23ff00ff") &&
                 strstr(data, "photo=door.jpg") && strstr(data, "news=on") &&
                 strstr(data, "go=send") && strstr(data, "src=example16") &&
                 !strstr(data, "gift=") && !strstr(data, "off="),
             "with what was typed, ticked and chosen");
    }

    /* Start again puts every control back to its markup. */
    TAB_TO("clear");
    STEP(0, AR_KEY_SPACE);
    STEP(0, 0);
    WANT(value_is(c, "name", "") && value_is(c, "size", "Medium") && value_is(c, "tip", "5") &&
             value_is(c, "addr", "12 Mill Lane"),
         "reset puts the form back the way the markup had it");

    /* Every control in the tree has a role and a name, which is the half of
       criterion 6 that can be checked without a screen reader. */
    {
        static ar_a11y_item items[256];
        ar_i32              n = ar_a11y_tree(c, g_doc, items, 256), k, controls = 0, named = 0;

        for (k = 0; k < n && k < 256; ++k)
        {
            ar_u8 r = items[k].role;
            char  name[96];

            if (r == AR_ROLE_BUTTON || r == AR_ROLE_CHECKBOX || r == AR_ROLE_RADIO ||
                r == AR_ROLE_TEXTBOX || r == AR_ROLE_SPINBUTTON || r == AR_ROLE_SLIDER ||
                r == AR_ROLE_COMBOBOX || r == AR_ROLE_PROGRESSBAR || r == AR_ROLE_METER)
            {
                ++controls;
                if (ar_a11y_name(g_doc, items[k].node, name, sizeof name) > 0)
                {
                    ++named;
                }
                else
                {
                    printf("       unnamed: %s\n", role_name(r));
                }
            }
        }
        WANT(controls >= 20 && named == controls, "twenty controls, every one named");
        printf("       (%d controls, %d named)\n", (int)controls, (int)named);
    }

    printf("\n%s\n", fail ? "selftest FAILED" : "selftest passed");
    return fail;

#undef STEP
#undef WANT
#undef TAB_TO
#undef CLICK
}

int main(int argc, char **argv)
{
    ar_ctx     *c;
    int         want_dump = 0, want_selftest = 0, open_select = 0, i;
    const char *ppm_path = 0;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--dump") == 0)
        {
            want_dump = 1;
        }
        else if (strcmp(argv[i], "--selftest") == 0)
        {
            want_selftest = 1;
        }
        else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
        {
            ppm_path = argv[++i];
        }
        else if (strcmp(argv[i], "--html") == 0)
        {
            /* The page, exactly as it is parsed: form.html is this output. */
            int k;

            for (k = 0; k < DOC_N; ++k)
            {
                fputs(DOC[k], stdout);
            }
            return 0;
        }
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
        {
            /* Draws at that many times the window's pixels: 2 or 3 for
               smoother text and edges, 0.5 for a slow machine. */
            g_scale = parse_scale(argv[++i]);
            g_scale = g_scale < 250 ? 250 : g_scale > 8000 ? 8000 : g_scale;
        }
        else if (strcmp(argv[i], "--ppm-open") == 0 && i + 1 < argc)
        {
            /* The same picture with the select open, which is the one state of
               the form a still image of it untouched cannot show. */
            ppm_path = argv[++i];
            open_select = 1;
        }
    }

    c = ar_init_ex(g_mem, (ar_u32)sizeof g_mem, 256, 64 * 1024);
    if (!c)
    {
        printf("could not initialise\n");
        return 1;
    }
    if (!load_face(c))
    {
        printf("no outline face found -- falling back to the built-in 8x8\n");
    }
    build(c);

    {
        static char whole[8192];
        ar_u32      used = 0;
        int         k;

        for (k = 0; k < DOC_N; ++k)
        {
            ar_u32 n = (ar_u32)strlen(DOC[k]);

            if (used + n + 1 < sizeof whole)
            {
                memcpy(whole + used, DOC[k], n);
                used += n;
            }
        }
        whole[used] = 0;
        g_doc = ar_html_parse_into(c, whole, used);
    }
    if (!g_doc)
    {
        printf("could not parse the document\n");
        return 1;
    }
    ar_doc_stylesheets(c, g_doc);

    if (ppm_path)
    {
        ppm(c, ppm_path, open_select);
        return 0;
    }

    if (want_dump || want_selftest)
    {
        static ar_u32 pixels[WIN_W * WIN_H];
        ar_surface    surf;

        surf.pixels = pixels;
        surf.w = WIN_W;
        surf.h = WIN_H;
        surf.stride = WIN_W;

        if (want_selftest)
        {
            return selftest(c, &surf);
        }
        {
            ar_input in;

            memset(&in, 0, sizeof in);
            in.mouse_x = -1;
            in.mouse_y = -1;
            frame(c, &in, &surf);
        }
        dump(c);
        return 0;
    }

    {
        ar_win *win = ar_win_open("areole - a form", WIN_W, WIN_H);

        if (!win)
        {
            printf("could not open a window\n");
            return 1;
        }
        ar_set_clock(c, ar_time_us);
        ar_win_a11y(win, c, g_doc);
        if (g_scale != 1000 && !ar_win_set_render_scale(win, c, g_scale))
        {
            printf("no memory to render at %ld/1000 -- drawing at the window's own size\n",
                   (long)g_scale);
        }
        printf("areole %s -- Tab to move, Space or Enter to press, type into a field.\n",
               ar_version());
        printf("Ctrl+A/Z/Y select all, undo, redo; Ctrl+C/X/V the clipboard.\n");

        while (ar_win_pump(win))
        {
            const ar_input *in = ar_win_input(win);
            ar_i32          region, form, by;

            /* Woken by the caret's timer and nothing else: the blink is one
               column, painted from the frame already standing, and building
               a whole frame to find that out is what ar_frame_blink saves. */
            if (ar_win_idle(win))
            {
                ar_win_present(win, ar_frame_blink(c, ar_win_surface(win)));
                ar_win_wake_after(win, ar_caret_wait_us(c));
                continue;
            }

            frame(c, in, ar_win_surface(win));
            for (region = 0; region < ar_damage_count(c); ++region)
            {
                ar_win_present(win, ar_damage_rect(c, region));
            }
            ar_frame_presented(c);

            /* What a server would have been sent, which is what a form does
               without a network: it hands its data to the program. */
            form = ar_form_submitted(c, &by);
            if (form >= 0)
            {
                static char data[8192];

                ar_form_encode(c, form, by, data, sizeof data);
                printf("submitted: %s\n", data);
                ar_win_set_title(win, "areole - a form (submitted)");
            }

            ar_win_after_frame(win, c);
            if (ar_needs_redraw(c))
            {
                ar_win_wake(win);
            }
        }
        ar_win_close(win);
    }
    return 0;
}
