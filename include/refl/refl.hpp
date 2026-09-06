// refl — C++26 compile-time-reflection-based runtime reflection framework.
//
// Wrap a class as Refl<MyClass> so it registers in a global pool queryable at
// runtime via find_class("MyClass").  From a Class handle you can find
// constructors, member functions, and data fields by name or parameter-type
// name, then construct objects, invoke functions, and get/set fields through
// the returned handles.
//
// The framework is type-erased at the call boundary: Constructor::call
// returns an Object (an owning, type-erased handle) rather than a typed
// Refl<T>.  You only need to know the concrete type when you explicitly
// cast an Object to get at its members directly.
//
// Lookups return std::expected<T, Error> — dereference with operator* or
// check has_value()/error(), as shown in the design docs.
#pragma once

#include <meta>
#include <any>
#include <array>
#include <cctype>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace refl {

// ---------------------------------------------------------------------------
// Error — returned inside std::expected when a lookup fails.
// ---------------------------------------------------------------------------
enum class Error {
    NotFound,
    BadSignature,
    NullHandle,
    TypeError,
};

inline std::string_view to_string(Error e) {
    switch (e) {
    case Error::NotFound:      return "NotFound";
    case Error::BadSignature:  return "BadSignature";
    case Error::NullHandle:    return "NullHandle";
    case Error::TypeError:     return "TypeError";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Type-erased function pointer signatures for factories, invokers, getters,
// and setters.
//
// The factory returns a shared_ptr<void> with the deleter baked in, so the
// result is never a bare owning pointer.  Invokers, getters, and setters
// take a non-owning void* (a borrow from the Object's internal storage,
// valid for the duration of the call) and the void*[] arg array (borrows
// of the caller's arguments).
// ---------------------------------------------------------------------------
using FactoryFn       = std::shared_ptr<void> (*)(const std::any* args);
using InvokerFn       = std::any (*)(void* obj, const std::any* args);
using GetterFn        = std::any (*)(void* obj);
using SetterFn        = void  (*)(void* obj, std::any val);
using StaticGetterFn  = std::any (*)();
using StaticSetterFn  = void  (*)(std::any val);
using StaticInvokerFn = std::any (*)(const std::any* args);

struct ConstructorInfo {
    std::vector<std::string> param_types;
    FactoryFn factory;
};

struct FunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    InvokerFn invoker;
};

struct StaticFunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    StaticInvokerFn invoker;
};

struct FieldInfo {
    std::string name;
    std::string type;
    GetterFn getter;
    SetterFn setter;  // nullptr for const / bit-field members
};

struct StaticFieldInfo {
    std::string name;
    std::string type;
    StaticGetterFn getter;
    StaticSetterFn setter;  // nullptr for const members
};

struct ClassInfo {
    std::string name;
    std::vector<std::string> base_names;
    std::vector<FieldInfo> fields;
    std::vector<StaticFieldInfo> static_fields;
    std::vector<ConstructorInfo> constructors;
    std::vector<FunctionInfo> functions;
    std::vector<StaticFunctionInfo> static_functions;
};

struct EnumeratorInfo {
    std::string name;
    long long value;
};

struct EnumInfo {
    std::string name;
    std::vector<EnumeratorInfo> enumerators;
};

// ---------------------------------------------------------------------------
// Global class and enum pools — Meyers singletons to avoid SIOF.
// ---------------------------------------------------------------------------
inline std::unordered_map<std::string, ClassInfo>& class_pool() {
    static std::unordered_map<std::string, ClassInfo> pool;
    return pool;
}

inline std::unordered_map<std::string, EnumInfo>& enum_pool() {
    static std::unordered_map<std::string, EnumInfo> pool;
    return pool;
}

inline std::mutex& pool_mutex() {
    static std::mutex m;
    return m;
}

struct Registrar {
    explicit Registrar(const ClassInfo& info) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        class_pool()[info.name] = info;
    }
};

struct EnumRegistrar {
    explicit EnumRegistrar(const EnumInfo& info) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        enum_pool()[info.name] = info;
    }
};

// ---------------------------------------------------------------------------
// Type-erased factory / invoker / getter / setter templates.
//
// Each is parameterised on the target type T and (where relevant) the
// compile-time meta::info of the specific constructor, member function, or
// data member.  template-for in make_info instantiates one of these per
// reflected member, and stores its address in ClassInfo.
// ---------------------------------------------------------------------------

