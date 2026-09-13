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

#include <any>
#include <functional>

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
protected:
    std::shared_ptr<Mockable<T>> mockable_;
private:
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
        static constexpr auto all_members = []() consteval {
            std::vector<std::meta::info> members;
            detail::collect_instance_members(^^T, members);
            return std::define_static_array(members);
        }();
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
                    // Inherited invokers expect their declaring base subobject.
                    auto* receiver = static_cast<char*>(static_cast<void*>(raw)) + r.offset;
                    mockable_->template set_slot<m>(
                        {r.fi->invoker, receiver, obj_});
                }
            } else if constexpr (detail::is_public_data_member(m)
                          && std::meta::has_identifier(m)
                          && !std::meta::is_static_member(m)) {
                constexpr auto nm_sv = std::meta::identifier_of(m);
                constexpr auto nm = std::define_static_string(nm_sv);
                auto key = std::string(nm);
                auto r = detail::find_field_in_hierarchy(info, key);
                if (r.fi) {
                    auto* receiver = static_cast<char*>(static_cast<void*>(raw)) + r.offset;
                    mockable_->template set_prop_slot<m>(
                        {r.fi->getter, receiver,
                         r.fi->setter, receiver, obj_, obj_});
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
        // Class-level statics belong to T, not its synthetic dispatch metadata.
        return Class(detail::lookup_class_info(detail::type_name<T>()));
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
        // Clearing the slots drops their native owners and invalidates dispatch
        // until implement() supplies replacements. Active calls retain a copy.
        mockable_->clear_slots();
        obj_.reset();
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

    // --- wrap: wrap the current invoker with a user lambda ---
    // The lambda receives: an invocable `original` (callable with the
    // method's arguments, returns the method's return type), the Dyn<T>&
    // self-reference, and the method's arguments.
    //
    //   p.wrap<^^T::method>([](auto& orig, Dyn<T>& self, int x) {
    //       // pre-hook
    //       auto r = orig(x);
    //       // post-hook
    //       return r;
    //   });
    //
    // `original` boxes the typed args into Objects and calls the saved
    // invoker with the saved ctx.  Works after Proxy bind.
    struct WrapCtx {
        InvokerFn saved_invoker;
        void* saved_ctx;
        std::shared_ptr<void> saved_owner;
        std::shared_ptr<SelfRef> self_ref;
        std::any wrap_fn;  // std::function<R(Orig, Dyn<T>&, Args...)>
    };

    template <std::meta::info Method>
    static Object wrap_trampoline(const std::shared_ptr<void>& owner,
                                    void* ctx, const Object* args) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));
        auto* wc = static_cast<WrapCtx*>(ctx);

        auto& dyn = *wc->self_ref->dyn;

        if constexpr (params.size() == 0) {
            using Orig = std::function<R()>;
            auto orig = Orig([owner = wc->saved_owner ? wc->saved_owner : owner,
                              sv = wc->saved_invoker,
                              sc = wc->saved_ctx]() -> R {
                Object result = sv(owner, sc, nullptr);
                if constexpr (std::is_void_v<R>) return;
                else {
                    auto cr = result.template cast_ref<std::remove_cvref_t<R>>();
                    if (!cr) throw std::runtime_error("wrap: return type mismatch");
                    return R(std::move(*cr.value()));
                }
            });
            using Fn = std::function<R(Orig&, Dyn<T>&)>;
            auto& fn = std::any_cast<Fn&>(wc->wrap_fn);
            if constexpr (std::is_void_v<R>) {
                fn(orig, dyn);
                return Object{};
            } else {
                return Object(std::make_shared<R>(fn(orig, dyn)),
                    detail::ensure_class_info<R>());
            }
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            auto& a0 = *static_cast<C0*>(args[0].raw());
            using Orig = std::function<R(C0)>;
            auto orig = Orig([owner = wc->saved_owner ? wc->saved_owner : owner,
                              sv = wc->saved_invoker,
                              sc = wc->saved_ctx](C0 x) -> R {
                C0 storage(std::move(x));
                Object ref(storage);
                Object result = sv(owner, sc, &ref);
                if constexpr (std::is_void_v<R>) return;
                else {
                    auto cr = result.template cast_ref<std::remove_cvref_t<R>>();
                    if (!cr) throw std::runtime_error("wrap: return type mismatch");
                    return R(std::move(*cr.value()));
                }
            });
            using Fn = std::function<R(Orig&, Dyn<T>&, C0)>;
            auto& fn = std::any_cast<Fn&>(wc->wrap_fn);
            if constexpr (std::is_void_v<R>) {
                fn(orig, dyn, a0);
                return Object{};
            } else {
                return Object(std::make_shared<R>(fn(orig, dyn, a0)),
                    detail::ensure_class_info<R>());
            }
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            auto& a0 = *static_cast<C0*>(args[0].raw());
            auto& a1 = *static_cast<C1*>(args[1].raw());
            using Orig = std::function<R(C0, C1)>;
            auto orig = Orig([owner = wc->saved_owner ? wc->saved_owner : owner,
                              sv = wc->saved_invoker,
                              sc = wc->saved_ctx](C0 x0, C1 x1) -> R {
                C0 s0(std::move(x0));
                C1 s1(std::move(x1));
                std::array<Object, 2> refs = {Object(s0), Object(s1)};
                Object result = sv(owner, sc, refs.data());
                if constexpr (std::is_void_v<R>) return;
                else {
                    auto cr = result.template cast_ref<std::remove_cvref_t<R>>();
                    if (!cr) throw std::runtime_error("wrap: return type mismatch");
                    return R(std::move(*cr.value()));
                }
            });
            using Fn = std::function<R(Orig&, Dyn<T>&, C0, C1)>;
            auto& fn = std::any_cast<Fn&>(wc->wrap_fn);
            if constexpr (std::is_void_v<R>) {
                fn(orig, dyn, a0, a1);
                return Object{};
            } else {
                return Object(std::make_shared<R>(fn(orig, dyn, a0, a1)),
                    detail::ensure_class_info<R>());
            }
        } else {
            // ponytail: 3+ args not yet supported.
            return Object{};
        }
    }

    // consteval helper for the wrap trampoline address.
    template <std::meta::info Method>
    consteval static InvokerFn get_wrap_tramp() {
        return &wrap_trampoline<Method>;
    }

    template <std::meta::info Method, typename F>
    void wrap(F fn) {
        using R = [: std::meta::return_type_of(Method) :];
        constexpr auto params = std::define_static_array(
            std::meta::parameters_of(Method));

        // Save the current slot.
        auto saved = mockable_->template slot<Method>();

        // Each wrapper owns a distinct context and the previous slot's owner.
        // Reusing one map entry would make a second wrap point back to itself.
        auto context = std::make_shared<WrapCtx>();
        auto& wc = *context;
        wc.saved_invoker = saved.invoker;
        wc.saved_ctx = saved.ctx;
        wc.saved_owner = saved.owner;
        wc.self_ref = make_self_ref();

        // Store the user's lambda, typed by arity.  The first arg is
        // std::function<R(Args...)> — the callable `original`.
        if constexpr (params.size() == 0) {
            using Orig = std::function<R()>;
            wc.wrap_fn = std::function<R(Orig&, Dyn<T>&)>(
                [fn = std::move(fn)](Orig& orig, Dyn<T>& self) -> R {
                    return fn(orig, self);
                });
        } else if constexpr (params.size() == 1) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using C0 = std::remove_cvref_t<P0>;
            using Orig = std::function<R(C0)>;
            wc.wrap_fn = std::function<R(Orig&, Dyn<T>&, C0)>(
                [fn = std::move(fn)](Orig& orig, Dyn<T>& self, C0 a0) -> R {
                    return fn(orig, self, a0);
                });
        } else if constexpr (params.size() == 2) {
            using P0 = [: std::meta::type_of(params[0]) :];
            using P1 = [: std::meta::type_of(params[1]) :];
            using C0 = std::remove_cvref_t<P0>;
            using C1 = std::remove_cvref_t<P1>;
            using Orig = std::function<R(C0, C1)>;
            wc.wrap_fn = std::function<R(Orig&, Dyn<T>&, C0, C1)>(
                [fn = std::move(fn)](Orig& orig, Dyn<T>& self, C0 a0, C1 a1) -> R {
                    return fn(orig, self, a0, a1);
                });
        }

        // Install the wrap trampoline as the new slot.
        mockable_->template set_slot<Method>(
            {get_wrap_tramp<Method>(), &wc, context});
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
            auto* receiver = static_cast<char*>(static_cast<void*>(obj_.get())) + r.offset;
            mockable_->template set_slot<Method>(
                {r.fi->invoker, receiver, obj_});
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
