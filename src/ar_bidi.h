/*
 * areole - the bidirectional algorithm, UAX #9.
 * SPDX-License-Identifier: MIT
 *
 * Text in Hebrew or Arabic runs right to left, but numbers inside it run left
 * to right, and a Latin quotation inside that runs left to right again. The
 * order characters are stored in and the order they are drawn in are different
 * orders, and working out the second from the first is not a matter of
 * reversing the Arabic parts -- it is a defined algorithm with a resolution
 * phase per rule class.
 *
 * This implements it: the paragraph level, explicit embeddings and isolates,
 * the weak, neutral and implicit rules in order, the paired bracket algorithm,
 * and reordering into visual runs.
 *
 * Everything above this consumes runs rather than strings from here on. That
 * is an interface change which is nearly free now and would be painful later.
 *
 * No allocation. The caller supplies one byte of level and one of class per
 * character, which is what an algorithm defined over per-character properties
 * needs and all it needs.
 */
#ifndef AR_BIDI_H
#define AR_BIDI_H

#include "areole.h"

/* The bidi classes, named as UAX #9 names them so the rules below can be read
   against the specification. */
enum
{
    AR_BC_L = 0, /* left to right                    */
    AR_BC_R,     /* right to left                    */
    AR_BC_AL,    /* right to left Arabic             */
    AR_BC_EN,    /* European number                  */
    AR_BC_ES,    /* European separator, + -          */
    AR_BC_ET,    /* European terminator, % degree    */
    AR_BC_AN,    /* Arabic number                    */
    AR_BC_CS,    /* common separator, : , .          */
    AR_BC_NSM,   /* non-spacing mark                 */
    AR_BC_BN,    /* boundary neutral                 */
    AR_BC_B,     /* paragraph separator              */
    AR_BC_S,     /* segment separator, tab           */
    AR_BC_WS,    /* whitespace                       */
    AR_BC_ON,    /* other neutral                    */
    AR_BC_LRE,
    AR_BC_LRO,
    AR_BC_RLE,
    AR_BC_RLO,
    AR_BC_PDF,
    AR_BC_LRI,
    AR_BC_RLI,
    AR_BC_FSI,
    AR_BC_PDI,
    AR_BC_COUNT
};

ar_i32 ar_bidi_class(ar_u32 cp);

/* The paragraph direction. AUTO applies rule P2/P3: the first strong character
   decides, and a paragraph with none is left to right. */
enum
{
    AR_DIR_AUTO = 0,
    AR_DIR_LTR = 1,
    AR_DIR_RTL = 2
};

/*
 * Resolves embedding levels for one paragraph of UTF-8.
 *
 * `levels` receives one byte per *codepoint*, not per byte, and `count` is the
 * number of codepoints. Even levels are left to right and odd ones right to
 * left, which is the whole of what the rest of the system needs to know.
 *
 * Returns the number of codepoints, or 0 if the buffers are too small.
 */
ar_i32 ar_bidi_levels(const char *utf8, ar_i32 dir, ar_u8 *levels, ar_u8 *classes, ar_i32 cap,
                      ar_i32 *paragraph_level);

/* One run of characters at a single embedding level, in visual order. */
typedef struct ar_bidi_run
{
    ar_i32 start; /* index of the first codepoint      */
    ar_i32 count;
    ar_u8  level; /* odd means it is drawn right to left */
} ar_bidi_run;

/*
 * Rule L2: reorders resolved levels into the runs to draw, left to right.
 *
 * Returns how many runs there are, which is at most `count` and in practice a
 * handful. A caller draws them in the order given and reverses the characters
 * within any run whose level is odd.
 */
ar_i32 ar_bidi_runs(const ar_u8 *levels, ar_i32 count, ar_bidi_run *runs, ar_i32 max_runs);

#endif /* AR_BIDI_H */
