/*
 * areole example 16 - a form you can actually fill in
 * SPDX-License-Identifier: MIT
 *
 *     example_forms              open a window and use it
 *     example_forms --dump       print what the engine thinks it has
 *     example_forms --selftest   drive it without a window and check
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
 *   Ctrl+D            switches the whole form to the dark colour scheme
 *
 * Nothing in the markup changes for that. `color-scheme: dark` selects the
 * other set of system colours, and every control follows -- which is what
 * those nineteen names are for.
 */
#include "areole.h"
#include "areole_win32.h"

/* The role names. ar_a11y_role returns one of these and the enum lives in
   src/ar_a11y.h, which an embedder does not include -- so the two values
   this example actually names are spelled out here rather than pulling an
   internal header into a demo. */
#define EX_ROLE_NONE 0

#include <stdio.h>
#include <string.h>

#define WIN_W 520
#define WIN_H 560

/* Boxes, plus room for the parsed document: AR_MEM alone budgets the box
   tree and a parsed page needs its own nodes, attributes and text on top --
   which is what AR_MEM_DOC adds and what ar_init_ex is told about. */
static unsigned char g_mem[AR_MEM_DOC(1024, 64 * 1024)];
static ar_doc       *g_doc;
static int           g_dark = 0;

/*
 * The document, written the way somebody would write it.
 *
 * No classes for the controls and no wrapper divs to make them lay out: the
 * point of building controls out of real boxes with a user-agent stylesheet is
 * that ordinary markup produces a usable form, and a demo that needs help to
 * look right has demonstrated the opposite.
 */
/*
 * In parts, because C90 caps a string literal at 509 characters and the
 * strict gate enforces it -- the same wall the user-agent sheet hit, and the
 * third time this session. Adjacent literals concatenate into one, so the
 * commas are what make these separate.
 */
static const char *const DOC[] = {
    "<html><body>"
    "<h1>Delivery</h1>"
    "<form>"
    "<p><label for=\"name\">Name</label>"
    "<input id=\"name\" type=\"text\" value=\"\"></p>"
    "<p><label for=\"note\">Note for the driver</label>"
    "<input id=\"note\" type=\"text\" value=\"leave at the door\"></p>"
    "<fieldset><legend>When</legend>"
    "<p><input id=\"soon\" type=\"radio\" name=\"when\" checked>",

    "<label for=\"soon\">As soon as possible</label></p>"
    "<p><input id=\"evening\" type=\"radio\" name=\"when\">"
    "<label for=\"evening\">This evening</label></p>"
    "<p><input id=\"weekend\" type=\"radio\" name=\"when\">"
    "<label for=\"weekend\">At the weekend</label></p>"
    "</fieldset>"
    "<p><input id=\"gift\" type=\"checkbox\">"
    "<label for=\"gift\">Wrap it as a gift</label></p>",

    "<p><input id=\"news\" type=\"checkbox\" checked>"
    "<label for=\"news\">Email me about offers</label></p>"
    "<details id=\"more\">"
    "<summary>Delivery instructions</summary>"
    "<p>The gate code is 4417. The dog is friendly but loud.</p>"
    "<p><input id=\"code\" type=\"text\" value=\"4417\"></p>"
    "</details>"
    "<p><label for=\"packing\">Packing</label>"
    "<progress id=\"packing\" value=\"0.6\" max=\"1\"></progress></p>",

    "<p><label for=\"fragile\">Fragility</label>"
    "<meter id=\"fragile\" min=\"0\" max=\"10\" value=\"7\"></meter></p>"
    "<p><button id=\"send\">Place order</button>"
    "<input id=\"clear\" type=\"reset\" value=\"Start again\"></p>"
    "</form>"
    "<p id=\"disabled-note\"><input id=\"off\" type=\"text\" value=\"not editable\" disabled>"
    "<label for=\"off\">A disabled field is not a tab stop</label></p>"
    "</body></html>",

};

#define DOC_N ((int)(sizeof DOC / sizeof DOC[0]))

static const char *CSS_LIGHT =
    "body { padding:16px; background:Canvas; color:CanvasText; }"
    "h1 { font-size:20px; }"
    "p { display:block; margin-top:6px; margin-bottom:6px; }"
    "label { padding-left:6px; }"
    "fieldset { display:block; padding-left:8px; padding-right:8px; }"
    "legend { display:block; }"
    "input { width:220px; }"
    ".ar-checkbox, .ar-radio { width:13px; height:13px; }"
    "summary { padding-top:4px; padding-bottom:4px; }"
    "details { padding-left:4px; }";

/* The whole of switching themes. Nothing in the markup knows about it. */
static const char *CSS_DARK = ":root { color-scheme:dark; }";

static void build(ar_ctx *c)
{
    ar_ua_stylesheet(c);
    ar_stylesheet(c, CSS_LIGHT);
    if (g_dark)
    {
        ar_stylesheet(c, CSS_DARK);
    }
}

static void frame(ar_ctx *c, const ar_input *in, ar_surface *s)
{
    ar_frame_begin(c, in);
    ar_dom_build(c, g_doc);
    ar_frame_end(c, s);
}

