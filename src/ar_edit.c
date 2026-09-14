/*
 * ar_edit.c -- the editable run: caret, selection, clusters, undo.
 *
 * The part of text editing that is always underestimated, and the reason it is
 * underestimated is that every mistake in it is invisible in English. A caret
 * that steps by codepoint works perfectly on "hello" and cuts a family emoji
 * in half; a word selection built on `isspace` selects "don" out of "don't"
 * and the whole of a Japanese sentence.
 *
 * So the file is mostly Unicode and hardly at all editing.
 */

#include "ar_edit.h"
#include "ar_text.h"

#include <string.h>

/* ------------------------------------------------------------------------
 * Grapheme clusters
 *
 * Not the whole of UAX #29, and the gap is stated rather than left to be
 * discovered: this implements the rules that decide where a *caret* may stand,
 * which is the subset that matters here. Extend, SpacingMark, Prepend, ZWJ
 * sequences, regional indicator pairs and Hangul syllables are handled;
 * emoji-modifier sequences fall out of ZWJ and Extend without a table of their
 * own.
 *
 * What is not here is the property table itself. The real one is fifteen
 * hundred ranges; this carries the ranges a caret can actually land wrong in,
 * which is combining marks, variation selectors, joiners, regional indicators
 * and jamo. A codepoint outside them is its own cluster, which is the right
 * answer for every script this engine can currently shape.
 * ------------------------------------------------------------------------ */

/* Grapheme_Cluster_Break classes, in the few kinds that change an answer. */
enum
{
    AR__GB_OTHER = 0,
    AR__GB_CR,
    AR__GB_LF,
    AR__GB_CONTROL,
    AR__GB_EXTEND,  /* combining marks, variation selectors, emoji modifiers */
    AR__GB_ZWJ,
    AR__GB_RI,      /* regional indicator, the halves of a flag */
    AR__GB_PREPEND,
    AR__GB_SPACING, /* SpacingMark: a mark that takes width and still joins */
    AR__GB_L,
    AR__GB_V,
    AR__GB_T,
    AR__GB_LV,
    AR__GB_LVT
};

static int ar__in(ar_u32 c, ar_u32 lo, ar_u32 hi)
{
    return c >= lo && c <= hi;
}

static ar_u8 ar__gb_class(ar_u32 c)
{
    if (c == 0x0D)
    {
        return AR__GB_CR;
    }
    if (c == 0x0A)
    {
        return AR__GB_LF;
    }
    if (c < 0x20 || c == 0x7F)
    {
        return AR__GB_CONTROL;
    }
    if (c == 0x200D)
    {
        return AR__GB_ZWJ;
    }
    if (ar__in(c, 0x1F1E6, 0x1F1FF))
    {
        return AR__GB_RI;
    }

    /*
     * Extend: the combining marks, the variation selectors and the emoji skin
     * tones. These are the ranges a caret lands wrong in, and the list is the
     * blocks rather than the full property -- a mark outside them is rare and
     * becomes its own cluster, which shows as a caret stop somebody did not
     * expect rather than as a broken character.
     */
    if (ar__in(c, 0x0300, 0x036F) ||  /* combining diacriticals            */
        ar__in(c, 0x0483, 0x0489) ||  /* Cyrillic                          */
        ar__in(c, 0x0591, 0x05BD) || c == 0x05BF || ar__in(c, 0x05C1, 0x05C2) ||
        ar__in(c, 0x0610, 0x061A) || ar__in(c, 0x064B, 0x065F) || c == 0x0670 ||
        ar__in(c, 0x06D6, 0x06DC) || ar__in(c, 0x0730, 0x074A) ||
        ar__in(c, 0x07A6, 0x07B0) || ar__in(c, 0x0900, 0x0902) || c == 0x093A ||
        ar__in(c, 0x093E, 0x094C) ||  /* Devanagari matras, mostly Extend  */
        ar__in(c, 0x0951, 0x0957) || ar__in(c, 0x0E31, 0x0E3A) ||
        ar__in(c, 0x1AB0, 0x1AFF) || ar__in(c, 0x1DC0, 0x1DFF) ||
        ar__in(c, 0x20D0, 0x20F0) ||  /* combining marks for symbols       */
        ar__in(c, 0xFE00, 0xFE0F) ||  /* variation selectors               */
        ar__in(c, 0xFE20, 0xFE2F) || ar__in(c, 0x1F3FB, 0x1F3FF) || /* skin tones */
        ar__in(c, 0xE0100, 0xE01EF))
    {
        return AR__GB_EXTEND;
    }

    /* Hangul, by jamo class. A syllable typed as L+V+T is one cluster and one
       caret stop, and splitting it puts the caret inside a letter. */
    if (ar__in(c, 0x1100, 0x115F))
    {
        return AR__GB_L;
    }
    if (ar__in(c, 0x1160, 0x11A7))
    {
        return AR__GB_V;
    }
    if (ar__in(c, 0x11A8, 0x11FF))
    {
        return AR__GB_T;
    }
    if (ar__in(c, 0xAC00, 0xD7A3))
    {
        /* Precomposed syllables: those at a multiple of 28 from the base are
           LV, the rest LVT. */
        return ((c - 0xAC00) % 28) == 0 ? AR__GB_LV : AR__GB_LVT;
    }
    return AR__GB_OTHER;
}

