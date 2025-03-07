# areole

**A GUI library in strict C89 that uses no graphics API.**

No Direct2D. No OpenGL. No Vulkan. No SDL. No GTK. `areole` rasterizes every
pixel itself and hands the finished buffer to the operating system in a single
blit. Layout is written in **real CSS**, parsed once at startup.

![the hello example](docs/hello.png)

Every rectangle above came out of a stylesheet. The example that draws it does
not contain a single coordinate.

```c
ar_stylesheet(ui,
    ".rail    { width:220px; display:flex; flex-direction:column; padding:16px; gap:2px; }"
    ".nav     { padding:9px 12px; font-size:16px; color:#8a8175; }"
    ".nav:hover { background:#f0e9db; color:#2b2b2b; }");

ar_begin(ui, "div.rail");
    for (i = 0; i < 5; ++i)
        if (ar_button(ui, "div.nav", pages[i])) selected = i;
ar_end(ui);
```

## Why

There are excellent immediate mode GUI libraries. None of them is this one.

| | C89 | zero deps | software blit | declarative flex layout |
| --- | :-: | :-: | :-: | :-: |
| [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear) | yes | yes | yes | **no** — manual rows and columns |
| [Clay](https://github.com/nicbarker/clay) | **no** — C99 compound literals | yes | **no** — emits commands only | yes |
| [Dear ImGui](https://github.com/ocornut/imgui) | **no** — C++ | no | **no** — GPU only | no |
| [LVGL](https://github.com/lvgl/lvgl) | **no** — C99 | libc | yes | yes |
| [luigi](https://github.com/nakst/luigi) | **no** — C99 | yes | yes | **no** — retained, manual |
| **areole** | **yes** | **yes** | **yes** | **yes** |

## Two invariants

Everything else is negotiable.

**No heap allocation after `ar_init`.** The caller hands areole one block and
that is all it ever gets. The example makes exactly one allocation, and the
operating system did it before `main` ran:

```c
static unsigned char memory[AR_MEM(512)];
ar_ctx *ui = ar_init(memory, sizeof memory);
```

**No floating point in the layout or raster hot path.** Flex slack is
distributed in integers and the rasterizer never sees a float. A Pentium III
has an FPU; it is still slower and less predictable than the integer unit, and
predictability is what makes p99 frame time equal p50.

## Numbers

areole measures itself. The phases are reported separately because a single
frame time hides the one thing worth knowing on old hardware: whether the cost
is the rasterizer or the blit. Those have entirely different fixes.

From the example above, 50 boxes at 1024×640 on a modern desktop:

| phase | p50 | p99 |
| --- | --: | --: |
| style | 16 µs | 30 µs |
| layout | 5 µs | 14 µs |
| raster | 433 µs | 816 µs |
| present | 609 µs | 2161 µs |
| **frame** | **1071 µs** | **2744 µs** |

Note which half dominates. With rectangles only, the GDI blit was more than
twice the rasterizer; with a few hundred glyphs it is the other way round.
That is the entire reason the two are never added together.

Averages are not reported. A UI that is smooth apart from one stall every two
seconds has an excellent average and is unusable.

## Design

```
  ar_begin / ar_button / ar_text        called fresh every frame
             |
             v   flat box array, indices never pointers
    style resolve     hash the class or id, merge base with :hover / :active
    layout            two passes per axis: bottom up fit, top down grow and place
    rasterizer        writes straight into the pixels the OS will blit
             |
             v
       one BitBlt
```

The back buffer *is* the `CreateDIBSection` memory: 32 bpp `BI_RGB`, top-down.
areole rasterizes directly into the pixels GDI owns, so presenting costs one
copy instead of two. Matching the native format is what keeps the driver from
converting every pixel, and on old hardware that is worth far more than which
blit call you choose.

Hover is resolved from the previous frame. It has to be known before the style
is resolved, but it depends on where the box ended up, which is not known until
after layout — hit testing against last frame is how every immediate mode
toolkit breaks that circle.

## Status

**Pre-alpha.** Built in the open, one issue at a time.

- **v0.1** *It draws* — window, DIB back buffer, rasterizer, bitmap font ✅
- **v0.2** *It's CSS* — parser, selector matching, flexbox solver ✅
- **v0.3** *It's a UI* — text input, scrolling, dropdowns, rounded corners
- **v0.4** *It's fast* — hash-grid dirty rects, glyph atlas, benchmark gate
- **v0.5** *It's portable* — X11, Cocoa
- **v0.6** *It's pretty* — optional TrueType, OS-hinted text

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ar_test          # 210 checks
./build/example_hello
```

## Requirements

Windows 2000 or newer. A C89 compiler. That is the entire list.

## Documentation

- [The CSS subset](docs/CSS_REFERENCE.md) — every property and selector, and
  what is deliberately missing
- [Contributing](CONTRIBUTING.md) — the two invariants, the C89 rules, and why
  the core and the platform layer are separate targets
- [Third party material](THIRDPARTY.md)

## License

MIT
