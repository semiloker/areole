/*
 * areole benchmark runner.
 * SPDX-License-Identifier: MIT
 *
 *   ar_bench --list
 *   ar_bench --all --iters 200 --repeat 3
 *   ar_bench --scene dashboard --json > results.json
 *
 * Headless, so it runs in CI with no display and under an emulator with no
 * graphics driver.
 *
 * Three things this tool does that the throwaway harness it replaces did not,
 * each because that harness got it wrong first:
 *
 *   - Every buffer is sized for the scene that needs the most. Sizing for the
 *     typical case is how a 64 MB uncached fill segfaulted.
 *   - Every rate is checked for plausibility before it is printed. An
 *     impossible number means the optimiser deleted the work.
 *   - Every scene runs several complete times and the disagreement between
 *     those runs is reported. A machine that disagrees with itself by ten per
 *     cent cannot support a three per cent regression gate, and the tool says
 *     so rather than letting somebody gate on noise.
 *
 * bench_selftest.c tests all three guards.
 */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough for the largest tree any scene builds: table_1k_rows peaks near 6000
   boxes and arena_churn near 4000. */
#define BENCH_UI_BOXES 16384

#define DEFAULT_W   800
#define DEFAULT_H   600
#define MAX_REPEATS 16

/* The regression gate wants three per cent at p50. Whether a scene can support
   that is not a matter of opinion: it is whether the scene's own repeated runs
   agree that closely on the machine in question.

   On the reference laptop most scenes do not, because boost clocks and thermal
   state move between runs. So the gate is not a global constant. Each scene
   carries its measured stability into the baseline, and the gate threshold for
   that scene becomes max(3%, 2x its measured spread). A flat three per cent
   would either fail constantly or have to be raised until it caught nothing. */
#define STABILITY_LIMIT_PCT 3.0

typedef struct bench_result
{
    const bench_scene *scene;

    int    iters;
    int    repeats;
    double p50, p95, p99, p999, min, max, mean;
    double stability_pct;

    /* Phase medians from the library's own counters. Zero without a context. */
    double style_us, layout_us, raster_us;

    /* Per frame, from ar_counters. */
    double fills, fill_px, blend_px, glyphs, glyph_px, clipped_out;

    /* Normalised rates, emitted only when the denominator is large enough for
       the answer to mean something. */
    double ns_per_px, ns_per_glyph, ns_per_node;
    double overdraw;
    int    below_timer;

    ar_u32 nodes;
    ar_u32 mem_persist, mem_frame_peak;
    ar_u32 sheet_errors;
    int    alloc_violation;
    int    overflowed;
} bench_result;

static double g_samples[BENCH_MAX_SAMPLES];
static double g_one[BENCH_MAX_SAMPLES];

static void die(const char *msg)
{
    fprintf(stderr, "ar_bench: %s\n", msg);
    exit(1);
}

