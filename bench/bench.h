/*
 * areole benchmark harness.
 * SPDX-License-Identifier: MIT
 *
 * Headless: every scene renders into a memory surface, so this runs in CI with
 * no display and under an emulator with no graphics driver.
 *
 * Rates, not totals. A total is true for one machine; nanoseconds per pixel,
 * per glyph and per node survive a change of machine and can be multiplied by
 * a different one's profile. That is what makes a number from a Ryzen say
 * something about a Pentium II.
 */
#ifndef BENCH_H
#define BENCH_H

#include "areole.h"

#include <stddef.h> /* size_t, for the guarded allocator */

#define BENCH_MAX_SCENES  64
#define BENCH_MAX_SAMPLES 20000

/* Everything a scene is given. The surface is allocated by the runner to the
   size the scene asked for, and the context is created fresh before every
   scene so one cannot leave a stylesheet behind for the next. */
typedef struct bench_env
{
    ar_surface surface;
    ar_ctx    *ui;
    ar_u32     frame; /* index within the current run, for scenes that vary */

    /* Set by --full-repaint. areole normally paints only what changed, so a
       steady frame and a worst frame differ by more than an order of
       magnitude. Both are worth measuring: the steady frame is what the
       machine pays, the worst frame is what the budget must cover. */
    int full_repaint;
} bench_env;

typedef struct bench_scene
{
    const char *name;     /* "fill_opaque" */
    const char *group;    /* "primitive"   */
    const char *stresses; /* what a regression here means, in one line */

    /* 0 for the default 800x600. clear_uncached asks for something far larger
       than any cache, which is the only condition under which a software
       rasterizer shows its real memory behaviour. */
    ar_i32 want_w, want_h;

    int needs_ui; /* create an ar_ctx for this scene */

    void (*init)(bench_env *e);  /* stylesheet, scene state; not measured */
    void (*frame)(bench_env *e); /* one frame; this is what is timed */
} bench_scene;

void               bench_register(const bench_scene *s);
int                bench_scene_count(void);
const bench_scene *bench_scene_at(int i);

/* One registration entry point per scene file, called from main. Keeps the
   scene list explicit rather than depending on link order or constructors,
   neither of which C89 has. */
void bench_register_primitive(void);
void bench_register_text(void);
void bench_register_layout(void);
void bench_register_style(void);
void bench_register_realistic(void);
void bench_register_patho(void);

/* Registers nothing unless bench_font_load succeeded: these scenes need a real
   font file, which is the one external input the benchmark has. */
void bench_register_outline(void);
int  bench_font_load(const char *path);

/* ------------------------------------------------------------------------
 * Clock
 *
 * The benchmark owns its own, rather than borrowing the Win32 backend's,
 * because linking that would drag a window into a headless tool and would not
 * build on Linux at all.
 * ------------------------------------------------------------------------ */
void        bench_clock_init(void);
double      bench_now_s(void);
ar_u32      bench_time_us(void); /* handed to ar_set_clock */
const char *bench_clock_name(void);

/* The smallest interval this clock can distinguish, measured rather than
   assumed. A scene whose frame time is a few ticks is measuring the clock. */
double bench_clock_resolution_s(void);

/* Yields the processor. Used to settle between scenes: a scene that has just
   written nineteen gigabytes leaves the memory subsystem and the power state
   disturbed, and the next scene measures the recovery rather than itself.
   That is not hypothetical -- it produced a 6x outlier in the first full run
   of this tool. */
void bench_sleep_ms(int ms);

/* ------------------------------------------------------------------------
 * Statistics
 * ------------------------------------------------------------------------ */
void   bench_sort(double *v, int n);
double bench_percentile(const double *sorted, int n, double pct);

/* ------------------------------------------------------------------------
 * Guards
 *
 * Both of these exist because the throwaway harness that produced the first
 * baseline fell into both traps within an hour. A benchmark that silently
 * lies is worse than no benchmark, so the tool checks itself.
 * ------------------------------------------------------------------------ */

/* Non-zero if the implied rate is physically impossible, which in practice
   means the optimiser deleted the work. A single-threaded scalar loop does not
   reach a terabyte per second on any machine that exists. */
int bench_rate_is_plausible(double bytes, double seconds);

/* A buffer with a known pattern written past both ends. bench_guard_intact
   returns zero if anything wrote outside the requested region, which is how an
   undersized allocation is caught at the moment it happens rather than as a
   segfault somewhere else. */
#define BENCH_GUARD_BYTES 64

void *bench_guarded_alloc(size_t bytes);
int   bench_guard_intact(void *p, size_t bytes);
void  bench_guarded_free(void *p);

/* ------------------------------------------------------------------------
 * Reading the tool's own JSON
 *
 * A scanner, not a parser, and it says so in bench_json.c. It reads exactly
 * the shape this tool writes and nothing else.
 * ------------------------------------------------------------------------ */
char       *bench_read_file(const char *path, long *len_out);
const char *bench_json_object_named(const char *text, const char *name);
int         bench_json_number(const char *obj, const char *key, double *out);
int         bench_json_string(const char *obj, const char *key, char *buf, size_t n);

/* ------------------------------------------------------------------------
 * Shared scene helpers
 * ------------------------------------------------------------------------ */

/* A deterministic pseudo-random sequence, so a scene that scatters rectangles
   scatters them the same way on every machine and every run. rand() is not
   portable enough for a number that gets published. */
ar_u32 bench_rand(void);
void   bench_srand(ar_u32 seed);

#endif /* BENCH_H */