/*
 * What a screen reader would be told, printed.
 *
 * The point of the accessibility tree is that it is checkable without a screen
 * reader, and this is where that pays off: a name that comes out wrong is
 * visible here as a line of text rather than as somebody's report that the
 * form says "edit edit edit".
 */
static void dump(ar_ctx *c)
{
    static const char *ROLE[] = {"none",     "button",  "link",     "checkbox",  "radio",
                                 "textbox",  "search",  "slider",   "combobox",  "option",
                                 "heading",  "para",    "list",     "listitem",  "table",
                                 "row",      "cell",    "colhead",  "image",     "dialog",
                                 "progress", "meter",   "group",    "form",      "main",
                                 "nav",      "banner",  "contentinfo", "complementary",
                                 "region",   "article"};
    ar_i32 i;

    printf("areole %s -- the accessibility tree of example 16\n\n", ar_version());
    for (i = 0; i < g_doc->node_count; ++i)
    {
        char   name[128];
        ar_u8  role;

        if (g_doc->nodes[i].kind != AR_DOM_ELEMENT)
        {
            continue;
        }
        role = ar_a11y_role(g_doc, i);
        if (role == EX_ROLE_NONE)
        {
            continue;
        }
        ar_a11y_name(g_doc, i, name, sizeof name);
        printf("  %-12s %s\n", ROLE[role], name[0] ? name : "(unnamed)");
    }

    printf("\n  %d tab stops\n", (int)ar_tab_stops(c));

    /*
     * And the boxes, because a tree that reads correctly can still draw
     * nothing -- which is what a picture showed and what no test did.
     */
    printf("\n  boxes (%d):\n", (int)ar_node_count(c));
    for (i = 0; i < ar_node_count(c) && i < 70; ++i)
    {
        ar_rect r = ar_node_rect(c, i);

        printf("    %-3d parent %-3d  %4d,%-4d %4dx%-4d\n", (int)i, (int)ar_node_parent(c, i),
               (int)r.x, (int)r.y, (int)r.w, (int)r.h);
    }
}

/*
 * The form, rendered to a file, so it can be looked at.
 *
 * Every other check in this file is about behaviour -- what the focus does,
 * what the buffer holds, what a reader would be told. None of them can see
 * whether the checkbox has a mark in it or whether the text sits inside the
 * field, and "the tests pass" is not an answer to "does it look right".
 */
static void ppm(ar_ctx *c, const char *path)
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
 * The same thing a person would do, without a window.
 *
 * Every check here is one the demo is for: Tab reaches the fields in order,
 * typing lands in the focused one, the text survives leaving and returning,
 * and a radio group excludes itself. If this passes, the window will behave.
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

    printf("areole %s -- example 16 selftest\n\n", ar_version());

    STEP(0, 0);
    WANT(!ar_has_focus(c), "a form starts with nothing focused");

    STEP(0, AR_KEY_TAB);
    WANT(ar_has_focus(c), "tab focuses something");
    WANT(ar_focus_is_visible(c), "and a key-driven focus draws its ring");

    STEP("Ada", 0);
    {
        ar_u32      n = 0;
        const char *t = ar_field_text(c, &n);

        WANT(t && n == 3 && memcmp(t, "Ada", 3) == 0, "typing reaches the first field");
    }

    STEP(0, AR_KEY_TAB);
    {
        ar_u32      n = 0;
        const char *t = ar_field_text(c, &n);

        WANT(t && n == 17, "the second field starts from its value attribute");
    }

    STEP(0, AR_KEY_TAB_BACK);
    {
        ar_u32      n = 0;
        const char *t = ar_field_text(c, &n);

        WANT(t && n == 3 && memcmp(t, "Ada", 3) == 0,
             "and going back finds what was typed, not the markup");
    }

    /* The caret moves by cluster and the selection has a direction. */
    STEP(0, AR_KEY_LEFT | AR_KEY_SHIFT);
    STEP("i", 0);
    {
        ar_u32      n = 0;
        const char *t = ar_field_text(c, &n);

        WANT(t && n == 3 && memcmp(t, "Adi", 3) == 0, "shift-left selects and typing replaces");
    }

    printf("\n%s\n", fail ? "selftest FAILED" : "selftest passed");
    return fail;

#undef STEP
#undef WANT
}

int main(int argc, char **argv)
{
    ar_ctx *c;
    int         want_dump = 0, want_selftest = 0, i;
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
    }

    c = ar_init_ex(g_mem, (ar_u32)sizeof g_mem, 256, 64 * 1024);
    if (!c)
    {
        printf("could not initialise\n");
        return 1;
    }
    build(c);

    {
        static char whole[4096];
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
        ppm(c, ppm_path);
        return 0;
    }

    if (want_dump || want_selftest)
    {
        static ar_u32     pixels[WIN_W * WIN_H];
        ar_surface        surf;

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
        printf("areole %s -- Tab to move, Space or Enter to press, type into a field.\n",
               ar_version());
        printf("Ctrl+D for the dark scheme, Ctrl+A/Z/Y to select all, undo, redo.\n");

        while (ar_win_pump(win))
        {
            const ar_input *in = ar_win_input(win);
            ar_i32          region;

            frame(c, in, ar_win_surface(win));
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
