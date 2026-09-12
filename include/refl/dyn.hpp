// dyn — typed dispatch and dynamic-implementation layer for the refl core.
//
// Built on top of refl/refl.hpp (type-erased reflection pool),
// refl/dyn/proxy.hpp (Proxy<T> typed dispatch), and
// refl/mockable.hpp (Mockable<T> runtime mock with swappable slots).
//
// Dyn<T> holds a shared_ptr<Mockable<T>> and, for concrete T, a
// shared_ptr<T>.  The Mockable's Object is bound to a Proxy<T>, and
// operator->() returns the Proxy's dispatch struct.  implement() wraps
// the user's lambda to capture a shared_ptr<Dyn<T>> as self, then
// delegates to Mockable::implement().  restore() saves the slot and
// re-sets it.  Hooks are user-side: wrap your lambda.
//
//   refl::Dyn<Point> p(1, 2);
//   p->set(10, 20);        // overload resolved by argument type
//   int s = p->sum();       // real return type
//
//   refl::Dyn<IShape> s;   // abstract — no real object
//   s.implement<^^IShape::area>([](refl::Dyn<IShape>&, int scale) {
//       return scale * 100;
//   });
//   int a = s->area(5);
//
// This layer is experimental and likely to change.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>
#include <refl/mockable.hpp>

#include <functional>
#include <map>

namespace refl {

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
// Dyn<T> — typed dispatch built on Mockable + Proxy.
//
// Dyn owns a Mockable<T> (for slot management) and optionally a shared_ptr<T>
// (for concrete mode).  operator->() returns the Proxy's dispatch struct,
// so calls flow: Proxy dispatch → Mockable slot trampoline → user lambda or
// real invoker.
//
// Dyn<T> is non-copyable.  Movable: the move updates the SelfRef back-
// pointer so existing lambdas point at the new address.
// ---------------------------------------------------------------------------
template <typename T>
class Dyn {
    std::shared_ptr<Mockable<T>> mockable_;
    std::shared_ptr<T> obj_;
    bool dynamic_mode_ = false;
    Proxy<T> proxy_;

    // Wire real invoker slots from T's ClassInfo into the Mockable.
    // Each method slot is set to {real_invoker, T*}.  Property slots
    // are set to {real_getter, T*} / {real_setter, T*}.
    void wire_real_slots() {
        if (!obj_) return;
        const ClassInfo* info = detail::lookup_class_info(
            detail::type_name<T>()).get();
        if (!info) return;
        T* raw = obj_.get();

        // Walk T's members via reflection, match against ClassInfo.
        static constexpr auto all_members = std::define_static_array(
            std::meta::members_of(^^T,
                std::meta::access_context::unchecked()));
        template for (constexpr auto m : all_members) {
            if constexpr (detail::is_public_method(m)
                          && std::meta::has_identifier(m)
                          && !std::meta::is_static_member(m)) {
                constexpr auto nm_sv = std::meta::identifier_of(m);
                constexpr auto nm = std::define_static_string(nm_sv);
                auto key = std::string(nm);
                auto r = detail::find_function_in_hierarchy(
                    info, key,
                    // Build param_types from the method's params.
                    [&]() {
                        std::vector<std::string> pts;
                        static constexpr auto fparams =
                            std::define_static_array(
                                std::meta::parameters_of(m));
                        template for (constexpr auto p : fparams) {
                            pts.emplace_back(detail::normalize_type(
                                std::meta::display_string_of(
                                    std::meta::type_of(p))));
                        }
                        return pts;
                    }());
                if (r.fi) {
                    // Save the real invoker as the method slot.
                    // The real invoker expects T* as obj.
                    mockable_->template set_slot<m>(
                        {r.fi->invoker, raw});
                }
            } else if constexpr (detail::is_public_data_member(m)
                          && std::meta::has_identifier(m)
                          && !std::meta::is_static_member(m)) {
                constexpr auto nm_sv = std::meta::identifier_of(m);
                constexpr auto nm = std::define_static_string(nm_sv);
                auto key = std::string(nm);
                auto r = detail::find_field_in_hierarchy(info, key);
                if (r.fi) {
                    mockable_->template set_prop_slot<m>(
                        {r.fi->getter, raw,
                         r.fi->setter, raw});
                }
            }
        }
    }

    // Rebuild the proxy from the Mockable's current Object.
    void rebuild_proxy() {
        proxy_ = mockable_->proxy();
    }

public:
    using value_type = T;

    Dyn() {
        ensure_registered<T>();
        mockable_ = Mockable<T>::create();
        rebuild_proxy();
    }

    template <typename... Args>
    explicit Dyn(Args&&... args) {
        ensure_registered<T>();
        mockable_ = Mockable<T>::create();
        if constexpr (!std::meta::is_abstract_type(^^T)) {
            obj_ = std::make_shared<T>(std::forward<Args>(args)...);
            wire_real_slots();
        }
        rebuild_proxy();
    }

    Dyn& operator=(const Dyn&) = delete;
    Dyn(Dyn&& other) noexcept
        : mockable_(std::move(other.mockable_))
        , obj_(std::move(other.obj_))
        , dynamic_mode_(other.dynamic_mode_)
        , proxy_(std::move(other.proxy_))
        , self_ref_(std::move(other.self_ref_)) {
        if (self_ref_) self_ref_->dyn = this;
    }
    Dyn& operator=(Dyn&& other) noexcept {
        if (this != &other) {
            mockable_ = std::move(other.mockable_);
            obj_ = std::move(other.obj_);
            dynamic_mode_ = other.dynamic_mode_;
            proxy_ = std::move(other.proxy_);
            self_ref_ = std::move(other.self_ref_);
            if (self_ref_) self_ref_->dyn = this;
        }
        return *this;
    }

