// refl — C++26 compile-time-reflection-based runtime reflection framework.
//
// Wrap a class as Refl<MyClass> so it registers in a global pool queryable at
// runtime via find_class("MyClass").  From a Class handle you can find
// constructors and member functions by parameter-type name, then construct
// objects or invoke functions through the returned handles.
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
#include <tuple>
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
// Type-erased function pointer signatures for factories, deleters, and
// invokers.
// ---------------------------------------------------------------------------
using FactoryFn  = void* (*)(void* args[]);
using DeleterFn  = void  (*)(void* obj);
using InvokerFn  = std::any (*)(void* obj, void* args[]);

struct ConstructorInfo {
    std::vector<std::string> param_types;
    FactoryFn factory;
    DeleterFn deleter;
};

struct FunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    InvokerFn invoker;
};

struct ClassInfo {
    std::string name;
    std::vector<std::string> data_member_names;
    std::vector<std::string> data_member_types;
    std::vector<ConstructorInfo> constructors;
    std::vector<FunctionInfo> functions;
};

// ---------------------------------------------------------------------------
// Global class pool — Meyers singletons to avoid SIOF.
// ---------------------------------------------------------------------------
inline std::unordered_map<std::string, ClassInfo>& class_pool() {
    static std::unordered_map<std::string, ClassInfo> pool;
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

// ---------------------------------------------------------------------------
// Type-erased factory / deleter / invoker templates.
//
// Each is parameterised on the target type T and the compile-time meta::info
// of the specific constructor or member function.  template-for in make_info
// instantiates one of these per reflected member, and stores its address as
// a FactoryFn / DeleterFn / InvokerFn in ClassInfo.
// ---------------------------------------------------------------------------

namespace detail {

template <typename T, std::meta::info Ctor>
void* factory(void* args[]) {
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Ctor));
    constexpr std::size_t n = params.size();

    if constexpr (n == 0) {
        return new T();
    } else if constexpr (n == 1) {
        using P0 = [:std::meta::type_of(params[0]):];
        return new T(*static_cast<std::remove_reference_t<P0>*>(args[0]));
    } else if constexpr (n == 2) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        return new T(*static_cast<std::remove_reference_t<P0>*>(args[0]),
                     *static_cast<std::remove_reference_t<P1>*>(args[1]));
    } else if constexpr (n == 3) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        return new T(*static_cast<std::remove_reference_t<P0>*>(args[0]),
                     *static_cast<std::remove_reference_t<P1>*>(args[1]),
                     *static_cast<std::remove_reference_t<P2>*>(args[2]));
    } else if constexpr (n == 4) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        using P3 = [:std::meta::type_of(params[3]):];
        return new T(*static_cast<std::remove_reference_t<P0>*>(args[0]),
                     *static_cast<std::remove_reference_t<P1>*>(args[1]),
                     *static_cast<std::remove_reference_t<P2>*>(args[2]),
                     *static_cast<std::remove_reference_t<P3>*>(args[3]));
    }
    // ponytail: supports constructors with 0-4 parameters. Extend if needed.
    return nullptr;
}

template <typename T>
void deleter(void* obj) {
    delete static_cast<T*>(obj);
}

template <typename T, std::meta::info Fn>
std::any invoker(void* obj, void* args[]) {
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
            (target->*mfn)(*static_cast<std::remove_reference_t<P0>*>(args[0]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                *static_cast<std::remove_reference_t<P0>*>(args[0])));
        }
    } else if constexpr (n == 2) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(*static_cast<std::remove_reference_t<P0>*>(args[0]),
                           *static_cast<std::remove_reference_t<P1>*>(args[1]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                *static_cast<std::remove_reference_t<P0>*>(args[0]),
                *static_cast<std::remove_reference_t<P1>*>(args[1])));
        }
    } else if constexpr (n == 3) {
        using P0 = [:std::meta::type_of(params[0]):];
        using P1 = [:std::meta::type_of(params[1]):];
        using P2 = [:std::meta::type_of(params[2]):];
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(*static_cast<std::remove_reference_t<P0>*>(args[0]),
                           *static_cast<std::remove_reference_t<P1>*>(args[1]),
                           *static_cast<std::remove_reference_t<P2>*>(args[2]));
            return std::any{};
        } else {
            return std::any((target->*mfn)(
                *static_cast<std::remove_reference_t<P0>*>(args[0]),
                *static_cast<std::remove_reference_t<P1>*>(args[1]),
                *static_cast<std::remove_reference_t<P2>*>(args[2])));
        }
    }
    // ponytail: supports member functions with 0-3 parameters. Extend if needed.
    return std::any{};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Forward declarations.
