# Observation

Use the types from [typed binding](binding.md). `observe` wraps a dispatch handle.
Keep the subscription token alive to receive events.

```cpp
--8<-- "test_usage.cpp:observation"
```

Events expose read-only receiver, argument, and result views. Delivery runs after
successful result capture and before typed extraction. Listener exceptions go to
the required non-throwing error sink.

The adapter observes calls made through it. Direct calls bypass it.
A generated typed observation facade and dedicated property events remain pending.
See [observation semantics](../architecture/observation.md).
