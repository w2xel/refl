// Metadata records and erased operation declarations; no registration policy.
#pragma once

#include <refl/core/call.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace refl {

// Forward declarations — needed for function pointer type aliases below.
class Object;

// ---------------------------------------------------------------------------
// Type-erased function pointer signatures for factories, invokers, getters,
// and setters.
//
// Args arrive as const Object* — each carries a void* to the caller's
// stack storage plus a string_view type name (from display_string_of, no
// RTTI).  The invoker checks the type name against the compile-time param
// type and moves from the caller's storage.
//
// The invoker also receives the shared_ptr<void> owner of the target
// object (empty for non-owning / stack objects).  This allows
// reference-returning functions (operator=, operator+=, etc.) to return
// an aliasing Object that shares ownership with the original.
//
// Returns are Object (owning or non-owning).  Void functions return
// a default (invalid) Object.  The caller checks .valid() to distinguish
// void from a real return value.
// ---------------------------------------------------------------------------
using FactoryFn       = Object (*)(const Object* args);
using InvokerFn       = Object (*)(const std::shared_ptr<void>& owner,
                                  void* obj, const Object* args);
using GetterFn        = Object (*)(void* obj);
using SetterFn        = void  (*)(void* obj, const Object* val);
using StaticGetterFn  = Object (*)();
using StaticSetterFn  = void  (*)(const Object* val);
using StaticInvokerFn = Object (*)(const Object* args);
using CloneFn         = std::shared_ptr<void> (*)(void* obj);

struct ConstructorInfo {
    std::vector<std::string> param_types;
    FactoryFn factory;
    MemberId member = {};
    Signature signature = {};
};

struct FunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    InvokerFn invoker;
    bool is_const = false;
    MemberId member = {};
    Signature signature = {};
};

struct StaticFunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    StaticInvokerFn invoker;
    MemberId member = {};
    Signature signature = {};
};

struct FieldInfo {
    std::string name;
    std::string type;
    std::ptrdiff_t offset;  // byte offset of member within T (for get_ref)
    GetterFn getter;        // nullptr if move-only (not copy-constructible)
    SetterFn setter;        // nullptr for const / not move-assignable
    bool is_const;          // true for const-qualified members
    MemberId member = {};
    Signature signature = {};
};

struct StaticFieldInfo {
    std::string name;
    std::string type;
    void* address;             // address of the static storage (for get_ref)
    StaticGetterFn getter;
    StaticSetterFn setter;  // nullptr for const / not move-assignable
    bool is_const;          // true for const-qualified members
    MemberId member = {};
    Signature signature = {};
};

struct ClassInfo;
struct NativeOperation {
    OperationDescriptor descriptor;
    std::function<Result<CallTarget>(ObjectView, TargetOptions)> bind;
    std::function<std::shared_ptr<const ClassInfo>()> result_descriptor;
};
using NativeViewFn = ObjectView (*)(void*, LifetimeAnchor, bool);
struct BaseInfo {
    std::string name;
    std::ptrdiff_t offset;  // byte offset of this base within the derived class
    std::shared_ptr<const ClassInfo> descriptor = {};
};

struct ClassInfo {
    std::string name;
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
    std::vector<StaticFieldInfo> static_fields;
    std::vector<ConstructorInfo> constructors;
    std::vector<FunctionInfo> functions;
    std::vector<StaticFunctionInfo> static_functions;
    TypeId identity = {};
    NativeViewFn make_view = nullptr;
    std::vector<NativeOperation> operations = {};
    std::vector<std::string> unsupported_members = {};
    CloneFn clone = nullptr;  // nullptr if T is not copy-constructible
};

struct EnumeratorInfo {
    std::string name;
    long long value;
};

struct EnumInfo {
    std::string name;
    std::vector<EnumeratorInfo> enumerators;
};

}  // namespace refl
