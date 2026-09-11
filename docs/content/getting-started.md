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
meson compile -C builddir
meson test -C builddir
```

The `selfcheck` test verifies the toolchain can compile C++26 reflection by
reflecting a `struct Point { int x; int y; }` and asserting two members.

## Run a sample

```sh
./builddir/basic_sample      # core type-erased API
./builddir/dynamic_sample    # Dyn<T> dispatch + mocking
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
