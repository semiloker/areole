#!/usr/bin/env python3
"""areole beside a browser, page after page, in one folder to look through.

    python tools/comparison.py              every situation below
    python tools/comparison.py forms rfc    just these

Each situation is one page at one window size, run through tools/versus.py:
the same file in areole and in Edge, with the fonts Edge uses on Windows. The
results go to docs/comparison/<name>/ -- areole.png, edge.png, versus.png with
the two side by side and every differing pixel in red, report.txt -- and
docs/comparison/index.html shows them all with their numbers.

Like the gallery's renders, the pictures are regenerated on demand and not
committed: docs/comparison/ is in .gitignore. The pages are the repository's
own, so anyone can make the same folder.

The gallery is the gate, one feature to a page with one font shipped beside it.
This is the other question -- what whole pages look like, from a form with no
stylesheet to a saved Wikipedia article -- and it is a picture to look at, not
a number to gate on: two font rasterizers never agree bit for bit.
"""
import html
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'docs', 'comparison')

# name, page, window, what the page is a test of
SITUATIONS = [
    ('forms', 'examples/16_forms/form.html', '800x900',
     'twenty form controls and no stylesheet at all: the user-agent sheet alone'),
    ('document', 'examples/13_document/document.html', '800x600',
     'a short document with a small stylesheet'),
    ('interface', 'examples/14_interface/page.html', '800x600',
     'an application screen written in HTML and CSS'),
    ('rfc', 'examples/15_real/rfc2616.html', '1000x1200',
     'a long plain-text RFC: preformatted text and nothing else'),
    ('whatwg', 'examples/15_real/whatwg-syntax.html', '1000x1200',
     'the HTML standard: headings, prose, code, no author CSS'),
    ('w3c-tables', 'examples/15_real/w3c-css21-tables.html', '1000x1200',
     'a W3C recommendation: tables and figures, no author CSS'),
    ('wikipedia', 'examples/15_real/wikipedia-css.html', '1000x1200',
     'a Wikipedia article with its own stylesheets inlined'),
    ('wikipedia-tables', 'examples/15_real/wikipedia-status-codes.html', '1000x1200',
     'a Wikipedia article that is mostly tables'),
    ('wikipedia-ja', 'examples/15_real/wikipedia-ja-html.html', '1000x1200',
     'Japanese Wikipedia: CJK text, for which neither engine has the same font'),
    ('mdn-flex', 'examples/15_real/mdn-flex.html', '1000x1200',
     'MDN on flexbox: a modern site\'s stylesheet'),
    ('mdn-table', 'examples/15_real/mdn-table.html', '1000x1200',
     'MDN on <table>'),
    ('nasa', 'examples/15_real/nasa.html', '1000x1200',
     'nasa.gov: a heavily designed landing page'),
    ('weather', 'examples/15_real/weather-gov.html', '1000x1200',
     'weather.gov: an older government layout'),
]


def run(name, page, size):
    out = os.path.join(OUT, name)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'versus.py'),
                        os.path.join(ROOT, page), '--size', size, '--out', out],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    report = os.path.join(out, 'report.txt')
    text = io.open(report, encoding='utf-8').read() if os.path.exists(report) else r.stdout
    pixels = re.search(r'pixels that differ: \d+ of \d+ \(([\d.]+)%\)', text)
    elems = re.search(r'(\d+) of (\d+) elements differ', text)
    return {
        'pixels': float(pixels.group(1)) if pixels else None,
        'elements': (int(elems.group(1)), int(elems.group(2))) if elems else None,
        'ok': os.path.exists(os.path.join(out, 'versus.png')),
    }


def index(rows):
    cards = []
    for name, page, size, what, res in rows:
        px = '%.2f%% of pixels differ' % res['pixels'] if res['pixels'] is not None else 'no picture'
        if res['elements'] and res['elements'][1]:
            el = '%d of %d elements with an id within a pixel' % (
                res['elements'][1] - res['elements'][0], res['elements'][1])
        else:
            el = 'no elements with an id to compare'
        img = ('<a href="%s/versus.png"><img src="%s/versus.png" alt="%s"></a>' % (name, name, name)
               if res['ok'] else '<p class="none">versus.py made no picture -- see report.txt</p>')
        cards.append("""<section>
<h2>%s <small>%s at %s</small></h2>
<p>%s</p>
<p class="nums">%s &middot; %s &middot;
<a href="%s/areole.png">areole</a> &middot; <a href="%s/edge.png">Edge</a> &middot;
<a href="%s/report.txt">report</a></p>
%s
</section>""" % (html.escape(name), html.escape(page), size, html.escape(what), px, el, name, name,
                 name, img))
    return """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>areole against Edge</title>
<style>
:root { --bg:#fafafa; --fg:#1d1d1f; --muted:#6b6b70; --card:#fff; --line:#e3e3e6; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#141416; --fg:#ececf0; --muted:#9a9aa2; --card:#1d1d21; --line:#2c2c32; }
}
body { margin:0; padding:16px; background:var(--bg); color:var(--fg);
       font:15px/1.5 system-ui, sans-serif; }
header, section { max-width:1600px; margin:0 auto 24px; }
section { background:var(--card); border:1px solid var(--line); border-radius:8px; padding:16px; }
h1 { font-size:22px; margin:8px 0; } h2 { font-size:18px; margin:0 0 4px; }
small, .nums { color:var(--muted); font-weight:normal; }
img { width:100%%; height:auto; border:1px solid var(--line); }
</style></head><body>
<header><h1>areole against Edge</h1>
<p>Each picture is three panels: what areole drew, what Edge drew from the same file at the same
size with the same fonts, and every pixel that differs in red. Generated by
<code>tools/comparison.py</code>.</p></header>
%s
</body></html>
""" % '\n'.join(cards)


def main():
    want = set(sys.argv[1:])
    rows = []
    for name, page, size, what in SITUATIONS:
        if want and name not in want:
            continue
        print('%-18s %s at %s ...' % (name, page, size), flush=True)
        res = run(name, page, size)
        rows.append((name, page, size, what, res))
        print('   %s pixels differ, elements %s' % (
            '%.2f%%' % res['pixels'] if res['pixels'] is not None else '-',
            '%d/%d off' % res['elements'] if res['elements'] else '-'), flush=True)
    if not want:
        os.makedirs(OUT, exist_ok=True)
        io.open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8').write(index(rows))
        print('open', os.path.join(OUT, 'index.html'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
