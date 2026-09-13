# Contributing

## Local checks

In `nix develop`, run:

```sh
meson setup build --reconfigure
meson compile -C build -j 2
meson test -C build --no-rebuild --print-errorlogs
mkdocs build --strict -f docs/mkdocs.yml
```

For a first build, use `meson setup build` without `--reconfigure`.
Tests use assertions. Keep assertions enabled (`-Db_ndebug=false`).
Meson generates `build/compile_commands.json` for editors and analysis tools.

## Documentation changes

Keep text concise. Use ASD-STE-100 principles: short sentences, active voice,
and consistent terms. Prefer tested code, tables, and diagrams. Describe current
behavior in the architecture; put incomplete work in its remaining-work section.

Add usage code to `tests/test_usage.cpp` with an assertion for the expected result.
Use named `// --8<-- [start:name]` and `// --8<-- [end:name]` markers. Include the
section in a fenced C++ block with `--8<-- "test_usage.cpp:name"`.
Keep each page's setup and execution order clear. Do not add standalone samples
or duplicate the code in Markdown.

