# Metadata and calls

## Identity and generation

| Contract | Meaning |
| --- | --- |
| `TypeId` | Exact C++ type identity within the program |
| `TypeUse` | Type, cv qualifiers, and reference category |
| `MemberId` | Declaring type and declaration index |
| `Signature` | Parameters, result, receiver qualifiers, and `noexcept` |
| Display name | Diagnostic text and catalog lookup name |

Names do not replace type identity. Member indices are not persistence keys.
Pointer and array descriptors retain their structure.

`describe_class<T>()` and `describe_enum<T>()` cache immutable descriptors without
registration. Base relationships retain their descriptors. Hierarchy lookup and
base casts use that graph. `describe_interface<T>()` records method signatures and
property capabilities without taking method addresses; method bodies are optional.

A `Registry` publishes descriptors explicitly. Publishing the same descriptor is
idempotent. A different descriptor under the same name returns `conflict`.
Catalog locks protect lookup and publication. User calls run outside those locks.

`Reg<T>`, `ensure_registered<T>()`, and free name lookups use `default_registry()`.
Native `Object` construction consults that catalog for full metadata. Pass an
explicit descriptor or construct through `Class` for an independent catalog.

## Ownership

| Value | Lifetime |
| --- | --- |
| Class, enum, and member handles | Retain immutable metadata |
| Owning `Object` | Shares concrete storage |
| Borrowed `Object` | Caller keeps storage alive |
| `ObjectView` | Const-aware access with an optional lifetime anchor |
| `OwnedValue` | Owns an erased result |
| `RetainedRef<T>` | Retains the declared result owner |
| `CallTarget` | Retains callable context and result/export policy |

Metadata handles survive registry destruction. `cast_safe<T>()` needs an owning
object and returns shared ownership; `cast_ref<T>()` also accepts a borrow.
Checked const views cannot grant mutable access. Raw accessors remain unchecked.
An anchor extends lifetime; it does not prevent invalidation within the storage.

## One checked call path

```text
runtime or typed call → argument views → CallFrame → validation
                     → CallTarget → erased result → checked extraction
```

Native constructors, methods, static methods, and supported field reads/writes use
this path. `Function::try_invoke` returns structured diagnostics. The compatibility
`Function::invoke` adapter returns `expected<Object, Error>`.

| Input or outcome | Behavior |
| --- | --- |
| Mutable reference argument | Writes reach the caller |
| Const input or receiver | Mutable access fails before entry |
| Move-only argument | Requires explicit consumption |
| Invalid arity, type, qualifier, or export | Fails validation before entry |
| Target exception | Original exception propagates |
| Void success | Distinct from an invalid receiver |
| Reference result | Requires declared provenance for checked export |

Validation does not promise rollback. Argument preparation, user code, result
capture, and extraction can throw. A failure after target entry can leave effects.
Typed convenience APIs translate diagnostics to `ReflectionError`; `try_call`
returns them through `Result`.

Dynamic reference targets default to restricted raw export. Retained extraction
uses the declared receiver, argument, callable, or external anchor. Native typed
references retain caller-borrow behavior; the older runtime adapter retains its
receiver-anchor policy.

[Runtime usage](../getting-started/runtime.md) and
[reference usage](../getting-started/references.md) include tested examples.
