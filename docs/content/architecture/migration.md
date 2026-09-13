# Migration and verification

Refactor along one vertical scenario: register `Square`, construct it by name,
bind it to `Drawable`, replace `render`, then observe its completion. Keep each
step working as responsibilities move behind new boundaries.

The milestones below are ordered by semantic dependencies. Header moves alone
are not proof that a layer has been extracted.

## Proposed destination

```text
include/refl/
  core/          identity, descriptors, values, call frames, errors
  reflect/       common schemas, native generation, supported-member policy
  runtime/       registry, lookup, checked invocation
  adapters/      typed proxy
  dynamic/       slots and Dyn composition
  extensions/    observation
  register.hpp   generation + registry integration
  refl.hpp       compatibility umbrella for core runtime + registration
  dyn.hpp        compatibility facade for dynamic composition
  mockable.hpp   compatibility facade for slot backend
  hooks.hpp      compatibility facade for observation + Dyn integration
  dyn/proxy.hpp  compatibility include for the typed adapter
```

Keep this header-only. Add a Meson dependency object for consumers so include
paths and required compile options have one definition. Fine-grained headers can
come gradually; start with the smallest split that makes a dependency enforceable.

## Reviewable milestones

| Step | Concrete change | Completion evidence |
| --- | --- | --- |
| 0. Establish baseline | Run existing configured tests; add existing mockable/hooks executables to Meson; inventory API/documentation mismatches | Report each component's pass/fail state separately; do not treat unregistered files as tested |
| 0a. Prove the risky contracts | Build a narrow end-to-end prototype across runtime, proxy, slots, and observation before broad migration | Out-parameter, move-only, and closure-reference scenarios below pass; record initial compile time and dispatch allocations |
| 1. Extract contracts | Introduce complete type uses/member IDs, immutable descriptor handles, explicit value access, endpoints and call snapshots; preserve legacy API translation | Qualifier/case collisions are distinguishable; synthetic handles survive source destruction; contracts compile without reflection |
| 2. Unify calls | Add common argument frames, validation, result/lifetime policies, and owned call targets; route runtime calls through them | Out-parameter, const, move-only, reference, void, and exception contract scenarios pass; callable-context anchors survive replacement |
| 3. Separate generation/publication | Extract native/interface schemas; put pools behind `Registry`; remove global dependency from descriptor relationships | Interface-only description has no linker dependence on method definitions; two isolated registries work |
| 4. Unify binding | Centralize hierarchy resolution; separate structural plans, bound views, and call snapshots; route typed calls through common frames | Runtime/proxy conformance matrix agrees; failed rebind preserves old view; shared plans isolate instances and observe later replacements |
| 5. Stabilize dynamic state | Use member-keyed owned targets; separate slot backend from proxy; implement live endpoints and declared capture dependencies | Saved targets survive replacement; wrapping composes; detachment handles tracked and unknown dependencies; captured views and live endpoints follow their specified generations |
| 6. Isolate observation | Add public observed endpoint adapter, connection tokens, read-only events, explicit delivery policy | Native endpoints work without Dyn; source lifetime, reentrancy, listener failure, and subscription survival across reset pass |
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
Check that subsequent calls see replacements, captured views keep their generation
after reset, and observed calls follow fresh state while direct calls emit no
events. Include declared native captures and unknown detachment dependencies.

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

| Fixture | Runtime | Proxy | Slots / Dyn | Observation |
| --- | --- | --- | --- | --- |
| `void update(int&)` | Caller changes | Same caller changes | Replacement preserves reference | Sees completion |
| Const receiver with mutating member | Reject | Reject or unavailable syntax | Same access validation | No success event |
| Move-only value parameter/result | Explicit consumption | Same category behavior | Generic callable thunk | Observe without copy |
| Return reference into argument | Correct anchor/borrow | Document typed borrow | Same provenance policy | Retention requires capability |
| Return reference into replacement closure | Retain invoked context | Typed return remains a borrow | Retained result survives replacement | Preserve result anchor through delivery |
| Two instances sharing a structural plan | Resolve each endpoint | No instance state in plan | Replacing one leaves the other intact | Events belong to the invoked adapter |
| Two overloads sharing a name | Select exact declaration | Bind each independently | Replace one only | Subscribe to one only |
| Ambiguous repeated base | Diagnose | Bind fails | Native wiring uses same resolver | No synthetic success |
| User exception | Propagate target exception | Same | Same | No success event |
| Retained member handle | Descriptor remains alive | Plan retains descriptors | Synthetic descriptor remains alive | Event cannot escape unretained storage |

Add lifecycle sequences for save → replace → restore, wrap → wrap → restore,
bind → failed rebind → call, native → hybrid → dynamic → reset, and connect →
disconnect/destroy → invoke. Use destruction counters and sanitizers where the
configured compiler supports them. Test behavior and lifetime, not private map
layouts or pointer spellings.

## Enforce architecture in the build

Compile a translation unit including each intended public header alone. Keep a
contracts/runtime-only fixture free of reflection syntax and `<meta>`; generation
fixtures use the configured reflection-capable compiler. This makes the separation
testable without promising support for an additional compiler or older language
standard yet.

Add an include-boundary check: `core` cannot include `reflect`, `runtime`, or
adapters; `runtime` cannot include generation; slots cannot include proxy; no lower
layer includes hooks. Observation depends on contracts/runtime and cannot include
dynamic internals. Integration/umbrella headers are the deliberate exceptions.
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
