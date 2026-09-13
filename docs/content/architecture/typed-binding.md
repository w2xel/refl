# Layer 4: Typed binding

`Proxy<Interface>` should mean “a typed view of an object satisfying this interface.”
It should work equally well with a native object and a slot backend. It does not
own replacement policy, observe calls, or manufacture a concrete `Interface`.

Proposed home: `refl/adapters/proxy.hpp`. Keep `refl/dyn/proxy.hpp` as a forwarding
include during migration. Dependencies are runtime services and generated interface
schemas; neither `Dyn` nor `Hooks` belongs below this layer.

## Preserve the useful current API

**Current API**, as demonstrated by `samples/proxy.cpp`:

```cpp
struct IDrawable { int render(int scale) const; };
struct Square {
    int side;
    int render(int scale) const { return side * side * scale; }
};

refl::ensure_registered<Square>();
auto square = std::make_shared<Square>(4);
refl::Proxy<IDrawable> view(square);
int pixels = view->render(2);
```

The interface has no method bodies and the implementation has no inheritance
relationship to it. Keep this property through every internal change.

## Bind requirements to operations

```cpp
// Target sketch.
struct BindingEntry {
    MemberId requirement;
    BoundOperation operation;      // target + signature + receiver path
};
struct BindingPlan {
    InterfaceHandle interface;
    TypeHandle implementation;
    std::vector<BindingEntry> entries;
};

Result<Proxy<Drawable>> try_bind(Object object);
```

Binding validates the entire interface before publishing a new view. A failure
contains the member, expected signature/capability, and found candidates. Failed
rebind leaves the previous binding usable. Build a complete new state and swap
it in; a move operation should transfer that state without rerunning fallible
structural lookup.

Cache immutable structural plans only after profiling. A cache key must include
interface identity, descriptor identity/version, and relevant access policy.
Instance pointers, slot contents, and closure owners do not belong in a shared
structural plan.

| Requirement | Initial binding rule |
| --- | --- |
| Parameter and return type uses | Exact match, preserving qualifiers and references |
| Const method | Implementation must provide the const receiver guarantee |
| Mutable method | Const implementation may satisfy it if the full remaining contract matches |
| `noexcept` method | Implementation must satisfy the guarantee; otherwise reject |
| Read-only property | Matching read/view capability |
| Writable property | Matching read/write capabilities |
| Ambiguous inherited member | Reject binding with path/candidate details |
| Unsupported qualifier combination | Reject explicitly; never select the first declaration |

If native-looking syntax cannot safely expose a `noexcept` member because wrapper
validation or result storage can throw, reject that interface requirement initially.
Retaining metadata is not itself a promise that every adapter supports it.

## Share call semantics

```mermaid
sequenceDiagram
    participant U as Caller
    participant P as Typed view
    participant F as Common call frame
    participant T as Bound target
    U->>P: update(int& output)
    P->>F: Borrow output; retain its category
    F->>F: Validate runtime-dependent constraints
    F->>T: Invoke with adjusted receiver
    T->>U: Write to original output
    T-->>F: Explicit result and lifetime
    F-->>P: Checked typed extraction
    P-->>U: Typed return
```

Do not copy every argument into a decayed tuple in the adapter. Use the common
argument builder so `T&`, `const T&`, and `T&&` behave consistently with runtime
invocation. A typed reference return is still a C++ borrow; its validity must
follow the underlying object's lifetime and mutation rules. Offer a retained-view
operation when a caller needs lifetime extension.

A const proxy grants read-only access to its receiver. It does not make all
objects reachable through pointer-valued members deeply const. Document that
ordinary C++ distinction. Property access syntax should be offered only where it
preserves the declared capability; a raw mutable reference can bypass later
observation.

Keep operators on the same resolver and call contract as named methods. Explicitly
specify whether a class-valued operator returns a concrete value or a typed view
of an interface; do not infer that every class result uses the receiver's interface.

## Migration seam and proof

Extract `populate` into a plan builder, then replace `TypedMethod` argument/result
handling with the shared frame. Expose `try_bind` while retaining a throwing
constructor for compatibility. Remove the slot backend's dependency on this
adapter by moving `Mockable::proxy()` sugar to an integration header or free factory.

Run the structural sample with both unrelated native implementations and a slot
implementation. Verify mixed-return overloads, ref-qualified rejection/support,
const requirements, failed rebinding, move behavior, and inherited receiver paths.
Use the same out-parameter and move-only scenarios as runtime invocation.

Next: [dynamic composition](dynamic.md).
