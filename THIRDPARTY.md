# Third party material

## font8x8

`third_party/font8x8_basic.h`

8x8 monochrome bitmap font, ASCII U+0000 to U+007F.

- Author: Daniel Hepper <daniel@hepper.net>
- Based on work by Marcel Sondaar and the public domain IBM VGA fonts
- License: **Public Domain**
- Source: https://github.com/dhepper/font8x8

Vendored rather than fetched, so the build has no network step and the exact
bytes that produced `src/ar_font_data.c` are in the tree. Regenerate with:

```sh
python tools/gen_font.py
```

The generator does not alter a single glyph. It re-emits the bitmaps with the
types areole uses, and precomputes each glyph's ink extent so text can be laid
out proportionally at no run time cost.