/*
 * Is there a cluster boundary between these two classes?
 *
 * The rules in the order UAX #29 gives them, because the order is what makes
 * them agree: GB3 before GB4, and GB9 after everything that would otherwise
 * break in front of a mark.
 */
static int ar__gb_break(ar_u8 a, ar_u8 b, ar_i32 ri_run)
{
    if (a == AR__GB_CR && b == AR__GB_LF)
    {
        return 0; /* GB3: CRLF is one cluster */
    }
    if (a == AR__GB_CR || a == AR__GB_LF || a == AR__GB_CONTROL)
    {
        return 1; /* GB4 */
    }
    if (b == AR__GB_CR || b == AR__GB_LF || b == AR__GB_CONTROL)
    {
        return 1; /* GB5 */
    }
    if (a == AR__GB_L && (b == AR__GB_L || b == AR__GB_V || b == AR__GB_LV || b == AR__GB_LVT))
    {
        return 0; /* GB6 */
    }
    if ((a == AR__GB_LV || a == AR__GB_V) && (b == AR__GB_V || b == AR__GB_T))
    {
        return 0; /* GB7 */
    }
    if ((a == AR__GB_LVT || a == AR__GB_T) && b == AR__GB_T)
    {
        return 0; /* GB8 */
    }
    if (b == AR__GB_EXTEND || b == AR__GB_ZWJ)
    {
        return 0; /* GB9: never break before a mark or a joiner */
    }
    if (b == AR__GB_SPACING)
    {
        return 0; /* GB9a */
    }
    if (a == AR__GB_PREPEND)
    {
        return 0; /* GB9b */
    }
    if (a == AR__GB_ZWJ)
    {
        /*
         * GB11, simplified: a joiner glues whatever follows.
         *
         * The full rule only glues an Extended_Pictographic, so this keeps a
         * ZWJ between two letters as one cluster where Unicode would break.
         * That sequence is not text anybody types; the sequences people do
         * type -- every emoji family, every profession, every flag -- come out
         * right, and the simplification is here rather than in a comment
         * nobody reads.
         */
        return 0;
    }
    if (a == AR__GB_RI && b == AR__GB_RI)
    {
        /* GB12/13: flags pair up, so break only after an even number. An odd
           run means this one starts a new pair. */
        return (ri_run % 2) == 0;
    }
    return 1; /* GB999 */
}

/* Decode the codepoint starting at `at`, and where it ends. */
static ar_u32 ar__cp_at(const ar_edit *e, ar_u16 at, ar_u16 *end)
{
    const char *p = e->text + at;
    const char *q = p;
    ar_u32      c;

    if (at >= e->len)
    {
        *end = at;
        return 0;
    }
    c = ar_utf8_next(&q);
    *end = (ar_u16)(at + (q - p));
    if (*end > e->len)
    {
        *end = e->len;
    }
    return c;
}

/* The offset of the codepoint before `at`, found by walking back over
   continuation bytes -- which is what makes UTF-8 worth using. */
