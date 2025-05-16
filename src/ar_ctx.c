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
/*
 * Combine, then avalanche.
 *
 * The combining step alone is the classic hash_combine, and it is not enough
 * here. Two things went wrong with it, both measured on a three hundred row
 * list, and the finalizer fixes both.
 *
 * **The low bits never moved.** The table is indexed by them, and for the
 * children of one parent `a` is fixed while `b` walks up by one -- so the keys
 * walked up by one too and three hundred rows landed in three hundred and
 * seventy-five consecutive indices. Linear probing gives up after 32 and the
 * longest unbroken run of occupied slots was **257**, with the table only
 * **29% full**. Doubling the table changed nothing, because the run was a
 * property of the hash and not of the load. A box that finds no slot is
 * repainted every frame, since nothing remembers that it did not change.
 *
 * **And the keys themselves collided.** A row's child is mixed with a constant,
 * so its key is a function of the row's key, and both sets lived in small
 * structured subsets of the word that overlapped constantly. Across four
 * hundred random trees of this shape, **186 of them had two boxes sharing a
 * key** -- against the birthday bound of about four in a hundred thousand.
 * Boxes sharing a key share a slot, which means sharing hover, active, scroll
 * position and damage: the wrong row lights up under the cursor.
 *
 * Zero of the same four hundred collide with the finalizer, and the longest
 * index run falls from 257 to 6. Two multiplies per box per frame buys that.
 */
static ar_u32 ar__mix(ar_u32 a, ar_u32 b)
{
    a ^= b + 0x9E3779B9u + (a << 6) + (a >> 2);
    a ^= a >> 16;
    a *= 0x85EBCA6Bu;
    a ^= a >> 13;
    a *= 0xC2B2AE35u;
    a ^= a >> 16;
    return a ? a : 1u;
}

/*
 * Finds a slot without claiming one.
 *
 * ar_ctx_slot below will take an empty slot and make it this box's, which is
 * what tree building wants and what a query must not do -- asking where a
 * container is scrolled to should not create state for a box that has none.
 */
