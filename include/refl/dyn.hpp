// dyn — typed dispatch and dynamic-implementation layer for the refl core.
//
// Built on top of refl/refl.hpp (which provides the type-erased reflection
// pool) and refl/dyn/proxy.hpp (which provides Proxy<T> and the typed proxy
// field types).  Dyn<T> extends Proxy<T>'s dispatch mechanism with runtime
// method implementation (mocking), Qt-style hooks (connect / on_change /
// emit), and dynamic properties.
//
//   refl::Dyn<Point> p(1, 2);
//   p->set(10, 20);        // overload resolved by argument type
//   int s = p->sum();       // real return type
//   p->x = 42;              // member-like assignment
//
// This layer is experimental and likely to change.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <any>
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
            constexpr bool add_obj = !std::meta::is_abstract_type(^^T);
            std::vector<std::meta::info> specs;
            detail::make_dispatch_specs(^^T, add_obj, specs);
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
                        // Non-static member function → bind from ClassInfo,
                        // matching by param-type signature.  Searches the
                        // base hierarchy for inherited methods.
                        using TM = std::remove_cv_t<FieldType>;
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        auto& vec = overload_storage_[key];
                        std::ptrdiff_t method_off = 0;
                        for (const auto& exp : TM::expected_param_types()) {
                            auto r = detail::find_function_in_hierarchy(
                                info, key, exp);
                            if (r.fi) {
                                vec.push_back({r.fi->invoker});
                                method_off = r.offset;
                            }
                        }
                        dispatch_.[:field:].obj =
                            static_cast<char*>(static_cast<void*>(dispatch_.obj.get())) + method_off;
                        dispatch_.[:field:].overloads = vec.data();
                        dispatch_.[:field:].num = vec.size();
                    } else {
                        // Non-static data member → bind from ClassInfo.
                        // Searches the base hierarchy for inherited fields.
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
                        constexpr auto nm_sv = std::meta::identifier_of(field);
                        auto key = std::string(nm_sv);
                        auto r = detail::find_field_in_hierarchy(info, key);
                        if (r.fi) {
                            dispatch_.[:field:].obj =
                                static_cast<char*>(static_cast<void*>(dispatch_.obj.get())) + r.offset;
                            dispatch_.[:field:].member_offset =
                                static_cast<std::size_t>(r.fi->offset);
                            dispatch_.[:field:].getter = r.fi->getter;
                            dispatch_.[:field:].setter = r.fi->setter;
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
                        using TM = std::remove_cv_t<FT>;
                        dispatch_.[:field:].obj = dispatch_.obj.get();
                        dispatch_.[:field:].owner =
                            std::shared_ptr<void>(dispatch_.obj);
                        auto key = std::string(nm);
                        auto& vec = overload_storage_[key];
                        vec.clear();
                        for (const auto& exp : TM::expected_param_types())
                            for (const auto& fi : info->functions)
                                if (fi.name == key
                                    && fi.param_types == exp) {
                                    vec.push_back({fi.invoker});
                                    break;
                                }
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
    Dyn& operator=(const Dyn&) = delete;
    Dyn(Dyn&&) = delete;
    Dyn& operator=(Dyn&&) = delete;

    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }

    T& get() requires (!std::meta::is_abstract_type(^^T)) { return *dispatch_.obj; }
    const T& get() const requires (!std::meta::is_abstract_type(^^T)) { return *dispatch_.obj; }

    // Check if this Dyn is in dynamic (runtime-implemented) mode.
    bool is_dynamic() const { return dynamic_mode_; }

    // The Class of T (the bound type).  Statics are class-level, not
    // instance-level — access them through this Class instead of the
    // dispatch struct.  Returns an invalid Class if T is unregistered.
    Class get_class() const {
        return find_class(detail::type_name<T>()).value_or(Class{});
    }

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
                if constexpr (!detail::is_typed_method_v<std::remove_cv_t<FT>>) {
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
