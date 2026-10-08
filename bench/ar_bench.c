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

    /* Resolved-style cache. A hit rate short of 1.0 means the interface has
       more distinct selector-and-state combinations than the table holds, and
       is paying a full rule scan per box for the surplus. */
    double cache_hit_rate;

    /* Damage, which decides whether the merged rectangle is enough or whether
       the hash grid has to be built. dirty_ratio is the mean fraction of the
       surface repainted; degenerate is the fraction of frames where merging
       distant changes produced something close to a full window, which is the
       exact failure the hash grid exists to fix. */
    double dirty_ratio, degenerate;

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

static int         g_full_repaint = 0;
static const char *g_font_path = 0;
static double      g_samples[BENCH_MAX_SAMPLES];

static void die(const char *msg)
{
    fprintf(stderr, "ar_bench: %s\n", msg);
    exit(1);
}

/* ------------------------------------------------------------------------ */
static void run_scene(const bench_scene *sc, int iters, int warmup, bench_result *out)
{
    bench_env      e;
    unsigned char *ui_mem = 0;
    ar_u32        *pixels;
    ar_i32         w = sc->want_w ? sc->want_w : DEFAULT_W;
    ar_i32         h = sc->want_h ? sc->want_h : DEFAULT_H;
    ar_u32         persist_before = 0, persist_after = 0;
    double         t0, t1, sum = 0.0;
    int            i, total;

    memset(out, 0, sizeof *out);
    out->scene = sc;
    out->iters = iters;

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
    e.full_repaint = g_full_repaint;

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

    {
        double surface_area = (double)e.surface.w * (double)e.surface.h;
        double dirty_sum = 0.0;
        int    degenerate_frames = 0;

        for (i = 0; i < iters; ++i)
        {
            e.frame = (ar_u32)(warmup + i);
            t0 = bench_now_s();
            sc->frame(&e);
            t1 = bench_now_s();
            g_samples[i] = t1 - t0;
            sum += g_samples[i];

            if (e.ui && surface_area > 0.0)
            {
                double d = (double)ar_perf_of(e.ui)->cur.dirty_px;
                dirty_sum += d;
                if (d >= surface_area * 0.9)
                {
                    ++degenerate_frames;
                }
            }
        }

        if (iters > 0 && surface_area > 0.0)
        {
            out->dirty_ratio = dirty_sum / ((double)iters * surface_area);
            out->degenerate = (double)degenerate_frames / (double)iters;
        }
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

        {
            ar_u32 hits = 0, misses = 0;
            ar_style_cache_stats(e.ui, &hits, &misses);
            if (hits + misses > 0)
            {
                out->cache_hit_rate = (double)hits / (double)(hits + misses);
            }
        }
    }

    total = iters;

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
 * Regression gate
 *
 * The threshold is not a global constant, and that is the point.
 *
 * A three per cent gate only means something if the machine agrees with
 * itself to better than three per cent, and on the reference laptop most
 * scenes do not: boost clocks and thermal state move between runs. Measured
 * spreads across the scene library range from 0.1% to 44%. A flat gate would
 * either fail constantly or have to be raised until it caught nothing real.
 *
 * So each scene carries its own measured spread into the baseline, and its
 * gate is twice that, floored at three per cent. A stable scene is held to a
 * tight bound; an inherently noisy one is held to a loose one and says so,
 * rather than being quietly excluded or quietly trusted.
 * ------------------------------------------------------------------------ */
/* Calibrated against the reference laptop by the only method that works:
   running the gate against unchanged code until it stopped firing, and asking
   why each failure happened rather than simply widening the bound.

   Two terms, because the noise has two sources.

   The relative term is twice the scene's own measured spread. That covers
   variance proportional to the work: cache state, boost clock, thermal.

   The absolute term covers variance that does not scale with the work at all:
   scheduler slices, interrupts, page faults. Eight microseconds of it swamps a
   55 microsecond scene and is invisible in a four millisecond one, which is
   exactly why deep_60 kept failing at eleven per cent while flat_8k passed. A
   purely proportional threshold cannot express that.

   These constants belong to this machine. A dedicated, quiet CI runner should
   measure its own and tighten them, and the right long-term answer is a
   quieter machine rather than a looser bound. */
#define GATE_FLOOR_PCT    4.0
#define GATE_ABS_NOISE_US 8.0

static double gate_threshold(double baseline_spread_pct, double baseline_p50_us)
{
    double relative = baseline_spread_pct * 2.0;
    double absolute = baseline_p50_us > 0.0 ? GATE_ABS_NOISE_US * 100.0 / baseline_p50_us : 0.0;

    if (relative < GATE_FLOOR_PCT)
    {
        relative = GATE_FLOOR_PCT;
    }
    return relative + absolute;
}

static int compare_against(const char *path, const bench_result *rs, int n)
{
    char *text = bench_read_file(path, 0);
    int   failures = 0;
    int   missing = 0;
    int   mismatched = 0;
    int   i;

    if (!text)
    {
        fprintf(stderr, "ar_bench: cannot read baseline %s\n", path);
        return 2;
    }

    printf("\n");
    printf("%-20s %10s %10s %8s %8s  %s\n", "scene", "baseline", "now", "delta", "gate", "verdict");
    printf("%-20s %10s %10s %8s %8s  %s\n", "--------------------", "----------", "----------",
           "--------", "--------", "-------");

    for (i = 0; i < n; ++i)
    {
        const char *obj = bench_json_object_named(text, rs[i].scene->name);
        double      base_p50 = 0.0, base_spread = 0.0;
        double      now_p50 = rs[i].p50 * 1e6;
        double      delta, thresh;

        if (!obj || !bench_json_number(obj, "p50_us", &base_p50))
        {
            printf("%-20s %10s %10.1f %8s %8s  new\n", rs[i].scene->name, "-", now_p50, "-", "-");
            missing++;
            continue;
        }
        if (!bench_json_number(obj, "stability_pct", &base_spread))
        {
            base_spread = 0.0;
        }

        /* Comparing a run against a baseline taken with different parameters
           is not a comparison. Fewer iterations means less warm state and more
           variance, and the difference shows up as a regression that is really
           a change of method. This was found by doing exactly that: a 60
           iteration run against a 200 iteration baseline reported three
           regressions in code that had not changed. */
        {
            double base_iters = 0.0, base_repeats = 0.0;
            bench_json_number(obj, "iters", &base_iters);
            bench_json_number(obj, "repeats", &base_repeats);
            if ((int)base_iters != rs[i].iters || (int)base_repeats != rs[i].repeats)
            {
                printf("%-20s %10.1f %10.1f %8s %8s  mismatched run: baseline %dx%d, now %dx%d\n",
                       rs[i].scene->name, base_p50, now_p50, "-", "-", (int)base_iters,
                       (int)base_repeats, rs[i].iters, rs[i].repeats);
                mismatched++;
                continue;
            }
        }

        delta = base_p50 > 0.0 ? (now_p50 - base_p50) * 100.0 / base_p50 : 0.0;
        thresh = gate_threshold(base_spread, base_p50);

        printf("%-20s %10.1f %10.1f %7.1f%% %7.1f%%  %s\n", rs[i].scene->name, base_p50, now_p50,
               delta, thresh, delta > thresh ? "REGRESSED" : "ok");
        if (delta > thresh)
        {
            failures++;
        }
    }

    printf("\n");
    if (missing)
    {
        printf("%d scene(s) absent from the baseline; reported, not gated\n", missing);
    }
    if (mismatched)
    {
        printf("%d scene(s) run with different parameters than the baseline.\n", mismatched);
        printf("  Rerun with the same --iters and --repeat, or the comparison is\n");
        printf("  measuring the method rather than the code.\n");
        failures++;
    }
    if (failures)
    {
        printf("%d scene(s) regressed beyond their own measured noise\n", failures);
    }
    else
    {
        printf("no scene regressed beyond its own measured noise\n");
    }

    free(text);
    return failures ? 1 : 0;
}

/* Whether this machine can support a regression gate at all.
 *
 * Acceptance criterion three of 0.1.1 says a benchmark noisier than the
 * gate cannot enforce the gate. That is a property of the machine, not of
 * the code, and the only useful thing to do about it is measure it and say
 * so. A tool that quietly gates on a machine with thirty per cent variance
 * produces failures nobody can act on, and people learn to ignore it.
 */
static void print_machine_verdict(const bench_result *rs, int n)
{
    double v[BENCH_MAX_SCENES];
    double median, worst = 0.0;
    int    usable = 0;
    int    i;

    for (i = 0; i < n; ++i)
    {
        v[i] = rs[i].stability_pct;
        if (v[i] > worst)
        {
            worst = v[i];
        }
        if (v[i] <= GATE_FLOOR_PCT)
        {
            usable++;
        }
    }
    bench_sort(v, n);
    median = bench_percentile(v, n, 50.0);

    printf("\nmachine: median spread %.1f%%, worst %.1f%%, %d of %d scenes within %.0f%%\n", median,
           worst, usable, n, GATE_FLOOR_PCT);

    if (median > GATE_FLOOR_PCT * 2.0)
    {
        printf("This machine cannot support a tight regression gate. The thresholds\n");
        printf("derived from these spreads will only catch gross changes, which is\n");
        printf("honest but weak. A quiet, unshared machine with boost disabled is\n");
        printf("what a real gate needs; nothing in the tool can substitute for it.\n");
    }
    else
    {
        printf("This machine can support a gate at roughly %.0f%% on most scenes.\n",
               GATE_FLOOR_PCT * 2.0);
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
    if (g_font_path)
    {
        printf("  \"font\": \"%s\",\n", g_font_path);
    }
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
        printf("      \"cache_hit_rate\": %.4f,\n", r->cache_hit_rate);
        printf("      \"dirty_ratio\": %.5f,\n", r->dirty_ratio);
        printf("      \"degenerate\": %.3f,\n", r->degenerate);
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
    printf("  --full-repaint     invalidate every frame: the worst frame,\n");
    printf("  --font PATH        add the outline text scenes, using this face\n");
    printf("                     not the steady one\n");
    printf("  --repeat N         epochs: complete passes over the scene list\n");
    printf("  --warmup N         untimed frames before timing (default 30)\n");
    printf("  --json             emit JSON instead of a table\n");
    printf("  --compare FILE     report every scene against a baseline JSON\n");
    printf("  --gate             exit non-zero if any scene regressed\n\n");
    printf("The spread column is the disagreement between epochs at p50, which is\n");
    printf("the variance a second invocation of this tool would actually see.\n");
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
    int                 as_json = 0, list = 0, all = 0, gate = 0;
    const char         *compare_path = 0;
    int                 i, n = 0, ep;
    static bench_result epochs[BENCH_MAX_SCENES][MAX_REPEATS];

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
        else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc)
        {
            g_font_path = argv[i + 1];
            if (!bench_font_load(argv[++i]))
            {
                die("cannot read that font file");
            }
        }
        else if (strcmp(argv[i], "--full-repaint") == 0)
        {
            g_full_repaint = 1;
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
        else if (strcmp(argv[i], "--compare") == 0 && i + 1 < argc)
        {
            compare_path = argv[++i];
        }
        else if (strcmp(argv[i], "--gate") == 0)
        {
            gate = 1;
        }
        else
        {
            usage();
            return 1;
        }
    }

    /* After parsing, because these scenes only exist if --font named a file
       the runner could read. */
    bench_register_outline();

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
    if (iters > BENCH_MAX_SAMPLES)
    {
        iters = BENCH_MAX_SAMPLES;
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
        printf("%d iterations per scene, %d epochs over the whole list\n\n", iters, repeats);
        print_header();
    }

    /* Epochs, not back-to-back repeats.
     *
     * The first version repeated each scene several times in a row and called
     * the spread between those repeats its stability. That number was far too
     * small: twenty milliseconds apart, with the cache warm and the clock
     * boosted, a scene agrees with itself far more closely than it agrees with
     * the same scene run a minute later. A gate built on it fired on code that
     * had not changed, twice.
     *
     * An epoch is a complete pass over every selected scene. Each scene is
     * therefore measured again only after the whole rest of the list has run,
     * which approximates two separate invocations, and the spread across
     * epochs is the variance a gate actually has to tolerate.
     */
    for (ep = 0; ep < repeats; ++ep)
    {
        int slot = 0;

        if (ep > 0)
        {
            bench_sleep_ms(500);
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

            /* Settle between scenes as well. clear_uncached writes 64 MB a
               frame, and whatever follows it would otherwise measure the
               memory subsystem recovering: that produced a sixfold outlier in
               the first full run of this tool. */
            if (slot > 0 || ep > 0)
            {
                bench_sleep_ms(200);
            }

            run_scene(sc, iters, warmup, &epochs[slot][ep]);
            slot++;
        }
        if (ep == 0)
        {
            n = slot;
        }
    }

    /* One whole epoch is chosen, not a blend of them.
     *
     * Taking p50 from one epoch and p95 from another produced a row where p95
     * was lower than p50, which is not a distribution. The median epoch by p50
     * is selected and every figure comes from it, so the row describes one
     * coherent measurement. The spread across the discarded epochs is what
     * becomes stability, which is the only thing they are needed for. */
    for (i = 0; i < n; ++i)
    {
        double v[MAX_REPEATS];
        double mid;
        int    k, pick = 0;

        for (k = 0; k < repeats; ++k)
        {
            v[k] = epochs[i][k].p50;
        }
        bench_sort(v, repeats);
        mid = bench_percentile(v, repeats, 50.0);

        for (k = 0; k < repeats; ++k)
        {
            if (epochs[i][k].p50 == mid)
            {
                pick = k;
                break;
            }
        }

        results[i] = epochs[i][pick];
        results[i].repeats = repeats;
        results[i].stability_pct = mid > 0.0 ? (v[repeats - 1] - v[0]) * 100.0 / mid : 0.0;

        if (!as_json)
        {
            print_row(&results[i]);
        }
    }

    if (n == 0)
    {
        fprintf(stderr, "ar_bench: no scene matched\n");
        return 1;
    }

    if (!as_json)
    {
        print_machine_verdict(results, n);
    }
    if (as_json)
    {
        print_json(results, n);
    }
    if (compare_path)
    {
        int rc = compare_against(compare_path, results, n);
        if (gate)
        {
            return rc;
        }
    }
    return 0;
}
