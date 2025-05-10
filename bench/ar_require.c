/*
 * areole requirements calculator.
 * SPDX-License-Identifier: MIT
 *
 *   ar_require --scene dashboard --fps 60 --res 800x600 --bpp 32 \
 *              --results bench/baseline.json \
 *              --reference bench/profiles/reference-ryzen-8840hs.json \
 *              --target bench/profiles/pentium2-400.json
 *
 * Answers the question the whole toolchain exists for: what machine does this
 * interface actually need.
 *
 * The model has two components and no more, because a software rasterizer has
 * two bottlenecks and no more.
 *
 *   memory   proportional to bytes touched, bounded by write bandwidth
 *   compute  everything else, bounded by how fast the machine retires work
 *
 * The reference time is split between them using the reference machine's
 * measured bandwidth, then each half is scaled to the target by the ratio that
 * governs it. That is the whole model. It is stated here so it can be argued
 * with rather than trusted.
 *
 * Compute is reported as a RANGE, not a number. A dependent chain scales
 * roughly with clock; independent work scales with clock times issue width.
 * Real code is somewhere between, and pretending to know where would be the
 * kind of false precision this project exists to avoid.
 */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct profile
{
    char   name[128];
    char   kind[32]; /* "measured" or "derived" */
    double dep_ops;
    double indep_ops;
    double write_gbs;
    int    ok;
} profile;

static profile load_profile(const char *path)
{
    profile pr;
    char   *text;

    memset(&pr, 0, sizeof pr);
    text = bench_read_file(path, 0);
    if (!text)
    {
        fprintf(stderr, "ar_require: cannot read profile %s\n", path);
        return pr;
    }

    bench_json_string(text, "name", pr.name, sizeof pr.name);
    bench_json_string(text, "profile", pr.kind, sizeof pr.kind);
    if (!pr.name[0])
    {
        strcpy(pr.name, "(unnamed)");
    }

    pr.ok = bench_json_number(text, "scalar_dependent_ops_per_s", &pr.dep_ops) &&
            bench_json_number(text, "scalar_independent_ops_per_s", &pr.indep_ops) &&
            bench_json_number(text, "mem_write_gbs_scalar", &pr.write_gbs);

    if (!pr.ok)
    {
        fprintf(stderr, "ar_require: %s is missing a required field\n", path);
    }
    free(text);
    return pr;
}

static void usage(void)
{
    printf("ar_require - what machine does this interface need\n\n");
    printf("  --scene NAME       scene from the results file (required)\n");
    printf("  --results FILE     ar_bench --json output (required)\n");
    printf("  --reference FILE   profile of the machine the results came from\n");
    printf("  --target FILE      profile of the machine being asked about\n");
    printf("  --fps N            target frame rate (default 60)\n");
    printf("  --res WxH          target resolution (default the scene's own)\n");
    printf("  --bpp N            target bits per pixel, 32 or 16 (default 32)\n");
}

