# CPP Runtime Reflection Framework

Goal: Make a Framework based on latest compile-time reflection features that enables some partial runtime reflection.

Highly experimental.

## Pseudo Code Design

A user of this framework will have to wrap classes in containers:

```cpp
Refl<MyClass> obj;
```

The existance of this alone already allows to find this class in the global class pool and create it.

```cpp
auto my_class_class = *refl::find_class("MyClass");
```

Holding an object of this class allows querying functions and construct it.

```cpp
auto construct = *my_class_class.find_constructor("int", "int");
auto my_obj = *construct.call(1, 3);
```

The returned `Object` is a type-erased, shared-ownership handle — it holds
a `std::shared_ptr<void>` internally, so copies share ownership.  You can
invoke methods on it directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
std::any result = fn.invoke(my_obj, arg1, arg2);
```

When you do need the concrete type, use the safe cast, which checks the
class name at runtime and returns a `std::shared_ptr<T>` that keeps the
object alive independently:

```cpp
auto result = my_obj.cast_safe<MyClass>();
if (result) { auto sp = result.value(); /* sp->... */ }
```

The cast also succeeds for base classes — `cast_safe<Base>()` on a derived
Object works.  Use `is_class("Name")` to check the type without casting.

Fields can be found by name and get/set through type-erased handles:

```cpp
auto field = *my_class_class.find_field("x");
std::any old = field.get(my_obj);
field.set(my_obj, std::any(42));
```

Note the dereferences, the return values should be std::expected.  Function
results are returned as `std::any` — use `std::any_cast<T>` to extract.

