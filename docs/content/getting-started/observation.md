# Observation

Use the types from [typed binding](binding.md). `observe(dynamic, sink)` returns a
typed facade. Keep each subscription token alive to receive events.

```cpp
--8<-- "test_usage.cpp:observation"
```

Call through `observed->` to emit method, property-read, and property-write events.
Use `after_view<M>` with `observed.dispatch(OperationKind::view)` for retained
property views. Events expose read-only receiver, argument, and result views plus
their operation kind. Delivery runs after successful result capture and before
typed extraction. Listener exceptions go to the required non-throwing error sink.

The adapter observes calls made through it. Direct calls bypass it. See
[observation semantics](../architecture/observation.md).
