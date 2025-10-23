#!/usr/bin/env python3
"""One page, two engines, side by side: areole against a real browser.

    python tools/versus.py examples/16_forms/form.html
    python tools/versus.py page.html --size 800x900 --out build/versus/form

Both engines are handed **the same file** and the same window size, with the
fonts the browser itself uses by default on Windows -- Times New Roman for text,
Arial for controls, Consolas for monospace -- so that what differs is the
engine and not which font each one found.

Writes three pictures and a report:

  areole.png    what areole drew
  edge.png      what the browser drew
  versus.png    the two side by side, and a third panel where every pixel
                that differs is red
  report.txt    every element with an `id`, its box in each engine, and the
                ones that disagree by more than a pixel

The gallery (tools/gallery.py) is the gate: one feature per file, one font
shipped with it, geometry at a pixel. This is the other question -- does a
whole page with *no stylesheet at all* look like a browser's -- and it is a
picture to look at rather than a number to gate on, because two font
rasterizers never agree bit for bit.
"""

import argparse
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from html import unescape

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from compare_layout import find_browser  # noqa: E402
from gallery import read_png  # noqa: E402

ENGINE = os.path.join(ROOT, 'build', 'ar_gallery.exe')
FONTS = 'C:/Windows/Fonts/'
DEFAULT_FONTS = {'body': FONTS + 'times.ttf', 'bold': FONTS + 'timesbd.ttf',
                 'italic': FONTS + 'timesi.ttf',
                 'sans': FONTS + 'arial.ttf', 'mono': FONTS + 'consola.ttf'}

# The page goes in an iframe and is measured from outside, so the page itself
# carries no script and is exactly the file areole is given.
WRAPPER = """<!doctype html><meta charset="utf-8">
<style>html,body{margin:0;padding:0}
iframe{width:%dpx;height:%dpx;border:0;display:block}</style>
<iframe id="f" src="%s"></iframe>
<textarea id="out"></textarea>
<script>
function dump(){
  const out = [];
  try {
    const d = document.getElementById('f').contentDocument;
    /* The screenshot is taken with --hide-scrollbars, and areole's bar is an
       overlay that takes no room; a page taller than the window measured
       here with a 15 px scrollbar disagreed with its own picture. */
    const s = d.createElement('style');
    s.textContent = 'html{scrollbar-width:none}';
    (d.head || d.documentElement).appendChild(s);
    for (const e of d.querySelectorAll('[id]')) {
      const r = e.getBoundingClientRect();
      out.push('#' + e.id + ' ' + Math.round(r.left) + ' ' + Math.round(r.top) + ' ' +
               Math.round(r.width) + ' ' + Math.round(r.height));
    }
  } catch (e) { out.push('# blocked ' + e); }
  const t = document.getElementById('out');
  t.value = out.join('\\n'); t.textContent = out.join('\\n');
}
window.addEventListener('load', dump);
</script>
"""


def parse_boxes(text):
    boxes = {}
    for line in text.splitlines():
        if not line.startswith('#') or line.startswith('# '):
            continue
        p = line.split()
        if len(p) == 5:
            boxes[p[0][1:]] = tuple(int(v) for v in p[1:])
    return boxes


def areole(page, w, h, fonts, out_ppm):
    args = [ENGINE, page, '--size', '%dx%d' % (w, h), '--ids', '--ppm', out_ppm]
    if fonts.get('body'):
        args += ['--font', fonts['body']]
    if fonts.get('sans'):
        args += ['--sans', fonts['sans']]
    if fonts.get('mono'):
        args += ['--mono', fonts['mono']]
    if fonts.get('bold'):
        args += ['--bold', fonts['bold']]
    if fonts.get('italic'):
        args += ['--italic', fonts['italic']]
    r = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace')
    return parse_boxes(r.stdout)


def browser_run(browser, args):
    """Run the browser with a timeout, so a hung headless instance cannot hang
    the tool -- which is the failure tools/gallery.py documents."""
    # Light, always. A page that offers `color-scheme: light dark` follows the
    # system theme in a browser, and on a machine in dark mode the WHATWG
    # standard came out white on black -- 97% of its pixels "different" --
    # while areole's prefers-color-scheme is pinned to light until 0.16.1.
    try:
        return subprocess.run([browser, '--headless', '--disable-gpu', '--no-sandbox',
                               '--blink-settings=preferredColorScheme=1'] + args,
                              capture_output=True, text=True, encoding='utf-8', errors='replace',
                              timeout=60).stdout
    except subprocess.TimeoutExpired:
        return ''


