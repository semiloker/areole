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

/*
 * Roles, and the list is short because a role is only worth having when it
 * changes what a reader says or does.
 *
 * There is no generic "group" for a `<div>`. A reader that announces one for
 * every wrapper on the page buries the three things that mattered under forty
 * that did not, and AR_ROLE_NONE means "walk through me to my children" rather
 * than "I am nothing".
 */
enum
{
    AR_ROLE_NONE = 0,
    AR_ROLE_BUTTON,
    AR_ROLE_LINK,
    AR_ROLE_CHECKBOX,
    AR_ROLE_RADIO,
    AR_ROLE_TEXTBOX,
    AR_ROLE_SEARCHBOX,
    AR_ROLE_SLIDER,
    AR_ROLE_COMBOBOX,
    AR_ROLE_OPTION,
    AR_ROLE_HEADING,
    AR_ROLE_PARAGRAPH,
    AR_ROLE_LIST,
    AR_ROLE_LISTITEM,
    AR_ROLE_TABLE,
    AR_ROLE_ROW,
    AR_ROLE_CELL,
    AR_ROLE_COLUMNHEADER,
    AR_ROLE_IMAGE,
    AR_ROLE_DIALOG,
    AR_ROLE_PROGRESSBAR,
    AR_ROLE_METER,
    AR_ROLE_GROUP,
    AR_ROLE_FORM,
    AR_ROLE_MAIN,
    AR_ROLE_NAVIGATION,
    AR_ROLE_BANNER,
    AR_ROLE_CONTENTINFO,
    AR_ROLE_COMPLEMENTARY,
    AR_ROLE_REGION,
    AR_ROLE_ARTICLE,
    AR_ROLE_COUNT
};

/* The states a reader announces alongside the role and the name. */
enum
{
    AR_A11Y_CHECKED = 1u << 0,
    AR_A11Y_DISABLED = 1u << 1,
    AR_A11Y_EXPANDED = 1u << 2,
    AR_A11Y_FOCUSED = 1u << 3
};

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
