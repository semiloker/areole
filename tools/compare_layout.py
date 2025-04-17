#!/usr/bin/env python3
"""Compare areole's layout against a browser's, box by box.

    python tools/compare_layout.py --run ./build/example_tour.exe

which runs both sides itself: `--dump` for areole, and a headless browser on
examples/02_tour/tour.html for the other. With two files instead, it compares
dumps that already exist:

    python tools/compare_layout.py areole.txt browser.txt

Both files are the same format: a header line, then `# page <id>` sections, then
one line per box:

    <path> <x> <y> <w> <h> |<text>

Boxes are matched on their path through the tree, which is the one identifier
the two engines genuinely share -- neither class names nor source order survive
the trip intact, but "second child of the third child of the root" does.

Two kinds of disagreement are reported separately, because they have different
causes and different fixes:

  GEOMETRY   the same box in the same place got a different rectangle. This is
             the flex solver disagreeing, and is the number that matters.

  TEXT       the strings differ, so the boxes are not comparable at all. Almost
             always means the HTML twin has drifted from the C source.

A box whose width is decided by its own text is flagged rather than counted as
a failure, because the two engines measure text with different rasterizers and
never will agree exactly. Those are reported as a spread, not a verdict.
"""

import os
import re
import subprocess
import sys
import tempfile
from html import unescape

# Tried in order. Any Chromium will do; the comparison is against a browser's
# flex solver, not against one vendor's.
BROWSERS = [
    r'C:\Program Files\Google\Chrome\Application\chrome.exe',
    r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
    r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
    '/usr/bin/chromium', '/usr/bin/google-chrome',
]

HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    '..', 'examples', '02_tour', 'tour.html')


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    return None


def run_browser(browser, html_path, out_path):
    """Load the page headless and recover what its Dump button produced.

    The result is read out of the textarea's serialized child text rather than
    its `value`: `value` is a property and never appears in the markup, which
    is why the page writes both.
    """
    url = 'file:///' + os.path.abspath(html_path).replace('\\', '/')
    dom = subprocess.run(
        [browser, '--headless', '--disable-gpu', '--no-sandbox',
         '--virtual-time-budget=4000', '--dump-dom', url],
        capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    m = re.search(r'<textarea[^>]*id="out"[^>]*>(.*?)</textarea>', dom, re.S)
    if not m:
        raise SystemExit('the page produced no dump -- did its script run?')
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(unescape(m.group(1)))


def load(path):
    """-> [(page_id, {box_path: (x, y, w, h, text)})], in file order."""
    pages = []
    cur = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('# page '):
                cur = (line[len('# page '):].strip(), {})
                pages.append(cur)
                continue
            if line.startswith('#') or not line.strip():
                continue
            if cur is None:
                continue
            head, _, text = line.partition('|')
            parts = head.split()
            if len(parts) != 5:
                continue
            box, x, y, w, h = parts
            cur[1][box] = (int(x), int(y), int(w), int(h), norm(text))
    return pages


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


def main(argv):
    if len(argv) == 3 and argv[1] == '--run':
        browser = find_browser()
        if not browser:
            raise SystemExit('no Chromium found; pass two dump files instead')
        tmp = tempfile.mkdtemp(prefix='areole-layout-')
        a_path = os.path.join(tmp, 'areole.txt')
        b_path = os.path.join(tmp, 'browser.txt')
        with open(a_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(subprocess.run([argv[2], '--dump'], capture_output=True,
                                   text=True, encoding='utf-8',
                                   errors='replace').stdout)
        run_browser(browser, HTML, b_path)
        print('areole  : %s --dump' % argv[2])
        print('browser : %s' % os.path.basename(browser))
        print()
        argv = [argv[0], a_path, b_path]

    if len(argv) != 3:
        print(__doc__)
        return 2

    a_pages = load(argv[1])
    b_pages = load(argv[2])

    a_by_id = dict(a_pages)
    b_by_id = dict(b_pages)

    total = matched = 0
    geom_bad = []
    text_bad = []
    missing = []
    text_sized = []

    for page_id, a_boxes in a_pages:
        b_boxes = b_by_id.get(page_id)
        if b_boxes is None:
            print('page missing from the second file: %s' % page_id)
            continue

        for box, a in sorted(a_boxes.items()):
            total += 1
            b = b_boxes.get(box)
            if b is None:
                missing.append((page_id, box, a[4]))
                continue
            matched += 1

            if a[4] != b[4]:
                text_bad.append((page_id, box, a[4], b[4]))
                continue

            d = tuple(b[i] - a[i] for i in range(4))
            if d == (0, 0, 0, 0):
                continue

            # A box with text of its own and no children is sized by that text,
            # and two rasterizers will not agree. Reported, not failed.
            if a[4]:
                text_sized.append((page_id, box, a, b, d))
            else:
                geom_bad.append((page_id, box, a, b, d))

    extra = 0
    for page_id, b_boxes in b_pages:
        a_boxes = a_by_id.get(page_id, {})
        extra += len(set(b_boxes) - set(a_boxes))

    print('boxes in areole  : %d' % total)
    print('matched by path  : %d' % matched)
    print('only in areole   : %d' % len(missing))
    print('only in browser  : %d' % extra)
    print()

    if text_bad:
        print('TEXT DIFFERS -- the twin has drifted, these are not comparable (%d)'
              % len(text_bad))
        for page_id, box, at, bt in text_bad[:12]:
            print('  %-28s %-12s' % (page_id, box))
            print('      areole  %r' % at[:70])
            print('      browser %r' % bt[:70])
        print()

    if missing:
        print('IN AREOLE, NOT IN THE BROWSER (%d)' % len(missing))
        for page_id, box, t in missing[:12]:
            print('  %-28s %-12s %r' % (page_id, box, t[:50]))
        print()

    print('GEOMETRY, boxes not sized by their own text -- this is the verdict')
    if geom_bad:
        print('  %d disagree' % len(geom_bad))
        for page_id, box, a, b, d in geom_bad[:20]:
            print('  %-28s %-12s areole %4d,%4d %4dx%-4d  browser %4d,%4d %4dx%-4d'
                  '  delta %+d,%+d %+dx%+d'
                  % (page_id, box, a[0], a[1], a[2], a[3],
                     b[0], b[1], b[2], b[3], d[0], d[1], d[2], d[3]))
    else:
        print('  all agree exactly')
    print()

    if text_sized:
        dx = [abs(t[4][0]) for t in text_sized]
        dw = [abs(t[4][2]) for t in text_sized]
        print('TEXT-SIZED BOXES -- expected to differ, two rasterizers (%d)'
              % len(text_sized))
        print('  x offset  max %d px, mean %.1f px' % (max(dx), sum(dx) / len(dx)))
        print('  width     max %d px, mean %.1f px' % (max(dw), sum(dw) / len(dw)))
        worst = sorted(text_sized, key=lambda t: -abs(t[4][2]))[:8]
        for page_id, box, a, b, d in worst:
            print('  %-28s %-12s w %4d vs %4d  (%+d)  %r'
                  % (page_id, box, a[2], b[2], d[2], a[4][:38]))
        print()

    return 1 if (geom_bad or text_bad or missing) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
