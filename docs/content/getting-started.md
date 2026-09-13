# Getting Started

## Prerequisites

- [Nix](https://nixos.org) with flakes enabled
- [direnv](https://direnv.net) with [nix-direnv](https://github.com/nix-community/nix-direnv)

## Enter the dev shell

With direnv installed, allow the directory once:

```sh
direnv allow
```

Or manually:

```sh
nix develop
```

This gives you GCC 16.2 (C++26 reflection), Meson, Ninja, cppcheck, valgrind,
and the GCC-bundled gcov/gprof/gdb — all clang-free.

## Build and test

```sh
meson setup builddir
meson compile -C builddir -j 2
meson test -C builddir --print-errorlogs
```

The `selfcheck` test verifies the toolchain can compile C++26 reflection by
reflecting a `struct Point { int x; int y; }` and asserting two members.
The default test run also includes the core, Dyn/Proxy, Mockable, and Hooks suites
and smoke tests for all three samples. Core contract headers also have standalone
C++23 compilation, synthetic metadata, and include-boundary checks. Warnings remain errors.

LTO is disabled by default because the configured GCC 16.2 compiler crashes while
linking the reflection-heavy Dyn tests. Existing build directories keep their old
options; update one explicitly with `meson configure builddir -Db_lto=false`.
Use `-Db_lto=true` only when investigating compiler support; it is not part of the
verified baseline.

GCC static analysis is off by default. Use a separate build for it:

```sh
meson setup build-analysis -Dstatic_analysis=true
meson compile -C build-analysis -j 1
```

Analysis of a large reflection test can exceed 39 GiB per compiler process.
The normal build keeps `-O2`, debug information, and compiler warnings.
Use `-j 2` to limit concurrent compiler processes. Ninja otherwise chooses
parallelism from the CPU count.

Local measurement for `test_refl.cpp`, GCC 16.2, during runtime migration:

| Static analysis | Peak compiler memory | Compile time |
| --- | ---: | ---: |
| On | At least 38.9 GiB | Stopped after 4 min |
| Off | 3.14 GiB | 82 s |

Existing builds pick up the new default when Meson regenerates. If analysis was
enabled explicitly, use `meson configure builddir -Dstatic_analysis=false`.

## Run a sample

```sh
./builddir/samples/basic_sample      # core type-erased API
./builddir/samples/dynamic_sample    # Dyn<T> dispatch + mocking
./builddir/samples/proxy_sample      # structural binding without inheritance
```

## Serve the documentation

```sh
mkdocs serve -f docs/mkdocs.yml
```

Then open `http://127.0.0.1:8000`. To build a static site instead:

```sh
mkdocs build -f docs/mkdocs.yml -d site
```

The site is written to `docs/site`.

## Layout

```
include/refl/   framework headers (refl.hpp = core, dyn.hpp = Dyn<T> layer)
tests/           meson tests (assert-based executables, no test framework)
samples/         runnable usage examples
docs/            MkDocs Material site
```
