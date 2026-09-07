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
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
// ponytail: BadSignature is overloaded for two distinct failure modes —
// write to a read-only (const or bit-field) field, and clone of a
// non-copy-constructible class. Split into separate enumerators if a caller
// ever needs to distinguish them.

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

struct ClassInfo {
    std::string name;
    std::vector<std::string> base_names;
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
// Applied at storage time so lookups compare raw strings.
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

// Compile-time type name for safe-cast checks.
// ponytail: identifier_of yields the unqualified name, so two classes with
// the same name in different namespaces collide in the pool and in cast_safe.
// Use a qualified/mangled name if cross-namespace registration is needed.
template <typename T>
consteval std::string_view type_name() {
    return std::meta::identifier_of(^^T);
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
inline bool is_base_of_unlocked(std::string_view derived_name, std::string_view base_name) {
    auto it = class_pool().find(std::string(derived_name));
    if (it == class_pool().end()) return false;
    for (const auto& bn : it->second.base_names) {
        if (bn == base_name) return true;
        if (is_base_of_unlocked(bn, base_name)) return true;
    }
    return false;
}

// Check whether `base_name` is a base class of `derived_name` by walking
// the registered base_names hierarchy.  Used by Object::is_class and
// cast_safe for runtime upcast checks.
inline bool is_base_of(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return is_base_of_unlocked(derived_name, base_name);
}

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

// ---------------------------------------------------------------------------
// TypedMethod<Sigs...> — variadic callable, one template arg per overload
// signature.  Each Sigs is a function type R(Args...).  operator() uses a
// concept to pick the matching Sig at compile time and returns that Sig's
// return type — no std::variant, no std::any at the call site.
//
//   TypedMethod<int(), void(int), void(int,int)>
//     p->sum()       → int
//     p->set(42)     → void
//     p->set(1, 2)   → void
//
// Mixed return types are fine: TypedMethod<int(int), double(double)>
//     p->compute(3)    → int
//     p->compute(3.0)   → double
//
// The invoker returns std::any internally; the cast to the real return
// type happens inside operator() — the caller never sees std::any.
// No std::function, no heap, no variant.
// ---------------------------------------------------------------------------

// Extract R and Args... from a function type R(Args...).
template <typename Sig> struct sig_traits;
template <typename R, typename... Args>
struct sig_traits<R(Args...)> {
    using return_type = R;
    using args_type = std::tuple<std::remove_cvref_t<Args>...>;
};

// Concept: do the call's argument types match this signature?
template <typename Sig, typename... CallArgs>
concept matches_sig = std::same_as<
    typename sig_traits<Sig>::args_type,
    std::tuple<std::remove_cvref_t<CallArgs>...>>;

// Helper alias for building function types via substitute.
template <typename R, typename... Args> using fn_type = R(Args...);

namespace detail {

// consteval: build a function-type reflection R(Args...) from a member.
consteval std::meta::info make_fn_sig(std::meta::info m) {
    auto rt = std::meta::return_type_of(m);
    auto params = std::meta::parameters_of(m);
    std::vector<std::meta::info> args = {rt};
    for (auto p : params)
        args.push_back(std::meta::type_of(p));
    return std::meta::substitute(^^fn_type, args);
}

// consteval: collect all overload signatures for a function name.
consteval std::vector<std::meta::info>
collect_sigs(std::meta::info type, std::string_view name) {
    std::vector<std::meta::info> result;
    for (auto m : std::meta::members_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_function(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m)
            && std::meta::identifier_of(m) == name)
            result.push_back(make_fn_sig(m));
    }
    return result;
}

}  // namespace detail

template <typename... Sigs>
struct TypedMethod {
    struct OverloadEntry {
        InvokerFn invoker;
    };
    const OverloadEntry* overloads = nullptr;
    std::size_t num = 0;
    void* obj = nullptr;

    void (*after_call)(void* ctx, std::any& result) = nullptr;
    void* hook_ctx = nullptr;

