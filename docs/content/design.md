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

When you need the concrete type, cast explicitly.  The fast cast returns
a non-owning `T*` (valid as long as the Object is alive):

```cpp
MyClass* ptr = my_obj.cast<MyClass>();
```

Or use the safe cast, which checks the class name at runtime and returns
a `std::shared_ptr<T>` that shares ownership with the Object:

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
  (or on a concrete `T&` via `invoke<T>`) and returns `std::any`.
- `Field::get(Object&)` returns the field value as `std::any`.
- `Field::set(Object&, std::any)` sets the field value.  Returns
  `Error::BadSignature` for read-only (const or bit-field) fields.
- `Object::cast<T>()` returns a non-owning `T*` (fast, unchecked).
- `Object::cast_safe<T>()` checks the class name at runtime and returns
  `std::expected<std::shared_ptr<T>, Error>` — the shared_ptr keeps the
  object alive independently of the Object.
- `find_enum("Name")` returns `std::expected<Enum, Error>`.
- `Enum::find_enumerator("name")` and `Enum::find_enumerator(value)` return
  `std::expected<Enumerator, Error>`.
- `list_all_classes()` and `list_all_enums()` enumerate registered names.

Limitations (marked with `ponytail:` in the source):

- Constructors and member functions with more than 10 parameters are skipped.
- Bit-field data members are skipped (pointer-to-member is not valid for them).
- Const data members are read-only (getter only, no setter).
- Inheritance walk is single-inheritance only — multiple inheritance with
  offset bases would produce wrong pointer adjustments in invokers/getters.
- Arguments are passed by address (value types only); reference parameters work
  but the caller must ensure the argument outlives the call.