namespace detail {

// Maximum supported arity for constructors and member functions.
inline constexpr std::size_t max_arity = 10;

// Compile-time type name for safe-cast checks.
template <typename T>
consteval std::string_view type_name() {
    return std::meta::identifier_of(^^T);
}

template <typename T, std::meta::info Ctor>
std::shared_ptr<void> factory(const std::any* args) {
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Ctor));
    constexpr std::size_t n = params.size();

    if constexpr (n == 0) {
        return std::make_shared<T>();
    } else if constexpr (n == 1) {
        using P0 = [:std::meta::type_of(params[0]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]));
    } else if constexpr (n == 2) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]));
    } else if constexpr (n == 3) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]));
    } else if constexpr (n == 4) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]));
    } else if constexpr (n == 5) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]));
    } else if constexpr (n == 6) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]),
                     std::any_cast<std::remove_reference_t<P5>>(args[5]));
    } else if constexpr (n == 7) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]),
                     std::any_cast<std::remove_reference_t<P5>>(args[5]),
                     std::any_cast<std::remove_reference_t<P6>>(args[6]));
    } else if constexpr (n == 8) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]),
                     std::any_cast<std::remove_reference_t<P5>>(args[5]),
                     std::any_cast<std::remove_reference_t<P6>>(args[6]),
                     std::any_cast<std::remove_reference_t<P7>>(args[7]));
    } else if constexpr (n == 9) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        using P8 = [:std::meta::type_of(params[8]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]),
                     std::any_cast<std::remove_reference_t<P5>>(args[5]),
                     std::any_cast<std::remove_reference_t<P6>>(args[6]),
                     std::any_cast<std::remove_reference_t<P7>>(args[7]),
                     std::any_cast<std::remove_reference_t<P8>>(args[8]));
    } else if constexpr (n == 10) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        using P8 = [:std::meta::type_of(params[8]):];
        using P9 = [:std::meta::type_of(params[9]):];
        return std::make_shared<T>(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                     std::any_cast<std::remove_reference_t<P1>>(args[1]),
                     std::any_cast<std::remove_reference_t<P2>>(args[2]),
                     std::any_cast<std::remove_reference_t<P3>>(args[3]),
                     std::any_cast<std::remove_reference_t<P4>>(args[4]),
                     std::any_cast<std::remove_reference_t<P5>>(args[5]),
                     std::any_cast<std::remove_reference_t<P6>>(args[6]),
                     std::any_cast<std::remove_reference_t<P7>>(args[7]),
                     std::any_cast<std::remove_reference_t<P8>>(args[8]),
                     std::any_cast<std::remove_reference_t<P9>>(args[9]));
    }
    // ponytail: supports constructors with 0-10 parameters. Extend if needed.
    return nullptr;
}

