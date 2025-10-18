/*
 * ar_edit.h -- an editable run of text, its caret, and its selection.
 *
 * Deliberately free of the rest of the engine: no boxes, no styles, no input
 * struct. An edit buffer is a string, two offsets and an undo log, and every
 * hard question in it is about Unicode rather than about layout. Keeping it
 * separable is what lets the hard questions be tested directly -- a caret that
 * lands mid-emoji is a bug you want to see as a failing assertion, not as a
 * smear on a screenshot.
 */
#ifndef AR_EDIT_H
#define AR_EDIT_H

#include "../include/areole.h"

/*
 * One field's worth, and a textarea is a field.
 *
 * 4,096 bytes because 0.10.0's own budget is written against a 2,000 character
 * text area, and 2,000 characters of anything past Latin-1 is more than 2,000
 * bytes. It was 256 while the only field was a one-line input, which is the
 * size a name or a gate code needs and a paragraph does not.
 *
 * The text is UTF-8 and is not NUL-terminated inside the buffer; `len` is the
 * authority.
 */
#define AR_EDIT_CAP 4096

/*
 * The undo log: what changed, not what the whole buffer used to be.
 *
 * It was a ring of sixteen whole buffers, which was the cheap correct answer at
 * 256 bytes -- the header said so, and said it would be the wrong answer the
 * day a field was a document. At 4,096 bytes sixteen snapshots are 64 KB for
 * sixteen steps of history, and the acceptance criterion is five hundred.
 *
 * So a step is a replacement: at `at`, `del_n` bytes went and `ins_n` bytes
 * came, and both sets of bytes are kept end to end in one log. Undo puts the
 * deleted bytes back over the inserted ones; redo does the reverse. Every edit
 * this file makes -- typing, a paste over a selection, a backspace, a cut -- is
 * one replacement, which is why there is one representation and one apply.
 *
 * 512 steps and 16 KB of bytes, so 24 KB in all against 0.10.0's 128 KB
 * budget. When either runs out the oldest step goes, which is what every editor
 * does at its own limit; nothing here ever refuses an edit to keep a history.
 */
#define AR_EDIT_UNDO_STEPS 512
#define AR_EDIT_UNDO_BYTES 16384

typedef struct ar_edit_step
{
    ar_u16 at;     /* where the replacement happened */
    ar_u16 del_n;  /* bytes removed; first in the log */
    ar_u16 ins_n;  /* bytes inserted; straight after them */
    ar_u16 off;    /* where this step's bytes start in the log */
    ar_u16 caret0; /* the selection before, so undo can restore it */
    ar_u16 anchor0;
    ar_u8  typing; /* a typing run, which the next character may extend */
    ar_u8  pad_;
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

    /* The most codepoints this buffer may hold, or zero for as many as fit.
       What `maxlength` turns into: a cap on what is typed or pasted, never a
       reason to cut text that is already there. */
    ar_u16 max_cp;

    ar_edit_step steps[AR_EDIT_UNDO_STEPS];
    char         log[AR_EDIT_UNDO_BYTES];
    ar_u16       steps_n;    /* recorded                                  */
    ar_u16       steps_at;   /* [0, at) can be undone, [at, n) redone     */
    ar_u16       log_used;   /* bytes the recorded steps occupy            */
    ar_u8        coalescing; /* the last change was a typed character    */
} ar_edit;

void ar_edit_init(ar_edit *e, const char *text);

/* The same, from a span that is not NUL-terminated -- which is what an HTML
   attribute is. Copying to a NUL from one walks into the next element. */
void ar_edit_init_n(ar_edit *e, const char *text, ar_u32 n);

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

/* The same three over any run of UTF-8, for text that is not in a buffer --
   a password field nobody is typing in still draws one bullet per cluster. */
ar_u16 ar_cluster_next(const char *t, ar_u16 len, ar_u16 at);
ar_u16 ar_cluster_prev(const char *t, ar_u16 len, ar_u16 at);
int    ar_cluster_is_boundary(const char *t, ar_u16 len, ar_u16 at);
ar_u32 ar_cluster_count(const char *t, ar_u32 n);

/* The nearest boundary at or before `at`, for a caret placed by the mouse. */
ar_u16 ar_edit_snap(const ar_edit *e, ar_u16 at);

/* Whether anything is selected, and the ordered range if so. */
int ar_edit_selection(const ar_edit *e, ar_u16 *lo, ar_u16 *hi);

/* Replace the selection (or insert at the caret) with UTF-8 text. Typing
   coalesces into one undo step; `ar_edit_paste` never does. */
void ar_edit_insert(ar_edit *e, const char *utf8, ar_u32 n);
void ar_edit_paste(ar_edit *e, const char *utf8, ar_u32 n);

/* Replace everything, as one undo step. A number field stepped by an arrow
   key is this: the old value is the step back, not each digit of it. */
void ar_edit_replace_all(ar_edit *e, const char *utf8, ar_u32 n);

/* Backspace and Delete: remove the selection if there is one, otherwise one
   cluster in that direction -- or one word, with `word` set, which is what
   Ctrl does to both keys on every desktop. */
void ar_edit_backspace(ar_edit *e);
void ar_edit_delete(ar_edit *e);
void ar_edit_backspace_word(ar_edit *e);
void ar_edit_delete_word(ar_edit *e);

/* Move the caret. `extend` keeps the anchor, which is what Shift does. */
void ar_edit_move(ar_edit *e, ar_i32 delta_clusters, int extend);
void ar_edit_move_word(ar_edit *e, ar_i32 dir, int extend);
void ar_edit_home(ar_edit *e, int extend);
void ar_edit_end(ar_edit *e, int extend);

/* Put the caret at an offset, snapped back to a cluster boundary. */
void ar_edit_set_caret(ar_edit *e, ar_u16 at, int extend);

/* The word around an offset, for a double click. Boundaries come from the same
   rules the line breaker uses rather than from `isspace`. */
void ar_edit_word_at(const ar_edit *e, ar_u16 at, ar_u16 *lo, ar_u16 *hi);
void ar_edit_select_word(ar_edit *e, ar_u16 at);
void ar_edit_select_all(ar_edit *e);

int ar_edit_undo(ar_edit *e);
int ar_edit_redo(ar_edit *e);

/* How many steps could be undone right now, and redone. */
ar_u16 ar_edit_undo_depth(const ar_edit *e);
ar_u16 ar_edit_redo_depth(const ar_edit *e);

#endif /* AR_EDIT_H */
