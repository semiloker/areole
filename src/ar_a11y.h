/*
 * ar_a11y.h -- role, name and state, computed from the document.
 *
 * The core computes an accessibility tree; a backend adapts it to MSAA, UI
 * Automation, AT-SPI or NSAccessibility. That split is deliberate and it runs
 * in one direction: the core never learns what an IAccessible is, and the
 * backend never has to work out what a `<summary>` announces as.
 *
 * What the core owes is a stable tree with correct names, and the accessible
 * name is where correctness actually lives -- see the header comment in
 * ar_a11y.c for why the order of the algorithm is the whole of it.
 */
#ifndef AR_A11Y_H
#define AR_A11Y_H

#include "ar_html.h"

/* The roles and states are public now -- AR_ROLE_* and AR_A11Y_* live in
   areole.h, because a backend adapting the tree has to be able to name them. */

/* The role of an element: `role=` if it has one, otherwise the tag, and for
   `<input>` the `type` -- because a checkbox and a text field share a tag and
   share nothing else. */
ar_u8 ar_a11y_role(const ar_doc *d, ar_i32 node);

/*
 * The accessible name, written into `buf` and returned by length.
 *
 * aria-labelledby, then aria-label, then a `<label>`, then the element's own
 * content, then `alt`/`value`/`title`. Skipping a step is how a button
 * labelled by an icon reads as its filename.
 */
ar_u32 ar_a11y_name(const ar_doc *d, ar_i32 node, char *buf, ar_u32 cap);

/* The reader's states, from the box's state bits plus whether it has focus. */
ar_u32 ar_a11y_state(ar_u32 box_state, int focused);

/* An attribute by name, or an empty span. Shared with ar_dom.c's own lookup in
   spirit; separate because this file is meant to be readable on its own. */
ar_span ar_a11y_attr(const ar_doc *d, ar_i32 node, const char *name);

/* Compare a span against a literal, for the bisection in ar_a11y.c. */
int ar_span_cmp(ar_span a, const char *lit);

#endif /* AR_A11Y_H */
