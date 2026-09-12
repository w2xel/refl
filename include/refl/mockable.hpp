// mockable — runtime mock with swappable per-method and per-property slots.
//
// Mockable<T> synthesizes a per-instance ClassInfo whose method invokers
// and property getter/setters are static trampolines that read swappable
// slots at call time.  implement() sets a method slot's invoker.
// inject_hook() wraps a method slot's invoker in a hook trampoline.
// on_change() wraps a property slot's setter to fire callbacks after
// writes.  All work after Proxy bind — the Proxy holds the static
// trampoline pointers, which always re-read the slots.
//
//   auto m = refl::Mockable<IWidget>::create();
//   m->implement<^^IWidget::draw>([]() { ... });
//   m->set_property<^^IWidget::width>(42);
//   auto p = m->proxy();
//   m->on_change<^^IWidget::width>([](refl::Object& v) { ... });
//   p->width = 99;  // fires the on_change callback
//
// Prototype: method + property hooks, 0-2 args.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <any>
#include <functional>
#include <map>
#include <vector>

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
            return Object{};
        }
    }

    struct HookCtx {
        InvokerFn prev_invoker;
        void* prev_ctx;
        std::vector<std::function<void(Object&)>>* hooks;
    };

    template <std::meta::info Method>
    static Object hook_trampoline(const std::shared_ptr<void>& owner,
                                    void* ctx, const Object* args) {
        auto* hc = static_cast<HookCtx*>(ctx);
        Object result = hc->prev_invoker
            ? hc->prev_invoker(owner, hc->prev_ctx, args)
            : Object{};
        for (auto& cb : *hc->hooks) cb(result);
        return result;
    }

    // ======================================================================
    // Property trampolines
    // ======================================================================
    // The mock has no real object, so properties are stored as std::any
    // values on the Mockable.  The getter/setter trampolines recover the
    // Mockable* from obj (which Proxy sets to obj_.raw() + offset; offset
    // is 0 for the mock since there's no base hierarchy) and read/write
    // the stored value.

    struct PropHookCtx {
        SetterFn prev_setter;
        void* prev_set_ctx;
        std::shared_ptr<const ClassInfo> class_info;
        std::vector<std::function<void(Object&)>>* hooks;
    };

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

    // Property implementation getter/setter: read/write a stored std::any.
    // ctx is the Mockable* itself.
    template <std::meta::info Member>
    static Object prop_impl_get(void* ctx) {
        auto* self = static_cast<Mockable<T>*>(ctx);
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->prop_values_.find(key);
        if (it == self->prop_values_.end())
            throw std::runtime_error(
                "Mockable: property '" + key + "' not set");
        auto& val = std::any_cast<Bare&>(it->second);
        return Object(std::make_shared<Bare>(val),
                     detail::ensure_class_info<Bare>());
    }

    template <std::meta::info Member>
    static void prop_impl_set(void* ctx, const Object* val) {
        auto* self = static_cast<Mockable<T>*>(ctx);
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto cr = val->template cast_ref<Bare>();
        if (!cr) throw std::runtime_error(
            "Mockable: property '" + key + "' type mismatch on set");
        self->prop_values_[key] = Bare(std::move(*cr.value()));
    }

    // Property hook setter: calls the previous setter, then reads the
    // current value (via the slot's getter) and fires the hook list.
    template <std::meta::info Member>
    static void prop_hook_set(void* ctx, const Object* val) {
        auto* hc = static_cast<PropHookCtx*>(ctx);
        hc->prev_setter(hc->prev_set_ctx, val);
        // Read current value via the property's getter slot.
        auto* self = static_cast<Mockable<T>*>(hc->prev_set_ctx);
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->prop_slots_.find(key);
        Object current = it->second.getter
            ? it->second.getter(it->second.get_ctx)
            : Object{};
        for (auto& cb : *hc->hooks) cb(current);
    }

    // consteval helpers: return function pointers to trampolines.  GCC 16.2
    // requires taking the address of a function template instantiated with a
    // std::meta::info NTTP to be in a constant-evaluated context (the function
    // body uses consteval-only expressions like define_static_string).  These
    // wrappers provide that context.
    template <std::meta::info Method>
    consteval static InvokerFn get_hook_tramp() {
        return &hook_trampoline<Method>;
    }
    template <std::meta::info Method>
    consteval static InvokerFn get_impl_tramp() {
        return &impl_trampoline<Method>;
    }
    template <std::meta::info Method>
    consteval static InvokerFn get_slot_tramp() {
        return &slot_trampoline<Method>;
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
    consteval static GetterFn get_prop_impl_get() {
        return &prop_impl_get<Member>;
    }
    template <std::meta::info Member>
    consteval static SetterFn get_prop_impl_set() {
        return &prop_impl_set<Member>;
    }
    template <std::meta::info Member>
    consteval static SetterFn get_prop_hook_set() {
        return &prop_hook_set<Member>;
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
    std::map<std::string, std::any> prop_values_;
    std::map<std::string, std::vector<std::function<void(Object&)>>> hooks_;
    std::map<std::string, HookCtx> hook_ctxs_;
    std::map<std::string, std::vector<std::function<void(Object&)>>> prop_hooks_;
    std::map<std::string, PropHookCtx> prop_hook_ctxs_;

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

    template <std::meta::info Method>
    void inject_hook(std::function<void(Object&)> cb) {
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& vec = hooks_[key];
        vec.push_back(std::move(cb));
        auto& slot = method_slots_[key];
        if (hook_ctxs_.find(key) == hook_ctxs_.end()) {
            auto& hc = hook_ctxs_[key];
            hc.prev_invoker = slot.invoker;
            hc.prev_ctx = slot.ctx;
            hc.hooks = &vec;
            slot.invoker = get_hook_tramp<Method>();
            slot.ctx = &hc;
        }
    }

    // ======================================================================
    // Property API
    // ======================================================================

    // Set a mock property's initial value (wires the impl getter/setter).
    template <std::meta::info Member, typename V>
    void set_property(V&& val) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        prop_values_[key] = Bare(std::forward<V>(val));
        prop_slots_[key] = {
            (GetterFn)get_prop_impl_get<Member>(), this,
            (SetterFn)get_prop_impl_set<Member>(), this
        };
    }

    // Inject an on_change hook on a property.  Wraps the property's
    // setter so that after a write, the current value is read back and
    // all hooks fire.  Works after Proxy bind.
    template <std::meta::info Member>
    void on_change(std::function<void(Object&)> cb) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& vec = prop_hooks_[key];
        vec.push_back(std::move(cb));
        auto& slot = prop_slots_[key];
        if (prop_hook_ctxs_.find(key) == prop_hook_ctxs_.end()) {
            auto& hc = prop_hook_ctxs_[key];
            hc.prev_setter = slot.setter;
            hc.prev_set_ctx = slot.set_ctx;
            hc.class_info = detail::ensure_class_info<Bare>();
            hc.hooks = &vec;
            slot.setter = get_prop_hook_set<Member>();
            slot.set_ctx = &hc;
        }
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