// ---------------------------------------------------------------------------
class Class;
class Constructor;
class Function;
class Object;

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

template <typename T>
inline void ensure_registered() {
    (void)RegistrarHolder<T>::registrar;
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
// Object — a type-erased owning handle to a heap-allocated instance.
//
// Returned by Constructor::call.  Holds a void* plus the deleter and class
// name needed to manage it safely without knowing the C++ type.  Use
// cast<T>() to recover the concrete type when you need direct access.
// ---------------------------------------------------------------------------

class Object {
public:
    Object() = default;

    Object(void* ptr, DeleterFn deleter, std::string class_name)
        : ptr_(ptr), deleter_(deleter), class_name_(std::move(class_name)) {}

    ~Object() {
        if (ptr_ && deleter_) deleter_(ptr_);
    }

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    Object(Object&& o) noexcept
        : ptr_(o.ptr_), deleter_(o.deleter_), class_name_(std::move(o.class_name_)) {
        o.ptr_ = nullptr;
        o.deleter_ = nullptr;
    }

    Object& operator=(Object&& o) noexcept {
        if (this != &o) {
            if (ptr_ && deleter_) deleter_(ptr_);
            ptr_ = o.ptr_;
            deleter_ = o.deleter_;
            class_name_ = std::move(o.class_name_);
            o.ptr_ = nullptr;
            o.deleter_ = nullptr;
        }
        return *this;
    }

    // The class name this object was constructed as (for runtime checks).
    const std::string& class_name() const { return class_name_; }

    // Recover the concrete type.  Caller is responsible for passing the
    // correct T; a mismatch is undefined behaviour (as with any cast).
    template <typename T>
    T& cast() {
        return *static_cast<T*>(ptr_);
    }

    template <typename T>
    const T& cast() const {
        return *static_cast<const T*>(ptr_);
    }

    // Low-level: the raw pointer, for passing to invokers.
    void* raw() { return ptr_; }
    const void* raw() const { return ptr_; }

    bool valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    void* ptr_ = nullptr;
    DeleterFn deleter_ = nullptr;
    std::string class_name_;

    friend class Function;
};

// ---------------------------------------------------------------------------
// make_info — gather all reflection metadata for T at compile time.
// ---------------------------------------------------------------------------

template <typename T>
ClassInfo RegistrarHolder<T>::make_info() {
    ClassInfo info;
    info.name = std::string(std::meta::identifier_of(^^T));

    // Data members.
    static constexpr auto data_members = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : data_members) {
        info.data_member_names.emplace_back(std::meta::identifier_of(m));
        info.data_member_types.emplace_back(
            std::meta::display_string_of(std::meta::type_of(m)));
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
            if constexpr (n == 1) {
                using P0 = [:std::meta::type_of(params[0]):];
                if constexpr (!std::is_same_v<std::remove_cvref_t<P0>, T>) {
                    ConstructorInfo ci;
                    ci.factory = &detail::factory<T, m>;
                    ci.deleter = &detail::deleter<T>;
                    template for (constexpr auto p : params) {
                        ci.param_types.emplace_back(
                            std::meta::display_string_of(std::meta::type_of(p)));
                    }
                    info.constructors.push_back(std::move(ci));
                }
            } else if constexpr (n >= 2 && n <= 4) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                ci.deleter = &detail::deleter<T>;
                template for (constexpr auto p : params) {
                    ci.param_types.emplace_back(
                        std::meta::display_string_of(std::meta::type_of(p)));
                }
                info.constructors.push_back(std::move(ci));
            }
            // ponytail: n==0 (default ctor) and n>4 are skipped.
        } else if constexpr (std::meta::is_function(m) && std::meta::has_identifier(m)) {
            static constexpr auto fparams = std::define_static_array(
                std::meta::parameters_of(m));
            constexpr std::size_t fn = fparams.size();

            if constexpr (fn <= 3) {
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
            // ponytail: functions with >3 params are skipped.
        }
    }

    return info;
}

