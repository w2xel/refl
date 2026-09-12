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
#include <stdexcept>

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

// Runtime: search a ClassInfo and its registered public bases for a
// field by name.  Returns the FieldInfo and accumulated byte offset to
// the base subobject (0 for direct fields), or {nullptr, 0} if not found.
// Used by Proxy and Dyn populate() to bind inherited data members.
// The pool is append-only (no unregister), so ClassInfo pointers remain
// valid after the lock is released.
struct FieldSearchResult {
    const FieldInfo* fi;
    std::ptrdiff_t offset;
};
inline FieldSearchResult find_field_in_hierarchy(
        const ClassInfo* info, std::string_view name) {
    for (const auto& fi : info->fields)
        if (fi.name == name) return {&fi, 0};
    for (const auto& b : info->bases) {
        const ClassInfo* base_info;
        {
            std::lock_guard<std::mutex> lk(pool_mutex());
            auto it = class_pool().find(b.name);
            if (it == class_pool().end()) continue;
            base_info = it->second.get();
        }
        auto r = find_field_in_hierarchy(base_info, name);
        if (r.fi) return {r.fi, b.offset + r.offset};
    }
    return {nullptr, 0};
}

// Runtime: search a ClassInfo and its registered public bases for a
// function by name + param-type signature.  Returns the FunctionInfo
// and accumulated byte offset, or {nullptr, 0} if not found.
struct FunctionSearchResult {
    const FunctionInfo* fi;
    std::ptrdiff_t offset;
};
inline FunctionSearchResult find_function_in_hierarchy(
        const ClassInfo* info, std::string_view name,
        const std::vector<std::string>& param_types) {
    for (const auto& fi : info->functions)
        if (fi.name == name && fi.param_types == param_types)
            return {&fi, 0};
    for (const auto& b : info->bases) {
        const ClassInfo* base_info;
        {
            std::lock_guard<std::mutex> lk(pool_mutex());
            auto it = class_pool().find(b.name);
            if (it == class_pool().end()) continue;
            base_info = it->second.get();
        }
        auto r = find_function_in_hierarchy(base_info, name, param_types);
        if (r.fi) return {r.fi, b.offset + r.offset};
    }
    return {nullptr, 0};
}

// consteval: build a function-type reflection R(Args...) from a member.
consteval std::meta::info make_fn_sig(std::meta::info m) {
    auto rt = std::meta::return_type_of(m);
    auto params = std::meta::parameters_of(m);
    std::vector<std::meta::info> args = {rt};
    for (auto p : params)
        args.push_back(std::meta::type_of(p));
    return std::meta::substitute(^^fn_type, args);
}

// consteval: collect all overload signatures for a function name,
// walking the base hierarchy for inherited methods.
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
    // Walk public bases for inherited methods.
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto inherited = collect_sigs(std::meta::type_of(b), name);
            for (auto s : inherited) result.push_back(s);
        }
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

    // Returns normalized param-type names per overload, in Sigs-pack
    // (interface declaration) order.  Used by populate() to match the
    // impl's FunctionInfo entries by signature, not just by name — so
    // overloads are correctly paired even when the impl type declares
    // them in a different order than the interface.
    static std::vector<std::vector<std::string>> expected_param_types() {
        return { sig_param_names<typename sig_traits<Sigs>::args_type>()... };
    }

    // Returns return-type display strings per overload, in Sigs-pack order.
    // Used by populate() to verify the impl's return type matches the
    // interface's, catching structural mismatches at bind time.
    static std::vector<std::string> expected_return_types() {
        return { std::string(detail::type_name<
            std::remove_cvref_t<typename sig_traits<Sigs>::return_type>>())... };
    }

