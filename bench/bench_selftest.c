/*
 * areole benchmark - the tool testing itself.
 * SPDX-License-Identifier: MIT
 *
 * A benchmark that lies is worse than no benchmark, because it gets believed
 * once and then quietly distrusted forever. The throwaway harness that
 * produced the first baseline fell into two traps within an hour:
 *
 *   - It reported INFINITE copy bandwidth, because at -O2 the compiler saw a
 *     store to a buffer that was never read and then freed, and deleted the
 *     memcpy outright.
 *   - It SEGFAULTED on the uncached fill, because the surface was sized for
 *     the largest window rather than for the benchmark that needed 64 MB.
 *
 * Both are now guarded, and this file tests the guards. Testing that a
 * particular compiler deletes a particular loop would be testing the compiler;
 * what matters is that the detector fires when it happens.
 */
#include "bench.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;
static int g_failed = 0;

static void check(int ok, const char *what, const char *file, int line)
{
    g_checks++;
    if (!ok)
    {
        g_failed++;
        printf("FAIL  %s\n      at %s:%d\n", what, file, line);
    }
}

#define CHECK(expr, what) check((expr) ? 1 : 0, what, __FILE__, __LINE__)

/* ------------------------------------------------------------------------
 * Trap one: a deleted loop
 * ------------------------------------------------------------------------ */
static void test_deleted_work_is_caught(void)
{
    /* What a deleted loop looks like from the outside: a large amount of work
       claimed to have happened in no measurable time. */
    CHECK(!bench_rate_is_plausible(192.0 * 1024.0 * 1024.0, 0.0),
          "guard: zero elapsed time for real work is rejected");
    CHECK(!bench_rate_is_plausible(192.0 * 1024.0 * 1024.0, 1e-7),
          "guard: 2 TB/s is rejected as physically impossible");

    /* And what a real measurement looks like, so the guard is not simply
       rejecting everything. */
    CHECK(bench_rate_is_plausible(192.0 * 1024.0 * 1024.0, 0.010),
          "guard: 20 GB/s is accepted as a real measurement");
    CHECK(bench_rate_is_plausible(1024.0, 0.001), "guard: a slow small transfer is accepted");
}

/* ------------------------------------------------------------------------
 * Trap two: a buffer sized for the wrong benchmark
 * ------------------------------------------------------------------------ */
static void test_undersized_buffer_is_caught(void)
{
    unsigned char *p = (unsigned char *)bench_guarded_alloc(256);

    CHECK(p != 0, "guard: a guarded allocation succeeds");
    if (!p)
    {
        return;
    }

    memset(p, 0x11, 256);
    CHECK(bench_guard_intact(p, 256), "guard: writing exactly the requested region is fine");

    /* Exactly the mistake that segfaulted the throwaway harness, in miniature:
       write past the end of a buffer that was sized for something smaller. */
    p[256] = 0x00;
    CHECK(!bench_guard_intact(p, 256), "guard: one byte past the end is detected");

    bench_guarded_free(p);

    p = (unsigned char *)bench_guarded_alloc(256);
    if (p)
    {
        p[-1] = 0x00;
        CHECK(!bench_guard_intact(p, 256), "guard: one byte before the start is detected");
        bench_guarded_free(p);
    }
}

/* ------------------------------------------------------------------------
 * Trap three: measuring the clock instead of the code
 * ------------------------------------------------------------------------ */
static void test_clock_resolution(void)
{
    double r = bench_clock_resolution_s();

    CHECK(r > 0.0, "clock: a resolution is measured");
    CHECK(r < 1e-3, "clock: resolution is better than a millisecond");
    /* Called twice, it must agree with itself: it is cached, and a probe that
       re-measured each time would report a different floor per scene. */
    CHECK(r == bench_clock_resolution_s(), "clock: the measured resolution is stable");
}

static void test_clock_is_monotonic(void)
{
    double a = bench_now_s();
    double b = bench_now_s();
    int    i;
    int    ok = 1;

    CHECK(b >= a, "clock: two consecutive readings do not go backwards");

    for (i = 0; i < 100000; ++i)
    {
        double c = bench_now_s();
        if (c < b)
        {
            ok = 0;
        }
        b = c;
    }
    CHECK(ok, "clock: a hundred thousand readings never go backwards");
}

