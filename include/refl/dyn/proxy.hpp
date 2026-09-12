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

#include <map>

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

// ---------------------------------------------------------------------------
// Proxy<T> — typed dispatch struct with type-erased object binding.
//
// Synthesizes a Dispatch struct from T's public interface at compile time
// (same field types as Dyn<T>: TypedMethod, TypedProperty, etc.).  At
// runtime, bind(Object) wires the dispatch fields from the Object's
// ClassInfo — so calls through the proxy dispatch to the object's methods
// with T's typed return types.
//
// The object behind the proxy can be any type whose interface is
// structurally compatible with T (same method names and signatures),
// not just T itself.  Matching is by method name (overloads in
// declaration order, same as Dyn).  This is structural typing through
// the reflection pool: any type with compatible public methods works.
//
// T need not be abstract or have pure-virtual methods — it is a
// compile-time interface descriptor only.  Its methods are never called;
// they exist solely for signature extraction via reflection.
//
//   struct IDrawable { int render(int scale); void set_tint(int t); };
//   struct Square { int side; Square(int s) : side(s) {}
//                  int render(int scale) const { ... } };
//
//   refl::ensure_registered<Square>();
//   auto cls = *refl::find_class("Square");
//   auto obj = *cls.constructors()[0].call(4);  // type-erased Object
//   refl::Proxy<IDrawable> p(obj);
//   int a = p->render(2);   // calls Square::render, returns int
//
// Proxy<T> is non-copyable, non-movable (dispatch fields point into it).
// ---------------------------------------------------------------------------
template <typename T>
class Proxy {
    struct Dispatch;
    consteval {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            std::vector<std::meta::info> specs;
            std::vector<std::string> seen_fns;
            template for (constexpr auto m : members) {
                if constexpr (detail::is_public_method(m)
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
                if constexpr (detail::is_public_data_member(m)
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
            // Static data members → TypedStaticProperty.
            static constexpr auto static_data = std::define_static_array(
                std::meta::static_data_members_of(^^T,
                    std::meta::access_context::unchecked()));
            std::vector<std::string> seen_static;
            template for (constexpr auto m : static_data) {
                if constexpr (std::meta::has_identifier(m)
                              && std::meta::is_public(m)) {
                    constexpr auto nm = std::meta::identifier_of(m);
                    auto nm_str = std::string(nm);
                    bool dup = false;
                    for (const auto& s : seen_static)
                        if (s == nm_str) { dup = true; break; }
                    if (!dup) {
                        seen_static.push_back(nm_str);
                        constexpr auto field_type =
                            detail::make_static_property_type(m);
                        specs.push_back(std::meta::data_member_spec(
                            field_type, {.name=nm_str}));
                    }
                }
            }
            // Static member functions → TypedStaticMethod.
            std::vector<std::string> seen_static_fns;
            template for (constexpr auto m : members) {
                if constexpr (detail::is_public_method(m)
                              && std::meta::has_identifier(m)
                              && std::meta::is_static_member(m)) {
                    constexpr auto nm = std::meta::identifier_of(m);
                    auto nm_str = std::string(nm);
                    bool dup = false;
                    for (const auto& s : seen_static_fns)
                        if (s == nm_str) { dup = true; break; }
                    if (!dup) {
                        seen_static_fns.push_back(nm_str);
                        constexpr auto field_type =
                            detail::make_static_method_type(m);
                        specs.push_back(std::meta::data_member_spec(
                            field_type, {.name=nm_str}));
                    }
                }
            }
            // No obj field — Proxy stores the object separately as
            // Object since the actual type differs from T.
            std::meta::define_aggregate(^^Dispatch, specs);
        } else {
            std::meta::define_aggregate(^^Dispatch, {});
        }
    }

    Dispatch dispatch_;
    Object obj_;
    // ponytail: overload matching is by name + declaration order, same as Dyn.
    // If the impl type reorders overloads relative to T, the wrong invoker may
    // be picked.  Match by param-type signature would fix this; deferred.
    std::map<std::string, std::vector<detail::OverloadEntry>> overload_storage_;

    static const ClassInfo* lookup_class_info(std::string_view name) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(std::string(name));
        return it != class_pool().end() ? it->second.get() : nullptr;
    }

    void populate() {
        if constexpr (std::is_class_v<T>) {
            if (!obj_.valid()) return;
            overload_storage_.clear();
            const ClassInfo* info = lookup_class_info(obj_.class_name());
            if (!info) return;
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)) {
                    using FieldType = [:std::meta::type_of(field):];
                    if constexpr (detail::is_typed_method_v<
                            std::remove_cv_t<FieldType>>) {
                        // Non-static method → bind from Object's ClassInfo.
                        dispatch_.[:field:].obj = obj_.raw();
                        dispatch_.[:field:].owner = obj_.owner();
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        auto& vec = overload_storage_[key];
                        for (const auto& fi : info->functions)
                            if (fi.name == key)
                                vec.push_back({fi.invoker});
                        dispatch_.[:field:].overloads = vec.data();
                        dispatch_.[:field:].num = vec.size();
                    } else if constexpr (detail::is_typed_static_method_v<
                            std::remove_cv_t<FieldType>>) {
                        // Static method → bind from Object's ClassInfo.
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        for (const auto& fi : info->static_functions)
                            if (fi.name == key) {
                                dispatch_.[:field:].invoker = fi.invoker;
                                break;
                            }
                    } else if constexpr (detail::is_typed_static_property_v<
                            std::remove_cv_t<FieldType>>) {
                        // Static property → bind from Object's ClassInfo.
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        for (const auto& fi : info->static_fields)
                            if (fi.name == key) {
                                dispatch_.[:field:].getter = fi.getter;
                                dispatch_.[:field:].setter = fi.setter;
                                break;
                            }
                    } else {
                        // Non-static data member → bind from Object's ClassInfo.
                        dispatch_.[:field:].obj = obj_.raw();
                        dispatch_.[:field:].owner = obj_.owner();
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        for (const auto& fi : info->fields)
                            if (fi.name == key) {
                                dispatch_.[:field:].member_offset =
                                    static_cast<std::size_t>(fi.offset);
                                dispatch_.[:field:].getter = fi.getter;
                                dispatch_.[:field:].setter = fi.setter;
                                break;
                            }
                    }
                }
            }
        }
    }

public:
    Proxy() = default;

    // Bind a type-erased Object — wires T's dispatch fields to the
    // Object's methods via its ClassInfo.  The Object's type must be
    // registered in the pool (e.g. via ensure_registered<T>() or
    // Constructor::call).  The Object need not be owning — for non-owning
    // Objects the caller must keep the source alive.
    explicit Proxy(Object obj) : obj_(std::move(obj)) { populate(); }
    void bind(Object obj) { obj_ = std::move(obj); populate(); }

    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }

    bool is_bound() const { return obj_.valid(); }

    Proxy(const Proxy&) = delete;
    Proxy(Proxy&&) = delete;
    Proxy& operator=(const Proxy&) = delete;
    Proxy& operator=(Proxy&&) = delete;
};

}  // namespace refl