/* ------------------------------------------------------------------------ */
static void run_scene(const bench_scene *sc, int iters, int warmup, int repeats, bench_result *out)
{
    bench_env      e;
    unsigned char *ui_mem = 0;
    ar_u32        *pixels;
    ar_i32         w = sc->want_w ? sc->want_w : DEFAULT_W;
    ar_i32         h = sc->want_h ? sc->want_h : DEFAULT_H;
    ar_u32         persist_before = 0, persist_after = 0;
    double         rep_p50[MAX_REPEATS];
    double         t0, t1, sum = 0.0;
    int            i, rep, total;

    memset(out, 0, sizeof *out);
    out->scene = sc;
    out->iters = iters;
    out->repeats = repeats;

    /* Sized for this scene rather than for the typical one. clear_uncached
       asks for 4096 by 4096, which is 64 MB. */
    pixels = (ar_u32 *)malloc((size_t)w * (size_t)h * sizeof(ar_u32));
    if (!pixels)
    {
        die("out of memory for the scene surface");
    }
    memset(pixels, 0, (size_t)w * (size_t)h * sizeof(ar_u32));

    e.surface.pixels = pixels;
    e.surface.w = w;
    e.surface.h = h;
    e.surface.stride = w;
    e.ui = 0;
    e.frame = 0;

    if (sc->needs_ui)
    {
        ui_mem = (unsigned char *)malloc(AR_MEM(BENCH_UI_BOXES));
        if (!ui_mem)
        {
            die("out of memory for the areole context");
        }
        e.ui = ar_init(ui_mem, AR_MEM(BENCH_UI_BOXES));
        if (!e.ui)
        {
            die("ar_init refused the block");
        }
        ar_set_clock(e.ui, bench_time_us);
    }

    if (sc->init)
    {
        sc->init(&e);
    }
    if (e.ui)
    {
        out->sheet_errors = ar_stylesheet_errors(e.ui);
    }

    for (i = 0; i < warmup; ++i)
    {
        e.frame = (ar_u32)i;
        sc->frame(&e);
    }

    /* Counters and the arena watermark are read around the timed region only,
       so the warmup does not contaminate either. */
    ar_counters_reset();
    if (e.ui)
    {
        ar_memory_stats(e.ui, &persist_before, 0, 0, 0);
    }

    /* Each repeat is a complete timed run, with a pause between them so a
       scene that has just written gigabytes is not measured while the memory
       subsystem recovers. */
    for (rep = 0; rep < repeats; ++rep)
    {
        if (rep > 0)
        {
            bench_sleep_ms(20);
        }
        for (i = 0; i < iters; ++i)
        {
            e.frame = (ar_u32)(warmup + rep * iters + i);
            t0 = bench_now_s();
            sc->frame(&e);
            t1 = bench_now_s();
            g_samples[rep * iters + i] = t1 - t0;
            sum += g_samples[rep * iters + i];
        }
        memcpy(g_one, &g_samples[rep * iters], (size_t)iters * sizeof(double));
        bench_sort(g_one, iters);
        rep_p50[rep] = bench_percentile(g_one, iters, 50.0);
    }

    if (e.ui)
    {
        ar_perf *pf = ar_perf_of(e.ui);
        ar_memory_stats(e.ui, &persist_after, 0, &out->mem_frame_peak, 0);
        out->mem_persist = persist_after;
        out->nodes = pf->cur.nodes;
        out->style_us = (double)ar_perf_percentile(pf, AR_PHASE_STYLE, 50);
        out->layout_us = (double)ar_perf_percentile(pf, AR_PHASE_LAYOUT, 50);
        out->raster_us = (double)ar_perf_percentile(pf, AR_PHASE_RASTER, 50);
        out->overflowed = ar_overflowed(e.ui);

        /* The invariant, checked rather than assumed: the persistent half of
           the arena must not move once init is over. If it does, something
           allocated during a frame and p99 will stop equalling p50. */
        out->alloc_violation = (persist_before != persist_after);
    }

    total = iters * repeats;

    {
        ar_counters *c = ar_counters_get();
        double       n = (double)total;
        out->fills = (double)c->fills / n;
        out->fill_px = (double)c->fill_px / n;
        out->blend_px = (double)c->blend_px / n;
        out->glyphs = (double)c->glyphs / n;
        out->glyph_px = (double)c->glyph_px / n;
        out->clipped_out = (double)c->clipped_out / n;
    }

    bench_sort(g_samples, total);
    out->min = g_samples[0];
    out->max = g_samples[total - 1];
    out->mean = sum / (double)total;
    out->p50 = bench_percentile(g_samples, total, 50.0);
    out->p95 = bench_percentile(g_samples, total, 95.0);
    out->p99 = bench_percentile(g_samples, total, 99.0);
    out->p999 = bench_percentile(g_samples, total, 99.9);

    {
        double mid;
        bench_sort(rep_p50, repeats);
        mid = bench_percentile(rep_p50, repeats, 50.0);
        out->stability_pct = mid > 0.0 ? (rep_p50[repeats - 1] - rep_p50[0]) * 100.0 / mid : 0.0;
    }

    {
        /* Rectangle pixels and glyph pixels are deliberately not summed. A
           glyph costs per bit tested, not per pixel written -- the baseline
           proved it by drawing four times the pixels at scale 2 for the same
           money -- so dividing a text scene by its ink pixels produces a
           confident number that means nothing. Rectangles get nanoseconds per
           pixel; glyphs get nanoseconds per glyph. */
        double px = out->fill_px + out->blend_px;
        double surface_px = (double)w * (double)h;

        out->ns_per_px = px >= 1000.0 ? out->p50 * 1e9 / px : 0.0;
        out->ns_per_glyph = out->glyphs >= 50.0 ? out->p50 * 1e9 / out->glyphs : 0.0;
        out->ns_per_node = out->nodes >= 20 ? out->p50 * 1e9 / (double)out->nodes : 0.0;
        out->overdraw = surface_px > 0.0 ? (px + out->glyph_px) / surface_px : 0.0;
        out->below_timer = (out->p50 < bench_clock_resolution_s() * 20.0);
    }

    free(pixels);
    if (ui_mem)
    {
        free(ui_mem);
    }
}

/* ------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------ */
static void print_header(void)
{
    printf("%-20s %9s %9s %9s %7s  %8s %8s %8s\n", "scene", "p50", "p95", "p99", "spread", "ns/px",
           "ns/glyph", "ns/node");
    printf("%-20s %9s %9s %9s %7s  %8s %8s %8s\n", "--------------------", "---------", "---------",
           "---------", "-------", "--------", "--------", "--------");
}