private:
    template <typename Tuple>
    static std::vector<std::string> sig_param_names() {
        return sig_param_names_impl<Tuple>(
            std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    }

    template <typename Tuple, std::size_t... I>
    static std::vector<std::string> sig_param_names_impl(
            std::index_sequence<I...>) {
        return { std::string(detail::normalize_type(
            detail::type_name<std::tuple_element_t<I, Tuple>>()))... };
    }

public:

    template <typename... Args>
    auto operator()(Args&&... args) {
        return call_dispatch<0, Args...>(std::forward<Args>(args)...);
    }

    template <std::size_t I, typename... Args>
    auto call_dispatch(Args&&... args) {
        using Sig = std::tuple_element_t<I, std::tuple<Sigs...>>;
        if constexpr (matches_sig<Sig, Args...>) {
            using R = typename sig_traits<Sig>::return_type;
            if (!overloads || I >= num)
                throw std::runtime_error(
                    "Proxy: call to unbound or missing overload");
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
                // Extract the return value from the Object, throwing
                // runtime_error (not bad_expected_access) on type mismatch.
                if constexpr (std::is_reference_v<R>) {
                    auto cr = result.template cast_ref<std::remove_cvref_t<R>>();
                    if (!cr) throw std::runtime_error(
                        "Proxy: return type mismatch on method call");
                    return *cr.value();
                } else {
                    auto cr = result.template cast_ref<R>();
                    if (!cr) throw std::runtime_error(
                        "Proxy: return type mismatch on method call");
                    return std::move(*cr.value());
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
        if (!getter || !obj) throw std::runtime_error(
            "Proxy: read from unbound property");
        Object result = getter(obj);
        auto cr = result.template cast_ref<T>();
        if (!cr) throw std::runtime_error(
            "Proxy: property type mismatch on read");
        return std::move(*cr.value());
    }

    // Assignment from T (write).  Compile error when Readonly=true.
    void operator=(T val) requires (!Readonly) {
        if (!obj) throw std::runtime_error("Proxy: write to unbound property");
        if (!setter) throw std::runtime_error(
            "Proxy: property has no setter (move-only or non-assignable)");
        std::decay_t<T> storage(std::move(val));
        Object val_ref(storage);
        setter(obj, &val_ref);
        if (after_set) {
            Object current = getter(obj);
            after_set(hook_ctx, current);
        }
    }

    // operator[] — returns a reference to the element in the actual object.
    // Only available when T is subscriptable (std::array, std::vector, etc.).
    // ponytail: no bounds check — out-of-range index is UB, same as raw
    // operator[] on the underlying container.  The caller owns the index.
    template <typename Self>
    auto& operator[](this Self&& self, std::size_t i)
        requires requires { typename std::remove_cvref_t<T>::value_type; }
    {
        return reinterpret_cast<std::remove_cv_t<T>*>(
            static_cast<char*>(self.obj) + self.member_offset)->operator[](i);
    }

    static constexpr bool is_readonly() { return Readonly; }

    // Expected type name of the proxied data member, normalized for
    // comparison against the impl's ClassInfo field type.  Both sides use
    // normalize_type so const-qualifier differences (the interface strips
    // const via remove_cv_t, the impl stores the raw display string) don't
    // cause false mismatches.
    static std::string expected_type() {
        return std::string(detail::normalize_type(
            detail::type_name<std::remove_cvref_t<T>>()));
    }
};

// ---------------------------------------------------------------------------
// Field-type builders — must be after TypedMethod/TypedProperty definitions
// because they use ^^TypedMethod / ^^TypedProperty in consteval substitute().
//
// Statics are not proxied: they are class-level, not instance-level, and
// belong on refl::Class.  Proxy<T>::get_class() and Dyn<T>::get_class()
// return the bound object's Class for static access.
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

// consteval: build the TypedProperty field type for a data member name,
// walking the base hierarchy for inherited data members.
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
    // Walk public bases for inherited data members.
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto found = make_property_field_type(std::meta::type_of(b), name);
            if (found != std::meta::info{}) return found;
        }
    }
    return std::meta::info{};
}

// consteval: collect inherited non-static data members from public bases.
// ponytail: recursive base walk, O(n) per base level; fine for realistic
// hierarchies.  Virtual inheritance unsupported (offset_of is not constant
// for virtual bases), matching the rest of the framework.
consteval void collect_inherited_dms(std::meta::info type,
        std::vector<std::meta::info>& out) {
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto bt = std::meta::type_of(b);
            for (auto m : std::meta::nonstatic_data_members_of(bt,
                    std::meta::access_context::unchecked()))
                out.push_back(m);
            collect_inherited_dms(bt, out);
        }
    }
}

// consteval: collect all non-static method member reflections from a
// type and its public bases (for inherited methods).
consteval void collect_all_methods(std::meta::info type,
        std::vector<std::meta::info>& out) {
    for (auto m : std::meta::members_of(type,
            std::meta::access_context::unchecked())) {
        if (is_public_method(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m))
            out.push_back(m);
    }
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b))
            collect_all_methods(std::meta::type_of(b), out);
    }
}

