# Setup

## Toolchain

Install [Nix](https://nixos.org/download/) with flakes enabled. From the repository root:

```sh
nix develop
meson setup build
meson compile -C build -j 2
meson test -C build --no-rebuild --print-errorlogs
```

`flake.lock` pins GCC 16, Meson, Ninja, and MkDocs Material. The build requires
C++26 reflection (`-freflection`). Core contract tests also compile as C++23.
With direnv and nix-direnv installed, `direnv allow` enters the same shell.

## Memory use

Limit compilation to two jobs locally; CI uses one. A normal compiler process
peaked near 3.1 GiB during migration. GCC static analysis exceeded 39 GiB on a large
reflection test. It is off by default.

For an existing build, clear old options with:

```sh
meson configure build -Dstatic_analysis=false -Db_lto=false
```

LTO is off because GCC 16.2 crashes when linking the dynamic tests.
To investigate static analysis, use a separate build and one job:

```sh
meson setup build-analysis -Dstatic_analysis=true
meson compile -C build-analysis -j 1
```

## Documentation

```sh
mkdocs serve -f docs/mkdocs.yml
mkdocs build --strict -f docs/mkdocs.yml
```

The preview is at `http://127.0.0.1:8000`. The static site is in `docs/site`.
MkDocs watches `tests/` during preview. Missing snippet files or sections fail the build.

## Usage tests

The following pages include named sections from `tests/test_usage.cpp`.
Each section runs in the `usage` test. Examples use `.value()` to expose unexpected
errors; application code can inspect the returned `expected` before use.

```sh
meson compile -C build -j 2 test_usage
meson test -C build --no-rebuild --print-errorlogs usage
```

All examples use these headers:

```cpp
--8<-- "test_usage.cpp:headers"
```

## Repository layout

| Path | Contents |
| --- | --- |
| `include/refl/` | Library headers |
| `tests/` | Contract tests and usage snippets |
| `docs/` | MkDocs configuration and Markdown |

See [contributing](../contributing.md) for local checks and the documentation workflow.
