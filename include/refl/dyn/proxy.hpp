// proxy — typed proxy field types for the dyn dispatch layer.
//
// TypedMethod<Sigs...> and TypedProperty<T> are the compile-time field
// types used by Proxy<T> and Dyn<T>'s synthesized Dispatch struct.  Each
// proxies a single reflected member with real C++ types at the call site
// — no std::any, no std::variant.
//
//   TypedMethod<int(), void(int)>     — overload-resolved method dispatch
//   TypedProperty<int>               — data member get/set via operator=/cast
//
// Proxy<T> synthesizes a Dispatch struct from T's public interface at
// compile time, then wires it to a type-erased Object at runtime via its
// ClassInfo.  Statics are not proxied — use Proxy<T>::get_class() for
// static access via refl::Class.
//
// Pure primitives: no dependency on Dyn<T>.  Dyn<T> (in refl/dyn.hpp)
// includes this header and builds its Dispatch struct from these field
// types.
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
//
// Argument matching is exact (after remove_cvref_t) — no implicit
// conversions.  Calling p->render(short{2}) against int render(int) is a
// compile error, not a conversion.  This is stricter than C++ overload
// resolution but keeps the dispatch unambiguous at compile time.
//
// Const-correctness: a const TypedMethod (accessed through a const Proxy
// or Dyn via operator->() const) blocks non-const overloads — calling a
// non-const method through a const proxy throws at call time.  Each
// overload's const-ness is stored at bind time in OverloadEntry::is_const.
//
// Reference returns: R& get_ref() returns a real R& into the bound object.
// The reference's lifetime is tied to the Proxy's bound Object — rebinding
// or destroying the proxy dangles the reference.  The caller must keep
// the proxy alive while holding the reference.
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
    bool is_const = false;  // matches the interface overload's const-ness
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

}  // namespace detail

template <typename... Sigs>
struct TypedMethod {
    const detail::OverloadEntry* overloads = nullptr;
    std::size_t num = 0;
    void* obj = nullptr;
    std::shared_ptr<void> owner;  // shared_ptr for aliasing invoker calls

    // Returns normalized param-type names per overload, in Sigs-pack
    // (interface declaration) order.  Used by populate() to match the
    // impl's FunctionInfo entries by signature, not just by name — so
    // overloads are correctly paired even when the impl type declares
    // them in a different order than the interface.
    static std::vector<std::vector<std::string>> expected_param_types() {
        return { sig_param_names<typename sig_traits<Sigs>::args_type>()... };
    }

