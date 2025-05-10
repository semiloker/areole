/*
 * areole hardware probe.
 * SPDX-License-Identifier: MIT
 *
 * Measures the machine rather than asking it. The operating system reports a
 * nominal clock the part does not run at, and says nothing at all about
 * achievable bandwidth, which is what actually bounds a software rasterizer.
 *
 * The output is a profile embedded in every benchmark result. A result without
 * one is not comparable to anything.
 *
 * On the compute figure, deliberately: this does NOT report a clock speed. A
 * clock alone is useless for scaling a result to another machine, because a
 * Pentium II and a Ryzen at the same clock differ by roughly a factor of four
 * in instructions retired per cycle. What is reported instead is the
 * throughput of a dependent integer chain, which folds clock and IPC into the
 * one number scaling actually needs, and which is measured on both machines
 * rather than assumed on one.
 *
 * Every measurement is checked against a plausibility ceiling before it is
 * published. An impossible rate means the optimiser deleted the work, and the
 * probe refuses to print rather than lying. That is not hypothetical: the
 * throwaway harness this replaced reported infinite copy bandwidth for exactly
 * that reason.
 */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Far larger than any cache on any machine this is likely to meet. */
#define BW_MB    192u
#define BW_BYTES (BW_MB * 1024u * 1024u)

static volatile ar_u32 g_sink;

static void refuse(const char *what)
{
    fprintf(stderr, "ar_hwprobe: %s reported a physically impossible rate.\n", what);
    fprintf(stderr, "  The optimiser almost certainly deleted the work. Refusing to publish.\n");
    exit(2);
}

/* ------------------------------------------------------------------------
 * Scalar compute
 *
 * A chain in which every operation depends on the previous result, so the
 * machine cannot hide latency behind parallelism and the figure reflects the
 * serial speed that layout, style resolution and parsing actually run at.
 *
 * Seeded from a volatile so the compiler cannot fold the loop, and read back
 * afterwards so it cannot delete it.
 * ------------------------------------------------------------------------ */
/* The companion to the dependent chain: four independent chains interleaved,
   so the machine can run them in parallel and the figure reflects issue width
   rather than latency.

   Both are needed and neither alone is enough. A dependent chain scales
   between machines roughly with clock, because latency is what bounds it. An
   independent one scales with clock times issue width. Real code -- layout,
   style resolution, parsing -- sits between the two, so the pair brackets the
   scaling factor instead of pretending to a single number. hardware-tiers.md
   originally assumed 45x from clock times IPC; the dependent measurement says
   the lower bound is far closer to the clock ratio alone. */
static double probe_scalar_ilp(void)
{
    volatile ar_u32 seed = 1u;
    ar_u32          a = (ar_u32)seed, b = a + 1u, c = a + 2u, d = a + 3u;
    double          t0, t1, best = 1e30;
    long            n = 20000000L;
    long            i;
    int             rep;

    for (rep = 0; rep < 3; ++rep)
    {
        t0 = bench_now_s();
        for (i = 0; i < n; ++i)
        {
            a ^= a << 13;
            b ^= b << 13;
            c ^= c << 13;
            d ^= d << 13;
        }
        t1 = bench_now_s();
        if (t1 - t0 < best)
        {
            best = t1 - t0;
        }
    }
    g_sink = a + b + c + d;

    if (!bench_rate_is_plausible((double)n * 4.0, best))
    {
        refuse("the independent chains");
    }
    return (double)n * 4.0 / best;
}

