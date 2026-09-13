# Architecture

The library separates reflection generation from runtime contracts. All modules
are header-only. This section describes the implemented state.

```mermaid
flowchart TD
    C[Core: identity, metadata, values, targets]
    R[Registry and checked invocation] --> C
    G[Reflection generation] --> C
    D[Dispatch table] --> R
    P[Typed proxy] --> R
    P --> G
    F[Dyn facade] --> D
    F --> P
    O[Observation] --> R
```

## Modules

| Headers under `include/refl/` | Responsibility |
| --- | --- |
| `core/{type,descriptor,error}.hpp` | Exact identity, signatures, metadata, diagnostics |
| `core/{value,call}.hpp` | Views, owners, frames, and targets |
| `runtime/registry.hpp` | Independent catalogs |
| `runtime/invoke.hpp` | Shared call validation and extraction |
| `reflect/native.hpp` | Native operation generation |
| `reflect/schema.hpp` | Declaration-only interface schemas |
| `refl.hpp` | Full descriptor generation and runtime object/member handles |
| `dyn/proxy.hpp` | Shared binding plans and typed calls |
| `dynamic/dispatch_table.hpp` | Owned targets and generation publication |
| `dyn.hpp` | Typed dynamic facade |
| `extensions/observed.hpp` | Completion events and subscription tokens |
| `hooks.hpp` | Compatibility callbacks over dynamic targets |

Core, registry, invocation, dispatch, and observation compile without reflection.
Generation and typed facades use C++26 reflection. Include-boundary tests enforce
this separation.

## Read next

- [Metadata and calls](contracts.md): identity, ownership, validation, and errors.
- [Typed binding](typed-binding.md): structural matching and shared plans.
- [Dynamic state](dynamic.md): replacement, reset, capture, and detachment.
- [Observation](observation.md): delivery and interception boundaries.

## Migration status

| Complete | Evidence |
| --- | --- |
| Reflection-independent core contracts | `core_headers`, `core_include_boundaries`, `core_contracts` |
| Retained metadata and isolated registries | `metadata_lifetime`, `registry_contracts` |
| Shared native/runtime call frames | `runtime_contracts`, `refl_api` |
| Shared typed binding plans | `binding_contracts`, `dyn_api` |
| Owned dynamic generations | `dynamic_state`, `dispatch_table_api`, `call_contract` |
| Executable usage documentation | `usage` |

Remaining work: integrate typed and property observation, retire compatibility
hooks where covered, then finish header/API cleanup. Dynamic state and observation
mutation require caller synchronization. Cross-DSO identity and stable serialized
member IDs are outside the current contract.
