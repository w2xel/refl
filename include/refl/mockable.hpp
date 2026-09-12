// mockable — runtime mock with swappable per-method invokers.
//
// Mockable<T> synthesizes a per-instance ClassInfo whose invoker is a
// static trampoline that reads a MethodSlot at call time.  Each slot
// holds a swappable (InvokerFn, void* ctx) pair.  implement() sets the
// slot's invoker to the user lambda.  inject_hook() wraps the current
// invoker in a hook trampoline and swaps it in — all after Proxy bind,
// because the Proxy only holds the static trampoline pointer, which
// always re-reads the slot.
//
//   auto m = refl::Mockable<IShape>::create();
//   m->implement<^^IShape::area>([](int s) { return s * 100; });
//   auto p = m->proxy();
//   // After bind: inject a hook that fires after area() calls.
//   m->inject_hook<^^IShape::area>([](refl::Object& r) { ... });
//   p->area(5);  // calls the lambda, then fires the hook
//
// Prototype: method-only (no property hooks), 0-2 args.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <any>
#include <functional>
#include <map>
#include <vector>

namespace refl {

// A swappable invoker slot.  The ClassInfo trampoline reads
// slot->invoker and slot->ctx at call time, so swapping here takes
// effect for all already-bound Proxies immediately.
struct MethodSlot {
    InvokerFn invoker = nullptr;
    void* ctx = nullptr;
};

template <typename T>
class Mockable : public std::enable_shared_from_this<Mockable<T>> {
    // The static trampoline stored in ClassInfo.functions[].invoker.
    // It reads the MethodSlot for this method (keyed by a compile-time
    // index) and dispatches to slot->invoker with slot->ctx.  The slot
    // pointer is stored in a static per-(T, Method) table, looked up
    // by recovering the Mockable* from obj and indexing into its
    // slots_ vector.
    template <std::meta::info Method>
    static Object slot_trampoline(const std::shared_ptr<void>& owner,
                                   void* obj, const Object* args) {
        auto* self = static_cast<Mockable<T>*>(obj);
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto it = self->slots_.find(key);
        if (it == self->slots_.end() || !it->second.invoker)
            throw std::runtime_error(
                "Mockable: method '" + key + "' not implemented");
        return it->second.invoker(owner, it->second.ctx, args);
    }

    // --- Implementation trampoline: calls the user lambda.
    //     ctx points at the MethodSlot itself; the lambda is in
    //     callables_[key].  We read the slot to find the key, but
    //     the lambda is stored separately (std::any in callables_).
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

    // --- Hook trampoline: calls the current invoker, then fires hooks.
    //     ctx points at a HookCtx that holds the previous (wrapped)
    //     invoker+ctx and the hook list.  This is what inject_hook
    //     installs as the new slot->invoker.
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

    // Build a per-instance ClassInfo.  Each FunctionInfo.invoker points
    // at slot_trampoline<m> — the static trampoline that reads the
    // swappable slot at call time.
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
                    fi.invoker = &slot_trampoline<m>;
                    static constexpr auto fparams = std::define_static_array(
                        std::meta::parameters_of(m));
                    template for (constexpr auto p : fparams) {
                        fi.param_types.emplace_back(
                            detail::normalize_type(
                                std::meta::display_string_of(
                                    std::meta::type_of(p))));
                    }
                    ci->functions.push_back(std::move(fi));
                }
            }
        }
        return ci;
    }

    std::shared_ptr<const ClassInfo> class_info_;
    std::map<std::string, std::any> callables_;
    std::map<std::string, MethodSlot> slots_;
    // Hook storage: stable vectors (map nodes are stable, .data() valid).
    std::map<std::string, std::vector<std::function<void(Object&)>>> hooks_;
    std::map<std::string, HookCtx> hook_ctxs_;

    Mockable() : class_info_(make_class_info()) {}

public:
    static std::shared_ptr<Mockable<T>> create() {
        return std::shared_ptr<Mockable<T>>(new Mockable<T>());
    }

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
        // Wire the slot: invoker points at impl_trampoline, ctx is self.
        slots_[key] = {&impl_trampoline<Method>, this};
    }

    // Inject a hook (after-only observer) on a method.  Wraps the
    // current slot invoker in hook_trampoline<Method>, which calls
    // the previous invoker then fires all hooks for this method.
    // Multiple inject_hook calls accumulate — all callbacks fire.
    // Works after Proxy bind: the Proxy holds slot_trampoline, which
    // re-reads the slot at call time.
    template <std::meta::info Method>
    void inject_hook(std::function<void(Object&)> cb) {
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& vec = hooks_[key];
        vec.push_back(std::move(cb));
        auto& slot = slots_[key];
        if (hook_ctxs_.find(key) == hook_ctxs_.end()) {
            // First hook: wrap the current invoker.
            auto& hc = hook_ctxs_[key];
            hc.prev_invoker = slot.invoker;
            hc.prev_ctx = slot.ctx;
            hc.hooks = &vec;
            slot.invoker = &hook_trampoline<Method>;
            slot.ctx = &hc;
        }
        // Subsequent hooks: already wrapped, just appended to vec.
    }

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