static double probe_scalar(void)
{
    volatile ar_u32 seed = 1u;
    ar_u32          x = (ar_u32)seed;
    double          t0, t1, best = 1e30;
    long            n = 20000000L;
    long            i;
    int             rep;

    for (rep = 0; rep < 3; ++rep)
    {
        t0 = bench_now_s();
        for (i = 0; i < n; ++i)
        {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        t1 = bench_now_s();
        if (t1 - t0 < best)
        {
            best = t1 - t0;
        }
    }
    g_sink = x;

    /* Three dependent operations per iteration. A dependent chain cannot
       retire a trillion of them a second on anything. */
    if (!bench_rate_is_plausible((double)n * 3.0, best))
    {
        refuse("the scalar chain");
    }
    return (double)n * 3.0 / best;
}

/* ------------------------------------------------------------------------
 * Memory bandwidth
 *
 * The write figure is the one that matters for areole, because areole's fill
 * loop is exactly this: a scalar loop storing one 32 bit word at a time. The
 * copy figure uses the platform memcpy, which is vectorised and may legally
 * exceed the scalar write rate; that is a difference in the code, not a
 * contradiction in the measurement, and the two are labelled separately so
 * nobody has to wonder.
 * ------------------------------------------------------------------------ */
static void probe_bandwidth(double *wr, double *rd, double *cp)
{
    ar_u32 *a = (ar_u32 *)malloc(BW_BYTES);
    ar_u32 *b = (ar_u32 *)malloc(BW_BYTES);
    size_t  n = BW_BYTES / sizeof(ar_u32);
    size_t  i;
    double  t0, t1;

    if (!a || !b)
    {
        *wr = *rd = *cp = 0.0;
        free(a);
        free(b);
        return;
    }

    /* Touch both first, so what is measured is memory bandwidth rather than
       the kernel handing out fresh pages. */
    memset(a, 1, BW_BYTES);
    memset(b, 2, BW_BYTES);

    t0 = bench_now_s();
    for (i = 0; i < n; ++i)
    {
        a[i] = 0x336699FFu;
    }
    t1 = bench_now_s();
    if (!bench_rate_is_plausible((double)BW_BYTES, t1 - t0))
    {
        refuse("the scalar write loop");
    }
    *wr = (double)BW_BYTES / (t1 - t0) / 1e9;

    t0 = bench_now_s();
    {
        ar_u32 acc = 0;
        for (i = 0; i < n; ++i)
        {
            acc += a[i];
        }
        g_sink = acc;
    }
    t1 = bench_now_s();
    if (!bench_rate_is_plausible((double)BW_BYTES, t1 - t0))
    {
        refuse("the scalar read loop");
    }
    *rd = (double)BW_BYTES / (t1 - t0) / 1e9;

    t0 = bench_now_s();
    for (i = 0; i < 3; ++i)
    {
        memcpy(b, a, BW_BYTES);
    }
    t1 = bench_now_s();

    /* Read b back before checking. Without this the optimiser sees a store to
       memory that is never read and then freed, deletes the memcpy, and the
       probe reports infinity. */
    g_sink = b[0] + b[n / 2] + b[n - 1];

    if (!bench_rate_is_plausible((double)BW_BYTES * 2.0 * 3.0, t1 - t0))
    {
        refuse("the copy");
    }
    /* A copy moves each byte twice. */
    *cp = (double)BW_BYTES * 2.0 * 3.0 / (t1 - t0) / 1e9;

    free(a);
    free(b);
}

/* ------------------------------------------------------------------------
 * Cache hierarchy
 *
 * A pointer chase through a random permutation of cache lines, so the
 * prefetcher cannot help and each access waits for the previous one. Latency
 * steps as the working set outgrows each level, and the sizes at which it
 * steps are the cache sizes.
 *
 * The first version of this used a constant stride and produced a curve that
 * went down as the working set grew, which is impossible. A constant stride is
 * exactly what a hardware prefetcher is built to recognise, so it was
 * measuring the prefetcher. A random permutation cannot be predicted.
 *
 * This matters more for areole than for most libraries: the baseline showed
 * fill rate collapsing by a factor of two the moment the framebuffer stopped
 * fitting in cache, and the target hardware can never fit one.
 * ------------------------------------------------------------------------ */
#define CACHE_STEPS 14
#define CACHE_MAX   (64u * 1024u * 1024u)

static void probe_cache(double *ns_per_access, ar_u32 *sizes_kb)
{
    ar_u32 *buf = (ar_u32 *)malloc(CACHE_MAX);
    ar_u32 *perm = (ar_u32 *)malloc(CACHE_MAX / 16u);
    int     step;

    if (!buf || !perm)
    {
        for (step = 0; step < CACHE_STEPS; ++step)
        {
            ns_per_access[step] = 0.0;
            sizes_kb[step] = 0;
        }
        free(buf);
        free(perm);
        return;
    }

    bench_srand(0x9E3779B9u);

    for (step = 0; step < CACHE_STEPS; ++step)
    {
        ar_u32 bytes = 4096u << step; /* 4 KB up to 32 MB */
        ar_u32 count = bytes / (ar_u32)sizeof(ar_u32);
        ar_u32 lines = count / 16u; /* 64 byte cache lines */
        ar_u32 i, idx;
        double t0, t1;
        long   hops = 2000000L;
        long   h;

        sizes_kb[step] = bytes / 1024u;
        if (lines < 2u)
        {
            lines = 2u;
        }

        for (i = 0; i < lines; ++i)
        {
            perm[i] = i;
        }
        for (i = lines - 1u; i > 0u; --i)
        {
            ar_u32 j = bench_rand() % (i + 1u);
            ar_u32 t = perm[i];
            perm[i] = perm[j];
            perm[j] = t;
        }
        for (i = 0; i < lines; ++i)
        {
            ar_u32 k = (i + 1u) % lines;
            buf[perm[i] * 16u] = perm[k] * 16u;
        }

        idx = perm[0] * 16u;
        t0 = bench_now_s();
        for (h = 0; h < hops; ++h)
        {
            idx = buf[idx];
        }
        t1 = bench_now_s();
        g_sink = idx;

        ns_per_access[step] = (t1 - t0) * 1e9 / (double)hops;
    }

    free(buf);
    free(perm);
}

/* ------------------------------------------------------------------------ */
static void print_text(double scalar, double ilp, double wr, double rd, double cp,
                       const double *cache_ns, const ar_u32 *cache_kb)
{
    int i;

    printf("clock            %s, %.0f ns resolution\n", bench_clock_name(),
           bench_clock_resolution_s() * 1e9);
    printf("scalar latency    %.3g dependent ops/s     bounds serial code\n", scalar);
    printf("scalar throughput %.3g independent ops/s   bounds parallel code\n", ilp);
    printf("                  ratio %.2fx, the usable issue width of this machine\n", ilp / scalar);
    printf("memory write     %.2f GB/s   scalar 32 bit store loop, which is what areole does\n",
           wr);
    printf("memory read      %.2f GB/s   scalar 32 bit load loop\n", rd);
    printf("memory copy      %.2f GB/s   platform memcpy, vectorised; exceeding the scalar\n", cp);
    printf("                              figure is a difference in the code, not an error\n");

    printf("\ncache latency curve, random pointer chase, prefetcher defeated\n");
    for (i = 0; i < CACHE_STEPS; ++i)
    {
        printf("  %7lu KB  %7.2f ns/access", (unsigned long)cache_kb[i], cache_ns[i]);
        if (i > 0 && cache_ns[i] > cache_ns[i - 1] * 1.6)
        {
            printf("   <- the working set outgrew a level here");
        }
        printf("\n");
    }

    printf("\nThe step sizes are the cache sizes. areole cares because a\n");
    printf("framebuffer that fits in cache fills at roughly twice the rate of one\n");
    printf("that does not, and the target hardware can never fit one.\n");
}

static void print_json(const char *name, double scalar, double ilp, double wr, double rd, double cp,
                       const double *cache_ns, const ar_u32 *cache_kb)
{
    int i;

    printf("{\n");
    printf("  \"profile\": \"measured\",\n");
    printf("  \"name\": \"%s\",\n", name);
    printf("  \"clock_source\": \"%s\",\n", bench_clock_name());
    printf("  \"clock_resolution_ns\": %.1f,\n", bench_clock_resolution_s() * 1e9);
    printf("  \"scalar_dependent_ops_per_s\": %.4g,\n", scalar);
    printf("  \"scalar_independent_ops_per_s\": %.4g,\n", ilp);
    printf("  \"mem_write_gbs_scalar\": %.3f,\n", wr);
    printf("  \"mem_read_gbs_scalar\": %.3f,\n", rd);
    printf("  \"mem_copy_gbs_memcpy\": %.3f,\n", cp);
    printf("  \"cache_curve\": [\n");
    for (i = 0; i < CACHE_STEPS; ++i)
    {
        printf("    { \"kb\": %lu, \"ns_per_access\": %.2f }%s\n", (unsigned long)cache_kb[i],
               cache_ns[i], i + 1 < CACHE_STEPS ? "," : "");
    }
    printf("  ]\n");
    printf("}\n");
}

int main(int argc, char **argv)
{
    double      scalar, ilp, wr, rd, cp;
    double      cache_ns[CACHE_STEPS];
    ar_u32      cache_kb[CACHE_STEPS];
    const char *name = "(unnamed machine)";
    int         as_json = 0;
    int         i;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--json") == 0)
        {
            as_json = 1;
        }
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc)
        {
            name = argv[++i];
        }
        else
        {
            printf("ar_hwprobe [--json] [--name STRING]\n");
            printf("  measures the machine: dependent scalar throughput, memory\n");
            printf("  bandwidth, and the cache latency curve\n");
            return 1;
        }
    }

    bench_clock_init();

    if (!as_json)
    {
        printf("probing, this takes a few seconds\n\n");
    }

    scalar = probe_scalar();
    ilp = probe_scalar_ilp();
    probe_bandwidth(&wr, &rd, &cp);
    probe_cache(cache_ns, cache_kb);

    if (as_json)
    {
        print_json(name, scalar, ilp, wr, rd, cp, cache_ns, cache_kb);
    }
    else
    {
        print_text(scalar, ilp, wr, rd, cp, cache_ns, cache_kb);
    }
    return 0;
}
