# Runtime reflection

## Define native types

```cpp
--8<-- "test_usage.cpp:runtime-types"
```

## Publish and construct

A local registry owns its catalog. Descriptors and object handles retain metadata
independently of that catalog.

```cpp
--8<-- "test_usage.cpp:registry"
```

## Read, write, and invoke

Select an overload by its parameter types. Checked casts retain object storage.

```cpp
--8<-- "test_usage.cpp:runtime-members"
```

## Inspect static members and enums

```cpp
--8<-- "test_usage.cpp:static-enum"
```

See [metadata and calls](../architecture/contracts.md) for ownership and error behavior.
