#!/usr/bin/env python3
"""The README's pictures of docs/showcase.html, all drawn by areole.

    python tools/readme_shots.py
    python tools/readme_shots.py --fonts D:/fonts --no-browser

Writes into docs/:

  showcase-live.png    docs/showcase.html being used -- a name retyped, an
                       email typed, a checkbox, a radio, the slider, the
                       select opened and an option chosen. An animated PNG,
                       which every current browser and GitHub animate.
  vs/<style>.png       one page per style, areole on the left and Edge on the
                       right, the same file and window -- when a browser is found

The pages are ordinary HTML and CSS; nothing in them was written for areole.
The faces come from C:/Windows/Fonts unless --fonts says otherwise.

The session is the SCRIPT below, played through `ar_gallery --script`, which
writes every frame and where the pointer was. areole draws no cursor -- the
platform does -- so this draws one onto each frame, and then keeps only what
changed from one frame to the next: the animation is a few hundred kilobytes
rather than a hundred full pictures.
"""

import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from versus import read_ppm, write_png, browser  # noqa: E402
from gallery import read_png  # noqa: E402

ENGINE = os.path.join(ROOT, 'build', 'ar_gallery.exe' if os.name == 'nt' else 'ar_gallery')
PAGE = os.path.join(ROOT, 'docs', 'showcase.html')
W, H = 1000, 640

# (name, page, faces, size): the styles the README shows beside Edge.
SEGOE = {'body': 'segoeui.ttf', 'bold': 'segoeuib.ttf', 'sans': 'segoeui.ttf', 'mono': 'consola.ttf'}
GEORGIA = {'body': 'georgia.ttf', 'bold': 'georgiab.ttf', 'italic': 'georgiai.ttf',
           'bold-italic': 'georgiaz.ttf', 'sans': 'arial.ttf', 'mono': 'consola.ttf'}
DEFAULTS = {'body': 'times.ttf', 'bold': 'timesbd.ttf', 'italic': 'timesi.ttf',
            'bold-italic': 'timesbi.ttf', 'sans': 'arial.ttf', 'mono': 'consola.ttf'}
STYLES = [
    ('settings', 'docs/showcase.html', SEGOE, (1000, 640)),
    ('dashboard', 'docs/styles/dashboard.html', SEGOE, (1000, 640)),
    ('article', 'docs/styles/article.html', GEORGIA, (1000, 640)),
    ('form', 'examples/16_forms/form.html', DEFAULTS, (800, 600)),
]

SCRIPT = """\
# the pointer starts at the bottom right, and the page is untouched for a moment
at 930 600
delay 16
hold 900
# a triple click selects the whole name, and typing replaces it
click #name 3
type Ada Byron
hold 400
# an empty field: the placeholder goes as the first character arrives
click #email
type ada@northwind.dev
hold 400
click #n3
hold 250
click #d2
hold 250
drag #vol 650 300
hold 300
# the select opens over the textarea, and a row is chosen
click #role
hold 700
clicktext Manager
hold 500
move #save
hold 2200
"""

# A pointer, the shape every desktop draws: B is the outline, W the fill.
ARROW = [
    'B..........',
    'BB.........',
    'BWB........',
    'BWWB.......',
    'BWWWB......',
    'BWWWWB.....',
    'BWWWWWB....',
    'BWWWWWWB...',
    'BWWWWWWWB..',
    'BWWWWWWWWB.',
    'BWWWWWBBBBB',
    'BWWBWWB....',
    'BWB.BWWB...',
    'BB..BWWB...',
    'B....BWWB..',
    '.....BWWB..',
    '......BB...',
]


def engine_args(folder, faces, page, size):
    if not os.path.exists(ENGINE):
        raise SystemExit('build/ar_gallery is not built')
    args = [ENGINE, os.path.join(ROOT, page), '--size', '%dx%d' % size]
    for slot, name in faces.items():
        path = os.path.join(folder, name)
        if not os.path.exists(path):
            raise SystemExit('no %s -- pass --fonts with the folder that has it' % path)
        args += ['--' + ('font' if slot == 'body' else slot), path]
    return args


LABEL_H = 34
LABEL = """<!doctype html><html><body style="margin:0; background:%s">
<div style="padding:6px 14px; font-family:'Segoe UI'; font-size:15px; color:#ffffff">
<b>%s</b> %s</div></body></html>"""


def label(folder, w, name, note, bg):
    """A strip naming one half of a picture, drawn by areole like everything
    else here."""
    with tempfile.TemporaryDirectory(prefix='areole-label-') as tmp:
        page = os.path.join(tmp, 'label.html')
        ppm = os.path.join(tmp, 'label.ppm')
        with io.open(page, 'w', encoding='utf-8') as f:
            f.write(LABEL % (bg, name, note))
        args = engine_args(folder, SEGOE, page, (w, LABEL_H))
        subprocess.run(args + ['--ppm', ppm], capture_output=True)
        return read_ppm(ppm)[2]


def blend(px, i, rgb, alpha):
    for k in range(3):
        px[i + k] = (px[i + k] * (256 - alpha) + rgb[k] * alpha) >> 8


def draw_pointer(px, x, y, held):
    """The arrow with its tip at (x, y), and a ring under it while the button
    is held, so a click reads as a click."""
    if x < 0 or y < 0:
        return
    if held:
        for dy in range(-11, 12):
            for dx in range(-11, 12):
                d = dx * dx + dy * dy
                if 64 <= d <= 121 and 0 <= x + dx < W and 0 <= y + dy < H:
                    blend(px, ((y + dy) * W + x + dx) * 3, (47, 111, 237), 150)
    for row, line in enumerate(ARROW):
        for col, ch in enumerate(line):
            if ch == '.' or not (0 <= x + col < W and 0 <= y + row < H):
                continue
            i = ((y + row) * W + x + col) * 3
            px[i:i + 3] = b'\x00\x00\x00' if ch == 'B' else b'\xff\xff\xff'


