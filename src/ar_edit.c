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
    AR__GB_EXTEND, /* combining marks, variation selectors, emoji modifiers */
    AR__GB_ZWJ,
    AR__GB_RI, /* regional indicator, the halves of a flag */
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
    if (ar__in(c, 0x0300, 0x036F) || /* combining diacriticals            */
        ar__in(c, 0x0483, 0x0489) || /* Cyrillic                          */
        ar__in(c, 0x0591, 0x05BD) || c == 0x05BF || ar__in(c, 0x05C1, 0x05C2) ||
        ar__in(c, 0x0610, 0x061A) || ar__in(c, 0x064B, 0x065F) || c == 0x0670 ||
        ar__in(c, 0x06D6, 0x06DC) || ar__in(c, 0x0730, 0x074A) || ar__in(c, 0x07A6, 0x07B0) ||
        ar__in(c, 0x0900, 0x0902) || c == 0x093A ||
        ar__in(c, 0x093E, 0x094C) || /* Devanagari matras, mostly Extend  */
        ar__in(c, 0x0951, 0x0957) || ar__in(c, 0x0E31, 0x0E3A) || ar__in(c, 0x1AB0, 0x1AFF) ||
        ar__in(c, 0x1DC0, 0x1DFF) || ar__in(c, 0x20D0, 0x20F0) || /* combining marks for symbols */
        ar__in(c, 0xFE00, 0xFE0F) || /* variation selectors               */
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
static ar_u32 ar__cp_at(const char *t, ar_u16 len, ar_u16 at, ar_u16 *end)
{
    const char *p = t + at;
    const char *q = p;
    ar_u32      c;

    if (at >= len)
    {
        *end = at;
        return 0;
    }
    c = ar_utf8_next(&q);
    *end = (ar_u16)(at + (q - p));
    if (*end > len)
    {
        *end = len;
    }
    return c;
}

/* The offset of the codepoint before `at`, found by walking back over
   continuation bytes -- which is what makes UTF-8 worth using. */
static ar_u16 ar__cp_before(const char *t, ar_u16 at)
{
    ar_u16 i = at;

    while (i > 0)
    {
        --i;
        if (((unsigned char)t[i] & 0xC0) != 0x80)
        {
            break;
        }
    }
    return i;
}

/* How many regional indicators run backwards from `at`. Flags pair up, so the
   parity of this decides whether the next one joins or starts afresh. */
static ar_i32 ar__ri_run(const char *t, ar_u16 len, ar_u16 at)
{
    ar_i32 n = 0;
    ar_u16 i = at;

    while (i > 0)
    {
        ar_u16 prev = ar__cp_before(t, i);
        ar_u16 end;

        if (ar__gb_class(ar__cp_at(t, len, prev, &end)) != AR__GB_RI)
        {
            break;
        }
        ++n;
        i = prev;
    }
    return n;
}

ar_u16 ar_cluster_next(const char *t, ar_u16 len, ar_u16 at)
{
    ar_u16 i;

    if (at >= len)
    {
        return len;
    }
    i = at;
    for (;;)
    {
        ar_u16 end, next_end;
        ar_u8  a = ar__gb_class(ar__cp_at(t, len, i, &end));
        ar_u8  b;

        if (end >= len)
        {
            return len;
        }
        b = ar__gb_class(ar__cp_at(t, len, end, &next_end));
        if (ar__gb_break(a, b, ar__ri_run(t, len, end)))
        {
            return end;
        }
        i = end;
    }
}

ar_u16 ar_cluster_prev(const char *t, ar_u16 len, ar_u16 at)
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
    i = ar__cp_before(t, at);
    while (i > 0)
    {
        ar_u16 prev = ar__cp_before(t, i);
        ar_u16 end;
        ar_u8  a = ar__gb_class(ar__cp_at(t, len, prev, &end));
        ar_u8  b;
        ar_u16 unused;

        b = ar__gb_class(ar__cp_at(t, len, i, &unused));
        if (ar__gb_break(a, b, ar__ri_run(t, len, i)))
        {
            return i;
        }
        i = prev;
    }
    return 0;
}

