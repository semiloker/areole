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

/* Distinct selector-and-state tuples in an interface, not boxes: a thousand
   cards sharing one class occupy one entry. The shipped example uses eleven.
   Sixty-four leaves room for an interface an order of magnitude richer, and
   the cache reports its own hit rate so a wrong guess is visible rather than
   merely slow. Must stay a power of two: the probe masks with it. */
#define AR_STYLE_CACHE 64

typedef char ar__cache_is_pow2[((AR_STYLE_CACHE & (AR_STYLE_CACHE - 1)) == 0) ? 1 : -1];

typedef char ar__mem_fixed_holds[(sizeof(ar_ctx) + AR_MAX_RULES * sizeof(ar_rule) +
                                          AR_STYLE_CACHE * sizeof(ar_cache_entry) + 1024 <=
                                      AR_MEM_FIXED)
                                     ? 1
                                     : -1];

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
            /* A claimed slot must not inherit the previous occupant's verdict:
               a stale seen or digest would let a brand new box be judged
               unchanged and never painted. */
            s->digest = 0;
            s->seen = 0;
            s->text_key = 0;
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
        stale->digest = 0;
        stale->seen = 0;
        stale->text_key = 0;
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

    {
        ar_cache_entry *cache = (ar_cache_entry *)ar_arena_persist(
            &c->arena, AR_STYLE_CACHE * (ar_u32)sizeof(ar_cache_entry));
        if (!cache)
        {
            return 0;
        }
        ar_sheet_set_cache(&c->sheet, cache, AR_STYLE_CACHE);
    }

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

/* ------------------------------------------------------------------------
 * Fonts
 * ------------------------------------------------------------------------ */

#define AR_GLYPH_SLOTS  512
#define AR_GLYPH_PTS    2048
#define AR_GLYPH_POINTS 512
#define AR_SHAPE_RUN    192

int ar_font_load(ar_ctx *c, const void *data, ar_u32 size, ar_u32 atlas_bytes, ar_i32 max_px)
{
    ar_glyph_slot *slots;
    ar_u8         *atlas;
    ar_i32        *pts;
    ar_i32        *ox;
    ar_i32        *oy;
    ar_u8         *on;
    ar_i32        *acc;
    ar_i32        *shape;
    ar_u32         acc_ints;

    if (max_px < 8)
    {
        max_px = 8;
    }
    /* The accumulator is the largest single allocation here and it grows with
       the square of the size, so it is the caller's number rather than a
       generous constant: 48 px costs 9 KB, 128 px costs 66 KB. */
    acc_ints = (ar_u32)(max_px + 2) * (ar_u32)max_px;

    c->have_face = 0;
    c->chain.count = 0;
    if (!ar_face_init(&c->face[0], data, size))
    {
        return 0;
    }
    if (atlas_bytes < 4096u)
    {
        atlas_bytes = 4096u;
    }

    /* Every allocation is persistent and happens exactly once, here. A context
       that never loads a font never pays for any of it, which is why this is
       not part of ar_init. */
    slots = (ar_glyph_slot *)ar_arena_persist(&c->arena,
                                              AR_GLYPH_SLOTS * (ar_u32)sizeof(ar_glyph_slot));
    atlas = (ar_u8 *)ar_arena_persist(&c->arena, atlas_bytes);
    pts = (ar_i32 *)ar_arena_persist(&c->arena, AR_GLYPH_PTS * 2u * (ar_u32)sizeof(ar_i32));
    ox = (ar_i32 *)ar_arena_persist(&c->arena, AR_GLYPH_POINTS * (ar_u32)sizeof(ar_i32));
    oy = (ar_i32 *)ar_arena_persist(&c->arena, AR_GLYPH_POINTS * (ar_u32)sizeof(ar_i32));
    on = (ar_u8 *)ar_arena_persist(&c->arena, AR_GLYPH_POINTS);
    acc = (ar_i32 *)ar_arena_persist(&c->arena, acc_ints * (ar_u32)sizeof(ar_i32));
    shape = (ar_i32 *)ar_arena_persist(&c->arena, AR_SHAPE_RUN * 3u * (ar_u32)sizeof(ar_i32));

    if (!slots || !atlas || !pts || !ox || !oy || !on || !acc || !shape)
    {
        /* The arena is short. Saying so is better than rendering with a face
           whose cache has nowhere to live; the caller sized the block, so the
           caller can fix it. */
        return 0;
    }

    c->glyph_scratch.path_pts = pts;
    c->glyph_scratch.path_cap = AR_GLYPH_PTS;
    c->glyph_scratch.outline.x = ox;
    c->glyph_scratch.outline.y = oy;
    c->glyph_scratch.outline.on = on;
    c->glyph_scratch.outline.cap = AR_GLYPH_POINTS;
    c->glyph_scratch.acc = acc;
    c->glyph_scratch.acc_cap = (ar_i32)acc_ints;
    c->glyph_scratch.shape_glyph = shape;
    c->glyph_scratch.shape_adv = shape + AR_SHAPE_RUN;
    c->glyph_scratch.shape_cluster = shape + AR_SHAPE_RUN * 2;
    c->glyph_scratch.shape_cap = AR_SHAPE_RUN;

    ar_glyph_cache_init(&c->glyphs, slots, AR_GLYPH_SLOTS, atlas, (ar_i32)atlas_bytes);

    c->chain.face[0] = &c->face[0];
    c->chain.count = 1;

    /* Ligatures and kerning are on when the face has the tables, because a
       font that ships them means them. */
    c->shaping = ar_shape_init(&c->shaper, &c->face[0]);
    c->have_face = 1;
    return 1;
}