template <typename T, std::meta::info Fn>
std::any invoker(void* obj, const std::any* args) {
    auto* target = static_cast<T*>(obj);
    auto mfn = &[:Fn:];
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Fn));
    constexpr std::size_t n = params.size();
    using R = [:std::meta::return_type_of(Fn):];

    if constexpr (n == 0) {
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)();
            return std::any{};
        } else {
            return std::any((target->*mfn)());
        }
    } else if constexpr (n == 1) {
        using P0 = [:std::meta::type_of(params[0]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0])));
        }
    } else if constexpr (n == 2) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1])));
        }
    } else if constexpr (n == 3) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2])));
        }
    } else if constexpr (n == 4) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3])));
        }
    } else if constexpr (n == 5) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4])));
        }
    } else if constexpr (n == 6) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]),
                           std::any_cast<std::remove_reference_t<P5>>(args[5]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4]),
                std::any_cast<std::remove_reference_t<P5>>(args[5])));
        }
    } else if constexpr (n == 7) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]),
                           std::any_cast<std::remove_reference_t<P5>>(args[5]),
                           std::any_cast<std::remove_reference_t<P6>>(args[6]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4]),
                std::any_cast<std::remove_reference_t<P5>>(args[5]),
                std::any_cast<std::remove_reference_t<P6>>(args[6])));
        }
    } else if constexpr (n == 8) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]),
                           std::any_cast<std::remove_reference_t<P5>>(args[5]),
                           std::any_cast<std::remove_reference_t<P6>>(args[6]),
                           std::any_cast<std::remove_reference_t<P7>>(args[7]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4]),
                std::any_cast<std::remove_reference_t<P5>>(args[5]),
                std::any_cast<std::remove_reference_t<P6>>(args[6]),
                std::any_cast<std::remove_reference_t<P7>>(args[7])));
        }
    } else if constexpr (n == 9) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        using P8 = [:std::meta::type_of(params[8]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]),
                           std::any_cast<std::remove_reference_t<P5>>(args[5]),
                           std::any_cast<std::remove_reference_t<P6>>(args[6]),
                           std::any_cast<std::remove_reference_t<P7>>(args[7]),
                           std::any_cast<std::remove_reference_t<P8>>(args[8]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4]),
                std::any_cast<std::remove_reference_t<P5>>(args[5]),
                std::any_cast<std::remove_reference_t<P6>>(args[6]),
                std::any_cast<std::remove_reference_t<P7>>(args[7]),
                std::any_cast<std::remove_reference_t<P8>>(args[8])));
        }
    } else if constexpr (n == 10) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        using P4 = [:std::meta::type_of(params[4]):];
        using P5 = [:std::meta::type_of(params[5]):];
        using P6 = [:std::meta::type_of(params[6]):];
        using P7 = [:std::meta::type_of(params[7]):];
        using P8 = [:std::meta::type_of(params[8]):];
        using P9 = [:std::meta::type_of(params[9]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                           std::any_cast<std::remove_reference_t<P1>>(args[1]),
                           std::any_cast<std::remove_reference_t<P2>>(args[2]),
                           std::any_cast<std::remove_reference_t<P3>>(args[3]),
                           std::any_cast<std::remove_reference_t<P4>>(args[4]),
                           std::any_cast<std::remove_reference_t<P5>>(args[5]),
                           std::any_cast<std::remove_reference_t<P6>>(args[6]),
                           std::any_cast<std::remove_reference_t<P7>>(args[7]),
                           std::any_cast<std::remove_reference_t<P8>>(args[8]),
                           std::any_cast<std::remove_reference_t<P9>>(args[9]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                std::any_cast<std::remove_reference_t<P0>>(args[0]),
                std::any_cast<std::remove_reference_t<P1>>(args[1]),
                std::any_cast<std::remove_reference_t<P2>>(args[2]),
                std::any_cast<std::remove_reference_t<P3>>(args[3]),
                std::any_cast<std::remove_reference_t<P4>>(args[4]),
                std::any_cast<std::remove_reference_t<P5>>(args[5]),
                std::any_cast<std::remove_reference_t<P6>>(args[6]),
                std::any_cast<std::remove_reference_t<P7>>(args[7]),
                std::any_cast<std::remove_reference_t<P8>>(args[8]),
                std::any_cast<std::remove_reference_t<P9>>(args[9])));
        }
    }
    // ponytail: supports member functions with 0-10 parameters. Extend if needed.
    return std::any{};
}

template <typename T, std::meta::info Member>
std::any getter(void* obj) {
    auto* target = static_cast<T*>(obj);
    auto ptr = &[:Member:];
    return std::any(target->*ptr);
}

template <typename T, std::meta::info Member>
void setter(void* obj, std::any val) {
    auto* target = static_cast<T*>(obj);
    auto ptr = &[:Member:];
    using MemberType = [:std::meta::type_of(Member):];
    target->*ptr = std::any_cast<MemberType>(std::move(val));
}

// --- Static data member getter/setter ---
// No obj pointer — static storage is accessed via &[:Member:].

template <typename T, std::meta::info Member>
std::any static_getter() {
    auto* ptr = &[:Member:];
    return std::any(*ptr);
}

template <typename T, std::meta::info Member>
void static_setter(std::any val) {
    auto* ptr = &[:Member:];
    using MemberType = [:std::meta::type_of(Member):];
    *ptr = std::any_cast<MemberType>(std::move(val));
}

// --- Static member function invoker ---
// No obj pointer — static functions are called directly via &[:Fn:].