static ar_u16 ar__cp_before(const ar_edit *e, ar_u16 at)
{
    ar_u16 i = at;

    while (i > 0)
    {
        --i;
        if (((unsigned char)e->text[i] & 0xC0) != 0x80)
        {
            break;
        }
    }
    return i;
}

/* How many regional indicators run backwards from `at`. Flags pair up, so the
   parity of this decides whether the next one joins or starts afresh. */
static ar_i32 ar__ri_run(const ar_edit *e, ar_u16 at)
{
    ar_i32 n = 0;
    ar_u16 i = at;

    while (i > 0)
    {
        ar_u16 prev = ar__cp_before(e, i);
        ar_u16 end;

        if (ar__gb_class(ar__cp_at(e, prev, &end)) != AR__GB_RI)
        {
            break;
        }
        ++n;
        i = prev;
    }
    return n;
}

ar_u16 ar_edit_next(const ar_edit *e, ar_u16 at)
{
    ar_u16 i;

    if (at >= e->len)
    {
        return e->len;
    }
    i = at;
    for (;;)
    {
        ar_u16 end, next_end;
        ar_u8  a = ar__gb_class(ar__cp_at(e, i, &end));
        ar_u8  b;

        if (end >= e->len)
        {
            return e->len;
        }
        b = ar__gb_class(ar__cp_at(e, end, &next_end));
        if (ar__gb_break(a, b, ar__ri_run(e, end)))
        {
            return end;
        }
        i = end;
    }
}

ar_u16 ar_edit_prev(const ar_edit *e, ar_u16 at)
{
    ar_u16 i;

    if (at == 0)
    {
        return 0;
    }

    /*
     * Walk back one codepoint at a time until the boundary test agrees.
     *
     * Backwards is not the mirror of forwards, because the rules look at what
     * comes before: the only honest way to find the previous boundary is to
     * find the codepoint start and then ask whether it is one.
     */
    i = ar__cp_before(e, at);
    while (i > 0)
    {
        ar_u16 prev = ar__cp_before(e, i);
        ar_u16 end;
        ar_u8  a = ar__gb_class(ar__cp_at(e, prev, &end));
        ar_u8  b;
        ar_u16 unused;

        b = ar__gb_class(ar__cp_at(e, i, &unused));
        if (ar__gb_break(a, b, ar__ri_run(e, i)))
        {
            return i;
        }
        i = prev;
    }
    return 0;
}

int ar_edit_is_boundary(const ar_edit *e, ar_u16 at)
{
    ar_u16 end;
    ar_u8  a, b;
    ar_u16 prev;

    if (at == 0 || at >= e->len)
    {
        return 1;
    }
    if (((unsigned char)e->text[at] & 0xC0) == 0x80)
    {
        return 0; /* inside a codepoint, never a boundary */
    }
    prev = ar__cp_before(e, at);
    a = ar__gb_class(ar__cp_at(e, prev, &end));
    b = ar__gb_class(ar__cp_at(e, at, &end));
    return ar__gb_break(a, b, ar__ri_run(e, at));
}

/* ------------------------------------------------------------------------
 * The buffer
 * ------------------------------------------------------------------------ */

void ar_edit_init(ar_edit *e, const char *text)
{
    ar_u32 n = 0;

    if (!e)
    {
        return;
    }
    memset(e, 0, sizeof *e);
    if (text)
    {
        while (text[n] && n < AR_EDIT_CAP)
        {
            e->text[n] = text[n];
            ++n;
        }
    }
    e->len = (ar_u16)n;
    e->caret = e->len;
    e->anchor = e->len;
}

int ar_edit_selection(const ar_edit *e, ar_u16 *lo, ar_u16 *hi)
{
    if (e->caret == e->anchor)
    {
        return 0;
    }
    *lo = e->caret < e->anchor ? e->caret : e->anchor;
    *hi = e->caret < e->anchor ? e->anchor : e->caret;
    return 1;
}

/* Take a copy before changing anything, unless this is more of the same typing
   run -- so a word typed is one undo step and not eleven. */
