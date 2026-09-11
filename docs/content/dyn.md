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
p->coords[0] = 99;         // operator[] for subscriptable members
p.reset(100, 200);         // swap the underlying object
p.get().x                  // typed escape hatch (int&)
```

One `TypedMethod<Sigs...>` field per function name, one `TypedProperty<T>`
field per data member, one `TypedStaticProperty<T>` per static data member,
one `TypedStaticMethod<R>` per static member function.  Overload resolution
is by argument type at compile time via the `matches_sig` concept — mixed
return types work (`TypedMethod<int(int), double(double)>`).

## Hooks (Qt-style, after-only)

```cpp
p.connect("sum", [](std::any& result) { ... });   // fires after sum()
p.on_change("x", [](std::any& newval) { ... });   // fires after x = ...
p.emit("custom", std::any(42));                   // fire connected callbacks
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

## Limitations

- Inherited members are not in the dispatch struct — use the core
  `find_class` / `Function` path for inherited methods.
- `Dyn<T>` is non-copyable, non-movable (dispatch fields point into it).
- Same-arity same-type overloads are ambiguous (match the first declared).
- `implement` trampolines support 0–2 parameters (3+ not yet supported).
- Static functions with parameters are not callable via `->` (the
  `TypedStaticMethod` field is nullary-only); use the core `StaticFunction`
  path instead.
