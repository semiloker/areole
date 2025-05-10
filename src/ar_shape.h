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

    /* Arabic positional forms. A letter has up to four shapes depending on
       what it joins to on each side, and each shape is a separate glyph
       reached through a single substitution. Without these, Arabic renders as
       a row of isolated letters -- readable in the way that S P A C E D
       C A P I T A L S are readable. */
    ar_u32 init[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 init_count;
    ar_u32 medi[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 medi_count;
    ar_u32 fina[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 fina_count;
    ar_u32 rlig[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 rlig_count;

    /* Mark attachment. A diacritic has no advance and no position of its own:
       the font says where it sits relative to the letter it belongs to. With
       nothing applied, every Arabic and Devanagari mark lands on the baseline
       at the origin, which reads as the text having lost them. */
    ar_u32 mark[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 mark_count;

    /* Mark to mark. A shadda with a vowel above it needs the vowel placed
       against the shadda, not against the letter underneath both, or the two
       marks sit on top of each other. */
    ar_u32 mkmk[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 mkmk_count;

    /* ccmp runs before everything else and does whatever a font needs done
       first: composing a letter and its mark into one glyph, or decomposing
       one character into several so that later rules can see the parts. */
    ar_u32 ccmp[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 ccmp_count;

    /* Contextual alternates, and the lookup list itself.
     *
     * A chained contextual lookup does not substitute anything on its own: it
     * matches a pattern and then names other lookups by index to run at
     * positions inside the match. So this is the one feature that needs the
     * list rather than the handful of offsets resolved from it. */
    ar_u32 calt[AR_SHAPE_MAX_LOOKUPS];
    ar_i32 calt_count;
    ar_u32 gsub_lookups;

    /* GDEF's glyph class definition, which is how a mark is recognised. */
    ar_u32 glyph_classes;

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

/*
 * The same, told which characters the glyphs came from so that Arabic
 * positional forms can be chosen.
 *
 * Joining is a property of the characters, not the glyphs: whether a letter
 * takes its initial form depends on what precedes and follows it in the text.
 * A shaper handed only glyph indices cannot know that, which is why this takes
 * the codepoints as well.
 *
 * They are not const, and that is not an oversight. Indic reordering moves
 * characters as well as glyphs -- a vowel sign typed after its consonant is
 * drawn before it -- and the passes that follow have to see the order that
 * will actually be rendered.
 */
ar_i32 ar_shape_run_cp(const ar_shaper *sh, ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv,
                       ar_i32 *cluster, ar_i32 count);

/*
 * The same again, with somewhere to put mark offsets.
 *
 * dx and dy receive a per-glyph displacement in font units, which is zero for
 * everything except an attached mark. They may be null, and then mark
 * attachment is skipped rather than silently dropped -- a caller with nowhere
 * to put the offsets is better off with marks on the baseline than with marks
 * it thinks are positioned and are not.
 */
ar_i32 ar_shape_run_pos(const ar_shaper *sh, ar_u32 *cps, ar_i32 *glyphs, ar_i32 *adv, ar_i32 *dx,
                        ar_i32 *dy, ar_i32 *cluster, ar_i32 count, ar_i32 cap);

/* Arabic joining classes, from the Unicode joining type property. */
enum
{
    AR_JOIN_U = 0, /* non-joining                       */
    AR_JOIN_R,     /* joins to the right only           */
    AR_JOIN_D,     /* joins on both sides               */
    AR_JOIN_C,     /* causes joining, joins nothing     */
    AR_JOIN_T      /* transparent: marks, skipped over  */
};

ar_i32 ar_join_type(ar_u32 cp);

#endif /* AR_SHAPE_H */
