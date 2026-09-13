# Layer 5: Dynamic dispatch

`DispatchTable` owns method and property targets. It accepts an `InterfaceSchema`
and includes no reflection, proxy, or registry headers. `Dyn<Interface>` combines
that table with a typed view. The table is in `dynamic/dispatch_table.hpp`; the
facade remains in `dyn.hpp`.

## State and ownership

| Part | Owns |
| --- | --- |
| Slot | A complete `CallTarget`, keyed by member and operation kind |
| Generation | Current targets, native baselines, and optional native storage |
| Live handle | The state pointer used for each new call |
| Captured handle | One retained generation |
| Resolved call | The selected target and generation until the call completes |

Replacement validates the full signature and operation kind before changing a slot.
Saved targets retain their receiver, callable context, and export/dependency policy.
A running call survives self-replacement. Method and property read/write/view
operations use the same target store.

```cpp
refl::Dyn<Drawable> dynamic; // interface-only; slots may be unimplemented
dynamic.reset_native<Square>(4);
auto live = dynamic.dispatch();
auto captured = dynamic.capture_binding();
auto saved = dynamic.target<^^Drawable::render>();

dynamic.reset_native<Triangle>(6, 8);
dynamic.replace<^^Drawable::render>(saved); // calls the retained Square
dynamic.restore<^^Drawable::render>();     // selects the Triangle baseline
```

`reset(Object)` validates an owned object and builds its targets before publication.
`reset_native<T>(args...)` constructs that explicit type, then calls `reset(Object)`.
Failure preserves the current state. Reset discards replacements and wrappers.
It does not register types or undo external constructor side effects.

Live handles and `dynamic->member()` follow successful reset and detachment.
Captured bindings see replacements in their captured generation but do not follow
later publication. Moving the facade preserves live handle identity.

## Wrapping and detachment

`wrap<M>(fn)` captures the previous target. Nested wraps compose as `B(A(target))`.
Generic signature expansion preserves references and accepts arbitrary argument counts.
There are no raw context slots or `std::any` wrapping contexts.

```cpp
dynamic.implement<^^Drawable::render>([](int scale) { return scale * 10; },
    {.native_dependency = refl::NativeDependency::independent});
dynamic.detach_native(); // keeps that explicitly independent replacement
```

Detachment publishes a generation without native storage or baselines. It retains
only explicitly independent targets. Native and unknown dependencies are dropped.
A wrapper inherits its previous target's dependencies and declares additional ones;
its default additional dependency is unknown. Saved targets and captured generations
keep their owners until released.

Dependency declarations are caller assertions. The library cannot inspect borrowed
pointers hidden in captures. A callback that needs state can accept `Dyn<T>::Self&`.
That call-scoped backend handle locks a weak state reference; it does not point at
a movable facade. An expired state produces a diagnostic. `weak()` provides the
same explicit locking mechanism for custom callbacks.

## References and properties

Reference replacements default to restricted raw export. Declare result provenance
and use `try_call_retained<T>` to keep a closure-owned result across replacement.
Typed raw reference calls fail before target entry unless caller-borrow export is
explicitly allowed. Wrappers retain the previous target and its declared policy.

`dispatch(kind)` selects method, read, write, or view operations. Property views
return read-only references. `set_property<M>` owns backing storage; replacing that
storage does not invalidate an already retained view. Replacing a read or write
target alone does not create a view capability.

Concurrent mutation and invocation require external synchronization. Retention
supports lifetime and reentrancy; it does not make slot mutation thread-safe.

## Removed APIs and verification

Removed: `Mockable`, raw method/property slots, fake interface `Object` storage,
`SelfRef`, `WrapCtx`, string-based implementation selection, `make_dynamic()`, and
implicit-type `reset(args...)`. Use member IDs or reflections, owned targets,
`detach_native()`, and explicit `reset_native<T>()`.

The former prototype state provider is removed. Its tests now use `DispatchTable`
under C++23 without reflection. `dynamic_state` covers publication, isolation,
saved targets, wrapping, failed reset, detachment, closure references, weak context
expiry, and observation during replacement. Existing API tests and samples use the
new table. Full observation integration remains the [next step](observation.md).
