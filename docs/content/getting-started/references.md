# Retained references

A reference to closure storage needs an owner after the call ends. Declare its
provenance and use `try_call_retained<T>`.

```cpp
--8<-- "test_usage.cpp:reference-type"
```

```cpp
--8<-- "test_usage.cpp:retained-reference"
```

The ordinary typed call `counter->value()` rejects restricted raw export before
entering the target. `RetainedRef<T>` keeps the declared owner alive. It cannot
prevent invalidation inside that owner, such as vector reallocation.

Native typed references use caller-borrow export: the caller keeps the receiver
and referenced storage valid. See [call contracts](../architecture/contracts.md).
