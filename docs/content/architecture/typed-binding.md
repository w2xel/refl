# Typed binding

`Proxy<Interface>` is a typed view over an owning native object or
live or captured dispatch handles. It does not own slot replacement or observation policy.
The implementation is in `refl/dyn/proxy.hpp`.

## Native binding

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

Native targets use generated operations. Dynamic views resolve the current target
through a dispatch handle before each call. Captured handles retain one generation;
live handles follow state publication. Both use the common validator.

Native typed raw references retain the caller-borrow policy. Dynamic replacements
default to restricted export. `try_call_retained` retains the selected target's
declared result anchor across replacement and observation.

## Verification

`binding_contracts` checks shared plans, isolated receivers, inherited adjustment,
mutable arguments, const rejection, move-only inputs/results, qualifier mismatch,
failed rebinding, moves, and slot replacement. Existing Dyn/Proxy/Hooks
suites cover overloads, properties, operators, and saved slot behavior.

Next: [dynamic dispatch](dynamic.md).

See [tested usage](../getting-started/binding.md).
