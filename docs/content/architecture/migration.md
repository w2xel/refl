# Migration and verification

Refactor along one vertical scenario: register `Square`, construct it by name,
bind it to `Drawable`, replace `render`, then observe its completion. Keep each
step working as responsibilities move behind new boundaries.

The milestones below are ordered by semantic dependencies. Header moves alone
are not proof that a layer has been extracted.

## Current progress

The [implemented architecture](current-state.md) records completed extractions.
Step 0 is complete with eight baseline tests passing. The first bounded slice of
step 1 extracts metadata declarations and errors into independently compiled core
headers (commit `eb474e2`). The second completed slice gives public metadata handles const
shared ownership, preserves raw-pointer construction through snapshots, and retains
inherited lookup results. Eleven tests cover the suite, boundaries, and metadata
lifetimes. The step 0a call prototype now passes. Its production call primitives are reused
by the next steps; its state provider remains a test fixture. Core identity/access
validation is complete. Native runtime calls now use common frames and owned targets. Independent
registries and shared typed binding are the next steps. The [current-state page](current-state.md) records compatibility
changes and remaining global hierarchy dependencies.

## Proposed destination

```text
include/refl/
  core/          identity, descriptors, values, call frames, errors
  reflect/       common schemas, native generation, supported-member policy
  runtime/       registry, lookup, checked invocation
  adapters/      typed proxy
  dynamic/       dispatch_table.hpp and dynamic.hpp
  extensions/    observed.hpp
  register.hpp   generation + registry integration
  refl.hpp       compatibility umbrella for core runtime + registration
```

Keep this header-only. Add a Meson dependency object for consumers so include
paths and required compile options have one definition. Fine-grained headers can
come gradually; start with the smallest split that makes a dependency enforceable.

## Reviewable milestones

| Step | Concrete change | Completion evidence |
| --- | --- | --- |
| 0. Establish baseline | Run existing configured tests; add existing dispatch/observation executables to Meson; inventory API/documentation mismatches | Report each component's pass/fail state separately; do not treat unregistered files as tested |
| 0a. Prove the risky contracts | Build a narrow end-to-end prototype across runtime, proxy, slots, and observation before broad migration | Out-parameter, move-only, retained-reference and raw-export rejection scenarios below pass; record initial compile time and dispatch allocations |
| 1. Extract contracts | Introduce complete type uses/member IDs, immutable descriptor handles, explicit value access, dispatch handles and resolved calls; preserve legacy API translation | Qualifier/case collisions are distinguishable; synthetic handles survive source destruction; contracts compile without reflection |
| 2. Unify calls | Add common argument frames, validation, result/lifetime policies, and opaque validated call targets; route runtime calls through them | Out-parameter, const, move-only, reference, void, and exception contract scenarios pass; callable-context anchors survive replacement; mismatched erased targets cannot be installed |
| 3. Separate generation/publication | Extract native/interface schemas; put pools behind `Registry`; remove global dependency from descriptor relationships | Interface-only description has no linker dependence on method definitions; two isolated registries work |
| 4. Unify binding | Centralize hierarchy resolution; separate structural plans, binding states, and resolved calls; route typed calls through common frames | Runtime/proxy conformance matrix agrees; failed rebind preserves old view; shared plans isolate instances and observe later replacements |
| 5. Stabilize dynamic state | Use member-keyed owned targets; separate dispatch table from proxy; implement live dispatch handles, explicit object reset, and declared capture dependencies | Saved targets preserve receiver and policy across reset to another concrete type; wrapping composes; detachment handles tracked and unknown dependencies; captured bindings and live dispatch handles follow their specified generations |
| 6. Isolate observation | Add public Observed adapter, subscription tokens, read-only events, explicit delivery and failure stages | Native dispatch handles work without Dynamic or registry lookup; source lifetime, reentrancy, listener failure, capture/extraction failure, and subscription survival across reset pass |
| 7. Publish the boundaries | Finish header moves behind compatibility includes; align API docs/samples; complete consumer target | Old includes compile; accumulated dependency and standalone-consumer checks pass; diagrams match actual dependencies |

If step 0 exposes failures, record them and fix those blocking the vertical
scenario in separate changes. Do not encode accidental behavior as a permanent
contract merely to obtain a green baseline.

At every extraction step, add its standalone-header and include-boundary checks
immediately. Step 7 completes coverage; it is not the first enforcement point.

### Early prototype acceptance

Keep the prototype limited to enough operations to exercise `void update(int&)`,
a move-only value result, and a reference into a replacement closure. Use the
existing adapters where possible, with the proposed contracts at the boundary;
this is a feasibility checkpoint, not a second production invocation engine.
Promote its fixtures as the corresponding implementation milestones land.

