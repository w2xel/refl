# cpp_runtime_reflection

A C++26 framework that builds partial **runtime reflection** on top of the
new compile-time reflection features (`^^`, `template for`, `std::meta`).

Highly experimental. Built with GCC 16, which is the first mainline compiler
to ship C++26 reflection (P2996, enabled with `-std=c++26 -freflection`).

## Goal

Make a framework where registering a class in a global pool lets you find it
by name at runtime, construct it, and call its members without writing
boilerplate registration code.

## Two layers

- **Core** (`refl/refl.hpp`) — registration (`Reg<T>`) and the type-erased
  query API (`find_class`, `Constructor`, `Function`, `Field`, `Object`,
  `cast_safe`, enums).  See [Design](design.md).
- **Dyn** (`refl/dyn.hpp`) — `Dyn<T>`, a typed dispatch struct with real
  return types, Qt-style hooks, and runtime method implementation (mocking).
  See [Dyn](dyn.md).

## Where to go next

- [Architecture](architecture/index.md) — a proposed target design, repository
  assessment, layer contracts, and an incremental migration guide for maintainers.
- [Design](design.md) — the core reflection API.
- [Dyn](dyn.md) — the typed dispatch / dynamic layer.
- [Getting Started](getting-started.md) — building, testing, and serving docs.
