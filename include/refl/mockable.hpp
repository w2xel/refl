// mockable — runtime mock with swappable per-method and per-property slots.
//
// Mockable<T> synthesizes a per-instance ClassInfo whose method invokers
// and property getter/setters are static trampolines that read swappable
// slots at call time.  implement() sets a method slot.  implement_property()
// sets a property slot with user-provided getter/setter lambdas.
// set_property() is sugar for a stored-value property.  All work after
// Proxy bind — the Proxy holds the static trampoline pointers, which
// always re-read the slots.
//
//   auto m = refl::Mockable<IWidget>::create();
//   m->implement<^^IWidget::draw>([]() { ... });
//   m->set_property<^^IWidget::width>(42);
//   m->implement_property<^^IWidget::height>(
//       []() { return 24; },
//       [](int v) { ... });
//   auto p = m->proxy();
//   p->width = 99;
//
// Hooks are user-side: wrap your lambda before passing it to implement
// or implement_property.  Re-implement to add a hook — the slot swap is
// picked up by all bound Proxies at call time.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <any>
#include <functional>
#include <map>

namespace refl {

// Swappable invoker slot for methods.
struct MethodSlot {
    InvokerFn invoker = nullptr;
    void* ctx = nullptr;
};

// Swappable getter/setter slot for properties.
struct PropertySlot {
    GetterFn getter = nullptr;
    void* get_ctx = nullptr;
    SetterFn setter = nullptr;
    void* set_ctx = nullptr;
};

template <typename T>
class Mockable : public std::enable_shared_from_this<Mockable<T>> {

    // ======================================================================
    // Method trampolines
    // ======================================================================

    template <std::meta::info Method>
    static Object slot_trampoline(const std::shared_ptr<void>& owner,
                                   void* obj, const Object* args) {
        auto* self = static_cast<Mockable<T>*>(obj);
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->method_slots_.find(key);
        if (it == self->method_slots_.end() || !it->second.invoker)
            throw std::runtime_error(
                "Mockable: method '" + key + "' not implemented");
        return it->second.invoker(owner, it->second.ctx, args);
    }

    template <std::meta::info Method>
    static Object impl_trampoline(const std::shared_ptr<void>&,
                                    void* obj, const Object* args) {
        auto* self = static_cast<Mockable<T>*>(obj);
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        if constexpr (params.size() == 0) {
            auto& fn = std::any_cast<std::function<R()>&>(
                self->callables_[key]);
            if constexpr (std::is_void_v<R>) { fn(); return Object{}; }
            else return Object(std::make_shared<R>(fn()),
                             detail::ensure_class_info<R>());
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            auto& fn = std::any_cast<std::function<R(C0)>&>(
                self->callables_[key]);
            auto& a0 = *static_cast<C0*>(args[0].raw());
            if constexpr (std::is_void_v<R>) { fn(a0); return Object{}; }
            else return Object(std::make_shared<R>(fn(a0)),
                             detail::ensure_class_info<R>());
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            auto& fn = std::any_cast<std::function<R(C0, C1)>&>(
                self->callables_[key]);
            auto& a0 = *static_cast<C0*>(args[0].raw());
            auto& a1 = *static_cast<C1*>(args[1].raw());
            if constexpr (std::is_void_v<R>) { fn(a0, a1); return Object{}; }
            else return Object(std::make_shared<R>(fn(a0, a1)),
                             detail::ensure_class_info<R>());
        } else {
            // ponytail: 3+ args not yet supported.
            return Object{};
        }
    }

    // ======================================================================
    // Property trampolines
    // ======================================================================
    // The ClassInfo's FieldInfo getter/setter point at prop_get_trampoline
    // / prop_set_trampoline, which read the swappable PropertySlot at call
    // time.  The slot holds the user's getter/setter lambdas (via
    // prop_get_impl / prop_set_impl trampolines that call the stored
    // std::function).  Proxy's obj (obj_.raw() + offset) is the Mockable*
    // (offset 0 — no base hierarchy in a mock).

    template <std::meta::info Member>
    static Object prop_get_trampoline(void* obj) {
        auto* self = static_cast<Mockable<T>*>(obj);
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->prop_slots_.find(key);
        if (it == self->prop_slots_.end() || !it->second.getter)
            throw std::runtime_error(
                "Mockable: property '" + key + "' has no getter");
        return it->second.getter(it->second.get_ctx);
    }

    template <std::meta::info Member>
    static void prop_set_trampoline(void* obj, const Object* val) {
        auto* self = static_cast<Mockable<T>*>(obj);
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->prop_slots_.find(key);
        if (it == self->prop_slots_.end() || !it->second.setter)
            throw std::runtime_error(
                "Mockable: property '" + key + "' has no setter");
        it->second.setter(it->second.set_ctx, val);
    }

    // Call the user's getter lambda.  ctx is the Mockable*.
    template <std::meta::info Member>
    static Object prop_get_impl(void* ctx) {
        auto* self = static_cast<Mockable<T>*>(ctx);
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& fn = std::any_cast<std::function<Bare()>&>(
            self->prop_getters_[key]);
        return Object(std::make_shared<Bare>(fn()),
                     detail::ensure_class_info<Bare>());
    }

    // Call the user's setter lambda.  ctx is the Mockable*.
    template <std::meta::info Member>
    static void prop_set_impl(void* ctx, const Object* val) {
        auto* self = static_cast<Mockable<T>*>(ctx);
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& fn = std::any_cast<std::function<void(Bare)>&>(
            self->prop_setters_[key]);
        auto cr = val->template cast_ref<Bare>();
        if (!cr) throw std::runtime_error(
            "Mockable: property '" + key + "' type mismatch on set");
        fn(std::move(*cr.value()));
    }

