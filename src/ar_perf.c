/*
 * areole - frame timing, counters and percentiles.
 * SPDX-License-Identifier: MIT
 *
 * Nothing here allocates and nothing here calls the clock. Timestamps arrive
 * from the caller, which keeps this file portable and lets the backend own
 * the awkward question of which clock is actually trustworthy.
 */
#include "ar_internal.h"

#include <stdio.h>
#include <string.h>

void ar_perf_reset(ar_perf *p)
{
    memset(p, 0, sizeof *p);
}

void ar_perf_begin(ar_perf *p, ar_u32 now_us)
{
    memset(&p->cur, 0, sizeof p->cur);
    p->started = now_us;
    p->mark = now_us;
}

void ar_perf_mark(ar_perf *p, ar_phase phase, ar_u32 now_us)
{
    if ((ar_u32)phase >= (ar_u32)AR_PHASE_COUNT)
    {
        return;
    }
    /* The clock is monotonic by construction, but a wrap at the 32 bit
       boundary would otherwise show up as an enormous phase. Treat it as
       zero rather than as a spike that would poison the percentiles. */
    p->cur.phase_us[phase] = now_us >= p->mark ? now_us - p->mark : 0;
    p->mark = now_us;
}

void ar_perf_end(ar_perf *p, ar_u32 now_us)
{
    p->cur.total_us = now_us >= p->started ? now_us - p->started : 0;

    p->ring[p->head] = p->cur;
    p->head = (p->head + 1u) % AR_PERF_RING;
    if (p->count < AR_PERF_RING)
    {
        p->count++;
    }
    p->frames++;
}

static ar_u32 ar__sample(const ar_perf_frame *f, ar_phase phase)
{
    if ((ar_u32)phase >= (ar_u32)AR_PHASE_COUNT)
    {
        return f->total_us;
    }
    return f->phase_us[phase];
}

ar_u32 ar_perf_max(const ar_perf *p, ar_phase phase)
{
    ar_u32 i, best = 0;

    for (i = 0; i < p->count; ++i)
    {
        ar_u32 v = ar__sample(&p->ring[i], phase);
        if (v > best)
        {
            best = v;
        }
    }
    return best;
}

ar_u32 ar_perf_percentile(const ar_perf *p, ar_phase phase, ar_u32 pct)
{
    ar_u32 sorted[AR_PERF_RING];
    ar_u32 i, j, v, idx;

    if (p->count == 0)
    {
        return 0;
    }
    if (pct > 100u)
    {
        pct = 100u;
    }

    for (i = 0; i < p->count; ++i)
    {
        sorted[i] = ar__sample(&p->ring[i], phase);
    }

    /* ponytail: insertion sort over at most 256 already-similar values, run
       only when something asks for a percentile rather than every frame. It
       is the wrong algorithm at ten thousand samples; the ring is 256 and the
       upgrade path is a histogram if the overlay ever runs per frame. */
    for (i = 1; i < p->count; ++i)
    {
        v = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > v)
        {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }

    /* Nearest rank. p100 must be the true maximum, and p0 the true minimum,
       so the index is clamped rather than allowed to run off the end. */
    idx = (pct * (p->count - 1u) + 50u) / 100u;
    if (idx >= p->count)
    {
        idx = p->count - 1u;
    }
    /* False positive below. The count == 0 case returned at the top, and the
       fill loop above writes sorted[0 .. count-1] while idx is clamped below
       count; cppcheck cannot connect the early return to the loop bound.

       The suppression is on its own line with nothing after the id, because
       cppcheck parses the rest of that line as suppression attributes and
       rejects prose there. */
    /* cppcheck-suppress uninitvar */
    return sorted[idx];
}

/* ------------------------------------------------------------------------
 * Overlay
 * ------------------------------------------------------------------------ */
static const char *const AR_PHASE_NAME[AR_PHASE_COUNT] = {"style", "layout", "raster", "present"};

void ar_perf_overlay(ar_perf *p, ar_surface *s, ar_rect clip, ar_i32 x, ar_i32 y, ar_i32 scale)
{
    char   line[96];
    ar_i32 lh, row, i;
    ar_i32 w, h;

    if (scale < 1)
    {
        scale = 1;
    }
    lh = ar_text_line_height(scale);

    w = 46 * 6 * scale;
    h = lh * (AR_PHASE_COUNT + 4) + 10 * scale;

    ar_fill_rect(s, ar_rect_make(x, y, w, h), clip, AR_RGBA(0x14, 0x14, 0x16, 0xD8));
    ar_fill_rect(s, ar_rect_make(x, y, w, 1), clip, AR_RGBA(0xFF, 0xFF, 0xFF, 0x20));

    row = 0;
    x += 6 * scale;
    y += 5 * scale;

    sprintf(line, "%-8s %7s %7s %7s", "phase", "p50", "p99", "max");
    ar_draw_text(s, clip, x, y + row++ * lh, line, scale, AR_HEX(0x8A8FA0));

    for (i = 0; i < AR_PHASE_COUNT; ++i)
    {
        sprintf(line, "%-8s %6luu %6luu %6luu", AR_PHASE_NAME[i],
                (unsigned long)ar_perf_percentile(p, (ar_phase)i, 50),
                (unsigned long)ar_perf_percentile(p, (ar_phase)i, 99),
                (unsigned long)ar_perf_max(p, (ar_phase)i));
        ar_draw_text(s, clip, x, y + row++ * lh, line, scale, AR_HEX(0xD8D4CC));
    }

    sprintf(line, "%-8s %6luu %6luu %6luu", "frame",
            (unsigned long)ar_perf_percentile(p, AR_PHASE_COUNT, 50),
            (unsigned long)ar_perf_percentile(p, AR_PHASE_COUNT, 99),
            (unsigned long)ar_perf_max(p, AR_PHASE_COUNT));
    ar_draw_text(s, clip, x, y + row++ * lh, line, scale, AR_HEX(0xE8C39E));

    /* The invariant worth putting on screen. If arena use per frame ever
       varies once the UI has settled, something started allocating. */
    sprintf(line, "arena %lu B/frame   nodes %lu   frames %lu",
            (unsigned long)p->ring[(p->head + AR_PERF_RING - 1u) % AR_PERF_RING].arena_frame_bytes,
            (unsigned long)p->ring[(p->head + AR_PERF_RING - 1u) % AR_PERF_RING].nodes,
            (unsigned long)p->frames);
    ar_draw_text(s, clip, x, y + row++ * lh, line, scale, AR_HEX(0x8A8FA0));
}