    // --- dispatch via Proxy ---
    auto* operator->() { return proxy_.operator->(); }
    const auto* operator->() const { return proxy_.operator->(); }

    // --- real-object access ---
    T& get() requires (!std::meta::is_abstract_type(^^T)) { return *obj_; }
    const T& get() const requires (!std::meta::is_abstract_type(^^T)) {
        return *obj_;
    }

    bool is_dynamic() const { return dynamic_mode_; }

    Class get_class() const {
        return proxy_.get_class();
    }

    // --- reset: swap real object, re-wire slots ---
    template <typename... Args>
    void reset(Args&&... args)
        requires (!std::meta::is_abstract_type(^^T)) {
        obj_ = std::make_shared<T>(std::forward<Args>(args)...);
        dynamic_mode_ = false;
        wire_real_slots();
        rebuild_proxy();
    }

    // --- make_dynamic: drop real object, mock-only ---
    void make_dynamic() {
        dynamic_mode_ = true;
        obj_.reset();
        // Clear all slots — implement() will set them.
        // ponytail: no per-slot clear; implement() overwrites.
    }

    // --- implement: override a method with a runtime callable ---
    // The lambda receives Dyn<T>& as its first argument (self-reference).
    // Dyn captures a shared_ptr to a control block holding a raw pointer
    // to itself.  Since Dyn is non-movable, the raw pointer is stable.
    // The shared_ptr keeps the control block alive (not the Dyn); the
    // control block's deleter is a no-op so it doesn't delete the stack-
    // allocated Dyn.
    struct SelfRef {
        Dyn<T>* dyn;
    };
    std::shared_ptr<SelfRef> self_ref_;

    std::shared_ptr<SelfRef> make_self_ref() {
        if (!self_ref_)
            self_ref_ = std::shared_ptr<SelfRef>(new SelfRef{this});
        return self_ref_;
    }

    template <std::meta::info Method, typename F>
    void implement(F fn) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        auto self = make_self_ref();

        if constexpr (params.size() == 0) {
            mockable_->template implement<Method>(
                [self = std::move(self), fn = std::move(fn)]() mutable -> R {
                    return fn(*self->dyn);
                });
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            mockable_->template implement<Method>(
                [self = std::move(self), fn = std::move(fn)](
                    C0 a0) mutable -> R {
                    return fn(*self->dyn, a0);
                });
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            mockable_->template implement<Method>(
                [self = std::move(self), fn = std::move(fn)](
                    C0 a0, C1 a1) mutable -> R {
                    return fn(*self->dyn, a0, a1);
                });
        }
        // ponytail: 3+ args not yet supported.
    }

    // String-based implement (no ^^ syntax, compile-time checked).
    template <FixedString Name, typename F>
    void implement(F fn) {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            constexpr bool found = []() consteval {
                for (auto m : std::meta::members_of(^^T,
                        std::meta::access_context::unchecked())) {
                    if (detail::is_public_method(m)
                        && std::meta::has_identifier(m)
                        && !std::meta::is_static_member(m)
                        && std::meta::identifier_of(m) == Name.sv())
                        return true;
                }
                return false;
            }();
            static_assert(found, "implement: method not found on T");
            template for (constexpr auto m : members) {
                if constexpr (detail::is_public_method(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)
                              && std::meta::identifier_of(m) == Name.sv()) {
                    implement<m>(std::move(fn));
                }
            }
        }
    }

    // --- restore: remove an override, go back to the real invoker ---
    template <std::meta::info Method>
    void restore() requires (!std::meta::is_abstract_type(^^T)) {
        if (dynamic_mode_) return;  // can't restore in full dynamic mode
        if (!obj_) return;
        // Re-wire just this method from ClassInfo.
        const ClassInfo* info = detail::lookup_class_info(
            detail::type_name<T>()).get();
        if (!info) return;
        constexpr auto nm_sv = std::meta::identifier_of(Method);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto r = detail::find_function_in_hierarchy(info, key,
            [&]() {
                std::vector<std::string> pts;
                static constexpr auto fparams = std::define_static_array(
                    std::meta::parameters_of(Method));
                template for (constexpr auto p : fparams) {
                    pts.emplace_back(detail::normalize_type(
                        std::meta::display_string_of(
                            std::meta::type_of(p))));
                }
                return pts;
            }());
        if (r.fi) {
            mockable_->template set_slot<Method>(
                {r.fi->invoker, obj_.get()});
        }
    }

    // String-based restore.
    template <FixedString Name>
    void restore() requires (!std::meta::is_abstract_type(^^T)) {
        if constexpr (std::is_class_v<T>) {
            static constexpr auto members = std::define_static_array(
                std::meta::members_of(^^T,
                    std::meta::access_context::unchecked()));
            template for (constexpr auto m : members) {
                if constexpr (detail::is_public_method(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)
                              && std::meta::identifier_of(m) == Name.sv()) {
                    restore<m>();
                }
            }
        }
    }

    static const Registrar& registrar() {
        return RegistrarHolder<T>::registrar;
    }
};

}  // namespace refl
