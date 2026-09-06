# cpp_runtime_reflection

A C++26 framework that builds partial **runtime reflection** on top of the
new compile-time reflection features (`^^`, `template for`, `std::meta`).

Highly experimental. Built with GCC 16, which is the first mainline compiler
to ship C++26 reflection (P2996, enabled with `-std=c++26 -freflection`).

## Goal

Make a framework where wrapping a class in a container registers it in a
global pool queryable at runtime, so you can find classes by name, construct
them, and call their members without writing boilerplate registration code.

## Where to go next

- [Design](design.md) — the intended API and pseudo-code.
- [Getting Started](getting-started.md) — building, testing, and serving docs.
