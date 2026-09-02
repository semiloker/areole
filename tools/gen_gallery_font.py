#!/usr/bin/env python3
"""Build the gallery's shared TrueType face from font8x8.

    python tools/gen_gallery_font.py

Writes `examples/gallery/_font.ttf` and `examples/gallery/_font.css`, the second
being an `@font-face` rule with the first inlined as a data URI. Every demo
includes that CSS, so both engines rasterize the same outlines and the
comparison measures layout rather than font substitution -- rule 4 of
docs/roadmap/example-gallery.md, and the thing that kept two dozen demos out of
the geometry gate.

A development tool, like `gen_font.py` and `gen_entities.py`: it needs
`fontTools`, the output is committed, and neither the library nor CI needs
Python to build.

------------------------------------------------------------------------
Why this face

Because areole already draws with it. font8x8 is public domain -- see
THIRDPARTY.md -- so it can be redistributed inside a demo, and it is the face
`src/ar_font_data.c` was generated from. The two engines are not merely handed
the same file: they are drawing the same glyph shapes, with the same advances
areole already computes in `gen_font.py`.

Any other face would need areole to load it too, and the demo would then be
testing a TrueType rasterizer rather than a layout engine.

------------------------------------------------------------------------
The arithmetic that has to line up

areole draws a glyph as an 8 by 8 grid at `scale = font-size / 8`, so at 16px
one bitmap pixel is two screen pixels. The em here is 1024 units and a bitmap
pixel is 128 of them, so at 16px a bitmap pixel is again two screen pixels. The
advance is areole's own advance times 128.

The baseline is under the whole cell: eight pixels of ascent and none of
descent, because that is where areole puts it. See ASCENT_PX below -- a face
that describes itself differently from the way the other engine draws it puts
three pixels into every line box.

------------------------------------------------------------------------
Why fontTools writes the container

Because a hand-written one was rejected and said nothing about why. A browser
runs a font through a sanitizer before it will use it, and a font it declines
is not an error anywhere: `document.fonts` reports the face `unloaded`, the
text renders in the fallback, and the fallback looks perfectly fine. Four
versions of a hand-rolled writer were refused in a row -- OS/2 rebuilt to a
field-exact version 4, maxp measured rather than guessed, cmap's binary-search
fields derived, the data URI ruled out by loading from a file instead -- and
fontTools loaded every one of them without complaint, which is how little a
permissive parser tells you.

So the container is written by something that already knows the rules, and this
file is only the glyphs and the metrics.
"""

import base64
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, 'third_party', 'font8x8_basic.h')
OUT_TTF = os.path.join(ROOT, 'examples', 'gallery', '_font.ttf')
OUT_CSS = os.path.join(ROOT, 'examples', 'gallery', '_font.css')

FAMILY = 'areole gallery'
FIRST, LAST = 32, 126
SPACE_ADVANCE = 4     # pixels, as gen_font.py has it
GAP = 1

UPM = 1024
PX = UPM // 8
# The whole cell is above the baseline, and none of it below.
#
# That is not what a proportional face looks like, and it is exactly what
# areole draws: the bitmap blitter starts at the top of the cell and
# `ar__text_metrics` puts the baseline at its bottom -- "no descender to speak
# of", in as many words. A face declaring seven-eighths ascent and an eighth of
# descent describes a different font from the one on the other side of the
# comparison, and the difference lands on every line box: the browser's strut
# had two pixels of descent that this engine's did not, so every line was three
# pixels taller there than here.
#
# font8x8's own descenders -- the tails of g, p, y -- live in the bottom row of
# the cell and are drawn above the baseline here. Saying so in the font is
# what makes both engines agree about where a line ends.
ASCENT_PX = 8
DESCENT_PX = 0


def bitmaps():
    rows_of = {}
    with io.open(SRC, encoding='utf-8') as f:
        for line in f:
            m = re.match(r"\s*\{([^}]*)\},?\s*//\s*U\+([0-9A-Fa-f]{4})", line)
            if not m:
                continue
            cp = int(m.group(2), 16)
            rows_of[cp] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]
    return rows_of


def metrics(rows):
    """-> (left_ink_column, advance_in_pixels), exactly as gen_font.py has it."""
    ink = 0
    for r in rows:
        ink |= r
    if ink == 0:
        return 0, SPACE_ADVANCE
    left = 0
    while not (ink >> left) & 1:
        left += 1
    right = 7
    while not (ink >> right) & 1:
        right -= 1
    return left, (right - left + 1) + GAP


def runs(rows):
    """The ink as horizontal runs: (x0, x1, y), x1 exclusive.

    One rectangle per run rather than per pixel: a row of eight lit pixels is
    one contour instead of eight, and the rasterizer never has to resolve
    coincident edges.
    """
    out = []
    for y, r in enumerate(rows):
        x = 0
        while x < 8:
            if (r >> x) & 1:
                x0 = x
                while x < 8 and (r >> x) & 1:
                    x += 1
                out.append((x0, x, y))
            else:
                x += 1
    return out


