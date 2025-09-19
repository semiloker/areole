#!/usr/bin/env python3
"""The HTML parser's binary size, measured rather than typed.

    python tools/check_size.py            # print the table
    python tools/check_size.py --check    # and fail if a budget is breached

0.9.0 sets two budgets: the whole HTML subsystem, and the named character
reference table separately because it is the one item an application that only
renders its own markup can drop.

Why this exists at all. The figures in the release document were typed by hand
and were 12 KB wrong by the time anyone looked -- the same failure as the
benchmark baseline that never checked it covered its scenes, and as the five
places that quoted a stale conformance number. A budget nothing measures is a
wish.

The objects are the release build's, so run cmake first:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
"""

import argparse
import os
import re
import subprocess
import sys

# The files that make up the HTML subsystem. Everything else in src/ predates
# 0.9.0 and is measured by nothing here.
OBJECTS = [
    "ar_html_entity.c",
    "ar_html_tree.c",
    "ar_html_token.c",
    "ar_encoding.c",
    "ar_dom.c",
    "ar_ua_css.c",
]

# Set in docs/roadmap/0.9.0-html-parser.md. Change them there and here in the
# same commit, with the reason, or not at all.
#
# 90 KB -> 112 KB -> 144 KB, and both raises have the same shape: the figure
# was written for a smaller release than the one that shipped. 90 KB was set
# when 0.9.0 was "tokenizer, tree construction, DOM", before foreign content
# and template contents were in it. 112 KB was set while acceptance criterion 2
# -- html5lib tree construction at 98% -- was explicitly deferred to 0.9.3, in
# those words, in the coverage document. That release's content is now in this
# one: fragment parsing, both select modes, the column group, after after
# frameset, Noah's Ark and about twenty-five insertion-mode rules.
#
# ar_html_tree.c is 47 KB of the 26 KB growth on its own. It is not a fatter
# implementation of the same thing; it is §13.2.6 with the third of it that was
# going to be a separate release added.
#
# Raising a published number twice needs an argument that can be checked, and
# this one can: the roadmap says in writing which release criterion 2 belonged
# to when the 112 KB was set.
#
# 144 KB -> 150 KB at 0.9.3, and this raise has a weaker argument than the last
# two, so it is worth being plain about it.
#
# 0.9.2 was cut at 147,412 of 147,456: forty-four bytes of headroom, which is
# not headroom, it is a coincidence. 0.9.3's remaining insertion-mode rules --
# the stack of template insertion modes, foster parenting stopping at a
# template, the in-table ignore list, `</button>`, the cell's row-group end
# tags, the column group's split character token, the duplicate `<a>` coming
# off the open stack -- cost 1,472 bytes between them and put it 1,428 over.
#
# 0.9.3's own document does allocate "under 6 KB", but honesty about what that
# figure was for: it was written for the fragment parsing algorithm and CDATA,
# and both of those shipped inside 0.9.0 instead, where they are already
# counted in the 144 KB. So this is not drawing on an allowance that was set
# aside; it is a new one, needed because the half of 0.9.3 that looked like the
# small half turned out to have a size of its own.
#
# 150 KB against 148,884 measured: 4,716 bytes, which is meant to cover the
# rest of 0.9.3 and nothing after it. Four of the thirteen failures left are
# `<selectedcontent>`, which is an element that mirrors another element's
# content and will not be free.
TOTAL_BUDGET = 150 * 1024
ENTITY_BUDGET = 30 * 1024
ENTITY_OBJECT = "ar_html_entity.c"

