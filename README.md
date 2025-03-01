# areole

A GUI library in **strict C89** that uses **no graphics API**.

No Direct2D, no OpenGL, no Vulkan, no SDL. `areole` rasterizes every pixel itself and hands the
finished buffer to the operating system with one blit. Layout is written in **real CSS**.

```c
ar_stylesheet(ui,
    ".card { width:200px; padding:12px; background:#f8f3e9; border-radius:8px; }"
    ".card:hover { background:#f0e9db; }");

ar_begin(ui, "div.card");
    ar_text(ui, "h2", "Tulip");
ar_end(ui);
```

Status: **pre-alpha**, under active construction. Nothing works yet.

## License

MIT