/* ------------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------------ */
static void test_percentiles(void)
{
    double v[100];
    int    i;

    /* Values 0 to 99, fed in an order that is neither sorted nor reversed, so
       a percentile that quietly returned the first or last sample would show
       up here. */
    for (i = 0; i < 100; ++i)
    {
        v[i] = (double)((i * 37) % 100);
    }
    bench_sort(v, 100);

    CHECK(v[0] == 0.0 && v[99] == 99.0, "stats: the sort actually sorts");
    CHECK(bench_percentile(v, 100, 0.0) == 0.0, "stats: p0 is the minimum");
    CHECK(bench_percentile(v, 100, 50.0) == 50.0, "stats: p50 is the median");
    CHECK(bench_percentile(v, 100, 100.0) == 99.0, "stats: p100 is the maximum");

    /* Nearest rank, pinned deliberately and by the same reasoning as the
       library's own ar_perf: publishing p99 means nothing if the tool and the
       library disagree about what p99 is. */
    CHECK(bench_percentile(v, 100, 99.0) == 98.0,
          "stats: p99 of a hundred samples is the ninety ninth of them");

    CHECK(bench_percentile(v, 100, 500.0) == 99.0, "stats: an out of range percentile is clamped");
    CHECK(bench_percentile(v, 0, 50.0) == 0.0, "stats: a percentile of nothing is zero");

    /* Monotonic, which is the property a reader assumes without checking. */
    CHECK(bench_percentile(v, 100, 50.0) <= bench_percentile(v, 100, 95.0) &&
              bench_percentile(v, 100, 95.0) <= bench_percentile(v, 100, 99.0) &&
              bench_percentile(v, 100, 99.0) <= bench_percentile(v, 100, 100.0),
          "stats: percentiles rise monotonically");
}

static void test_determinism(void)
{
    ar_u32 first[8], second[8];
    int    i;
    int    same = 1;

    bench_srand(12345u);
    for (i = 0; i < 8; ++i)
    {
        first[i] = bench_rand();
    }
    bench_srand(12345u);
    for (i = 0; i < 8; ++i)
    {
        second[i] = bench_rand();
    }
    for (i = 0; i < 8; ++i)
    {
        if (first[i] != second[i])
        {
            same = 0;
        }
    }
    /* A scene that scatters rectangles must scatter them identically on every
       machine and every run, or the comparison between two runs is measuring
       the scatter. */
    CHECK(same, "rand: the same seed gives the same sequence");
    CHECK(first[0] != first[1], "rand: the sequence is not constant");
}

/* ------------------------------------------------------------------------
 * The instrumented build must actually count
 * ------------------------------------------------------------------------ */
static void test_counters(void)
{
    static ar_u32 px[64 * 64];
    ar_surface    s;
    ar_rect       clip;
    ar_counters  *c;

    s.pixels = px;
    s.w = 64;
    s.h = 64;
    s.stride = 64;
    clip = ar_rect_make(0, 0, 64, 64);

    CHECK(ar_counters_enabled(), "counters: the benchmark links an instrumented build");

    ar_counters_reset();
    c = ar_counters_get();
    CHECK(c->fills == 0 && c->fill_px == 0, "counters: reset clears everything");

    ar_fill_rect(&s, ar_rect_make(0, 0, 10, 10), clip, AR_HEX(0x336699));
    CHECK(c->fills == 1, "counters: an opaque fill is counted once");
    CHECK(c->fill_px == 100, "counters: pixels come from the clipped rectangle");
    CHECK(c->blend_px == 0, "counters: an opaque fill is not counted as a blend");

    ar_fill_rect(&s, ar_rect_make(0, 0, 10, 10), clip, AR_RGBA(0x33, 0x66, 0x99, 0x80));
    CHECK(c->blend_px == 100, "counters: a translucent fill goes to the blend total");

    /* Clipped entirely away: no pixels, and recorded as a rejection rather
       than not recorded at all. */
    ar_counters_reset();
    ar_fill_rect(&s, ar_rect_make(500, 500, 10, 10), clip, AR_HEX(0x336699));
    CHECK(c->fills == 0 && c->fill_px == 0, "counters: a fully clipped fill writes nothing");
    CHECK(c->clipped_out == 1, "counters: a fully clipped fill is counted as a rejection");

    /* The clipped rectangle, not the requested one. A counter that trusted the
       caller would inflate every scene that draws partly off screen. */
    ar_counters_reset();
    ar_fill_rect(&s, ar_rect_make(-10, -10, 20, 20), clip, AR_HEX(0x336699));
    CHECK(c->fill_px == 100, "counters: pixels are counted after clipping, not before");
}

int main(void)
{
    bench_clock_init();

    printf("ar_bench selftest\n\n");

    test_deleted_work_is_caught();
    test_undersized_buffer_is_caught();
    test_clock_resolution();
    test_clock_is_monotonic();
    test_percentiles();
    test_determinism();
    test_counters();

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
