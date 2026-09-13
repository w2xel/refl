# Implemented architecture

## Modules

| Header | Responsibility |
| --- | --- |
| `core/{type,descriptor,error}.hpp` | Type/member identity, complete signatures, immutable metadata handles, diagnostics |
| `core/{value,call}.hpp` | Const-aware views, owned results, call frames and validated targets |
| `reflect/native.hpp` | Native operation generation |
| `reflect/schema.hpp` | Interface method declarations; no method definitions required |
| `runtime/registry.hpp` | Independent class/enum catalogs; explicit publication |
| `runtime/invoke.hpp` | Argument, receiver, result and reference-export validation |
| `extensions/observed.hpp` | Read-only call events and subscription tokens |
| `refl.hpp` | Object/class handles, full descriptor generation and runtime lookup |

Core, registry, and invocation headers compile without reflection. Generation uses
C++26 reflection. The library remains header-only.

## Descriptors and registries

```cpp
auto descriptor = refl::describe_class<Widget>(); // no registration
refl::Registry registry;
auto published = registry.publish(descriptor);
refl::Class type(registry.find_class("Widget"));
```

`describe_class<T>()` and `describe_enum<T>()` cache immutable descriptors.
`describe_interface<T>()` records method signatures without taking method addresses.
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
retains its receiver-anchor policy. Raw backend slots remain for Dyn and Mockable;
their state migration is pending.

## Verification and remaining work

The suite covers core headers, ownership, registry isolation, declaration-only
interfaces, runtime calls, and existing Dyn/Proxy/Mockable/Hooks APIs and samples.
The call prototype also covers replacement, reset, retained closure references,
listener failure, and reentry. Its state provider remains a test fixture.

Builds use GCC 16.2, `-O2`, warnings as errors, and no LTO. Static analysis is opt-in;
see [build measurements](../getting-started.md#build-and-test).

Completed: core extraction, contract prototype, native runtime calls, and independent
registries. Next: shared typed binding. Production dynamic state, full observation
integration, and final header/API cleanup follow.