def browser(page, w, h, out_png):
    exe = find_browser()
    if not exe:
        raise SystemExit('no browser found')
    src = 'file:///' + os.path.abspath(page).replace('\\', '/')
    browser_run(exe, ['--hide-scrollbars', '--window-size=%d,%d' % (w, h),
                      '--virtual-time-budget=3000', '--screenshot=' + out_png, src])
    tmp = tempfile.mkdtemp(prefix='areole-versus-')
    wrap = os.path.join(tmp, 'wrap.html')
    with io.open(wrap, 'w', encoding='utf-8', newline='\n') as f:
        f.write(WRAPPER % (w, h, src))
    dom = browser_run(exe, ['--allow-file-access-from-files', '--window-size=%d,%d' % (w, h),
                            '--virtual-time-budget=3000', '--dump-dom',
                            'file:///' + wrap.replace('\\', '/')])
    m = re.search(r'<textarea[^>]*id="out"[^>]*>(.*?)</textarea>', dom, re.S)
    return parse_boxes(unescape(m.group(1))) if m else {}


def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts = data.split(b'\n', 3)
    w, h = (int(v) for v in parts[1].split())
    return w, h, parts[3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        c = struct.pack('>I', len(body)) + kind + body
        return c + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 6)))
        f.write(chunk(b'IEND', b''))


def side_by_side(a, b, w, h):
    """areole | browser | difference, with a four-pixel gutter between them."""
    gap = 4
    out_w = w * 3 + gap * 2
    rows = []
    differ = 0
    for y in range(h):
        ra = a[y * w * 3:(y + 1) * w * 3]
        rb = b[y * w * 3:(y + 1) * w * 3]
        diff = bytearray(w * 3)
        for x in range(w):
            i = x * 3
            if abs(ra[i] - rb[i]) + abs(ra[i + 1] - rb[i + 1]) + abs(ra[i + 2] - rb[i + 2]) > 48:
                diff[i:i + 3] = b'\xe0\x20\x20'
                differ += 1
            else:
                v = 230 + (ra[i] + ra[i + 1] + ra[i + 2]) // 30
                diff[i:i + 3] = bytes((min(v, 255),) * 3)
        rows.append(bytes(ra) + b'\x80' * (gap * 3) + bytes(rb) + b'\x80' * (gap * 3) + bytes(diff))
    return out_w, b''.join(rows), differ


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('page')
    ap.add_argument('--size', default='800x600')
    ap.add_argument('--out', default=None)
    ap.add_argument('--tolerance', type=int, default=1)
    args = ap.parse_args()

    w, h = (int(v) for v in args.size.split('x'))
    name = os.path.splitext(os.path.basename(args.page))[0]
    out = args.out or os.path.join(ROOT, 'build', 'versus', name)
    os.makedirs(out, exist_ok=True)

    ppm = os.path.join(out, 'areole.ppm')
    mine = areole(args.page, w, h, DEFAULT_FONTS, ppm)
    theirs = browser(args.page, w, h, os.path.join(out, 'edge.png'))

    aw, ah, a = read_ppm(ppm)
    write_png(os.path.join(out, 'areole.png'), aw, ah, a)
    got = read_png(os.path.join(out, 'edge.png')) if os.path.exists(os.path.join(out, 'edge.png')) else None
    lines = []
    if got and got[0] == aw and got[1] == ah:
        vw, vrgb, differ = side_by_side(a, got[2], aw, ah)
        write_png(os.path.join(out, 'versus.png'), vw, ah, vrgb)
        lines.append('pixels that differ: %d of %d (%.2f%%)' % (differ, aw * ah,
                                                              100.0 * differ / (aw * ah)))
    else:
        lines.append('no browser screenshot to compare against')

    bad = 0
    lines.append('%-14s %-22s %-22s %s' % ('element', 'areole x,y wxh', 'browser x,y wxh', ''))
    for key in sorted(set(mine) | set(theirs), key=lambda k: (theirs.get(k, (0, 0))[1], k)):
        m = mine.get(key)
        t = theirs.get(key)
        fmt = lambda r: '%d,%d %dx%d' % r if r else '-'  # noqa: E731
        # Two empty boxes agree wherever they are: a browser reports an element
        # it does not render at 0,0 and areole at its place in the flow, and
        # neither is anything a reader can see.
        empty = m is not None and t is not None and m[2] == m[3] == 0 and t[2] == t[3] == 0
        off = (m is None or t is None or
               (not empty and max(abs(m[i] - t[i]) for i in range(4)) > args.tolerance))
        bad += off
        lines.append('%-14s %-22s %-22s %s' % (key, fmt(m), fmt(t), 'DIFFERS' if off else 'ok'))
    lines.append('%d of %d elements differ by more than %dpx' % (bad, len(set(mine) | set(theirs)),
                                                                args.tolerance))
    report = '\n'.join(lines)
    with io.open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8') as f:
        f.write(report + '\n')
    print(report)
    print('pictures in', out)


if __name__ == '__main__':
    main()