template <typename T, std::meta::info Fn>
std::any static_invoker(const std::any* args) {
    auto fn = &[:Fn:];
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Fn));
    constexpr std::size_t n = params.size();
    using R = [:std::meta::return_type_of(Fn):];

    if constexpr (n == 0) {
        if constexpr (std::is_void_v<R>) {
            fn();
            return std::any{};
        } else {
            return std::any(fn());
        }
    } else if constexpr (n == 1) {
        using P0 = [:std::meta::type_of(params[0]):];
        if constexpr (std::is_void_v<R>) {
            fn(std::any_cast<std::remove_reference_t<P0>>(args[0]));
            return std::any{};
        } else {
            return std::any(fn(std::any_cast<std::remove_reference_t<P0>>(args[0])));
        }
    } else if constexpr (n == 2) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        if constexpr (std::is_void_v<R>) {
            fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
               std::any_cast<std::remove_reference_t<P1>>(args[1]));
            return std::any{};
        } else {
            return std::any(fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                              std::any_cast<std::remove_reference_t<P1>>(args[1])));
        }
    } else if constexpr (n == 3) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        if constexpr (std::is_void_v<R>) {
            fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
               std::any_cast<std::remove_reference_t<P1>>(args[1]),
               std::any_cast<std::remove_reference_t<P2>>(args[2]));
            return std::any{};
        } else {
            return std::any(fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                              std::any_cast<std::remove_reference_t<P1>>(args[1]),
                              std::any_cast<std::remove_reference_t<P2>>(args[2])));
        }
    } else if constexpr (n == 4) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        if constexpr (std::is_void_v<R>) {
            fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
               std::any_cast<std::remove_reference_t<P1>>(args[1]),
               std::any_cast<std::remove_reference_t<P2>>(args[2]),
               std::any_cast<std::remove_reference_t<P3>>(args[3]));
            return std::any{};
        } else {
            return std::any(fn(std::any_cast<std::remove_reference_t<P0>>(args[0]),
                              std::any_cast<std::remove_reference_t<P1>>(args[1]),
                              std::any_cast<std::remove_reference_t<P2>>(args[2]),
                              std::any_cast<std::remove_reference_t<P3>>(args[3])));
        }
    }
    // ponytail: supports static functions with 0-4 parameters. Extend if needed.
    return std::any{};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Forward declarations.
// ---------------------------------------------------------------------------
class Class;
class Constructor;
class Function;
class Field;
class StaticField;
class StaticFunction;
class Object;
class Enum;
class Enumerator;

// Forward declaration — defined later, used by Class::find_base.
std::expected<Class, Error> find_class(std::string_view name);

template <typename T>
class Refl;

// ---------------------------------------------------------------------------
// Refl<T> — registration driver.
//
// Instantiating Refl<T> as a variable, or calling ensure_registered<T>(),
// triggers static-initialisation of RegistrarHolder<T>::registrar, which
// populates the global pool with T's metadata.  Refl<T> is NOT needed at
// call sites — once registered, T is found via find_class("T") and
// constructed/called through type-erased handles.
// ---------------------------------------------------------------------------

template <typename T>
class RegistrarHolder {
    static ClassInfo make_info();
public:
    static inline const Registrar registrar{make_info()};
};

// Enum registration — triggered when T is an enum type.
template <typename T>
class EnumRegistrarHolder {
    static EnumInfo make_enum_info();
public:
    static inline const EnumRegistrar registrar{make_enum_info()};
};

template <typename T>
inline void ensure_registered() {
    if constexpr (std::is_enum_v<T>) {
        (void)EnumRegistrarHolder<T>::registrar;
    } else {
        (void)RegistrarHolder<T>::registrar;
    }
}

template <typename T>
class Refl {
public:
    using value_type = T;

    Refl() { ensure_registered<T>(); }
    explicit Refl(T v) : value_(std::move(v)) { ensure_registered<T>(); }

    Refl(const Refl& o) : value_(o.value_) { ensure_registered<T>(); }
    Refl(Refl&& o) noexcept : value_(std::move(o.value_)) { ensure_registered<T>(); }
    Refl& operator=(const Refl&) = default;
    Refl& operator=(Refl&&) = default;

    T& get() { return *value_; }
    const T& get() const { return *value_; }
    T&& take() { return std::move(*value_); }

    static const Registrar& registrar() { return RegistrarHolder<T>::registrar; }

private:
    std::optional<T> value_;
};

// ---------------------------------------------------------------------------
// Object — a type-erased, shared-ownership handle to a heap-allocated
// instance.
//
// Returned by Constructor::call.  Internally holds a std::shared_ptr<void>
// with the deleter baked in, plus the class name for runtime type checks.
// Copyable — copies share ownership.  Use cast<T>() for a fast non-owning
// borrow (T*), or cast_safe<T>() for a checked std::shared_ptr<T> that
// keeps the object alive independently.
// ---------------------------------------------------------------------------

class Object {
public:
    Object() = default;

    Object(std::shared_ptr<void> ptr, std::string class_name)
        : ptr_(std::move(ptr)), class_name_(std::move(class_name)) {}

