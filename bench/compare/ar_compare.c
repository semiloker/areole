/*
 * areole benchmark - the comparison runner.
 * SPDX-License-Identifier: MIT
 *
 *   ar_compare --engine gdi --iters 200 --repeat 3
 *   ar_compare --all --json
 *
 * Runs the same case with areole and with a rival, alternating between them so
 * neither gets a systematically warmer machine, and reports the ratio.
 *
 * Alternating matters more than it sounds. Running all of one engine and then
 * all of the other gives the second one a hotter chip and a different clock
 * state, and on this laptop that is worth ten to forty per cent -- larger than
 * most differences worth measuring. Interleaving them cancels it.
 */
#include "compare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EPOCHS 16

typedef struct cmp_result
{
    const cmp_case *c;
    double          areole_p50, rival_p50;
    double          areole_spread, rival_spread;
    double          extra_us; /* zero when the case has none, or cannot answer */
} cmp_result;

static double g_a[BENCH_MAX_SAMPLES];
static double g_r[BENCH_MAX_SAMPLES];

static double median_of(double *v, int n)
{
    bench_sort(v, n);
    return bench_percentile(v, n, 50.0);
}

static void run_case(const cmp_case *cs, cmp_ctx *ctx, int iters, int warmup, int epochs,
                     cmp_result *out)
{
    double a_ep[MAX_EPOCHS], r_ep[MAX_EPOCHS];
    double t0, t1;
    int    ep, i;

    memset(out, 0, sizeof *out);
    out->c = cs;

    for (i = 0; i < warmup; ++i)
    {
        ctx->frame = (ar_u32)i;
        cs->areole(ctx);
        cs->rival(ctx);
    }

    for (ep = 0; ep < epochs; ++ep)
    {
        if (ep > 0)
        {
            bench_sleep_ms(250);
        }

        /* Alternating, one frame each, so neither engine sits on a warmer
           machine than the other. */
        for (i = 0; i < iters; ++i)
        {
            ctx->frame = (ar_u32)i;

            t0 = bench_now_s();
            cs->areole(ctx);
            t1 = bench_now_s();
            g_a[i] = t1 - t0;

            t0 = bench_now_s();
            cs->rival(ctx);
            t1 = bench_now_s();
            g_r[i] = t1 - t0;
        }
        a_ep[ep] = median_of(g_a, iters);
        r_ep[ep] = median_of(g_r, iters);
    }

    {
        double av[MAX_EPOCHS], rv[MAX_EPOCHS];
        double am, rm;

        memcpy(av, a_ep, (size_t)epochs * sizeof(double));
        memcpy(rv, r_ep, (size_t)epochs * sizeof(double));
        bench_sort(av, epochs);
        bench_sort(rv, epochs);
        am = bench_percentile(av, epochs, 50.0);
        rm = bench_percentile(rv, epochs, 50.0);

        out->areole_p50 = am;
        out->rival_p50 = rm;
        out->areole_spread = am > 0.0 ? (av[epochs - 1] - av[0]) * 100.0 / am : 0.0;
        out->rival_spread = rm > 0.0 ? (rv[epochs - 1] - rv[0]) * 100.0 / rm : 0.0;
    }

    /* The extra figure comes from areole's own ring, which holds the last
       AR_PERF_RING frames. Read it only when this case ran at least that many,
       otherwise the ring still holds frames from the case before and the number
       would be a blend of two different scenes. Reporting nothing beats
       reporting a number that quietly means something else. */
    if (cs->extra_us && warmup + iters * epochs >= (int)AR_PERF_RING)
    {
        out->extra_us = cs->extra_us();
    }
}

static void print_engine(const cmp_engine *eng, const cmp_result *rs, int n, int as_json)
{
    const char *label = "";
    int         i, extra;

    /* The extra column appears only when a case supplies one, so engines that
       compare like with like keep the narrow table. */
    for (i = 0, extra = 0; i < n; ++i)
    {
        if (rs[i].extra_us > 0.0)
        {
            extra = 1;
            label = rs[i].c->extra_label;
        }
    }

    if (as_json)
    {
        printf("  {\n");
        printf("    \"engine\": \"%s\",\n", eng->name);
        printf("    \"cases\": [\n");
        for (i = 0; i < n; ++i)
        {
            printf("      {\n");
            printf("        \"name\": \"%s\",\n", rs[i].c->name);
            printf("        \"what\": \"%s\",\n", rs[i].c->what);
            printf("        \"areole_us\": %.2f,\n", rs[i].areole_p50 * 1e6);
            printf("        \"rival_us\": %.2f,\n", rs[i].rival_p50 * 1e6);
            printf("        \"ratio\": %.3f,\n",
                   rs[i].areole_p50 > 0.0 ? rs[i].rival_p50 / rs[i].areole_p50 : 0.0);
            printf("        \"areole_spread_pct\": %.1f,\n", rs[i].areole_spread);
            printf("        \"rival_spread_pct\": %.1f,\n", rs[i].rival_spread);
            if (rs[i].extra_us > 0.0)
            {
                printf("        \"extra_label\": \"%s\",\n", rs[i].c->extra_label);
                printf("        \"extra_us\": %.2f,\n", rs[i].extra_us);
                printf("        \"extra_ratio\": %.3f,\n",
                       rs[i].rival_p50 * 1e6 / rs[i].extra_us);
            }
            printf("        \"fair\": %s\n", rs[i].c->caveat ? "false" : "true");
            printf("      }%s\n", i + 1 < n ? "," : "");
        }
        printf("    ]\n");
        printf("  }");
        return;
    }

    printf("\n%s versus areole\n", eng->name);
    printf("%s\n\n", eng->version);
    printf("%-16s %12s %12s %9s", "case", "areole", eng->name, "ratio");
    if (extra)
    {
        printf(" %12s %9s", label, "ratio");
    }
    printf("\n%-16s %12s %12s %9s", "----------------", "------------", "------------",
           "---------");
    if (extra)
    {
        printf(" %12s %9s", "------------", "---------");
    }
    printf("\n");

    for (i = 0; i < n; ++i)
    {
        double ratio = rs[i].areole_p50 > 0.0 ? rs[i].rival_p50 / rs[i].areole_p50 : 0.0;
        printf("%-16s %10.1fus %10.1fus %8.2fx", rs[i].c->name, rs[i].areole_p50 * 1e6,
               rs[i].rival_p50 * 1e6, ratio);
        if (extra)
        {
            if (rs[i].extra_us > 0.0)
            {
                printf(" %10.1fus %8.2fx", rs[i].extra_us,
                       rs[i].rival_p50 * 1e6 / rs[i].extra_us);
            }
            else
            {
                printf(" %12s %9s", "-", "-");
            }
        }
        printf("%s\n", rs[i].c->caveat ? "  (see note)" : "");
    }

    printf("\nA ratio above 1.00 means areole is faster.\n");
    if (extra)
    {
        printf("The '%s' column is the like-for-like one; see the note below.\n", label);
    }

    for (i = 0; i < n; ++i)
    {
        if (rs[i].c->caveat)
        {
            printf("\n%s: %s\n", rs[i].c->name, rs[i].c->caveat);
        }
    }

    printf("\nspreads between epochs: ");
    for (i = 0; i < n; ++i)
    {
        printf("%s %.0f/%.0f%%  ", rs[i].c->name, rs[i].areole_spread, rs[i].rival_spread);
    }
    printf("\nA ratio is only worth reading when both spreads are well under it.\n");
}

