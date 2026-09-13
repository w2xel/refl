# Layer 4: Typed binding

`Proxy<Interface>` is a typed view of an owning object. It binds native metadata or
Mockable metadata. It does not own slot replacement or observation policy.
The implementation is in `refl/dyn/proxy.hpp`.

## Bind without registration

```cpp
struct Drawable { int render(int scale) const; };
struct Square {
    int side = 4;
    int render(int scale) const { return side * side * scale; }
};

auto type = refl::Class(refl::describe_class<Square>());
auto object = type.find_constructor({})->call().value();
refl::Proxy<Drawable> view(object);
int pixels = view->render(2); // 32
```

The interface needs no method bodies or inheritance relationship. Discovery by
name is optional; binding uses the object's retained descriptor.

## Ownership and selection

| Part | Contents |
| --- | --- |
| `BindingPlan` | Descriptor and selected method/property locations; no receiver addresses |
| `BindingState` | Owning object and shared plan |
| Typed fields | Owned call targets bound to that instance |

Live views of the same interface and descriptor share a plan. The weak plan cache
does not retain unused plans or descriptors. Binding uses the runtime hierarchy
lookup helpers and checks complete signatures, property types, constness, hiding,
and ambiguity. Each overload binds its own adjusted receiver.

`view.bind(object)` prepares the new binding before replacing the old one. Failure
leaves the old view usable. Moving a proxy transfers its prepared state; it does
not repeat lookup. A moved-from proxy is unbound.

## Calls

```text
typed method/property → argument views → invoke_target → checked extraction
```

Arguments retain their constness and value category. Mutable references write to
the caller. Move-only values require explicit consumption. Const proxies reject
mutable receiver calls. Typed wrappers can throw diagnostics and do not advertise
`noexcept`.

Native targets use generated operations. Mockable targets adapt their backend
trampolines at the boundary; the next call reads the current slot. Mock
implementations preserve parameter qualifiers and support arbitrary parameter counts.
The adapter uses the common validator, not a separate argument-validation path.

Typed raw reference returns retain the existing caller-borrow policy: callers keep
the referent alive. Production dynamic provenance and retained-reference integration
remain part of the dynamic-state migration. The strict core API already supports
explicit export policies and `try_call_retained`.

## Verification

`binding_contracts` checks shared plans, isolated receivers, inherited adjustment,
mutable arguments, const rejection, move-only inputs/results, qualifier mismatch,
failed rebinding, moves, and slot replacement. Existing Dyn/Proxy/Mockable/Hooks
suites cover overloads, properties, operators, and saved slot behavior.

Removed: raw overload arrays, copied argument tuples, string-normalized typed
signature matching, and the second lookup pass during successful rebind.

Next: [dynamic dispatch](dynamic.md).
