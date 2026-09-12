// mockable — runtime mock implementation of an interface, built bottom-up
// from a synthetic ClassInfo owned by the Mockable instance.
//
// Mockable<T> synthesizes a ClassInfo for T's public interface at compile
// time (reusing proxy.hpp's reflection walkers) and owns it via a
// shared_ptr.  It is NOT registered in the global pool — the Mockable
// owns the ClassInfo directly, and it dies with the Mockable (or with
// the last Object referencing it).  implement<^^T::method>(lambda)
// wires a runtime callable; calls through Proxy<T> dispatch to the
// trampoline, which recovers the Mockable<T>* from the Object's raw
// pointer and calls the stored lambda.
//
//   auto m = refl::Mockable<IShape>::create();
//   m->implement<^^IShape::area>([](int scale) { return scale * 100; });
//   refl::Proxy<IShape> p = m->proxy();
//   int a = p->area(5);  // 500
//
// Prototype: no hooks, no property mocking, no self-ref, 0-2 args.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <any>
#include <functional>
#include <map>

namespace refl {

template <typename T>
class Mockable : public std::enable_shared_from_this<Mockable<T>> {
    template <std::meta::info Method>
    static Object trampoline(const std::shared_ptr<void>&,
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
            // ponytail: 3+ args not yet supported in the trampoline.
            return Object{};
        }
    }

    // Build a synthetic ClassInfo for T's public interface.  Per-instance:
    // each Mockable owns its own ClassInfo via shared_ptr.  Not added to
    // the global pool — the Object carries the shared_ptr directly.  No
    // bases, no constructors, no clone — a mock is not a real T and
    // cast_safe<T>() correctly fails (no base offset).
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
                    fi.invoker = &trampoline<m>;
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
    }

    // Create an owning Object whose raw pointer is this Mockable and whose
    // class_info is the Mockable's own synthetic ClassInfo.  Proxy<T>
    // binds to this Object normally — populate() uses the ClassInfo
    // directly (no pool lookup) and wires the TypedMethod fields to the
    // trampolines.  The Object's shared_ptr keeps both the Mockable and
    // the ClassInfo alive.
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
