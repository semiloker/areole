/*
 * areole benchmark - registry, statistics, deterministic randomness.
 * SPDX-License-Identifier: MIT
 *
 * Strict C89, no platform header.
 */
#include "bench.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * Registry
 * ------------------------------------------------------------------------ */
static const bench_scene *g_scenes[BENCH_MAX_SCENES];
static int                g_scene_count = 0;

void bench_register(const bench_scene *s)
{
    if (g_scene_count < BENCH_MAX_SCENES)
    {
        g_scenes[g_scene_count++] = s;
    }
}

int bench_scene_count(void)
{
    return g_scene_count;
}

const bench_scene *bench_scene_at(int i)
{
    if (i < 0 || i >= g_scene_count)
    {
        return 0;
    }
    return g_scenes[i];
}

/* ------------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------------ */

/* ponytail: insertion sort over at most BENCH_MAX_SAMPLES values, run once per
   scene after the timed region. It is the wrong algorithm at a million
   samples; the sample count is twenty thousand and this costs less than one
   frame of the thing it is sorting. */
void bench_sort(double *v, int n)
{
    int i, j;

    for (i = 1; i < n; ++i)
    {
        double key = v[i];
        j = i - 1;
        while (j >= 0 && v[j] > key)
        {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

/* Nearest rank, the same definition the library's own ar_perf uses and pinned
   by the same reasoning: publishing p99 means nothing if the definition
   quietly differs between the library and the tool measuring it. */
double bench_percentile(const double *sorted, int n, double pct)
{
    double idx;
    int    i;

    if (n <= 0)
    {
        return 0.0;
    }
    if (pct < 0.0)
    {
        pct = 0.0;
    }
    if (pct > 100.0)
    {
        pct = 100.0;
    }

    idx = (pct / 100.0) * (double)(n - 1) + 0.5;
    i = (int)idx;
    if (i < 0)
    {
        i = 0;
    }
    if (i >= n)
    {
        i = n - 1;
    }
    return sorted[i];
}

/* ------------------------------------------------------------------------
 * Deterministic randomness
 *
 * A scene that scatters rectangles must scatter them identically on every
 * machine and every run, or the comparison between two runs is measuring the
 * scatter. rand() is not specified well enough for a number that gets
 * published, so this is a plain xorshift with a fixed seed.
 * ------------------------------------------------------------------------ */
static ar_u32 g_rand_state = 0x2545F491u;

void bench_srand(ar_u32 seed)
{
    g_rand_state = seed ? seed : 0x2545F491u;
}

ar_u32 bench_rand(void)
{
    ar_u32 x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

/* ------------------------------------------------------------------------
 * Guards
 * ------------------------------------------------------------------------ */

/* One terabyte per second. Well above anything a scalar loop on a single core
   reaches, and well below the infinity that a deleted loop reports. */
#define BENCH_MAX_PLAUSIBLE_BPS 1.0e12

int bench_rate_is_plausible(double bytes, double seconds)
{
    if (seconds <= 0.0)
    {
        return 0; /* zero elapsed time for real work never happens */
    }
    return (bytes / seconds) < BENCH_MAX_PLAUSIBLE_BPS;
}

#define BENCH_GUARD_BYTE 0xA5u

void *bench_guarded_alloc(size_t bytes)
{
    unsigned char *raw = (unsigned char *)malloc(bytes + 2u * BENCH_GUARD_BYTES);

    if (!raw)
    {
        return 0;
    }
    memset(raw, BENCH_GUARD_BYTE, BENCH_GUARD_BYTES);
    memset(raw + BENCH_GUARD_BYTES, 0, bytes);
    memset(raw + BENCH_GUARD_BYTES + bytes, BENCH_GUARD_BYTE, BENCH_GUARD_BYTES);
    return raw + BENCH_GUARD_BYTES;
}

int bench_guard_intact(void *p, size_t bytes)
{
    unsigned char *raw = (unsigned char *)p - BENCH_GUARD_BYTES;
    size_t         i;

    for (i = 0; i < BENCH_GUARD_BYTES; ++i)
    {
        if (raw[i] != BENCH_GUARD_BYTE)
        {
            return 0;
        }
        if (raw[BENCH_GUARD_BYTES + bytes + i] != BENCH_GUARD_BYTE)
        {
            return 0;
        }
    }
    return 1;
}

void bench_guarded_free(void *p)
{
    if (p)
    {
        free((unsigned char *)p - BENCH_GUARD_BYTES);
    }
}
