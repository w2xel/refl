# Design

## Idea

A user of this framework registers classes into a global pool:

```cpp
[[maybe_unused]] static refl::Reg<MyClass> reg;
```

The existence of this alone already allows finding the class in the global
class pool and creating it.  (A typed dispatch / dynamic-implementation
layer, `Dyn<T>`, is described separately in [Dyn](dyn.md).)

```cpp
auto my_class_class = *refl::find_class("MyClass");
```

Holding a class handle allows querying functions and constructing it.

```cpp
auto construct = *my_class_class.find_constructor({"int", "int"});
auto my_obj = *construct.call(1, 3);
```

The returned `Object` is a type-erased, shared-ownership handle (backed by
`std::shared_ptr<void>`) — you don't need to know the C++ type to construct
or invoke methods.  Copies share ownership.  Functions are called on the
`Object` directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
std::any result = *fn.invoke(my_obj, arg1, arg2);
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
std::any old = *field.get(my_obj);
field.set(my_obj, std::any(42));
```

Note the dereferences — the return values are `std::expected`.
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
instantiating `Reg<T>` (or calling `ensure_registered<T>()`); the registration
data is gathered at compile time via reflection and emitted as a static object.

## Status

Initial implementation. The API above is working:

- `Reg<T>` registers T in the global pool on construction (core's sole
  user-facing side effect; `ensure_registered<T>()` does the same).
- `find_class("Name")` returns `std::expected<Class, Error>`.
- `Class::find_constructor({"int", "int"})` returns `std::expected<Constructor, Error>`.
- `Class::find_function("name")` returns `std::expected<Function, Error>` (walks bases).
- `Class::find_function("name", {"int"})` resolves overloads by param types (walks bases).
- `Class::find_functions("name")` returns all overloads as `std::vector<Function>`.
- `Class::find_field("name")` returns `std::expected<Field, Error>` (walks bases).
- `Class::bases()` returns the direct base classes (name + byte offset within T).
- `Constructor::call(args...)` returns `std::expected<Object, Error>` — a
  type-erased, shared-ownership handle.  No template parameter needed.
- `Function::invoke(obj, args...)` calls the member function on an `Object`
  (or on a concrete `T&` via `invoke<T>`) and returns `std::expected<std::any, Error>`.
  Returns `Error::TypeError` if the object is not the function's class (or a
  derived class), `Error::NullHandle` if the handle is invalid.
- `Field::get(Object&)` returns the field value as `std::expected<std::any, Error>`,
  with the same `Error::TypeError` / `Error::NullHandle` semantics.
- `Field::set(Object&, std::any)` sets the field value.  Returns
  `Error::ReadOnly` for read-only (const or bit-field) fields, and
  `Error::TypeError` / `Error::NullHandle` as above.
- `Class::find_static_field("name")` returns `std::expected<StaticField, Error>`
  (walks bases).
  `StaticField::get()` returns `std::expected<std::any, Error>`; `StaticField::set(std::any)`
  writes the static storage (no Object needed) and returns `std::expected<void, Error>`.
- `Class::find_static_function("name")` returns `std::expected<StaticFunction, Error>`
  (walks bases). `find_static_function("name", {"int"})` resolves overloads by param
  types. `find_static_functions("name")` returns all overloads.
  `StaticFunction::invoke(args...)` calls the function directly (no Object needed)
  and returns `std::expected<std::any, Error>`.
- `Class::constructors()` enumerates all registered constructors.
- `Class::functions()` enumerates all registered member functions.
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
  Returns `Error::NotCopyable` if the class is not copy-constructible.
- `Object::to_string()` returns a debug string with the class name and address.

Type identity uses the fully-qualified name (`display_string_of`), so two
classes with the same unqualified name in different namespaces do not collide
in the pool or in `cast_safe`.  Global-scope types have no prefix
(`find_class("Point")`); namespaced types require it
(`find_class("ns::Point")`).

Argument forwarding is checked at the call boundary: an argument-count
mismatch returns `Error::ArityMismatch` and a `std::any` type mismatch returns
`Error::TypeError` (caught internally — never thrown to the caller).  This
applies to `Constructor::call`, `Function::invoke`, `StaticFunction::invoke`,
`Field::set`, and `StaticField::set`.  Writing to a read-only field returns
`Error::ReadOnly`; cloning a non-copy-constructible class returns
`Error::NotCopyable`.

Limitations (marked with `ponytail:` in the source):

- Bit-field data members are skipped (pointer-to-member is not valid for them).
- Const data members (static and non-static) are read-only (getter only, no setter).
- Multiple inheritance is supported — base-class pointer adjustment uses
  `offset_of` at registration time, accumulated through the base hierarchy.
  Virtual inheritance is not supported (`offset_of` is not constant for
  virtual bases); virtual base hierarchies will fail to register correctly.
- Clone requires a copy constructor — non-copyable classes return
  `Error::NotCopyable`.
- Registration is static-init order dependent — `find_class` only works after
  `Reg<T>` has been instantiated (or `ensure_registered<T>()` called).
