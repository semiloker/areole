/*
 * areole benchmark - comparison harness.
 * SPDX-License-Identifier: MIT
 *
 * The same work, done by areole and by an alternative, measured the same way.
 *
 * Fairness is the whole difficulty here, and the rules are written down so the
 * result can be argued with:
 *
 *   1. The same output target. Both engines write into the same DIB section,
 *      so neither is helped or hurt by where the pixels live.
 *   2. The same operations, in the same order, at the same coordinates. The
 *      loops are written side by side in one file for exactly this reason.
 *   3. Each side uses its own idiomatic fast path, not a translation of the
 *      other's. GDI gets a cached brush and FillRect, because that is what a
 *      Win32 application would actually write.
 *   4. The same clock, the same epochs, the same percentiles.
 *   5. Where a rival cannot express a case, the harness SAYS SO rather than
 *      substituting something easier. An unfair comparison is worth less than
 *      a missing one.
 *
 * A case that cannot be made fair is not included.
 */
#ifndef COMPARE_H
#define COMPARE_H

#include "../bench.h"

#define CMP_MAX_CASES 32

typedef struct cmp_ctx
{
    ar_surface surface; /* the shared DIB pixels */
    void      *native;  /* engine-private handle: an HDC for GDI */
    ar_i32     w, h;
    ar_u32     frame;
} cmp_ctx;

typedef struct cmp_case
{
    const char *name;
    const char *what;   /* what is being compared, in one line */
    const char *caveat; /* non-null when the comparison is imperfect */
    void (*areole)(cmp_ctx *c);
    void (*rival)(cmp_ctx *c);

    /* Optional third figure, for cases where whole-frame totals compare
       different amounts of work. Clay has no style engine, so areole's total
       includes a phase Clay simply does not have; the layout phase alone is
       the honest head to head, and it is reported beside the totals rather
       than instead of them. Left null by cases that do not need it. */
    const char *extra_label;
    double (*extra_us)(void);
} cmp_case;

typedef struct cmp_engine
{
    const char *name;    /* "Win32 GDI" */
    const char *version; /* what was measured against */
    int (*setup)(cmp_ctx *c);
    void (*teardown)(cmp_ctx *c);
    const cmp_case *(*cases)(int *count);
} cmp_engine;

const cmp_engine *cmp_engine_gdi(void);
const cmp_engine *cmp_engine_clay(void);
const cmp_engine *cmp_engine_microui(void);

/* areole's layout phase alone, for the head to head against Clay. Clay has
   no style engine, so comparing whole frames would compare different work. */
double cmp_areole_layout_us(void);

#endif /* COMPARE_H */
