# Example 16 — what 0.10.0 is

0.10.0 is the release where areole stops being a viewer. Everything before it
renders; this is the first version you can *use* — Tab through, type into,
tick, choose from, submit — and the first one a screen reader can read.

This example is one delivery form that exercises every part of the release:
[`form.html`](form.html), an ordinary HTML document with **no stylesheet at
all** -- no `<style>`, no classes, no wrappers on the controls. What it looks
like is entirely the user-agent sheet, which is the point: controls built out
of real boxes and a measured user-agent sheet mean plain markup produces a
usable form, and one that looks like a browser's.

```sh
cmake --build build --target example_forms

./build/example_forms                 # open a window and use it
./build/example_forms --selftest      # drive it without a window: 40 checks
./build/example_forms --dump          # print the accessibility tree
./build/example_forms --html          # print the page -- it is form.html
./build/example_forms --ppm out.ppm   # render it, focused, to an image
./build/example_forms --ppm-open out.ppm  # the same, with the dropdown open

python tools/versus.py examples/16_forms/form.html --size 800x900   # beside Edge
```

`main.c` embeds `form.html` rather than a copy of it: `tools/embed_form.py`
writes the array, and `tools/embed_form.py --check` fails when the two
disagree, so the window, the selftest and the comparison are always looking at
the same page.

## Against a browser

The first version of this example carried a page of CSS, and looked like a
form only because of it; with the sheet taken away it looked nothing like a
browser's. So the sheet went, and the user-agent sheet's controls were measured
against Edge until they matched. `tools/versus.py` is how: it renders
`form.html` in areole and in Edge at the same 800 by 900 -- tall enough that
neither draws a scrollbar, which would take 15 pixels off one side only --
with the fonts Edge itself uses on Windows (Times New Roman, Arial for
controls, Consolas for `<textarea>`), and writes `areole.png`, `edge.png`, a
side-by-side `versus.png` with every differing pixel in red, and `report.txt`
with every element's box in both.

The last run: **59 of the 60 elements with an `id` are within
a pixel of Edge on every edge**, and 3.18% of the pixels differ -- nearly all
of them glyph edges, because two rasterizers draw the same Times New Roman
differently. The one element out is the select, 2 pixels narrower: its width is
its widest option, measured at 13 pixels where Edge measures 13.33, and areole
has no fractional pixel to keep. Most rows below the heading also sit a pixel
higher than Edge's, for the same reason -- the heading's margins are 21.44
pixels each in a browser and 21 here.

It is a picture to look at and not a gate, because two font rasterizers never
agree bit for bit. The gate is `tools/gallery.py`, one feature to a page with
one font shipped beside it.

## What to try, and what it shows