    // Shared ownership — copies are fine.
    Object(const Object&) = default;
    Object& operator=(const Object&) = default;
    Object(Object&&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;

    // The class name this object was constructed as (for runtime checks).
    const std::string& class_name() const { return class_name_; }

    // Fast cast: returns a non-owning pointer.  The pointer is valid as
    // long as any copy of this Object (or a shared_ptr from cast_safe)
    // is alive.  Caller is responsible for the correct T; mismatch is UB.
    template <typename T>
    T* cast() {
        return static_cast<T*>(ptr_.get());
    }

    template <typename T>
    const T* cast() const {
        return static_cast<const T*>(ptr_.get());
    }

    // Safe cast: checks the class name against T's name at runtime and
    // returns a std::shared_ptr<T> that shares ownership with the Object.
    // Returns Error::TypeError on mismatch, Error::NullHandle if invalid.
    template <typename T>
    std::expected<std::shared_ptr<T>, Error> cast_safe() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (class_name_ != detail::type_name<T>())
            return std::unexpected(Error::TypeError);
        return std::static_pointer_cast<T>(ptr_);
    }

    bool valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    // Non-owning borrow of the internal pointer, for passing to invokers
    // and getters/setters.  Only accessible to friend classes.
    void* raw() { return ptr_.get(); }
    const void* raw() const { return ptr_.get(); }

    std::shared_ptr<void> ptr_;
    std::string class_name_;

    friend class Function;
    friend class Field;
};

// ---------------------------------------------------------------------------
// make_info — gather all reflection metadata for T at compile time.
// ---------------------------------------------------------------------------

template <typename T>
ClassInfo RegistrarHolder<T>::make_info() {
    ClassInfo info;
    info.name = std::string(std::meta::identifier_of(^^T));

    // Base classes.
    static constexpr auto bases = std::define_static_array(
        std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto b : bases) {
        info.base_names.emplace_back(
            std::meta::identifier_of(std::meta::type_of(b)));
    }

    // Data members — generate getter/setter for each.
    static constexpr auto data_members = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : data_members) {
        // Skip bit-fields — pointer-to-member is not valid for them.
        if constexpr (!std::meta::is_bit_field(m)) {
            FieldInfo fi;
            fi.name = std::string(std::meta::identifier_of(m));
            fi.type = std::string(
                std::meta::display_string_of(std::meta::type_of(m)));
            fi.getter = &detail::getter<T, m>;

            // No setter for const members.
            using MemberType = [:std::meta::type_of(m):];
            if constexpr (std::is_const_v<MemberType>) {
                fi.setter = nullptr;
            } else {
                fi.setter = &detail::setter<T, m>;
            }

            info.fields.push_back(std::move(fi));
        }
    }

    // Static data members — generate static getter/setter for each.
    static constexpr auto static_data = std::define_static_array(
        std::meta::static_data_members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : static_data) {
        StaticFieldInfo fi;
        fi.name = std::string(std::meta::identifier_of(m));
        fi.type = std::string(
            std::meta::display_string_of(std::meta::type_of(m)));
        fi.getter = &detail::static_getter<T, m>;

        using MemberType = [:std::meta::type_of(m):];
        if constexpr (std::is_const_v<MemberType>) {
            fi.setter = nullptr;
        } else {
            fi.setter = &detail::static_setter<T, m>;
        }

        info.static_fields.push_back(std::move(fi));
    }

    // All members — filter for constructors and named functions.
    static constexpr auto all_members = std::define_static_array(
        std::meta::members_of(^^T, std::meta::access_context::unchecked()));

    template for (constexpr auto m : all_members) {
        if constexpr (std::meta::is_constructor(m)) {
            static constexpr auto params = std::define_static_array(
                std::meta::parameters_of(m));
            constexpr std::size_t n = params.size();

            // Skip copy/move constructors (first param is T or T&/T&&).
            if constexpr (n == 0) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                info.constructors.push_back(std::move(ci));
            } else if constexpr (n == 1) {
                using P0 = [:std::meta::type_of(params[0]):];
                if constexpr (!std::is_same_v<std::remove_cvref_t<P0>, T>) {
                    ConstructorInfo ci;
                    ci.factory = &detail::factory<T, m>;
                    template for (constexpr auto p : params) {
                        ci.param_types.emplace_back(
                            std::meta::display_string_of(std::meta::type_of(p)));
                    }
                    info.constructors.push_back(std::move(ci));
                }
            } else if constexpr (n >= 2 && n <= detail::max_arity) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                template for (constexpr auto p : params) {
                    ci.param_types.emplace_back(
                        std::meta::display_string_of(std::meta::type_of(p)));
                }
                info.constructors.push_back(std::move(ci));
            }
            // ponytail: n>max_arity constructors are skipped.
        } else if constexpr (std::meta::is_function(m) && std::meta::has_identifier(m)) {
            static constexpr auto fparams = std::define_static_array(
                std::meta::parameters_of(m));
            constexpr std::size_t fn = fparams.size();

            if constexpr (std::meta::is_static_member(m)) {
                // Static member function — no obj pointer.
                if constexpr (fn <= 4) {
                    StaticFunctionInfo fi;
                    fi.name = std::string(std::meta::identifier_of(m));
                    fi.return_type = std::string(
                        std::meta::display_string_of(std::meta::return_type_of(m)));
                    fi.invoker = &detail::static_invoker<T, m>;
                    template for (constexpr auto p : fparams) {
                        fi.param_types.emplace_back(
                            std::meta::display_string_of(std::meta::type_of(p)));
                    }
                    info.static_functions.push_back(std::move(fi));
                }
                // ponytail: static functions with >4 params are skipped.
            } else if constexpr (fn <= detail::max_arity) {
                FunctionInfo fi;
                fi.name = std::string(std::meta::identifier_of(m));
                fi.return_type = std::string(
                    std::meta::display_string_of(std::meta::return_type_of(m)));
                fi.invoker = &detail::invoker<T, m>;
                template for (constexpr auto p : fparams) {
                    fi.param_types.emplace_back(
                        std::meta::display_string_of(std::meta::type_of(p)));
                }
                info.functions.push_back(std::move(fi));
            }
            // ponytail: non-static functions with >max_arity params are skipped.
        }
    }

    return info;
}