int ar_font_add(ar_ctx *c, const void *data, ar_u32 size)
{
    ar_i32 n = c->chain.count;

    if (!c->have_face || n <= 0 || n >= AR_MAX_FACES)
    {
        return 0;
    }
    if (!ar_face_init(&c->face[n], data, size))
    {
        return 0;
    }
    c->chain.face[n] = &c->face[n];
    c->chain.count = n + 1;

    /* The chain is part of every cache key by way of the face index, so
       nothing already cached becomes wrong. But a codepoint that fell back to
       the notdef box may now resolve, so the window has to be repainted. */
    ar_invalidate_all(c);
    return 1;
}

ar_i32 ar_font_count(const ar_ctx *c)
{
    return c->chain.count;
}

ar_i32 ar_font_family(const ar_ctx *c, ar_i32 index, char *out, ar_i32 cap)
{
    if (index < 0 || index >= c->chain.count)
    {
        if (cap > 0)
        {
            out[0] = 0;
        }
        return 0;
    }
    return ar_face_family(c->chain.face[index], out, cap);
}

int ar_font_loaded(const ar_ctx *c)
{
    return c->have_face;
}

void ar_font_antialias(ar_ctx *c, int on)
{
    if (c->glyphs.antialias == (on ? 1 : 0))
    {
        return;
    }
    c->glyphs.antialias = on ? 1 : 0;
    /* Every cached bitmap was rasterized under the old setting, and the flag
       is part of the key, so they would simply never be found again. Dropping
       them reclaims the space instead of stranding it. */
    ar_glyph_cache_clear(&c->glyphs);
    ar_invalidate_all(c);
}

void ar_font_grid_fit(ar_ctx *c, int on)
{
    if (c->glyphs.grid_fit == (on ? 1 : 0))
    {
        return;
    }
    c->glyphs.grid_fit = on ? 1 : 0;
    ar_glyph_cache_clear(&c->glyphs);
    ar_invalidate_all(c);
}

void ar_font_darken(ar_ctx *c, ar_i32 amount)
{
    ar_glyph_cache_set_darken(&c->glyphs, amount);
    ar_invalidate_all(c);
}

void ar_font_subpixel(ar_ctx *c, int on)
{
    ar_i32 want = on ? AR_SUBPX_STEPS : 1;

    if (c->glyphs.subpx == want)
    {
        return;
    }
    c->glyphs.subpx = want;
    ar_glyph_cache_clear(&c->glyphs);
    ar_invalidate_all(c);
}

void ar_font_shaping(ar_ctx *c, int on)
{
    int want = on ? (c->face[0].gsub || c->face[0].gpos || c->face[0].kern) : 0;

    if (c->shaping == want)
    {
        return;
    }
    c->shaping = want;
    ar_invalidate_all(c);
}

void ar_font_cache_stats(const ar_ctx *c, ar_u32 *hits, ar_u32 *misses, ar_u32 *resets)
{
    if (hits)
    {
        *hits = c->glyphs.hits;
    }
    if (misses)
    {
        *misses = c->glyphs.misses;
    }
    if (resets)
    {
        *resets = c->glyphs.resets;
    }
}

