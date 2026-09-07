# Design

## Idea

A user of this framework wraps classes in containers:

```cpp
refl::Refl<MyClass> obj;
```

The existence of this alone already allows finding the class in the global
class pool and creating it.

```cpp
auto my_class_class = *refl::find_class("MyClass");
```

Holding a class handle allows querying functions and constructing it.

```cpp
auto construct = *my_class_class.find_constructor("int", "int");
auto my_obj = *construct.call(1, 3);
```

The returned `Object` is a type-erased, shared-ownership handle (backed by
`std::shared_ptr<void>`) — you don't need to know the C++ type to construct
or invoke methods.  Copies share ownership.  Functions are called on the
`Object` directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
std::any result = fn.invoke(my_obj, arg1, arg2);
```

When you need the concrete type, use the safe cast, which checks the class
name at runtime and returns a `std::shared_ptr<T>` that shares ownership
with the Object — it succeeds for the object's own class and for any base
class (upcast):

```cpp
auto result = my_obj.cast_safe<MyClass>();
if (result) { auto sp = result.value(); /* sp->... */ }
```

Fields can be found by name and get/set through type-erased handles:

```cpp
auto field = *my_class_class.find_field("x");
std::any old = field.get(my_obj);
field.set(my_obj, std::any(42));
```

Note the dereferences — the return values should be `std::expected`.
Function results are returned as `std::any` — use `std::any_cast<T>` to
extract (empty for void functions).

## Mechanism

The framework leans on C++26 compile-time reflection (P2996) and expansion
statements (P1306):

- `^^T` produces a `std::meta::info` for a type.
- `std::meta::nonstatic_data_members_of` enumerates members.
- `template for` expands a statement over each reflected member.
- `std::define_static_array` (P3491) materialises a reflection query result
  into static storage so it can feed a `template for` range (the query
  functions return `std::vector<info>`, which allocates and so cannot itself
  be a constant expression).

The global class pool is populated by the static-initialisation side effect of
instantiating `Refl<T>`; the registration data is gathered at compile time via
reflection and emitted as a static object.

## Status

Initial implementation. The API above is working:

- `Refl<T>` registers T in the global pool on construction.
- `find_class("Name")` returns `std::expected<Class, Error>`.
- `Class::find_constructor({"int", "int"})` returns `std::expected<Constructor, Error>`.
- `Class::find_function("name")` returns `std::expected<Function, Error>` (walks bases).
- `Class::find_function("name", {"int"})` resolves overloads by param types (walks bases).
- `Class::find_functions("name")` returns all overloads as `std::vector<Function>`.
- `Class::find_field("name")` returns `std::expected<Field, Error>` (walks bases).
- `Class::base_names()` returns the direct base class names.
- `Constructor::call(args...)` returns `std::expected<Object, Error>` — a
  type-erased, shared-ownership handle.  No template parameter needed.
- `Function::invoke(obj, args...)` calls the member function on an `Object`
  (or on a concrete `T&` via `invoke<T>`) and returns `std::expected<std::any, Error>`.
  Returns `Error::TypeError` if the object is not the function's class (or a
  derived class), `Error::NullHandle` if the handle is invalid.
- `Field::get(Object&)` returns the field value as `std::expected<std::any, Error>`,
  with the same `Error::TypeError` / `Error::NullHandle` semantics.
- `Field::set(Object&, std::any)` sets the field value.  Returns
  `Error::BadSignature` for read-only (const or bit-field) fields, and
  `Error::TypeError` / `Error::NullHandle` as above.
- `Class::find_static_field("name")` returns `std::expected<StaticField, Error>`
  (walks bases).
  `StaticField::get()` returns the value as `std::any`; `StaticField::set(std::any)`
  writes the static storage (no Object needed).
- `Class::find_static_function("name")` returns `std::expected<StaticFunction, Error>`
  (walks bases). `find_static_function("name", {"int"})` resolves overloads by param
  types. `find_static_functions("name")` returns all overloads.
  `StaticFunction::invoke(args...)` calls the function directly (no Object needed)
  and returns `std::any`.
- `Class::constructors()` enumerates all registered constructors.
- `Object::cast_safe<T>()` checks the class name at runtime and returns
  `std::expected<std::shared_ptr<T>, Error>` — the shared_ptr keeps the
  object alive independently of the Object.  Succeeds if T matches the
  object's class or any of its bases (upcast).
- `Object::is_class("Name")` checks whether the object is of the given class
  or a class derived from it.
- `find_enum("Name")` returns `std::expected<Enum, Error>`.
- `Enum::find_enumerator("name")` and `Enum::find_enumerator(value)` return
  `std::expected<Enumerator, Error>`.
- `list_all_classes()` and `list_all_enums()` enumerate registered names.
- `Object::clone()` deep-copies the object through the type-erased handle.
  Returns `Error::BadSignature` if the class is not copy-constructible.
- `Object::to_string()` returns a debug string with the class name and address.

Limitations (marked with `ponytail:` in the source):

- Bit-field data members are skipped (pointer-to-member is not valid for them).
- Const data members (static and non-static) are read-only (getter only, no setter).
- Inheritance walk is single-inheritance only — multiple inheritance with
  offset bases would produce wrong pointer adjustments in invokers/getters.
- Clone requires a copy constructor — non-copyable classes return
  `Error::BadSignature`.
- Arguments are passed as `std::any` — a type mismatch throws
  `std::bad_any_cast` at the call site rather than being undefined behaviour.
- Registration is static-init order dependent — `find_class` only works after
  `Refl<T>` has been instantiated.

## Refl<T> dispatch struct

In addition to the type-erased `Object` / `find_class` path, `Refl<T>` can
be constructed with arguments to build a **dispatch struct** — a
compile-time-synthesized struct (via `define_aggregate`) with named
callable fields for each member function and data member:

```cpp
refl::Refl<Point> p(1, 2);
p->set(10, 20);           // overloaded — resolved by argument type
int s = p->sum();          // TypedMethod<int()> — real return type!
int x = p->x;              // TypedProperty<int> — implicit conversion (read)
p->x = 42;                 // TypedProperty<int> — assignment (write)
p->coords[0] = 99;         // TypedProperty<array<int,3>> — operator[]
p->instance_count = 5;    // TypedStaticProperty<int> — static member
int gi = p->get_instance_count(); // TypedStaticMethod<int> — static method
p.reset(100, 200);         // swap the underlying object
p.get().x                  // typed escape hatch (int&)
```

The dispatch struct is synthesized at compile time: `define_aggregate`
creates one `TypedMethod<Sigs...>` field per function name, one
`TypedProperty<T>` field per non-static data member, one
`TypedStaticProperty<T>` field per static data member, one
`TypedStaticMethod<R>` field per static member function, plus a
`std::shared_ptr<T>` holding the object.  Field types are built via
`substitute` from `return_type_of` / `type_of`.

`TypedMethod<Sigs...>` uses a concept (`matches_sig`) to pick the
matching signature at compile time and returns that signature's return
type — no `std::variant`, no `std::any` at the call site.

`TypedProperty<T, bool Readonly>` mimics a public data member: implicit
conversion to `T` for reading, `operator=(T)` for writing, and
`operator[]` for subscriptable types (arrays, vectors).  Const members
get `Readonly=true` (compile-time error on assignment).  `operator[]`
uses the member's byte offset (`offset_of`) to access the element
directly in the object, returning a reference.

`TypedStaticProperty` and `TypedStaticMethod` access static storage
without an `obj` pointer — they use `StaticGetterFn` / `StaticSetterFn`
/ `StaticInvokerFn` directly.

The underlying object is stored in a `std::shared_ptr<T>`, enabling
`reset(args...)` to swap the object at runtime.  All fields are
re-populated after a swap.

### Mixed return types — no variant needed

If overloads of a function name return different types (e.g.
`compute(int)` returns `int` but `compute(double)` returns `double`),
the field is `TypedMethod<int(int), double(double)>`:

```cpp
int  i = p->compute(3);     // matches int(int) → returns int
double d = p->compute(3.0); // matches double(double) → returns double
```

The `matches_sig` concept compares the call's argument types (after
`remove_cvref_t`) to each overload's parameter types, picking the right
signature at compile time.  The return type follows from the signature —
no runtime dispatch, no variant unpacking.

### Hooks (Qt-style, after-only)

```cpp
p.connect("sum", [](std::any& result) { ... });   // fires after sum()
p.on_change("x", [](std::any& newval) { ... });   // fires after x.set(...)
```

`connect` registers a per-name callback fired after the method returns
(Qt-style per-signal connect).  `on_change` registers a per-property
callback fired after a property set (Qt-style NOTIFY).  Both are
after-only observers — they see the result/value but cannot veto or
modify.  The hook callback receives `std::any&` (type-agnostic) — the
typed return is produced before the hook fires; the hook observes the raw
any.  The design is extensible to before-call hooks if needed later.

### Limitations of the dispatch struct

- Inherited members are not in the dispatch struct — `members_of` returns
  direct members only.  Use the `find_class` / `Function` path for
  inherited methods.
- `Refl<T>` is non-copyable, non-movable: `TypedMethod` / `TypedProperty`
  fields store pointers into the `optional<T>` member, which is pinned
  to the `Refl`'s address.
- Overloads are resolved by argument type match (via the `matches_sig`
  concept), not just arity — `compute(int)` and `compute(double)` dispatch
  to the correct overload and return the correct type.  However,
  same-arity same-type overloads (e.g. two overloads both taking `int`)
  are ambiguous and will match the first declared.
- For compile-time-checked calls with the real return type via the
  type-erased path, use `find_class` + `Function::invoke`.
