"""Regenerates src/ar_font_data.c from third_party/font8x8_basic.h.

A development tool, not part of the build. The output is committed so that
neither the library nor CI needs Python.

Two things happen here that matter at run time:

  * The bitmaps are re-emitted as ar_u8 rather than char. The original is
    signed char, which makes every bit test in the blitter a sign extension
    hazard for no reason.

  * Each glyph's leftmost and rightmost ink columns are measured once, here,
    and the advance is stored alongside the bitmap. That is what lets text be
    spaced proportionally instead of monospaced, and it costs nothing at run
    time because the measuring already happened.
"""

import re
import os

FIRST = 32   # space
LAST = 126   # tilde
SPACE_ADVANCE = 4
GAP = 1

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = os.path.join(root, "third_party", "font8x8_basic.h")
dst = os.path.join(root, "src", "ar_font_data.c")

rows_of = {}
for line in open(src, encoding="utf-8"):
    m = re.match(r"\s*\{([^}]*)\},?\s*//\s*U\+([0-9A-Fa-f]{4})", line)
    if not m:
        continue
    codepoint = int(m.group(2), 16)
    rows_of[codepoint] = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))]

glyphs = []
for cp in range(FIRST, LAST + 1):
    rows = rows_of.get(cp, [0] * 8)
    assert len(rows) == 8, cp

    # Bit 0 is the leftmost pixel in this format.
    ink = 0
    for r in rows:
        ink |= r
    if ink == 0:
        left, advance = 0, SPACE_ADVANCE
    else:
        left = 0
        while not (ink >> left) & 1:
            left += 1
        right = 7
        while not (ink >> right) & 1:
            right -= 1
        advance = (right - left + 1) + GAP
    glyphs.append((cp, rows, left, advance))

out = []
out.append("/*\n")
out.append(" * areole - embedded bitmap font data. GENERATED, do not edit.\n")
out.append(" * SPDX-License-Identifier: MIT\n")
out.append(" *\n")
out.append(" * Regenerate with: python tools/gen_font.py\n")
out.append(" *\n")
out.append(" * Glyphs are font8x8 by Daniel Hepper, public domain, itself based on the\n")
out.append(" * public domain IBM VGA fonts. See THIRDPARTY.md. Not one bit is altered\n")
out.append(" * here; only the container type and the precomputed spacing are ours.\n")
out.append(" *\n")
out.append(" * Row order is top to bottom, and within a row bit 0 is the LEFTMOST pixel.\n")
out.append(" */\n")
out.append('#include "ar_internal.h"\n\n')
out.append("const ar_u8 ar__font_rows[AR_FONT_GLYPHS][AR_FONT_H] = {\n")
for cp, rows, left, advance in glyphs:
    ch = chr(cp)
    label = "space" if cp == 32 else ch
    out.append("    {%s}, /* %s */\n" % (", ".join("0x%02X" % r for r in rows), label))
out.append("};\n\n")

out.append("/* First ink column, so a glyph can be drawn flush against its own left edge. */\n")
out.append("const ar_u8 ar__font_left[AR_FONT_GLYPHS] = {\n")
for i in range(0, len(glyphs), 16):
    out.append("    " + ", ".join("%d" % g[2] for g in glyphs[i:i + 16]) + ",\n")
out.append("};\n\n")

out.append("/* Ink width plus one column of spacing. Space gets a fixed width of its own. */\n")
out.append("const ar_u8 ar__font_advance[AR_FONT_GLYPHS] = {\n")
for i in range(0, len(glyphs), 16):
    out.append("    " + ", ".join("%d" % g[3] for g in glyphs[i:i + 16]) + ",\n")
out.append("};\n")

open(dst, "w", encoding="utf-8", newline="\n").write("".join(out))
print("wrote %s (%d glyphs, U+%04X..U+%04X)" % (dst, len(glyphs), FIRST, LAST))
