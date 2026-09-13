# Dynamic dispatch

```cpp
#include <refl/dyn.hpp>

refl::Dyn<Point> point;
point.reset_native<Point>(1, 2);
point->set(10, 20);
int sum = point->sum();
point->x = 42;
point.get().coords[0] = 99;
```

`Dyn<T>` is movable and non-copyable. Default construction creates interface-only
state. Native construction and reset do not publish to a registry. Static members
remain available through `get_class()` when native storage is present.

```cpp
point.implement<^^Point::sum>([] { return 100; });
point.wrap<^^Point::sum>([](auto& previous) { return previous() + 1; });
point.restore<^^Point::sum>();
```

Callbacks preserve argument qualifiers and support arbitrary parameter counts.
A callback can accept `Dyn<Point>::Self&` before its ordinary arguments when it
needs backend access. This handle locks weak state for the duration of the call.

`dispatch()` follows reset; `capture_binding()` retains the current generation.
`reset(Object)` validates before publication. `reset_native<U>(args...)` can change
the concrete type while keeping the interface. `detach_native()` keeps only targets
explicitly declared independent of native state.

Reference replacements require explicit export/lifetime policies. Properties have
separate read, write, and read-only view operations. See the
[dynamic architecture](architecture/dynamic.md) for ownership and migration details.

`Hooks<T>` remains an event facade over owned targets. Full observation integration
is the next migration step.
