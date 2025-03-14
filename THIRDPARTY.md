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


## Clay

`third_party/bench/clay.h`

A layout library in C99, used **only by the benchmark comparison harness**. It is never linked
into areole, and areole has no dependency on it.

- Author: Nic Barker and contributors
- Version: 0.14
- License: **zlib**
- Source: https://github.com/nicbarker/clay

Vendored because the comparison has to be reproducible by anyone who checks out the repository,
and because a benchmark that downloads its rival at build time is a benchmark that stops working.

Clay publishes microsecond layout times for 8,192 elements, which is the number `bench/compare/`
puts areole against. It is C99 and uses compound literals throughout, which is exactly why areole
could not be built on it and why the comparison is interesting.
