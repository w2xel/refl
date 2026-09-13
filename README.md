# C++ runtime reflection

A header-only C++26 library for runtime metadata, checked calls, structural binding,
and replaceable implementations. Experimental; requires GCC 16 reflection support.

## Build and test

```sh
nix develop
meson setup build
meson compile -C build -j 2
meson test -C build --no-rebuild --print-errorlogs
mkdocs build --strict -f docs/mkdocs.yml
```

The Nix lock file pins the toolchain. Use at most two compiler jobs to limit memory.
Static analysis and LTO are off by default. See [setup](docs/content/getting-started/index.md).

## Documentation

- [Documentation](https://w2xel.github.io/refl/)
- [Usage](docs/content/getting-started/index.md): runtime reflection, typed binding,
  dynamic dispatch, retained references, and observation. Code comes from asserted tests.
- [Architecture](docs/content/architecture/index.md): implemented contracts and remaining work.
- [Contributing](docs/content/contributing.md): local checks and documentation.

Serve the docs with `mkdocs serve -f docs/mkdocs.yml`.
