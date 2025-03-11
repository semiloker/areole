/*
 * areole - the context: memory, the box tree, widgets, and painting.
 * SPDX-License-Identifier: MIT
 */
#include "ar_node.h"

#include <string.h>

/* AR_MEM promises a byte budget per box to callers who want a static array,
   which means the promise has to be checked rather than remembered. If a
   structure grows past it the build stops here instead of running out of
   arena at some unlucky tree depth in front of a user. */
typedef char ar__mem_budget_holds[(sizeof(ar_node) + sizeof(ar_slot) <= AR_BYTES_PER_BOX) ? 1 : -1];

#define AR_MAX_RULES 256

typedef char ar__mem_fixed_holds
    [(sizeof(ar_ctx) + AR_MAX_RULES * sizeof(ar_rule) + 1024 <= AR_MEM_FIXED) ? 1 : -1];

/* ------------------------------------------------------------------------
 * Keys
 *
 * A box needs an identity that survives to the next frame, because that is
 * where its hover state comes from. The identity is its selector mixed with
 * its parent and its position among its siblings, so two buttons declared with
 * the same class in the same loop still get different keys.
 * ------------------------------------------------------------------------ */
static ar_u32 ar__mix(ar_u32 a, ar_u32 b)
{
    a ^= b + 0x9E3779B9u + (a << 6) + (a >> 2);
    return a ? a : 1u;
}