int ar_cluster_is_boundary(const char *t, ar_u16 len, ar_u16 at)
{
    ar_u16 end;
    ar_u8  a, b;
    ar_u16 prev;

    if (at == 0 || at >= len)
    {
        return 1;
    }
    if (((unsigned char)t[at] & 0xC0) == 0x80)
    {
        return 0; /* inside a codepoint, never a boundary */
    }
    prev = ar__cp_before(t, at);
    a = ar__gb_class(ar__cp_at(t, len, prev, &end));
    b = ar__gb_class(ar__cp_at(t, len, at, &end));
    return ar__gb_break(a, b, ar__ri_run(t, len, at));
}

ar_u16 ar_edit_next(const ar_edit *e, ar_u16 at)
{
    return ar_cluster_next(e->text, e->len, at);
}

ar_u16 ar_edit_prev(const ar_edit *e, ar_u16 at)
{
    return ar_cluster_prev(e->text, e->len, at);
}

int ar_edit_is_boundary(const ar_edit *e, ar_u16 at)
{
    return ar_cluster_is_boundary(e->text, e->len, at);
}

ar_u32 ar_cluster_count(const char *t, ar_u32 n)
{
    ar_u32 k = 0;
    ar_u16 at = 0;
    ar_u16 len = (ar_u16)(n > AR_EDIT_CAP ? AR_EDIT_CAP : n);

    while (at < len)
    {
        at = ar_cluster_next(t, len, at);
        ++k;
    }
    return k;
}

/* ------------------------------------------------------------------------
 * The buffer
 * ------------------------------------------------------------------------ */

void ar_edit_init_n(ar_edit *e, const char *text, ar_u32 n)
{
    ar_u32 i;

    if (!e)
    {
        return;
    }
    /*
     * Not memset over the whole structure. It is 29 KB, most of it the undo
     * log, and every byte of the log is unreachable until a step writes it --
     * the counts below are what make it empty. Clearing it anyway would be
     * paid on every focus change for the sake of bytes nobody can read.
     */
    if (!text)
    {
        n = 0;
    }
    if (n > AR_EDIT_CAP)
    {
        n = AR_EDIT_CAP;
        /* Never keep half a codepoint at the cut. */
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
        {
            --n;
        }
    }
    for (i = 0; i < n; ++i)
    {
        e->text[i] = text[i];
    }
    e->len = (ar_u16)n;
    e->caret = e->len;
    e->anchor = e->len;
    e->max_cp = 0;
    e->steps_n = 0;
    e->steps_at = 0;
    e->log_used = 0;
    e->coalescing = 0;
}

