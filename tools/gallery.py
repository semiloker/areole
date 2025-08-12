#!/usr/bin/env python3
"""The demo gallery: render a demo in both engines and compare them.

    python tools/gallery.py run --all              every demo, geometry + pixels
    python tools/gallery.py run --all --gate       and exit non-zero on a failure
    python tools/gallery.py compare css/grid/gap   one demo, in detail
    python tools/gallery.py report --out docs/gallery
    python tools/gallery.py bless --all            accept today's renders as golden

A demo is one standalone HTML file under examples/gallery/, and its path is its
id: examples/gallery/css/grid/gap.html is `css/grid/gap`. Both engines are
handed **the same file**, which is the whole point -- a demo written twice,
once as HTML and once programmatically, compares two authors rather than two
engines.

Three levels, and they answer different questions.

  **Geometry** is a gate at one pixel. Layout is arithmetic and two correct
  implementations of the same arithmetic agree; a pixel covers rounding at the
  used-value stage and nothing else.

  **Pixels against Chrome** is a published number and not a gate. Two font
  rasterizers cannot agree bit for bit -- different hinting, different gamma,
  different anti-aliasing -- and the only ways to make them would be to ship
  Chrome's rasterizer or to lie about the number.

  **Pixels against areole's own last render** is a gate at zero. Any change in
  what this engine draws has to be a deliberate, blessed golden update.

Chrome's geometry is read through a wrapper page that puts the demo in an
iframe and walks it. The demo file itself carries no script, because the
gallery's sixth rule says so and because a demo that needs one is testing the
wrong thing -- so the walking has to be done from outside it.
"""

import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEMOS = os.path.join(ROOT, 'examples', 'gallery')
# The blessed renders, as a hash each.
#
# The images themselves are 1.4 MB apiece -- 800 by 600 in three bytes -- and
# forty of them is sixty megabytes of binary in a repository that has none. The
# gate is "nothing changed at all", and a hash answers that exactly. What it
# cannot answer is *what* changed, and the Chrome percentage beside it is what
# says whether the change is worth looking at.
GOLDEN = os.path.join(ROOT, 'examples', 'gallery', '_golden.txt')
ENGINE = os.path.join(ROOT, 'build', 'ar_gallery.exe')

VIEW_W, VIEW_H = 800, 600

# One pixel on every edge. See the module docstring for why this one is a gate
# and the pixel ones are not.
GEOMETRY_TOLERANCE = 1

# The root element, and the one box that is left out of the comparison.
#
# `<html>` is a box with a content height in a browser and the canvas in
# areole: Chrome reports it as tall as the page, this engine as tall as the
# window. Every box below it agrees, which is what says the difference is about
# where the canvas lives and not about layout.
#
# Left out rather than tolerated, and said out loud in the run summary, because
# a comparison that quietly drops a box it cannot win is a comparison that will
# quietly drop the next one too.
ROOT_PATH = '0'

# Demos whose geometry is not gated, with the reason for each.
#
# A list in a file rather than a flag in a demo, so the whole of the debt is
# readable in one place and cannot grow one quiet line at a time.
NOT_GATED = os.path.join(DEMOS, '_not-gated.txt')


