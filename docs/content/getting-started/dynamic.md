# Dynamic dispatch

Use the `Drawable`, `Square`, and `Line` types from [typed binding](binding.md).

## Implement and wrap

A default `Dyn<T>` has unimplemented operations. Install methods and owned properties:

```cpp
--8<-- "test_usage.cpp:implementation"
```

## Reset, capture, and restore

```cpp
--8<-- "test_usage.cpp:generations"
```

| Handle | After reset |
| --- | --- |
| `dispatch()` | Resolves the new generation |
| `capture_binding()` | Keeps the captured generation |
| Saved target | Keeps its original receiver and callable |

## Detach native storage

Declare a replacement independent only when it needs no native receiver.
Detachment removes native and unknown dependencies.

```cpp
--8<-- "test_usage.cpp:detach"
```

See [dynamic state](../architecture/dynamic.md) for lifetime and publication rules.
