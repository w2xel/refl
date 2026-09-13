# Repository assessment

The strongest existing separation is structural binding versus replaceable
dispatch: binding is useful independently of replacement. Preserve that distinction
and make it visible in the public architecture. The largest pressure point is the
shared semantic contract underneath them, currently distributed across headers.

This is a static architecture review of revision `29ddfd2`, including headers,
samples, tests, Meson configuration, and existing documentation. It does not claim
that the C++ suite was executed or that every implementation defect was identified.
Source links in the migration guide pin the reviewed revision so future refactoring
does not erase the evidence behind the recommendations. This assessment describes
responsibilities using the target vocabulary; it does not claim that the target
symbols already exist in the implementation.

## What exists

Structural binding and replaceable dispatch are already separate components,
but their dependencies and call semantics still overlap. The reviewed code has
these architectural pressure points:

| Existing responsibility | Architectural consequence | Target direction |
| --- | --- | --- |
| Generation, global registration, values, and invocation share one header | Runtime consumption requires compiler reflection and process policy | Separate core contracts, generation, and runtime registry and invocation |
| Lookup strips qualification and invocation compares display names | Identity and compatibility can disagree | Use type identity, complete signatures, and explicit compatibility |
| Some public handles borrow metadata without retaining it | Synthetic metadata can expire before a handle | Retain immutable descriptors in metadata handles |
| Typed forwarding builds its own argument storage | Typed and runtime calls can behave differently | Reuse binding plans and the common call frame |
| Replaceable dispatch combines synthetic identity, name keys, and raw contexts | Storage identity, overload selection, and lifetime are entangled | Use `DispatchTable<Interface>`, member IDs, and owned targets |
| State transitions coordinate several independent stores | Replacement and reset depend on scattered lifetime bookkeeping | Publish complete dynamic state and retain resolved calls |
| Observation reaches into dispatch storage | Subscriptions depend on replacement layout | Use `Observed<Source>` over a `DispatchHandle` |
| Some component tests are not registered in the build | A passing default run does not cover the full architecture | Register and verify every public component |

The [legacy implementation evidence](migration.md#legacy-implementation-evidence)
contains the original dependency diagram, symbols, and revision-pinned source links.

## Keep these strengths

The current `Object` already separates a storage owner from the address being
viewed. That is the right starting point for base subobjects and retained field
references. Extend it with explicit constness and lifetime provenance.

The runtime exposes handles rather than asking users to manipulate metadata
vectors directly. Preserve that API shape while moving the metadata behind an
internal, immutable representation.

[`samples/proxy.cpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/samples/proxy.cpp)
demonstrates the central product feature: unrelated `Square` and `Triangle` types
bound to the same declaration-only renderer interface. Promote this to an
architectural acceptance scenario.

The core tests already exercise inheritance, ambiguity, ownership, and reference
behavior; the proxy tests include failed rebinding and moved-from behavior. Use
these scenarios as seeds for contracts shared across layers, while reviewing
whether each expectation is intended behavior or a historical limitation.

## Decisions in priority order

1. Define identity and complete signatures before expanding overload support.
   Otherwise every new adapter inherits lossy matching rules.
2. Define storage, reference, and callable ownership before expanding reset,
   wrapping, or cross-object observation. These are graph-lifetime decisions.
3. Centralize resolution and argument binding before adding more dispatch syntax.
   A typed view must preserve the semantics of the object it exposes.
4. Separate metadata construction from registry publication. Local registries and
   synthetic backends should not need global mutable state to behave correctly.
5. Make observation an optional consumer of dispatch handles and checked runtime
   calls, with explicit delivery and unsubscription rules. Keep `Dynamic` integration
   separate from the observation adapter.

The [recorded baseline](migration.md#recorded-baseline-limitations) adds execution
evidence from the subsequent documentation review at `7eb6bbe`. It distinguishes
passing core tests from build failures and unregistered tests; it does not change
the revision or static-review scope of the source assessment above.

## Documentation drift is a boundary signal

The existing API documentation disagrees with the source about which component
owns observation, whether dynamic facades are movable, and which dispatch features
remain available. The core design page also gives conflicting accounts of
inheritance hiding and metadata ownership. Exact historical spellings and examples
are recorded under [legacy documentation drift](migration.md#legacy-documentation-drift).

These discrepancies are evidence that the conceptual model has not kept pace
with component changes. This section provides a proposed architecture, not a
replacement API reference. During migration, align each existing API page and
sample with its owning layer and verify examples before presenting them as current.

Continue with [core contracts](contracts.md).
