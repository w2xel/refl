// proxy — typed proxy field types for the dyn dispatch layer.
//
// These are the compile-time proxy field types used by Dyn<T>'s synthesized
// Dispatch struct.  Each proxies a single reflected member (method, data
// member, static member) with real C++ types at the call site — no std::any,
// no std::variant.
//
//   TypedMethod<int(), void(int)>     — overload-resolved method dispatch
//   TypedProperty<int>               — data member get/set via operator=/cast
//   TypedStaticProperty<int>         — static data member proxy
//   TypedStaticMethod<int()>         — static member function proxy
//
// Pure primitives: no dependency on Dyn<T>.  Dyn<T> (in refl/dyn.hpp) includes
// this header and builds its Dispatch struct from these field types.
#pragma once

#include <refl/refl.hpp>

namespace refl {
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
// The invoker returns Object internally; the cast to the real return
// type happens inside operator() — the caller never sees Object.
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

// Shared overload-entry type used by all TypedMethod<Sigs...> specializations.
// Defined here (in detail) rather than nested in TypedMethod so Dyn<T> can
// store std::vector<OverloadEntry> without knowing the specific Sigs pack.
struct OverloadEntry {
    InvokerFn invoker;
};

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
// Applies the same filters as make_info: public, non-deleted, non-consteval.
consteval std::vector<std::meta::info>
collect_sigs(std::meta::info type, std::string_view name) {
    std::vector<std::meta::info> result;
    for (auto m : std::meta::members_of(type,
            std::meta::access_context::unchecked())) {
        if (is_public_method(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m)
            && std::meta::identifier_of(m) == name)
            result.push_back(make_fn_sig(m));
    }
    return result;
}

}  // namespace detail

template <typename... Sigs>
struct TypedMethod {
    const detail::OverloadEntry* overloads = nullptr;
    std::size_t num = 0;
    void* obj = nullptr;
    std::shared_ptr<void> owner;  // shared_ptr for aliasing invoker calls

    void (*after_call)(void* ctx, Object& result) = nullptr;
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
            // Build Object arg array from forwarded args.
            std::tuple<std::decay_t<Args>...> storage(
                std::forward<Args>(args)...);
            std::array<Object, sizeof...(Args)> refs;
            if constexpr (sizeof...(Args) > 0) {
                [&]<std::size_t... J>(std::index_sequence<J...>) {
                    ((refs[J] = Object(std::get<J>(storage))), ...);
                }(std::make_index_sequence<sizeof...(Args)>{});
            }
            const Object* args_ptr =
                sizeof...(Args) == 0 ? nullptr : refs.data();
            Object result = overloads[I].invoker(
                owner, obj, args_ptr);
            if (after_call) after_call(hook_ctx, result);
            if constexpr (std::is_void_v<R>) return;
            else {
                // Extract the return value from the Object.
                if constexpr (std::is_reference_v<R>) {
                    return *result.template cast_ref<std::remove_cvref_t<R>>().value();
                } else {
                    return std::move(*result.template cast_ref<R>().value());
                }
            }
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
// Subscriptable members (std::array, std::vector) also offer operator[].
// ---------------------------------------------------------------------------
template <typename T, bool Readonly = false>
struct TypedProperty {
    GetterFn getter = nullptr;
    SetterFn setter = nullptr;  // always nullptr when Readonly=true
    void* obj = nullptr;
    std::shared_ptr<void> owner;  // shared_ptr for getter/setter calls
    std::size_t member_offset = 0;  // byte offset of the member within T

    void (*after_set)(void* ctx, Object& val) = nullptr;
    void* hook_ctx = nullptr;

    // Implicit conversion to T (read).
    operator T() const {
        if (!getter || !obj) throw std::bad_cast{};
        Object result = getter(obj);
        return std::move(*result.template cast_ref<T>().value());
    }

    // Assignment from T (write).  Compile error when Readonly=true.
    void operator=(T val) requires (!Readonly) {
        if (!setter || !obj) return;
        std::decay_t<T> storage(std::move(val));
        Object val_ref(&storage, detail::type_name<std::decay_t<T>>());
        setter(obj, &val_ref);
        if (after_set) {
            Object current = getter(obj);
            after_set(hook_ctx, current);
        }
    }