static void ar__record(ar_edit *e, int coalesce)
{
    ar_edit_step *st;

    if (coalesce && e->coalescing && e->undo_n > 0)
    {
        return;
    }
    if (e->undo_at >= AR_EDIT_UNDO)
    {
        ar_u8 i;

        /* The ring is full: drop the oldest. A field's history is not worth
           more than sixteen steps and a moving window is what every editor
           does once its own limit is reached. */
        for (i = 1; i < AR_EDIT_UNDO; ++i)
        {
            e->undo[i - 1] = e->undo[i];
        }
        e->undo_at = AR_EDIT_UNDO - 1;
    }
    st = &e->undo[e->undo_at];
    memcpy(st->text, e->text, e->len);
    st->len = e->len;
    st->caret = e->caret;
    e->undo_at++;
    e->undo_n = e->undo_at;
    e->coalescing = (ar_u8)(coalesce ? 1 : 0);
}

static void ar__erase(ar_edit *e, ar_u16 lo, ar_u16 hi)
{
    ar_u16 i;

    for (i = hi; i < e->len; ++i)
    {
        e->text[lo + (i - hi)] = e->text[i];
    }
    e->len = (ar_u16)(e->len - (hi - lo));
    e->caret = lo;
    e->anchor = lo;
}

void ar_edit_insert(ar_edit *e, const char *utf8, ar_u32 n)
{
    ar_u16 lo, hi;
    ar_u32 i;

    if (!e || !utf8 || n == 0)
    {
        return;
    }
    ar__record(e, 1);
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__erase(e, lo, hi);
    }
    if ((ar_u32)e->len + n > AR_EDIT_CAP)
    {
        n = AR_EDIT_CAP - e->len;
    }
    for (i = e->len; i > e->caret; --i)
    {
        e->text[i + n - 1] = e->text[i - 1];
    }
    for (i = 0; i < n; ++i)
    {
        e->text[e->caret + i] = utf8[i];
    }
    e->len = (ar_u16)(e->len + n);
    e->caret = (ar_u16)(e->caret + n);
    e->anchor = e->caret;
}

void ar_edit_backspace(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    ar__record(e, 0);
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__erase(e, lo, hi);
        return;
    }
    if (e->caret == 0)
    {
        return;
    }
    ar__erase(e, ar_edit_prev(e, e->caret), e->caret);
}

void ar_edit_delete(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    ar__record(e, 0);
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__erase(e, lo, hi);
        return;
    }
    if (e->caret >= e->len)
    {
        return;
    }
    ar__erase(e, e->caret, ar_edit_next(e, e->caret));
}

void ar_edit_move(ar_edit *e, ar_i32 delta, int extend)
{
    ar_i32 i;

    if (!e)
    {
        return;
    }
    e->coalescing = 0;

    /*
     * A plain arrow with a selection collapses it to the near end rather than
     * moving from the caret, which is what every editor does and what people
     * expect without being able to say so: select a word, press Left, and the
     * caret is at its start.
     */
    if (!extend && e->caret != e->anchor)
    {
        ar_u16 lo, hi;

        ar_edit_selection(e, &lo, &hi);
        e->caret = delta < 0 ? lo : hi;
        e->anchor = e->caret;
        if (delta == 0)
        {
            return;
        }
        delta += delta < 0 ? 1 : -1;
    }

    for (i = 0; i < (delta < 0 ? -delta : delta); ++i)
    {
        e->caret = delta < 0 ? ar_edit_prev(e, e->caret) : ar_edit_next(e, e->caret);
    }
    if (!extend)
    {
        e->anchor = e->caret;
    }
}

void ar_edit_home(ar_edit *e, int extend)
{
    e->caret = 0;
    if (!extend)
    {
        e->anchor = 0;
    }
    e->coalescing = 0;
}

void ar_edit_end(ar_edit *e, int extend)
{
    e->caret = e->len;
    if (!extend)
    {
        e->anchor = e->len;
    }
    e->coalescing = 0;
}

/* ------------------------------------------------------------------------
 * Words
 * ------------------------------------------------------------------------ */