// consteval: build the dispatch struct member specs for T's public
// interface — non-static methods (TypedMethod) and data members
// (TypedProperty), including inherited members via base hierarchy walk.
// Shared by Proxy<T> and Dyn<T>.  When add_obj is true, an obj field
// (std::shared_ptr<T>) is appended (Dyn<T> concrete mode only).
// Fills the output vector in-place (consteval functions returning
// std::vector by value are not yet reliably supported by GCC 16.2).
consteval void make_dispatch_specs(std::meta::info type, bool add_obj,
        std::vector<std::meta::info>& specs) {

    // Non-static member functions → TypedMethod (one field per name,
    // carrying all overloads).  Walks the base hierarchy via collect_sigs
    // and collect_all_methods for inherited methods.
    std::vector<std::string> seen_fns;
    std::vector<std::meta::info> all_methods;
    collect_all_methods(type, all_methods);
    for (auto m : all_methods) {
        auto nm = std::string(std::meta::identifier_of(m));
        bool dup = false;
        for (const auto& s : seen_fns)
            if (s == nm) { dup = true; break; }
        if (!dup) {
            seen_fns.push_back(nm);
            auto field_type = make_typed_method_type(type,
                std::meta::identifier_of(m));
            specs.push_back(std::meta::data_member_spec(
                field_type, {.name=nm}));
        }
    }

    // Non-static data members → TypedProperty.  Walks the base hierarchy
    // via make_property_field_type.
    std::vector<std::string> seen_fields;
    std::vector<std::meta::info> all_dms;
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked()))
        all_dms.push_back(m);
    collect_inherited_dms(type, all_dms);

    for (auto m : all_dms) {
        if (is_public_data_member(m) && std::meta::has_identifier(m)) {
            auto nm = std::string(std::meta::identifier_of(m));
            bool dup = false;
            for (const auto& s : seen_fields)
                if (s == nm) { dup = true; break; }
            if (!dup) {
                seen_fields.push_back(nm);
                auto field_type = make_property_field_type(type,
                    std::meta::identifier_of(m));
                if (field_type != std::meta::info{})
                    specs.push_back(std::meta::data_member_spec(
                        field_type, {.name=nm}));
            }
        }
    }

    // Dyn<T> concrete mode: append the obj field.
    if (add_obj) {
        auto sp_type = std::meta::substitute(^^std::shared_ptr,
            std::initializer_list<std::meta::info>{type});
        specs.push_back(std::meta::data_member_spec(
            sp_type, {.name="obj"}));
    }
}

}  // namespace detail

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
// not just T itself.  Matching is by method name + param-type signature
// (overloads are paired correctly even if the impl declares them in a
// different order).  This is structural typing through the reflection
// pool: any type with compatible public methods works.
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
// Validation: bind() and the constructor throw std::runtime_error at bind
// time if:
//   1. The Object is non-owning (not backed by a shared_ptr) — the proxy
//      outlives a borrow, so non-owning Objects are rejected.
//   2. The object's type is not registered in the reflection pool.
//   3. Any interface method or field has no match in the object's ClassInfo.
//   4. A matched method's return type or a matched field's type differs
//      from the interface's.
// This catches structural mismatches and lifetime issues as early as
// possible — before any call through the proxy.
//
// Thread safety: NOT thread-safe.  Concurrent bind() + operator->(), or
// concurrent bind() from multiple threads, is a data race on the dispatch
// fields.  The caller must synchronize.  Calls through operator->() from
// multiple threads are safe only if no bind() is in progress.
//
// Proxy<T> is non-copyable, non-movable (dispatch fields point into it).
// ---------------------------------------------------------------------------
template <typename T>
class Proxy {
    struct Dispatch;
    consteval {
        if constexpr (std::is_class_v<T>) {
            std::vector<std::meta::info> specs;
            detail::make_dispatch_specs(^^T, false, specs);
            std::meta::define_aggregate(^^Dispatch, specs);
        } else {
            std::meta::define_aggregate(^^Dispatch, {});
        }
    }

    Dispatch dispatch_;
    Object obj_;
    std::map<std::string, std::vector<detail::OverloadEntry>> overload_storage_;