Verify caller mutation through runtime and typed calls, observation without
copying the move-only result, and a retained closure reference surviving slot
replacement. Bind two instances with one structural plan and prove isolation.
Check that subsequent calls see replacements, captured bindings keep their generation
after reset, and observed calls follow fresh state while direct calls emit no
events. Include declared native captures and unknown detachment dependencies.

Exercise the boundary that call-scoped retention alone cannot prove: a closure
returns a reference, an observer replaces its slot, and the caller then accesses
the result. `try_call_retained` must keep the original closure alive. The equivalent
ordinary typed reference call must fail its export-policy check before the target
runs; do not test this by dereferencing an intentionally dangling reference.
Also exercise a declared `caller_borrow` with an independently owned lvalue.

Reject an erased target with the wrong signature without changing the slot. Save
a native target, reset from `Square` to `Triangle`, and verify that the saved target
still invokes the old receiver while a restored baseline invokes the new one.
Test `reset(Object)` after detachment and a failed incompatible reset. Inject
preparation, result-capture, and typed-extraction failures to verify target effects
and the completion-event boundary, including an extraction failure after delivery.

Use the [initial capability table](generation.md#initial-callable-capabilities) as
the acceptance scope. Compile explicit rejection fixtures for receiver ref qualifiers,
volatile access, `noexcept` interface requirements, and unsupported result categories.
Do not turn prototype limitations into silently reduced signatures. If evidence
requires revising the subset, update the table, diagnostics, and fixtures before
broad migration. Promote the [usage sketches](usage.md) into consumer samples as
their operations become available.

Record clean consumer compile time and direct/runtime/proxy/slot/observed call
costs and allocation counts on the same toolchain. Use this evidence to decide
whether to expand the supported surface; keep result-storage optimizations behind
the established semantic contract.

### Recorded baseline limitations

A review of revision `7eb6bbe` using the configured GCC 16.2.0 environment found:

- The strict MkDocs build passed.
- The default C++ test build stopped with an internal compiler error during LTO
  linking of `test_dyn`; the complete test suite did not run.
- With `-Db_lto=false`, `selfcheck` and `refl_api` passed, but `test_dyn` failed
  assembly with a duplicate `Mockable<Point>::impl_trampoline` symbol.
- `test_mockable.cpp` and `test_hooks.cpp` were not registered in Meson and were
  not run by these checks.

These are observations about that revision and toolchain, not permanent compiler
limitations or proof that the target design works. Stabilize and rerun the baseline
before claiming end-to-end feasibility; record configuration changes separately.

### Stabilized build baseline

The baseline stabilization following documentation revision `89c21b8` uses the
configured GCC 16.2.0 toolchain, Meson 1.10.2, C++26 reflection, optimization level 2,
warnings as errors, and GCC's static analyzer. LTO is now off by default: its
`lto_read_decls` compiler crash was reproduced and remains an opt-in compiler
investigation, not a supported baseline configuration.

The stabilization fixes implementation-thunk duplicate symbols, signature collisions
between method slots, missing inherited dispatch metadata and receiver adjustment,
native class-level lookup from `Dyn`, saved callable/property ownership, and nested
wrapper ownership. The proxy sample selects its constructor by signature and checks
lookup results. Mockable and Hooks tests are registered, and all samples run as
smoke tests. These are repairs to the existing API, not adoption of the proposed
`CallTarget`, `MemberId`, registry, or reference-export contracts.

Reproduce the baseline in a fresh directory:

```sh
meson setup builddir
meson compile -C builddir -j 2
meson test -C builddir --print-errorlogs
```

An existing directory configured with LTO needs
`meson configure builddir -Db_lto=false` first. The expected default run has eight
entries: `selfcheck`, `refl_api`, `dyn_api`, `mockable_api`, `hooks_api`, and the three
sample smoke tests. The new regressions cover saved-context retention and release,
self-replacement, nested wrapping and move/restore, and inherited dispatch through
a non-first base. The inheritance fixture registers base types explicitly because
the current resolver still depends on the global metadata pool.

Verification on 2026-09-13:

| Check | Result |
| --- | --- |
| Full build with the settings above | All eight executables compiled and linked |
| Default Meson test run | 8 passed, 0 failed, including every sample |
| Valgrind on `dyn_api`, `mockable_api`, and `hooks_api` | All three passed; zero errors and all heap blocks freed |
| Strict MkDocs build and `git diff --check` | Passed |

The memory check used:

```sh
meson test -C builddir --no-rebuild --print-errorlogs \
  --wrapper 'valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite,indirect' \
  dyn_api mockable_api hooks_api
```

A passing baseline does not prove the proposed qualifier, reference, detachment,
or observation contracts; step 0a and the subsequent conformance milestones remain
necessary.

## Legacy implementation evidence

The following names and source links describe revision `29ddfd2`, not the target
API. They preserve the evidence for the [repository assessment](assessment.md).

```mermaid
flowchart TB
    H["hooks.hpp · Hooks inherits Dyn"] --> D["dyn.hpp · real object + slots + proxy"]
    D --> M["mockable.hpp · synthetic ClassInfo + slots"]
    D --> P["dyn/proxy.hpp · generated typed dispatch"]
    M --> P
    P --> R["refl.hpp · generation + pools + Object + lookup + invocation"]
    M --> R
    D --> R
```

These are header dependencies, with a few redundant edges omitted. The public
“two layers” description hides both reusable components and an optional extension.

| Evidence in this revision | Architectural consequence | Direction |
| --- | --- | --- |
| [`refl.hpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/include/refl/refl.hpp): `RegistrarHolder::make_info`, global pools, `Object`, hierarchy helpers, and public handles share one header | Compiler reflection, process policy, and runtime semantics cannot be consumed independently | Extract contracts first, then generation and runtime services |
| Same header: `normalize_type` lowercases and removes qualifiers, while `extract_arg` compares display names and performs upcasts | Lookup identity and invocation compatibility answer different questions through string conventions | Give identity, complete signatures, and compatibility separate representations |
| Same header: `Object` owns `ClassInfo`; `Class`, `Function`, and `Field` hold raw metadata pointers | Handle validity depends on where metadata came from; synthetic metadata is not necessarily process-lived | Handles retain immutable descriptor storage |
| [`proxy.hpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/include/refl/dyn/proxy.hpp): `populate` resolves structure and `TypedMethod` builds its own argument storage | Typed calls can diverge from core forwarding and resolution semantics | Produce a binding plan and reuse the call-frame contract |
| [`mockable.hpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/include/refl/mockable.hpp): synthetic `T$mock` metadata, name-keyed slots, raw `ctx`, and `proxy()` convenience | Interface identity, storage identity, overload selection, and adapter dependency are intertwined | Separate interface from storage; key slots by member identity; own callable context |
| [`dyn.hpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/include/refl/dyn.hpp): `wire_real_slots`, `restore`, `wrap`, and mode transitions coordinate several stores and pointers | Lifetime and transition correctness depend on each operation remembering every related store | Publish complete backend states and compose owned call targets |
| [`hooks.hpp`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/include/refl/hooks.hpp): derived class reaches `mockable_`; callbacks and saved property contexts live in separate maps | Observation is coupled to dynamic storage layout and wrapper lifetime | Observed endpoint adapter outside replacement chains, plus connection tokens |
| [`tests/meson.build`](https://github.com/swuerl/cpp_runtime_reflection/blob/29ddfd2/tests/meson.build) registers `selfcheck`, `test_refl`, and `test_dyn`, but not existing `test_mockable.cpp` or `test_hooks.cpp` | A successful default test run does not cover all architectural components | Make each public layer a first-class build/test target |

### Legacy documentation drift

The existing Dyn page places `connect` and `on_change` on `Dyn`, while the source
places them on `Hooks`; it calls `Dyn` non-movable while the source declares move
operations. It also describes dispatch features that the current proxy no longer
exposes, such as property subscripting and static dispatch fields. The core design
page contains conflicting accounts of inheritance hiding and metadata ownership.

## Naming and compatibility map

The target names below are used throughout the architecture chapters. Historical
API names, superseded design names, and old include paths appear only in this
migration guide. A name in an earlier sketch does not imply an implemented API.

| Earlier name or term | Target name | Migration meaning |
| --- | --- | --- |
| `Dyn<T>`; inconsistent `Dynamic<T>` spelling | `Dynamic<Interface>` | Use the full name for the composition facade; keep the abbreviated API as compatibility sugar |
| `Mockable<T>`; slot backend | `DispatchTable<Interface>` | Replaceable operations are useful beyond mocking |
| `Hooks<T>` | `Observed<Source>` via `observe(...)` | Compose a separate observed adapter instead of inheriting from the dynamic facade |
| Connection token; `connection.disconnect()` | `Subscription`; `subscription.unsubscribe()` | The token owns a subscription; destruction unsubscribes |
| `EndpointHandle` | `DispatchHandle` | Retained access to member dispatch, exposed by `dispatch()` |
| Internal `BoundView` | `BindingState` | Per-instance plan and dispatch state, separate from the public proxy |
| `CallSnapshot` | `ResolvedCall` | Retains a selected target and receiver without copying receiver contents |
| `InvocationEvent` | `CallCompletedEvent` | Describes successful completion only |
| `make_dynamic()`; dynamic mode | `detach_native()`; detached mode | Drops native baselines and affected targets; dispatch itself was already dynamic |
| `ClassInfo`, `FunctionInfo`, `FieldInfo`, and related metadata records | `ClassDescriptor`, `FunctionDescriptor`, `FieldDescriptor`, and corresponding `*Descriptor` names | Immutable native metadata uses one suffix |
| `Class`, `Function`, `Field`, and related metadata access objects | `ClassHandle`, `FunctionHandle`, `FieldHandle`, and corresponding `*Handle` names | Handles retain their descriptors |
| Virtual fields used as a generic term | Properties | `FieldDescriptor` denotes native storage; `PropertyDescriptor` denotes capabilities |
| Unqualified access to live or captured state | `dispatch()` and `capture_binding()` | Live dispatch follows reset; a captured `Proxy<Interface>` keeps its generation |
| `reset(args...)` with an implied concrete type | `reset(Object)`; `reset_native<T>(args...)` | Supply new storage or name the concrete constructor explicitly; the interface does not choose it |
| Unrestricted typed reference forwarding | `caller_borrow` policy or `try_call_retained<T>` | Raw typed export requires independent caller lifetime; dispatch-owned references retain the actual result anchor |
| `Reg<T>` and `ensure_registered<T>()` | `register_type<T>(registry)` | Explicit registration is the primary spelling; the default registry remains available |

Keep `Proxy<Interface>`, `Registry`, `TypeUse`, `BindingPlan`, `CallFrame`, and
`CallTarget`. Native descriptions use `*Descriptor`; structural requirements use
`InterfaceSchema`. The target `Object` owns storage, while `ObjectView` is possibly
retained. Translate existing borrowing constructors deliberately rather than
silently making them allocate or extend lifetimes.

| Earlier chapter title | Target chapter title |
| --- | --- |
| Contracts and values | Core contracts |
| Reflection generation | Reflection and descriptor generation |
| Runtime services | Runtime registry and invocation |
| Dynamic composition | Dynamic dispatch |
| Observation | Call observation |

Typed binding keeps its chapter title. Markdown page paths remain stable so links
continue to work; navigation, headings, and diagrams use the target titles.

| Existing or previously proposed include | Target include |
| --- | --- |
| `refl/dyn.hpp`; proposed `refl/dynamic/dyn.hpp` | `refl/dynamic/dynamic.hpp` |
| `refl/mockable.hpp`; proposed `refl/dynamic/slots.hpp` | `refl/dynamic/dispatch_table.hpp` |
| `refl/hooks.hpp`; proposed `refl/extensions/hooks.hpp` | `refl/extensions/observed.hpp` |
| `refl/dyn/proxy.hpp` | `refl/adapters/proxy.hpp` |

Retain existing include paths as forwarding compatibility facades. In particular,
the observation facade supplies integration with dynamic dispatch and typed proxies;
the core observed adapter does not depend on those implementations. API wrappers
must translate changed semantics, not merely alias incompatible types.

### Existing implementation seams

Extract `populate` into a `BindingPlan` builder and move `TypedMethod` argument and
result handling onto the shared `CallFrame`. Move `Mockable::proxy()` convenience
into integration code. Introduce owned `CallTarget` values in the current
`MethodSlot` and `PropertySlot` stores, then key operations by `MemberId`.
Replace `Hooks` access to `mockable_` with public dispatch resolution and checked
invocation. Share generation queries across `RegistrarHolder::make_info`, native
registration, dispatch tables, and proxies. Preserve interface-only generation
without taking addresses of undefined methods.

## Compatibility is an explicit translation

Keep `Reg<T>`, global `find_class`, and existing umbrella paths while introducing
explicit registries. Preserve simple `Proxy<T>` construction and typed dispatch.
New `try_*` operations can sit beside current throwing conveniences.

Some semantics need deliberate changes: case/qualifier-preserving matching,
reference provenance, name-only overloaded replacement, const-borrow behavior,
and mode transitions. For each, document the old behavior, replacement spelling,
and rejection/diagnostic. A legacy string lookup may resolve a unique candidate;
it must report ambiguity when stripped qualifiers no longer select exactly one.
Do not perpetuate unsafe matching in the new core to preserve convenience syntax.

Compatibility wrappers should call the new implementation. Avoid maintaining a
second hierarchy resolver or invocation engine behind the old headers.

## Contract matrix

Use small fixtures across entry points instead of separate large scenario suites
whose semantics quietly drift. These are proposed checks, not claims about current
coverage.

| Fixture | Runtime | Proxy | DispatchTable / Dynamic | Call observation |
| --- | --- | --- | --- | --- |
| `void update(int&)` | Caller changes | Same caller changes | Replacement preserves reference | Sees completion |
| Const receiver with mutating member | Reject | Reject or unavailable syntax | Same access validation | No success event |
| Move-only value parameter/result | Explicit consumption | Same category behavior | Generic callable thunk | Observe without copy |
| Return reference into argument | Correct anchor/borrow | Document typed borrow | Same provenance policy | Retention requires capability |
| Return reference into replacement closure | Retain invoked context | Reject ordinary typed export; offer retained extraction | Retained result survives replacement | Replace during delivery; caller's retained result stays valid |
| Explicit `caller_borrow` result | Preserve borrow and access policy | Validate policy before target entry; caller retains referent | Recheck selected policy after replacement | Caller lifetime includes delivery and later use |
| Saved native target across reset | Retain bound receiver | New calls through live views use new generation | Saved target uses old receiver; baseline restore uses new receiver | Subscriptions follow live interface identity |
| Two instances sharing a structural plan | Resolve each dispatch handle | No instance state in plan | Replacing one leaves the other intact | Events belong to the invoked adapter |
| Two overloads sharing a name | Select exact declaration | Bind each independently | Replace one only | Subscribe to one only |
| Ambiguous repeated base | Diagnose | Bind fails | Native wiring uses same resolver | No synthetic success |
| User exception | Propagate target exception | Same | Same | No success event |
| Result capture or typed extraction throws | Target effects may remain | Extraction may fail after successful erased call | Preserve selected owners through failure | No event on capture failure; event already delivered before typed extraction failure |
| Retained member handle | Descriptor remains alive | Plan retains descriptors | Synthetic descriptor remains alive | Event cannot escape unretained storage |

Add lifecycle sequences for save → replace → restore, wrap → wrap → restore,
bind → failed rebind → call, native → hybrid → detached → reset, and subscribe →
unsubscribe/destroy → invoke. Use destruction counters and sanitizers where the
configured compiler supports them. Test behavior and lifetime, not private map
layouts or pointer spellings.

## Enforce architecture in the build

Compile a translation unit including each intended public header alone. Keep a
contracts/runtime-only fixture free of reflection syntax and `<meta>`; generation
fixtures use the configured reflection-capable compiler. This makes the separation
testable without promising support for an additional compiler or older language
standard yet.

Add an include-boundary check: `core` cannot include `reflect`, `runtime`, or
adapters; `runtime` cannot include generation; dispatch tables cannot include proxy;
no lower layer includes observation. Call observation depends on contracts/runtime
and cannot include dynamic internals. Integration/umbrella headers are the deliberate
exceptions.
Within runtime, `invoke.hpp` cannot include registry or lookup headers. A standalone
fixture must observe a pre-bound synthetic dispatch handle through checked invocation
without catalog or name-resolution headers. The observation adapter's direct runtime
dependency is invocation only.
Exercise public operations from small consumer programs rather than granting tests
special access to internals.

Track clean consumer compile time, descriptor construction/registration cost,
bind cost, steady-state direct/runtime/proxy/slot calls, and allocation counts.
Compare relative costs on the same machine and toolchain. Optimize after the shared
contract is stable; proposed boundaries do not imply allocation-free dispatch.

## Documentation verification

Build the section with the existing development environment:

```sh
mkdocs build --strict -f docs/mkdocs.yml
mkdocs serve -f docs/mkdocs.yml
```

The build checks navigation and Markdown links. It does not execute C++ sketches
or parse Mermaid diagrams in a browser. Preview the dependency, call-sequence, and
state diagrams in both theme modes, and run every Mermaid block through the
matching Mermaid parser when editing diagram sources. Sequence-message text must
not contain an unescaped semicolon: Mermaid treats it as a statement separator.
Use plain wording or the documented `#59;` escape (see
[Mermaid sequence-diagram escaping](https://mermaid.js.org/syntax/sequenceDiagram.html#entity-codes-to-escape-characters)).
Material loads the Mermaid runtime in the browser, which may require network
access under the default theme configuration;
an offline site needs a separate asset-bundling decision. Diagram source remains
readable in Markdown.

When adopting a target API, promote its sketch into a compiled sample, update the
corresponding API reference, and label the architecture decision implemented with
its revision. Until then, retain the distinction between proposed contracts and
current behavior.
