// hooks — Qt-style signal/slot layer on top of Dyn<T>.
//
// Hooks<T> extends Dyn<T> with connect(), on_change(), and emit().
// These are built entirely on Dyn's wrap() and Mockable's slot API —
// no new dispatch mechanism, no new trampolines.
//
//   refl::Hooks<Point> p(1, 2);
//   p.connect<^^Point::sum>([](refl::Object& r) { ... });
//   p.on_change<^^Point::x>([](refl::Object& v) { ... });
//   p.emit("custom", 42);
//
// Multi-listener: multiple connect/on_change calls on the same name
// accumulate — all fire.  emit fires all callbacks for a name.
#pragma once

#include <refl/dyn.hpp>

#include <functional>
#include <map>
#include <vector>

namespace refl {

template <typename T>
class Hooks : public Dyn<T> {
    // Hook storage: stable vectors (map nodes are stable).
    std::map<std::string, std::vector<std::function<void(Object&)>>> hooks_;

public:
    using Dyn<T>::Dyn;

    // connect: after-only hook on a method.  Multiple connect calls
    // accumulate.  The callback fires with the method's result Object.
    template <std::meta::info Method>
    void connect(std::function<void(Object&)> cb) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& vec = hooks_[key];
        bool first = vec.empty();
        vec.push_back(std::move(cb));

        if (first) {
            // Wrap the method to fire all hooks after the call.
            if constexpr (params.size() == 0) {
                this->template wrap<Method>(
                    [&vec](auto& orig, Dyn<T>&) -> R {
                        if constexpr (std::is_void_v<R>) {
                            orig();
                            Object empty{};
                            for (auto& cb : vec) cb(empty);
                        } else {
                            R result = orig();
                            Object obj(std::make_shared<R>(result),
                                detail::ensure_class_info<R>());
                            for (auto& cb : vec) cb(obj);
                            return result;
                        }
                    });
            } else if constexpr (params.size() == 1) {
                using P0 = [: std::meta::type_of(params[0]) :];
                using C0 = std::remove_cvref_t<P0>;
                this->template wrap<Method>(
                    [&vec](auto& orig, Dyn<T>& self, C0 a0) -> R {
                        if constexpr (std::is_void_v<R>) {
                            orig(a0);
                            Object empty{};
                            for (auto& cb : vec) cb(empty);
                        } else {
                            R result = orig(a0);
                            Object obj(std::make_shared<R>(result),
                                detail::ensure_class_info<R>());
                            for (auto& cb : vec) cb(obj);
                            return result;
                        }
                    });
            } else if constexpr (params.size() == 2) {
                using P0 = [: std::meta::type_of(params[0]) :];
                using P1 = [: std::meta::type_of(params[1]) :];
                using C0 = std::remove_cvref_t<P0>;
                using C1 = std::remove_cvref_t<P1>;
                this->template wrap<Method>(
                    [&vec](auto& orig, Dyn<T>& self, C0 a0, C1 a1) -> R {
                        if constexpr (std::is_void_v<R>) {
                            orig(a0, a1);
                            Object empty{};
                            for (auto& cb : vec) cb(empty);
                        } else {
                            R result = orig(a0, a1);
                            Object obj(std::make_shared<R>(result),
                                detail::ensure_class_info<R>());
                            for (auto& cb : vec) cb(obj);
                            return result;
                        }
                    });
            }
        }
    }

    // on_change: after-only hook on a property.  Multiple on_change calls
    // accumulate.  The callback fires with the new value as Object.
    // Wraps the property's setter slot via Mockable's prop_slot/set_prop_slot.
    template <std::meta::info Member>
    void on_change(std::function<void(Object&)> cb) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto& vec = hooks_[key];
        bool first = vec.empty();
        vec.push_back(std::move(cb));

        if (first) {
            // Save the current property slot.
            auto saved = this->mockable_->template prop_slot<Member>();

            auto& pwc = prop_wrap_ctxs_[key];
            pwc.saved_getter = saved.getter;
            pwc.saved_get_ctx = saved.get_ctx;
            pwc.saved_setter = saved.setter;
            pwc.saved_set_ctx = saved.set_ctx;
            pwc.saved_get_owner = saved.get_owner;
            pwc.saved_set_owner = saved.set_owner;
            pwc.class_info = detail::ensure_class_info<Bare>();
            pwc.hooks = &vec;

            // The wrapper setter trampoline.
            auto wrapper_setter = [](void* ctx, const Object* val) {
                auto* p = static_cast<PropWrapCtx*>(ctx);
                p->saved_setter(p->saved_set_ctx, val);
                Object current = p->saved_getter
                    ? p->saved_getter(p->saved_get_ctx)
                    : Object{};
                for (auto& cb : *p->hooks) cb(current);
            };

            // Install the wrapper setter, keep the getter as-is.
            this->mockable_->template set_prop_slot<Member>(
                {saved.getter, saved.get_ctx,
                 +wrapper_setter, &pwc, saved.get_owner, {}});
        }
    }

    // emit: fire all callbacks for a name with a value.
    void emit(std::string_view name, Object value = {}) {
        auto key = std::string(name);
        auto it = hooks_.find(key);
        if (it != hooks_.end())
            for (auto& cb : it->second) cb(value);
    }

    // Cross-object connect: when this->name fires, call other->emit(slot).
    template <std::meta::info Method>
    void connect_to(Hooks<T>* other, std::string_view slot_name) {
        auto slot = std::string(slot_name);
        auto* other_ptr = other;
        connect<Method>([other_ptr, slot](Object& r) {
            other_ptr->emit(slot, r);
        });
    }

    // String-based signal connect (for custom signals used with emit).
    // No method is wrapped — the callback only fires via emit().
    void connect_signal(std::string_view name,
                        std::function<void(Object&)> cb) {
        hooks_[std::string(name)].push_back(std::move(cb));
    }

private:
    struct PropWrapCtx {
        GetterFn saved_getter;
        void* saved_get_ctx;
        SetterFn saved_setter;
        void* saved_set_ctx;
        std::shared_ptr<void> saved_get_owner;
        std::shared_ptr<void> saved_set_owner;
        std::shared_ptr<const ClassInfo> class_info;
        std::vector<std::function<void(Object&)>>* hooks;
    };
    std::map<std::string, PropWrapCtx> prop_wrap_ctxs_;
};

}  // namespace refl
