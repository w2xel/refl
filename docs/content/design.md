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

The returned `Object` is a type-erased handle — it can be owning (backed by
`std::shared_ptr<void>`, copies share ownership) or non-owning (a raw
pointer borrow into a stack or heap object).  You don't need to know the
C++ type to construct or invoke methods.  Functions are called on the
`Object` directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
auto result = *fn.invoke(my_obj, arg1, arg2);
// result is an Object — extract via cast_safe (owning) or cast_ref (non-owning)
auto val = result.cast_safe<ReturnType>().value();
```

When you need the concrete type, use the safe cast, which checks the class
name at runtime and returns a `std::shared_ptr<T>` that shares ownership
with the Object — it succeeds for the object's own class and for any base
class (upcast).  `cast_safe` only works on owning Objects; for non-owning
Objects, use `cast_ref<T>()` which returns a raw `T*`:

```cpp
auto result = my_obj.cast_safe<MyClass>();
if (result) { auto sp = result.value(); /* sp->... */ }
```

Fields can be found by name and get/set through type-erased handles:

```cpp
auto field = *my_class_class.find_field("x");
auto old = *field.get(my_obj);          // returns Object (copy of the value)
field.set(my_obj, 42);                 // typed set, no std::any needed
auto ptr = *field.get_ref(my_obj);     // void* into the object (move-only OK)
```

Note the dereferences — the return values are `std::expected`.
Function and field results are returned as `Object` — use `cast_safe<T>()`
(owning) or `cast_ref<T>()` (non-owning) to extract.  Void functions return
an invalid `Object` (`.valid()` is false).

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
- `Class::find_functions("name")` returns all overloads as `std::vector<Function>`
  (walks base classes — inherited overloads accumulate alongside the derived
  class's own).
- `Class::all_functions()` returns all functions across the full hierarchy as
  `std::vector<Function>` (by value, merged view).  Unlike `functions()`
  which returns only this class's own members, `all_functions()`
  includes inherited functions.  Elements are `Function` handles — call
  `.invoke()` directly.
- `Class::all_static_functions()` is the static-function equivalent of
  `all_functions()`, returning `std::vector<StaticFunction>`.
- `Class::all_fields()` returns all data fields across the full hierarchy
  as `std::vector<Field>` (by value, merged view).  Unlike `fields()`
  which returns only this class's own members, `all_fields()` includes
  inherited fields.
- `Class::all_static_fields()` is the static-field equivalent of
  `all_fields()`, returning `std::vector<StaticField>`.
- `Class::find_field("name")` returns `std::expected<Field, Error>` (walks bases).
- `Class::bases()` returns the direct base classes as `std::vector<Base>`
  handles — `Base::name()` and `Base::offset()` give the base class name
  and byte offset within T.
- `Constructor::call(args...)` returns `std::expected<Object, Error>` — a
  type-erased, owning handle.  No template parameter needed.
- `Function::invoke(obj, args...)` calls the member function on any object —
  an owned `Object` or a stack/concrete instance (implicit conversion).
  Returns `std::expected<Object, Error>`.
  Returns `Error::TypeError` if the object is not the function's class (or a
  derived class), `Error::NullHandle` if the handle is invalid.
  Reference-returning functions (operator=, operator+=, fluent builders)
  return an aliasing Object that keeps the original alive.
  Arguments are also upcast: a function taking `Base&` can be invoked with
  a `Derived` Object — the argument is adjusted to the base subobject.
- `Field::get(obj)` and `Field::set(obj, val)` get/set the field on any
  object (owned `Object` or stack/concrete instance).
  `get` returns `std::expected<Object, Error>` (a copy of the value);
  `set` takes a typed value (no `std::any` needed) and returns
  `Error::ReadOnly` for read-only (const or not move-assignable) fields,
  and `Error::TypeError` / `Error::NullHandle` as above.
  Like `invoke`, `set` upcasts a derived-class value to a base-class field
  (the value is adjusted to the base subobject before copying).
- `Field::get_ref(obj)` returns `std::expected<void*, Error>` — a non-owning
  pointer into the field inside the object.  Works for all members including
  move-only (unique_ptr).  The caller casts `void*` to the member type.
- `Field::get_ref<T>(obj)` returns `std::expected<T*, Error>` — a typed
  pointer into the field.  Checks `T` against the field's type name at
  runtime; returns `Error::TypeError` on mismatch.  For all members
  including move-only.
- `Field::has_getter()` returns true if the field has a copy-based getter
  (false for move-only members — use `get_ref` instead).
  `Field::has_setter()` returns true if the field has a setter (false for
  const or not move-assignable members).
  `Field::is_readonly()` returns true if the field is const-qualified.
- `Class::find_static_field("name")` returns `std::expected<StaticField, Error>`
  (walks bases).
  `StaticField::get()` returns `std::expected<Object, Error>`;
  `StaticField::get_ref()` returns `std::expected<void*, Error>` — a
  non-owning pointer to the static storage (works for move-only members);
  `StaticField::get_ref<T>()` is the typed variant (checks the type name at
  runtime).  Both return `Error::ReadOnly` for const static members, which
  may lack addressable storage (use `get()` to read by copy).
  `StaticField::set(val)` writes the static storage (no Object needed)
  and returns `std::expected<void, Error>`.
- `Class::find_static_function("name")` returns `std::expected<StaticFunction, Error>`
  (walks bases). `find_static_function("name", {"int"})` resolves overloads by param
  types. `find_static_functions("name")` returns all overloads (walks bases).
  `StaticFunction::invoke(args...)` calls the function directly (no Object needed)
  and returns `std::expected<Object, Error>`.
- `Class::constructors()` enumerates all registered constructors as
  `std::vector<Constructor>`.
- `Class::functions()` enumerates this class's own member functions as
  `std::vector<Function>`.  `Class::all_functions()` returns the full
  hierarchy (by value).  `Class::fields()`, `Class::static_fields()`,
  and `Class::static_functions()` likewise return handle vectors of their
  own members; the `all_*` variants include inherited members across the
  full hierarchy.
- The `*Info` structs (`FunctionInfo`, `StaticFunctionInfo`, `FieldInfo`,
  `StaticFieldInfo`, `ConstructorInfo`) are internal metadata stored in
  `ClassInfo`.  The public API returns handle types (`Function`, `Field`,
  etc.) that wrap them; call the handle methods (`name()`, `param_types()`,
  `invoke()`, `get()`, etc.) directly.
- `Object::cast_safe<T>()` checks the class name at runtime and returns
  `std::expected<std::shared_ptr<T>, Error>` — the shared_ptr keeps the
  object alive independently of the Object.  Succeeds if T matches the
  object's class or any of its bases (upcast).  Only works on owning
  Objects; returns `Error::NotOwned` for non-owning Objects.
- `Object::cast_ref<T>()` returns `std::expected<T*, Error>` — a raw pointer
  for both owned and non-owning Objects.  The caller manages lifetime.
- `Object::is_owned()` returns true if the Object owns its data (backed by
  shared_ptr), false if it's a non-owning borrow.
- Constructing an `Object` from a const lvalue yields an owning copy, not a
  mutable borrow — a non-owning borrow of a const object would let `cast_ref`
  write through it (UB), so the borrow constructor rejects `const T&`.
- `Object::is_class("Name")` checks whether the object is of the given class
  or a class derived from it.
- `find_enum("Name")` returns `std::expected<Enum, Error>`.
- `Enum::find_enumerator("name")` and `Enum::find_enumerator(value)` return
  `std::expected<Enumerator, Error>`.
- `list_all_classes()` and `list_all_enums()` enumerate registered names.
- `Object::clone()` deep-copies the object through the type-erased handle.
  Returns `Error::NotOwned` if the Object is non-owning, `Error::NotCopyable`
  if the class is not copy-constructible.
- `Object::to_string()` returns a debug string with the class name and address.

Type identity uses the fully-qualified name (`display_string_of`), so two
classes with the same unqualified name in different namespaces do not collide
in the pool or in `cast_safe`.  Global-scope types have no prefix
(`find_class("Point")`); namespaced types require it
(`find_class("ns::Point")`).

Argument forwarding is checked at the call boundary: an argument-count
mismatch returns `Error::ArityMismatch` and a type mismatch returns
`Error::TypeError` (caught internally — never thrown to the caller).  Type
checking uses compile-time `display_string_of` type names (no RTTI).  This
applies to `Constructor::call`, `Function::invoke`, `StaticFunction::invoke`,
`Field::set`, and `StaticField::set`.  Writing to a read-only field returns
`Error::ReadOnly`; cloning a non-copy-constructible class returns
`Error::NotCopyable`.

Type matching at the call boundary is exact: the argument's class name
(compile-time `display_string_of`) must equal the parameter type name, or
be a class derived from it (the argument is then upcast to the base
subobject).  Implicit primitive conversions are not performed — passing an
`int` to a `long` parameter returns `Error::TypeError`, not a widened
call.  This is deliberate: the framework checks type identity via string
comparison (no RTTI), and the project compiles with `-Wconversion
-Wsign-conversion`, so silent widening at the reflection boundary would
contradict the codebase's own stance on implicit conversions.

All `Class` and `Enum` accessor methods are null-safe: calling `fields()`,
`functions()`, `bases()`, `name()`, etc. on a default-constructed (invalid)
handle returns an empty vector or empty string rather than crashing.  The
`find_*` methods return `Error::NullHandle`.

Limitations:

- Bit-field data members are skipped (pointer-to-member is not valid for them).
- Only public members are reflected — protected and private data members,
  static data members, member functions, and static member functions are not
  registered.  This matches the public-base filter on inheritance: the framework
  reflects the public interface, not the implementation.  Public constructors
  are registered; private/protected constructors are not.
- Const data members (static and non-static) are read-only (getter only, no setter).
- Move-only data members (e.g. `unique_ptr`) have no copy getter (getter copies
  into a `shared_ptr`) but DO have a setter if move-assignable — `Field::set()`
  move-assigns the value.  Use `Field::get_ref()` for direct pointer access.
  Registration no longer fails to compile for classes with move-only members.
  Members that are neither copy-constructible nor move-assignable have neither
  getter nor setter — use `get_ref` for read access.
- Functions with reference return types (e.g. `operator=`, `operator+=`)
  return an aliasing Object that shares ownership with the original (for
  owned objects) or a non-owning Object (for stack objects).  The caller
  can use `cast_ref<T>()` to access the return value.
- Deleted functions are skipped (not registered).
- C++ name hiding is respected by `find_function`, `find_functions`,
  `find_static_function`, `find_static_functions`, `all_functions()`, and
  `all_static_functions()`.  If a derived class declares any function named
  `X`, all base `X` overloads are hidden — they are not returned by any of
  these methods.  This matches C++ semantics: hiding is name-based, not
  signature-based (a derived `set(int)` hides a base `set(int,int)`).
  `using Base::set;` declarations that un-hide base overloads are **not**
  represented in the reflection metadata — the using-declaration does not
  create a new function member, so the framework cannot detect it.  Base
  overloads remain hidden even when a `using`-declaration makes them
  callable in raw C++.
- Multiple inheritance is supported — base-class pointer adjustment uses
  `offset_of` at registration time, accumulated through the base hierarchy.
  Only public inheritance is walked — protected and private bases are not
  stored, so `find_function`, `find_field`, `is_class`, and `cast_safe` skip
  them.  Virtual inheritance is not supported (`offset_of` is not constant for
  virtual bases); virtual base hierarchies will fail to register correctly.
  Diamond inheritance (a shared base reached via two paths) is deduplicated
  in `all_functions()`, `all_fields()`, `all_static_fields()`, `all_static_functions()`,
  `find_functions()`, and `find_static_functions()` — each member from the
  shared base appears once, not once per path.
- Overloaded operators (`operator+`, `operator==`, `operator[]`, `operator()`,
  `operator+=`, `operator=`, etc.) are registered as functions, findable by
  name (`"operator+"`, `"operator=="`, etc.).  Conversion operators and the
  destructor are not registered.
- Clone requires a copy constructor — non-copyable classes return
  `Error::NotCopyable`.
- Enum values are stored as `long long`.  Enums with an unsigned underlying
  type and values exceeding `LLONG_MAX` undergo an implementation-defined
  conversion; the stored value may not match the true enumerator value.
- Registration is static-init order dependent — `find_class` only works after
  `Reg<T>` has been instantiated (or `ensure_registered<T>()` called).