    template <typename... Args>
    auto operator()(Args&&... args) {
        return call_dispatch<0, Args...>(std::forward<Args>(args)...);
    }

    template <std::size_t I, typename... Args>
    auto call_dispatch(Args&&... args) {
        using Sig = std::tuple_element_t<I, std::tuple<Sigs...>>;
        if constexpr (matches_sig<Sig, Args...>) {
            using R = typename sig_traits<Sig>::return_type;
            auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
            const std::any* args_ptr =
                arg_anys.empty() ? nullptr : arg_anys.data();
            std::any result = overloads[I].invoker(obj, args_ptr);
            if (after_call) after_call(hook_ctx, result);
            if constexpr (std::is_void_v<R>) return;
            else return std::any_cast<R>(result);
        } else {
            if constexpr (I + 1 < sizeof...(Sigs))
                return call_dispatch<I + 1, Args...>(std::forward<Args>(args)...);
            else
                static_assert(sizeof...(Args) == 0, "no matching overload");
        }
    }
};

// ---------------------------------------------------------------------------
// TypedProperty<T, Readonly> — mimics a public data member via operator=
// and implicit conversion.  p->x = 42 writes; int v = p->x reads.
// Const members use Readonly=true, which deletes operator= at compile time.
// ---------------------------------------------------------------------------
template <typename T, bool Readonly = false>
struct TypedProperty {
    GetterFn getter = nullptr;
    SetterFn setter = nullptr;  // always nullptr when Readonly=true
    void* obj = nullptr;

    void (*after_set)(void* ctx, std::any& val) = nullptr;
    void* hook_ctx = nullptr;

    // Implicit conversion to T (read).
    operator T() const {
        if (!getter || !obj) throw std::bad_any_cast{};
        return std::any_cast<T>(getter(obj));
    }

    // Assignment from T (write).  Compile error when Readonly=true.
    void operator=(T val) requires (!Readonly) {
        if (!setter || !obj) return;
        setter(obj, std::any(std::move(val)));
        if (after_set) {
            std::any current = getter(obj);
            after_set(hook_ctx, current);
        }
    }

