# Implemented architecture

This page records the running implementation as migration changes land. The
remaining chapters label their target contracts separately from this state.

## Core metadata boundary

`refl/core/descriptor.hpp` contains the existing `ClassInfo`, `EnumInfo`, member
records, and erased operation pointer aliases. It includes only standard-library
headers and forward-declares `Object`. `refl/core/error.hpp` defines the existing
`Error` enumeration and includes no other headers. Both compile as C++23 without
reflection enabled. The public names and record layouts are unchanged.

`refl/refl.hpp` includes both headers as a compatibility umbrella. It still owns
`Object`, generation, global pools, hierarchy lookup, and invocation. Describing
metadata requires no registration or compiler reflection; generating native
metadata and invoking operations still require the umbrella and C++26 reflection.
The records remain mutable construction data, and operation pointers do not own
callable contexts or receivers.

Metadata handles currently borrow records. `Object` retains class metadata, but a
`Class` or member handle obtained from a synthetic object does not extend that
metadata's lifetime. Retaining descriptors is the next bounded change.

## Verification

Meson compiles each core header independently with C++23 and `-fno-reflection`.
The `core_headers` consumer builds and reads synthetic metadata under those same
settings. `core_include_boundaries` rejects reflection and higher-layer includes
inside core headers. These checks run alongside all five existing component tests
and the three sample smoke tests (ten test entries in total).

## Migration status

Baseline stabilization is complete. The first contracts extraction is limited to
existing metadata declarations and errors; it does not claim completion of
milestone 1. The end-to-end prototype in milestone 0a remains pending before broad
invocation or state migration. This narrow extraction establishes a testable
header boundary without changing those semantics.

Complete type identities/signatures, value views, owned call targets, local
registries, common invocation, and the proposed observation adapter remain target
work. The [migration guide](migration.md) retains their acceptance criteria.
