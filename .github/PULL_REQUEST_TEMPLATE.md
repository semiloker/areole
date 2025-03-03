### What

<!-- One paragraph. What changed and why. -->

Closes #

### How it was verified

<!-- Delete the lines that do not apply. Do not delete a line to avoid doing it. -->

- [ ] `cmake --build build` clean on MinGW
- [ ] `ctest --test-dir build` green
- [ ] Strict C89 gate passes: core compiles with `-std=c89 -pedantic-errors -Werror`
- [ ] Ran an example and looked at it
- [ ] Benchmark delta recorded below

### Performance

<!-- Required for anything touching layout, raster, font or present.
     Paste the relevant rows of `ar_bench --json`, before and after. -->

| metric | before | after |
| ------ | ------ | ----- |
|        |        |       |

### Invariants

- [ ] No heap allocation after `ar_init` (`allocs_since_init` still 0)
- [ ] No floating point added to the layout or raster hot path
- [ ] Core `src/` gained no platform header include
