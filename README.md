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
auto my_obj = std::move(*construct.call(1, 3));
```

The returned `Object` is type-erased — it owns the heap-allocated instance
without you needing to know its C++ type.  You can invoke methods on it
directly:

```cpp
auto fn = *my_class_class.find_function("some_method");
std::any result = fn.invoke(my_obj, arg1, arg2);
```

When you do need the concrete type, cast explicitly:

```cpp
auto& concrete = my_obj.cast<MyClass>();
```

Note the dereferences, the return values should be std::expected.  Function
results are returned as `std::any` — use `std::any_cast<T>` to extract.