void ar_style_cache_stats(const ar_ctx *c, ar_u32 *hits, ar_u32 *misses)
{
    if (hits)
    {
        *hits = c->sheet.cache_hits;
    }
    if (misses)
    {
        *misses = c->sheet.cache_misses;
    }
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

/* What the width was measured from: the string's bytes and the size. Anything
   else that could change it -- the face, the fallback chain, grid fitting --
   drops the whole glyph cache and invalidates, so it cannot change underneath
   this without the next frame remeasuring anyway. */
static ar_u32 ar__text_key(const char *t, ar_i32 ppem)
{
    ar_u32 h = 2166136261u;

    while (*t)
    {
        h ^= (ar_u32)(unsigned char)*t++;
        h *= 16777619u;
    }
    h ^= (ar_u32)ppem;
    h *= 16777619u;
    return h ? h : 1u;
}

/* One place decides how wide a run of text is, so painting and layout cannot
   disagree about it. Which face answers depends on whether one was loaded. */
static ar_i32 ar__measure(ar_ctx *c, const ar_node *n)
{
    ar_slot *slot;
    ar_u32   key;
    ar_i32   w;

    if (!n->text)
    {
        return 0;
    }
    if (!c->have_face)
    {
        /* The bitmap face measures by table lookup, which is already cheaper
           than hashing the string would be. */
        return ar_text_width(n->text, n->scale);
    }

    key = ar__text_key(n->text, n->style.v[AR_P_FONT_SIZE]);
    slot = ar_ctx_slot(c, n->key);
    if (slot && slot->text_key == key)
    {
        return slot->text_px;
    }

    w = ar_text_measure_chain(n->text, &c->chain, n->style.v[AR_P_FONT_SIZE], &c->glyphs,
                              &c->glyph_scratch);
    w = (w + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL;
    if (slot)
    {
        slot->text_key = key;
        slot->text_px = w;
    }
    return w;
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

    ar_damage_reset(&c->damage);

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
    n->text_w = ar__measure(c, n);

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
            ar_rect  tclip = ar_rect_intersect(clip, n->rect);
            ar_i32   tx = n->rect.x + n->style.v[AR_P_PAD_LEFT];
            ar_i32   ty = n->rect.y + n->style.v[AR_P_PAD_TOP];
            ar_color tc = (ar_color)n->style.v[AR_P_COLOR];

            if (c->have_face)
            {
                /* A font puts the baseline below the top of the line box; the
                   bitmap face has no baseline and draws from the top, so the
                   two paths take different y values for the same text. */
                ar_i32 ppem = n->style.v[AR_P_FONT_SIZE];
                ar_i32 base = ar_face_scale(&c->face[0], c->face[0].ascender, ppem) / AR_ONE_PIXEL;
                ar_text_draw_shaped(s, tclip, tx, ty + base, n->text, &c->chain,
                                    c->shaping ? &c->shaper : 0, ppem, tc, &c->glyphs,
                                    &c->glyph_scratch, 0);
            }
            else
            {
                ar_draw_text(s, tclip, tx, ty, n->text, n->scale, tc);
            }
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
    ar_rect damage;
    ar_i32  i;
    ar_i32  survivors = 0; /* boxes present this frame that were present last */

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

    /* A resize repaints everything: every box moved, and the surface behind
       them is new memory. */
    if (viewport.w != c->last_viewport.w || viewport.h != c->last_viewport.h)
    {
        ar_damage_add_all(&c->damage);
    }
    c->last_viewport = viewport;
    ar_damage_set_viewport(&c->damage, viewport);

    /* Where every box ended up, remembered for next frame to hit test
       against. This is the whole cost of the one frame delay: one store per
       box.

       The same walk decides what to repaint. A box is damaged when it moved,
       when it resized, when its resolved style came out different, or when it
       was not in the tree last frame -- and when it moved, both the rectangle
       it left and the one it arrived at are damaged, because the old one has
       to be painted over. */
    for (i = 0; i < c->node_count; ++i)
    {
        ar_node *n = &c->nodes[i];
        ar_slot *slot = ar_ctx_slot(c, n->key);
        ar_u32   digest = ar_paint_digest(n);

        if (!slot)
        {
            /* No slot means no memory of this box, so no way to know it did
               not change. Repaint it. */
            ar_damage_add(&c->damage, n->rect);
            continue;
        }

        if (slot->seen == c->frame - 1)
        {
            ++survivors;
        }

        if (slot->seen != c->frame - 1 || slot->digest != digest)
        {
            ar_damage_add(&c->damage, n->rect);
        }
        else if (slot->rect.x != n->rect.x || slot->rect.y != n->rect.y ||
                 slot->rect.w != n->rect.w || slot->rect.h != n->rect.h)
        {
            ar_damage_add(&c->damage, slot->rect);
            ar_damage_add(&c->damage, n->rect);
        }

        slot->rect = n->rect;
        slot->digest = digest;
        slot->seen = c->frame;
        slot->last_frame = c->frame;
    }

    /* A box that was on screen last frame and is gone now leaves a hole, and
       the slot table has to be walked to find it because the tree is exactly
       what no longer contains it.

       That walk is over the whole table, not the tree, so it is the one piece
       of damage tracking whose cost does not shrink with the interface. It is
       therefore skipped in the case that matters: when every box present last
       frame was seen again this frame, nothing was removed, and the count says
       so without looking. An interface whose shape is stable -- which is most
       interfaces, most frames -- never pays for this. */
    if (survivors != c->seen_last)
    {
        for (i = 0; i < c->slot_cap; ++i)
        {
            ar_slot *slot = &c->slots[i];
            if (slot->last_frame != 0 && slot->seen == c->frame - 1)
            {
                ar_damage_add(&c->damage, slot->rect);
            }
        }
    }
    c->seen_last = survivors;

    damage = ar_damage_bounds(&c->damage, viewport);

    if (s && damage.w > 0 && damage.h > 0)
    {
        ar__paint(c, s, damage);
    }
    ar__update_hot(c);

    /* Hover resolves from the previous frame, so the frame that notices a new
       box under the cursor cannot also style it. Damaging both boxes now means
       the next frame -- the one that can style them -- repaints them. */
    if (c->hot_changed)
    {
        ar_slot *slot = ar_ctx_slot(c, c->hot);
        if (slot)
        {
            ar_damage_add(&c->damage, slot->rect);
        }
    }

    /* What is actually presented, which is the sum of the regions rather than
       their bounding box. Reporting the bounding box would hide the entire
       reason there is more than one of them. */
    {
        ar_i32 k, presented = 0;
        for (k = 0; k < ar_damage_count(c); ++k)
        {
            ar_rect dr = ar_damage_rect(c, k);
            if (dr.w > 0 && dr.h > 0)
            {
                presented += dr.w * dr.h;
            }
        }
        c->perf.cur.dirty_px = (ar_u32)presented;
    }
    c->last_damage = damage;

    ar_perf_mark(&c->perf, AR_PHASE_RASTER, ar__now(c));

    c->perf.cur.nodes = (ar_u32)c->node_count;
    /* What the frame actually used, not what it reserved. Reporting the
       reservation would show a flat number that never moves and would say
       nothing about whether the interface is growing. */
    c->perf.cur.arena_frame_bytes = (ar_u32)c->node_count * (ar_u32)sizeof(ar_node);

    /* The frame is left open on purpose: ar_frame_presented closes it, once
       the caller has actually put the pixels on screen. */
    return damage;
}

void ar_invalidate(ar_ctx *c, ar_rect r)
{
    ar_damage_add(&c->damage, r);
}

void ar_invalidate_all(ar_ctx *c)
{
    ar_damage_add_all(&c->damage);
}

int ar_frame_is_dirty(const ar_ctx *c)
{
    return c->damage.all || c->damage.count > 0;
}

ar_i32 ar_damage_count(const ar_ctx *c)
{
    return c->damage.all ? 1 : c->damage.count;
}

ar_rect ar_damage_rect(const ar_ctx *c, ar_i32 i)
{
    if (c->damage.all)
    {
        return i == 0 ? c->last_viewport : ar_rect_make(0, 0, 0, 0);
    }
    if (i < 0 || i >= c->damage.count)
    {
        return ar_rect_make(0, 0, 0, 0);
    }
    return ar_rect_intersect(c->damage.r[i], c->last_viewport);
}

void ar_frame_presented(ar_ctx *c)
{
    ar_u32 now = ar__now(c);

    ar_perf_mark(&c->perf, AR_PHASE_PRESENT, now);
    ar_perf_end(&c->perf, now);
}