void ar_edit_init(ar_edit *e, const char *text)
{
    ar_u32 n = 0;

    if (text)
    {
        while (text[n] && n < AR_EDIT_CAP)
        {
            ++n;
        }
    }
    ar_edit_init_n(e, text, n);
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

ar_u16 ar_edit_snap(const ar_edit *e, ar_u16 at)
{
    if (at >= e->len)
    {
        return e->len;
    }
    while (at > 0 && !ar_edit_is_boundary(e, at))
    {
        --at;
    }
    return at;
}

/* ------------------------------------------------------------------------
 * The undo log
 *
 * Steps [0, steps_at) can be undone; [steps_at, steps_n) are what redo would
 * bring back. Their bytes sit end to end in `log` in step order, so the bytes
 * of step k start where step k-1's end -- `off` is stored anyway, because
 * dropping the oldest step moves everything and recomputing every offset from
 * the start on each undo would make the log quadratic in its own length.
 * ------------------------------------------------------------------------ */

static ar_u16 ar__step_bytes(const ar_edit_step *s)
{
    return (ar_u16)(s->del_n + s->ins_n);
}

/* Throw the oldest step away, which is what happens when either the steps or
   the bytes run out. A moving window, as in every editor at its own limit. */
static void ar__drop_oldest(ar_edit *e)
{
    ar_u16 gone, i;

    if (e->steps_n == 0)
    {
        return;
    }
    gone = ar__step_bytes(&e->steps[0]);
    memmove(e->log, e->log + gone, (size_t)(e->log_used - gone));
    e->log_used = (ar_u16)(e->log_used - gone);
    for (i = 1; i < e->steps_n; ++i)
    {
        e->steps[i - 1] = e->steps[i];
        e->steps[i - 1].off = (ar_u16)(e->steps[i - 1].off - gone);
    }
    e->steps_n--;
    if (e->steps_at > 0)
    {
        e->steps_at--;
    }
}

/*
 * Make the change at [lo, hi) to `ins` and remember how to take it back.
 *
 * `typing` is the coalescing request: a typed character that lands exactly
 * where the last typed character ended extends that step rather than starting
 * one, so a word typed is one undo and not eleven. Anything else -- a paste, a
 * deletion, a caret that moved in between -- starts a fresh step.
 */
static void ar__replace(ar_edit *e, ar_u16 lo, ar_u16 hi, const char *ins, ar_u16 n, int typing)
{
    ar_u16        del_n = (ar_u16)(hi - lo);
    ar_edit_step *last;
    ar_u16        i;

    if (del_n == 0 && n == 0)
    {
        return;
    }

    /* A new edit forgets what redo could have brought back, as everywhere. */
    if (e->steps_at < e->steps_n)
    {
        e->steps_n = e->steps_at;
        e->log_used =
            e->steps_n
                ? (ar_u16)(e->steps[e->steps_n - 1].off + ar__step_bytes(&e->steps[e->steps_n - 1]))
                : 0;
        e->coalescing = 0;
    }

    last = e->steps_n ? &e->steps[e->steps_n - 1] : 0;
    if (typing && e->coalescing && last && last->typing && del_n == 0 &&
        last->at + last->ins_n == lo && (ar_u32)e->log_used + n <= AR_EDIT_UNDO_BYTES)
    {
        /* The run's bytes are the last thing in the log, so the new ones go
           straight on the end of them. */
        for (i = 0; i < n; ++i)
        {
            e->log[e->log_used + i] = ins[i];
        }
        e->log_used = (ar_u16)(e->log_used + n);
        last->ins_n = (ar_u16)(last->ins_n + n);
    }
    else
    {
        ar_edit_step *st;

        while (e->steps_n > 0 && (e->steps_n >= AR_EDIT_UNDO_STEPS ||
                                  (ar_u32)e->log_used + del_n + n > AR_EDIT_UNDO_BYTES))
        {
            ar__drop_oldest(e);
        }
        st = &e->steps[e->steps_n];
        st->at = lo;
        st->del_n = del_n;
        st->ins_n = n;
        st->off = e->log_used;
        st->caret0 = e->caret;
        st->anchor0 = e->anchor;
        st->typing = (ar_u8)(typing ? 1 : 0);
        st->pad_ = 0;
        for (i = 0; i < del_n; ++i)
        {
            e->log[e->log_used + i] = e->text[lo + i];
        }
        for (i = 0; i < n; ++i)
        {
            e->log[e->log_used + del_n + i] = ins[i];
        }
        e->log_used = (ar_u16)(e->log_used + del_n + n);
        e->steps_n++;
    }
    e->steps_at = e->steps_n;
    e->coalescing = (ar_u8)(typing ? 1 : 0);

    /* And the text itself: close the gap or open one, then fill it. */
    if (n != del_n)
    {
        memmove(e->text + lo + n, e->text + hi, (size_t)(e->len - hi));
    }
    for (i = 0; i < n; ++i)
    {
        e->text[lo + i] = ins[i];
    }
    e->len = (ar_u16)(e->len - del_n + n);
    e->caret = (ar_u16)(lo + n);
    e->anchor = e->caret;
}

/* How many codepoints a run of UTF-8 holds: everything but continuation
   bytes. What `maxlength` counts, approximately -- HTML counts UTF-16 code
   units, and the two differ only for characters outside the BMP. */
static ar_u32 ar__cp_count(const char *p, ar_u32 n)
{
    ar_u32 i, k = 0;

    for (i = 0; i < n; ++i)
    {
        if (((unsigned char)p[i] & 0xC0) != 0x80)
        {
            ++k;
        }
    }
    return k;
}

/*
 * How much of `n` bytes of insertion fits, cut back to a codepoint boundary.
 *
 * Two limits: the buffer, and `max_cp` if somebody set one. Text over the
 * limit is dropped from the end of what was typed, never from what was already
 * there -- a field does not eat its own contents because a paste was long.
 */
static ar_u32 ar__fits(const ar_edit *e, const char *utf8, ar_u32 n, ar_u16 lo, ar_u16 hi)
{
    ar_u32 room = AR_EDIT_CAP - (ar_u32)(e->len - (hi - lo));
    ar_u32 asked = n;

    if (n > room)
    {
        n = room;
    }
    if (e->max_cp)
    {
        ar_u32 have = ar__cp_count(e->text, lo) + ar__cp_count(e->text + hi, (ar_u32)(e->len - hi));
        ar_u32 i, k = 0;

        if (have >= e->max_cp)
        {
            return 0;
        }
        for (i = 0; i < n; ++i)
        {
            if (((unsigned char)utf8[i] & 0xC0) != 0x80)
            {
                if (have + k >= e->max_cp)
                {
                    n = i;
                    break;
                }
                ++k;
            }
        }
    }
    /* Only a cut can land inside a codepoint, and only then is utf8[n] a byte
       of the caller's string rather than one past its end. */
    while (n > 0 && n < asked && ((unsigned char)utf8[n] & 0xC0) == 0x80)
    {
        --n;
    }
    return n;
}

static void ar__insert(ar_edit *e, const char *utf8, ar_u32 n, int typing)
{
    ar_u16 lo, hi;

    if (!e || !utf8 || n == 0)
    {
        return;
    }
    lo = e->caret;
    hi = e->caret;
    ar_edit_selection(e, &lo, &hi);
    n = ar__fits(e, utf8, n, lo, hi);
    if (n == 0 && lo == hi)
    {
        return;
    }
    /* Typing over a selection starts a typing step: the replacement is its
       first character and the rest of the word joins it, so select-all and
       retype is one undo -- which is what it was before the log existed. */
    ar__replace(e, lo, hi, utf8, (ar_u16)n, typing);
}

void ar_edit_insert(ar_edit *e, const char *utf8, ar_u32 n)
{
    if (e)
    {
        ar__insert(e, utf8, n, 1);
    }
}

void ar_edit_paste(ar_edit *e, const char *utf8, ar_u32 n)
{
    if (e)
    {
        e->coalescing = 0;
        ar__insert(e, utf8, n, 0);
    }
}

void ar_edit_replace_all(ar_edit *e, const char *utf8, ar_u32 n)
{
    if (!e)
    {
        return;
    }
    if (n > AR_EDIT_CAP)
    {
        n = AR_EDIT_CAP;
    }
    if (n == e->len && (n == 0 || memcmp(e->text, utf8, n) == 0))
    {
        return; /* no step for a change that changes nothing */
    }
    e->coalescing = 0;
    ar__replace(e, 0, e->len, utf8, (ar_u16)n, 0);
}

/* Remove [lo, hi) as one step, which is every deletion this file makes. */
static void ar__remove(ar_edit *e, ar_u16 lo, ar_u16 hi)
{
    if (hi <= lo)
    {
        return;
    }
    e->coalescing = 0;
    ar__replace(e, lo, hi, "", 0, 0);
}

void ar_edit_backspace(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__remove(e, lo, hi);
        return;
    }
    if (e->caret == 0)
    {
        return; /* and records nothing: an undo that undoes nothing is a bug */
    }
    ar__remove(e, ar_edit_prev(e, e->caret), e->caret);
}

