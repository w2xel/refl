# Implemented architecture

## Modules

| Header | Responsibility |
| --- | --- |
| `core/{type,descriptor,error}.hpp` | Type/member identity, complete signatures, immutable metadata handles, diagnostics |
| `core/{value,call}.hpp` | Const-aware views, owned results, call frames and validated targets |
| `reflect/native.hpp` | Native operation generation |
| `reflect/schema.hpp` | Interface methods and property capabilities; no method definitions required |
| `runtime/registry.hpp` | Independent class/enum catalogs; explicit publication |
| `runtime/invoke.hpp` | Argument, receiver, result and reference-export validation |
| `extensions/observed.hpp` | Read-only call events and subscription tokens |
| `dyn/proxy.hpp` | Shared native binding plans and live/captured dispatch views |
| `dynamic/dispatch_table.hpp` | Owned method/property targets and state publication |
| `dyn.hpp` | Typed dynamic facade and native reset |
| `refl.hpp` | Object/class handles, full descriptor generation and runtime lookup |

Core, registry, invocation, and dispatch-table headers compile without reflection. Generation uses
C++26 reflection. The library remains header-only.

## Descriptors and registries

```cpp
auto descriptor = refl::describe_class<Widget>(); // no registration
refl::Registry registry;
auto published = registry.publish(descriptor);
refl::Class type(registry.find_class("Widget"));
```

`describe_class<T>()` and `describe_enum<T>()` cache immutable descriptors.
`describe_interface<T>()` records method signatures and property capabilities without taking method addresses.
Each base relationship retains its descriptor. Hierarchy lookup, base casts, and
`Base::as_class()` use this retained graph. Handles remain usable after a local
registry is destroyed. Describing a derived class does not publish its bases.

Publication of the same descriptor is idempotent. A different descriptor with the
same catalog name returns `conflict`; it does not replace the entry. Catalog locks
protect publication and lookup. Calls run outside those locks.

`Reg<T>`, `ensure_registered<T>()`, and free name lookups use `default_registry()`.
Native `Object` construction still checks that catalog for full metadata. Explicit
construction through a `Class` retains the selected descriptor.

Removed APIs: mutable class/enum pools, pool mutex access, `Registrar`,
`EnumRegistrar`, registration holder classes, `Dyn::registrar()`, and raw-pointer
metadata-handle constructors. Pass `shared_ptr<const ClassInfo>` or
`shared_ptr<const EnumInfo>` to handles.

## Calls

Native constructors, methods, static methods, and supported field reads/writes use
`CallTarget` and `CallFrame`. `Function::try_invoke` returns structured diagnostics;
`Function::invoke` translates to `expected<Object, Error>`.

| Scenario | Behavior |
| --- | --- |
| Mutable reference argument | Writes reach the caller |
| Const argument or receiver | Mutable access fails before execution |
| Move-only argument | Requires explicit consumption |
| Target exception | Original exception propagates |
| Const reference result | Checked access stays read-only |
| Restricted raw reference | Export fails before execution |

The checked API requires declared reference provenance. The older `invoke` adapter
retains its receiver-anchor policy. Dynamic replacements use explicit target policies;
raw backend slots and synthetic interface objects are removed.

## Verification and remaining work

The suite covers core headers, ownership, registry isolation, declaration-only
interfaces, runtime calls, Dyn/Proxy/Hooks APIs, and samples. The former prototype
tests use the production `DispatchTable` for replacement, reset, retained closure
references, listener failure, and reentry. The separate test state provider is removed.

Builds use GCC 16.2, `-O2`, warnings as errors, and no LTO. Static analysis is opt-in;
see [build measurements](../getting-started.md#build-and-test).

Completed: core extraction, contract prototype, native runtime calls, independent
registries, shared typed binding, and production dynamic state. Full observation
integration and final header/API cleanup remain.

## Typed binding

`Proxy<T>` separates shared plans from owning instance state. Plans match complete
signatures and retain selected declarations. Native bindings own targets; dynamic
bindings resolve through live or captured handles. Both use common call frames. Rebinding is transactional; moves transfer prepared
state without lookup. Existing dynamic views see later slot replacements.

Removed: raw overload arrays, tuple argument copies, string-normalized typed matching,
and duplicate rebind lookup. Implementations preserve reference parameters
and accept arbitrary parameter counts. See [typed binding](typed-binding.md).

## Dynamic state

`DispatchTable` keys owned targets by member and operation kind. Generations retain
native baselines and storage. `Dyn<T>::reset(Object)` publishes only after complete
binding; `reset_native<U>(args...)` selects the concrete constructor explicitly.

`dispatch()` follows reset and detachment. `capture_binding()` retains one generation.
Saved targets retain their original receiver; `restore<M>()` selects the current
baseline. Wrappers own their predecessor. Detachment retains only declared
independent targets. Callbacks use weak backend state instead of facade pointers.

Properties have separate read/write/view targets. Retained read-only views survive
backing-storage replacement. Typed raw closure references are restricted by default;
retained extraction preserves the selected callable's anchor.

Removed: `Mockable`, raw slots, `SelfRef`, `WrapCtx`, string-selected implementation,
`make_dynamic()`, implicit-type reset, and hidden registration by `Dyn`. Hooks now
wrap owned targets; full observation policy remains the next step. See
[dynamic dispatch](dynamic.md).