ar_slot *ar_ctx_slot(ar_ctx *c, ar_u32 key)
{
    ar_u32   mask = (ar_u32)c->slot_cap - 1u;
    ar_u32   i = key & mask;
    ar_u32   probe;
    ar_slot *stale = 0;

    for (probe = 0; probe < 32u; ++probe)
    {
        ar_slot *s = &c->slots[i];

        if (s->key == key)
        {
            return s;
        }
        if (s->last_frame == 0)
        {
            s->key = key;
            s->rect = ar_rect_make(0, 0, 0, 0);
            s->scroll = 0;
            return s;
        }
        /* A box that has not been declared for two frames is gone. Reusing
           its slot is what stops the table filling up over a session as
           panels open and close. */
        if (!stale && s->last_frame + 2u <= c->frame)
        {
            stale = s;
        }
        i = (i + 1u) & mask;
    }

    if (stale)
    {
        stale->key = key;
        stale->rect = ar_rect_make(0, 0, 0, 0);
        stale->scroll = 0;
        return stale;
    }
    return 0; /* the table is genuinely full; the box loses its hover, not its pixels */
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */
static ar_u32 ar__round_pow2(ar_u32 v)
{
    ar_u32 p = 1;
    while (p < v)
    {
        p <<= 1;
    }
    return p;
}

ar_ctx *ar_init(void *mem, ar_u32 size)
{
    ar_arena a;
    ar_ctx  *c;
    ar_rule *rules;
    ar_u32   boxes, slots;

    if (!mem || size < AR_MEM_FIXED)
    {
        return 0;
    }

    ar_arena_init(&a, mem, size);
    c = (ar_ctx *)ar_arena_persist(&a, (ar_u32)sizeof(ar_ctx));
    if (!c)
    {
        return 0;
    }

    memset(c, 0, sizeof *c);
    c->arena = a; /* from here on the arena lives inside the thing it allocated */

    rules = (ar_rule *)ar_arena_persist(&c->arena, AR_MAX_RULES * (ar_u32)sizeof(ar_rule));
    if (!rules)
    {
        return 0;
    }
    ar_sheet_init(&c->sheet, rules, AR_MAX_RULES);

    boxes = (size - AR_MEM_FIXED) / AR_BYTES_PER_BOX;
    if (boxes < 32u)
    {
        boxes = 32u;
    }

    /* Half full at the stated box count, so probes stay short. */
    slots = ar__round_pow2(boxes * 2u);
    c->slots = (ar_slot *)ar_arena_persist(&c->arena, slots * (ar_u32)sizeof(ar_slot));
    if (!c->slots)
    {
        return 0;
    }
    memset(c->slots, 0, slots * sizeof(ar_slot));
    c->slot_cap = (ar_i32)slots;

    c->box_budget = (ar_i32)boxes;
    c->frame = 1; /* zero means an unused slot, so frames start at one */
    ar_perf_reset(&c->perf);
    return c;
}

static ar_u32 ar__now(ar_ctx *c)
{
    /* With no clock lent to it, the context reports every phase as taking no
       time rather than inventing a number. Zeros in the overlay are honest;
       a fabricated breakdown is not. */
    return c->clock ? c->clock() : c->perf.mark;
}

void ar_set_clock(ar_ctx *c, ar_u32 (*clock_us)(void))
{
    c->clock = clock_us;
}

void ar_stylesheet(ar_ctx *c, const char *css)
{
    ar_sheet_parse(&c->sheet, css);
}

ar_u32 ar_stylesheet_errors(const ar_ctx *c)
{
    return c->sheet.errors;
}

int ar_overflowed(const ar_ctx *c)
{
    return c->overflowed;
}

int ar_unbalanced(const ar_ctx *c)
{
    return c->unbalanced;
}

int ar_needs_redraw(const ar_ctx *c)
{
    return c->hot_changed;
}

ar_perf *ar_perf_of(ar_ctx *c)
{
    return &c->perf;
}

void ar_memory_stats(const ar_ctx *c, ar_u32 *persist, ar_u32 *frame_now, ar_u32 *frame_peak,
                     ar_u32 *available)
{
    if (persist)
    {
        *persist = ar_arena_persist_used(&c->arena);
    }
    if (frame_now)
    {
        *frame_now = ar_arena_frame_used(&c->arena);
    }
    if (frame_peak)
    {
        *frame_peak = ar_arena_frame_peak(&c->arena);
    }
    if (available)
    {
        *available = ar_arena_available(&c->arena);
    }
}

/* ------------------------------------------------------------------------
 * Frame
 * ------------------------------------------------------------------------ */
void ar_frame_begin(ar_ctx *c, const ar_input *in)
{
    ar_u32 room;

    ar_perf_begin(&c->perf, ar__now(c));

    /* Releasing the whole previous tree. One integer store. */
    ar_arena_frame_reset(&c->arena);

    c->node_count = 0;
    c->depth = 0;
    c->overflowed = 0;
    c->unbalanced = 0;
    c->frame++;

    c->mouse_x = in ? in->mouse_x : -1;
    c->mouse_y = in ? in->mouse_y : -1;
    c->mouse_down = in ? in->mouse_down : 0;
    c->mouse_pressed = in ? in->mouse_pressed : 0;
    c->mouse_released = in ? in->mouse_released : 0;
    c->mouse_inside = in ? in->mouse_inside : 0;

    /* A press latches whichever box the cursor was over, and a release only
       counts as a click if it lands on the same one. Dragging off a button
       and letting go therefore does nothing, which is what every other
       toolkit does and what people expect. */
    if (c->mouse_pressed & AR_MOUSE_LEFT)
    {
        c->active = c->hot;
    }
    c->clicked = 0;
    if (c->mouse_released & AR_MOUSE_LEFT)
    {
        if (c->active && c->active == c->hot)
        {
            c->clicked = c->active;
        }
        c->active = 0;
    }

    /* The tree has to be contiguous to be indexed, so the whole array is
       reserved up front rather than grown a box at a time. It is capped at the
       budget the caller sized the block for, not at whatever happens to be
       left: reserving every spare byte would make the arena figure in the
       overlay meaningless and would hide a runaway tree instead of reporting
       it. */
    room = ar_arena_available(&c->arena);
    c->node_cap = c->box_budget;
    if ((ar_u32)c->node_cap * (ar_u32)sizeof(ar_node) > room)
    {
        c->node_cap = (ar_i32)(room / (ar_u32)sizeof(ar_node));
    }
    c->nodes =
        c->node_cap > 0
            ? (ar_node *)ar_arena_frame(&c->arena, (ar_u32)c->node_cap * (ar_u32)sizeof(ar_node))
            : 0;
    if (!c->nodes)
    {
        c->node_cap = 0;
        c->overflowed = 1;
    }
}

/* ------------------------------------------------------------------------
 * Tree building
 * ------------------------------------------------------------------------ */
static ar_i32 ar__push_node(ar_ctx *c, const char *selector, const char *text)
{
    ar_i32   idx, parent;
    ar_node *n;
    ar_u32   tag = 0, klass = 0, id = 0;
    ar_u32   key;
    ar_slot *slot;
    ar_u8    state = AR_STATE_NONE;

    if (c->node_count >= c->node_cap)
    {
        c->overflowed = 1;
        return -1;
    }

    idx = c->node_count++;
    n = &c->nodes[idx];
    parent = c->depth > 0 ? c->stack[c->depth - 1] : -1;

    ar_selector_split(selector, &tag, &klass, &id);

    /* Identity is position in the tree, plus an explicit id when one is given.
       Deliberately not the class: a class changes with state, and an interface
       that swaps .nav for .nav-selected would otherwise hand the box a new
       identity at the exact moment it is being clicked, losing the hover and
       the click together. The tag is left out for the same reason. */
    key = parent >= 0
              ? ar__mix(c->nodes[parent].key, (ar_u32)c->nodes[parent].child_count + 0x9E37u)
              : 0x5BF03635u;
    if (id)
    {
        key = ar__mix(key, id);
    }

    /* Hover and active come from where this box was last frame, because where
       it is this frame is not known until after layout. See ar_node.h. */
    slot = ar_ctx_slot(c, key);
    if (key == c->hot)
    {
        state |= AR_STATE_HOVER;
    }
    if (key == c->active)
    {
        state |= AR_STATE_ACTIVE;
    }

    n->parent = parent;
    n->first_child = -1;
    n->last_child = -1;
    n->next_sibling = -1;
    n->child_count = 0;
    n->key = key;
    n->state = state;
    n->text = text;
    n->fit[0] = 0;
    n->fit[1] = 0;
    n->rect = ar_rect_make(0, 0, 0, 0);

    ar_sheet_resolve(&c->sheet, tag, klass, id, state, &n->style);

    /* font-size is expressed in pixels because that is what people write, and
       the face is eight pixels tall, so the scale is the ratio. Rounding down
       and clamping at one keeps small sizes legible rather than absent. */
    n->scale = n->style.v[AR_P_FONT_SIZE] / AR_FONT_H;
    if (n->scale < 1)
    {
        n->scale = 1;
    }

    if (parent >= 0)
    {
        ar_node *p = &c->nodes[parent];
        if (p->last_child < 0)
        {
            p->first_child = idx;
        }
        else
        {
            c->nodes[p->last_child].next_sibling = idx;
        }
        p->last_child = idx;
        p->child_count++;
    }

    if (slot)
    {
        slot->last_frame = c->frame;
    }
    return idx;
}

void ar_begin(ar_ctx *c, const char *selector)
{
    ar_i32 idx = ar__push_node(c, selector, 0);

    if (c->depth >= AR_MAX_DEPTH)
    {
        c->unbalanced = 1;
        return;
    }

    if (idx < 0)
    {
        /* Out of boxes. Push the current parent again rather than nothing, so
           the matching ar_end still balances and the subtree collapses into
           its parent instead of corrupting the stack. */
        c->stack[c->depth] = c->depth > 0 ? c->stack[c->depth - 1] : -1;
        c->depth++;
        return;
    }
    c->stack[c->depth++] = idx;
}

void ar_end(ar_ctx *c)
{
    if (c->depth <= 0)
    {
        c->unbalanced = 1;
        return;
    }
    c->depth--;
}

void ar_text(ar_ctx *c, const char *selector, const char *text)
{
    ar__push_node(c, selector, text);
}

int ar_button(ar_ctx *c, const char *selector, const char *label)
{
    ar_i32 idx = ar__push_node(c, selector, label);

    if (idx < 0)
    {
        return 0;
    }
    return c->nodes[idx].key == c->clicked && c->clicked != 0;
}

/* ------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------ */
static void ar__paint(ar_ctx *c, ar_surface *s, ar_rect viewport)
{
    ar_i32 i;

    for (i = 0; i < c->node_count; ++i)
    {
        ar_node *n = &c->nodes[i];
        ar_rect  clip;
        ar_color bg, border;
        ar_i32   bw;

        if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }

        /* Parents are painted before children, so by the time a box is
           reached its parent has already narrowed the region. A clip is
           therefore inherited, not recomputed from the ancestor chain. */
        if (n->parent < 0)
        {
            clip = viewport;
        }
        else
        {
            ar_node *p = &c->nodes[n->parent];
            clip = p->clip;
            if (p->style.v[AR_P_OVERFLOW] != AR_OVERFLOW_VISIBLE)
            {
                clip = ar_rect_intersect(clip, p->rect);
            }
        }
        n->clip = clip;

        bg = (ar_color)n->style.v[AR_P_BACKGROUND];
        if (AR_ALPHA_OF(bg) != 0)
        {
            ar_fill_rect(s, n->rect, clip, bg);
        }

        bw = n->style.v[AR_P_BORDER_WIDTH];
        border = (ar_color)n->style.v[AR_P_BORDER_COLOR];
        if (bw > 0 && AR_ALPHA_OF(border) != 0)
        {
            ar_rect r = n->rect;
            ar_fill_rect(s, ar_rect_make(r.x, r.y, r.w, bw), clip, border);
            ar_fill_rect(s, ar_rect_make(r.x, r.y + r.h - bw, r.w, bw), clip, border);
            ar_fill_rect(s, ar_rect_make(r.x, r.y, bw, r.h), clip, border);
            ar_fill_rect(s, ar_rect_make(r.x + r.w - bw, r.y, bw, r.h), clip, border);
        }

        if (n->text)
        {
            /* Text starts inside the padding box. Anything more elaborate is
               alignment, which belongs to the box, not to the glyphs. */
            ar_draw_text(s, ar_rect_intersect(clip, n->rect), n->rect.x + n->style.v[AR_P_PAD_LEFT],
                         n->rect.y + n->style.v[AR_P_PAD_TOP], n->text, n->scale,
                         (ar_color)n->style.v[AR_P_COLOR]);
        }
    }
}

