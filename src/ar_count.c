/*
 * areole - instrumentation counters.
 * SPDX-License-Identifier: MIT
 *
 * The storage exists in every build so callers need no conditional
 * compilation. What is conditional is whether anything ever writes to it: the
 * AR_COUNT macros in ar_internal.h expand to nothing unless AR_INSTRUMENT is
 * defined, so a shipping build carries one zeroed structure and no cost.
 *
 * A build that does not count reports zeroes, which says "this build did not
 * measure" rather than "nothing happened". ar_counters_enabled distinguishes
 * the two, and the benchmark refuses to publish a result from a build that
 * cannot count.
 */
#include "ar_internal.h"

#include <string.h>

/* ponytail: one set of counters per process, not per context. Benchmarks
   measure one context at a time and this keeps the macros to a single memory
   reference. Per-context counters if something ever needs to measure two at
   once. */
static ar_counters g_counters;

ar_counters *ar_counters_get(void)
{
    return &g_counters;
}

void ar_counters_reset(void)
{
    memset(&g_counters, 0, sizeof g_counters);
}

int ar_counters_enabled(void)
{
#ifdef AR_INSTRUMENT
    return 1;
#else
    return 0;
#endif
}
