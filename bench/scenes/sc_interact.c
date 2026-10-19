/*
 * areole benchmark - interaction.
 * SPDX-License-Identifier: MIT
 *
 * 0.10.0 made a document something a person can type into, and three of the
 * things it added run on frames nobody asked for: a keystroke rebuilds the
 * frame, a blinking caret asks for one twice a second, and a screen reader
 * walks the tree whenever it changes. None of them had a scene, so none of
 * them had a number -- these are the numbers.
 *
 * All three go through a parsed document and ar_dom_build, because that is the
 * path a form takes. A field built with ar_begin pays the same edit and the
 * same blink, and a scene for it would measure the same code twice.
 */
#include "areole.h"
#include "bench.h"

#include <string.h>

/* Caller storage, as in sc_html.c, so the arena holds the frame and not the
   document. */
static ar_dom_node g_nodes[2048];
static ar_attr     g_attrs[2048];
static char        g_text[16384];
static char        g_scratch[8192];
static ar_doc      g_doc;

static char   g_page[16384];
static ar_u32 g_page_len;

static void parse(void)
{
    memset(&g_doc, 0, sizeof g_doc);
    g_doc.nodes = g_nodes;
    g_doc.node_cap = (ar_i32)(sizeof g_nodes / sizeof g_nodes[0]);
    g_doc.attrs = g_attrs;
    g_doc.attr_cap = (ar_i32)(sizeof g_attrs / sizeof g_attrs[0]);
    g_doc.text = g_text;
    g_doc.text_cap = (ar_u32)sizeof g_text;
    ar_html_parse(&g_doc, g_page, g_page_len, g_scratch, (ar_u32)sizeof g_scratch);
}

static void put(const char *s)
{
    ar_u32 k = (ar_u32)strlen(s);

    if (g_page_len + k < sizeof g_page - 1u)
    {
        memcpy(g_page + g_page_len, s, k);
        g_page_len += k;
        g_page[g_page_len] = 0;
    }
}

/* One frame of the document with this input. */
static void step(bench_env *e, const char *text, ar_u32 keys)
{
    ar_input in;

    memset(&in, 0, sizeof in);
    in.mouse_x = -1;
    in.mouse_y = -1;
    in.text = text;
    in.text_len = text ? (ar_u32)strlen(text) : 0;
    in.keys_pressed = keys;
    ar_frame_begin(e->ui, &in);
    if (e->full_repaint)
    {
        ar_invalidate_all(e->ui);
    }
    ar_dom_build(e->ui, &g_doc);
    ar_frame_end(e->ui, &e->surface);
    ar_frame_presented(e->ui);
}

/* ------------------------------------------------------------------------
 * A keystroke into a long field
 *
 * 0.10.0's document asks that a keystroke into a two-thousand-character
 * textarea not cost a frame anybody could notice. A textarea's text wraps, so
 * every edit re-wraps the lot, and that re-wrap is the cost this watches: a
 * field that re-measured its whole value per glyph instead of per line would
 * show here first.
 *
 * Frames alternate a character and a Backspace so the value stays two thousand
 * characters long however many samples the harness takes.
 * ------------------------------------------------------------------------ */
static void init_keystroke(bench_env *e)
{
    int i;

    g_page_len = 0;
    put("<!doctype html><html><body><textarea cols=\"60\" rows=\"20\">");
    for (i = 0; i < 25; ++i)
    {
        /* 80 characters, so twenty-five of them are the two thousand. */
        put("The quick brown fox jumps over the lazy dog, and then it does so again, thrice. ");
    }
    put("</textarea></body></html>");
    parse();
    ar_ua_stylesheet(e->ui);

    /* Focus it, and let the frame that placed the caret settle. */
    step(e, 0, 0);
    step(e, 0, AR_KEY_TAB);
    step(e, 0, AR_KEY_END);
    step(e, 0, 0);
}

static void frame_keystroke(bench_env *e)
{
    if (e->frame & 1u)
    {
        step(e, 0, AR_KEY_BACKSPACE);
    }
    else
    {
        step(e, "x", 0);
    }
}

