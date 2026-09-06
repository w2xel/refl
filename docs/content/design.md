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
refl::Refl<MyClass> my_obj = construct.call(1, 3);
```

Note the dereferences — the return values should be `std::expected`.

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
- `Class::find_function("name")` returns `std::expected<Function, Error>`.
- `Constructor::call<T>(args...)` heap-allocates and returns `std::expected<Refl<T>, Error>`.
- `Function::invoke<T>(obj, args...)` calls the member function and returns the
  result as `std::any` (use `std::any_cast<R>` to extract; empty for void functions).

Limitations (marked with `ponytail:` in the source):

- Constructors with 0 or more than 4 parameters are skipped.
- Member functions with more than 3 parameters are skipped.
- Default constructors are skipped.
- Arguments are passed by address (value types only); reference parameters work
  but the caller must ensure the argument outlives the call.