static void usage(void)
{
    printf("ar_compare - areole against the alternatives\n\n");
    printf("  --engine NAME      gdi (more as they are vendored)\n");
    printf("  --all              every available engine\n");
    printf("  --iters N          frames per engine per epoch (default 200)\n");
    printf("  --repeat N         epochs (default 3)\n");
    printf("  --warmup N         untimed frames (default 30)\n");
    printf("  --json             emit JSON\n\n");
    printf("Both engines draw into the same DIB section, in the same order, at the\n");
    printf("same coordinates, alternating one frame each so neither sits on a\n");
    printf("warmer machine. Cases that cannot be made fair carry a note saying so.\n");
}

int main(int argc, char **argv)
{
    static cmp_result results[CMP_MAX_CASES];
    const cmp_engine *engines[4];
    int               engine_count = 0;
    const char       *want = 0;
    int               iters = 200, warmup = 30, epochs = 3;
    int               as_json = 0, all = 0;
    int               i, e;

    bench_clock_init();

#if defined(_WIN32)
    engines[engine_count++] = cmp_engine_gdi();
#endif
    engines[engine_count++] = cmp_engine_clay();

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--engine") == 0 && i + 1 < argc)
        {
            want = argv[++i];
        }
        else if (strcmp(argv[i], "--all") == 0)
        {
            all = 1;
        }
        else if (strcmp(argv[i], "--json") == 0)
        {
            as_json = 1;
        }
        else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
        {
            iters = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
        {
            epochs = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc)
        {
            warmup = atoi(argv[++i]);
        }
        else
        {
            usage();
            return 1;
        }
    }

    if (engine_count == 0)
    {
        printf("ar_compare: no rival engine is available on this platform yet.\n");
        printf("GDI needs Windows; Clay, Nuklear, microui and LVGL need vendoring.\n");
        return 1;
    }
    if (!all && !want)
    {
        usage();
        return 1;
    }
    if (iters < 1)
    {
        iters = 1;
    }
    if (iters > BENCH_MAX_SAMPLES)
    {
        iters = BENCH_MAX_SAMPLES;
    }
    if (epochs < 1)
    {
        epochs = 1;
    }
    if (epochs > MAX_EPOCHS)
    {
        epochs = MAX_EPOCHS;
    }

    if (as_json)
    {
        printf("{\n");
        printf("  \"tool\": \"ar_compare\",\n");
        printf("  \"areole_version\": \"%s\",\n", ar_version());
        printf("  \"clock\": \"%s\",\n", bench_clock_name());
        printf("  \"iters\": %d,\n", iters);
        printf("  \"epochs\": %d,\n", epochs);
        printf("  \"engines\": [\n");
    }

    for (e = 0; e < engine_count; ++e)
    {
        const cmp_engine *eng = engines[e];
        const cmp_case   *cases;
        cmp_ctx           ctx;
        int               count = 0, k;

        if (want)
        {
            int match = (strcmp(eng->name, "Win32 GDI") == 0 && strcmp(want, "gdi") == 0) ||
                        (strcmp(eng->name, "Clay") == 0 && strcmp(want, "clay") == 0);
            if (!match)
            {
                continue;
            }
        }

        memset(&ctx, 0, sizeof ctx);
        ctx.w = 1024;
        ctx.h = 768;

        if (!eng->setup(&ctx))
        {
            fprintf(stderr, "ar_compare: %s failed to start\n", eng->name);
            continue;
        }

        cases = eng->cases(&count);
        if (count > CMP_MAX_CASES)
        {
            count = CMP_MAX_CASES;
        }
        for (k = 0; k < count; ++k)
        {
            run_case(&cases[k], &ctx, iters, warmup, epochs, &results[k]);
        }

        print_engine(eng, results, count, as_json);
        if (as_json && e + 1 < engine_count)
        {
            printf(",");
        }
        if (as_json)
        {
            printf("\n");
        }

        eng->teardown(&ctx);
    }

    if (as_json)
    {
        printf("  ]\n");
        printf("}\n");
    }
    return 0;
}
