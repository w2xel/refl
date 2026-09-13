# Layer 5: Dynamic composition

Keep two concepts: a slot backend supplies replaceable operations; `Dyn<T>` is a
convenience object combining that backend, an optional native instance, and a typed
view. Proposed homes are `refl/dynamic/{slots,dyn}.hpp`, with compatibility includes
at the current paths.

The backend depends on contracts and generated interface schemas. `Dyn` also uses
runtime binding and the proxy adapter. The backend must remain usable by a runtime
caller that never instantiates a typed proxy.

The backend implements the common invocation-endpoint contract. `Dyn` provides a
stable live endpoint whose state reference changes only after a successful reset
or detachment. Moving the facade transfers that endpoint identity. Typed views
and observers consume the endpoint without accessing slot containers directly.

## Slots retain complete targets

```cpp
// Target sketch: member identity distinguishes overloads.
struct Slot {
    MemberId member;
    CallTarget baseline;            // optional native implementation
    CallTarget current;             // native, replacement, or decorator
};

auto previous = backend.target(render_member);  // owns its context
backend.replace(render_member, make_target(lambda)).value();
backend.replace(render_member, previous).value();
```

A saved target owns its callable and receiver context. Replacing a map entry must
not destroy a context still referenced by a saved target or an executing call.
Each invocation snapshots its current target before entering user code. This
also defines behavior when a callable replaces itself reentrantly.

A synthetic backend satisfies an interface; it is not an instance of that C++
interface. Keep separate storage identity and interface schema identity instead
of making `T$mock` carry both roles. Structural binding selects operations; native
casts inspect actual storage identity.

## Wrapping is owned composition

```mermaid
flowchart LR
    S[Slot] --> W2[Decorator B]
    W2 --> W1[Decorator A]
    W1 --> C[Replacement callable]
    C --> O[Closure storage]
    N[Baseline native target] --> R[Native receiver storage]
```

Each decorator owns the previous target and its own callable. `wrap(B)` after
`wrap(A)` produces `B(A(target))`. Restoring the baseline drops that chain from the
slot, while active snapshots remain valid. This model avoids saving an invoker
address in one place and keeping its context alive by convention elsewhere.

Give method and property operations the same ownership discipline. A property
target exposes separate read/write/view capabilities; replacing a setter does not
implicitly manufacture addressable storage or a copy getter.

If a callback needs `self`, provide a backend handle with an explicit lifetime
policy. Prefer a weak reference to the containing dynamic state to prevent a
state → slot → closure → state ownership cycle. Lock it when invoked and return
an expired-context diagnostic if unavailable. Do not retain a raw pointer to the
address of a movable facade as the semantic identity of the instance.

## Define mode transitions as state publication

```mermaid
stateDiagram-v2
    [*] --> Dynamic: interface-only construction
    [*] --> Native: construction with real object
    Native --> Hybrid: replace a member
    Hybrid --> Native: restore all native baselines
    Hybrid --> Dynamic: make_dynamic
    Native --> Dynamic: make_dynamic
    Dynamic --> Native: reset with new real object
    Native --> Native: reset
    Hybrid --> Native: reset
```

| Operation | Proposed postcondition |
| --- | --- |
| Interface-only construction | Required slots exist but may be unimplemented |
| Construction with native object | Baselines and current targets refer to that owned object |
| `implement(member, fn)` | Only that exact member changes; signature validated |
| `restore(member)` | Restore native baseline; report unavailable baseline in dynamic mode |
| `wrap(member, fn)` | New owned decorator surrounds the current target |
| `make_dynamic()` | Drop native baselines and targets with native or unknown dependencies; retain declared independent replacements |
| `reset(args...)` | Build a new native object and complete slot table; discard replacements/wrappers on success |

For `make_dynamic`, track native dependencies on generated targets, replacement
registrations, and wrapper chains. A decorator inherits its previous target's
dependencies and declares any additional ones. Replacement registration supplies
owned contexts or lifetime anchors plus a dependency policy: native-dependent,
independent, or unknown. Unknown dependencies default to making the affected slot
unimplemented on detachment; explicitly independent replacements remain callable.

The framework cannot infer dependencies hidden in arbitrary lambda captures.
Marking a callable independent is a caller assertion, and capturing a borrowed
pointer leaves its lifetime with the caller. Offer explicit context ownership and
native-dependency declarations for captures the backend should manage. The
detachment guarantee covers these tracked dependencies; it cannot make an
incorrectly declared borrowed capture safe. Dropping targets from current slots
does not destroy contexts still owned by saved targets or active calls.

`reset` is transactional with respect to dynamic state publication:
construction/binding failure leaves the old state. This does not undo external
side effects in a constructor.

Captured state views and in-progress calls keep their old generation alive after
a transition. A captured view still sees replacements within that generation;
it does not follow a later reset. Fresh calls through `Dyn` or its stable live
endpoint resolve the new generation. Observed adapters over that live endpoint
also follow reset. This distinction concerns state ownership, not protection
against mutations that invalidate references inside a retained object.

Initially require external synchronization for concurrent mutation and invocation.
Immutable target snapshots define lifetime and reentrancy, but do not automatically
make the slot map thread-safe. State this separately from registry thread safety.

## Migration seam and proof

Introduce owned `CallTarget` into existing `MethodSlot`/`PropertySlot` machinery
before moving headers. Replace string keys with `MemberId`, then implement generic
signature-driven callable thunks instead of a separate handwritten arity branch in
every layer. Unsupported signatures must fail at registration or binding.

Prove overload isolation, save/replace/restore lifetime, nested wrapping order,
self-replacement during a call, failed reset, and transition to dynamic mode with
a mixture of native targets and independent replacements. A destruction counter
should demonstrate that saved targets retain exactly the contexts they need.
Include declared native captures, unknown dependencies, closure-owned reference
results, and an old captured view alongside a live endpoint after reset.

Next: [observation](observation.md).
