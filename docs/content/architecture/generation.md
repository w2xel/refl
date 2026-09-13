# Layer 2: Reflection and descriptor generation

Generation translates C++ declarations into the common descriptors and callable
targets. It depends on contracts, the concrete type, and compiler reflection. It
does not choose a registry or allocate per-instance replacement state.

Native operations and interface schemas are in `refl/reflect/{native,schema}.hpp`.
Full class/enum description and the default-registration helpers remain in `refl.hpp`.

## Separate description from publication

```cpp
auto descriptor = refl::describe_class<Square>();
auto requirements = refl::describe_interface<Drawable>();

refl::Registry registry;
registry.publish(descriptor).value();

// Optional process-wide discovery.
refl::default_registry().publish(descriptor).value();
```

Interface description records declarations without taking method addresses. Native
description emits callable operations and retains base descriptors. Both use the
same signature contracts. Description does not publish a catalog entry.

Compile-time enumeration does not require every descriptor container to be
constant-initialized. Materializing immutable runtime storage during initialization
is acceptable. Keep this distinction explicit when documenting startup cost.

## One support policy

Publish capabilities and reasons for omitted members rather than letting each
adapter silently invent a different subset.

| C++ feature | Initial target behavior |
| --- | --- |
| Public ordinary instance and static members | Describe with complete signatures |
| Private/protected members | Exclude under the public-interface policy |
| Abstract type | Describe interface; expose no native construction operation |
| Declaration-only interface | Describe requirements; emit no native method addresses |
| Const data member | Read capability; no write capability |
| Move-only data member | View capability; copy getter absent; setter only if assignable |
| Deleted or immediate function | Exclude with a diagnostic reason |
| Member template | Require an explicitly selected specialization before exposing it |
| Bit field | Initially exclude explicitly; address-based access is unavailable |
| Public non-virtual inheritance | Describe edges and native cast operations |
| Virtual inheritance | Reject affected native registration with a clear diagnostic until supported |

Do not imply the framework implements full C++ name lookup. Preserve declarations
and inheritance edges; let the runtime own visibility, hiding, and ambiguity.
Where compiler metadata cannot describe a `using` declaration, expose the limitation
or require an explicit policy entry. Do not silently claim raw-C++ equivalence.

### Initial callable capabilities

This table defines the first implementation target, separate from broader metadata
description. Unsupported call capabilities produce an explicit diagnostic; adapters
must not quietly drop qualifiers or choose a different overload. The early prototype
proves the risky method/reference subset; the remaining supported rows need their
own conformance fixtures before being advertised as implemented. Expanding this
target requires updating the table and shared fixtures together.

| Feature | Initial runtime and adapter behavior |
| --- | --- |
| Ordinary unqualified and `const` instance methods | Support with receiver access validation |
| `volatile`, `const volatile`, and `&`/`&&` receiver qualifiers | Retain metadata; reject invocation, replacement registration, and interface binding |
| Native `noexcept` methods | Allow checked runtime invocation; wrapper operations may still throw |
| `noexcept` interface requirements | Reject typed binding and dispatch-table schema construction; allow a native `noexcept` method to satisfy a throwing requirement |
| Value, `T&`, `const T&`, and `T&&` parameters | Support exact type uses and supported public upcasts; require explicit consumption for moves |
| Volatile-qualified parameter or result access | Retain metadata; reject invocation and binding initially |
| Movable value results, including move-only values; `void` | Support owning results and explicit void success |
| Immovable value results and rvalue-reference results | Reject invocation and binding initially |
| Lvalue-reference results | Support erased views and anchored retained extraction; ordinary typed export requires explicit `caller_borrow` policy |
| Unannotated reference results with temporary arguments | Reject before target entry |
| Static functions | Support class-level runtime calls; exclude from instance proxy requirements |
| Native fields | Support available copy/read/write/view operations; retain constness and assignability restrictions |
| Virtual properties and generated operator syntax | Defer public adapter support until the initial method/reference prototype passes |

Reference policies and retained extraction are specified by
[core contracts](contracts.md#exporting-a-reference-to-the-caller). Receiver
qualification restrictions are distinct from reference parameters: rejecting an
`&&`-qualified method does not reject an ordinary method taking `T&&`.

## Generate operations, not assumptions about layout

Prefer generated access and cast thunks at the boundary:

```cpp
// Conceptual native edge operation; generated only for a supported conversion.
const void* derived_to_base(const void* address) {
    return static_cast<const Base*>(static_cast<const Derived*>(address));
}
```

Provide a corresponding mutable operation where access allows it. A base path can
compose these operations. The runtime sees graph edges and operations, not a
requirement that all inheritance or properties can be represented by byte offsets.
This also allows a virtual property to expose read/write operations without
pretending it has a native field address.

Type description should discover required public base descriptors without making
users depend on static registration order. Build descriptor dependencies with a
two-phase builder or another cycle-safe mechanism: establish identities, then
populate and seal records. Self-referential member types must not cause recursive
publication. Do not force complete metadata for every reachable library type just
because it appears as a parameter; identity and type-use information can suffice.

## Implementation boundary and proof

Share public-member predicates and signature builders between native descriptor,
interface schema, dispatch table, and proxy generation. Keep native thunk
generation separate from interface-only queries. The migration guide records the
existing implementation entry points to extract.

Compile fixtures should cover declaration-only interfaces, abstract classes,
qualified overloads, self-referential types, and unsupported-feature diagnostics.
Verify that describing a type leaves `default_registry()` untouched. Cross-check
that native and dispatch table descriptions use the same member identities for the same
interface requirements.

Next: [runtime registry and invocation](runtime.md).
