// dyn — typed dispatch and dynamic-implementation layer for the refl core.
//
// Built on top of refl/refl.hpp (which provides the type-erased reflection
// pool).  Dyn<T> wraps a shared instance of T and synthesizes a compile-time
// dispatch struct (via define_aggregate) with named callable fields for each
// member function and data member, so you get real return types at the call
// site — no std::any, no std::variant.
//
//   refl::Dyn<Point> p(1, 2);
//   p->set(10, 20);        // overload resolved by argument type
//   int s = p->sum();       // real return type
//   p->x = 42;              // member-like assignment
//
// Also supports runtime method implementation (mocking), Qt-style hooks
// (connect / on_change / emit), and dynamic properties.
//
// This layer is experimental and likely to change.
#pragma once

#include <refl/refl.hpp>

#include <any>
#include <functional>
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
        if (std::meta::is_function(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m)
            && std::meta::is_public(m)
            && !std::meta::is_deleted(m)
            && !is_consteval_fn(m)
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
// Dyn<T> — typed proxy with compile-time-synthesized dispatch struct.
//
//   Dyn<Point> p(1, 2);
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
// Dyn<T> is non-copyable, non-movable.
// ---------------------------------------------------------------------------
template <typename T>
class Dyn {
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
                              && !std::meta::is_static_member(m)
                              && std::meta::is_public(m)
                              && !std::meta::is_deleted(m)
                              && !detail::is_consteval_fn(m)) {
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
                              && std::meta::has_identifier(m)
                              && std::meta::is_public(m)) {
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
                if constexpr (std::meta::is_function(m)
                              && std::meta::has_identifier(m)
                              && std::meta::is_static_member(m)
                              && std::meta::is_public(m)
                              && !std::meta::is_deleted(m)
                              && !detail::is_consteval_fn(m)) {
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
            // Only add the obj field when T is constructible (not abstract).
            if constexpr (!std::meta::is_abstract_type(^^T)) {
                specs.push_back(std::meta::data_member_spec(
                    ^^std::shared_ptr<T>, {.name="obj"}));
            }
            std::meta::define_aggregate(^^Dispatch, specs);
        } else {
            std::meta::define_aggregate(^^Dispatch, {});
        }
    }

    Dispatch dispatch_;

    void populate() {
        if constexpr (std::is_class_v<T> && !std::meta::is_abstract_type(^^T)) {
            overload_storage_.clear();
            const ClassInfo* info = lookup_class_info();
            if (!info) return;
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)
                              && std::meta::identifier_of(field) != "obj") {
                    using FieldType = [:std::meta::type_of(field):];
                    if constexpr (detail::is_typed_method_v<
                            std::remove_cv_t<FieldType>>) {
                        // Non-static member function → bind from ClassInfo.
                        dispatch_.[:field:].obj = dispatch_.obj.get();
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
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
                        // Static member function → bind invoker from ClassInfo.
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        for (const auto& fi : info->static_functions)
                            if (fi.name == key) {
                                dispatch_.[:field:].invoker = fi.invoker;
                                break;
                            }
                    } else if constexpr (detail::is_typed_static_property_v<
                            std::remove_cv_t<FieldType>>) {
                        // Static data member → bind from ClassInfo.
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        for (const auto& fi : info->static_fields)
                            if (fi.name == key) {
                                dispatch_.[:field:].getter = fi.getter;
                                dispatch_.[:field:].setter = fi.setter;
                                break;
                            }
                    } else {
                        // Non-static data member → bind from ClassInfo.
                        dispatch_.[:field:].obj = dispatch_.obj.get();
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
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

    // Hook storage: multi-listener (vector of callbacks per name).
    // std::map nodes are stable — pointers to the vectors don't move.
    std::map<std::string, std::vector<std::function<void(Object&)>>> invoke_hooks_;
    std::map<std::string, std::vector<std::function<void(Object&)>>> change_hooks_;

    // Dynamic properties (runtime-added, not reflected from T).
    std::map<std::string, Object> dynamic_props_;

    // Shared callback-dispatcher for both after_call (method hooks) and
    // after_set (property-change hooks).  ctx points at the
    // std::vector<std::function<void(Object&)>> stored in invoke_hooks_ or
    // change_hooks_ (std::map nodes are stable).
    static void fire_hooks(void* ctx, Object& result) {
        auto* v = static_cast<
            std::vector<std::function<void(Object&)>>*>(ctx);
        for (auto& cb : *v) cb(result);
    }

    // --- Dynamic mode: runtime callable storage + trampolines.
    //     When T is abstract, no object is constructed; instead, implement()
    //     wires each method to a runtime-provided callable.
    //     When T is concrete, implement() can override individual methods
    //     while keeping the real object alive — other methods still call
    //     through to the real object.  This enables per-method mocking.
    std::map<std::string, std::any> dynamic_callables_;
    bool dynamic_mode_ = false;  // true = fully dynamic (no real object)

    // Stable storage for overload-entry vectors built from ClassInfo at
    // runtime.  Dyn is non-movable and std::map nodes are stable, so
    // .data() pointers remain valid for the Dyn's lifetime.
    std::map<std::string, std::vector<detail::OverloadEntry>> overload_storage_;

    // Look up T's ClassInfo from the global pool (T is registered via
    // ensure_registered<T>() before populate() is called).
    static const ClassInfo* lookup_class_info() {
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(std::string(detail::type_name<T>()));
        return it != class_pool().end() ? it->second.get() : nullptr;
    }

    // Trampoline: calls a stored std::function matching the method's signature.
    // The void* ctx points at the Dyn<T> itself (this).  The trampoline
    // looks up the callable by method name and passes *self as the first
    // argument — so the lambda receives Dyn<T>& as its "this".
    // Supports 0-2 args (extendable).  The lambda signature is
    // R(Dyn<T>&, Args...) — the first arg is always the self-reference.
    // dynamic_callables_ still stores the lambdas in std::any (it's
    // internal storage, not the call boundary).
    template <std::meta::info Method>
    static Object trampoline(const std::shared_ptr<void>&,
                             void* ctx, const Object* args) {
        auto* self = static_cast<Dyn<T>*>(ctx);
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        if constexpr (params.size() == 0) {
            auto& fn = std::any_cast<std::function<R(Dyn<T>&)>&>(
                self->dynamic_callables_[key]);
            if constexpr (std::is_void_v<R>) { fn(*self); return Object{}; }
            else return Object(std::make_shared<R>(fn(*self)),
                             detail::type_name<R>());
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            auto& fn = std::any_cast<std::function<R(Dyn<T>&, C0)>&>(
                self->dynamic_callables_[key]);
            auto& a0 = *static_cast<C0*>(args[0].raw());
            if constexpr (std::is_void_v<R>) {
                fn(*self, a0); return Object{};
            } else {
                return Object(std::make_shared<R>(fn(*self, a0)),
                             detail::type_name<R>());
            }
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            auto& fn = std::any_cast<std::function<R(Dyn<T>&, C0, C1)>&>(
                self->dynamic_callables_[key]);
            auto& a0 = *static_cast<C0*>(args[0].raw());
            auto& a1 = *static_cast<C1*>(args[1].raw());
            if constexpr (std::is_void_v<R>) {
                fn(*self, a0, a1); return Object{};
            } else {
                return Object(std::make_shared<R>(fn(*self, a0, a1)),
                             detail::type_name<R>());
            }
        } else {
            // ponytail: 3+ args not yet supported in the trampoline.
            return Object{};
        }
    }

public:
    using value_type = T;

    Dyn() { if constexpr (!std::meta::is_abstract_type(^^T)) ensure_registered<T>(); }

    template <typename... Args>
    explicit Dyn(Args&&... args) {
        ensure_registered<T>();
        if constexpr (std::is_class_v<T>) {
            dispatch_.obj = std::make_shared<T>(std::forward<Args>(args)...);
            populate();
        }
    }

    // Replace the underlying object (swap to real-object mode).
    // Re-populates all fields with real invokers.  Clears any
    // dynamic-mode callables and per-method overrides.
    template <typename... Args>
    void reset(Args&&... args) requires (!std::meta::is_abstract_type(^^T)) {
        if constexpr (std::is_class_v<T>) {
            dispatch_.obj = std::make_shared<T>(std::forward<Args>(args)...);
            dynamic_mode_ = false;
            dynamic_callables_.clear();
            populate();
        }
    }

    // Remove a per-method override, restoring the real invoker.
    // Only works when a real object is present (not in full dynamic mode).
    template <std::meta::info Method>
    void restore() requires (!std::meta::is_abstract_type(^^T)) {
        if (dynamic_mode_) return;  // can't restore in full dynamic mode
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        dynamic_callables_.erase(std::string(nm));
        // Re-populate just this method from ClassInfo.
        if constexpr (std::is_class_v<T>) {
            const ClassInfo* info = lookup_class_info();
            if (!info) return;
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)
                              && std::meta::identifier_of(field) == nm_sv) {
                    using FT = [:std::meta::type_of(field):];
                    if constexpr (detail::is_typed_method_v<
                            std::remove_cv_t<FT>>) {
                        dispatch_.[:field:].obj = dispatch_.obj.get();
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
                        auto key = std::string(nm);
                        auto& vec = overload_storage_[key];
                        vec.clear();
                        for (const auto& fi : info->functions)
                            if (fi.name == key)
                                vec.push_back({fi.invoker});
                        dispatch_.[:field:].overloads = vec.data();
                        dispatch_.[:field:].num = vec.size();
                    }
                }
            }
        }
    }

    // Switch to dynamic mode (no real object).  All methods must be
    // implemented via implement() before calling.  If currently in
    // real-object mode, the real object is released.
    void make_dynamic() {
        dynamic_mode_ = true;
        dynamic_callables_.clear();
        // Don't populate — implement() will wire each method.
    }

    // Implement a method with a runtime callable (dynamic mode).
    // T must be abstract or the method must not have a real invoker yet.
    // The callable's signature must match the method's.
    //
    //   refl::Dyn<IShape> s;
    //   s.implement<^^IShape::area>([](int scale) { return scale * 100; });
    //   int a = s->area(5);  // calls the lambda
    template <std::meta::info Method, typename F>
    void implement(F fn) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        // Store as std::function<R(Dyn<T>&, Args...)> — first arg is self.
        if constexpr (params.size() == 0) {
            dynamic_callables_[key] =
                std::function<R(Dyn<T>&)>(std::move(fn));
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            dynamic_callables_[key] =
                std::function<R(Dyn<T>&, C0)>(std::move(fn));
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            dynamic_callables_[key] =
                std::function<R(Dyn<T>&, C0, C1)>(std::move(fn));
        }
        // Wire the trampoline.  obj points at THIS Dyn<T> — the
        // trampoline casts it to Dyn<T>* and passes *self to the lambda.
        static constexpr auto dm = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch,
                std::meta::access_context::unchecked()));
        template for (constexpr auto field : dm) {
            if constexpr (std::meta::has_identifier(field)
                          && std::meta::identifier_of(field) == nm_sv) {
                using FT = [:std::meta::type_of(field):];
                if constexpr (detail::is_typed_method_v<std::remove_cv_t<FT>>) {
                    constexpr auto entries = []() consteval {
                        return std::define_static_array(
                            std::vector<detail::OverloadEntry>{
                                detail::OverloadEntry{&trampoline<Method>}});
                    }();
                    dispatch_.[:field:].overloads = entries.data();
                    dispatch_.[:field:].num = 1;
                    dispatch_.[:field:].obj = this;  // pass Dyn<T>* to trampoline
                }
            }
        }
    }

    // Implement a method by name (string literal) — no ^^ syntax needed.
    //   refl::Dyn<IShape> s;
    //   s.implement("area", [](int scale) { return scale * 100; });
    //   int a = s->area(5);
    //
    // Only non-overloaded methods can be implemented by name (the string
    // matches the first member with that identifier).  For overloaded
    // methods, use the ^^-based implement<^^T::method>().
    //
    // The name is carried as a structural NTTP (FixedString) so it's
    // usable in constexpr comparisons inside a template-for.
    template <FixedString Name, typename F>
    void implement(F fn) {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            constexpr bool found = []() consteval {
                for (auto m : std::meta::members_of(^^T,
                        std::meta::access_context::unchecked())) {
                    if (std::meta::is_function(m)
                        && std::meta::has_identifier(m)
                        && !std::meta::is_static_member(m)
                        && std::meta::is_public(m)
                        && !std::meta::is_deleted(m)
                        && !detail::is_consteval_fn(m)
                        && std::meta::identifier_of(m) == Name.sv())
                        return true;
                }
                return false;
            }();
            static_assert(found, "implement: method not found on T");
            template for (constexpr auto m : members) {
                if constexpr (std::meta::is_function(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)
                              && std::meta::is_public(m)
                              && !std::meta::is_deleted(m)
                              && !detail::is_consteval_fn(m)
                              && std::meta::identifier_of(m) == Name.sv()) {
                    implement<m>(std::move(fn));
                }
            }
        }
    }
    Dyn& operator=(const Dyn&) = delete;
    Dyn(Dyn&&) = delete;
    Dyn& operator=(Dyn&&) = delete;

    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }

    T& get() requires (!std::meta::is_abstract_type(^^T)) { return *dispatch_.obj; }
    const T& get() const requires (!std::meta::is_abstract_type(^^T)) { return *dispatch_.obj; }

    // Check if this Dyn is in dynamic (runtime-implemented) mode.
    bool is_dynamic() const { return dynamic_mode_; }

    // Connect a callback to a method (multi-listener).  Multiple connect()
    // calls on the same method name accumulate — all callbacks fire.
    //
    //   p.connect("sum", [](std::any& r) { ... });
    //   p.connect("sum", [](std::any& r) { ... });  // also fires
    void connect(std::string_view name,
                  std::function<void(Object&)> cb) {
        auto key = std::string(name);
        auto& vec = invoke_hooks_[key];
        vec.push_back(std::move(cb));
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
                        tm->hook_ctx = &vec;
                        tm->after_call = &fire_hooks;
                    }
                }
            }
        }
    }

    // on_change: connect a callback to a property change (multi-listener).
    void on_change(std::string_view name,
                    std::function<void(Object&)> cb) {
        auto key = std::string(name);
        auto& vec = change_hooks_[key];
        vec.push_back(std::move(cb));
        void* field = find_field(name);
        if (!field) return;
        static constexpr auto dm = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch,
                std::meta::access_context::unchecked()));
        template for (constexpr auto f : dm) {
            if constexpr (std::meta::has_identifier(f)
                          && std::meta::identifier_of(f) != "obj") {
                using FT = [:std::meta::type_of(f):];
                if constexpr (!detail::is_typed_method_v<std::remove_cv_t<FT>>
                              && !detail::is_typed_static_method_v<std::remove_cv_t<FT>>
                              && !detail::is_typed_static_property_v<std::remove_cv_t<FT>>) {
                    constexpr auto nm_sv = std::meta::identifier_of(f);
                    constexpr auto nm = std::define_static_string(nm_sv);
                    if (name == std::string_view(nm)) {
                        auto* tp = static_cast<std::remove_cv_t<FT>*>(field);
                        tp->hook_ctx = &vec;
                        tp->after_set = &fire_hooks;
                    }
                }
            }
        }
    }

    // Explicitly emit a signal — fire all connected callbacks for a name.
    // The std::any is passed to each callback.  Useful for custom signals
    // that don't map to a specific method.
    void emit(std::string_view name, Object value = {}) {
        auto key = std::string(name);
        auto it = invoke_hooks_.find(key);
        if (it != invoke_hooks_.end())
            for (auto& cb : it->second) cb(value);
    }

    // Set a dynamic property (runtime-added, not reflected from T).
    // Fires on_change hooks for that name if any are connected.
    void set_property(std::string_view name, Object val) {
        auto key = std::string(name);
        dynamic_props_[key] = std::move(val);
        auto it = change_hooks_.find(key);
        if (it != change_hooks_.end())
            for (auto& cb : it->second) cb(dynamic_props_[key]);
    }

    // Get a dynamic property (runtime-added).
    Object get_property(std::string_view name) const {
        auto key = std::string(name);
        auto it = dynamic_props_.find(key);
        if (it != dynamic_props_.end()) return it->second;
        return Object{};
    }

    // Connect a method on this object to a method on another Dyn<T>.
    // When this->name fires, it calls other->slot_name via emit().
    // Both Dyn<T> instances must be kept alive (raw pointers — Dyn is
    // non-movable so addresses are stable, like Qt's connect).
    void connect(std::string_view name, Dyn* other,
                  std::string_view slot_name) {
        auto slot = std::string(slot_name);
        auto* other_ptr = other;
        connect(name, [other_ptr, slot](Object& r) {
            other_ptr->emit(slot, r);
        });
    }

    static const Registrar& registrar() { return RegistrarHolder<T>::registrar; }
};
}  // namespace refl
