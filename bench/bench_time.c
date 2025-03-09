/*
 * areole benchmark - the clock.
 * SPDX-License-Identifier: MIT
 *
 * The only file in bench/ that includes a platform header, for the same reason
 * the library has that rule: everything else stays portable and provably so.
 *
 * Compiled as gnu89, not strict C89.
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#else
#include <time.h>
#endif

#include "bench.h"

#if defined(_WIN32)

static double        g_qpf = 0.0;
static LARGE_INTEGER g_origin;

void bench_clock_init(void)
{
    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f);
    g_qpf = (double)f.QuadPart;
    QueryPerformanceCounter(&g_origin);

    /* 1 ms scheduling for the session. Without it a Sleep or a scheduler slice
       lands wherever Windows feels like, and the p99 of a benchmark measures
       the scheduler rather than the code. */
    timeBeginPeriod(1);
}

double bench_now_s(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_origin.QuadPart) / g_qpf;
}

const char *bench_clock_name(void)
{
    return "QueryPerformanceCounter";
}

#else

static double g_origin = 0.0;

static double bench__raw(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void bench_clock_init(void)
{
    g_origin = bench__raw();
}

double bench_now_s(void)
{
    return bench__raw() - g_origin;
}

const char *bench_clock_name(void)
{
    return "clock_gettime(CLOCK_MONOTONIC)";
}

#endif

/* Measured by sampling until the value changes, repeated, taking the smallest
   non-zero delta. Asking the platform for its resolution reports what it
   promises; this reports what it does. */
double bench_clock_resolution_s(void)
{
    static double cached = 0.0;
    double        best = 1.0;
    int           i;

    if (cached > 0.0)
    {
        return cached;
    }
    for (i = 0; i < 64; ++i)
    {
        double a = bench_now_s();
        double b = a;
        while (b == a)
        {
            b = bench_now_s();
        }
        if (b - a < best)
        {
            best = b - a;
        }
    }
    cached = best;
    return cached;
}

void bench_sleep_ms(int ms)
{
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
#endif
}

ar_u32 bench_time_us(void)
{
    return (ar_u32)(bench_now_s() * 1e6);
}