// ---------------------------------------------------------------------------
// Runtime handles — Class, Constructor, Function.
// ---------------------------------------------------------------------------

class Constructor {
public:
    Constructor() = default;
    Constructor(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::vector<std::string>& param_types() const {
        return owner_->constructors[idx_].param_types;
    }

    // Construct an Object from the given arguments.  The Object owns the
    // heap-allocated instance and will delete it via the type-erased deleter.
    template <typename... Args>
    std::expected<Object, Error> call(Args&&... args) {
        if (!valid()) return std::unexpected(Error::NullHandle);

        auto arg_tuple = std::forward_as_tuple(args...);
        std::array<void*, sizeof...(Args)> arg_ptrs{};
        if constexpr (sizeof...(Args) > 0) {
            fill_arg_ptrs(arg_ptrs.data(), arg_tuple, std::make_index_sequence<sizeof...(Args)>{});
        }

        const auto& ci = owner_->constructors[idx_];
        void* result = ci.factory(arg_ptrs.data());
        if (!result) return std::unexpected(Error::NullHandle);
        return Object(result, ci.deleter, owner_->name);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;

    template <typename Tuple, std::size_t... I>
    static void fill_arg_ptrs(void* ptrs[], Tuple& t, std::index_sequence<I...>) {
        ((ptrs[I] = static_cast<void*>(std::addressof(std::get<I>(t)))), ...);
    }
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
        auto arg_tuple = std::forward_as_tuple(args...);
        std::array<void*, sizeof...(Args)> arg_ptrs{};
        if constexpr (sizeof...(Args) > 0) {
            fill_arg_ptrs(arg_ptrs.data(), arg_tuple, std::make_index_sequence<sizeof...(Args)>{});
        }
        return owner_->functions[idx_].invoker(obj.raw(), arg_ptrs.data());
    }

    // Invoke on a raw pointer — for when you have the concrete type already.
    template <typename T, typename... Args>
    std::any invoke(T& obj, Args&&... args) {
        auto arg_tuple = std::forward_as_tuple(args...);
        std::array<void*, sizeof...(Args)> arg_ptrs{};
        if constexpr (sizeof...(Args) > 0) {
            fill_arg_ptrs(arg_ptrs.data(), arg_tuple, std::make_index_sequence<sizeof...(Args)>{});
        }
        return owner_->functions[idx_].invoker(static_cast<void*>(&obj), arg_ptrs.data());
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;

    template <typename Tuple, std::size_t... I>
    static void fill_arg_ptrs(void* ptrs[], Tuple& t, std::index_sequence<I...>) {
        ((ptrs[I] = static_cast<void*>(std::addressof(std::get<I>(t)))), ...);
    }
};

class Class {
public:
    Class() = default;
    explicit Class(const ClassInfo* info) : info_(info) {}

    const std::string& name() const { return info_->name; }

    const std::vector<std::string>& data_members() const {
        return info_->data_member_names;
    }
    const std::vector<std::string>& data_member_types() const {
        return info_->data_member_types;
    }

    std::expected<Constructor, Error> find_constructor(
        std::initializer_list<std::string_view> types) const;

    std::expected<Function, Error> find_function(std::string_view name) const;

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
    return std::unexpected(Error::NotFound);
}

}  // namespace refl
