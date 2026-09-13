# Typed binding

## Define an interface and implementations

`Proxy<T>` binds matching names and complete signatures. Implementations need no
inheritance relationship with the interface.

```cpp
--8<-- "test_usage.cpp:binding-types"
```

## Bind and replace the receiver

Pass an owning `Object` with its descriptor. Registration is optional.

```cpp
--8<-- "test_usage.cpp:binding"
```

A failed `bind` preserves the previous binding. See [binding internals](../architecture/typed-binding.md).
