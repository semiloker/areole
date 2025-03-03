# areole

**A GUI library in strict C89 that uses no graphics API.**

No Direct2D. No OpenGL. No Vulkan. No SDL. No GTK. `areole` rasterizes every pixel itself and
hands the finished buffer to the operating system in a single blit. Layout is written in **real
CSS**, parsed once at startup.

```c
ar_stylesheet(ui,
    ".rail  { width:220px; display:flex; flex-direction:column; gap:4px; padding:12px; }"
    ".nav   { padding:10px 14px; border-radius:6px; color:#3a3a3a; }"
    ".nav:hover { background:#f0e9db; }"
    ".card  { width:200px; padding:12px; background:#f8f3e9; border-radius:8px; }");

ar_begin(ui, "div.rail");
    for (i = 0; i < 5; ++i)
        if (ar_button(ui, "nav", pages[i])) active = i;
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

## Design

```
  ar_begin / ar_button / ar_text        called fresh every frame
             |
             v   flat node array, indices never pointers
    style resolve     hash the class or id, merge base with :hover / :active
    layout            two passes per axis: bottom up fit, top down grow and place
    command list      linear byte buffer, every command carries its bounds
    dirty rect cull   only what actually changed gets touched
    rasterizer        writes straight into the pixels the OS will blit
             |
             v
       one BitBlt of one sub-rectangle
```

The back buffer *is* the `CreateDIBSection` memory, 32 bpp `BI_RGB`, top-down. areole rasterizes
directly into the pixels GDI owns, so presenting costs one copy instead of two. Matching the
native format is what keeps the driver from running a per-pixel conversion, and on old hardware
that is worth far more than which blit call you choose.

## Status

**Pre-alpha.** Being built in the open, one issue at a time. See the
[milestones](https://github.com/semiloker/areole/milestones).

- `v0.1.0` It draws — window, DIB back buffer, rasterizer, bitmap font
- `v0.2.0` It's CSS — parser, selector matching, flexbox solver
- `v0.3.0` It's a UI — buttons, text input, scrolling, dropdowns
- `v0.4.0` It's fast — hash-grid dirty rects, glyph atlas, benchmark gate
- `v0.5.0` It's portable — X11, Cocoa
- `v0.6.0` It's pretty — optional TrueType, OS-hinted text

## Requirements

Windows 2000 or newer. A C89 compiler. That is the entire list.

## License

MIT
