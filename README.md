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
auto my_class_class = *Refl::find_class("MyClass");
```

Holding an object of this class allows querying functions and construct it.

```cpp
auto construct = *my_class_class.find_constructor("int", "int");
Refl<MyClass> my_obj = construct.call(1,3);
```

Note the dereferences, the return values should be std::expect.