# The CSS subsystem, added at 0.4.2 -- and added because that release found the
# gap. Its document sets a 14 KB budget for the release, `@media` alone cost
# 8.7 KB of it, and the figure above measures *the HTML files only*: ar_css.c
# was never in it, so a CSS release could have spent any amount and passed the
# only size gate there was. A budget nothing measures is the thing this file
# exists to prevent, and it had one.
#
# 60 KB against 56,504 measured at 0.4.2's close -- ar_css.c 52,400 and
# ar_ua_css.c 4,104. Just under four kilobytes of headroom, which was meant to
# be uncomfortable, and was: 0.4.1's units spent 2,152 of it and 0.4.3's
# `calc()` went 1,484 past the line.
#
# **Raised to 80 KB, which is 0.4.3's own allowance and not a round number.**
# Its document asks for 20 KB -- 8 of that the fixed-point maths primitives it
# shares with 0.17.0 -- and 60 + 20 is where that lands. `calc()` and the five
# maths functions cost 4,332 of it; custom properties and `var()` are the rest
# of the release and have not been written yet.
#
# The figure this replaces said the mistake to avoid is "a budget with the next
# two releases already inside it". This is one release's worth, stated by that
# release, and 0.4.4's colour work must move it again and say so -- which is
# the whole point of the gate firing here rather than after the fact. It fired
# on the commit that spent the money, which is the first time either CSS or
# HTML budget has done that.
#
# **Raised to 96 KB at 0.4.4, which is 80 plus that release's stated 16.**
# Said in advance by the line above -- "0.4.4's colour work must move it again
# and say so" -- and this is the saying.
#
# The arithmetic is worth writing down because two different numbers are true
# at once. Colour *spent* 24,064 bytes: ar_color.c is 20,281 of which 4,608 is
# the three generated tables, and the notations cost ar_css.c another 20,567
# between them. That is half again what 0.4.4's document allows. But the budget
# only has to move by 16 KB, because 0.4.3 closed at 66,576 against an 80 KB
# line and left 13,744 unspent -- so the release overran its own allowance and
# still lands inside the number its roadmap named, at 90,640 of 98,304.
#
# Both halves go here rather than only the comfortable one. A release that
# overspends and passes because its predecessor underspent has still
# overspent, and the next one to look at this file should see that rather than
# the 7,664 bytes of headroom it appears to inherit.
#
# The named place to get it back: the notations dispatch on their own name
# through a chain of a dozen ar__same_fold calls, where every other table in
# ar_css.c -- properties, keywords, units -- is a sorted array and a loop. That
# is the shape this file already has twice and the conversion was not done
# here only because the release ran out of room to do it in. It has not been
# measured, so it is a lead and not a promise.
CSS_OBJECTS = [
    "ar_css.c",
    "ar_ua_css.c",
    "ar_color.c",
]
CSS_BUDGET = 96 * 1024

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def object_path(build, name):
    return os.path.join(build, "CMakeFiles", "areole.dir", "src", name + ".obj")


def measure(path):
    """text + data + bss, the way `size` reports it."""
    out = subprocess.run(["size", path], capture_output=True, text=True)
    if out.returncode != 0:
        raise SystemExit("size failed on %s:\n%s" % (path, out.stderr.strip()))
    last = [l for l in out.stdout.splitlines() if l.strip()][-1]
    nums = re.findall(r"\d+", last)
    if len(nums) < 3:
        raise SystemExit("could not read `size` output for %s: %r" % (path, last))
    return sum(int(n) for n in nums[:3])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(HERE, "build"))
    ap.add_argument("--check", action="store_true", help="exit non-zero past a budget")
    args = ap.parse_args()

    rows = []
    missing = []
    for name in OBJECTS:
        path = object_path(args.build, name)
        if not os.path.exists(path):
            missing.append(path)
            continue
        rows.append((name, measure(path)))

    if missing:
        raise SystemExit(
            "no release objects to measure; build first.\n  missing: %s" % missing[0]
        )

    total = sum(n for _, n in rows)
    entity = dict(rows).get(ENTITY_OBJECT, 0)

    css_rows = [(n, measure(object_path(args.build, n))) for n in CSS_OBJECTS]
    css_total = sum(n for _, n in css_rows)

    width = max(len(n) for n, _ in rows)
    for name, n in sorted(rows, key=lambda r: -r[1]):
        print("  %-*s %8d" % (width, name, n))
    print("  %-*s %8d" % (width, "total", total))
    print()
    print(
        "  total  %8d of %d  (%+d)" % (total, TOTAL_BUDGET, TOTAL_BUDGET - total)
    )
    print(
        "  entity %8d of %d  (%+d)"
        % (entity, ENTITY_BUDGET, ENTITY_BUDGET - entity)
    )
    print(
        "  css    %8d of %d  (%+d)"
        % (css_total, CSS_BUDGET, CSS_BUDGET - css_total)
    )

    if not args.check:
        return 0

    bad = 0
    if total > TOTAL_BUDGET:
        print("\nFAIL: the HTML subsystem is %d bytes over its budget" % (total - TOTAL_BUDGET))
        bad = 1
    if entity > ENTITY_BUDGET:
        print("\nFAIL: the entity table is %d bytes over its budget" % (entity - ENTITY_BUDGET))
        bad = 1
    if css_total > CSS_BUDGET:
        print("\nFAIL: the CSS subsystem is %d bytes over its budget" % (css_total - CSS_BUDGET))
        bad = 1
    if not bad:
        print("\nall three budgets met")
    return bad


if __name__ == "__main__":
    sys.exit(main())