| Do this | What happens | The part of 0.10.0 doing it |
| --- | --- | --- |
| **Tab**, **Shift+Tab** | focus moves in document order and **draws a ring** | focus, tab order, `:focus-visible` |
| **click** a field | it is focused and draws **no** ring | `:focus-visible` — a click knows where it went |
| type into *Name* | the text appears, the caret follows | the edit buffer, the field's text box |
| **←/→**, **Ctrl+←/→**, **Home/End**, **Shift** | caret moves by cluster or by word; Shift selects | grapheme clusters (UAX #29), word stops |
| **Ctrl+A / Z / Y** | select all, undo, redo — a run of typing is one step | the undo log, 512 steps |
| **Ctrl+C / X / V** | the system clipboard | `ar_clipboard_text`, `ar_win_set_clipboard` |
| double / triple click in a field | selects a word / everything | the platform counts clicks, the core selects |
| Tab away from *Name* and back | the text is still there | the value pool |
| *Door PIN* | draws `••••`; a reader hears its length, never the digits | `password` |
| *How many*, **↑/↓** | steps 1–9 and stops at the ends | `number`, `min`, `max`, `step` |
| *Address*, **Enter**, **↑/↓** | a newline; the caret moves by line; the box scrolls | `textarea`, wrapping, the post-layout field pass |
| *Box size*, **Space**, **↑/↓**, **Enter**, **Esc** | opens the list, chooses through the optgroup, closes | `select`, the open list, light dismiss |
| *Tip*, **←/→**, **Home/End**, drag | the slider steps or follows the pointer | `range` |
| *Ribbon colour*, **Enter** | a palette of sixteen; a click on a chip chooses | `color` |
| *Photo of the door*, **Space** | the Windows open-file dialog; the name is shown | `file`, `ar_file_wanted` / `ar_file_chosen` |
| click the words *Wrap it as a gift* | the checkbox ticks | `<label for>` passes its click on |
| click *Email me about offers* | ticks the box inside the label | a wrapping `<label>` |
| Tab into *When*, then **↑/↓** | one Tab stop for the whole group; arrows choose | radio groups |
| *Delivery instructions* | opens and closes; the triangle turns | `details` / `summary` |
| **Enter** in any text field, or *Place order* | the form is submitted to the program, which prints it | `ar_form_submitted`, `ar_form_encode` |
| *Start again* | every control goes back to its markup | `reset` |
| leave the caret in a field | it blinks, and each blink repaints **15 pixels** without building a frame | `ar_caret_wait_us`, `ar_win_idle`, `ar_frame_blink` |
| Ctrl+Win+Enter (Narrator), then Tab | every control should announce role, name and value -- not yet heard by ear | the accessibility tree, MSAA |

## The API an embedder uses

A window with fields in it needs one extra call per frame:

```c
ar_win_a11y(win, ctx, doc);                 /* once: expose the tree to readers */

while (ar_win_pump(win)) {
    ar_frame_begin(ctx, ar_win_input(win));
    ar_dom_build(ctx, doc);
    ar_frame_end(ctx, ar_win_surface(win));
    /* ... present the damage ... */

    ar_i32 by, form = ar_form_submitted(ctx, &by);
    if (form >= 0) {
        char data[8192];
        ar_form_encode(ctx, form, by, data, sizeof data);
        /* name=Ada&size=l&tip=20&...  -- what a server would have been sent */
    }

    ar_win_after_frame(win, ctx);  /* clipboard, IME position, caret blink,
                                      file dialog, accessibility events */
}
```

And one shortcut, which is what makes a blinking caret cost nothing: when
the caret's timer is the only thing that woke the window, there is no input
for a frame to learn from, and the blink is one column of the frame already
on screen.

```c
    if (ar_win_idle(win)) {                     /* the timer, and nothing else */
        ar_win_present(win, ar_frame_blink(ctx, ar_win_surface(win)));
        ar_win_wake_after(win, ar_caret_wait_us(ctx));
        continue;                               /* 3.7 us here, not a frame */
    }
```

`ar_win_after_frame` is five public calls bundled; a program that wants any of
them done differently calls them itself:

| Call | Why the core cannot do it |
| --- | --- |
| `ar_clipboard_text` → `ar_win_set_clipboard` | the clipboard belongs to the platform |
| `ar_caret_rect` → `ar_win_set_caret` | the IME candidate window is the platform's |
| `ar_caret_wait_us` → `ar_win_wake_after` | the core owns no clock and never wakes itself |
| `ar_file_wanted` → dialog → `ar_file_chosen` | the core has no file system |
| `ar_win_a11y_update` | tells readers about focus, value and shape changes |

And the accessibility tree is public, so it can be checked without a screen
reader — which is what `--dump` prints:

```c
ar_a11y_item items[256];
ar_i32 n = ar_a11y_tree(ctx, doc, items, 256);
for (i = 0; i < n; ++i) {
    ar_a11y_name(doc, items[i].node, name, sizeof name);       /* ARIA order */
    ar_a11y_value(ctx, doc, items[i].node, value, sizeof value);
    /* items[i].role, items[i].state, items[i].rect, items[i].parent */
}
```

## How each acceptance criterion is checked

| 0.10.0 criterion | Where it is checked |
| --- | --- |
| 1. every control renders, responds, reports its value | this selftest, per control; `ar_test` per behaviour; `--ppm` / `--ppm-open` by eye |
| 2. caret never lands mid-cluster, 200 strings | `ar_test`: `test_the_caret_never_lands_inside_a_cluster` |
| 3. 500-step undo/redo restores every step | `ar_test`: `test_undo_and_redo_over_five_hundred_steps` |
| 4. IME inline composition, candidate at the caret | core: `ar_test` (composition drawn and underlined, never in the buffer); Win32 handling written — **hand verification with a Japanese IME is still owed** |
| 5. tab order is document order, unaffected by `order` | `ar_test`: `test_tab_order_ignores_flex_order` |
| 6. a 20-control form names every control | this selftest ("twenty controls, every one named"); `ar_a11y_probe` reads the window back from another process through MSAA and native UI Automation and finds the twenty, every one named — **Narrator and NVDA by ear are still owed** |
| 7. a blink invalidates < 2,000 pixels | this selftest: 15 pixels; `ar_test` checks it too |
| 8. no allocation after init, undo log included | `ar_bench` checks it on every scene; the three written for this release -- a keystroke, a blink, the tree -- allocate nothing either, and land in the repository with the version stamp and the baseline that holds them |

**The probe's first run found that no reader had ever seen anything in the
window.** MinGW's import library hands back the address of an import thunk for
`IID_IAccessible`, so the provider marshalled an interface nobody had
registered and every reader got `REGDB_E_IIDNOTREG`. It also found the form
named with every word inside it, the fieldset with all three radios' labels,
and the closed `<details>` with the sentence it hides. All four are fixed, and
`ar_test` holds the naming rules; the probe is run by hand, because it opens a
window:

```sh
cmake --build build --target ar_a11y_probe example_forms
cd build && ./ar_a11y_probe        # exits 0 when twenty controls are all named
```

## What 0.10.0 does not do

Stated here rather than discovered:

- **No network.** A form is submitted *to the program*; there is no POST.
- **No date, time or datetime-local pickers** — they need a calendar and a
  locale database. Those types are text fields.
- **No `<select multiple>`, no `size` list boxes.** A select is a dropdown.
- **No colour dialog** — the colour field offers sixteen colours.
- **No `contenteditable`, no rich text, no spellcheck.**
- **Undo does not survive leaving a field** — one edit buffer, by design; the
  text survives, the history does not.
- **Bidirectional text moves the caret logically**, not visually.
- **IME and screen readers are not yet verified by hand** (see the table
  above). The tree has been read back by a program; whether Narrator and NVDA
  *say* it well, and whether a Japanese IME composes in place, needs a person,
  and cannot be settled by reasoning.