void ar_edit_delete(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__remove(e, lo, hi);
        return;
    }
    if (e->caret >= e->len)
    {
        return;
    }
    ar__remove(e, e->caret, ar_edit_next(e, e->caret));
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

void ar_edit_set_caret(ar_edit *e, ar_u16 at, int extend)
{
    if (!e)
    {
        return;
    }
    e->caret = ar_edit_snap(e, at);
    if (!extend)
    {
        e->anchor = e->caret;
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
        at = ar__cp_before(e->text, e->len);
    }

    if (!ar__word_char(ar__cp_at(e->text, e->len, at, &end)))
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
        ar_u16 prev = ar__cp_before(e->text, i);
        ar_u32 c = ar__cp_at(e->text, e->len, prev, &end);

        if (ar__word_char(c))
        {
            i = prev;
            continue;
        }
        if (ar__word_join(c) && prev > 0)
        {
            ar_u16 before = ar__cp_before(e->text, prev);
            ar_u16 unused;

            if (ar__word_char(ar__cp_at(e->text, e->len, before, &unused)))
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
        ar_u32 c = ar__cp_at(e->text, e->len, i, &end);

        if (ar__word_char(c))
        {
            i = end;
            continue;
        }
        if (ar__word_join(c) && end < e->len)
        {
            ar_u16 unused;

            if (ar__word_char(ar__cp_at(e->text, e->len, end, &unused)))
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

/*
 * Where Ctrl and an arrow land: the start of the next word going right, the
 * start of this or the previous word going left.
 *
 * Windows' rule rather than the Mac's -- the Mac stops at the *end* of the
 * next word going right -- because the platform this ships on first is
 * Windows, and a caret that lands one place in every other text box on the
 * machine and another here is a bug report however defensible either is.
 */
static ar_u16 ar__word_stop(const ar_edit *e, ar_u16 at, ar_i32 dir)
{
    ar_u16 end;

    if (dir > 0)
    {
        /* Through the rest of this word, then through what follows it. */
        while (at < e->len && ar__word_char(ar__cp_at(e->text, e->len, at, &end)))
        {
            at = end;
        }
        while (at < e->len && !ar__word_char(ar__cp_at(e->text, e->len, at, &end)))
        {
            at = end;
        }
        return at;
    }
    while (at > 0)
    {
        ar_u16 prev = ar__cp_before(e->text, at);

        if (ar__word_char(ar__cp_at(e->text, e->len, prev, &end)))
        {
            break;
        }
        at = prev;
    }
    while (at > 0)
    {
        ar_u16 prev = ar__cp_before(e->text, at);

        if (!ar__word_char(ar__cp_at(e->text, e->len, prev, &end)))
        {
            break;
        }
        at = prev;
    }
    return at;
}

void ar_edit_move_word(ar_edit *e, ar_i32 dir, int extend)
{
    if (!e)
    {
        return;
    }
    e->coalescing = 0;
    if (!extend && e->caret != e->anchor)
    {
        ar_u16 lo, hi;

        ar_edit_selection(e, &lo, &hi);
        e->caret = dir < 0 ? lo : hi;
        e->anchor = e->caret;
    }
    e->caret = ar_edit_snap(e, ar__word_stop(e, e->caret, dir));
    if (!extend)
    {
        e->anchor = e->caret;
    }
}

void ar_edit_backspace_word(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__remove(e, lo, hi);
        return;
    }
    ar__remove(e, ar_edit_snap(e, ar__word_stop(e, e->caret, -1)), e->caret);
}

void ar_edit_delete_word(ar_edit *e)
{
    ar_u16 lo, hi;

    if (!e)
    {
        return;
    }
    if (ar_edit_selection(e, &lo, &hi))
    {
        ar__remove(e, lo, hi);
        return;
    }
    ar__remove(e, e->caret, ar_edit_snap(e, ar__word_stop(e, e->caret, 1)));
}

/* ------------------------------------------------------------------------
 * Undo
 * ------------------------------------------------------------------------ */

/* Swap `cut_n` bytes at `at` for `put_n` bytes of the log at `from`. The one
   apply both directions use, because undo and redo are the same replacement
   with its two halves exchanged. */
static void ar__apply(ar_edit *e, ar_u16 at, ar_u16 cut_n, ar_u16 from, ar_u16 put_n)
{
    ar_u16 i;

    if (put_n != cut_n)
    {
        memmove(e->text + at + put_n, e->text + at + cut_n, (size_t)(e->len - at - cut_n));
    }
    for (i = 0; i < put_n; ++i)
    {
        e->text[at + i] = e->log[from + i];
    }
    e->len = (ar_u16)(e->len - cut_n + put_n);
}

int ar_edit_undo(ar_edit *e)
{
    const ar_edit_step *st;

    if (!e || e->steps_at == 0)
    {
        return 0;
    }
    st = &e->steps[e->steps_at - 1];
    ar__apply(e, st->at, st->ins_n, st->off, st->del_n);
    e->caret = st->caret0;
    e->anchor = st->anchor0;
    e->steps_at--;
    e->coalescing = 0;
    return 1;
}

int ar_edit_redo(ar_edit *e)
{
    const ar_edit_step *st;

    if (!e || e->steps_at >= e->steps_n)
    {
        return 0;
    }
    st = &e->steps[e->steps_at];
    ar__apply(e, st->at, st->del_n, (ar_u16)(st->off + st->del_n), st->ins_n);
    e->caret = (ar_u16)(st->at + st->ins_n);
    e->anchor = e->caret;
    e->steps_at++;
    e->coalescing = 0;
    return 1;
}

ar_u16 ar_edit_undo_depth(const ar_edit *e)
{
    return e ? e->steps_at : 0;
}

ar_u16 ar_edit_redo_depth(const ar_edit *e)
{
    return e ? (ar_u16)(e->steps_n - e->steps_at) : 0;
}