// ---------------------------------------------------------------------------
// make_enum_info — gather enum metadata at compile time.
// ---------------------------------------------------------------------------

template <typename T>
EnumInfo EnumRegistrarHolder<T>::make_enum_info() {
    EnumInfo info;
    info.name = std::string(std::meta::identifier_of(^^T));

    static constexpr auto enumerators = std::define_static_array(
        std::meta::enumerators_of(^^T));

    template for (constexpr auto e : enumerators) {
        EnumeratorInfo ei;
        ei.name = std::string(std::meta::identifier_of(e));
        constexpr auto val = std::meta::constant_of(e);
        ei.value = static_cast<long long>([:val:]);
        info.enumerators.push_back(std::move(ei));
    }

    return info;
}

// ---------------------------------------------------------------------------
// Runtime handles — Class, Constructor, Function, Field, Enum, Enumerator.
// ---------------------------------------------------------------------------

class Constructor {
public:
    Constructor() = default;
    Constructor(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::vector<std::string>& param_types() const {
        return owner_->constructors[idx_].param_types;
    }

    template <typename... Args>
    std::expected<Object, Error> call(Args&&... args) {
        if (!valid()) return std::unexpected(Error::NullHandle);

        std::array<std::any, sizeof...(Args)> arg_anys{
            std::any(std::forward<Args>(args))...
        };

        const auto& ci = owner_->constructors[idx_];
        std::shared_ptr<void> result = ci.factory(arg_anys.empty() ? nullptr : arg_anys.data());
        if (!result) return std::unexpected(Error::NullHandle);
        return Object(std::move(result), owner_->name);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Function {
public:
    Function() = default;
    Function(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->functions[idx_].name; }
    const std::vector<std::string>& param_types() const {
        return owner_->functions[idx_].param_types;
    }
    const std::string& return_type() const {
        return owner_->functions[idx_].return_type;
    }

    // Invoke on an Object — the type-erased owning handle from call().
    template <typename... Args>
    std::any invoke(Object& obj, Args&&... args) {
        if (!valid()) return std::any{};
        std::array<std::any, sizeof...(Args)> arg_anys{
            std::any(std::forward<Args>(args))...
        };
        return owner_->functions[idx_].invoker(obj.raw(),
            arg_anys.empty() ? nullptr : arg_anys.data());
    }

    // Invoke on a concrete type — for when you have the real object already.
    template <typename T, typename... Args>
    std::any invoke(T& obj, Args&&... args) {
        if (!valid()) return std::any{};
        std::array<std::any, sizeof...(Args)> arg_anys{
            std::any(std::forward<Args>(args))...
        };
        return owner_->functions[idx_].invoker(static_cast<void*>(&obj),
            arg_anys.empty() ? nullptr : arg_anys.data());
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Field {
public:
    Field() = default;
    Field(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->fields[idx_].name; }
    const std::string& type() const { return owner_->fields[idx_].type; }
    bool is_readonly() const { return owner_->fields[idx_].setter == nullptr; }

    // Get the field value from an Object.
    std::any get(Object& obj) const {
        if (!valid()) return std::any{};
        return owner_->fields[idx_].getter(obj.raw());
    }

    std::any get(const Object& obj) const {
        if (!valid()) return std::any{};
        return owner_->fields[idx_].getter(const_cast<Object&>(obj).raw());
    }

    // Set the field value on an Object.  Returns Error::BadSignature if
    // the field is read-only (const or bit-field), Error::NullHandle if
    // the Field handle is invalid.
    std::expected<void, Error> set(Object& obj, std::any val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (is_readonly()) return std::unexpected(Error::BadSignature);
        owner_->fields[idx_].setter(obj.raw(), std::move(val));
        return {};
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class StaticField {
public:
    StaticField() = default;
    StaticField(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->static_fields[idx_].name; }
    const std::string& type() const { return owner_->static_fields[idx_].type; }
    bool is_readonly() const { return owner_->static_fields[idx_].setter == nullptr; }

    std::any get() const {
        if (!valid()) return std::any{};
        return owner_->static_fields[idx_].getter();
    }

    std::expected<void, Error> set(std::any val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (is_readonly()) return std::unexpected(Error::BadSignature);
        owner_->static_fields[idx_].setter(std::move(val));
        return {};
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class StaticFunction {
public:
    StaticFunction() = default;
    StaticFunction(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->static_functions[idx_].name; }
    const std::vector<std::string>& param_types() const {
        return owner_->static_functions[idx_].param_types;
    }
    const std::string& return_type() const {
        return owner_->static_functions[idx_].return_type;
    }

    template <typename... Args>
    std::any invoke(Args&&... args) {
        if (!valid()) return std::any{};
        std::array<std::any, sizeof...(Args)> arg_anys{
            std::any(std::forward<Args>(args))...
        };
        return owner_->static_functions[idx_].invoker(
            arg_anys.empty() ? nullptr : arg_anys.data());
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Enumerator {
public:
    Enumerator() = default;
    Enumerator(const EnumInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->enumerators[idx_].name; }
    long long value() const { return owner_->enumerators[idx_].value; }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const EnumInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Enum {
public:
    Enum() = default;
    explicit Enum(const EnumInfo* info) : info_(info) {}

    const std::string& name() const { return info_->name; }

    const std::vector<EnumeratorInfo>& enumerators() const {
        return info_->enumerators;
    }

    std::expected<Enumerator, Error> find_enumerator(std::string_view name) const;
    std::expected<Enumerator, Error> find_enumerator(long long value) const;

    bool valid() const { return info_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const EnumInfo* info_ = nullptr;
};

class Class {
public:
    Class() = default;
    explicit Class(const ClassInfo* info) : info_(info) {}

    const std::string& name() const { return info_->name; }

    const std::vector<std::string>& base_names() const {
        return info_->base_names;
    }

    // Resolve a base class by name.  Returns Error::NotFound if not a base.
    std::expected<Class, Error> find_base(std::string_view name) const {
        for (const auto& bn : info_->base_names) {
            if (bn == name) return find_class(bn);
        }
        return std::unexpected(Error::NotFound);
    }

    const std::vector<FieldInfo>& fields() const {
        return info_->fields;
    }

    const std::vector<StaticFieldInfo>& static_fields() const {
        return info_->static_fields;
    }

    const std::vector<StaticFunctionInfo>& static_functions() const {
        return info_->static_functions;
    }

    std::expected<Constructor, Error> find_constructor(
        std::initializer_list<std::string_view> types) const;

    // Find a function by name.  If multiple overloads exist, returns the
    // first match.  Walks base classes if not found in this class.
    std::expected<Function, Error> find_function(std::string_view name) const;

    // Find a function by name and parameter type names.  This disambiguates
    // overloads.  Matching uses the same normalization as find_constructor
    // (ignores const, ref, whitespace).  Walks base classes.
    std::expected<Function, Error> find_function(
        std::string_view name,
        std::initializer_list<std::string_view> types) const;

    // Find all overloads of a function by name.  Does not walk bases.
    std::vector<Function> find_functions(std::string_view name) const;

    std::expected<Field, Error> find_field(std::string_view name) const;

    // Find a static data member by name.  Does not walk bases.
    std::expected<StaticField, Error> find_static_field(std::string_view name) const;

    // Find a static member function by name.  Does not walk bases.
    std::expected<StaticFunction, Error> find_static_function(std::string_view name) const;

    bool valid() const { return info_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* info_ = nullptr;
};

// ---------------------------------------------------------------------------
// Free functions — main entry points.
// ---------------------------------------------------------------------------

inline std::expected<Class, Error> find_class(std::string_view name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto it = class_pool().find(std::string(name));
    if (it == class_pool().end())
        return std::unexpected(Error::NotFound);
    return Class(&it->second);
}

// Enumerate all registered class names.
inline std::vector<std::string> list_all_classes() {
    std::lock_guard<std::mutex> lk(pool_mutex());
    std::vector<std::string> names;
    names.reserve(class_pool().size());
    for (const auto& [name, info] : class_pool())
        names.push_back(name);
    return names;
}

// Find an enum by runtime string name.
inline std::expected<Enum, Error> find_enum(std::string_view name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto it = enum_pool().find(std::string(name));
    if (it == enum_pool().end())
        return std::unexpected(Error::NotFound);
    return Enum(&it->second);
}

// Enumerate all registered enum names.
inline std::vector<std::string> list_all_enums() {
    std::lock_guard<std::mutex> lk(pool_mutex());
    std::vector<std::string> names;
    names.reserve(enum_pool().size());
    for (const auto& [name, info] : enum_pool())
        names.push_back(name);
    return names;
}

inline std::string normalize_type(std::string_view sv) {
    std::string result;
    for (char c : sv) {
        if (c == ' ' || c == '\t') continue;
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    auto strip_suffix = [&](std::string_view suffix) {
        if (result.size() >= suffix.size() &&
            result.compare(result.size() - suffix.size(),
                           suffix.size(), suffix) == 0) {
            result.erase(result.size() - suffix.size());
        }
    };
    strip_suffix("&&");
    strip_suffix("&");
    while (result.size() >= 5 && result.compare(result.size() - 5, 5, "const") == 0) {
        result.erase(result.size() - 5);
    }
    std::string clean;
    for (char c : result) {
        if (c != ' ' && c != '\t') clean += c;
    }
    return clean;
}

inline std::expected<Constructor, Error> Class::find_constructor(
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(normalize_type(t));

    for (std::size_t i = 0; i < info_->constructors.size(); ++i) {
        const auto& ctor = info_->constructors[i];
        if (ctor.param_types.size() != query.size()) continue;

        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j) {
            if (normalize_type(ctor.param_types[j]) != query[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return Constructor(info_, i);
    }
    return std::unexpected(Error::BadSignature);
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->functions.size(); ++i) {
        if (info_->functions[i].name == name)
            return Function(info_, i);
    }
    // Walk up the hierarchy.
    // ponytail: single inheritance only — multiple inheritance with offset
    // bases would produce wrong pointer adjustments in the invoker.
    for (const auto& bn : info_->base_names) {
        auto base = find_class(bn);
        if (base) {
            auto fn = base->find_function(name);
            if (fn) return fn;
        }
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Field, Error> Class::find_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->fields.size(); ++i) {
        if (info_->fields[i].name == name)
            return Field(info_, i);
    }
    // Walk up the hierarchy.
    // ponytail: single inheritance only — see find_function for rationale.
    for (const auto& bn : info_->base_names) {
        auto base = find_class(bn);
        if (base) {
            auto field = base->find_field(name);
            if (field) return field;
        }
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name,
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(normalize_type(t));

    for (std::size_t i = 0; i < info_->functions.size(); ++i) {
        const auto& fn = info_->functions[i];
        if (fn.name != name) continue;
        if (fn.param_types.size() != query.size()) continue;

        bool match = true;
        for (std::size_t j = 0; j < query.size(); ++j) {
            if (normalize_type(fn.param_types[j]) != query[j]) {
                match = false;
                break;
            }
        }
        if (match)
            return Function(info_, i);
    }
    // Walk up the hierarchy.
    for (const auto& bn : info_->base_names) {
        auto base = find_class(bn);
        if (base) {
            auto fn = base->find_function(name, types);
            if (fn) return fn;
        }
    }
    return std::unexpected(Error::NotFound);
}

inline std::vector<Function> Class::find_functions(
    std::string_view name) const {
    std::vector<Function> results;
    if (!valid()) return results;

    for (std::size_t i = 0; i < info_->functions.size(); ++i) {
        if (info_->functions[i].name == name)
            results.push_back(Function(info_, i));
    }
    return results;
}

inline std::expected<Enumerator, Error> Enum::find_enumerator(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->enumerators.size(); ++i) {
        if (info_->enumerators[i].name == name)
            return Enumerator(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Enumerator, Error> Enum::find_enumerator(
    long long value) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->enumerators.size(); ++i) {
        if (info_->enumerators[i].value == value)
            return Enumerator(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<StaticField, Error> Class::find_static_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->static_fields.size(); ++i) {
        if (info_->static_fields[i].name == name)
            return StaticField(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->static_functions.size(); ++i) {
        if (info_->static_functions[i].name == name)
            return StaticFunction(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

}  // namespace refl
