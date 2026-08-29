/*
 * areole - the HTML fuzzer.
 * SPDX-License-Identifier: MIT
 *
 *     ar_fuzz                       one million iterations, seed 1
 *     ar_fuzz --iters 50000000      the acceptance criterion
 *     ar_fuzz --seed 7 --iters 1e5  a different stream
 *     ar_fuzz --seed 7 --skip 41337 replay one failure
 *     ar_fuzz --file bad.html --caps 512,1,8192,1    parse one file, once
 *
 * 0.9.0 acceptance criterion 8: fifty million iterations with no crash, no
 * hang and no out-of-bounds read. HTML is untrusted input by definition -- it
 * is the one input this library takes that somebody else wrote -- so this is a
 * gate rather than an exercise.
 *
 * ------------------------------------------------------------------------
 * Why it is written here rather than driven by libFuzzer or AFL
 *
 * Both are better fuzzers than this one. Both are also dependencies, and the
 * rule is zero of those, fetched at build time or otherwise. What that costs
 * is coverage guidance: this walks the input space blind where a modern fuzzer
 * would steer towards new branches.
 *
 * What it buys is that anybody can run it, on any machine, from a clean
 * checkout, with no toolchain beyond the compiler already required. A gate
 * nobody can run is not a gate. If a coverage-guided run is ever wanted, the
 * seeds and the mutators here are the corpus to hand it.
 *
 * ------------------------------------------------------------------------
 * How a failure is caught without a sanitiser
 *
 * Three ways, because a crash is only the loudest of the three and the other
 * two are the ones that would otherwise ship:
 *
 *   1. **Canaries.** Every buffer the parser writes into -- nodes, attributes,
 *      text, scratch -- is bracketed by a known byte pattern that is checked
 *      after every single iteration. An overrun by one byte is caught even
 *      though nothing crashed and the tree came out plausible.
 *
 *      The capacity the parser is *told* about varies, and the array is placed
 *      so that its declared end lands exactly on the trailing canary. A cap of
 *      seven nodes in a five-hundred-node buffer would let an overrun hide in
 *      the slack; this way the very first byte past the capacity is a tripwire.
 *
 *      That is also what exercises criterion 7 -- fail cleanly and say so,
 *      rather than truncate silently. A quarter of iterations run with a
 *      capacity small enough to hit, including zero.
 *
 *   2. **Tree invariants.** After every parse the whole tree is walked: every
 *      parent index in range, every child's parent pointing back, the first
 *      and last child agreeing with the sibling chain, no cycles, every
 *      attribute range inside the table, and every name and text span owned by
 *      something -- see `span_is_owned`, which the first iteration of the first
 *      run corrected.
 *
 *   3. **Progress.** A hang shows up as the iteration counter stopping, which
 *      the operator sees and CI sees as a timeout. The tokenizer and the tree
 *      builder are both written with bounded loops, and ar_test has checks
 *      that they terminate on malformed input; this is the wider net.
 *
 * ------------------------------------------------------------------------
 * Reproducibility
 *
 * The stream is a seeded xorshift, so iteration N of seed S is always the same
 * bytes. A failure prints its seed and iteration, and `--seed S --skip N`
 * replays exactly it. A fuzzer that cannot reproduce its own findings is a
 * random number generator with a bad conscience.
 */
#include "ar_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Seeds
 *
 * The corpus cases, near enough. A blind mutator starting from random bytes
 * spends its life in the "not markup at all" corner; starting from real
 * documents puts it near the branches that matter.
 * ------------------------------------------------------------------------ */
