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
callable contexts or receivers. Publication copies records into const allocations.

## Retained metadata handles

`Class`, `Enum`, and every member handle retain a `shared_ptr<const ClassInfo>` or
`shared_ptr<const EnumInfo>`. A member also stores its declaration index. Copies
share that allocation; releasing the final owner destroys it. Member `valid()`
checks both ownership and the index bounds. Metadata accessors require a valid
handle; checked call operations return `Error::NullHandle` for invalid handles.

Global class and enum pools hold const shared descriptors. `Registrar` and
`EnumRegistrar` publish copies of construction records. Existing handles retain
their original records when a registration replaces the same name. Duplicate
registration still replaces catalog entries; conflict rejection is target work.

Class lookup and enumeration pass shared ownership into their results. Inherited
member lookup retains each declaring base's metadata while holding the pool lock
and carries it into the returned handle. `Proxy::get_class()` shares its object's
metadata, and `Dyn::get_class()` shares the registered native metadata. A synthetic
backend and its proxy can be destroyed while a class or member handle remains
usable for metadata access. Retaining metadata does not retain the backend or a
concrete receiver; invoking a member still requires a live compatible object.

### Construction and compatibility

Existing umbrella includes and handle names remain supported. A shared descriptor
can be passed directly to `Class`, `Enum`, or a member constructor. Its publisher
keeps it immutable after sharing; a mutable alias cannot be used to edit published
vectors. Prefer creating `std::make_shared<const refl::ClassInfo>(record)`.

Legacy constructors accepting `const ClassInfo*` or `const EnumInfo*` copy a const
snapshot. The source only needs to live through construction; later source edits
are not visible to the handle. Null pointers create invalid handles. Snapshot
construction allocates and copies metadata; passing shared ownership avoids those
copies. Pool metadata is now const, so editing records through pool pointers is
no longer supported.

### Remaining lifetime boundary

Base relationships still contain names and offsets. Hierarchy traversal,
`Base::as_class()`, and invocation's base adjustment still consult global pools;
retaining a descriptor does not freeze its transitive hierarchy. Typed dispatch
internals still use raw selected-operation pointers. This change establishes
public metadata-handle ownership, not independent registries, owned call targets,
or concurrent catalog replacement safety for typed dispatch. These remain part
of the planned registry and invocation migration.

## Verification

Meson compiles each core header independently with C++23 and `-fno-reflection`.
The `core_headers` consumer builds and reads synthetic metadata under those same
settings. `core_include_boundaries` rejects reflection and higher-layer includes
inside core headers. These checks run alongside all five existing component tests
and the three sample smoke tests. `metadata_lifetime` verifies each handle kind,
raw-pointer snapshots, invalid indices, catalog replacement, inherited lookup,
and native invocation after replacement. The Mockable suite verifies metadata
survival after backend/proxy destruction and release after the final member.
The full suite has eleven entries.

Verification on 2026-09-13 uses GCC 16.2, C++26 reflection for existing APIs,
optimization level 2, warnings as errors, static analysis, and LTO disabled.
The complete suite and strict MkDocs build pass. Valgrind checks cover the new
lifetime test and the Dyn, Mockable, and Hooks suites.

## Migration status

Baseline stabilization is complete. The first contracts extraction is limited to
existing metadata declarations, errors, and retained public metadata handles; it
does not claim completion of milestone 1. The end-to-end prototype in milestone 0a remains pending before broad
invocation or state migration. These narrow changes establish a testable header
boundary and descriptor ownership without introducing a second invocation engine.

Complete type identities/signatures, value views, owned call targets, local
registries, common invocation, and the proposed observation adapter remain target
work. The [migration guide](migration.md) retains their acceptance criteria.