def chunk(kind, body):
    c = struct.pack('>I', len(body)) + kind + body
    return c + struct.pack('>I', zlib.crc32(kind + body) & 0xffffffff)


def changed_box(a, b):
    """The smallest rectangle holding every pixel that differs, or None."""
    row = W * 3
    differs = lambda y: a[y * row:(y + 1) * row] != b[y * row:(y + 1) * row]  # noqa: E731
    top = next((y for y in range(H) if differs(y)), None)
    if top is None:
        return None
    bottom = next(y for y in range(H - 1, -1, -1) if differs(y))
    left, right = W, -1
    for y in range(top, bottom + 1):
        ra, rb = a[y * row:(y + 1) * row], b[y * row:(y + 1) * row]
        if ra == rb:
            continue
        x = 0
        while ra[x * 3:x * 3 + 3] == rb[x * 3:x * 3 + 3]:
            x += 1
        left = min(left, x)
        x = W - 1
        while ra[x * 3:x * 3 + 3] == rb[x * 3:x * 3 + 3]:
            x -= 1
        right = max(right, x)
    return left, top, right - left + 1, bottom - top + 1


def write_apng(path, frames):
    """frames: [(rgb, ms)]. The first whole, the rest as what changed."""
    merged = []
    for rgb, ms in frames:
        if merged and merged[-1][0] == rgb:
            merged[-1][1] += ms
        else:
            merged.append([rgb, ms])

    seq = 0
    out = [b'\x89PNG\r\n\x1a\n', chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)),
           chunk(b'acTL', struct.pack('>II', len(merged), 0))]
    prev = None
    for rgb, ms in merged:
        x, y, w, h = (0, 0, W, H) if prev is None else changed_box(prev, rgb)
        out.append(chunk(b'fcTL', struct.pack('>IIIIIHHBB', seq, w, h, x, y, min(ms, 65535), 1000,
                                              0, 0)))
        seq += 1
        raw = b''.join(b'\x00' + bytes(rgb[((y + r) * W + x) * 3:((y + r) * W + x + w) * 3])
                       for r in range(h))
        data = zlib.compress(raw, 9)
        if prev is None:
            out.append(chunk(b'IDAT', data))
        else:
            out.append(chunk(b'fdAT', struct.pack('>I', seq) + data))
            seq += 1
        prev = rgb
    out.append(chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(b''.join(out))
    return len(merged)


def live(args, out):
    with tempfile.TemporaryDirectory(prefix='areole-shots-') as tmp:
        script = os.path.join(tmp, 'session.txt')
        with io.open(script, 'w', encoding='utf-8', newline='\n') as f:
            f.write(SCRIPT)
        r = subprocess.run(args + ['--script', script, '--frames', tmp], capture_output=True,
                           text=True)
        if r.returncode != 0:
            raise SystemExit('the session did not play:\n' + r.stdout + r.stderr)
        frames = []
        with io.open(os.path.join(tmp, 'frames.txt'), encoding='utf-8') as f:
            for line in f:
                name, ms, x, y, held = line.split()
                w, h, rgb = read_ppm(os.path.join(tmp, name))
                px = bytearray(rgb)
                draw_pointer(px, int(x), int(y), held == '1')
                frames.append((px, int(ms)))
    n = write_apng(out, frames)
    print('%s  %d frames, %d kB' % (os.path.relpath(out, ROOT), n, os.path.getsize(out) // 1024))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fonts', default='C:/Windows/Fonts')
    ap.add_argument('--no-browser', action='store_true')
    opts = ap.parse_args()
    docs = os.path.join(ROOT, 'docs')
    os.makedirs(os.path.join(docs, 'vs'), exist_ok=True)

    for name, page, faces, (w, h) in STYLES if not opts.no_browser else []:
        with tempfile.TemporaryDirectory(prefix='areole-shots-') as tmp:
            ppm, edge = os.path.join(tmp, 'areole.ppm'), os.path.join(tmp, 'edge.png')
            subprocess.run(engine_args(opts.fonts, faces, page, (w, h)) + ['--ppm', ppm],
                           capture_output=True)
            _, _, mine = read_ppm(ppm)
            try:
                browser(os.path.join(ROOT, page), w, h, edge)
                got = read_png(edge)
            except SystemExit:
                got = None
            if not got or got[0] != w or got[1] != h:
                print('no browser screenshot of %s: docs/vs/%s.png left as it was' % (page, name))
                continue
            gap = 12
            left = label(opts.fonts, w, 'areole', '&mdash; this library', '#2f6fed') + mine
            right = label(opts.fonts, w, 'Edge', '&mdash; the browser, same file', '#5b6270')
            right += bytes(got[2])
            rows = [left[y * w * 3:(y + 1) * w * 3] + b'\xc8\xcc\xd4' * gap +
                    right[y * w * 3:(y + 1) * w * 3] for y in range(h + LABEL_H)]
            write_png(os.path.join(docs, 'vs', name + '.png'), w * 2 + gap, h + LABEL_H,
                      b''.join(rows))
            print('docs/vs/%s.png' % name)

    live(engine_args(opts.fonts, SEGOE, 'docs/showcase.html', (W, H)),
         os.path.join(docs, 'showcase-live.png'))


if __name__ == '__main__':
    main()