    // ======================================================================
    // Consteval helpers: return function pointers to trampolines.  GCC 16.2
    // requires taking the address of a function template instantiated with a
    // std::meta::info NTTP to be in a constant-evaluated context (the
    // function body uses consteval-only expressions like define_static_string).
    // ======================================================================

    template <std::meta::info Method>
    consteval static InvokerFn get_slot_tramp() {
        return &slot_trampoline<Method>;
    }
    template <std::meta::info Method>
    consteval static InvokerFn get_impl_tramp() {
        return &impl_trampoline<Method>;
    }
    template <std::meta::info Member>
    consteval static GetterFn get_prop_get_tramp() {
        return &prop_get_trampoline<Member>;
    }
    template <std::meta::info Member>
    consteval static SetterFn get_prop_set_tramp() {
        return &prop_set_trampoline<Member>;
    }
    template <std::meta::info Member>
    consteval static GetterFn get_prop_get_impl() {
        return &prop_get_impl<Member>;
    }
    template <std::meta::info Member>
    consteval static SetterFn get_prop_set_impl() {
        return &prop_set_impl<Member>;
    }

    // ======================================================================
    // ClassInfo synthesis
    // ======================================================================

    static std::shared_ptr<const ClassInfo> make_class_info() {
        auto ci = std::make_shared<ClassInfo>();
        ci->name = std::string(detail::type_name<T>()) + "$mock";
        if constexpr (std::is_class_v<T>) {
            static constexpr auto all_members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto m : all_members) {
                if constexpr (detail::is_public_method(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)) {
                    FunctionInfo fi;
                    fi.name = std::string(std::meta::identifier_of(m));
                    fi.is_const = detail::is_const_method(m);
                    fi.return_type = std::string(
                        std::meta::display_string_of(
                            std::meta::return_type_of(m)));
                    fi.invoker = get_slot_tramp<m>();
                    static constexpr auto fparams = std::define_static_array(
                        std::meta::parameters_of(m));
                    template for (constexpr auto p : fparams) {
                        fi.param_types.emplace_back(
                            detail::normalize_type(
                                std::meta::display_string_of(
                                    std::meta::type_of(p))));
                    }
                    ci->functions.push_back(std::move(fi));
                } else if constexpr (detail::is_public_data_member(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)) {
                    FieldInfo fi;
                    fi.name = std::string(std::meta::identifier_of(m));
                    fi.type = std::string(
                        std::meta::display_string_of(std::meta::type_of(m)));
                    fi.offset = 0;
                    using MemberType = [:std::meta::type_of(m):];
                    fi.is_const = std::is_const_v<MemberType>;
                    fi.getter = get_prop_get_tramp<m>();
                    fi.setter = std::is_const_v<MemberType>
                        ? nullptr
                        : get_prop_set_tramp<m>();
                    ci->fields.push_back(std::move(fi));
                }
            }
        }
        return ci;
    }

    // ======================================================================
    // State
    // ======================================================================

    std::shared_ptr<const ClassInfo> class_info_;
    std::map<std::string, std::any> callables_;
    std::map<std::string, MethodSlot> method_slots_;
    std::map<std::string, PropertySlot> prop_slots_;
    std::map<std::string, std::any> prop_getters_;
    std::map<std::string, std::any> prop_setters_;

    Mockable() : class_info_(make_class_info()) {}

public:
    static std::shared_ptr<Mockable<T>> create() {
        return std::shared_ptr<Mockable<T>>(new Mockable<T>());
    }

    // ======================================================================
    // Method API
    // ======================================================================

    template <std::meta::info Method, typename F>
    void implement(F fn) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        if constexpr (params.size() == 0) {
            callables_[key] = std::function<R()>(std::move(fn));
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            callables_[key] = std::function<R(C0)>(std::move(fn));
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            callables_[key] = std::function<R(C0, C1)>(std::move(fn));
        }
        method_slots_[key] = {get_impl_tramp<Method>(), this};
    }

    // ======================================================================
    // Property API
    // ======================================================================

    // Virtual property: user provides getter and setter lambdas.
    // The getter must return the property type; the setter takes it by
    // value.  For read-only properties, pass nullptr as the setter (or
    // omit it — see the one-argument overload below).
    template <std::meta::info Member, typename G, typename S>
    void implement_property(G getter, S setter) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        prop_getters_[key] = std::function<Bare()>(std::move(getter));
        prop_setters_[key] = std::function<void(Bare)>(std::move(setter));
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), this,
            get_prop_set_impl<Member>(), this
        };
    }

    // Read-only virtual property: getter only, no setter.
    template <std::meta::info Member, typename G>
    void implement_property(G getter) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        prop_getters_[key] = std::function<Bare()>(std::move(getter));
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), this,
            nullptr, nullptr
        };
    }

    // Stored-value property sugar: wires a getter/setter that read/write
    // a value stored on the Mockable.  Equivalent to implement_property
    // with lambdas that capture a stored copy.
    template <std::meta::info Member, typename V>
    void set_property(V&& val) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto stored = std::make_shared<Bare>(std::forward<V>(val));
        prop_getters_[key] = std::function<Bare()>(
            [stored] { return *stored; });
        prop_setters_[key] = std::function<void(Bare)>(
            [stored](Bare v) { *stored = std::move(v); });
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), this,
            get_prop_set_impl<Member>(), this
        };
    }

    // ======================================================================
    // Object / Proxy
    // ======================================================================

    Object as_object() {
        auto sp = this->shared_from_this();
        return Object(std::static_pointer_cast<void>(sp), this,
                      class_info_);
    }

    Proxy<T> proxy() {
        return Proxy<T>(as_object());
    }
};

}  // namespace refl
