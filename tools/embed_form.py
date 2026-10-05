#!/usr/bin/env python3
"""Embed examples/16_forms/form.html into examples/16_forms/main.c.

    python tools/embed_form.py           rewrite the DOC array from form.html
    python tools/embed_form.py --check   exit non-zero if they disagree

The example draws the page form.html is, so that the window, the selftest and
tools/versus.py are looking at one document. The page is the source; the C
array is generated from it, cut into literals under the 509 characters C90
guarantees, and `example_forms --html` prints it back out byte for byte.
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, 'examples', '16_forms', 'form.html')
MAIN = os.path.join(ROOT, 'examples', '16_forms', 'main.c')
BEGIN = 'static const char *const DOC[] = {\n'
END = '};\n'


COLUMNS = 100  # .clang-format's ColumnLimit
ROOM = COLUMNS - len('    ""')


def split(esc, room):
    """A literal too long for one source line, cut after a space the way
    clang-format cuts it, so the formatter has nothing to change."""
    out = []
    while len(esc) > room:
        cut = esc.rfind(' ', 0, room) + 1
        if cut <= 0:
            break
        out.append(esc[:cut])
        esc = esc[cut:]
    out.append(esc)
    return out


def literals(text):
    parts, cur, size = [], [], 0
    for line in text.splitlines(keepends=True):
        esc = line.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
        if size + len(line) > 480 and cur:
            parts.append(cur)
            cur, size = [], 0
        cur.append(esc)
        size += len(line)
    if cur:
        parts.append(cur)
    out = []
    for p in parts:
        lines = []
        for k, e in enumerate(p):
            # The last literal of an element carries the comma after it.
            lines += split(e, ROOM - 1 if k == len(p) - 1 else ROOM)
        out.append('\n'.join('    "%s"' % e for e in lines))
    return ',\n\n'.join(out) + ',\n'


def main():
    html = io.open(HTML, encoding='utf-8', newline='').read()
    src = io.open(MAIN, encoding='utf-8', newline='').read()
    a = src.index(BEGIN) + len(BEGIN)
    b = src.index(END, a)
    want = src[:a] + literals(html) + src[b:]
    if '--check' in sys.argv:
        if want != src:
            print('examples/16_forms/main.c does not embed form.html -- run tools/embed_form.py')
            return 1
        print('main.c embeds form.html')
        return 0
    io.open(MAIN, 'w', encoding='utf-8', newline='\n').write(want)
    print('embedded', len(html), 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
