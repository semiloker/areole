#!/usr/bin/env python3
"""Compare areole's HTML tree against a browser's, case by case.

    python tools/compare_trees.py --run ./build/example_html.exe \
           examples/12_html/cases.html

which runs both sides itself: `--dump` for areole, and a headless browser on
the generated twin for the other.

With two files instead, it compares dumps that already exist:

    python tools/compare_trees.py areole.txt browser.txt

Both files are the same format: a header line, then `# case <id>` sections,
each followed by one line holding the tree:

    # case p-closes-p
    html(head body(p(#) p(#)))

Element names, `#` for a text node, `!` for a comment. The doctype is left out
because neither side renders it.

Why trees rather than rectangles
--------------------------------
compare_layout.py compares geometry, because every other corpus here is about
layout. This one is about the parser, and comparing rectangles would measure
areole's user-agent stylesheet against a browser's -- a different and much
later question.

A whole tree per case, rather than a property at a time, for the reason the
grid corpus learned the hard way: a check that asks whether one substring
precedes another accepts almost anything.
"""

import os
import re
import subprocess
import sys
from html import unescape

BROWSERS = [
    r'C:\Program Files\Google\Chrome\Application\chrome.exe',
    r'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
    r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    r'C:\Program Files\Microsoft\Edge\Application\msedge.exe',
    '/usr/bin/chromium', '/usr/bin/google-chrome',
]


def find_browser():
    for b in BROWSERS:
        if os.path.exists(b):
            return b
    return None


def run_browser(browser, html_path, out_path):
    """Load the page headless and recover what its script produced.

    Read out of the textarea's serialized child text rather than its `value`:
    `value` is a property and never appears in the markup, which is why the
    page writes both.
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
    cases = {}
    order = []
    cur = None
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n').rstrip('\r')
            if line.startswith('# case '):
                cur = line[len('# case '):].strip()
                cases[cur] = ''
                order.append(cur)
                continue
            if line.startswith('#'):
                continue
            if cur is not None and not cases[cur]:
                cases[cur] = line.strip()
    return cases, order


def main(argv):
    if len(argv) in (3, 4) and argv[1] == '--run':
        browser = find_browser()
        if not browser:
            raise SystemExit('no Chromium found; pass two dump files instead')
        import tempfile
        tmp = tempfile.mkdtemp(prefix='areole-trees-')
        a_path = os.path.join(tmp, 'areole.txt')
        b_path = os.path.join(tmp, 'browser.txt')
        with open(a_path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(subprocess.run([argv[2], '--dump'], capture_output=True,
                                   text=True, encoding='utf-8',
                                   errors='replace').stdout)
        html = argv[3] if len(argv) == 4 else 'examples/12_html/cases.html'
        run_browser(browser, html, b_path)
        print('areole  : %s --dump' % argv[2])
        print('browser : %s' % os.path.basename(browser))
        print()
        argv = [argv[0], a_path, b_path]

    if len(argv) != 3:
        print(__doc__)
        return 2

    a, order = load(argv[1])
    b, _ = load(argv[2])

    missing = [k for k in order if k not in b]
    differ = [k for k in order if k in b and a[k] != b[k]]

    print('cases in areole  : %d' % len(order))
    print('matched by name  : %d' % (len(order) - len(missing)))
    print('only in areole   : %d' % len(missing))
    print()

    if missing:
        print('NOT IN THE BROWSER DUMP (%d)' % len(missing))
        for k in missing:
            print('  %s' % k)
        print()

    if not differ:
        print('TREES')
        print('  all %d agree exactly' % (len(order) - len(missing)))
        return 0

    print('TREES, %d of %d disagree' % (differ.__len__(), len(order) - len(missing)))
    for k in differ:
        print('  %s' % k)
        print('    areole  %s' % a[k])
        print('    browser %s' % b[k])
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