static const char *const SEEDS[] = {
    "<!DOCTYPE html><html><head><title>t</title></head><body><p>hi</p></body></html>",
    "<p>a<p>b",
    "<b><i><p></b></i>",
    "<b>1<i>2<em>3</b>4</em>5</i>",
    "<table><em>x</em><tr><td>y</table>",
    "<table><thead><tr><td>h<tbody><tr><td>b</table>",
    "<ul><li>a<li>b</ul>",
    "<select><option>a<option>b</select>",
    "<template><p>x</p></template>",
    "<frameset><frame></frameset>",
    "<style>a{content:\"<b>\";}</style><p>after",
    "<title>a<b>c</title>",
    "<textarea><p>x</textarea>",
    "<!-- c --><p>a&amp;b&#147;&#xD800;&nosuch;",
    "<div id=\"a\" class='b c' hidden>x</div>",
    "</p></div></b><p>a",
    "<?php echo 1; ?>",
    "<!DOCTYPE HTML PUBLIC \"-/"
    "/W3C/"
    "/DTD HTML 4.01/"
    "/EN\">",
    "<plaintext><b>x",
    "<a><a><a>x",
    "&#",
    "<!",
    "<",
    ""};

#define SEED_COUNT ((int)(sizeof SEEDS / sizeof SEEDS[0]))

/* ------------------------------------------------------------------------
 * The stream
 *
 * xorshift32. Integer, three lines, and the same sequence everywhere -- which
 * is the whole requirement, since the only thing asked of it is that seed and
 * iteration together name one input.
 * ------------------------------------------------------------------------ */
static ar_u32 g_state = 1u;

static ar_u32 rnd(void)
{
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}

static ar_u32 rnd_below(ar_u32 n)
{
    return n ? rnd() % n : 0u;
}

/* ------------------------------------------------------------------------
 * Buffers, with canaries
 * ------------------------------------------------------------------------ */
#define GUARD       64u
#define MAX_INPUT   4096u
#define NODE_CAP    512
#define ATTR_CAP    128
#define TEXT_CAP    8192u
#define SCRATCH_CAP 2048u

#define CANARY 0xA5u

static unsigned char g_nodes_buf[GUARD + NODE_CAP * sizeof(ar_dom_node) + GUARD];
static unsigned char g_attrs_buf[GUARD + ATTR_CAP * sizeof(ar_attr) + GUARD];
static unsigned char g_text_buf[GUARD + TEXT_CAP + GUARD];
static unsigned char g_scratch_buf[GUARD + SCRATCH_CAP + GUARD];

static char   g_input[MAX_INPUT + 1];
static ar_u32 g_input_len;

static void arm(unsigned char *b, ar_u32 total)
{
    memset(b, CANARY, GUARD);
    memset(b + total - GUARD, CANARY, GUARD);
}

/*
 * Where an array of `cap` items ending flush against the trailing canary
 * starts. The unused front of the buffer is left alone -- it is slack, not a
 * guard, and the parser has no business there either way.
 */
static void *flush_end(unsigned char *b, ar_u32 total, ar_u32 bytes)
{
    return b + total - GUARD - bytes;
}

/* A capacity to declare: usually generous, sometimes small enough to hit. */
static ar_u32 pick_cap(ar_u32 full)
{
    switch (rnd_below(8u))
    {
    case 0:
        return 0u;
    case 1:
        return 1u;
    case 2:
        return 1u + rnd_below(8u);
    default:
        return full;
    }
}