/*
 * What counts as inside a word.
 *
 * Not `isspace`, which selects "don" out of "don't" and treats a whole
 * Japanese sentence as one word. This is the shape of UAX #29's word rules
 * that a double click needs: letters and digits join, an apostrophe or a
 * hyphen between two letters joins, and everything above the Latin ranges is
 * treated per codepoint because the scripts that need dictionary segmentation
 * are not ones a rule can settle.
 */
static int ar__word_char(ar_u32 c)
{
    if (c >= '0' && c <= '9')
    {
        return 1;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
    {
        return 1;
    }
    if (c == '_')
    {
        return 1;
    }
    if (c >= 0x00C0 && c < 0x2000)
    {
        return 1; /* accented Latin, Greek, Cyrillic, Hebrew, Arabic, Indic */
    }
    return 0;
}

static int ar__word_join(ar_u32 c)
{
    /* MidLetter and MidNumLet, in the two spellings anybody types. */
    return c == '\'' || c == 0x2019 || c == '-' || c == '.';
}

void ar_edit_word_at(const ar_edit *e, ar_u16 at, ar_u16 *lo, ar_u16 *hi)
{
    ar_u16 i, end;

    *lo = at;
    *hi = at;
    if (e->len == 0)
    {
        return;
    }
    if (at >= e->len)
    {
        at = ar__cp_before(e, e->len);
    }

    if (!ar__word_char(ar__cp_at(e, at, &end)))
    {
        /* Not in a word: select the run of whatever this is, which is what a
           double click on a space does. */
        *lo = at;
        *hi = end;
        return;
    }

    i = at;
    while (i > 0)
    {
        ar_u16 prev = ar__cp_before(e, i);
        ar_u32 c = ar__cp_at(e, prev, &end);

        if (ar__word_char(c))
        {
            i = prev;
            continue;
        }
        if (ar__word_join(c) && prev > 0)
        {
            ar_u16 before = ar__cp_before(e, prev);
            ar_u16 unused;

            if (ar__word_char(ar__cp_at(e, before, &unused)))
            {
                i = before;
                continue;
            }
        }
        break;
    }
    *lo = i;

    i = at;
    while (i < e->len)
    {
        ar_u32 c = ar__cp_at(e, i, &end);

        if (ar__word_char(c))
        {
            i = end;
            continue;
        }
        if (ar__word_join(c) && end < e->len)
        {
            ar_u16 unused;

            if (ar__word_char(ar__cp_at(e, end, &unused)))
            {
                i = end;
                continue;
            }
        }
        break;
    }
    *hi = i;
}

void ar_edit_select_word(ar_edit *e, ar_u16 at)
{
    ar_u16 lo, hi;

    ar_edit_word_at(e, at, &lo, &hi);
    e->anchor = lo;
    e->caret = hi;
    e->coalescing = 0;
}

void ar_edit_select_all(ar_edit *e)
{
    e->anchor = 0;
    e->caret = e->len;
    e->coalescing = 0;
}

/* ------------------------------------------------------------------------
 * Undo
 * ------------------------------------------------------------------------ */

int ar_edit_undo(ar_edit *e)
{
    ar_edit_step now;
    ar_edit_step *st;

    if (!e || e->undo_at == 0)
    {
        return 0;
    }

    /* The current state goes back where the step came from, so redo has
       somewhere to return to -- the ring holds a position and not a stack. */
    memcpy(now.text, e->text, e->len);
    now.len = e->len;
    now.caret = e->caret;

    e->undo_at--;
    st = &e->undo[e->undo_at];
    memcpy(e->text, st->text, st->len);
    e->len = st->len;
    e->caret = st->caret;
    e->anchor = e->caret;
    *st = now;
    e->coalescing = 0;
    return 1;
}

int ar_edit_redo(ar_edit *e)
{
    ar_edit_step now;
    ar_edit_step *st;

    if (!e || e->undo_at >= e->undo_n)
    {
        return 0;
    }
    memcpy(now.text, e->text, e->len);
    now.len = e->len;
    now.caret = e->caret;

    st = &e->undo[e->undo_at];
    memcpy(e->text, st->text, st->len);
    e->len = st->len;
    e->caret = st->caret;
    e->anchor = e->caret;
    *st = now;
    e->undo_at++;
    e->coalescing = 0;
    return 1;
}
