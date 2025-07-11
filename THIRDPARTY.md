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

## microui 2.02

- Files: `third_party/bench/microui.h`, `third_party/bench/microui.c`
- Upstream: https://github.com/rxi/microui
- Licence: MIT, Copyright (c) 2024 rxi
- Used by: `bench/compare/cmp_microui.c`

Vendored unmodified. Benchmark only: it is not linked into the library, and no
shipping build of areole contains any of it.

Compiled with relaxed flags. microui is not C89 -- `long long`, an unsuffixed
unsigned constant, a const-discarding qsort comparator -- so its translation
unit is built as gnu99 while the library keeps its own rules.

## The HTML named character reference table

`tools/entities.json`

The 2,231 named character references the HTML Standard defines, as the standard
itself publishes them.

- Author: WHATWG, and its contributors
- License: **CC BY 4.0** — https://creativecommons.org/licenses/by/4.0/
- Source: https://html.spec.whatwg.org/entities.json

Vendored rather than fetched, for the reason font8x8 is: the build has no
network step, and the exact bytes that produced `src/ar_html_entity.c` are in
the tree. Regenerate with:

```sh
python tools/gen_entities.py --fetch    # only when the standard changes
python tools/gen_entities.py
```

It is data for a generator and not a build input. Nothing in a clean build
reads it; the generated `.c` is committed, and `--check` compares the two. That
check is 0.9.0 acceptance criterion 3 and runs in CI, which is why the file has
to be here rather than at a URL: a criterion that needs the network is a
criterion that fails on a runner without it.

The generator does not alter a code point. It re-emits the table as a names
blob with offsets beside it, because the obvious `{const char *, ar_u32}` array
is 51 KB against a budget of 30.

## The html5lib conformance suites

`third_party/html5lib/`

- Authors: the html5lib contributors, and the web-platform-tests contributors
- License: **MIT** (see `third_party/html5lib/LICENSE.html5lib-tests`)
- Sources:
  - tokenizer: https://github.com/html5lib/html5lib-tests
  - tree construction: https://github.com/web-platform-tests/wpt, under
    `html/syntax/parsing/resources/`

Two suites in two repositories, because the tree construction tests moved. The
html5lib-tests README is now one sentence saying so.

Vendored, and `tests/ar_html5lib.c` reads them exactly as they ship rather than
a converted form -- two copies of the same tests would eventually disagree, and
the day they do the gate means nothing. Refresh with:

```sh
python tools/fetch_html5lib.py
```

Test data only. Nothing here is linked into areole, and no shipping build
contains any of it.
