/*
 * areole - line breaking, after UAX #14.
 * SPDX-License-Identifier: MIT
 *
 * Where a line may be broken is not "at spaces". It is a property of the pair
 * of characters either side of every position, and getting it wrong is
 * immediately visible: a line ending in an opening bracket, a full stop
 * orphaned at the start of a line, a hyphenated word split after the hyphen of
 * a compound but not inside a number.
 *
 * This implements the pair-table algorithm from UAX #14 over the break classes
 * that occur in practice. It does not ship the full Unicode character
 * database: that is roughly fourteen hundred ranges, most of them scripts this
 * library cannot yet shape text for, and carrying it would cost more than the
 * library. What is covered is stated in ar_break.c beside the table, so the
 * gap is known rather than discovered.
 *
 * No allocation, no state between calls.
 */
#ifndef AR_BREAK_H
#define AR_BREAK_H

#include "areole.h"

/* The subset of UAX #14 classes this distinguishes. The names are the
   standard's own, so the table below can be read against the specification. */
enum
{
    AR_LB_XX = 0, /* unknown, treated as AL       */
    AR_LB_AL,     /* ordinary letter              */
    AR_LB_SP,     /* space                        */
    AR_LB_BK,     /* mandatory break              */
    AR_LB_CR,
    AR_LB_LF,
    AR_LB_NL,
    AR_LB_ZW, /* zero width space: break here */
    AR_LB_WJ, /* word joiner: never break     */
    AR_LB_GL, /* non-breaking glue            */
    AR_LB_BA, /* break after: hyphen, dashes  */
    AR_LB_HY, /* hyphen-minus                 */
    AR_LB_BB, /* break before                 */
    AR_LB_B2, /* break either side, em dash   */
    AR_LB_OP, /* opening punctuation          */
    AR_LB_CL, /* closing punctuation          */
    AR_LB_CP, /* closing parenthesis          */
    AR_LB_QU, /* quotation                    */
    AR_LB_NS, /* non-starter                  */
    AR_LB_EX, /* exclamation, question        */
    AR_LB_IS, /* infix separator: . ,         */
    AR_LB_SY, /* symbol: /                    */
    AR_LB_NU, /* numeric                      */
    AR_LB_PR, /* prefix: currency             */
    AR_LB_PO, /* postfix: percent             */
    AR_LB_ID, /* ideographic                  */
    AR_LB_CM, /* combining mark               */
    AR_LB_COUNT
};

ar_i32 ar_break_class(ar_u32 cp);

/* Break opportunities. */
enum
{
    AR_BREAK_NONE = 0,     /* may not break here      */
    AR_BREAK_ALLOWED = 1,  /* may break here          */
    AR_BREAK_MANDATORY = 2 /* must break here         */
};

/*
 * Walks UTF-8 and reports the next position at which the line may or must
 * break. `text` is the whole string; `from` is a byte offset into it.
 *
 * Returns the byte offset of the break opportunity, and writes the kind into
 * `kind`. The offset is where the next line begins, so trailing spaces belong
 * to the line before it -- which is what makes a run of spaces at a wrap point
 * disappear rather than indent the next line.
 *
 * Returns the length of the string when there are no more opportunities.
 */
ar_i32 ar_break_next(const char *text, ar_i32 from, ar_i32 *kind);

#endif /* AR_BREAK_H */