def not_gated():
    """-> {demo_id: reason}."""
    out = {}
    if not os.path.exists(NOT_GATED):
        return out
    with io.open(NOT_GATED, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            demo, _, reason = line.partition(' ')
            out[demo] = reason.strip()
    return out

sys.path.insert(0, HERE)
from compare_layout import find_browser  # noqa: E402


def engine_path():
    for cand in (ENGINE, ENGINE[:-4]):
        if os.path.exists(cand):
            return cand
    return None


def demos():
    """-> [demo_id], sorted. The path under examples/gallery is the id."""
    out = []
    for base, _, files in os.walk(DEMOS):
        if os.path.basename(base).startswith('_'):
            continue
        for f in sorted(files):
            if f.endswith('.html') and not f.startswith('_') and f != 'index.html':
                rel = os.path.relpath(os.path.join(base, f), DEMOS)
                out.append(rel.replace(os.sep, '/')[:-len('.html')])
    return sorted(out)


def demo_file(demo):
    return os.path.join(DEMOS, demo.replace('/', os.sep) + '.html')


# ----------------------------------------------------------------- areole


def areole_geometry(demo):
    """-> ({path: (x, y, w, h)}, truncated)."""
    exe = engine_path()
    if not exe:
        raise SystemExit('build/ar_gallery is not built')
    r = subprocess.run([exe, demo_file(demo), '--geometry'],
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    boxes = {}
    truncated = False
    for line in r.stdout.splitlines():
        if line.startswith('# nodes'):
            truncated = line.rstrip().endswith(' 1')
            continue
        if line.startswith('#') or not line.strip():
            continue
        p = line.split()
        if len(p) == 5:
            boxes[p[0]] = tuple(int(v) for v in p[1:])
    return boxes, truncated


def areole_png(demo, out_path):
    """Render to a PPM beside out_path. Returns the PPM's path."""
    exe = engine_path()
    subprocess.run([exe, demo_file(demo), '--ppm', out_path],
                   capture_output=True, text=True)
    return out_path


# ----------------------------------------------------------------- chrome

# The demo goes in an iframe and is walked from the parent, because the file
# itself may not carry a script. Same-origin over file:// needs the flag below.
WRAPPER = """<!doctype html><meta charset="utf-8">
<style>html,body{margin:0;padding:0}
iframe{width:%dpx;height:%dpx;border:0;display:block}</style>
<iframe id="f" src="%s"></iframe>
<textarea id="out"></textarea>
<script>
function childIndex(el){let n=0;for(let p=el.previousElementSibling;p;p=p.previousElementSibling)n++;return n;}
function walk(el, path, origin, out){
  const r = el.getBoundingClientRect();
  out.push(path + ' ' + Math.round(r.left-origin.left) + ' ' + Math.round(r.top-origin.top) +
           ' ' + Math.round(r.width) + ' ' + Math.round(r.height));
  for (const c of el.children) walk(c, path + '/' + childIndex(c), origin, out);
}
function dump(){
  const out = [];
  try {
    const d = document.getElementById('f').contentDocument;
    const root = d.documentElement;
    const origin = {left:0, top:0};
    walk(root, '0', origin, out);
  } catch (e) { out.push('# blocked ' + e); }
  const t = document.getElementById('out');
  t.value = out.join('\\n'); t.textContent = out.join('\\n');
}
window.addEventListener('load', dump);
dump();
</script>
"""


def chrome_geometry(demo, browser):
    src = 'file:///' + demo_file(demo).replace('\\', '/')
    tmp = tempfile.mkdtemp(prefix='areole-gallery-')
    wrap = os.path.join(tmp, 'wrap.html')
    with io.open(wrap, 'w', encoding='utf-8', newline='\n') as f:
        f.write(WRAPPER % (VIEW_W, VIEW_H, src))
    url = 'file:///' + wrap.replace('\\', '/')
    dom = subprocess.run(
        [browser, '--headless', '--disable-gpu', '--no-sandbox',
         '--allow-file-access-from-files',
         '--window-size=%d,%d' % (VIEW_W, VIEW_H),
         '--virtual-time-budget=4000', '--dump-dom', url],
        capture_output=True, text=True, encoding='utf-8', errors='replace').stdout
    m = re.search(r'<textarea[^>]*id="out"[^>]*>(.*?)</textarea>', dom, re.S)
    if not m:
        return {}
    from html import unescape
    boxes = {}
    for line in unescape(m.group(1)).splitlines():
        if line.startswith('#'):
            continue
        p = line.split()
        if len(p) == 5:
            boxes[p[0]] = tuple(int(v) for v in p[1:])
    return boxes


def chrome_png(demo, browser, out_path):
    src = 'file:///' + demo_file(demo).replace('\\', '/')
    subprocess.run(
        [browser, '--headless', '--disable-gpu', '--no-sandbox',
         '--hide-scrollbars',
         '--window-size=%d,%d' % (VIEW_W, VIEW_H),
         '--virtual-time-budget=4000',
         '--screenshot=' + out_path, src],
        capture_output=True, text=True)
    return out_path if os.path.exists(out_path) else None


# ------------------------------------------------------------------ images
#
# A PNG reader in the standard library, because the alternative is a
# dependency and this project has none. zlib does the hard half; the rest is
# undoing the per-row filters, which is five cases out of the specification.


def read_png(path):
    """-> (w, h, bytes) as RGB triples, or None."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    pos, w, h, depth, color = 8, 0, 0, 0, 0
    idat = []
    while pos + 8 <= len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if kind == b'IHDR':
            w, h, depth, color = struct.unpack('>IIBB', body[:10])
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
    if depth != 8 or color not in (2, 6) or not idat:
        return None
    chan = 3 if color == 2 else 4
    raw = zlib.decompress(b''.join(idat))
    stride = w * chan
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    at = 0
    for y in range(h):
        filt = raw[at]
        at += 1
        row = bytearray(raw[at:at + stride])
        at += stride
        if filt == 1:
            for i in range(chan, stride):
                row[i] = (row[i] + row[i - chan]) & 0xFF
        elif filt == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif filt == 3:
            for i in range(stride):
                left = row[i - chan] if i >= chan else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif filt == 4:
            for i in range(stride):
                a = row[i - chan] if i >= chan else 0
                b = prev[i]
                c = prev[i - chan] if i >= chan else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pr) & 0xFF
        prev = row
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = row[x * chan:x * chan + 3]
    return w, h, bytes(out)


def read_ppm(path):
    """-> (w, h, bytes) as RGB triples, or None."""
    with open(path, 'rb') as f:
        data = f.read()
    if not data.startswith(b'P6'):
        return None
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while i < len(data) and data[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    return fields[0], fields[1], data[i + 1:]


def pixel_diff(a, b, tolerance=8):
    """-> (differing, total, worst). Compares the overlap of two images."""
    if not a or not b:
        return (0, 0, 0)
    aw, ah, ap = a
    bw, bh, bp = b
    w, h = min(aw, bw), min(ah, bh)
    differing = worst = 0
    for y in range(h):
        ai = (y * aw) * 3
        bi = (y * bw) * 3
        for x in range(w):
            d = max(abs(ap[ai] - bp[bi]), abs(ap[ai + 1] - bp[bi + 1]),
                    abs(ap[ai + 2] - bp[bi + 2]))
            if d > worst:
                worst = d
            if d > tolerance:
                differing += 1
            ai += 3
            bi += 3
    return differing, w * h, worst


# ---------------------------------------------------------------- compare


def compare_one(demo, browser, tmp, want_pixels=True):
    mine, truncated = areole_geometry(demo)
    theirs = chrome_geometry(demo, browser) if browser else {}

    bad = []
    for path, box in sorted(mine.items()):
        if path not in theirs:
            continue
        if path == ROOT_PATH:
            continue
        t = theirs[path]
        if max(abs(box[i] - t[i]) for i in range(4)) > GEOMETRY_TOLERANCE:
            bad.append((path, box, t))

    result = {
        'demo': demo,
        'boxes': len(mine),
        'matched': len([p for p in mine if p in theirs]),
        'only_mine': len([p for p in mine if p not in theirs]),
        'only_theirs': len([p for p in theirs if p not in mine]),
        'geometry_bad': bad,
        'truncated': truncated,
        'pixels': None,
        'golden': None,
    }

    if want_pixels:
        ppm = areole_png(demo, os.path.join(tmp, 'a.ppm'))
        mine_img = read_ppm(ppm) if os.path.exists(ppm) else None
        if browser:
            png = chrome_png(demo, browser, os.path.join(tmp, 'b.png'))
            theirs_img = read_png(png) if png else None
            if mine_img and theirs_img:
                d, total, worst = pixel_diff(mine_img, theirs_img)
                result['pixels'] = {'differing': d, 'total': total,
                                    'pct': (100.0 * d / total) if total else 0.0,
                                    'worst': worst}
        have = golden_hashes().get(demo)
        if mine_img:
            with open(ppm, 'rb') as f:
                now = hashlib.sha256(f.read()).hexdigest()
            if have is None:
                result['golden'] = {'differing': -1, 'total': 0}  # not blessed yet
            else:
                result['golden'] = {'differing': 0 if now == have else 1, 'total': 1}
    return result


def golden_hashes():
    out = {}
    if not os.path.exists(GOLDEN):
        return out
    with io.open(GOLDEN, encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            demo, _, h = line.partition(' ')
            out[demo] = h.strip()
    return out


def render_hash(demo, tmp):
    ppm = areole_png(demo, os.path.join(tmp, 'h.ppm'))
    if not os.path.exists(ppm):
        return None
    with open(ppm, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def bless(todo):
    have = golden_hashes()
    with tempfile.TemporaryDirectory(prefix='areole-gallery-') as tmp:
        for d in todo:
            h = render_hash(d, tmp)
            if h:
                have[d] = h
    lines = ['# What areole draws for each demo, blessed.',
             '#',
             '# A hash and not an image: the renders are 1.4 MB each and the gate is',
             '# "nothing changed", which a hash answers exactly. A line that moves here',
             '# is a deliberate, reviewed change to what this engine draws.',
             '#',
             '# Regenerate with: python tools/gallery.py bless --all',
             '']
    for d in sorted(have):
        lines.append('%s %s' % (d, have[d]))
    with io.open(GOLDEN, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return GOLDEN


# ------------------------------------------------------------------ report


def report(results, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    rows = []
    for r in results:
        geo = 'pass' if not r['geometry_bad'] else '%d off' % len(r['geometry_bad'])
        px = '-' if not r['pixels'] else '%.2f%%' % r['pixels']['pct']
        gold = '-'
        if r['golden']:
            gold = 'not blessed' if r['golden']['differing'] < 0 else (
                'same' if r['golden']['differing'] == 0 else 'CHANGED')
        cls = 'ok' if not r['geometry_bad'] else 'bad'
        rows.append(
            '<tr class="%s"><td><code>%s</code></td><td>%d</td><td>%s</td>'
            '<td>%s</td><td>%s</td></tr>'
            % (cls, r['demo'], r['boxes'], geo, px, gold))

    html = """<!doctype html><meta charset="utf-8">
<title>areole gallery</title>
<style>
 body{font:14px/1.5 system-ui,sans-serif;margin:2rem;max-width:60rem}
 table{border-collapse:collapse;width:100%%} td,th{padding:.3rem .6rem;text-align:left}
 tr.ok td:nth-child(3){color:#166534} tr.bad td:nth-child(3){color:#b91c1c;font-weight:600}
 thead th{border-bottom:2px solid #ddd} tbody tr{border-bottom:1px solid #eee}
 code{font:13px ui-monospace,monospace}
 p{color:#555}
</style>
<h1>areole gallery</h1>
<p>%d demos. <b>Geometry</b> is a gate at one pixel on every edge.
<b>Chrome px</b> is the share of pixels that differ from Chrome's render and is
a published number, not a gate: two font rasterizers cannot agree bit for bit.
<b>Golden</b> is against areole's own last blessed render and is a gate at
zero.</p>
<table>
<thead><tr><th>demo</th><th>boxes</th><th>geometry</th><th>Chrome px</th><th>golden</th></tr></thead>
<tbody>
%s
</tbody></table>
""" % (len(results), '\n'.join(rows))
    path = os.path.join(out_dir, 'index.html')
    with io.open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(html)
    return path


# -------------------------------------------------------------------- main


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    args = argv[2:]
    want_all = '--all' in args
    gate = '--gate' in args
    named = [a for a in args if not a.startswith('--')]
    todo = demos() if (want_all or not named) else named

    if not todo:
        print('no demos under examples/gallery')
        return 0 if not gate else 1

    if cmd == 'bless':
        print('blessed %d demos into %s' % (len(todo), bless(todo)))
        return 0

    if cmd == 'report':
        out = 'docs/gallery'
        if '--out' in args:
            out = args[args.index('--out') + 1]
            todo = demos() if (want_all or len(named) < 2) else named[:-1]
        browser = find_browser()
        results = []
        with tempfile.TemporaryDirectory(prefix='areole-gallery-') as tmp:
            for d in todo:
                results.append(compare_one(d, browser, tmp))
        print('wrote %s' % report(results, os.path.join(ROOT, out)))
        return 0

    if cmd not in ('run', 'compare'):
        print(__doc__)
        return 2

    browser = find_browser()
    # `--golden-only` is for a machine with no browser -- CI. The golden gate
    # is against areole's own last render and needs nobody else.
    if '--golden-only' in args:
        browser = None
    if not browser:
        print('no browser: geometry and Chrome pixels are skipped, golden is not')

    exempt = not_gated()
    failed = 0
    reported = 0
    with tempfile.TemporaryDirectory(prefix='areole-gallery-') as tmp:
        for d in todo:
            r = compare_one(d, browser, tmp, want_pixels=(cmd != 'run') or True)
            px = '' if not r['pixels'] else '  chrome %.2f%%' % r['pixels']['pct']
            gold = ''
            if r['golden'] and r['golden']['differing'] > 0:
                gold = '  GOLDEN CHANGED'
            elif r['golden'] and r['golden']['differing'] < 0:
                gold = '  (not blessed)'
            note = ''
            if r['truncated']:
                note = '  TRUNCATED'
            if r['geometry_bad']:
                why = exempt.get(r['demo'])
                if why:
                    reported += 1
                else:
                    failed += 1
                print('%-34s %3d boxes  %d off%s%s%s%s'
                      % (r['demo'], r['boxes'], len(r['geometry_bad']), px, gold, note,
                         '  [' + why.split(':')[0] + ']' if why else ''))
                if cmd == 'compare':
                    for path, mine, theirs in r['geometry_bad'][:20]:
                        print('    %-14s areole %4d,%4d %4dx%-4d  chrome %4d,%4d %4dx%d'
                              % ((path,) + mine + theirs))
            else:
                if r['golden'] and r['golden']['differing'] > 0:
                    failed += 1
                print('%-34s %3d boxes  ok%s%s%s'
                      % (r['demo'], r['boxes'], px, gold, note))

    gated = len(todo) - reported
    print()
    if not browser:
        # Said rather than implied. With no browser there is nothing to compare
        # geometry *against*, and printing "all agree" would be a corpus
        # reporting agreement it never asked for.
        print('geometry was NOT compared: no browser. %d demos drew what was blessed.'
              % (len(todo) - failed))
        return 1 if (gate and failed) else 0
    print('%d of %d gated demos agree on geometry, at %d px on every edge'
          % (gated - failed, gated, GEOMETRY_TOLERANCE))
    if reported:
        print('%d more are reported and not gated -- see %s for the reason each'
              % (reported, os.path.relpath(NOT_GATED, ROOT)))
    print('the root <html> box is not compared: a browser gives it the height of')
    print('the page and areole gives it the height of the window.')
    return 1 if (gate and failed) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