/* The box under the cursor, for the next frame to style. Declaration order is
   paint order, so the last box that contains the point is the one on top. */
static void ar__update_hot(ar_ctx *c)
{
    ar_u32 was = c->hot;
    ar_i32 i;

    c->hot = 0;
    if (!c->mouse_inside)
    {
        return;
    }
    for (i = 0; i < c->node_count; ++i)
    {
        ar_node *n = &c->nodes[i];
        if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (ar_rect_contains(n->rect, c->mouse_x, c->mouse_y))
        {
            c->hot = n->key;
        }
    }

    /* Hover is resolved from the previous frame, so the frame that discovers
       a new box under the cursor is not the frame that can style it. A caller
       whose event pump blocks when idle would therefore show the highlight one
       event late, or not at all. Saying so here lets it ask for one more
       frame, which is the entire cost of the one frame delay. */
    c->hot_changed = (was != c->hot);
}

ar_rect ar_frame_end(ar_ctx *c, ar_surface *s)
{
    ar_rect viewport;
    ar_i32  i;

    if (c->depth != 0)
    {
        c->unbalanced = 1;
        c->depth = 0;
    }

    viewport = ar_rect_make(0, 0, s ? s->w : 0, s ? s->h : 0);

    if (c->node_count == 0)
    {
        return ar_rect_make(0, 0, 0, 0);
    }

    /* Style resolution happened during tree building, between frame_begin and
       here, so closing that phase now attributes it correctly. */
    ar_perf_mark(&c->perf, AR_PHASE_STYLE, ar__now(c));

    ar_layout_solve(c->nodes, c->node_count, viewport);
    ar_perf_mark(&c->perf, AR_PHASE_LAYOUT, ar__now(c));

    /* Where every box ended up, remembered for next frame to hit test
       against. This is the whole cost of the one frame delay: one store per
       box. */
    for (i = 0; i < c->node_count; ++i)
    {
        ar_slot *slot = ar_ctx_slot(c, c->nodes[i].key);
        if (slot)
        {
            slot->rect = c->nodes[i].rect;
            slot->last_frame = c->frame;
        }
    }

    if (s)
    {
        ar__paint(c, s, viewport);
    }
    ar__update_hot(c);

    ar_perf_mark(&c->perf, AR_PHASE_RASTER, ar__now(c));

    c->perf.cur.nodes = (ar_u32)c->node_count;
    /* What the frame actually used, not what it reserved. Reporting the
       reservation would show a flat number that never moves and would say
       nothing about whether the interface is growing. */
    c->perf.cur.arena_frame_bytes = (ar_u32)c->node_count * (ar_u32)sizeof(ar_node);

    /* The frame is left open on purpose: ar_frame_presented closes it, once
       the caller has actually put the pixels on screen. */
    return viewport;
}

void ar_frame_presented(ar_ctx *c)
{
    ar_u32 now = ar__now(c);

    ar_perf_mark(&c->perf, AR_PHASE_PRESENT, now);
    ar_perf_end(&c->perf, now);
}
