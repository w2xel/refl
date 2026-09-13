# Dyn — typed dispatch and dynamic implementation

`#include <refl/dyn.hpp>`

`Dyn<T>` is the layer on top of the refl core (`refl/refl.hpp`).  It wraps a
`std::shared_ptr<T>` and synthesizes a compile-time dispatch struct (via
`define_aggregate`) with named callable fields for each member, so you get
real return types at the call site — no `std::any`, no `std::variant`.

This layer is experimental and likely to change; this doc is intentionally
bare-bones.

## Dispatch struct

```cpp
refl::Dyn<Point> p(1, 2);
p->set(10, 20);           // overload resolved by argument type
int s = p->sum();          // real return type
int x = p->x;              // implicit conversion (read)
p->x = 42;                 // assignment (write)
p.get().coords[0] = 99;         // operator[] for subscriptable members
p.reset(100, 200);         // swap the underlying object
p.get().x                  // typed escape hatch (int&)
```

One `TypedMethod<Sigs...>` field per function name, one `TypedProperty<T>`
field per data member. Static members use `get_class()`.  Overload resolution
is by argument type at compile time via the `matches_sig` concept — mixed
return types work (`TypedMethod<int(int), double(double)>`).

## Hooks (Qt-style, after-only)

```cpp
p.connect("sum", [](refl::Object& result) { ... });   // fires after sum()
p.on_change("x", [](refl::Object& newval) { ... });   // fires after x = ...
p.emit("custom", 42);                                 // fire connected callbacks
```

After-only observers — they see the result/value but cannot veto or modify.
Multiple `connect` calls on the same name accumulate (multi-listener).

## Runtime method implementation (mocking)

```cpp
refl::Dyn<IShape> s;                         // abstract T — no object
s.implement<^^IShape::area>([](refl::Dyn<IShape>&, int scale) {
    return scale * 100;
});
int a = s->area(5);
```

The lambda receives `Dyn<T>&` as its first argument.  For concrete T,
`implement` overrides individual methods while keeping the real object
alive (per-method mocking); `restore<^^T::method>()` removes an override;
`make_dynamic()` switches to fully dynamic mode; `reset(args...)` switches
back to a real object.  String-based `implement<"method">(...)` is also
available (compile-time checked).

## Binding boundary

Typed fields use shared call frames. Plans retain metadata; each proxy binds its own
receiver. Failed rebinding preserves the old view. Inherited members are included.
Dyn is movable and non-copyable. Static members use `get_class()`.

Mockable implementations preserve parameter qualifiers and accept arbitrary parameter
counts. Dyn implementation/wrap helpers still support at most two arguments.
Production slot ownership and reset remain the next migration step.
See [typed binding](architecture/typed-binding.md) for the implemented boundary.