    static const ClassInfo* lookup_class_info(std::string_view name) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(std::string(name));
        return it != class_pool().end() ? it->second.get() : nullptr;
    }

    void check_owned() {
        if (obj_.valid() && !obj_.is_owned())
            throw std::runtime_error(
                "Proxy: non-owning Object cannot be bound — the proxy "
                "outlives the borrow. Use a shared_ptr-backed Object.");
    }

    static std::string join_types(const std::vector<std::string>& types) {
        std::string s;
        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i) s += ", ";
            s += types[i];
        }
        return s;
    }

    void populate() {
        if constexpr (std::is_class_v<T>) {
            if (!obj_.valid()) return;
            overload_storage_.clear();
            const ClassInfo* info = lookup_class_info(obj_.class_name());
            if (!info)
                throw std::runtime_error(
                    "Proxy: object type '" + std::string(obj_.class_name()) +
                    "' is not registered in the reflection pool");
            static constexpr auto dm = std::define_static_array(
                std::meta::nonstatic_data_members_of(^^Dispatch,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto field : dm) {
                if constexpr (std::meta::has_identifier(field)) {
                    using FieldType = [:std::meta::type_of(field):];
                    if constexpr (detail::is_typed_method_v<
                            std::remove_cv_t<FieldType>>) {
                        // Non-static method → bind from Object's ClassInfo,
                        // matching by param-type signature + return type.
                        // Searches the base hierarchy for inherited methods.
                        using TM = std::remove_cv_t<FieldType>;
                        dispatch_.[:field:].owner = obj_.owner();
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        auto& vec = overload_storage_[key];
                        auto exp_params = TM::expected_param_types();
                        auto exp_returns = TM::expected_return_types();
                        std::ptrdiff_t method_off = 0;
                        for (std::size_t oi = 0; oi < exp_params.size(); ++oi) {
                            auto r = detail::find_function_in_hierarchy(
                                info, key, exp_params[oi]);
                            if (!r.fi)
                                throw std::runtime_error(
                                    "Proxy: no matching overload for '" + key +
                                    "' with params [" +
                                    join_types(exp_params[oi]) +
                                    "] in type '" +
                                    std::string(obj_.class_name()) + "'");
                            if (r.fi->return_type != exp_returns[oi])
                                throw std::runtime_error(
                                    "Proxy: return type mismatch on '" +
                                    key + "' — interface expects '" +
                                    exp_returns[oi] +
                                    "', impl returns '" +
                                    r.fi->return_type + "'");
                            vec.push_back({r.fi->invoker});
                            method_off = r.offset;
                        }
                        dispatch_.[:field:].obj =
                            static_cast<char*>(obj_.raw()) + method_off;
                        dispatch_.[:field:].overloads = vec.data();
                        dispatch_.[:field:].num = vec.size();
                    } else {
                        // Non-static data member → bind from Object's ClassInfo.
                        // Searches the base hierarchy for inherited fields.
                        using TP = std::remove_cv_t<FieldType>;
                        dispatch_.[:field:].owner = obj_.owner();
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        auto exp_type = TP::expected_type();
                        auto r = detail::find_field_in_hierarchy(info, key);
                        if (!r.fi)
                            throw std::runtime_error(
                                "Proxy: no field '" + key +
                                "' in type '" +
                                std::string(obj_.class_name()) + "'");
                        auto impl_type = detail::normalize_type(r.fi->type);
                        if (impl_type != exp_type)
                            throw std::runtime_error(
                                "Proxy: field type mismatch on '" + key +
                                "' — interface expects '" + exp_type +
                                "', impl has '" + impl_type + "'");
                        dispatch_.[:field:].obj =
                            static_cast<char*>(obj_.raw()) + r.offset;
                        dispatch_.[:field:].member_offset =
                            static_cast<std::size_t>(r.fi->offset);
                        dispatch_.[:field:].getter = r.fi->getter;
                        dispatch_.[:field:].setter = r.fi->setter;
                    }
                }
            }
        }
    }

public:
    Proxy() = default;

    // Bind a type-erased Object — wires T's dispatch fields to the
    // Object's methods via its ClassInfo.  The Object must be owning
    // (backed by a shared_ptr); non-owning Objects are rejected because
    // the proxy outlives the borrow.  Use shared_ptr-backed Objects or
    // the implicit shared_ptr<T> → Object conversion:
    //
    //   auto sp = std::make_shared<Square>(4);
    //   refl::Proxy<IDrawable> p(sp);   // implicit, owning
    explicit Proxy(Object obj) : obj_(std::move(obj)) { check_owned(); populate(); }
    void bind(Object obj) { obj_ = std::move(obj); check_owned(); populate(); }

    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }

    bool is_bound() const { return obj_.valid(); }

    // The Class of the bound object's actual runtime type (not T).
    // Statics are class-level, not instance-level — access them through
    // this Class instead of the dispatch struct:
    //   auto c = p.get_class();
    //   int n = *c.find_static_function("instance_count")->invoke().value()
    //                .cast_ref<int>().value();
    // Returns an invalid Class if not bound or the type is unregistered.
    Class get_class() const {
        if (!obj_.valid()) return {};
        return find_class(obj_.class_name()).value_or(Class{});
    }

    Proxy(const Proxy&) = delete;
    Proxy(Proxy&&) = delete;
    Proxy& operator=(const Proxy&) = delete;
    Proxy& operator=(Proxy&&) = delete;
};

}  // namespace refl
