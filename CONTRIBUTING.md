# Contributing to areole

## The two invariants

Everything else is negotiable. These are not.

1. **No heap allocation after `ar_init`.** The caller hands areole one block of memory and that
   is all it ever gets. `allocs_since_init` is asserted to be `0` in CI. This is what makes p99
   frame time equal p50, and it is why areole works on a machine with no allocator worth using.
2. **No floating point in the layout or raster hot path.** Flex slack is distributed in 16.16
   fixed point and rounded to integer pixels once. A Pentium III has an FPU; it is still slower
   and less predictable than the integer unit.

If a feature cannot be built inside those two rules, the feature is wrong, not the rules.

## The files

| | |
| --- | --- |
| `src/ar_arena.c` | Two-ended arena. Persistent grows up, ephemeral grows down, a frame reset is one integer store |
| `src/ar_raster.c` | Rectangle fills and the source-over blend |
| `src/ar_font.c` | The built-in 8x8 bitmap face and its span blitter |
| `src/ar_path.c` | Paths and the scanline coverage rasterizer. Integer only, exact analytic antialiasing |
| `src/ar_font_file.c` | TrueType parsing. Never copies, never allocates, bounds-checks every read |
| `src/ar_cff.c` | CFF outlines and the Type 2 charstring interpreter. The one place that runs a program out of a file |
| `src/ar_break.c` | Line breaking, after UAX #14 |
| `src/ar_bidi.c` | The bidirectional algorithm, UAX #9 |
| `src/ar_shape.c` | OpenType shaping: ligatures, kerning, Arabic joining, mark attachment |
| `src/ar_text.c` | UTF-8, the glyph cache, outline text drawing |
| `src/ar_css.c` | Tokenizer, parser, selector matching, the resolved style cache |
| `src/ar_layout.c` | The flexbox solver. Pure: no allocation, no clock, no drawing |
| `src/ar_damage.c` | The damage region and the paint digest |
| `src/ar_ctx.c` | The tree, the frame, and everything public that is not one of the above |
| `platform/ar_win32.c` | The only file that may include a platform header |

## The C89 rules you will trip over

areole targets Windows 2000 and up, and compiles on every MSVC ever shipped. That means C89, and
CI rejects the following in `src/` and `include/`:

| Not allowed | Use instead |
| --- | --- |
| `// comment` | `/* comment */` |
| declarations after statements | declare everything at the top of the block |
| `<stdint.h>`, `<stdbool.h>` | `ar_u8` … `ar_i32` from `areole.h` |
| `long long` | `ar_u32`, or split the value |
| `snprintf` | bounded `sprintf`, or `ar_fmt` when it lands |
| designated initializers, compound literals | assign fields, or a `_make()` helper |
| variadic macros | a function |
| `inline` | `AR_INLINE` |
| `for (int i = 0; ...)` | declare `i` at the top of the block |

## The core / platform split

`<windows.h>` uses nameless unions and `__int64`, so it **does not compile** with language
extensions disabled. MSVC's `/Za` breaks it outright and Microsoft's position is that there is no
workaround. areole therefore splits by strictness, and CI enforces the split:

- `src/`, `include/` — strict C89, gated by `-std=c89 -pedantic-errors -Werror` on gcc *and*
  clang. **Never** includes a platform header. CI greps for this.
- `platform/` — `gnu89`, may include `<windows.h>`, `<X11/...>`, Cocoa headers.

A pull request that puts `#include <windows.h>` anywhere under `src/` fails before it compiles.

## Workflow

- One issue, one branch, one pull request.
- [Conventional Commits](https://www.conventionalcommits.org/): `feat(layout):`, `fix(raster):`,
  `perf(css):`, `ci:`, `docs:`, `chore:`.
- Anything touching layout, raster, font or present must paste a before/after row from
  `ar_bench --json` into the pull request. "It felt fine" is not a measurement.
- Non-trivial logic ships with one runnable check in `tests/`. No framework, no fixtures — an
  `assert` that fails if the logic breaks.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

To run the strict gate the way CI does:

```sh
for f in src/*.c; do
    gcc -std=c89 -pedantic-errors -Wall -Wextra -Werror -Iinclude -c "$f" -o /dev/null
done
```

## Deliberate simplifications

Shortcuts with a known ceiling are marked in the source with a `ponytail:` comment naming both
the ceiling and the upgrade path, for example:

```c
/* ponytail: merged bounding rect, one per window. Degenerates when two changes
   sit in opposite corners. Upgrade path is the hash grid in #21. */
```

Do not silently remove one. Either take the upgrade path, or leave the note alone.