int main(int argc, char **argv)
{
    const char *scene = 0;
    const char *results_path = 0;
    const char *ref_path = 0;
    const char *tgt_path = 0;
    double      fps = 60.0;
    int         want_w = 0, want_h = 0, bpp = 32;
    int         i;

    char       *results;
    const char *obj;
    profile     ref, tgt;

    double p50_us = 0, fill_px = 0, blend_px = 0, glyph_px = 0, nodes = 0;
    double scene_w = 0, scene_h = 0;
    double bytes_ref, mem_ref_s, compute_ref_s, total_ref_s, ref_fill_gbs;
    double area_scale, bytes_tgt, mem_tgt_s;
    double dep_ratio, indep_ratio;
    double compute_lo_s, compute_hi_s, total_lo_s, total_hi_s;
    double budget_s;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc)
        {
            scene = argv[++i];
        }
        else if (strcmp(argv[i], "--results") == 0 && i + 1 < argc)
        {
            results_path = argv[++i];
        }
        else if (strcmp(argv[i], "--reference") == 0 && i + 1 < argc)
        {
            ref_path = argv[++i];
        }
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
        {
            tgt_path = argv[++i];
        }
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)
        {
            fps = atof(argv[++i]);
        }
        else if (strcmp(argv[i], "--bpp") == 0 && i + 1 < argc)
        {
            bpp = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--res") == 0 && i + 1 < argc)
        {
            sscanf(argv[++i], "%dx%d", &want_w, &want_h);
        }
        else
        {
            usage();
            return 1;
        }
    }

    if (!scene || !results_path || !ref_path || !tgt_path)
    {
        usage();
        return 1;
    }
    if (fps <= 0.0)
    {
        fps = 60.0;
    }

    ref = load_profile(ref_path);
    tgt = load_profile(tgt_path);
    if (!ref.ok || !tgt.ok)
    {
        return 2;
    }

    results = bench_read_file(results_path, 0);
    if (!results)
    {
        fprintf(stderr, "ar_require: cannot read results %s\n", results_path);
        return 2;
    }
    obj = bench_json_object_named(results, scene);
    if (!obj)
    {
        fprintf(stderr, "ar_require: no scene named %s in %s\n", scene, results_path);
        free(results);
        return 2;
    }

    bench_json_number(obj, "p50_us", &p50_us);
    bench_json_number(obj, "fill_px", &fill_px);
    bench_json_number(obj, "blend_px", &blend_px);
    bench_json_number(obj, "glyph_px", &glyph_px);
    bench_json_number(obj, "nodes", &nodes);

    /* The scene's own resolution is not in the results, so the target
       resolution defaults to whatever was asked for and otherwise to 800x600,
       which is what most scenes use. Stated rather than guessed silently. */
    scene_w = 800.0;
    scene_h = 600.0;
    if (want_w <= 0 || want_h <= 0)
    {
        want_w = (int)scene_w;
        want_h = (int)scene_h;
    }

    /* A blended pixel is read as well as written, so it costs twice the
       traffic of an opaque one. Glyph ink is written once. */
    bytes_ref = (fill_px + blend_px * 2.0 + glyph_px) * 4.0;

    /* The reference machine's memory half is computed from its ACHIEVED fill
       rate, taken from the clear_cached scene, not from its main-memory
       bandwidth.
     *
     * Using main-memory bandwidth here produced a memory time larger than the
     * whole measured frame, and therefore a compute time of zero for a scene
     * that is mostly layout. The reason is that a 2.6 MB surface fits in this
     * machine's 16 MB of L3 and fills at about four times main-memory speed.
     *
     * The target is deliberately treated the other way round: a Pentium II has
     * 512 KB of L2 and can never hold a framebuffer, so its memory half is
     * computed from main memory and always will be. That asymmetry is not a
     * fudge, it is the single most important difference between the two
     * machines. */
    {
        const char *cached = bench_json_object_named(results, "clear_cached");
        double      ns_px = 0.0;

        if (cached && bench_json_number(cached, "ns_per_px", &ns_px) && ns_px > 0.0)
        {
            ref_fill_gbs = 4.0 / ns_px; /* 4 bytes per pixel, ns to GB/s */
        }
        else
        {
            ref_fill_gbs = ref.write_gbs;
            printf("note: clear_cached absent from the results, falling back to the\n");
            printf("      main-memory figure, which understates a cache-resident scene\n\n");
        }
    }
    mem_ref_s = bytes_ref / (ref_fill_gbs * 1e9);
    total_ref_s = p50_us * 1e-6;
    compute_ref_s = total_ref_s - mem_ref_s;
    if (compute_ref_s < 0.0)
    {
        /* The scene is entirely bandwidth bound and the split says so. This is
           not an error; it is what clear_uncached looks like. */
        compute_ref_s = 0.0;
    }

    area_scale = ((double)want_w * (double)want_h) / (scene_w * scene_h);
    bytes_tgt = bytes_ref * area_scale * ((double)bpp / 32.0);
    mem_tgt_s = bytes_tgt / (tgt.write_gbs * 1e9);

    dep_ratio = ref.dep_ops / tgt.dep_ops;
    indep_ratio = ref.indep_ops / tgt.indep_ops;

    compute_lo_s = compute_ref_s * (dep_ratio < indep_ratio ? dep_ratio : indep_ratio);
    compute_hi_s = compute_ref_s * (dep_ratio > indep_ratio ? dep_ratio : indep_ratio);

    total_lo_s = mem_tgt_s + compute_lo_s;
    total_hi_s = mem_tgt_s + compute_hi_s;
    budget_s = 1.0 / fps;

    printf("scene      %s\n", scene);
    printf("target     %dx%d at %d bpp, %.0f fps, budget %.2f ms\n\n", want_w, want_h, bpp, fps,
           budget_s * 1e3);

    printf("reference  %s\n", ref.name);
    printf("  measured %.3f ms per frame\n", total_ref_s * 1e3);
    printf("  of which %.3f ms memory (%.2f MB at %.2f GB/s achieved, cache resident)\n",
           mem_ref_s * 1e3, bytes_ref / 1048576.0, ref_fill_gbs);
    printf("  and      %.3f ms compute\n\n", compute_ref_s * 1e3);

    printf("target     %s\n", tgt.name);
    if (strcmp(tgt.kind, "derived") == 0)
    {
        printf("  ** this profile is DERIVED, not measured. Every figure below\n");
        printf("     inherits its assumptions; see the profile for each one.\n");
    }
    printf("  memory   %.3f ms (%.2f MB at %.2f GB/s)\n", mem_tgt_s * 1e3, bytes_tgt / 1048576.0,
           tgt.write_gbs);
    printf("  compute  %.3f to %.3f ms (scaling %.1fx to %.1fx)\n", compute_lo_s * 1e3,
           compute_hi_s * 1e3, dep_ratio, indep_ratio);
    printf("  total    %.3f to %.3f ms\n\n", total_lo_s * 1e3, total_hi_s * 1e3);

    if (total_hi_s <= budget_s)
    {
        printf("VERDICT: MEETS %.0f fps, with %.0f%% to %.0f%% headroom\n", fps,
               (1.0 - total_hi_s / budget_s) * 100.0, (1.0 - total_lo_s / budget_s) * 100.0);
    }
    else if (total_lo_s <= budget_s)
    {
        printf("VERDICT: MARGINAL. Meets %.0f fps at the optimistic end of the compute\n", fps);
        printf("         range and misses it at the pessimistic end. The range is this\n");
        printf("         wide because nobody has measured the target machine yet.\n");
    }
    else
    {
        printf("VERDICT: MISSES %.0f fps by %.1fx to %.1fx\n", fps, total_lo_s / budget_s,
               total_hi_s / budget_s);
        if (mem_tgt_s > budget_s)
        {
            printf("         Memory alone exceeds the budget. No amount of faster code\n");
            printf("         helps; the fix is touching fewer pixels or fewer bytes per\n");
            printf("         pixel. This is what damage tracking and the 16 bpp build are\n");
            printf("         for.\n");
        }
        else
        {
            printf("         Compute is the binding constraint, not bandwidth.\n");
        }
    }

    printf("\nminimum machine for %.0f fps on this scene:\n", fps);
    {
        double need_bw = bytes_tgt / budget_s / 1e9;
        printf("  write bandwidth at least %.2f GB/s if compute were free\n", need_bw);
        if (budget_s > mem_tgt_s)
        {
            double allowed_compute = budget_s - mem_tgt_s;
            double need_ops =
                compute_ref_s > 0.0 ? ref.indep_ops * compute_ref_s / allowed_compute : 0.0;
            printf("  scalar throughput at least %.3g ops/s given that bandwidth\n", need_ops);
        }
        else
        {
            printf("  no scalar throughput suffices: the memory traffic alone is over\n");
            printf("  budget, so the interface has to draw less rather than draw faster\n");
        }
    }

    printf("\nat 16 bpp the memory half halves, which on this target is %.3f ms\n",
           mem_tgt_s * 0.5 * 1e3);

    free(results);
    return 0;
}
