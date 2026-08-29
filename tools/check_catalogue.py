#!/usr/bin/env python3
"""Does the CSS catalogue still claim a shipped property is unbuilt?

    python tools/check_catalogue.py

Reads the property name table out of src/ar_css.c and every status row in
docs/roadmap/support-matrix-css.md, and fails when a property the parser
accepts is still marked planned.

Why this exists
---------------
Six releases shipped without one catalogue row changing status. At 0.8.2 the
file said `subgrid`, `aspect-ratio`, `display: grid`, the flex longhands, the
table display types and every combinator were unbuilt -- months after all of
them shipped. It read 26 shipped against 279 planned for an engine that had
done seventeen of its fifty-four releases.

The rule in the working brief -- *a coverage release is not done until its
catalogue rows change status* -- was already written down. Writing it down was
not enough, so this checks it.

What it does NOT check
----------------------
That a property is *correct*, or complete, or matches a browser. A property can
parse, lay out wrongly, and keep this gate green. It answers exactly one
question: does the catalogue admit the parser accepts this name?

That is a low bar deliberately. The high bar is the browser corpora, and no
script can stand in for them.

It also sees property *names* only, never values. `subgrid`, `grid`,
`contents` and `wrap-reverse` are values of properties, so a row marking any of
them planned stays green here -- which was found by stubbing this gate and
watching it not fail. Widening it means a value table the parser does not keep
in one place, and the honest answer for now is that the row for a value is
still checked by a human.

Not in CI
---------
`docs/roadmap/` is gitignored -- it is planning material, not repository
content -- so a CI runner does not have the file to check. This is a local
gate, run before finishing a coverage release. If the catalogue ever moves into
the repository, wire it into the tidy job.
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS = os.path.join(ROOT, "src", "ar_css.c")
MATRIX = os.path.join(ROOT, "docs", "roadmap", "support-matrix-css.md")

PLANNED = "⬜"
SHIPPED = "✅"
PARTIAL = "\U0001f7e1"
TRACKED = "\U0001f52d"
NEVER = "❌"

# Names the matrix uses for something other than a CSS property, and which
# therefore may be marked planned while the identically named property ships.
# Each one is an @media feature; `width` the media feature and `width` the
# property are different questions with the same spelling.
MEDIA_FEATURES = {"width", "height", "min-width", "max-width", "min-height",
                  "max-height", "aspect-ratio", "color", "orientation"}


def parsed_properties():
    """Every name in the longhand and shorthand tables in src/ar_css.c."""
    with io.open(CSS, encoding="utf-8", errors="replace") as f:
        src = f.read()
    return set(re.findall(r'\{"([a-z-]+)",\s*AR_(?:P|SH)_[A-Z_0-9]+\}', src))


def main():
    # The status glyphs are not in cp1252, which is what a Windows console
    # hands us by default. Without this the tool crashes while reporting the
    # very rows it exists to report.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass

    if not os.path.exists(MATRIX):
        print("no %s -- docs/roadmap is gitignored, so this is expected on a fresh clone"
              % os.path.relpath(MATRIX, ROOT))
        return 0

    parsed = parsed_properties()
    with io.open(MATRIX, encoding="utf-8") as f:
        lines = f.read().split("\n")

    bad = []
    for n, line in enumerate(lines, 1):
        if PLANNED not in line:
            continue
        # A line can carry several statuses. Split after each glyph, so a chunk
        # runs from just past the previous status up to and including the next
        # one, and the names it holds are the ones that status marks.
        #
        # Then drop everything from `--` onward. A row may explain itself in
        # prose after its glyph, and that prose names other properties: the
        # `flow-root` row mentions `overflow`, and the `direction` row mentions
        # `caption-side`. Both are parsed, neither is what the row is marking.
        # This is a heuristic on a hand-written format, and it errs towards
        # missing a stale row rather than towards crying wolf on a correct one.
        for chunk in re.split(r"(?<=[%s%s%s%s%s])" % (PLANNED, SHIPPED, PARTIAL, TRACKED, NEVER),
                              line):
            if not chunk.endswith(PLANNED):
                continue
            marked = chunk.split("--")[0]
            names = [x for x in re.findall(r"`([a-z][a-z0-9-]*)`", marked)
                     if x in parsed and x not in MEDIA_FEATURES]
            for name in names:
                bad.append((n, name, line.strip()))

    counts = dict((g, sum(l.count(g) for l in lines))
                  for g in (SHIPPED, PARTIAL, PLANNED, TRACKED, NEVER))
    print("catalogue: %d shipped, %d partial, %d planned, %d tracked, %d never"
          % (counts[SHIPPED], counts[PARTIAL], counts[PLANNED],
             counts[TRACKED], counts[NEVER]))
    print("parser accepts %d property names" % len(parsed))

    if bad:
        print()
        print("FAIL: %d row(s) mark a property the parser accepts as planned:" % len(bad))
        for n, name, line in bad:
            print("  L%-5d %-24s %s" % (n, name, line[:88]))
        print()
        print("Either the row is stale, or the property parses and does nothing --")
        print("in which case mark it partial and name the limitation, which is what")
        print("the partial glyph is for.")
        return 1

    print("no row claims a parsed property is unbuilt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