static void print_rate(double v, const char *fmt)
{
    if (v > 0.0)
    {
        printf(fmt, v);
    }
    else
    {
        printf("%8s ", "-");
    }
}

static void print_row(const bench_result *r)
{
    printf("%-20s %8.1fu %8.1fu %8.1fu %6.1f%%  ", r->scene->name, r->p50 * 1e6, r->p95 * 1e6,
           r->p99 * 1e6, r->stability_pct);
    print_rate(r->ns_per_px, "%8.3f ");
    print_rate(r->ns_per_glyph, "%8.1f ");
    print_rate(r->ns_per_node, "%8.1f ");
    printf("\n");

    if (r->alloc_violation)
    {
        printf("  ** ALLOCATION AFTER INIT: the persistent arena moved during the run\n");
    }
    if (r->overflowed)
    {
        printf("  ** TREE OVERFLOWED: the box budget was exceeded; the result is not valid\n");
    }
    if (r->sheet_errors)
    {
        printf("  ** STYLESHEET: %lu problem(s); rules may have been truncated\n",
               (unsigned long)r->sheet_errors);
    }
    if (r->stability_pct > STABILITY_LIMIT_PCT)
    {
        printf("  ** UNSTABLE: %d runs disagree by %.1f%% at p50, more than the %.0f%% gate;\n",
               r->repeats, r->stability_pct, STABILITY_LIMIT_PCT);
        printf("     this figure cannot support a regression gate\n");
    }
    if (r->below_timer)
    {
        printf("  ** CLOCK FLOOR: p50 is within 20 ticks of the %.0f ns resolution,\n",
               bench_clock_resolution_s() * 1e9);
        printf("     so this measures the clock as much as the code\n");
    }
}

static void json_string(const char *s)
{
    putchar('"');
    while (*s)
    {
        if (*s == '"' || *s == '\\')
        {
            putchar('\\');
        }
        putchar(*s);
        s++;
    }
    putchar('"');
}

static void print_json(const bench_result *rs, int n)
{
    int i;

    printf("{\n");
    printf("  \"tool\": \"ar_bench\",\n");
    printf("  \"areole_version\": \"%s\",\n", ar_version());
    printf("  \"clock\": \"%s\",\n", bench_clock_name());
    printf("  \"clock_resolution_ns\": %.1f,\n", bench_clock_resolution_s() * 1e9);
    printf("  \"instrumented\": %s,\n", ar_counters_enabled() ? "true" : "false");
    printf("  \"scenes\": [\n");

    for (i = 0; i < n; ++i)
    {
        const bench_result *r = &rs[i];
        printf("    {\n");
        printf("      \"name\": ");
        json_string(r->scene->name);
        printf(",\n      \"group\": ");
        json_string(r->scene->group);
        printf(",\n      \"stresses\": ");
        json_string(r->scene->stresses);
        printf(",\n");
        printf("      \"iters\": %d,\n", r->iters);
        printf("      \"repeats\": %d,\n", r->repeats);
        printf("      \"stability_pct\": %.2f,\n", r->stability_pct);
        printf("      \"p50_us\": %.3f,\n", r->p50 * 1e6);
        printf("      \"p95_us\": %.3f,\n", r->p95 * 1e6);
        printf("      \"p99_us\": %.3f,\n", r->p99 * 1e6);
        printf("      \"p999_us\": %.3f,\n", r->p999 * 1e6);
        printf("      \"min_us\": %.3f,\n", r->min * 1e6);
        printf("      \"max_us\": %.3f,\n", r->max * 1e6);
        printf("      \"style_us\": %.1f,\n", r->style_us);
        printf("      \"layout_us\": %.1f,\n", r->layout_us);
        printf("      \"raster_us\": %.1f,\n", r->raster_us);
        printf("      \"fills\": %.1f,\n", r->fills);
        printf("      \"fill_px\": %.0f,\n", r->fill_px);
        printf("      \"blend_px\": %.0f,\n", r->blend_px);
        printf("      \"glyphs\": %.1f,\n", r->glyphs);
        printf("      \"glyph_px\": %.0f,\n", r->glyph_px);
        printf("      \"clipped_out\": %.1f,\n", r->clipped_out);
        printf("      \"nodes\": %lu,\n", (unsigned long)r->nodes);
        printf("      \"ns_per_px\": %.4f,\n", r->ns_per_px);
        printf("      \"ns_per_glyph\": %.2f,\n", r->ns_per_glyph);
        printf("      \"ns_per_node\": %.2f,\n", r->ns_per_node);
        printf("      \"overdraw\": %.3f,\n", r->overdraw);
        printf("      \"mem_persist\": %lu,\n", (unsigned long)r->mem_persist);
        printf("      \"mem_frame_peak\": %lu,\n", (unsigned long)r->mem_frame_peak);
        printf("      \"allocations_after_init\": %d,\n", r->alloc_violation);
        printf("      \"overflowed\": %d,\n", r->overflowed);
        printf("      \"below_timer_floor\": %d,\n", r->below_timer);
        printf("      \"stylesheet_errors\": %lu\n", (unsigned long)r->sheet_errors);
        printf("    }%s\n", i + 1 < n ? "," : "");
    }

    printf("  ]\n");
    printf("}\n");
}

