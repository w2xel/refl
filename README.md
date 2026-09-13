# CPP Runtime Reflection Framework

Goal: Make a Framework based on latest compile-time reflection features that enables some partial runtime reflection.

Highly experimental.

## Build and test

In the configured Nix development shell:

```sh
meson setup builddir
meson compile -C builddir -j 2
meson test -C builddir --print-errorlogs
```

The test run covers the runtime and dynamic component suites, all three samples, metadata
lifetime regressions, and reflection-independent core-header checks. LTO defaults
to off because GCC 16.2 crashes linking the reflection-heavy Dyn coverage; warnings
as errors remain enabled. GCC static analysis is opt-in (`-Dstatic_analysis=true`)
because it uses substantial memory on generated reflection code. See
[Getting Started](docs/content/getting-started.md) for existing build directories
and toolchain setup.

The [implemented architecture](docs/content/architecture/current-state.md) records
current metadata boundaries and ownership, alongside the remaining migration plan.

## Two layers

- **Core** (`include/refl/refl.hpp`) — registration and the type-erased
  query API.  Register a type with `refl::Reg<T>`, then find it by name,
  construct, invoke, and get/set fields at runtime.
- **Dyn** (`include/refl/dyn.hpp`) — `refl::Dyn<T>`, a typed dispatch
  struct with real return types, Qt-style hooks, and runtime method
  implementation (mocking).  Includes the core.

## Pseudo Code Design

Register a class so it enters the global pool:

```cpp
[[maybe_unused]] static refl::Reg<MyClass> reg;
```

```cpp
auto my_class_class = *refl::find_class("MyClass");
```

Holding an object of this class allows querying functions and construct it.

```cpp
auto construct = *my_class_class.find_constructor({"int", "int"});
auto my_obj = *construct.call(1, 3);
```

The returned `Object` is a type-erased handle — it can be owning (backed by
`std::shared_ptr<void>`, copies share ownership) or non-owning (a raw
pointer borrow into a stack or heap object).  You can invoke methods on it
directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
auto result = *fn.invoke(my_obj, arg1, arg2);
// result is an Object — extract the value:
auto val = result.cast_safe<ReturnType>().value();
```

When you do need the concrete type, use the safe cast, which checks the
class name at runtime and returns a `std::shared_ptr<T>` that keeps the
object alive independently:

```cpp
auto result = my_obj.cast_safe<MyClass>();
if (result) { auto sp = result.value(); /* sp->... */ }
```

`cast_safe` only works on owning Objects.  For non-owning Objects (stack
borrows), use `cast_ref<T>()` which returns a raw `T*`.  The cast also
succeeds for base classes — `cast_safe<Base>()` on a derived Object works.
Use `is_class("Name")` to check the type without casting.  `is_owned()`
tells you whether the Object owns its data.

Fields can be found by name and get/set through type-erased handles:

```cpp
auto field = *my_class_class.find_field("x");
auto old = *field.get(my_obj);          // returns Object (copy of the value)
field.set(my_obj, 42);                 // typed set, no std::any needed
auto ptr = *field.get_ref(my_obj);     // void* into the object (move-only OK)
```

Note the dereferences — the return values are `std::expected`.  Function
and field results are returned as `Object` — use `cast_safe<T>()` (owning)
or `cast_ref<T>()` (non-owning) to extract.  Void functions return an
invalid `Object` (`.valid()` is false).
