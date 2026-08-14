/*
 * areole - Indic reordering.
 * SPDX-License-Identifier: MIT
 *
 * Indic scripts are written in an order that is not the order they are stored
 * in, and the difference is not cosmetic. In Devanagari, the vowel sign i is
 * typed after its consonant and drawn before it: ki is stored as KA then
 * I-MATRA and rendered as I-MATRA then KA. A renderer that draws them in
 * storage order does not produce plain text, it produces a different word.
 *
 * The other reordering is reph. A syllable beginning RA + VIRAMA loses both
 * and gains a mark drawn above the *end* of the syllable, which can be several
 * characters later.
 *
 * This implements the syllable model and those two reorderings, which is the
 * overwhelming majority of what Devanagari needs to be readable, and applies
 * the presentation features a font provides for the rest.
 *
 * What it is not: the full Universal Shaping Engine. Scripts whose syllable
 * structure differs from Devanagari's -- Tamil's, Khmer's, Myanmar's -- are
 * given the Devanagari model, which is right more often than leaving them
 * unshaped and wrong less visibly than getting the reordering backwards.
 */
#ifndef AR_INDIC_H
#define AR_INDIC_H

#include "ar_shape.h"

/* The syllabic categories that change what happens. The full property has
   about twenty values; these are the ones the reordering depends on. */
enum
{
    AR_IND_OTHER = 0,
    AR_IND_CONSONANT,
    AR_IND_RA,        /* a consonant, but the one that forms reph        */
    AR_IND_VIRAMA,    /* halant: joins the consonant before to the one after */
    AR_IND_MATRA_PRE, /* a vowel sign drawn before its consonant         */
    AR_IND_MATRA_POST,
    AR_IND_MATRA_ABOVE,
    AR_IND_MATRA_BELOW,
    AR_IND_VOWEL,  /* independent                                     */
    AR_IND_NUKTA,
    AR_IND_BINDU,  /* anusvara, chandrabindu, visarga                 */
    AR_IND_COUNT
};

ar_i32 ar_indic_category(ar_u32 cp);

/* Non-zero if this codepoint is in a script this reorders. */
int ar_indic_is_indic(ar_u32 cp);

/*
 * Reorders one run into rendering order, rewriting cps, glyphs, advances and
 * clusters together so nothing comes apart.
 *
 * Returns non-zero if anything moved, which the caller needs because the
 * cluster map is what a caret and a selection are built on.
 */
int ar_indic_reorder(const ar_shaper *sh, ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv,
                     ar_i32 *cluster, ar_i32 count);

#endif /* AR_INDIC_H */