def draw(pen, rows, left):
    """The glyph, as rectangles, relative to its own left ink column.

    Row 0 is the top and TrueType's y grows up from the baseline, so bitmap row
    y spans (ASCENT_PX - y - 1) to (ASCENT_PX - y).

    Clockwise, which is the direction TrueType fills: counter-clockwise outer
    contours draw nothing at all in some rasterizers and everything in others.
    """
    for x0, x1, y in runs(rows):
        gx0 = (x0 - left) * PX
        gx1 = (x1 - left) * PX
        gy1 = (ASCENT_PX - y) * PX
        gy0 = gy1 - PX
        pen.moveTo((gx0, gy0))
        pen.lineTo((gx1, gy0))
        pen.lineTo((gx1, gy1))
        pen.lineTo((gx0, gy1))
        pen.closePath()


def main():
    try:
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.ttGlyphPen import TTGlyphPen
    except ImportError:
        print('this needs fontTools: pip install fonttools')
        return 2

    rows_of = bitmaps()
    names = ['.notdef'] + ['u%04X' % cp for cp in range(FIRST, LAST + 1)]
    cmap = dict((cp, 'u%04X' % cp) for cp in range(FIRST, LAST + 1))

    glyphs, advances = {}, {}
    for name in names:
        pen = TTGlyphPen(None)
        if name == '.notdef':
            adv = SPACE_ADVANCE
        else:
            cp = int(name[1:], 16)
            rows = rows_of.get(cp, [0] * 8)
            left, adv = metrics(rows)
            draw(pen, rows, left)
        glyphs[name] = pen.glyph()
        advances[name] = (adv * PX, 0)

    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(advances)
    fb.setupHorizontalHeader(ascent=ASCENT_PX * PX, descent=-DESCENT_PX * PX)
    fb.setupNameTable({
        'familyName': FAMILY,
        'styleName': 'Regular',
        'fullName': FAMILY,
        'psName': 'areolegallery',
        'version': 'Version 1.0',
        'copyright': 'font8x8 by Daniel Hepper, public domain. See THIRDPARTY.md.',
    })
    fb.setupOS2(sTypoAscender=ASCENT_PX * PX, sTypoDescender=-DESCENT_PX * PX,
                usWinAscent=ASCENT_PX * PX, usWinDescent=DESCENT_PX * PX,
                sxHeight=PX * 4, sCapHeight=PX * 5)
    fb.setupPost()

    fb.save(OUT_TTF)
    with open(OUT_TTF, 'rb') as f:
        data = f.read()

    b64 = base64.b64encode(data).decode('ascii')
    css = ('/* GENERATED by tools/gen_gallery_font.py. Do not edit.\n'
           '\n'
           '   font8x8, public domain (see THIRDPARTY.md), as a TrueType face, so a\n'
           '   browser draws the same outlines areole draws. Every demo includes this:\n'
           '   rule 4 of docs/roadmap/example-gallery.md, one font shipped with the\n'
           '   gallery, so the comparison measures layout and not which font each\n'
           '   engine happened to find. */\n'
           '@font-face {\n'
           '  font-family: "%s";\n'
           '  src: url(data:font/ttf;base64,%s) format("truetype");\n'
           '}\n'
           '\n'
           '/* One hidden glyph, so the face is always loaded.\n'
           '\n'
           '   A browser fetches a web font only when a glyph needs it, and a demo\n'
           '   built out of empty boxes never asks for one. Its line boxes were then\n'
           '   struck against whatever font the browser found -- which is precisely\n'
           '   what rule 4 exists to prevent, and it cost three pixels a line on\n'
           '   every demo with no text in it.\n'
           '\n'
           '   Out of flow and invisible, so it changes no geometry and paints\n'
           '   nothing; `visibility: hidden` rather than `display: none`, because a\n'
           '   display-none glyph is never shaped and so never loads the face. The\n'
           '   selector names a pseudo-element, which areole refuses by design (see\n'
           '   ar__parse_compound, "a pseudo-element nothing here can paint"), so\n'
           '   this rule reaches one engine only -- and it has to, because the box\n'
           '   it makes is the browser catching up to areole rather than a box the\n'
           '   comparison should see. */\n'
           'html::before {\n'
           '  content: "x";\n'
           '  font-family: "%s";\n'
           '  position: absolute;\n'
           '  visibility: hidden;\n'
           '}\n'
           '/* END GENERATED by tools/gen_gallery_font.py */\n' % (FAMILY, b64, FAMILY))
    with io.open(OUT_CSS, 'w', encoding='utf-8', newline='\n') as f:
        f.write(css)

    print('wrote %s (%d bytes) and %s (%d bytes)'
          % (os.path.relpath(OUT_TTF, ROOT), len(data),
             os.path.relpath(OUT_CSS, ROOT), len(css)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