    // Returns normalized return-type strings per overload, in Sigs-pack
    // order.  Both sides (interface and impl) are normalized via
    // detail::normalize_type so const/ref-qualifier differences don't
    // cause false mismatches — same pattern as expected_type() for
    // TypedProperty.
    static std::vector<std::string> expected_return_types() {
        return { std::string(detail::normalize_type(
            detail::type_name<
                std::remove_cvref_t<typename sig_traits<Sigs>::return_type>>()))... };
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

    // Deducing-this: a const TypedMethod (accessed through a const
    // Proxy/Dyn, i.e. operator->() const) blocks non-const overloads —
    // a const view cannot mutate the object.  Const is a per-overload
    // property stored in OverloadEntry::is_const at bind time.
    template <typename Self, typename... Args>
    decltype(auto) operator()(this Self&& self, Args&&... args) {
        constexpr bool ConstCall =
            std::is_const_v<std::remove_reference_t<Self>>;
        return self.template call_dispatch<ConstCall, 0, Args...>(
            std::forward<Args>(args)...);
    }

    template <bool ConstCall, std::size_t I, typename... Args>
    decltype(auto) call_dispatch(Args&&... args) const {
        using Sig = std::tuple_element_t<I, std::tuple<Sigs...>>;
        if constexpr (matches_sig<Sig, Args...>) {
            using R = typename sig_traits<Sig>::return_type;
            if (!overloads || I >= num)
                throw std::runtime_error(
                    "Proxy: call to unbound or missing overload");
            if constexpr (ConstCall) {
                if (!overloads[I].is_const)
                    throw std::runtime_error(
                        "Proxy: non-const method called through const proxy");
            }
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
            if constexpr (std::is_void_v<R>) return;
            else {
                auto cr = result.template cast_ref<std::remove_cvref_t<R>>();
                if (!cr) throw std::runtime_error(
                    "Proxy: return type mismatch on method call");
                // Preserve references: return *ptr as R& for reference
                // return types, as R by value for non-reference types.
                if constexpr (std::is_reference_v<R>)
                    return static_cast<R>(*cr.value());
                else
                    return R(std::move(*cr.value()));
            }
        } else {
            if constexpr (I + 1 < sizeof...(Sigs))
                return call_dispatch<ConstCall, I + 1, Args...>(std::forward<Args>(args)...);
            else
                static_assert(false,
                    "no matching overload for the given argument types");
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
        if (!obj) throw std::runtime_error(
            "Proxy: read from unbound property");
        if (!getter) throw std::runtime_error(
            "Proxy: property has no getter (move-only or non-copyable)");
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
            Object current = getter ? getter(obj) : detail::borrow_object(
                static_cast<char*>(obj) + member_offset,
                detail::type_name<std::remove_cvref_t<T>>());
            after_set(hook_ctx, current);
        }
    }

    // operator[] — returns a reference to the element in the actual object.
    // Available whenever T is subscriptable (std::array, std::vector, etc.).
    // For non-readonly members the reference is mutable (writes go through to
    // the object); for Readonly members it is const-qualified, so element
    // reads are allowed but element writes are a compile error — the same
    // contract as whole-object operator= (deleted for Readonly).  A const
    // Proxy also yields a const reference for non-readonly members, so a
    // const view can't mutate the object through element access.  This keeps
    // individual-element access on const containers without a copy.
    // ponytail: no bounds check — out-of-range index is UB, same as raw
    // operator[] on the underlying container.  The caller owns the index.
    template <typename Self>
    auto& operator[](this Self&& self, std::size_t i)
        requires requires { typename std::remove_cvref_t<T>::value_type; }
    {
        if (!self.obj) throw std::runtime_error(
            "Proxy: subscript on unbound property");
        if constexpr (Readonly
                      || std::is_const_v<std::remove_reference_t<Self>>) {
            return reinterpret_cast<const std::remove_cvref_t<T>*>(
                static_cast<const char*>(self.obj)
                    + self.member_offset)->operator[](i);
        } else {
            return reinterpret_cast<std::remove_cv_t<T>*>(
                static_cast<char*>(self.obj)
                    + self.member_offset)->operator[](i);
        }
    }

    static constexpr bool is_readonly() { return Readonly; }

    // Expected type name of the proxied data member, normalized for
    // comparison against the impl's ClassInfo field type.  Both sides use
    // normalize_type so const-qualifier differences (the interface strips
    // cv-ref via remove_cvref_t, the impl stores the raw display string)
    // don't cause false mismatches.
    static std::string expected_type() {
        return std::string(detail::normalize_type(
            detail::type_name<T>()));
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
// type and its public bases (for inherited methods).  Respects C++
// name-hiding: if the type declares any own method of a given name,
// all base methods of that name are hidden — matching
// find_function_in_hierarchy on the impl side.
consteval void collect_all_methods(std::meta::info type,
        std::vector<std::meta::info>& out) {
    std::vector<std::string> own_names;
    for (auto m : std::meta::members_of(type,
            std::meta::access_context::unchecked())) {
        if (is_public_method(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m)) {
            out.push_back(m);
            own_names.push_back(std::string(std::meta::identifier_of(m)));
        }
    }
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto bt = std::meta::type_of(b);
            std::vector<std::meta::info> inherited;
            collect_all_methods(bt, inherited);
            for (auto m : inherited) {
                auto nm = std::string(std::meta::identifier_of(m));
                bool hidden = false;
                for (const auto& on : own_names)
                    if (on == nm) { hidden = true; break; }
                if (!hidden) out.push_back(m);
            }
        }
    }
}

// consteval: collect the const-ness of all overloads for a named method
// on the interface type, in Sigs-pack order.  Used by populate() to
// verify the impl's method const-ness matches the interface's.
// Reuses collect_all_methods (the single hierarchy walker with name-
// hiding) and filters by name, so the walk + name-hiding logic lives in
// one place.  Order is preserved: own methods in declaration order, then
// inherited non-hidden — matching the Sigs-pack order built by
// make_dispatch_specs, so exp_const_arr[i] pairs with exp_params[i].
consteval std::vector<bool> collect_const_quals(std::meta::info type,
                                                   std::string_view name) {
    std::vector<std::meta::info> all;
    collect_all_methods(type, all);
    std::vector<bool> result;
    for (auto m : all)
        if (std::meta::identifier_of(m) == name)
            result.push_back(is_const_method(m));
    return result;
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
    // carrying all overloads).  collect_all_methods walks the hierarchy
    // once; we group by name and build the function-type sigs inline,
    // avoiding a per-name re-walk.
    std::vector<std::string> method_names;
    std::vector<std::vector<std::meta::info>> method_sigs;
    std::vector<std::meta::info> all_methods;
    collect_all_methods(type, all_methods);
    for (auto m : all_methods) {
        auto nm = std::string(std::meta::identifier_of(m));
        bool found = false;
        for (std::size_t i = 0; i < method_names.size(); ++i) {
            if (method_names[i] == nm) {
                method_sigs[i].push_back(make_fn_sig(m));
                found = true;
                break;
            }
        }
        if (!found) {
            method_names.push_back(nm);
            method_sigs.push_back({make_fn_sig(m)});
        }
    }
    for (std::size_t i = 0; i < method_names.size(); ++i) {
        auto field_type = std::meta::substitute(^^TypedMethod,
            method_sigs[i]);
        specs.push_back(std::meta::data_member_spec(
            field_type, {.name=method_names[i]}));
    }

    // Non-static data members → TypedProperty.  All DMs (own + inherited)
    // are collected in one walk; the property type is built inline from
    // the member reflection, avoiding a per-name re-walk.  First
    // occurrence of each name wins (own members shadow bases).
    std::vector<std::string> field_names;
    std::vector<std::meta::info> all_dms;
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked()))
        all_dms.push_back(m);
    collect_inherited_dms(type, all_dms);

    for (auto m : all_dms) {
        if (is_public_data_member(m) && std::meta::has_identifier(m)) {
            auto nm = std::string(std::meta::identifier_of(m));
            bool dup = false;
            for (const auto& s : field_names)
                if (s == nm) { dup = true; break; }
            if (!dup) {
                field_names.push_back(nm);
                auto mt = std::meta::type_of(m);
                bool is_const = std::meta::is_const_type(mt);
                auto clean_mt = std::meta::substitute(^^std::remove_cvref_t,
                    std::initializer_list<std::meta::info>{mt});
                auto field_type = std::meta::substitute(^^TypedProperty,
                    {clean_mt, std::meta::reflect_constant(is_const)});
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
// Const-correctness: a const Proxy<T> (operator->() const) yields a const
// view — non-const methods throw at call time, readonly properties block
// writes, and operator[] yields const references.  Non-const methods,
// non-readonly properties, and mutable operator[] require a non-const
// Proxy.
//
// Proxy<T> is non-copyable.  Movable: move transfers the bound Object
// and re-populates the dispatch fields (which point into overload_storage_,
// a member of the Proxy, so they must be re-pointed after the move).  A
// moved-from Proxy is unbound: is_bound() returns false and calls through
// operator->() throw rather than use stale pointers.
// ---------------------------------------------------------------------------
template <typename T>
class Proxy {
    struct Dispatch;
    consteval {
        std::vector<std::meta::info> specs;
        detail::make_dispatch_specs(^^T, false, specs);
        std::meta::define_aggregate(^^Dispatch, specs);
    }

    Dispatch dispatch_;
    Object obj_;
    std::map<std::string, std::vector<detail::OverloadEntry>> overload_storage_;

    static void check_owned(const Object& obj) {
        if (obj.valid() && !obj.is_owned())
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

    // Check the impl's const-ness matches the interface's for one overload.
    // A const impl satisfies a non-const interface declaration (const is the
    // stronger guarantee — calling a const method on a non-const object is
    // fine).  Only the reverse is a contract violation: a non-const impl
    // cannot satisfy a const interface requirement.
    // Shared by operator and named-method binding paths.
    template <typename ConstArr>
    static void check_const_qual(const ConstArr& exp_const,
            std::size_t oi, const FunctionInfo* fi, std::string_view key) {
        if (oi < exp_const.size() && exp_const[oi] && !fi->is_const)
            throw std::runtime_error(
                "Proxy: const-ness mismatch on '" + std::string(key) +
                "' — interface expects const, impl is non-const");
    }

    void populate() {
        if (!obj_.valid()) return;
        overload_storage_.clear();
        const ClassInfo* info = detail::lookup_class_info(obj_.class_name());
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
                    using TM = std::remove_cv_t<FieldType>;
                    dispatch_.[:field:].owner = obj_.owner();
                    constexpr auto nm_sv = std::meta::identifier_of(field);
                    static constexpr auto exp_const_arr = []() consteval {
                        return std::define_static_array(
                            detail::collect_const_quals(^^T, nm_sv));
                    }();
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
                        if (detail::normalize_type(r.fi->return_type)
                                != exp_returns[oi])
                            throw std::runtime_error(
                                "Proxy: return type mismatch on '" +
                                key + "' — interface expects '" +
                                exp_returns[oi] +
                                "', impl returns '" +
                                r.fi->return_type + "'");
                        check_const_qual(exp_const_arr, oi, r.fi, key);
                        vec.push_back({r.fi->invoker, r.fi->is_const});
                        method_off = r.offset;
                    }
                    dispatch_.[:field:].obj =
                        static_cast<char*>(obj_.raw()) + method_off;
                    dispatch_.[:field:].overloads = vec.data();
                    dispatch_.[:field:].num = vec.size();
                } else {
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

    // Reset all dispatch fields to the unbound state so a moved-from
    // Proxy throws cleanly instead of calling through stale pointers.
    void clear_dispatch() {
        static constexpr auto dm = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch,
                std::meta::access_context::unchecked()));
        template for (constexpr auto field : dm) {
            if constexpr (std::meta::has_identifier(field)) {
                using FieldType = [:std::meta::type_of(field):];
                if constexpr (detail::is_typed_method_v<
                        std::remove_cv_t<FieldType>>) {
                    dispatch_.[:field:].overloads = nullptr;
                    dispatch_.[:field:].num = 0;
                    dispatch_.[:field:].obj = nullptr;
                    dispatch_.[:field:].owner.reset();
                } else {
                    dispatch_.[:field:].getter = nullptr;
                    dispatch_.[:field:].setter = nullptr;
                    dispatch_.[:field:].obj = nullptr;
                    dispatch_.[:field:].owner.reset();
                }
            }
        }
        overload_storage_.clear();
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
    explicit Proxy(Object obj) : obj_(std::move(obj)) { check_owned(obj_); populate(); }
    // Transactional rebind: build a temporary proxy bound to the new object
    // (which validates structural compatibility via populate()) before
    // releasing the existing binding.  populate() clears overload_storage_
    // then walks the ClassInfo and can throw partway through (missing
    // overload, return-type mismatch, const-ness mismatch, missing field).
    // Before this fix, a throw there left dispatch_ overloads pointers
    // dangling into the freed overload_storage_ vectors while obj_ already
    // pointed at the new object — a use-after-free on the next call.  Now
    // the failed populate destroys the temporary without touching *this.
    // ponytail: double populate() walk (once in the temporary, once in the
    // move-assign) on success — O(members), negligible for realistic types.
    void bind(Object obj) {
        check_owned(obj);
        Proxy tmp(std::move(obj));
        *this = std::move(tmp);
    }

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

    // -----------------------------------------------------------------------
    // Operators — looked up at call time via Object::invoke_op, which
    // resolves overloads by the argument's runtime type.
    //
    // The return type depends on the interface's operator return type:
    //   - void:           returns nothing
    //   - bool:           returns bool (comparison operators)
    //   - class type:     returns Proxy<R> wrapping the result (chainable)
    //   - non-class type: returns the raw value (int, double, etc.)
    //
    // Each operator generates two methods:
    //   1. Same-type: operator+(const Proxy& other)
    //   2. Generic: template<U> operator+(U&& val)
    //
    // Neither exists if T has no such operator.  The constraint uses
    // decltype — no consteval helpers needed for the return type.
    // -----------------------------------------------------------------------
public:
#define PROXY_BINARY_OP(symbol, op_name) \
    auto operator symbol(const Proxy& other) const \
        requires requires(T a, T b) { a symbol b; } \
    { \
        using R = decltype(std::declval<T>() symbol std::declval<T>()); \
        Object arg = other.obj_; \
        auto result = obj_.invoke_op(op_name, arg); \
        if (!result) \
            throw std::runtime_error( \
                "Proxy: " op_name " — no matching overload for '" + \
                std::string(arg.class_name()) + "' in '" + \
                std::string(obj_.class_name()) + "'"); \
        if constexpr (std::is_void_v<R>) return; \
        else if constexpr (std::is_class_v<std::remove_cvref_t<R>>) \
            return Proxy(std::move(*result)); \
        else { \
            auto cr = result->template cast_ref<std::remove_cvref_t<R>>(); \
            if (!cr) throw std::runtime_error( \
                "Proxy: " op_name " — return type mismatch: interface expects '" \
                + std::string(detail::type_name<std::remove_cvref_t<R>>()) \
                + "', impl returns '" + std::string(result->class_name()) + "'"); \
            return std::move(*cr.value()); \
        } \
    } \
    template <typename U> \
        requires (!std::is_same_v<std::remove_cvref_t<U>, Proxy>) \
              && requires(T a, U b) { a symbol b; } \
    auto operator symbol(U&& val) const \
    { \
        using R = decltype(std::declval<T>() symbol std::declval<U>()); \
        Object arg(std::forward<U>(val)); \
        auto result = obj_.invoke_op(op_name, arg); \
        if (!result) \
            throw std::runtime_error( \
                "Proxy: " op_name " — no matching overload for '" + \
                std::string(arg.class_name()) + "' in '" + \
                std::string(obj_.class_name()) + "'"); \
        if constexpr (std::is_void_v<R>) return; \
        else if constexpr (std::is_class_v<std::remove_cvref_t<R>>) \
            return Proxy(std::move(*result)); \
        else { \
            auto cr = result->template cast_ref<std::remove_cvref_t<R>>(); \
            if (!cr) throw std::runtime_error( \
                "Proxy: " op_name " — return type mismatch: interface expects '" \
                + std::string(detail::type_name<std::remove_cvref_t<R>>()) \
                + "', impl returns '" + std::string(result->class_name()) + "'"); \
            return std::move(*cr.value()); \
        } \
    }

    PROXY_BINARY_OP(+, "operator+")
    PROXY_BINARY_OP(-, "operator-")
    PROXY_BINARY_OP(*, "operator*")
    PROXY_BINARY_OP(/ , "operator/")
    PROXY_BINARY_OP(%, "operator%")
    PROXY_BINARY_OP(==, "operator==")
    PROXY_BINARY_OP(!=, "operator!=")
    PROXY_BINARY_OP(< , "operator<")
    PROXY_BINARY_OP(> , "operator>")
    PROXY_BINARY_OP(<=, "operator<=")
    PROXY_BINARY_OP(>=, "operator>=")

#undef PROXY_BINARY_OP

    Proxy(const Proxy&) = delete;
    Proxy(Proxy&& other)
        : obj_(std::move(other.obj_)) {
        if (obj_.valid()) { populate(); }
        other.obj_ = {};
        other.clear_dispatch();
    }
    Proxy& operator=(const Proxy&) = delete;
    Proxy& operator=(Proxy&& other) {
        if (this != &other) {
            obj_ = std::move(other.obj_);
            other.obj_ = {};
            if (obj_.valid()) { populate(); }
            other.clear_dispatch();
        }
        return *this;
    }
};

}  // namespace refl
