#!/usr/bin/env python3
"""Reconcile the catalogue against the gallery, both ways.

    python tools/check_gallery.py            list what is missing
    python tools/check_gallery.py --check    and exit non-zero if anything is

0.9.1's fifth acceptance criterion, and the mechanism the catalogue was
designed around: **the demo id is a column in the matrix and the path of the
file**, so the two can be compared and neither can drift alone.

Two failures, and they are different failures.

  A **row marked shipped with no demo file** is the catalogue claiming
  something nothing checks. That is the one this exists for: the matrix went
  six releases with rows that had gone stale in both directions, and the note
  at the top of `support-matrix-css.md` is the record of finding out.

  A **demo file with no row** is a demo nobody will maintain, because nothing
  points at it. Reported, and not a failure: the gallery may reasonably hold a
  case that is about the engine rather than about a catalogue entry -- the ones
  that found bugs did.

Only rows marked ✅ are required to have a demo. A 🟡 row is partial and names
its limitation; a ⬜ row is not built yet and a demo would be red on purpose.
Requiring those would make the check red for the two states the catalogue uses
to be honest about what is missing.
"""

import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEMOS = os.path.join(ROOT, 'examples', 'gallery')
MATRICES = ('docs/roadmap/support-matrix-css.md',
            'docs/roadmap/support-matrix-html.md')

# `## Selectors · demos `css/selectors/`` -- the prefix every id in the section
# hangs off.
SECTION = re.compile(r'^##+ .*demos\s+`([^`]+)`')


def rows():
    """-> [(demo_id, status, feature, matrix)] for every row that has an id."""
    out = []
    for name in MATRICES:
        path = os.path.join(ROOT, name.replace('/', os.sep))
        if not os.path.exists(path):
            continue
        prefix = ''
        with io.open(path, encoding='utf-8') as f:
            for line in f:
                m = SECTION.match(line)
                if m:
                    prefix = m.group(1)
                    continue
                if not line.startswith('|'):
                    continue
                cells = [c.strip() for c in line.strip().strip('|').split('|')]
                if len(cells) < 4 or cells[1] not in ('✅', '🟡', '⬜', '🔭', '❌'):
                    continue
                demo = cells[-1].strip('`').strip()
                if not demo or ' ' in demo:
                    continue
                out.append((prefix + demo, cells[1], cells[0], name))
    return out


def files():
    """-> {demo_id} for every .html under examples/gallery."""
    out = set()
    for base, _, names in os.walk(DEMOS):
        if os.path.basename(base).startswith('_'):
            continue
        for n in names:
            if n.endswith('.html') and not n.startswith('_') and n != 'index.html':
                rel = os.path.relpath(os.path.join(base, n), DEMOS)
                out.add(rel.replace(os.sep, '/')[:-len('.html')])
    return out


def main(argv):
    strict = '--check' in argv
    have = files()
    all_rows = rows()
    shipped = [r for r in all_rows if r[1] == '✅']
    claimed = set(r[0] for r in all_rows)

    missing = [r for r in shipped if r[0] not in have]
    orphans = sorted(have - claimed)

    print('catalogue rows with a demo id : %d' % len(all_rows))
    print('  of those, marked shipped    : %d' % len(shipped))
    print('demo files                    : %d' % len(have))
    print()

    if missing:
        print('SHIPPED, WITH NO DEMO (%d)' % len(missing))
        for demo, _, feature, _ in missing:
            print('  %-40s %s' % (demo, feature[:60]))
        print()

    if orphans:
        print('DEMOS WITH NO CATALOGUE ROW (%d) -- reported, not a failure' % len(orphans))
        for demo in orphans:
            print('  %s' % demo)
        print()

    done = len(shipped) - len(missing)
    print('%d of %d shipped rows have a demo' % (done, len(shipped)))
    return 1 if (strict and missing) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