const ar_slot *ar_ctx_slot_find(const ar_ctx *c, ar_u32 key)
{
    ar_u32 mask;
    ar_u32 i;
    ar_u32 probe;

    if (!c || !c->slots || c->slot_cap <= 0)
    {
        return 0;
    }
    mask = (ar_u32)c->slot_cap - 1u;
    i = key & mask;
    for (probe = 0; probe < 32u; ++probe)
    {
        if (c->slots[i].key == key)
        {
            return &c->slots[i];
        }
        if (c->slots[i].last_frame == 0)
        {
            return 0;
        }
        i = (i + 1u) & mask;
    }
    return 0;
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
            s->text_min_px = 0;
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
        stale->text_min_px = 0;
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
    shape = (ar_i32 *)ar_arena_persist(&c->arena, AR_SHAPE_RUN * 6u * (ar_u32)sizeof(ar_i32));

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
    c->glyph_scratch.shape_cp = (ar_u32 *)(shape + AR_SHAPE_RUN * 3);
    c->glyph_scratch.shape_dx = shape + AR_SHAPE_RUN * 4;
    c->glyph_scratch.shape_dy = shape + AR_SHAPE_RUN * 5;
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

/*
 * Every reason the next frame will differ from the one just drawn.
 *
 * Hover resolves from the previous frame and a wheel notch is applied after the
 * paint, so both settle state that only the next frame can show. A caller whose
 * pump blocks when idle has to draw that frame, and this is the single question
 * it has to ask.
 *
 * This meant hover alone, and ar_scrolled was left for the caller to remember
 * on its own. Nothing remembered it: ar_scrolled had no caller anywhere in the
 * tree, so the tour woke on hover only and scrolling appeared to work every
 * other try -- it showed up when the cursor happened to change box at the same
 * time and did nothing when the cursor sat still. A predicate that has to be
 * ORed with another one to be correct is a predicate that gets got wrong, so
 * the reasons live behind this one and ar_scrolled stays for a caller that
 * wants to know which reason it was.
 */
int ar_needs_redraw(const ar_ctx *c)
{
    return c->hot_changed || c->scrolled;
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

/*
 * The bridge between the selector matcher and the box tree.
 *
 * ar_css.c knows nothing about nodes and should not: it is handed a callback
 * that answers "what is the parent of this box", or "the sibling before it",
 * and fills in that box's tag, classes and id. Descendant and general-sibling
 * call it repeatedly, walking outwards until something matches or the tree
 * runs out.
 */
static int ar__sel_walk(void *ud, ar_i32 from, ar_i32 comb, ar_i32 *out_index, ar_u32 *tag,
                        ar_classes *klass, ar_u32 *id)
{
    ar_ctx *c = (ar_ctx *)ud;
    ar_i32  to = -1;

    if (from < 0 || from >= c->node_count)
    {
        return 0;
    }
    if (comb == AR_COMB_CHILD || comb == AR_COMB_DESCENDANT)
    {
        to = c->nodes[from].parent;
    }
    else
    {
        to = c->nodes[from].prev_sibling;
    }
    if (to < 0)
    {
        return 0;
    }
    *out_index = to;
    *tag = c->nodes[to].sel_tag;
    *id = c->nodes[to].sel_id;
    *klass = c->nodes[to].sel_class;
    return 1;
}

/*
 * Everything that turns a selector into a resolved style for one box.
 *
 * Pulled out of ar_begin because the structural pseudo-classes that depend on
 * the parent's final child count force a second run, and running it twice has
 * to mean running the same thing twice.
 */
static void ar__resolve(ar_ctx *c, ar_i32 i)
{
    ar_node *n = &c->nodes[i];

    ar_sheet_resolve(&c->sheet, n->sel_tag, &n->sel_class, n->sel_id, n->state, &n->style);

    /* Rules with a combinator, which the cache cannot hold because their
       answer depends on where this box sits rather than only on what it is.
       A stylesheet without combinators skips this entirely. */
    ar_sheet_resolve_contextual(&c->sheet, i, n->sel_tag, &n->sel_class, n->sel_id, n->state,
                                ar__sel_walk, c, &n->style);

    /* Inheritance, after the cache rather than inside it. The cache holds what
       the selectors produced, which does not depend on where a box sits; the
       inherited part does, and is a loop over two properties. Caching after
       inheritance would need the parent in the key and would be a different
       and much worse cache -- and the key's completeness is the one thing in
       this file that fails silently when it is wrong. */
    if (n->parent >= 0)
    {
        ar_style_inherit(&n->style, &c->nodes[n->parent].style);
    }
    else
    {
        /* The root has nothing above it, but `inherit` and `initial` still have
           to be resolved or their units reach layout as themselves. Inheriting
           from the defaults is what the specification says the root does. */
        ar_style root;
        ar_style_defaults(&root);
        ar_style_inherit(&n->style, &root);
    }
}

/*
 * :last-child, :only-child and :empty, once every box has been declared.
 *
 * Nodes are appended in declaration order, which is preorder, so walking the
 * array forwards visits every parent before its children and inheritance still
 * flows the right way on the second pass. A sheet that uses none of these
 * three never gets here.
 */
static void ar__resolve_late(ar_ctx *c)
{
    ar_i32 i;

    if (!c->sheet.has_late_state)
    {
        return;
    }
    for (i = 0; i < c->node_count; ++i)
    {
        ar_node *n = &c->nodes[i];
        ar_u16   was = n->state;

        if (n->child_count == 0 && !n->text)
        {
            n->state |= AR_STATE_EMPTY;
        }
        if (n->parent >= 0)
        {
            if (c->nodes[n->parent].last_child == i)
            {
                n->state |= AR_STATE_LAST;
            }
            if (c->nodes[n->parent].child_count == 1)
            {
                n->state |= AR_STATE_ONLY;
            }
        }
        /* Re-resolving unconditionally would be correct and would also throw
           away the first pass for every box in the tree. Only the ones whose
           state actually changed can resolve differently. */
        if (n->state != was)
        {
            ar__resolve(c, i);
        }
    }
}

/* ------------------------------------------------------------------------
 * Wrapping
 *
 * Layout needs to know how tall a paragraph is at a given width, and paint
 * needs to know where each line starts. Both come from here, so a line that
 * layout made room for is the same line paint draws -- the two drifting apart
 * is the classic way text ends up overflowing a box that looks the right size.
 * ------------------------------------------------------------------------ */
#define AR_MAX_LINES 64

/* The bitmap face's adapter for ar_text_wrap_by. It measures in whole pixels
   where the outline path measures in 1/AR_ONE_PIXEL, so it scales up. */
typedef struct ar__bmp_ud
{
    ar_i32 scale;
} ar__bmp_ud;

static ar_i32 ar__wrap_bitmap(void *ud, const char *t, ar_i32 from, ar_i32 to)
{
    return ar_text_width_range(t, from, to, ((ar__bmp_ud *)ud)->scale) * AR_ONE_PIXEL;
}

static ar_i32 ar__wrap_lines(ar_ctx *c, const char *text, ar_i32 font_px, ar_i32 scale,
                             ar_i32 max_w, ar_i32 *starts, ar_i32 cap)
{
    if (!text || max_w <= 0)
    {
        return 0;
    }
    if (c->have_face)
    {
        return ar_text_wrap_chain(text, &c->chain, font_px, max_w, &c->glyphs, &c->glyph_scratch,
                                  starts, cap);
    }
    {
        ar__bmp_ud ud;
        ud.scale = scale;
        return ar_text_wrap_by(text, ar__wrap_bitmap, &ud, max_w, starts, cap);
    }
}

/*
 * A box's vertical text metrics.
 *
 * With an outline face they come from the face: ascender to descender is the
 * line box, and the line gap is the space the designer asked for between
 * lines. This is the fix for a real bug -- the metrics used to come from the
 * bitmap face's eight-pixel cell whatever the font size was, so a 13 px font
 * got an 8 px box, and since text is clipped to its box the bottom of every
 * glyph was cut off.
 *
 * With the bitmap face they stay what they were, because there the eight pixel
 * cell is the truth rather than an approximation of it.
 */
static ar_i32 ar__round_px(ar_i32 v)
{
    return (v + AR_ONE_PIXEL / 2) / AR_ONE_PIXEL;
}

static void ar__text_metrics(ar_ctx *c, ar_node *n)
{
    if (!c->have_face)
    {
        n->text_h = ar_text_height(n->scale);
        n->line_h = ar_text_line_height(n->scale);
        /* The bitmap face draws from the top rather than from a baseline, and
            painting still does. But a line box needs a baseline to align
            against, and for a face with no descender the baseline is the
            bottom of the cell. Painting reads n->ascent only on the outline
            path, so this is free there and correct here. */
        n->ascent = ar_text_height(n->scale);
        return;
    }
    {
        const ar_face *f = &c->face[0];
        ar_i32         ppem = n->style.v[AR_P_FONT_SIZE];

        /*
         * Ascent, descent and gap are each rounded to a whole pixel and then
         * added, rather than added and then rounded once.
         *
         * It looks like the worse of the two -- it throws away up to half a
         * pixel three times instead of once -- and it is what every browser
         * does, because the ascent alone is the baseline offset and the
         * baseline has to sit on a pixel. Rounding the sum instead leaves the
         * baseline and the line box disagreeing by a fraction, and the
         * disagreement accumulates down a paragraph.
         */
        n->ascent = ar__round_px(ar_face_scale(f, f->ascender, ppem));
        n->text_h = n->ascent + ar__round_px(-ar_face_scale(f, f->descender, ppem)) +
                    ar__round_px(ar_face_scale(f, f->line_gap, ppem));
        if (n->text_h < 1)
        {
            n->text_h = 1;
        }
        n->line_h = n->text_h;
    }
}

/*
 * The min-content width of a box's own text: its widest unbreakable run.
 *
 * Measured once per box in the same place its max-content width already was,
 * so intrinsic sizing stays linear in the box count. Doing it lazily, per
 * query, is the classic route to a layout engine that is accidentally
 * quadratic on a large table.
 */
static ar_i32 ar__min_width_uncached(ar_ctx *c, const ar_node *n);

static ar_i32 ar__min_width(ar_ctx *c, const ar_node *n)
{
    ar_slot *slot;
    ar_u32   key;
    ar_i32   w;

    if (!n->text || !n->text[0])
    {
        return 0;
    }

    key = ar__text_key(n->text, n->style.v[AR_P_FONT_SIZE]);
    slot = ar_ctx_slot(c, n->key);
    if (slot && slot->text_key == key && slot->text_min_px > 0)
    {
        return slot->text_min_px;
    }

    w = ar__min_width_uncached(c, n);
    if (slot)
    {
        /* The full width is memoised under the same key, and stores it first;
           this only ever adds to an entry that is already current. */
        slot->text_min_px = w;
    }
    return w;
}

static ar_i32 ar__min_width_uncached(ar_ctx *c, const ar_node *n)
{
    if (c->have_face)
    {
        ar_i32 w = ar_text_min_width_chain(n->text, &c->chain, n->style.v[AR_P_FONT_SIZE],
                                           &c->glyphs, &c->glyph_scratch);

        return (w + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL;
    }
    {
        ar__bmp_ud ud;

        ud.scale = n->scale;
        return (ar_text_min_width_by(n->text, ar__wrap_bitmap, &ud) + AR_ONE_PIXEL - 1) /
               AR_ONE_PIXEL;
    }
}

/*
 * The width of one slice of a box's text.
 *
 * What the line breaker measures pieces with. It cannot be memoised the way
 * the whole string is -- the slices differ per line and per width -- so it is
 * a glyph cache lookup per character, against a cache that is warm by then.
 */
static ar_i32 ar__range_px(void *ud, const ar_node *n, ar_i32 from, ar_i32 to)
{
    ar_ctx *c = (ar_ctx *)ud;
    ar_i32  w;

    if (!n->text || to <= from)
    {
        return 0;
    }
    if (c->have_face)
    {
        w = ar_text_range_chain(n->text, from, to, &c->chain, n->style.v[AR_P_FONT_SIZE],
                                &c->glyphs, &c->glyph_scratch);
        return (w + AR_ONE_PIXEL - 1) / AR_ONE_PIXEL;
    }
    return ar_text_width_range(n->text, from, to, n->scale);
}

/* Where a scroll container currently is, read from its slot. */
static ar_i32 ar__scroll_of(void *ud, ar_i32 index)
{
    ar_ctx  *c = (ar_ctx *)ud;
    ar_slot *slot;

    if (index < 0 || index >= c->node_count)
    {
        return 0;
    }
    slot = ar_ctx_slot(c, c->nodes[index].key);
    return slot ? slot->scroll : 0;
}

/* What ar_layout_solve is handed. */
static ar_i32 ar__wrap_cb(void *ud, const ar_node *n, ar_i32 max_w)
{
    ar_ctx *c = (ar_ctx *)ud;
    ar_i32  starts[AR_MAX_LINES];
    ar_i32  lines = ar__wrap_lines(c, n->text, n->style.v[AR_P_FONT_SIZE], n->scale, max_w, starts,
                                   AR_MAX_LINES);

    if (lines < 1)
    {
        lines = 1;
    }
    return n->text_h + (lines - 1) * n->line_h;
}

/* ------------------------------------------------------------------------
 * Inspecting the frame
 * ------------------------------------------------------------------------ */
ar_i32 ar_node_count(const ar_ctx *c)
{
    return c ? c->node_count : 0;
}

ar_rect ar_node_rect(const ar_ctx *c, ar_i32 i)
{
    if (!c || i < 0 || i >= c->node_count)
    {
        return ar_rect_make(0, 0, 0, 0);
    }
    return c->nodes[i].rect;
}

ar_i32 ar_node_parent(const ar_ctx *c, ar_i32 i)
{
    if (!c || i < 0 || i >= c->node_count)
    {
        return -1;
    }
    return c->nodes[i].parent;
}

const char *ar_node_text(const ar_ctx *c, ar_i32 i)
{
    if (!c || i < 0 || i >= c->node_count || !c->nodes[i].text)
    {
        return "";
    }
    return c->nodes[i].text;
}

ar_i32 ar_node_scroll(const ar_ctx *c, ar_i32 i)
{
    const ar_slot *slot;

    if (!c || i < 0 || i >= c->node_count)
    {
        return 0;
    }
    slot = ar_ctx_slot_find(c, c->nodes[i].key);
    return slot ? slot->scroll : 0;
}

ar_i32 ar_node_scroll_range(const ar_ctx *c, ar_i32 i)
{
    if (!c || i < 0 || i >= c->node_count)
    {
        return 0;
    }
    return ar_scroll_range(&c->nodes[i]);
}

/*
 * Records that a container's pixels are now behind its position by `dy`, so the
 * next frame can move them rather than paint them again.
 *
 * Accumulated for the same container, because several notches can land before a
 * frame gets the chance to render one. A second container moving before the
 * first has been caught up gives up on both -- ordering two moves against each
 * other is where a nested pair goes wrong, and repainting is always correct.
 */
static void ar__scroll_moved(ar_ctx *c, ar_u32 key, ar_i32 dy)
{
    if (dy == 0)
    {
        return;
    }
    if (c->move_dy != 0 && c->move_key != key)
    {
        c->move_many = 1;
        return;
    }
    c->move_key = key;
    c->move_dy += dy;
}

ar_i32 ar_node_scroll_to(ar_ctx *c, ar_i32 i, ar_i32 y)
{
    ar_slot *slot;
    ar_i32   was;

    if (!c || i < 0 || i >= c->node_count || !ar_is_scroll_container(&c->nodes[i]))
    {
        return 0;
    }
    slot = ar_ctx_slot(c, c->nodes[i].key);
    if (!slot)
    {
        return 0;
    }
    was = slot->scroll;
    slot->scroll = ar_scroll_clamp(&c->nodes[i], y);
    ar__scroll_moved(c, c->nodes[i].key, slot->scroll - was);
    ar_damage_add(&c->damage, c->nodes[i].rect);
    return slot->scroll;
}

int ar_scrolled(const ar_ctx *c)
{
    return c ? c->scrolled : 0;
}

ar_i32 ar_node_frag_count(const ar_ctx *c, ar_i32 i)
{
    if (!c || i < 0 || i >= c->node_count)
    {
        return 0;
    }
    return c->nodes[i].frag_count;
}

ar_rect ar_node_frag(const ar_ctx *c, ar_i32 i, ar_i32 k, ar_i32 *out_from, ar_i32 *out_to)
{
    const ar_frag *f;

    if (!c || i < 0 || i >= c->node_count || k < 0 || k >= c->nodes[i].frag_count)
    {
        if (out_from)
        {
            *out_from = 0;
        }
        if (out_to)
        {
            *out_to = 0;
        }
        return ar_rect_make(0, 0, 0, 0);
    }
    f = &c->frags[c->nodes[i].frag_first + k];
    if (out_from)
    {
        *out_from = f->from;
    }
    if (out_to)
    {
        *out_to = f->to;
    }
    return f->rect;
}

ar_i32 ar_node_child_index(const ar_ctx *c, ar_i32 i)
{
    ar_i32 parent;
    ar_i32 at;
    ar_i32 n = 0;

    if (!c || i < 0 || i >= c->node_count)
    {
        return -1;
    }
    parent = c->nodes[i].parent;
    if (parent < 0)
    {
        return 0;
    }
    /* Counted by walking back along prev_sibling rather than forward from the
       parent's first child: the links are already there for the sibling
       combinators, and this way costs nothing extra to maintain. */
    for (at = c->nodes[i].prev_sibling; at >= 0; at = c->nodes[at].prev_sibling)
    {
        ++n;
    }
    return n;
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
    c->wheel = in ? in->wheel : 0;
    c->scrolled = 0;

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
    if ((ar_u32)c->node_cap *
            ((ar_u32)sizeof(ar_node) + (ar_u32)sizeof(ar_frag) + (ar_u32)sizeof(ar_i32)) >
        room)
    {
        c->node_cap = (ar_i32)(room / ((ar_u32)sizeof(ar_node) + (ar_u32)sizeof(ar_frag) +
                                       (ar_u32)sizeof(ar_i32)));
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

    /*
     * Fragments, from whatever the tree left. Two per box is generous -- a box
     * only has fragments if the line breaker cut it -- and running out means
     * the last inline stays whole rather than anything being scribbled on.
     */
    c->frag_count = 0;
    room = ar_arena_available(&c->arena);
    c->frag_cap = c->node_cap;
    if ((ar_u32)c->frag_cap * (ar_u32)sizeof(ar_frag) > room)
    {
        c->frag_cap = (ar_i32)(room / (ar_u32)sizeof(ar_frag));
    }
    c->frags =
        c->frag_cap > 0
            ? (ar_frag *)ar_arena_frame(&c->arena, (ar_u32)c->frag_cap * (ar_u32)sizeof(ar_frag))
            : 0;
    if (!c->frags)
    {
        c->frag_cap = 0;
    }

    /* Paint order, one entry per box, budgeted alongside the tree. Without it
       painting falls back to declaration order -- what it did before stacking
       existed, and wrong rather than blank. */
    c->order_count = 0;
    room = ar_arena_available(&c->arena);
    c->order =
        (ar_u32)c->node_cap * (ar_u32)sizeof(ar_i32) <= room && c->node_cap > 0
            ? (ar_i32 *)ar_arena_frame(&c->arena, (ar_u32)c->node_cap * (ar_u32)sizeof(ar_i32))
            : 0;
}

/* ------------------------------------------------------------------------
 * Tree building
 * ------------------------------------------------------------------------ */
static ar_i32 ar__push_node(ar_ctx *c, const char *selector, const char *text)
{
    ar_i32     idx, parent;
    ar_node   *n;
    ar_u32     tag = 0, id = 0;
    ar_classes klass;
    ar_u32     key;
    ar_slot   *slot;
    ar_u8      state = AR_STATE_NONE;

    if (c->node_count >= c->node_cap)
    {
        c->overflowed = 1;
        return -1;
    }

    idx = c->node_count++;
    n = &c->nodes[idx];
    parent = c->depth > 0 ? c->stack[c->depth - 1] : -1;

    ar_classes_clear(&klass);
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

    /* The structural bits that position among siblings already settles. The
       other three wait for the parent to close; see ar__resolve_late. */
    if (parent < 0)
    {
        state |= AR_STATE_ROOT;
    }
    else
    {
        ar_i32 nth = c->nodes[parent].child_count; /* zero-based, so far */

        if (nth == 0)
        {
            state |= AR_STATE_FIRST;
        }
        state |= (nth & 1) ? AR_STATE_EVEN : AR_STATE_ODD;
    }

    n->parent = parent;
    n->first_child = -1;
    n->last_child = -1;
    n->next_sibling = -1;
    n->child_count = 0;
    n->key = key;
    n->state = state;
    n->sel_tag = tag;
    n->sel_id = id;
    n->sel_class = klass;
    n->prev_sibling = parent >= 0 ? c->nodes[parent].last_child : -1;
    n->text = text;
    n->fit[0] = 0;
    n->fit[1] = 0;
    /* The arena hands back memory it does not clear, so a box that never
       reaches an inline run would otherwise inherit a fragment count from
       whatever box held this slot last -- and painting would follow it into
       another box's text. */
    n->frag_first = 0;
    n->frag_count = 0;
    n->rect = ar_rect_make(0, 0, 0, 0);

    ar__resolve(c, c->node_count - 1);

    /* font-size is expressed in pixels because that is what people write, and
       the face is eight pixels tall, so the scale is the ratio. Rounding down
       and clamping at one keeps small sizes legible rather than absent. */
    n->scale = n->style.v[AR_P_FONT_SIZE] / AR_FONT_H;
    if (n->scale < 1)
    {
        n->scale = 1;
    }
    ar__text_metrics(c, n);
    n->text_w = ar__measure(c, n);
    n->min_w = ar__min_width(c, n);

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
/*
 * One wrapped line.
 *
 * The drawing calls take a NUL-terminated string and this library does not
 * copy the caller's text, so a line -- which is a slice -- is copied into a
 * stack buffer to be terminated. One memcpy against rasterizing a line of
 * glyphs is not a cost worth designing around, and the alternative is a range
 * argument threaded through every drawing entry point including the shaper.
 *
 * `to` of -1 means "to the end". A line longer than the buffer is truncated at
 * a UTF-8 lead byte rather than mid-sequence: a clipped word is a visible
 * failure, half a codepoint is a corrupt one.
 */
#define AR_LINE_BUF 512

static void ar__draw_line(ar_ctx *c, ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y,
                          const ar_node *n, ar_i32 from, ar_i32 to, ar_color col)
{
    char   buf[AR_LINE_BUF];
    ar_i32 len = 0;
    ar_i32 i;

    if (!n->text)
    {
        return;
    }
    if (to < 0)
    {
        for (to = from; n->text[to]; ++to)
        {
        }
    }
    len = to - from;
    if (len <= 0)
    {
        return;
    }
    if (len > AR_LINE_BUF - 1)
    {
        len = AR_LINE_BUF - 1;
        while (len > 0 && ((unsigned char)n->text[from + len] & 0xC0u) == 0x80u)
        {
            --len;
        }
    }
    for (i = 0; i < len; ++i)
    {
        buf[i] = n->text[from + i];
    }
    /* A break leaves the space at the end of the line it broke. Drawing it
       would be invisible for a left-aligned line and wrong for anything else,
       and it makes the measured width disagree with the drawn one. */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\n' || buf[len - 1] == '\r'))
    {
        --len;
    }
    buf[len] = 0;
    if (len == 0)
    {
        return;
    }

    if (c->have_face)
    {
        /* A font puts the baseline below the top of the line box; the bitmap
           face has no baseline and draws from the top, so the two paths take
           different y values for the same text. */
        ar_text_draw_shaped(s, clip, x, y + n->ascent, buf, &c->chain, c->shaping ? &c->shaper : 0,
                            n->style.v[AR_P_FONT_SIZE], col, &c->glyphs, &c->glyph_scratch, 0);
    }
    else
    {
        ar_draw_text(s, clip, x, y, buf, n->scale, col);
    }
}

/*
 * Every box's clip, settled before anything asks for it.
 *
 * One forward sweep is enough because boxes are appended in declaration order,
 * so a parent always precedes its children -- the same property that lets both
 * layout passes be plain loops with no stack and no recursion.
 *
 * This used to be built inside ar__paint as it walked the stacking tree, which
 * meant nothing before painting could ask where a box was clipped. The region
 * move needs exactly that, and had to climb the ancestor chain itself to get
 * it. Now there is one answer, computed once, and both read it.
 */
static void ar__clip_tree(ar_ctx *c, ar_rect viewport)
{
    ar_i32 i;

    for (i = 0; i < c->node_count; ++i)
    {
        ar_node *n = &c->nodes[i];

        if (n->parent < 0)
        {
            n->clip = viewport;
        }
        else
        {
            ar_node *p = &c->nodes[n->parent];

            n->clip = p->clip;
            if (p->style.v[AR_P_OVERFLOW] != AR_OVERFLOW_VISIBLE)
            {
                n->clip = ar_rect_intersect(n->clip, p->rect);
            }
        }
    }
}

/* What a box confines its children to: its own clip, narrowed by itself when it
   clips at all. One line, where it used to be an ancestor walk. */
static ar_rect ar__content_clip(const ar_node *n)
{
    if (n->style.v[AR_P_OVERFLOW] == AR_OVERFLOW_VISIBLE)
    {
        return n->clip;
    }
    return ar_rect_intersect(n->clip, n->rect);
}

/* `region` is what this pass is allowed to touch -- the damage, or one band of
   it -- and is narrower than the viewport the clips were built against. */
static void ar__paint(ar_ctx *c, ar_surface *s, ar_rect region)
{
    ar_i32 ord;
    ar_i32 painted = c->order ? c->order_count : c->node_count;

    for (ord = 0; ord < painted; ++ord)
    {
        ar_i32   i = c->order ? c->order[ord] : ord;
        ar_node *n = &c->nodes[i];
        ar_rect  clip;
        ar_color bg, border;
        ar_i32   bw;

        if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }

        clip = ar_rect_intersect(n->clip, region);

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

        if (ar_scroll_bar_visible(n))
        {
            ar_rect track, thumb;

            ar_scroll_bar(n, ar__scroll_of(c, i), &track, &thumb);
            ar_fill_rect(s, track, clip, AR_RGBA(0x00, 0x00, 0x00, 0x14));
            ar_fill_rect(s, thumb, clip, AR_RGBA(0x00, 0x00, 0x00, 0x50));
        }

        /*
         * A box the line breaker cut has one rectangle per line it touched,
         * and each carries the slice of text that landed there. A box it did
         * not cut has none of them and is painted from its own rect, which is
         * every box that is not a split inline.
         */
        if (n->frag_count > 0)
        {
            ar_i32 k;

            for (k = 0; k < n->frag_count; ++k)
            {
                const ar_frag *f = &c->frags[n->frag_first + k];
                ar_rect        fclip = ar_rect_intersect(clip, f->rect);

                if (bg >> 24)
                {
                    ar_fill_rect(s, f->rect, clip, bg);
                }
                ar__draw_line(c, s, fclip, f->rect.x + n->style.v[AR_P_PAD_LEFT],
                              f->rect.y + n->style.v[AR_P_PAD_TOP], n, f->from, f->to,
                              (ar_color)n->style.v[AR_P_COLOR]);
            }
            continue;
        }

        if (n->text)
        {
            /* Text starts inside the padding box. Anything more elaborate is
               alignment, which belongs to the box, not to the glyphs. */
            ar_rect  tclip = ar_rect_intersect(clip, n->rect);
            ar_i32   tx = n->rect.x + n->style.v[AR_P_PAD_LEFT];
            ar_i32   ty = n->rect.y + n->style.v[AR_P_PAD_TOP];
            ar_color tc = (ar_color)n->style.v[AR_P_COLOR];

            /* The same wrap layout used, so the lines drawn are the lines
               that were made room for. Wrapping twice per frame is the cost
               of not storing the line table; it is a cache lookup per glyph
               against a warm cache, and the alternative is per-box storage
               for something only the painted boxes need. */
            ar_i32 starts[AR_MAX_LINES];
            ar_i32 inner_w = n->rect.w - n->style.v[AR_P_PAD_LEFT] - n->style.v[AR_P_PAD_RIGHT];
            ar_i32 lines = ar__wrap_lines(c, n->text, n->style.v[AR_P_FONT_SIZE], n->scale, inner_w,
                                          starts, AR_MAX_LINES);
            ar_i32 advance = n->line_h;
            ar_i32 li;

            if (lines < 1)
            {
                lines = 1;
                starts[0] = 0;
            }

            for (li = 0; li < lines; ++li)
            {
                /* The line is a slice of the caller's string, and this library
                   does not copy strings. Drawing is given the whole tail and a
                   clip that stops it at the next line's start. */
                ar_i32 from = starts[li];
                ar_i32 to = (li + 1 < lines) ? starts[li + 1] : -1;
                ar_i32 ly = ty + li * advance;

                ar__draw_line(c, s, tclip, tx, ly, n, from, to, tc);
            }
        }
    }
}

/* The box under the cursor, for the next frame to style. Declaration order is
   paint order, so the last box that contains the point is the one on top. */
/*
 * A wheel notch goes to the innermost scroll container under the cursor.
 *
 * Front to back, so a list inside a panel takes the notch rather than the
 * panel; and past any container that cannot move, so a list already at its
 * bottom hands the notch outwards instead of swallowing it -- which is scroll
 * chaining, and is what makes a nested list feel attached to the page rather
 * than like a trap.
 *
 * Applied after layout, so it lands on the next frame. The box under the
 * cursor is not known until the frame is laid out, and laying out twice to
 * find out costs more than the frame it saves. ar_needs_redraw already exists
 * to close the gap.
 */
/*
 * Dragging the scrollbar thumb.
 *
 * The bar is drawn by the container rather than declared as a box, so
 * ar__update_hot never sees it and the ordinary hot and active machinery
 * cannot carry it. It gets its own pass, for the same reason the wheel does.
 *
 * Once a drag has started it follows the cursor wherever it goes, including
 * off the container and out of the window: only letting go ends it. That is
 * what every scrollbar does, and it is what makes a long drag usable rather
 * than something that slips out from under you.
 *
 * The thumb's top is (rect.h - thumb.h) * scroll / range, so a drag is that
 * read backwards. The grab offset keeps the thumb where it was picked up.
 */
static void ar__apply_drag(ar_ctx *c)
{
    ar_i32 i;

    if (c->drag_key)
    {
        if (!(c->mouse_down & AR_MOUSE_LEFT))
        {
            c->drag_key = 0;
            return;
        }
        for (i = 0; i < c->node_count; ++i)
        {
            ar_node *n = &c->nodes[i];
            ar_slot *slot;
            ar_rect  track, thumb;
            ar_i32   range, span, want;

            if (n->key != c->drag_key || !ar_is_scroll_container(n))
            {
                continue;
            }
            range = ar_scroll_range(n);
            slot = ar_ctx_slot(c, n->key);
            if (range <= 0 || !slot)
            {
                c->drag_key = 0;
                return;
            }
            ar_scroll_bar(n, slot->scroll, &track, &thumb);
            span = n->rect.h - thumb.h;
            if (span <= 0)
            {
                return; /* the thumb fills the track; there is nowhere to drag */
            }
            want = ar_scroll_clamp(n, (c->mouse_y - track.y - c->drag_grab) * range / span);
            if (want != slot->scroll)
            {
                ar__scroll_moved(c, n->key, want - slot->scroll);
                slot->scroll = want;
                ar_damage_add(&c->damage, n->rect);
                c->scrolled = 1;
            }
            return;
        }
        c->drag_key = 0; /* the container is no longer in the tree */
        return;
    }

    if (!(c->mouse_pressed & AR_MOUSE_LEFT) || !c->mouse_inside)
    {
        return;
    }

    /* Front to back, so the innermost bar takes the press. */
    for (i = (c->order ? c->order_count : c->node_count) - 1; i >= 0; --i)
    {
        ar_i32   at = c->order ? c->order[i] : i;
        ar_node *n = &c->nodes[at];
        ar_slot *slot;
        ar_rect  track, thumb;

        if (!ar_is_scroll_container(n) || !ar_scroll_bar_visible(n))
        {
            continue;
        }
        if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE || ar_scroll_range(n) <= 0)
        {
            continue;
        }
        slot = ar_ctx_slot(c, n->key);
        if (!slot)
        {
            continue;
        }
        ar_scroll_bar(n, slot->scroll, &track, &thumb);
        if (!ar_rect_contains(thumb, c->mouse_x, c->mouse_y))
        {
            continue;
        }
        c->drag_key = n->key;
        c->drag_grab = c->mouse_y - thumb.y;
        return;
    }
}

static void ar__apply_wheel(ar_ctx *c)
{
    ar_i32 i;

    /* A drag owns its container for as long as it lasts, and a notch arriving
       mid-drag would fight it for the same position. */
    if (!c->wheel || !c->mouse_inside || c->drag_key)
    {
        return;
    }
    for (i = (c->order ? c->order_count : c->node_count) - 1; i >= 0; --i)
    {
        ar_i32   at = c->order ? c->order[i] : i;
        ar_node *n = &c->nodes[at];
        ar_slot *slot;
        ar_i32   want;

        if (!ar_is_scroll_container(n) || n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (!ar_rect_contains(n->rect, c->mouse_x, c->mouse_y))
        {
            continue;
        }
        slot = ar_ctx_slot(c, n->key);
        if (!slot)
        {
            continue;
        }
        want = ar_scroll_clamp(n, slot->scroll - c->wheel * AR_SCROLL_STEP);
        if (want == slot->scroll)
        {
            continue; /* nowhere to go here; the notch chains outwards */
        }
        ar__scroll_moved(c, n->key, want - slot->scroll);
        slot->scroll = want;
        ar_damage_add(&c->damage, n->rect);
        c->scrolled = 1;
        return;
    }
}

static void ar__update_hot(ar_ctx *c)
{
    ar_u32 was = c->hot;
    ar_i32 i;

    c->hot = 0;
    if (!c->mouse_inside)
    {
        return;
    }
    /*
     * Front to back, in reverse paint order: the first box found under the
     * cursor is the one on top, which is the one the cursor is actually over.
     *
     * It used to be a forward walk of the array taking the last match, which
     * is the same answer only while paint order and declaration order agree.
     * The moment anything is positioned they stop agreeing, and clicking a
     * dropdown would have hit whatever was behind it.
     */
    for (i = (c->order ? c->order_count : c->node_count) - 1; i >= 0; --i)
    {
        ar_i32   at = c->order ? c->order[i] : i;
        ar_node *n = &c->nodes[at];

        if (n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (ar_rect_contains(n->rect, c->mouse_x, c->mouse_y))
        {
            c->hot = n->key;
            break;
        }
    }

    /* Hover is resolved from the previous frame, so the frame that discovers
       a new box under the cursor is not the frame that can style it. A caller
       whose event pump blocks when idle would therefore show the highlight one
       event late, or not at all. Saying so here lets it ask for one more
       frame, which is the entire cost of the one frame delay. */
    c->hot_changed = (was != c->hot);
}

/* ------------------------------------------------------------------------
 * Scroll by region move
 *
 * Scrolling defeats damage tracking, because every box inside the container
 * moved and the walk in ar_frame_end faithfully reports every one of them: the
 * container repaints whole, and scroll_container measured a dirty ratio of
 * 1.000 against the under-25% both 0.1.2 and 0.6.0 asked for.
 *
 * Those pixels are not wrong, though. They are in the wrong place. So they get
 * moved inside the surface and only the band that came into view is painted --
 * the mechanism 0.1.2 deferred to 0.6.0 and 0.6.0 deferred again.
 * ------------------------------------------------------------------------ */
typedef struct ar__move
{
    ar_i32  container; /* -1 when no move was taken */
    ar_i32  dy;
    ar_rect area;  /* what the container confines its children to */
    ar_rect strip; /* the band that came into view */
    ar_rect bar;   /* the scrollbar track, whose thumb has moved */
} ar__move;

/*
 * ar__content_clip is defined up beside the paint pass now, because the clips
 * are built there and both readers should be looking at the same answer.
 */

static int ar__is_within(const ar_ctx *c, ar_i32 i, ar_i32 root)
{
    ar_i32 at;

    for (at = i; at >= 0; at = c->nodes[at].parent)
    {
        if (at == root)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * Non-zero when nothing outside this container's subtree lands on `area`.
 *
 * Anything that does would have its pixels dragged along by the move and then
 * never painted back: a fixed header over a list, a dropdown open across it, a
 * status bar that happens to overlap. That is the failure 0.6.0's document
 * named, and the reason the move is conditional rather than always taken.
 *
 * O(n), with an ancestor walk only for the boxes that actually overlap, once on
 * a frame that scrolled -- against repainting the whole container, which is what
 * it is competing with.
 */
static int ar__move_is_unobstructed(const ar_ctx *c, ar_i32 container, ar_rect area)
{
    ar_i32 i;

    for (i = 0; i < c->node_count; ++i)
    {
        const ar_node *n = &c->nodes[i];

        if (i == container || n->style.v[AR_P_DISPLAY] == AR_DISPLAY_NONE)
        {
            continue;
        }
        if (ar_rect_is_empty(ar_rect_intersect(n->rect, area)))
        {
            continue;
        }
        if (!ar__is_within(c, i, container))
        {
            return 0;
        }
    }
    return 1;
}

/*
 * Moves one scrolled container's pixels, and reports what still has to be
 * painted.
 *
 * At most one container, deliberately. ar__apply_wheel returns after the first
 * one it moves, so a wheel notch can only put one out of date; a caller driving
 * ar_node_scroll_to could move several, and rather than order those moves
 * against each other -- which is exactly where a nested pair would go wrong --
 * such a frame gives up and repaints.
 *
 * The debt is marked settled either way, because the damage walk that follows
 * repaints anything this declines to move.
 */
static ar__move ar__region_move(ar_ctx *c, ar_surface *s, ar_rect viewport)
{
    ar__move m;
    ar_i32   i, found = -1, dy = c->move_dy;
    ar_i32   mx, mw, keep, scroll = 0;
    ar_rect  track, thumb;

    m.container = -1;
    m.dy = 0;
    m.area = ar_rect_make(0, 0, 0, 0);
    m.strip = ar_rect_make(0, 0, 0, 0);
    m.bar = ar_rect_make(0, 0, 0, 0);

    if (dy == 0 && !c->move_many)
    {
        return m;
    }

    /* Without a surface nothing can be moved and nothing was painted, so the
       distance is still owed and the record has to stand. */
    if (!s)
    {
        return m;
    }

    /* From here the debt is settled either by the move below or by the damage
       walk that follows it, which repaints whatever this declines. */
    c->move_dy = 0;
    if (c->move_many)
    {
        c->move_many = 0;
        return m;
    }
    if (c->damage.all)
    {
        return m;
    }

    /* The record carries a key rather than an index, because the tree it
       referred to has been rebuilt since. */
    for (i = 0; i < c->node_count; ++i)
    {
        if (c->nodes[i].key == c->move_key && ar_is_scroll_container(&c->nodes[i]))
        {
            ar_slot *sl = ar_ctx_slot(c, c->nodes[i].key);

            found = i;
            scroll = sl ? sl->scroll : 0;
            break;
        }
    }
    if (found < 0)
    {
        return m;
    }

    m.area = ar_rect_intersect(ar__content_clip(&c->nodes[found]), viewport);
    keep = m.area.h - (dy < 0 ? -dy : dy);
    if (ar_rect_is_empty(m.area) || keep <= 0)
    {
        return m;
    }
    if (!ar__move_is_unobstructed(c, found, m.area))
    {
        return m;
    }

    /*
     * The scrollbar is drawn inside the right edge, so a blit across the whole
     * width would drag the thumb along with the content. Its column is left out
     * of the move and painted on its own.
     *
     * That is also why the strip stops where the track starts: ar__paint takes
     * one region and is called once per rectangle, and two rectangles that
     * overlapped would blend the overlap twice.
     */
    mx = m.area.x;
    mw = m.area.w;
    if (ar_scroll_bar_visible(&c->nodes[found]))
    {
        ar_scroll_bar(&c->nodes[found], scroll, &track, &thumb);
        track = ar_rect_intersect(track, m.area);
        if (!ar_rect_is_empty(track))
        {
            m.bar = track;
            if (track.x > m.area.x)
            {
                mw = track.x - m.area.x;
            }
        }
    }
    if (mw <= 0)
    {
        return m;
    }

    if (dy > 0)
    {
        /* Scrolled down: the content rises, and the band appears at the foot. */
        if (!ar_surface_move_rows(s, mx, mw, m.area.y + dy, m.area.y, keep))
        {
            return m;
        }
        m.strip = ar_rect_make(mx, m.area.y + keep, mw, dy);
    }
    else
    {
        if (!ar_surface_move_rows(s, mx, mw, m.area.y, m.area.y - dy, keep))
        {
            return m;
        }
        m.strip = ar_rect_make(mx, m.area.y, mw, -dy);
    }

    m.container = found;
    m.dy = dy;
    return m;
}

ar_rect ar_frame_end(ar_ctx *c, ar_surface *s)
{
    ar_rect  viewport;
    ar_rect  damage;
    ar__move move;
    ar_i32   i;
    ar_i32   survivors = 0;    /* boxes present this frame that were present last */
    int      other_damage = 0; /* something changed that the move does not explain */

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

    /* :last-child, :only-child and :empty could not be answered while the tree
       was being built. This is the first moment they can be. */
    ar__resolve_late(c);

    /* Style resolution happened during tree building, between frame_begin and
       here, so closing that phase now attributes it correctly. */
    ar_perf_mark(&c->perf, AR_PHASE_STYLE, ar__now(c));

    {
        ar_layout_env env;

        env.wrap = ar__wrap_cb;
        env.measure = ar__range_px;
        env.ud = c;
        env.scroll_of = ar__scroll_of;
        env.frags = c->frags;
        env.frag_cap = c->frag_cap;
        env.frag_used = 0;

        ar_layout_solve(c->nodes, c->node_count, viewport, &env);
        c->frag_count = env.frag_used;
    }

    /* Paint order, once the rectangles are final: a stacking context's bucket
       depends on nothing layout decides, but its subtree has to be walked and
       there is no reason to walk it twice. */
    c->order_count = c->order ? ar_stack_order(c->nodes, c->node_count, c->order, c->node_cap) : 0;

    /* Clips, once the rectangles are final and before anything asks. */
    ar__clip_tree(c, viewport);
    ar_perf_mark(&c->perf, AR_PHASE_LAYOUT, ar__now(c));

    /* A resize repaints everything: every box moved, and the surface behind
       them is new memory. */
    if (viewport.w != c->last_viewport.w || viewport.h != c->last_viewport.h)
    {
        ar_damage_add_all(&c->damage);
    }
    c->last_viewport = viewport;
    ar_damage_set_viewport(&c->damage, viewport);

    /*
     * Before the walk below, because the walk is what would otherwise report
     * every box in a scrolled container as having moved.
     *
     * The moved pixels still have to be presented -- they changed on screen even
     * though nothing repainted them -- so the whole region goes into damage. It
     * is only the painting that gets to skip it.
     */
    move = ar__region_move(c, s, viewport);
    if (move.container >= 0)
    {
        ar_damage_add(&c->damage, move.area);
    }

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
            other_damage = 1;
            continue;
        }

        if (slot->seen == c->frame - 1)
        {
            ++survivors;
        }

        if (slot->seen != c->frame - 1 || slot->digest != digest)
        {
            ar_damage_add(&c->damage, n->rect);
            other_damage = 1;
        }
        else if (slot->rect.x != n->rect.x || slot->rect.y != n->rect.y ||
                 slot->rect.w != n->rect.w || slot->rect.h != n->rect.h)
        {
            /*
             * A box inside a container whose pixels were just moved is already
             * drawn in its new place -- but only if it moved by exactly the
             * distance that move covered, and in no other way. A sticky header
             * that stayed put, or anything the move did not explain, falls
             * through and is repainted as usual.
             */
            if (move.container >= 0 && n->rect.x == slot->rect.x &&
                n->rect.y == slot->rect.y - move.dy && n->rect.w == slot->rect.w &&
                n->rect.h == slot->rect.h && ar__is_within(c, i, move.container))
            {
                /* Its pixels were moved, not repainted. */
            }
            else
            {
                ar_damage_add(&c->damage, slot->rect);
                ar_damage_add(&c->damage, n->rect);
                other_damage = 1;
            }
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
                other_damage = 1;
            }
        }
    }
    c->seen_last = survivors;

    damage = ar_damage_bounds(&c->damage, viewport);

    /*
     * Not gated on damage.all, and that is the whole subtlety.
     *
     * The moved region is added to damage above so the backend presents it --
     * those pixels really did change on screen. On a full-window container that
     * is more than half the surface, so ar_damage_add collapses it to "repaint
     * everything", and gating on that flag here meant the damage added for
     * presenting switched off the very painting saving it was recording. The
     * measurement said so plainly: the move engaged on every frame and fill_px
     * did not move at all.
     *
     * A caller that asked for a full repaint before the move is already handled,
     * inside ar__region_move, which checks damage.all before it touches a pixel.
     */
    if (s && move.container >= 0 && !other_damage)
    {
        /*
         * Two rectangles, painted one at a time and disjoint by construction.
         *
         * ar__paint takes a single region, so one call would have to be given
         * the bounding box of the band and the scrollbar track -- and the
         * bounding box of a horizontal band and a full-height track is very
         * nearly the whole container, which is precisely the cost the move
         * exists to avoid.
         *
         * Only when nothing else changed. Anything damaged outside the container
         * would go unpainted here, so a frame that both scrolls and changes
         * something else takes the ordinary path below and keeps the pixel move
         * as a harmless waste.
         */
        if (!ar_rect_is_empty(move.strip))
        {
            ar__paint(c, s, move.strip);
        }
        if (!ar_rect_is_empty(move.bar))
        {
            ar__paint(c, s, move.bar);
        }
    }
    else if (s && damage.w > 0 && damage.h > 0)
    {
        ar__paint(c, s, damage);
    }
    ar__update_hot(c);
    ar__apply_drag(c);
    ar__apply_wheel(c);

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