/* ------------------------------------------------------------------------ */
static void usage(void)
{
    printf("ar_bench - areole benchmark runner\n\n");
    printf("  --list             list the scenes and what each stresses\n");
    printf("  --all              run every scene\n");
    printf("  --scene NAME       run one scene\n");
    printf("  --group NAME       run one group\n");
    printf("  --iters N          timed frames per repeat (default 200)\n");
    printf("  --repeat N         complete timed runs per scene (default 3)\n");
    printf("  --warmup N         untimed frames before timing (default 30)\n");
    printf("  --json             emit JSON instead of a table\n\n");
    printf("The spread column is the disagreement between the repeats at p50.\n");
    printf("Anything above %.0f%% cannot support the regression gate and is\n",
           STABILITY_LIMIT_PCT);
    printf("flagged rather than quietly published.\n\n");
    printf("A build without -DAR_INSTRUMENT carries no pixel or glyph counts, so\n");
    printf("its normalised rates are absent rather than wrong.\n");
}

int main(int argc, char **argv)
{
    static bench_result results[BENCH_MAX_SCENES];
    const char         *want_scene = 0;
    const char         *want_group = 0;
    int                 iters = 200, warmup = 30, repeats = 3;
    int                 as_json = 0, list = 0, all = 0;
    int                 i, n = 0;

    bench_clock_init();

    bench_register_primitive();
    bench_register_text();
    bench_register_layout();
    bench_register_style();
    bench_register_realistic();
    bench_register_patho();

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--list") == 0)
        {
            list = 1;
        }
        else if (strcmp(argv[i], "--all") == 0)
        {
            all = 1;
        }
        else if (strcmp(argv[i], "--json") == 0)
        {
            as_json = 1;
        }
        else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
        {
            want_scene = argv[++i];
        }
        else if (strcmp(argv[i], "--group") == 0 && i + 1 < argc)
        {
            want_group = argv[++i];
        }
        else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
        {
            iters = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
        {
            repeats = atoi(argv[++i]);
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

    if (iters < 1)
    {
        iters = 1;
    }
    if (repeats < 1)
    {
        repeats = 1;
    }
    if (repeats > MAX_REPEATS)
    {
        repeats = MAX_REPEATS;
    }
    if (iters * repeats > BENCH_MAX_SAMPLES)
    {
        iters = BENCH_MAX_SAMPLES / repeats;
    }
    if (warmup < 0)
    {
        warmup = 0;
    }

    if (list)
    {
        for (i = 0; i < bench_scene_count(); ++i)
        {
            const bench_scene *sc = bench_scene_at(i);
            printf("%-20s %-14s %s\n", sc->name, sc->group, sc->stresses);
        }
        return 0;
    }

    if (!all && !want_scene && !want_group)
    {
        usage();
        return 1;
    }

    if (!as_json)
    {
        printf("areole %s   clock %s   instrumented %s\n", ar_version(), bench_clock_name(),
               ar_counters_enabled() ? "yes" : "NO (rates unavailable)");
        printf("%d iterations x %d repeats per scene\n\n", iters, repeats);
        print_header();
    }

    for (i = 0; i < bench_scene_count(); ++i)
    {
        const bench_scene *sc = bench_scene_at(i);

        if (want_scene && strcmp(sc->name, want_scene) != 0)
        {
            continue;
        }
        if (want_group && strcmp(sc->group, want_group) != 0)
        {
            continue;
        }

        /* Settle before each scene. clear_uncached writes 64 MB a frame, and
           whatever follows it would otherwise measure the memory subsystem
           recovering. That produced a sixfold outlier in the first full run of
           this tool, which is how the pause got here. */
        if (n > 0)
        {
            bench_sleep_ms(400);
        }

        run_scene(sc, iters, warmup, repeats, &results[n]);
        if (!as_json)
        {
            print_row(&results[n]);
        }
        n++;
    }

    if (n == 0)
    {
        fprintf(stderr, "ar_bench: no scene matched\n");
        return 1;
    }
    if (as_json)
    {
        print_json(results, n);
    }
    return 0;
}