    static constexpr bool is_readonly() { return Readonly; }
};

// ---------------------------------------------------------------------------
// Field-type builders — must be after TypedMethod/TypedProperty definitions
// because they use ^^TypedMethod / ^^TypedProperty in consteval substitute().
// ---------------------------------------------------------------------------
namespace detail {

// Check if a dispatch field type is a TypedMethod specialization.
template <typename T> struct is_typed_method : std::false_type {};
template <typename... Sigs> struct is_typed_method<TypedMethod<Sigs...>>
    : std::true_type {};
template <typename T> inline constexpr bool is_typed_method_v =
    is_typed_method<T>::value;

// consteval: build the TypedMethod<Sigs...> type for a function name.
consteval std::meta::info make_typed_method_type(std::meta::info type,
                                                    std::string_view name) {
    return std::meta::substitute(^^TypedMethod, collect_sigs(type, name));
}

// consteval: build the TypedProperty field type for a data member name.
// Passes Readonly=true for const members (deletes operator= at compile time).
consteval std::meta::info make_property_field_type(std::meta::info type,
                                                       std::string_view name) {
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked())) {
        if (!std::meta::is_bit_field(m) && std::meta::has_identifier(m)
            && std::meta::identifier_of(m) == name) {
            auto mt = std::meta::type_of(m);
            bool is_const = std::meta::is_const_type(mt);
            auto clean_mt = std::meta::substitute(^^std::remove_cv_t,
                std::initializer_list<std::meta::info>{mt});
            return std::meta::substitute(^^TypedProperty,
                {clean_mt, std::meta::reflect_constant(is_const)});
        }
    }
    return std::meta::info{};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Refl<T> — typed proxy with compile-time-synthesized dispatch struct.
//
//   Refl<Point> p(1, 2);
//   p->set(10, 20);           // TypedMethod<void(int), void(int,int)>
//   int s = p->sum();          // TypedMethod<int()> — real return type!
//   int x = p->x.get();        // TypedProperty<int> — real type!
//   p->x.set(42);             // typed set
//
// Mixed return types: TypedMethod<int(int), double(double)>
//   int  i = p->compute(3);    // returns int
//   double d = p->compute(3.0); // returns double
//
// Hooks (Qt-style, after-only):
//   p.connect("sum", [](std::any& r) { ... });
//   p.on_change("x", [](std::any& v) { ... });
//
// Refl<T> is non-copyable, non-movable.
// ---------------------------------------------------------------------------
template <typename T>
class Refl {
    struct Dispatch;
    consteval {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            std::vector<std::meta::info> specs;
            std::vector<std::string> seen_fns;
            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_function(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)) {
                    constexpr auto nm = std::meta::identifier_of(m);
                    auto nm_str = std::string(nm);
                    bool dup = false;
                    for (const auto& s : seen_fns)
                        if (s == nm_str) { dup = true; break; }
                    if (!dup) {
                        seen_fns.push_back(nm_str);
                        constexpr auto field_type =
                            detail::make_typed_method_type(^^T, nm);
                        specs.push_back(std::meta::data_member_spec(
                            field_type, {.name=nm_str}));
                    }
                }
            }
            static constexpr auto data_members = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^T,
                    std::meta::access_context::unchecked()));
            std::vector<std::string> seen_fields;
            template for (constexpr auto m : data_members) {
                if constexpr (!std::meta::is_bit_field(m)
                              && std::meta::has_identifier(m)) {
                    constexpr auto nm = std::meta::identifier_of(m);
                    auto nm_str = std::string(nm);
                    bool dup = false;
                    for (const auto& s : seen_fields)
                        if (s == nm_str) { dup = true; break; }
                    if (!dup) {
                        seen_fields.push_back(nm_str);
                        constexpr auto field_type =
                            detail::make_property_field_type(^^T, nm);
                        specs.push_back(std::meta::data_member_spec(
                            field_type, {.name=nm_str}));
                    }
                }
            }
            specs.push_back(std::meta::data_member_spec(
                ^^std::optional<T>, {.name="obj"}));
            std::meta::define_aggregate(^^Dispatch, specs);
        } else {
            std::meta::define_aggregate(^^Dispatch, {});
        }
    }

    Dispatch dispatch_;

    void populate() {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)
                              && std::meta::identifier_of(field) != "obj") {
                    using FieldType = [:std::meta::type_of(field):];
                    if constexpr (detail::is_typed_method_v<
                            std::remove_cv_t<FieldType>>) {
                        dispatch_.[:field:].obj = &*dispatch_.obj;
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        constexpr auto entries = []() consteval {
                            using OE = typename
                                std::remove_cv_t<FieldType>::OverloadEntry;
                            std::vector<OE> v;
                            static constexpr auto tmembers =
                                std::define_static_array(
                                    std::meta::members_of(^^T,
                                        std::meta::access_context::unchecked()));
                            template for (constexpr auto m : tmembers) {
                                if constexpr (std::meta::is_function(m)
                                    && std::meta::has_identifier(m)
                                    && !std::meta::is_static_member(m)
                                    && std::meta::identifier_of(m) == nm_sv) {
                                    v.push_back(OE{&detail::invoker<T, m>});
                                }
                            }
                            return std::define_static_array(v);
                        }();
                        dispatch_.[:field:].overloads = entries.data();
                        dispatch_.[:field:].num = entries.size();
                    } else {
                        dispatch_.[:field:].obj = &*dispatch_.obj;
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        static constexpr auto tdm = std::define_static_array(
                            std::meta::nonstatic_data_members_of(^^T,
                                std::meta::access_context::unchecked()));
                        template for (constexpr auto m : tdm) {
                            if constexpr (!std::meta::is_bit_field(m)
                                          && std::meta::has_identifier(m)
                                          && std::meta::identifier_of(m)
                                              == nm_sv) {
                                using MemberType = [:std::meta::type_of(m):];
                                if constexpr (!std::is_const_v<MemberType>) {
                                    dispatch_.[:field:].setter =
                                        &detail::setter<T, m>;
                                }
                                dispatch_.[:field:].getter =
                                    &detail::getter<T, m>;
                            }
                        }
                    }
                }
            }
        }
    }

    void* find_field(std::string_view name) {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)
                              && std::meta::identifier_of(field) != "obj") {
                    constexpr auto nm_sv = std::meta::identifier_of(field);
                    constexpr auto nm = std::define_static_string(nm_sv);
                    if (name == std::string_view(nm))
                        return &dispatch_.[:field:];
                }
            }
        }
        return nullptr;
    }

    std::map<std::string, std::function<void(std::any&)>> invoke_hooks_;
    std::map<std::string, std::function<void(std::any&)>> change_hooks_;

