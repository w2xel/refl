// refl — C++26 compile-time-reflection-based runtime reflection framework (core).
//
// Register a type T with refl::Reg<T> (or refl::ensure_registered<T>()) so it
// enters a global pool queryable at runtime via find_class("T").  From a
// Class handle you can find constructors, member functions, and data fields
// by name or parameter-type name, then construct objects, invoke functions,
// and get/set fields through the returned handles.
//
// The framework is type-erased at the call boundary: Constructor::call
// returns an Object (an owning, type-erased handle).  You only need to know
// the concrete type when you explicitly cast an Object to get at its
// members directly.
//
// Lookups return std::expected<T, Error> — dereference with operator* or
// check has_value()/error(), as shown in the design docs.
//
// The typed dispatch / dynamic-implementation layer (Dyn<T>) lives in
// refl/dyn.hpp and includes this header.
#pragma once

#include <meta>
#include <any>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
    NullHandle,
    TypeError,
    ArityMismatch,   // argument count doesn't match the member's param count
    ReadOnly,        // write to a const / bit-field member
    NotCopyable,     // clone of a non-copy-constructible class
};

inline std::string_view to_string(Error e) {
    switch (e) {
    case Error::NotFound:       return "NotFound";
    case Error::NullHandle:    return "NullHandle";
    case Error::TypeError:     return "TypeError";
    case Error::ArityMismatch:  return "ArityMismatch";
    case Error::ReadOnly:       return "ReadOnly";
    case Error::NotCopyable:    return "NotCopyable";
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
using CloneFn         = std::shared_ptr<void> (*)(void* obj);

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

struct BaseInfo {
    std::string name;
    std::ptrdiff_t offset;  // byte offset of this base within the derived class
};

struct ClassInfo {
    std::string name;
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
    std::vector<StaticFieldInfo> static_fields;
    std::vector<ConstructorInfo> constructors;
    std::vector<FunctionInfo> functions;
    std::vector<StaticFunctionInfo> static_functions;
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

// Forward declaration — defined later, used by walk_bases.
class Class;
std::expected<Class, Error> find_class(std::string_view name);

// ---------------------------------------------------------------------------
// Type-erased factory / invoker / getter / setter templates.
//
// Each is parameterised on the target type T and (where relevant) the
// compile-time meta::info of the specific constructor, member function, or
// data member.  template-for in make_info instantiates one of these per
// reflected member, and stores its address in ClassInfo.
// ---------------------------------------------------------------------------

namespace detail {

// Normalize a type string: lowercase, strip whitespace, const, ref qualifiers.
// Applied at storage time so lookups compare raw strings.  Handles both
// west const (`const int`) and east const (`int const`): const is stripped
// while whitespace is still present, so it matches as a distinct token
// rather than a substring (e.g. `const_iterator` is left untouched).
inline std::string normalize_type(std::string_view sv) {
    std::string s;
    for (char c : sv)
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto strip_suffix = [&](std::string_view suffix) {
        if (s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(),
                       suffix.size(), suffix) == 0) {
            s.erase(s.size() - suffix.size());
        }
    };
    auto strip_prefix = [&](std::string_view prefix) {
        if (s.size() >= prefix.size() &&
            s.compare(0, prefix.size(), prefix) == 0) {
            s.erase(0, prefix.size());
        }
    };

    // Strip ref qualifiers, then const (trailing east, leading west),
    // while whitespace separates "const" from the type name.
    strip_suffix("&&");
    strip_suffix("&");
    while (s.size() >= 6 && s.compare(s.size() - 6, 6, " const") == 0)
        s.erase(s.size() - 6);
    strip_prefix("const ");

    std::string result;
    for (char c : s)
        if (c != ' ' && c != '\t') result += c;
    return result;
}

// Match a query (already-normalized type names) against a candidate's
// param_types (also pre-normalized at storage time).  Returns true if
// the param counts and types match.
inline bool match_signature(
    const std::vector<std::string>& candidate_types,
    const std::vector<std::string>& query) {
    if (candidate_types.size() != query.size()) return false;
    for (std::size_t j = 0; j < query.size(); ++j) {
        if (candidate_types[j] != query[j]) return false;
    }
    return true;
}

// Build an std::any array from forwarded arguments.  The caller
// checks `.empty()` on the returned array to get the pointer.
template <typename... Args>
std::array<std::any, sizeof...(Args)>
make_arg_anys(Args&&... args) {
    return std::array<std::any, sizeof...(Args)>{
        std::any(std::forward<Args>(args))...
    };
}

// Compile-time fully-qualified type name for pool keys and safe-cast checks.
// display_string_of yields the qualified name (e.g. "ns::Point"), so two
// classes with the same unqualified name in different namespaces no longer
// collide in the pool or in cast_safe.  Global-scope types have no prefix.
template <typename T>
consteval std::string_view type_name() {
    return std::meta::display_string_of(^^T);
}

template <typename T, std::meta::info Ctor>
std::shared_ptr<void> factory(const std::any* args) {
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Ctor));
    constexpr std::size_t n = params.size();

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) {
        using P = [:std::meta::type_of(params[J]):];
        return std::any_cast<std::remove_reference_t<P>>(args[J]);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> std::shared_ptr<void> {
        if constexpr (n == 0) {
            return std::make_shared<T>();
        } else {
            return std::make_shared<T>(extract(std::integral_constant<std::size_t, I>{})...);
        }
    }(std::make_index_sequence<n>{});
}

template <typename T, std::meta::info Fn>
std::any invoker(void* obj, const std::any* args) {
    auto* target = static_cast<T*>(obj);
    auto mfn = &[:Fn:];
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Fn));
    constexpr std::size_t n = params.size();
    using R = [:std::meta::return_type_of(Fn):];

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) {
        using P = [:std::meta::type_of(params[J]):];
        return std::any_cast<std::remove_reference_t<P>>(args[J]);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> std::any {
        if constexpr (n == 0) {
            if constexpr (std::is_void_v<R>) {
                (target->*mfn)();
                return std::any{};
            } else {
                return std::any((target->*mfn)());
            }
        } else {
            if constexpr (std::is_void_v<R>) {
                (target->*mfn)(extract(std::integral_constant<std::size_t, I>{})...);
                return std::any{};
            } else {
                return std::any((target->*mfn)(
                    extract(std::integral_constant<std::size_t, I>{})...));
            }
        }
    }(std::make_index_sequence<n>{});
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

// --- Cloner ---
// Deep-copies the object via the copy constructor.  Only generated if T
// is copy-constructible.

template <typename T>
std::shared_ptr<void> clone(void* obj) {
    auto* src = static_cast<T*>(obj);
    return std::make_shared<T>(*src);
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

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) {
        using P = [:std::meta::type_of(params[J]):];
        return std::any_cast<std::remove_reference_t<P>>(args[J]);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> std::any {
        if constexpr (n == 0) {
        if constexpr (std::is_void_v<R>) {
            fn();
            return std::any{};
        } else {
            return std::any(fn());
        }
    } else {
        if constexpr (std::is_void_v<R>) {
            fn(extract(std::integral_constant<std::size_t, I>{})...);
            return std::any{};
        } else {
            return std::any(fn(extract(std::integral_constant<std::size_t, I>{})...));
        }
    }
    }(std::make_index_sequence<n>{});
}

// --- Arity + type-erased call guards ---
//
// The invokers, factory, getters, and setters above forward arguments as
// std::any.  An arity mismatch indexes the any array out of bounds (UB),
// and a type mismatch inside std::any_cast throws std::bad_any_cast.  The
// helpers below wrap the raw calls so the runtime handles report a clean
// Error instead of crashing or throwing across the C boundary.

inline std::expected<std::shared_ptr<void>, Error>
checked_factory(FactoryFn fn, const std::any* args) {
    try {
        return fn(args);
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<std::any, Error>
checked_invoke(InvokerFn fn, void* obj, const std::any* args) {
    try {
        return fn(obj, args);
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<std::any, Error>
checked_static_invoke(StaticInvokerFn fn, const std::any* args) {
    try {
        return fn(args);
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<std::any, Error>
checked_get(GetterFn fn, void* obj) {
    try {
        return fn(obj);
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<std::any, Error>
checked_static_get(StaticGetterFn fn) {
    try {
        return fn();
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<void, Error>
checked_static_set(StaticSetterFn fn, std::any val) {
    try {
        fn(std::move(val));
        return {};
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

inline std::expected<void, Error>
checked_set(SetterFn fn, void* obj, std::any val) {
    try {
        fn(obj, std::move(val));
        return {};
    } catch (const std::bad_any_cast&) {
        return std::unexpected(Error::TypeError);
    }
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

// Unlocked helper — caller must hold pool_mutex.
// Returns the byte offset of `base_name` within `derived_name` (accumulated
// through the base hierarchy), or nullopt if not found.  Offset 0 means
// either exact match or first base.
inline std::optional<std::ptrdiff_t>
base_offset_unlocked(std::string_view derived_name, std::string_view base_name) {
    if (derived_name == base_name) return 0;
    auto it = class_pool().find(std::string(derived_name));
    if (it == class_pool().end()) return std::nullopt;
    for (const auto& b : it->second.bases) {
        if (b.name == base_name) return b.offset;
        auto deeper = base_offset_unlocked(b.name, base_name);
        if (deeper) return b.offset + *deeper;
    }
    return std::nullopt;
}

// Check whether `base_name` is a base class of `derived_name` by walking
// the registered bases hierarchy.  Used by Object::is_class and cast_safe
// for runtime upcast checks.
inline bool is_base_of_unlocked(std::string_view derived_name, std::string_view base_name) {
    return base_offset_unlocked(derived_name, base_name).has_value();
}

// Check whether `base_name` is a base class of `derived_name`.
// Also returns the accumulated byte offset if it is.
inline std::optional<std::ptrdiff_t>
is_base_of_with_offset(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return base_offset_unlocked(derived_name, base_name);
}

inline bool is_base_of(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return is_base_of_unlocked(derived_name, base_name);
}

// Adjust a pointer from a most-derived object to a base subobject.
// Returns nullptr if the base is not found (guards against pool races
// after is_class passed).  Caller need not hold pool_mutex — this helper
// takes it for the offset lookup.
//
// ponytail: the call sites do is_class() then adjust_to_base() in two
// separate lock acquisitions.  This is a TOCTOU window in theory, but the
// pool is append-only at static-init time — no unregister exists — so a
// class cannot disappear between the check and the adjustment.
inline void* adjust_to_base(void* obj, std::string_view derived_name,
                            std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto off = base_offset_unlocked(derived_name, base_name);
    if (!off) return nullptr;
    return static_cast<char*>(obj) + *off;
}

// ---------------------------------------------------------------------------
// Registration — the single responsibility of the core.
//
// Call ensure_registered<T>() (or instantiate refl::Reg<T> as a static
// variable) to trigger static-initialisation of RegistrarHolder<T>::registrar,
// which populates the global pool with T's metadata.  Once registered, T is
// found via find_class("T") and constructed/called through type-erased
// handles — no wrapper object needed at call sites.
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

// Registration-only tag.  Instantiate as a static variable to register T
// in the global pool at static-initialisation time — the core's sole
// user-facing side effect.
//
//   [[maybe_unused]] static refl::Reg<MyClass> reg_my_class;
//   // ... find_class("MyClass") now works.
template <typename T>
struct Reg {
    Reg() { ensure_registered<T>(); }
};
// ---------------------------------------------------------------------------
// Object — a type-erased, shared-ownership handle to a heap-allocated
// instance.
//
// Returned by Constructor::call.  Internally holds a std::shared_ptr<void>
// with the deleter baked in, plus the class name for runtime type checks.
// Copyable — copies share ownership.  Use cast_safe<T>() for a checked
// std::shared_ptr<T> that keeps the object alive independently.  The cast
// checks not only the exact class but also walks the base-class hierarchy,
// so casting a derived Object to a base type succeeds.
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

    // Check whether this object is of the given class or a class derived
    // from it.  Walks the base-class hierarchy at runtime.
    bool is_class(std::string_view name) const {
        if (!valid()) return false;
        if (class_name_ == name) return true;
        return is_base_of(class_name_, name);
    }

    // Safe cast: checks the class name against T's name at runtime and
    // returns a std::shared_ptr<T> that shares ownership with the Object.
    // Succeeds if T matches the object's class or any of its bases.
    // Returns Error::TypeError on mismatch, Error::NullHandle if invalid.
    template <typename T>
    std::expected<std::shared_ptr<T>, Error> cast_safe() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto tname = detail::type_name<T>();
        // One hierarchy walk: 0 for exact match or first base, nonzero for
        // offset bases, nullopt if T is not the class or a base.
        auto off = is_base_of_with_offset(class_name_, tname);
        if (!off) return std::unexpected(Error::TypeError);
        auto* adjusted = static_cast<char*>(ptr_.get()) + *off;
        // Alias: share the control block with the original, point at the
        // offset-adjusted T (via void* — the offset is computed from
        // reflection metadata, not from the type system).
        return std::shared_ptr<T>(ptr_, static_cast<T*>(static_cast<void*>(adjusted)));
    }

    // Deep-copy the object through the type-erased handle.  Returns a
    // new Object with independent ownership.  Returns Error::NotCopyable
    // if the class is not copy-constructible, Error::NullHandle if invalid.
    std::expected<Object, Error> clone() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(class_name_);
        if (it == class_pool().end() || !it->second.clone)
            return std::unexpected(Error::NotCopyable);
        auto copied = it->second.clone(ptr_.get());
        return Object(std::move(copied), class_name_);
    }

    // Debug string: class name and pointer address.
    std::string to_string() const {
        if (!valid()) return "Object(invalid)";
        return "Object(" + class_name_ + " @ " +
               std::to_string(reinterpret_cast<std::uintptr_t>(ptr_.get())) + ")";
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
    info.name = std::string(std::meta::display_string_of(^^T));

    // Base classes — store name + byte offset within T.
    static constexpr auto bases = std::define_static_array(
        std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto b : bases) {
        info.bases.push_back({
            std::string(std::meta::display_string_of(std::meta::type_of(b))),
            std::meta::offset_of(b).bytes,
        });
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
                            detail::normalize_type(
                            std::meta::display_string_of(std::meta::type_of(p))));
                    }
                    info.constructors.push_back(std::move(ci));
                }
            } else if constexpr (n >= 2) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                template for (constexpr auto p : params) {
                    ci.param_types.emplace_back(
                            detail::normalize_type(
                            std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.constructors.push_back(std::move(ci));
            }
        } else if constexpr (std::meta::is_function(m) && std::meta::has_identifier(m)) {
            static constexpr auto fparams = std::define_static_array(
                std::meta::parameters_of(m));

            if constexpr (std::meta::is_static_member(m)) {
                // Static member function — no obj pointer.
                StaticFunctionInfo fi;
                fi.name = std::string(std::meta::identifier_of(m));
                fi.return_type = std::string(
                    std::meta::display_string_of(std::meta::return_type_of(m)));
                fi.invoker = &detail::static_invoker<T, m>;
                template for (constexpr auto p : fparams) {
                    fi.param_types.emplace_back(
                        detail::normalize_type(
                        std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.static_functions.push_back(std::move(fi));
            } else {
                FunctionInfo fi;
                fi.name = std::string(std::meta::identifier_of(m));
                fi.return_type = std::string(
                    std::meta::display_string_of(std::meta::return_type_of(m)));
                fi.invoker = &detail::invoker<T, m>;
                template for (constexpr auto p : fparams) {
                    fi.param_types.emplace_back(
                        detail::normalize_type(
                        std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.functions.push_back(std::move(fi));
            }
        }
    }

    // Clone function — only if T is copy-constructible.
    if constexpr (std::is_copy_constructible_v<T>) {
        info.clone = &detail::clone<T>;
    }

    return info;
}
// ---------------------------------------------------------------------------

template <typename T>
EnumInfo EnumRegistrarHolder<T>::make_enum_info() {
    EnumInfo info;
    info.name = std::string(std::meta::display_string_of(^^T));

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

    // Construct via the type-erased factory.  Returns Error::ArityMismatch
    // if the argument count doesn't match the constructor's parameter count,
    // Error::TypeError if an argument's std::any type doesn't match the
    // parameter type, Error::NullHandle if the handle is invalid.
    template <typename... Args>
    std::expected<Object, Error> call(Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto& ci = owner_->constructors[idx_];
        if (sizeof...(Args) != ci.param_types.size())
            return std::unexpected(Error::ArityMismatch);

        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();

        auto result = detail::checked_factory(ci.factory, args_ptr);
        if (!result) return std::unexpected(result.error());
        return Object(std::move(*result), owner_->name);
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
    // Returns Error::TypeError if obj is not the function's class (or a
    // derived class) or if an argument's std::any type doesn't match the
    // parameter type, Error::ArityMismatch if the argument count is wrong,
    // Error::NullHandle if the handle is invalid.
    template <typename... Args>
    std::expected<std::any, Error> invoke(Object& obj, Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        const auto& fi = owner_->functions[idx_];
        if (sizeof...(Args) != fi.param_types.size())
            return std::unexpected(Error::ArityMismatch);
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        void* adj = adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_invoke(fi.invoker, adj, args_ptr);
    }

    // Invoke on a concrete type — for when you have the real object already.
    // Returns Error::TypeError if T is not the function's class or a class
    // derived from it, Error::ArityMismatch if the argument count is wrong,
    // Error::NullHandle if the handle is invalid.
    template <typename T, typename... Args>
    std::expected<std::any, Error> invoke(T& obj, Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto& fi = owner_->functions[idx_];
        if (sizeof...(Args) != fi.param_types.size())
            return std::unexpected(Error::ArityMismatch);
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        constexpr auto tname = detail::type_name<T>();
        void* adj = adjust_to_base(static_cast<void*>(&obj), tname, owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_invoke(fi.invoker, adj, args_ptr);
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

    // Get the field value from an Object.  Returns Error::TypeError if obj
    // is not the field's class (or a derived class), Error::NullHandle if
    // the handle is invalid.
    std::expected<std::any, Error> get(Object& obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        void* adj = adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_get(owner_->fields[idx_].getter, adj);
    }

    std::expected<std::any, Error> get(const Object& obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        void* adj = adjust_to_base(const_cast<Object&>(obj).raw(),
                                   obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_get(owner_->fields[idx_].getter, adj);
    }

    // Set the field value on an Object.  Returns Error::ReadOnly if
    // the field is read-only (const or bit-field), Error::TypeError if obj
    // is not the field's class (or a derived class) or the std::any value's
    // type doesn't match the field type, Error::NullHandle if the Field
    // handle is invalid.
    std::expected<void, Error> set(Object& obj, std::any val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        if (is_readonly()) return std::unexpected(Error::ReadOnly);
        void* adj = adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_set(owner_->fields[idx_].setter,
                                   adj, std::move(val));
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

    // Get the static field value.  Returns Error::NullHandle if the handle
    // is invalid.
    std::expected<std::any, Error> get() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        return detail::checked_static_get(owner_->static_fields[idx_].getter);
    }

    // Set the static field value.  Returns Error::ReadOnly if the field
    // is read-only (const), Error::TypeError if the std::any value's type
    // doesn't match the field type, Error::NullHandle if the handle is invalid.
    std::expected<void, Error> set(std::any val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (is_readonly()) return std::unexpected(Error::ReadOnly);
        return detail::checked_static_set(owner_->static_fields[idx_].setter,
                                          std::move(val));
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

    // Invoke the static function.  Returns Error::ArityMismatch if the
    // argument count is wrong, Error::TypeError if an argument's std::any
    // type doesn't match the parameter type, Error::NullHandle if the
    // handle is invalid.
    template <typename... Args>
    std::expected<std::any, Error> invoke(Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto& sf = owner_->static_functions[idx_];
        if (sizeof...(Args) != sf.param_types.size())
            return std::unexpected(Error::ArityMismatch);
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        return detail::checked_static_invoke(sf.invoker, args_ptr);
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

    const std::vector<BaseInfo>& bases() const {
        return info_->bases;
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

    const std::vector<ConstructorInfo>& constructors() const {
        return info_->constructors;
    }

    const std::vector<FunctionInfo>& functions() const {
        return info_->functions;
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

    // Find a static data member by name.  Walks base classes.
    std::expected<StaticField, Error> find_static_field(std::string_view name) const;

    // Find a static member function by name.  Walks base classes.
    std::expected<StaticFunction, Error> find_static_function(std::string_view name) const;

    // Find a static member function by name and parameter type names.
    // Disambiguates overloads.  Walks base classes.
    std::expected<StaticFunction, Error> find_static_function(
        std::string_view name,
        std::initializer_list<std::string_view> types) const;

    // Find all overloads of a static function by name.  Does not walk bases.
    std::vector<StaticFunction> find_static_functions(std::string_view name) const;

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

// Walk the base-class hierarchy, calling `finder(base_class)` on each
// base until one returns a value.  Returns the first hit or Error::NotFound.
template <typename R, typename Finder>
std::expected<R, Error> walk_bases(
    const std::vector<BaseInfo>& bases,
    Finder&& finder) {
    for (const auto& b : bases) {
        auto base = find_class(b.name);
        if (base) {
            auto result = finder(*base);
            if (result) return result;
        }
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Constructor, Error> Class::find_constructor(
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(detail::normalize_type(t));

    for (std::size_t i = 0; i < info_->constructors.size(); ++i) {
        if (detail::match_signature(info_->constructors[i].param_types, query))
            return Constructor(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->functions.size(); ++i) {
        if (info_->functions[i].name == name)
            return Function(info_, i);
    }
    return walk_bases<Function>(info_->bases,
        [name](const Class& b) { return b.find_function(name); });
}

inline std::expected<Field, Error> Class::find_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->fields.size(); ++i) {
        if (info_->fields[i].name == name)
            return Field(info_, i);
    }
    return walk_bases<Field>(info_->bases,
        [name](const Class& b) { return b.find_field(name); });
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name,
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(detail::normalize_type(t));

    for (std::size_t i = 0; i < info_->functions.size(); ++i) {
        const auto& fn = info_->functions[i];
        if (fn.name == name && detail::match_signature(fn.param_types, query))
            return Function(info_, i);
    }
    return walk_bases<Function>(info_->bases,
        [name, types](const Class& b) { return b.find_function(name, types); });
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
    return walk_bases<StaticField>(info_->bases,
        [name](const Class& b) { return b.find_static_field(name); });
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->static_functions.size(); ++i) {
        if (info_->static_functions[i].name == name)
            return StaticFunction(info_, i);
    }
    return walk_bases<StaticFunction>(info_->bases,
        [name](const Class& b) { return b.find_static_function(name); });
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name,
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(detail::normalize_type(t));

    for (std::size_t i = 0; i < info_->static_functions.size(); ++i) {
        const auto& sf = info_->static_functions[i];
        if (sf.name == name && detail::match_signature(sf.param_types, query))
            return StaticFunction(info_, i);
    }
    return walk_bases<StaticFunction>(info_->bases,
        [name, types](const Class& b) { return b.find_static_function(name, types); });
}

inline std::vector<StaticFunction> Class::find_static_functions(
    std::string_view name) const {
    std::vector<StaticFunction> results;
    if (!valid()) return results;

    for (std::size_t i = 0; i < info_->static_functions.size(); ++i) {
        if (info_->static_functions[i].name == name)
            results.push_back(StaticFunction(info_, i));
    }
    return results;
}

}  // namespace refl
