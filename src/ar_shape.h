/*
 * areole - OpenType shaping.
 * SPDX-License-Identifier: MIT
 *
 * A string of characters is not a list of glyphs. Two characters can become
 * one glyph, one can become several, and the space between a pair depends on
 * which pair it is. A font carries tables saying so, and ignoring them is what
 * makes text look like it was set by a program rather than by a typesetter.
 *
 * This is not a HarfBuzz replacement. It is the subset that changes how Latin,
 * Greek and Cyrillic look, which is what this library draws today:
 *
 *   GSUB type 4   ligature substitution -- fi, fl, ffi
 *   GPOS type 2   pair positioning -- kerning, both formats
 *   kern          the legacy table, for fonts that ship no GPOS
 *
 * The complex scripts named in the release document -- Arabic joining, Indic
 * reordering, mark attachment -- need GSUB 1, 2 and 6 and GPOS 4, 5 and 6, and
 * are not here yet. A run in those scripts comes out unshaped, which is legible
 * and wrong, rather than absent.
 *
 * No allocation. Shaping rewrites a glyph buffer the caller owns.
 */
#ifndef AR_SHAPE_H
#define AR_SHAPE_H

#include "ar_font_file.h"

#define AR_SHAPE_MAX_LOOKUPS 64

typedef struct ar_shaper
{
    const ar_face *face;

    ar_u32 gsub, gpos, kern;

    /* Lookup list offsets and the lookups selected by the features we use, so
       that shaping a run does not walk the feature tables again. */
    /* A font repeats a feature once per script and language system, and they
       mostly point at the same lookups. Sixty-four was chosen after sixteen
       silently truncated Times and Calibri to their Arabic and Indic lookups
       and dropped the Latin ones -- a cap that loses the common case is worse
       than no cap. */
    ar_u32 liga[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 liga_count;
    ar_u32 kerns[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 kern_count;

    int ok;
} ar_shaper;

/* Reads the tables and selects the lookups. Cheap enough to do per face, not
   per run; a shaper is valid for as long as its face is. */
int ar_shape_init(ar_shaper *sh, const ar_face *face);

/*
 * Rewrites a run of glyphs in place.
 *
 * `glyphs` holds `count` glyph indices and `adv` their advances in font units.
 * Ligatures shorten the run; kerning adjusts advances. `cluster` maps each
 * glyph back to the index of the character it came from, so that a caret and a
 * selection still land between characters after two of them became one glyph.
 *
 * Returns the new glyph count.
 */
ar_i32 ar_shape_run(const ar_shaper *sh, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *cluster,
                    ar_i32 count);

#endif /* AR_SHAPE_H */