/* ------------------------------------------------------------------------
 * A caret blink, two ways
 *
 * What happens while nobody does anything: a focused field, and the caret
 * going on or off. Its damage is the caret's column -- ar_test checks the
 * pixel count -- and the two scenes are the two ways to pay for it.
 *
 * caret_blink is ar_frame_blink: the column repainted from the frame that is
 * standing, which is what a backend does when the caret's timer was all that
 * woke it. caret_blink_frame is the whole frame -- built, styled and laid out
 * to learn that one column changed -- which is what a blink costs a program
 * that does not ask. The budget is written against the first; the second is
 * kept so the saving stays measured and the fallback cannot quietly grow.
 *
 * The clock runs half a second ahead each frame, so every timed frame is a
 * blink and not one in thirty. It still reads the real clock underneath, so
 * the phase timings stay true.
 * ------------------------------------------------------------------------ */
static ar_u32 g_clock_ahead;

static ar_u32 clock_ahead(void)
{
    return bench_time_us() + g_clock_ahead;
}

static void init_blink(bench_env *e)
{
    int i;

    g_page_len = 0;
    put("<!doctype html><html><body><form>");
    for (i = 0; i < 6; ++i)
    {
        put("<p><label>A field <input type=\"text\" value=\"some text\"></label></p>");
    }
    put("<p><button>Send</button></p></form></body></html>");
    parse();
    ar_ua_stylesheet(e->ui);
    g_clock_ahead = 0;
    ar_set_clock(e->ui, clock_ahead);
    step(e, 0, 0);
    step(e, 0, AR_KEY_TAB);
    step(e, 0, 0);
}

static void frame_blink(bench_env *e)
{
    g_clock_ahead += 530000u; /* AR_CARET_BLINK_US: one phase a frame */
    (void)ar_frame_blink(e->ui, &e->surface);
}

static void frame_blink_frame(bench_env *e)
{
    g_clock_ahead += 530000u; /* AR_CARET_BLINK_US: one phase a frame */
    step(e, 0, 0);
}

/* ------------------------------------------------------------------------
 * The accessibility tree
 *
 * What a screen reader's backend does when the generation moves: build the
 * tree again from the document and the boxes. A form of sixty controls, so the
 * per-node cost has enough nodes to be a rate and not a constant.
 *
 * Only the tree is timed. The frame it reads is built once at init, which is
 * the honest shape -- a backend asks between frames, not inside one.
 * ------------------------------------------------------------------------ */
static ar_a11y_item g_items[512];

static void init_a11y(bench_env *e)
{
    int i;

    g_page_len = 0;
    put("<!doctype html><html><body><h1>Order</h1><form>");
    for (i = 0; i < 10; ++i)
    {
        put("<fieldset><legend>Item</legend>");
        put("<p><label>Name <input type=\"text\" value=\"a name\"></label></p>");
        put("<p><label><input type=\"checkbox\" checked> Gift</label></p>");
        put("<p><label><input type=\"radio\" name=\"r\"> Soon</label></p>");
        put("<p><select><option>Small<option selected>Medium</select></p>");
        put("<p><input type=\"range\" value=\"40\"></p>");
        put("</fieldset>");
    }
    put("<p><button>Send</button></p></form></body></html>");
    parse();
    ar_ua_stylesheet(e->ui);
    step(e, 0, 0);
}

static void frame_a11y(bench_env *e)
{
    (void)ar_a11y_tree(e->ui, &g_doc, g_items, (ar_i32)(sizeof g_items / sizeof g_items[0]));
}

static const bench_scene SC_KEYSTROKE = {
    "field_keystroke",
    "interact",
    "one character into a 2,000 character textarea: the edit, the re-wrap and the frame",
    800,
    600,
    1,
    init_keystroke,
    frame_keystroke};

static const bench_scene SC_BLINK = {
    "caret_blink", "interact", "a blink by ar_frame_blink: the caret's column, from the last frame",
    800,           600,        1,
    init_blink,    frame_blink};

static const bench_scene SC_BLINK_FRAME = {"caret_blink_frame",
                                           "interact",
                                           "the same blink by a whole frame, for the saving",
                                           800,
                                           600,
                                           1,
                                           init_blink,
                                           frame_blink_frame};

static const bench_scene SC_A11Y = {
    "a11y_tree", "interact", "the accessibility tree of a sixty-control form, rebuilt", 800, 600, 1,
    init_a11y,   frame_a11y};

void bench_register_interact(void)
{
    bench_register(&SC_KEYSTROKE);
    bench_register(&SC_BLINK);
    bench_register(&SC_BLINK_FRAME);
    bench_register(&SC_A11Y);
}