static int intact(const unsigned char *b, ar_u32 total, const char *what)
{
    ar_u32 i;

    for (i = 0; i < GUARD; ++i)
    {
        if (b[i] != CANARY)
        {
            printf("  canary before %s clobbered at -%lu\n", what, (unsigned long)(GUARD - i));
            return 0;
        }
        if (b[total - GUARD + i] != CANARY)
        {
            printf("  canary after %s clobbered at +%lu\n", what, (unsigned long)i);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * Mutation
 * ------------------------------------------------------------------------ */
static void pick_seed(void)
{
    const char *s = SEEDS[rnd_below(SEED_COUNT)];
    ar_u32      n = (ar_u32)strlen(s);

    if (n > MAX_INPUT)
    {
        n = MAX_INPUT;
    }
    memcpy(g_input, s, n);
    g_input_len = n;
}

/* The bytes markup is made of, so a flip lands on something structural more
   often than one in two hundred and fifty-six times. */
static const char INTERESTING[] = "<>/=\"'& !-\t\n\r;#abpdivtable";

static void mutate(void)
{
    ar_u32 op = rnd_below(8u);
    ar_u32 at;

    switch (op)
    {
    case 0: /* flip a byte to something structural */
        if (g_input_len)
        {
            at = rnd_below(g_input_len);
            g_input[at] = INTERESTING[rnd_below((ar_u32)sizeof INTERESTING - 1u)];
        }
        break;

    case 1: /* flip a byte to anything at all, high bytes included */
        if (g_input_len)
        {
            at = rnd_below(g_input_len);
            g_input[at] = (char)(rnd() & 0xFFu);
        }
        break;

    case 2: /* delete a run */
        if (g_input_len > 1u)
        {
            ar_u32 n = 1u + rnd_below(g_input_len / 2u);

            at = rnd_below(g_input_len - n + 1u);
            memmove(g_input + at, g_input + at + n, g_input_len - at - n);
            g_input_len -= n;
        }
        break;

    case 3: /* insert a structural byte */
        if (g_input_len < MAX_INPUT)
        {
            at = rnd_below(g_input_len + 1u);
            memmove(g_input + at + 1, g_input + at, g_input_len - at);
            g_input[at] = INTERESTING[rnd_below((ar_u32)sizeof INTERESTING - 1u)];
            ++g_input_len;
        }
        break;

    case 4: /* duplicate a run, which is how nesting gets deep */
        if (g_input_len && g_input_len * 2u < MAX_INPUT)
        {
            ar_u32 n = 1u + rnd_below(g_input_len);

            at = rnd_below(g_input_len - n + 1u);
            /* The move leaves the run where it was and puts a copy of it
               immediately after, which is the cheapest way to get depth. */
            memmove(g_input + at + n, g_input + at, g_input_len - at);
            g_input_len += n;
        }
        break;

    case 5: /* splice a whole seed in */
    {
        const char *s = SEEDS[rnd_below(SEED_COUNT)];
        ar_u32      n = (ar_u32)strlen(s);

        if (g_input_len + n < MAX_INPUT)
        {
            at = rnd_below(g_input_len + 1u);
            memmove(g_input + at + n, g_input + at, g_input_len - at);
            memcpy(g_input + at, s, n);
            g_input_len += n;
        }
        break;
    }

    case 6: /* truncate: half of all real failures are a document that stops */
        if (g_input_len)
        {
            g_input_len = rnd_below(g_input_len);
        }
        break;

    default: /* repeat one byte many times, for the depth caps */
        if (g_input_len && g_input_len + 64u < MAX_INPUT)
        {
            char   c = g_input[rnd_below(g_input_len)];
            ar_u32 k;

            for (k = 0; k < 64u; ++k)
            {
                g_input[g_input_len++] = c;
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------------
 * The invariants a tree must satisfy however malformed its input was
 * ------------------------------------------------------------------------ */
/*
 * The names the tree builder supplies itself.
 *
 * An implied element -- the <html> a document without one gets, the <tbody> a
 * bare <tr> gets -- has no bytes in the input to point at, so its name is a
 * string literal in the binary. That is a third home for a span beyond the two
 * ar_html.h describes, and this fuzzer found it on its first iteration.
 *
 * Matching the six words is a tighter check than a pointer range would be: a
 * range test passes for any address that happens to land inside .rodata, and a
 * corrupted pointer does not spell "tbody".
 */
static const char *const IMPLIED[] = {"html", "head", "body", "p", "tbody", "tr"};

static int span_is_owned(ar_span s, const char *input, ar_u32 in_len, const char *text_base,
                         ar_u32 text_cap)
{
    int i;

    if (s.n == 0)
    {
        return 1;
    }
    if (s.p >= input && s.p + s.n <= input + in_len)
    {
        return 1;
    }
    if (s.p >= text_base && s.p + s.n <= text_base + text_cap)
    {
        return 1;
    }
    for (i = 0; i < (int)(sizeof IMPLIED / sizeof IMPLIED[0]); ++i)
    {
        if (s.n == (ar_u32)strlen(IMPLIED[i]) && memcmp(s.p, IMPLIED[i], s.n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int tree_is_sane(const ar_doc *d, const char *input, ar_u32 in_len, const char *text_base)
{
    ar_i32 i;

    if (d->node_count > d->node_cap || d->attr_count > d->attr_cap || d->text_used > d->text_cap)
    {
        printf("  a count ran past its capacity\n");
        return 0;
    }

    for (i = 0; i < d->node_count; ++i)
    {
        const ar_dom_node *n = &d->nodes[i];
        ar_i32             c;
        ar_i32             guard;

        if (n->parent >= d->node_count || n->first_child >= d->node_count ||
            n->last_child >= d->node_count || n->next_sibling >= d->node_count ||
            n->prev_sibling >= d->node_count)
        {
            printf("  node %ld points outside the tree\n", (long)i);
            return 0;
        }
        if (n->parent == i || n->first_child == i || n->last_child == i || n->next_sibling == i ||
            n->prev_sibling == i)
        {
            printf("  node %ld points at itself: parent %ld first %ld last %ld next %ld prev %ld\n",
                   (long)i, (long)n->parent, (long)n->first_child, (long)n->last_child,
                   (long)n->next_sibling, (long)n->prev_sibling);
            return 0;
        }

        /* Every child agrees who its parent is, and the chain terminates. */
        guard = 0;
        for (c = n->first_child; c >= 0; c = d->nodes[c].next_sibling)
        {
            if (d->nodes[c].parent != i)
            {
                printf("  node %ld has a child that disowns it\n", (long)i);
                return 0;
            }
            if (++guard > d->node_count)
            {
                printf("  node %ld has a cycle in its children\n", (long)i);
                return 0;
            }
        }
        if (n->first_child < 0 && n->last_child >= 0)
        {
            printf("  node %ld has a last child and no first\n", (long)i);
            return 0;
        }

        /*
         * Every span points into the caller's input or the document's own text
         * buffer, and nowhere else.
         *
         * This is the check that would catch the tokenizer's scratch escaping
         * into the tree -- a failure that does not crash and does not look
         * wrong until the buffer is reused, which is the next token.
         */
        if (!span_is_owned(n->name, input, in_len, text_base, d->text_cap))
        {
            printf("  node %ld has a name owned by nothing\n", (long)i);
            return 0;
        }
        if (!span_is_owned(n->text, input, in_len, text_base, d->text_cap))
        {
            printf("  node %ld has text owned by nothing\n", (long)i);
            return 0;
        }

        /*
         * The attribute table, which nothing else here would notice. An
         * element's attributes are a range into one shared array, so a range
         * that runs past the end reads somebody else's attributes rather than
         * crashing -- exactly the kind of failure that ships.
         */
        if (n->attr_count > 0)
        {
            ar_i32 a;

            if (n->attr_first < 0 || n->attr_first + n->attr_count > d->attr_count)
            {
                printf("  node %ld names attributes outside the table\n", (long)i);
                return 0;
            }
            for (a = 0; a < n->attr_count; ++a)
            {
                const ar_attr *at = &d->attrs[n->attr_first + a];

                if (!span_is_owned(at->name, input, in_len, text_base, d->text_cap) ||
                    !span_is_owned(at->value, input, in_len, text_base, d->text_cap))
                {
                    printf("  node %ld has an attribute owned by nothing\n", (long)i);
                    return 0;
                }
            }
        }
    }
    return 1;
}

static void print_input(void)
{
    ar_u32 i;

    printf("  input (%lu bytes): \"", (unsigned long)g_input_len);
    for (i = 0; i < g_input_len && i < 512u; ++i)
    {
        unsigned char c = (unsigned char)g_input[i];

        if (c == '"' || c == '\\')
        {
            printf("\\%c", c);
        }
        else if (c >= 0x20u && c < 0x7Fu)
        {
            putchar((int)c);
        }
        else
        {
            printf("\\x%02X", (unsigned)c);
        }
    }
    printf("%s\"\n", g_input_len > 512u ? " ..." : "");
}

/*
 * One file, one parse, one set of capacities.
 *
 * This is what a minimiser drives: shrink the input, run this, see whether it
 * still fails. Without it the only handle on a finding is "iteration 315 of
 * seed 1", which reproduces but does not shrink -- and a five-hundred-byte
 * input with the bug somewhere in it is a lead, not a diagnosis.
 */
static int run_one(const char *path, ar_u32 node_cap, ar_u32 attr_cap, ar_u32 text_cap,
                   ar_u32 scratch_cap)
{
    ar_doc doc;
    FILE  *f = fopen(path, "rb");
    char  *nodes;
    char  *attrs;
    char  *text;
    char  *scratch;

    if (!f)
    {
        printf("cannot open %s\n", path);
        return 2;
    }
    g_input_len = (ar_u32)fread(g_input, 1, MAX_INPUT, f);
    fclose(f);

    if (node_cap > (ar_u32)NODE_CAP)
    {
        node_cap = (ar_u32)NODE_CAP;
    }
    if (attr_cap > (ar_u32)ATTR_CAP)
    {
        attr_cap = (ar_u32)ATTR_CAP;
    }
    if (text_cap > TEXT_CAP)
    {
        text_cap = TEXT_CAP;
    }
    if (scratch_cap > SCRATCH_CAP)
    {
        scratch_cap = SCRATCH_CAP;
    }

    arm(g_nodes_buf, (ar_u32)sizeof g_nodes_buf);
    arm(g_attrs_buf, (ar_u32)sizeof g_attrs_buf);
    arm(g_text_buf, (ar_u32)sizeof g_text_buf);
    arm(g_scratch_buf, (ar_u32)sizeof g_scratch_buf);

    nodes = (char *)flush_end(g_nodes_buf, (ar_u32)sizeof g_nodes_buf,
                              node_cap * (ar_u32)sizeof(ar_dom_node));
    attrs = (char *)flush_end(g_attrs_buf, (ar_u32)sizeof g_attrs_buf,
                              attr_cap * (ar_u32)sizeof(ar_attr));
    text = (char *)flush_end(g_text_buf, (ar_u32)sizeof g_text_buf, text_cap);
    scratch = (char *)flush_end(g_scratch_buf, (ar_u32)sizeof g_scratch_buf, scratch_cap);

    memset(&doc, 0, sizeof doc);
    doc.nodes = (ar_dom_node *)nodes;
    doc.node_cap = (ar_i32)node_cap;
    doc.attrs = (ar_attr *)attrs;
    doc.attr_cap = (ar_i32)attr_cap;
    doc.text = text;
    doc.text_cap = text_cap;

    ar_html_parse(&doc, g_input, g_input_len, scratch, scratch_cap);

    if (!intact(g_nodes_buf, (ar_u32)sizeof g_nodes_buf, "nodes") ||
        !intact(g_attrs_buf, (ar_u32)sizeof g_attrs_buf, "attrs") ||
        !intact(g_text_buf, (ar_u32)sizeof g_text_buf, "text") ||
        !intact(g_scratch_buf, (ar_u32)sizeof g_scratch_buf, "scratch") ||
        !tree_is_sane(&doc, g_input, g_input_len, text))
    {
        printf("FAIL on %s\n", path);
        return 1;
    }
    printf("ok: %ld nodes, %ld attrs, %lu text bytes, overflowed %d\n", (long)doc.node_count,
           (long)doc.attr_count, (unsigned long)doc.text_used, doc.overflowed);
    return 0;
}

int main(int argc, char **argv)
{
    ar_doc      doc;
    ar_u32      seed = 1u;
    double      want = 1000000.0;
    ar_u32      skip = 0u;
    const char *file = 0;
    const char *out_path = 0;
    ar_u32      caps[4];
    ar_u32      iters;
    ar_u32      i;
    ar_u32      total_nodes = 0u;
    ar_u32      overflowed = 0u;
    int         k;

    caps[0] = (ar_u32)NODE_CAP;
    caps[1] = (ar_u32)ATTR_CAP;
    caps[2] = TEXT_CAP;
    caps[3] = SCRATCH_CAP;

    for (k = 1; k < argc; ++k)
    {
        if (strcmp(argv[k], "--file") == 0 && k + 1 < argc)
        {
            file = argv[++k];
        }
        else if (strcmp(argv[k], "--out") == 0 && k + 1 < argc)
        {
            out_path = argv[++k];
        }
        else if (strcmp(argv[k], "--caps") == 0 && k + 1 < argc)
        {
            char *s = argv[++k];
            int   j;

            for (j = 0; j < 4 && *s; ++j)
            {
                caps[j] = (ar_u32)strtoul(s, &s, 10);
                if (*s == ',')
                {
                    ++s;
                }
            }
        }
        else if (strcmp(argv[k], "--seed") == 0 && k + 1 < argc)
        {
            seed = (ar_u32)strtoul(argv[++k], 0, 10);
        }
        else if (strcmp(argv[k], "--iters") == 0 && k + 1 < argc)
        {
            want = atof(argv[++k]);
        }
        else if (strcmp(argv[k], "--skip") == 0 && k + 1 < argc)
        {
            skip = (ar_u32)strtoul(argv[++k], 0, 10);
        }
        else
        {
            printf("ar_fuzz [--seed N] [--iters N] [--skip N]\n");
            printf("        [--file PATH] [--caps nodes,attrs,text,scratch]\n");
            return 2;
        }
    }

    if (file)
    {
        return run_one(file, caps[0], caps[1], caps[2], caps[3]);
    }
    if (want < 1.0)
    {
        want = 1.0;
    }
    iters = want > 4000000000.0 ? 4000000000u : (ar_u32)want;
    g_state = seed ? seed : 1u;

    printf("ar_fuzz  seed %lu  iterations %lu\n", (unsigned long)seed, (unsigned long)iters);

    for (i = 0; i < iters; ++i)
    {
        ar_u32 node_cap;
        ar_u32 attr_cap;
        ar_u32 text_cap;
        ar_u32 scratch_cap;
        char  *nodes;
        char  *attrs;
        char  *text;
        char  *scratch;
        ar_u32 m;

        /*
         * A fresh seed every so often, and several mutations each time in
         * between, so a lineage accumulates hundreds of edits before it is
         * abandoned. The first version reseeded every eight iterations and
         * applied one edit, and never built a document big enough to reach a
         * capacity -- eight nodes on average, where the cap was five hundred.
         */
        if ((i % 32u) == 0u)
        {
            pick_seed();
        }
        for (m = 1u + rnd_below(6u); m > 0u; --m)
        {
            mutate();
        }

        /*
         * Every draw for this iteration happens before the skip, so iteration N
         * is the same iteration whether or not the ones before it were parsed.
         *
         * The first version drew the capacities *after* the skip, which meant
         * `--skip` consumed four fewer numbers per iteration and replayed a
         * different stream entirely -- so the first hang this found reproduced
         * as a clean run and sent the investigation to the wrong iteration. A
         * fuzzer that cannot replay its own findings is a random number
         * generator with a bad conscience.
         */
        node_cap = pick_cap((ar_u32)NODE_CAP);
        attr_cap = pick_cap((ar_u32)ATTR_CAP);
        text_cap = pick_cap(TEXT_CAP);
        scratch_cap = pick_cap(SCRATCH_CAP);

        if (i < skip)
        {
            continue;
        }

        nodes = (char *)flush_end(g_nodes_buf, (ar_u32)sizeof g_nodes_buf,
                                  node_cap * (ar_u32)sizeof(ar_dom_node));
        attrs = (char *)flush_end(g_attrs_buf, (ar_u32)sizeof g_attrs_buf,
                                  attr_cap * (ar_u32)sizeof(ar_attr));
        text = (char *)flush_end(g_text_buf, (ar_u32)sizeof g_text_buf, text_cap);
        scratch = (char *)flush_end(g_scratch_buf, (ar_u32)sizeof g_scratch_buf, scratch_cap);

        arm(g_nodes_buf, (ar_u32)sizeof g_nodes_buf);
        arm(g_attrs_buf, (ar_u32)sizeof g_attrs_buf);
        arm(g_text_buf, (ar_u32)sizeof g_text_buf);
        arm(g_scratch_buf, (ar_u32)sizeof g_scratch_buf);

        memset(&doc, 0, sizeof doc);
        doc.nodes = (ar_dom_node *)nodes;
        doc.node_cap = (ar_i32)node_cap;
        doc.attrs = (ar_attr *)attrs;
        doc.attr_cap = (ar_i32)attr_cap;
        doc.text = text;
        doc.text_cap = text_cap;

        if (skip)
        {
            /* Investigating: hand the exact bytes over so a minimiser can take
               them, then get out of the way rather than parsing them here. */
            if (out_path)
            {
                FILE *of = fopen(out_path, "wb");

                if (!of)
                {
                    printf("cannot write %s\n", out_path);
                    return 2;
                }
                fwrite(g_input, 1, g_input_len, of);
                fclose(of);
                printf("wrote %lu bytes to %s\n", (unsigned long)g_input_len, out_path);
                printf("  caps: %lu,%lu,%lu,%lu\n", (unsigned long)node_cap,
                       (unsigned long)attr_cap, (unsigned long)text_cap,
                       (unsigned long)scratch_cap);
                return 0;
            }
            printf("iteration %lu\n", (unsigned long)i);
            printf("  caps: %lu nodes, %lu attrs, %lu text, %lu scratch\n", (unsigned long)node_cap,
                   (unsigned long)attr_cap, (unsigned long)text_cap, (unsigned long)scratch_cap);
            print_input();
            fflush(stdout);
        }

        ar_html_parse(&doc, g_input, g_input_len, scratch, scratch_cap);

        if (!intact(g_nodes_buf, (ar_u32)sizeof g_nodes_buf, "nodes") ||
            !intact(g_attrs_buf, (ar_u32)sizeof g_attrs_buf, "attrs") ||
            !intact(g_text_buf, (ar_u32)sizeof g_text_buf, "text") ||
            !intact(g_scratch_buf, (ar_u32)sizeof g_scratch_buf, "scratch") ||
            !tree_is_sane(&doc, g_input, g_input_len, text))
        {
            printf("FAIL at seed %lu iteration %lu\n", (unsigned long)seed, (unsigned long)i);
            printf("  caps: %lu nodes, %lu attrs, %lu text, %lu scratch\n", (unsigned long)node_cap,
                   (unsigned long)attr_cap, (unsigned long)text_cap, (unsigned long)scratch_cap);
            printf("  replay: ar_fuzz --seed %lu --skip %lu --iters %lu\n", (unsigned long)seed,
                   (unsigned long)i, (unsigned long)(i + 1u));
            print_input();
            return 1;
        }

        total_nodes += (ar_u32)doc.node_count;
        if (doc.overflowed)
        {
            ++overflowed;
        }

        if (iters >= 100000u && (i % (iters / 10u)) == 0u && i > 0u)
        {
            printf("  %lu%%\n", (unsigned long)(100u * i / iters));
            fflush(stdout);
        }
    }

    printf("%lu iterations, no crash, no overrun, every tree sane\n", (unsigned long)iters);
    printf("  %lu nodes built, %lu documents hit a capacity and said so\n",
           (unsigned long)total_nodes, (unsigned long)overflowed);
    return 0;
}
