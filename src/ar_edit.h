/*
 * ar_edit.h -- an editable run of text, its caret, and its selection.
 *
 * Deliberately free of the rest of the engine: no boxes, no styles, no input
 * struct. An edit buffer is a string, two offsets and an undo ring, and every
 * hard question in it is about Unicode rather than about layout. Keeping it
 * separable is what lets the hard questions be tested directly -- a caret that
 * lands mid-emoji is a bug you want to see as a failing assertion, not as a
 * smear on a screenshot.
 */
#ifndef AR_EDIT_H
#define AR_EDIT_H

#include "../include/areole.h"

/* One field's worth. The text is UTF-8 and is not NUL-terminated inside the
   buffer; `len` is the authority. */
#define AR_EDIT_CAP  256
#define AR_EDIT_UNDO 16

/*
 * One undo step: the whole buffer.
 *
 * Whole rather than a diff, which is the trade this size makes for itself. A
 * field is 256 bytes and sixteen steps of it is four kilobytes; a diff would be
 * smaller and would need a representation, an apply, an invert and a test for
 * each, to save three kilobytes in a structure that exists once per focused
 * field. The ring is the cheap correct answer at this size and would be the
 * wrong one for a document.
 */
typedef struct ar_edit_step
{
    char   text[AR_EDIT_CAP];
    ar_u16 len;
    ar_u16 caret;
} ar_edit_step;

typedef struct ar_edit
{
    char   text[AR_EDIT_CAP];
    ar_u16 len;

    /*
     * The caret and the other end of the selection.
     *
     * Two offsets and not a start and a length, because a selection has a
     * direction: shift-left from the middle of a word and then shift-right
     * must give the text back, which needs to know which end is moving. The
     * caret is the moving end and the anchor is the one that stays.
     */
    ar_u16 caret;
    ar_u16 anchor;

    ar_edit_step undo[AR_EDIT_UNDO];
    ar_u8        undo_n;    /* steps recorded */
    ar_u8        undo_at;   /* where redo would go */
    ar_u8        coalescing; /* the last change was a typed character */
} ar_edit;

void ar_edit_init(ar_edit *e, const char *text);

/*
 * The next and previous grapheme cluster boundary.
 *
 * Not the next codepoint. Stepping by codepoint splits a combining sequence,
 * an emoji with a skin tone, a flag, or a Hangul syllable -- and it is the
 * single most common text-editing bug precisely because it looks right in
 * English and in a test written in English.
 */
ar_u16 ar_edit_next(const ar_edit *e, ar_u16 at);
ar_u16 ar_edit_prev(const ar_edit *e, ar_u16 at);

/* Does this offset sit on a boundary? Used by the tests to sweep a corpus. */
int ar_edit_is_boundary(const ar_edit *e, ar_u16 at);

/* Whether anything is selected, and the ordered range if so. */
int ar_edit_selection(const ar_edit *e, ar_u16 *lo, ar_u16 *hi);

/* Replace the selection (or insert at the caret) with UTF-8 text. */
void ar_edit_insert(ar_edit *e, const char *utf8, ar_u32 n);

/* Backspace and Delete: remove the selection if there is one, otherwise one
   cluster in that direction. */
void ar_edit_backspace(ar_edit *e);
void ar_edit_delete(ar_edit *e);

/* Move the caret. `extend` keeps the anchor, which is what Shift does. */
void ar_edit_move(ar_edit *e, ar_i32 delta_clusters, int extend);
void ar_edit_home(ar_edit *e, int extend);
void ar_edit_end(ar_edit *e, int extend);

/* The word around an offset, for a double click. Boundaries come from the same
   rules the line breaker uses rather than from `isspace`. */
void ar_edit_word_at(const ar_edit *e, ar_u16 at, ar_u16 *lo, ar_u16 *hi);
void ar_edit_select_word(ar_edit *e, ar_u16 at);
void ar_edit_select_all(ar_edit *e);

int ar_edit_undo(ar_edit *e);
int ar_edit_redo(ar_edit *e);

#endif /* AR_EDIT_H */