public:
    using value_type = T;

    Refl() { ensure_registered<T>(); }

    template <typename... Args>
    explicit Refl(Args&&... args) {
        ensure_registered<T>();
        if constexpr (std::is_class_v<T>) {
            dispatch_.obj.emplace(std::forward<Args>(args)...);
            populate();
        }
    }

    Refl(const Refl&) = delete;
    Refl& operator=(const Refl&) = delete;
    Refl(Refl&&) = delete;
    Refl& operator=(Refl&&) = delete;

    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }

    T& get() requires std::is_class_v<T> { return *dispatch_.obj; }
    const T& get() const requires std::is_class_v<T> { return *dispatch_.obj; }

    void connect(std::string_view name,
                  std::function<void(std::any&)> cb) {
        auto key = std::string(name);
        invoke_hooks_[key] = std::move(cb);
        void* field = find_field(name);
        if (!field) return;
        static constexpr auto dm = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch,
                std::meta::access_context::unchecked()));
        template for (constexpr auto f : dm) {
            if constexpr (std::meta::has_identifier(f)
                          && std::meta::identifier_of(f) != "obj") {
                using FT = [:std::meta::type_of(f):];
                if constexpr (detail::is_typed_method_v<std::remove_cv_t<FT>>) {
                    constexpr auto nm_sv = std::meta::identifier_of(f);
                    constexpr auto nm = std::define_static_string(nm_sv);
                    if (name == std::string_view(nm)) {
                        auto* tm = static_cast<std::remove_cv_t<FT>*>(field);
                        tm->hook_ctx = &invoke_hooks_[key];
                        tm->after_call = +[](void* ctx, std::any& r) {
                            (*static_cast<std::function<void(std::any&)>*>(ctx))(r);
                        };
                    }
                }
            }
        }
    }

    void on_change(std::string_view name,
                    std::function<void(std::any&)> cb) {
        auto key = std::string(name);
        change_hooks_[key] = std::move(cb);
        void* field = find_field(name);
        if (!field) return;
        static constexpr auto dm = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch,
                std::meta::access_context::unchecked()));
        template for (constexpr auto f : dm) {
            if constexpr (std::meta::has_identifier(f)
                          && std::meta::identifier_of(f) != "obj") {
                using FT = [:std::meta::type_of(f):];
                if constexpr (!detail::is_typed_method_v<std::remove_cv_t<FT>>) {
                    constexpr auto nm_sv = std::meta::identifier_of(f);
                    constexpr auto nm = std::define_static_string(nm_sv);
                    if (name == std::string_view(nm)) {
                        auto* tp = static_cast<std::remove_cv_t<FT>*>(field);
                        tp->hook_ctx = &change_hooks_[key];
                        tp->after_set = +[](void* ctx, std::any& v) {
                            (*static_cast<std::function<void(std::any&)>*>(ctx))(v);
                        };
                    }
                }
            }
        }
    }

    static const Registrar& registrar() { return RegistrarHolder<T>::registrar; }
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
        if (!is_class(detail::type_name<T>()))
            return std::unexpected(Error::TypeError);
        return std::static_pointer_cast<T>(ptr_);
    }

    // Deep-copy the object through the type-erased handle.  Returns a
    // new Object with independent ownership.  Returns Error::BadSignature
    // if the class is not copy-constructible, Error::NullHandle if invalid.
    std::expected<Object, Error> clone() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(class_name_);
        if (it == class_pool().end() || !it->second.clone)
            return std::unexpected(Error::BadSignature);
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

        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();

        const auto& ci = owner_->constructors[idx_];
        std::shared_ptr<void> result = ci.factory(args_ptr);
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
    // Returns Error::TypeError if obj is not the function's class (or a
    // derived class), Error::NullHandle if the handle is invalid.
    template <typename... Args>
    std::expected<std::any, Error> invoke(Object& obj, Args&&... args) {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.is_class(owner_->name))
            return std::unexpected(Error::TypeError);
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        return owner_->functions[idx_].invoker(obj.raw(), args_ptr);
    }

    // Invoke on a concrete type — for when you have the real object already.
    // Returns Error::TypeError if T is not the function's class or a class
    // derived from it, Error::NullHandle if the handle is invalid.
    template <typename T, typename... Args>
    std::expected<std::any, Error> invoke(T& obj, Args&&... args) {
        if (!valid()) return std::unexpected(Error::NullHandle);
        constexpr auto tname = detail::type_name<T>();
        if (owner_->name != tname && !is_base_of(tname, owner_->name))
            return std::unexpected(Error::TypeError);
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        return owner_->functions[idx_].invoker(static_cast<void*>(&obj), args_ptr);
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
        if (!obj.is_class(owner_->name))
            return std::unexpected(Error::TypeError);
        return owner_->fields[idx_].getter(obj.raw());
    }

    std::expected<std::any, Error> get(const Object& obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.is_class(owner_->name))
            return std::unexpected(Error::TypeError);
        return owner_->fields[idx_].getter(const_cast<Object&>(obj).raw());
    }

    // Set the field value on an Object.  Returns Error::BadSignature if
    // the field is read-only (const or bit-field), Error::TypeError if obj
    // is not the field's class (or a derived class), Error::NullHandle if
    // the Field handle is invalid.
    std::expected<void, Error> set(Object& obj, std::any val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.is_class(owner_->name))
            return std::unexpected(Error::TypeError);
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
        auto arg_anys = detail::make_arg_anys(std::forward<Args>(args)...);
        const std::any* args_ptr = arg_anys.empty() ? nullptr : arg_anys.data();
        return owner_->static_functions[idx_].invoker(args_ptr);
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
    const std::vector<std::string>& base_names,
    Finder&& finder) {
    for (const auto& bn : base_names) {
        auto base = find_class(bn);
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
    // ponytail: single inheritance only — multiple inheritance with offset
    // bases would produce wrong pointer adjustments in the invoker.
    return walk_bases<Function>(info_->base_names,
        [name](const Class& b) { return b.find_function(name); });
}

inline std::expected<Field, Error> Class::find_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->fields.size(); ++i) {
        if (info_->fields[i].name == name)
            return Field(info_, i);
    }
    return walk_bases<Field>(info_->base_names,
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
    return walk_bases<Function>(info_->base_names,
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
    return walk_bases<StaticField>(info_->base_names,
        [name](const Class& b) { return b.find_static_field(name); });
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->static_functions.size(); ++i) {
        if (info_->static_functions[i].name == name)
            return StaticFunction(info_, i);
    }
    return walk_bases<StaticFunction>(info_->base_names,
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
    return walk_bases<StaticFunction>(info_->base_names,
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