    // operator[] — returns a reference to the element in the actual object.
    // Only available when T is subscriptable (std::array, std::vector, etc.).
    template <typename Self>
    auto& operator[](this Self&& self, std::size_t i)
        requires requires { typename std::remove_cvref_t<T>::value_type; }
    {
        return reinterpret_cast<std::remove_cv_t<T>*>(
            static_cast<char*>(self.obj) + self.member_offset)->operator[](i);
    }

    static constexpr bool is_readonly() { return Readonly; }
};

// ---------------------------------------------------------------------------
// TypedStaticProperty<T, Readonly> — static data member proxy.  No obj
// pointer needed; accesses static storage via function pointers.
// ---------------------------------------------------------------------------
template <typename T, bool Readonly = false>
struct TypedStaticProperty {
    StaticGetterFn getter = nullptr;
    StaticSetterFn setter = nullptr;

    operator T() const {
        if (!getter) throw std::bad_cast{};
        Object result = getter();
        return std::move(*result.template cast_ref<T>().value());
    }

    void operator=(T val) requires (!Readonly) {
        if (!setter) return;
        std::decay_t<T> storage(std::move(val));
        Object val_ref(&storage, detail::type_name<std::decay_t<T>>());
        setter(&val_ref);
    }

    static constexpr bool is_readonly() { return Readonly; }
};

// ---------------------------------------------------------------------------
// TypedStaticMethod<R> — static member function proxy.  No obj pointer.
// ---------------------------------------------------------------------------
template <typename R>
struct TypedStaticMethod {
    using is_static_method = void;
    StaticInvokerFn invoker = nullptr;

    R operator()() {
        Object result = invoker(nullptr);
        if constexpr (std::is_void_v<R>) return;
        else return std::move(*result.template cast_ref<R>().value());
    }
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

// Check if a dispatch field type is a TypedStaticMethod.
template <typename T> struct is_typed_static_method : std::false_type {};
template <typename R>
struct is_typed_static_method<TypedStaticMethod<R>> : std::true_type {};
template <typename T> inline constexpr bool is_typed_static_method_v =
    is_typed_static_method<T>::value;

// Check if a dispatch field type is a TypedStaticProperty.
template <typename T> struct is_typed_static_property : std::false_type {};
template <typename T2, bool R>
struct is_typed_static_property<TypedStaticProperty<T2, R>> : std::true_type {};
template <typename T> inline constexpr bool is_typed_static_property_v =
    is_typed_static_property<T>::value;

// consteval: build the TypedMethod<Sigs...> type for a function name.
consteval std::meta::info make_typed_method_type(std::meta::info type,
                                                    std::string_view name) {
    return std::meta::substitute(^^TypedMethod, collect_sigs(type, name));
}

// consteval: build the TypedStaticMethod<R> type for a static function.
consteval std::meta::info make_static_method_type(std::meta::info m) {
    auto rt = std::meta::return_type_of(m);
    return std::meta::substitute(^^TypedStaticMethod,
        std::initializer_list<std::meta::info>{rt});
}

// consteval: build the TypedStaticProperty<T, Readonly> type for a static member.
consteval std::meta::info make_static_property_type(std::meta::info m) {
    auto mt = std::meta::type_of(m);
    bool is_const = std::meta::is_const_type(mt);
    auto clean_mt = std::meta::substitute(^^std::remove_cv_t,
        std::initializer_list<std::meta::info>{mt});
    return std::meta::substitute(^^TypedStaticProperty,
        {clean_mt, std::meta::reflect_constant(is_const)});
}

// consteval: build the TypedProperty field type for a data member name.
// Passes Readonly=true for const members (deletes operator= at compile time).
consteval std::meta::info make_property_field_type(std::meta::info type,
                                                       std::string_view name) {
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked())) {
        if (!std::meta::is_bit_field(m) && std::meta::has_identifier(m)
            && std::meta::is_public(m)
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

// Structural fixed-size string for use as a non-type template parameter.
// Enables implement<"method_name">(...) without exposing ^^ syntax.
template <std::size_t N>
struct FixedString {
    char data[N] = {};
    static constexpr std::size_t size = N;
    constexpr FixedString(const char (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = str[i];
    }
    constexpr std::string_view sv() const {
        return std::string_view(data, N - 1);
    }
};

}  // namespace refl
