#!/usr/bin/env python3
"""Ask a browser and areole the same media queries, and report where they differ.

    python tools/compare_media.py                 # every viewport
    python tools/compare_media.py --gate          # and fail under the threshold

Reads `tests/media_corpus.txt`, runs it through `ar_media_corpus` at each
viewport below and through headless Chrome's `matchMedia` at the same one, and
prints the disagreements.

------------------------------------------------------------------------
Why a browser is the authority here

Because the whole value of a media query is that a stylesheet written for the
web behaves the same way when areole reads it. A suite written alongside the
implementation checks that the implementation does what its author meant; only
the other engine checks whether what its author meant was right. Every bug
0.9.1 found this way was invisible to the tests already in the tree.

The corpus deliberately holds only the questions both engines are answering
the same way -- see tools/gen_media_corpus.py for what is left out and why.
"""

import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from compare_layout import find_browser  # noqa: E402

CORPUS = os.path.join(ROOT, 'tests', 'media_corpus.txt')
ENGINE = os.path.join(ROOT, 'build', 'ar_media_corpus.exe')

# What the *window* is asked to be. The client area is smaller than this and
# the CSS viewport smaller again at a device scale factor above 1 -- a request
# for 800x600 at 1dppx gives 776x508, and 600x600 at 2dppx gives 576x508 CSS
# pixels, not 300x300. So the browser is asked what it actually got and areole
# is told that, which is the difference between measuring the two engines and
# measuring this file. Comparing against the requested size scored 91.99% and
# every single disagreement sat on a boundary.
VIEWPORTS = [
    (800, 600, 1000),
    (400, 900, 1000),
    (1200, 400, 1000),
    (600, 600, 2000),
]

GATE = 90.0

PAGE = """<!doctype html><meta charset="utf-8">
<textarea id="out"></textarea>
<script>
const Q = %s;
const o = [];
o.push('#viewport' + String.fromCharCode(9) + window.innerWidth + ' ' + window.innerHeight +
       ' ' + Math.round(window.devicePixelRatio * 1000));
for (const q of Q) { o.push((window.matchMedia(q).matches ? 1 : 0) + String.fromCharCode(9) + q); }
const t = document.getElementById('out');
t.value = o.join(String.fromCharCode(10)); t.textContent = t.value;
</script>
"""


def queries():
    out = []
    with io.open(CORPUS, encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n').rstrip('\r')
            if line and not line.startswith('#'):
                out.append(line)
    return out


def engine_answers(w, h, res):
    exe = ENGINE if os.path.exists(ENGINE) else ENGINE[:-4]
    if not os.path.exists(exe):
        raise SystemExit('build/ar_media_corpus is not built')
    r = subprocess.run([exe, CORPUS, str(w), str(h), str(res)],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    out = {}
    for line in r.stdout.splitlines():
        if line.startswith('#') or '\t' not in line:
            continue
        val, q = line.split('\t', 1)
        out[q] = val.strip()
    return out


def browser_answers(browser, qs, w, h, res):
    import json
    import tempfile

    tmp = tempfile.mkdtemp(prefix='areole-media-')
    page = os.path.join(tmp, 'mq.html')
    with io.open(page, 'w', encoding='utf-8', newline='\n') as f:
        f.write(PAGE % json.dumps(qs))
    dom = subprocess.run(
        [browser, '--headless', '--disable-gpu', '--no-sandbox',
         '--allow-file-access-from-files',
         '--window-size=%d,%d' % (w, h),
         '--force-device-scale-factor=%s' % (res / 1000.0),
         '--virtual-time-budget=4000', '--dump-dom',
         'file:///' + page.replace('\\', '/')],
        capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    m = re.search(r'<textarea[^>]*id="out"[^>]*>(.*?)</textarea>', dom, re.S)
    if not m:
        return {}
    from html import unescape
    out, seen = {}, None
    for line in unescape(m.group(1)).splitlines():
        if '\t' not in line:
            continue
        val, q = line.split('\t', 1)
        if val == '#viewport':
            seen = tuple(int(x) for x in q.split())
            continue
        out[q] = val.strip()
    return out, seen


def main():
    qs = queries()
    browser = find_browser()
    if not browser:
        print('no browser found: this corpus needs one')
        return 2

    total = 0
    agreed = 0
    for (w, h, res) in VIEWPORTS:
        theirs, seen = browser_answers(browser, qs, w, h, res)
        if not theirs or not seen:
            print('%dx%d @%s: the browser returned nothing -- retry' % (w, h, res))
            return 2
        # Ask areole about the viewport the browser actually had, not the one
        # the window was asked for. A window is not its client area, and at a
        # device scale factor of 2 the CSS viewport is half the pixels -- so
        # comparing against the requested size measures the harness rather
        # than either engine, and every disagreement lands on a boundary.
        w, h, res = seen
        mine = engine_answers(w, h, res)
        bad = []
        for q in qs:
            if q not in theirs or q not in mine:
                continue
            total += 1
            if mine[q] == theirs[q]:
                agreed += 1
            else:
                bad.append((q, mine[q], theirs[q]))
        print('%-14s %3d of %3d agree%s'
              % ('%dx%d @%s' % (w, h, res), len(qs) - len(bad), len(qs),
                 '' if not bad else '   %d differ' % len(bad)))
        for q, a, b in bad[:20]:
            print('    areole %s  chrome %s   %s' % (a, b, q))

    pct = (100.0 * agreed / total) if total else 0.0
    print()
    print('%d of %d agree, %.2f%%' % (agreed, total, pct))
    if '--gate' in sys.argv:
        if total == 0:
            print('FAIL: nothing was compared')
            return 2
        if pct < GATE:
            print('FAIL: the gate is %.0f%%' % GATE)
            return 1
        print('gate is %.0f%%: met' % GATE)
    return 0


if __name__ == '__main__':
    sys.exit(main())
