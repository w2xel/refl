// mockable — runtime mock with swappable per-method and per-property slots.
//
// Mockable<T> synthesizes a per-instance ClassInfo whose method invokers
// and property getter/setters are static trampolines that read swappable
// slots at call time.  The core API is slot/set_slot (methods) and
// prop_slot/set_prop_slot (properties) — raw access to the swappable
// (InvokerFn, void* ctx) pairs.  implement(), implement_property(), and
// set_property() are sugar over the raw slot API.
//
//   auto m = refl::Mockable<IWidget>::create();
//   m->implement<^^IWidget::draw>([]() { ... });
//   m->set_property<^^IWidget::width>(42);
//   auto p = m->proxy();
//
// Hybrid mode: initialize slots with real invokers from outside, then
// override individual methods via implement().  Restore by saving the
// original slot and calling set_slot() with it.
//
//   auto saved = m->slot<^^T::method>();
//   m->implement<^^T::method>([](int x) { return x * 2; });
//   m->set_slot<^^T::method>(saved);  // restore
//
// Hooks are user-side: wrap your lambda before passing it to implement
// or implement_property.
#pragma once

#include <refl/refl.hpp>
#include <refl/dyn/proxy.hpp>

#include <functional>
#include <map>

namespace refl {

// Swappable invoker slot for methods. Copies retain an installed callable's
// context; manually supplied raw contexts remain caller-owned if owner is empty.
struct MethodSlot {
    InvokerFn invoker = nullptr;
    void* ctx = nullptr;
    std::shared_ptr<void> owner = {};
};

// Swappable getter/setter slot for properties.
struct PropertySlot {
    GetterFn getter = nullptr;
    void* get_ctx = nullptr;
    SetterFn setter = nullptr;
    void* set_ctx = nullptr;
    std::shared_ptr<void> get_owner = {};
    std::shared_ptr<void> set_owner = {};
};

template <typename T>
class Mockable : public std::enable_shared_from_this<Mockable<T>> {

    // ======================================================================
    // Method trampolines
    // ======================================================================

    template <std::meta::info Method>
    consteval static const char* method_key() {
        // Preserve the complete function type so overloads do not share a slot.
        return std::define_static_string(
            std::string(std::meta::identifier_of(Method)) + ":" +
            std::string(std::meta::display_string_of(std::meta::type_of(Method))));
    }

    template <std::meta::info Method>
    static Object slot_trampoline(const std::shared_ptr<void>& owner,
                                   void* obj, const Object* args) {
        auto* self = static_cast<Mockable<T>*>(obj);
        auto key = std::string(method_key<Method>());
        auto it = self->method_slots_.find(key);
        if (it == self->method_slots_.end() || !it->second.invoker)
            throw std::runtime_error(
                "Mockable: method '" + key + "' not implemented");
        auto selected = it->second;  // retain the callable during self-replacement
        return selected.invoker(selected.owner ? selected.owner : owner,
                                selected.ctx, args);
    }

    // Use ordinary signature types for implementation thunks. GCC 16.2 can
    // emit duplicate symbols for reflection-NTTP thunks instantiated through
    // repeated implement<Method, F> calls with different lambda types.
    template <typename R, typename... Args>
    static Object impl_trampoline(const std::shared_ptr<void>& owner,
                                  void* ctx, const Object* args) {
        auto& fn = *static_cast<std::function<R(Args...)>*>(ctx);
        return [&]<std::size_t... I>(std::index_sequence<I...>) -> Object {
            if constexpr (std::is_void_v<R>) {
                fn(detail::extract_arg<Args>(args, I)...);
                return Object{};
            } else if constexpr (std::is_lvalue_reference_v<R>) {
                auto& value = fn(detail::extract_arg<Args>(args, I)...);
                auto* pointer = const_cast<void*>(static_cast<const void*>(std::addressof(value)));
                Object result(owner, pointer, detail::ensure_class_info<std::remove_cvref_t<R>>());
                if constexpr (std::is_const_v<std::remove_reference_t<R>>) return result.as_const();
                else return result;
            } else {
                return Object(std::make_shared<R>(
                    fn(detail::extract_arg<Args>(args, I)...)),
                    detail::ensure_class_info<R>());
            }
        }(std::index_sequence_for<Args...>{});
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
        auto selected = it->second;
        return selected.getter(selected.get_ctx);
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
        auto selected = it->second;
        selected.setter(selected.set_ctx, val);
    }

    // Call the user's getter lambda.  ctx points at the std::function.
    template <std::meta::info Member>
    static Object prop_get_impl(void* ctx) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        auto& fn = *static_cast<std::function<Bare()>*>(ctx);
        return Object(std::make_shared<Bare>(fn()),
                     detail::ensure_class_info<Bare>());
    }

    // Call the user's setter lambda.  ctx points at the std::function.
    template <std::meta::info Member>
    static void prop_set_impl(void* ctx, const Object* val) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        auto& fn = *static_cast<std::function<void(Bare)>*>(ctx);
        auto cr = val->template cast_ref<Bare>();
        if (!cr) throw std::runtime_error(
            "Mockable: property type mismatch on set");
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
        ci->identity = type_id<Mockable<T>>();
        ci->make_view = &reflect_detail::native_view<Mockable<T>>;
        if constexpr (std::is_class_v<T>) {
            static constexpr auto all_members = []() consteval {
                std::vector<std::meta::info> members;
                detail::collect_instance_members(^^T, members);
                return std::define_static_array(members);
            }();
            template for (constexpr auto m : all_members) {
                if constexpr (detail::is_public_method(m)
                              && std::meta::has_identifier(m)
                              && !std::meta::is_static_member(m)) {
                    FunctionInfo fi;
                    fi.name = std::string(std::meta::identifier_of(m));
                    fi.is_const = detail::is_const_method(m);
                    using S = [:std::meta::type_of(m):];
                    fi.signature = signature_of<S>();
                    fi.member = member_id<m>();
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
                    fi.signature.result = type_use<MemberType>();
                    fi.member = member_id<m>();
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
    std::map<std::string, MethodSlot> method_slots_;
    std::map<std::string, PropertySlot> prop_slots_;

    Mockable() : class_info_(make_class_info()) {}

public:
    static std::shared_ptr<Mockable<T>> create() {
        return std::shared_ptr<Mockable<T>>(new Mockable<T>());
    }

    void clear_slots() {
        method_slots_.clear();
        prop_slots_.clear();
    }

    // ======================================================================
    // Raw slot API — get/set the swappable (InvokerFn, ctx) pairs.
    // This is the low-level surface.  implement() and implement_property()
    // are sugar over set_slot()/set_prop_slot().  Hybrid mode: initialize
    // slots with real invokers from outside, override via implement(),
    // restore by saving and re-setting the original slot.
    // ======================================================================

    // Get the current method slot (by method reflection).
    template <std::meta::info Method>
    MethodSlot slot() const {
        auto it = method_slots_.find(method_key<Method>());
        return it != method_slots_.end() ? it->second : MethodSlot{};
    }

    // Set the method slot (raw InvokerFn + ctx).
    template <std::meta::info Method>
    void set_slot(MethodSlot s) {
        method_slots_[method_key<Method>()] = s;
    }

    // Get the current property slot (by member reflection).
    template <std::meta::info Member>
    PropertySlot prop_slot() const {
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto it = prop_slots_.find(std::string(nm));
        return it != prop_slots_.end() ? it->second : PropertySlot{};
    }

    // Set the property slot (raw getter/setter + ctxs).
    template <std::meta::info Member>
    void set_prop_slot(PropertySlot s) {
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        prop_slots_[std::string(nm)] = s;
    }

    // ======================================================================
    // Method API
    // ======================================================================

    template <std::meta::info Method, typename F>
    void implement(F fn) {
        using S = [:std::meta::type_of(Method):];
        using Traits = detail::signature_traits<S>;
        using R = typename Traits::result;
        using Args = typename Traits::arguments;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            auto holder = std::make_shared<std::function<R(std::tuple_element_t<I, Args>...)>>(std::move(fn));
            method_slots_[method_key<Method>()] = {
                &impl_trampoline<R, std::tuple_element_t<I, Args>...>, holder.get(), holder};
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
    }

    // ======================================================================
    // Property API
    // ======================================================================

    // Virtual property: user provides getter and setter lambdas.
    // The getter must return the property type; the setter takes it by
    // value.  For read-only properties, use the one-argument overload.
    template <std::meta::info Member, typename G, typename S>
    void implement_property(G getter, S setter) {
        using MemberType = [:std::meta::type_of(Member):];
        using Bare = std::remove_cvref_t<MemberType>;
        constexpr auto nm_sv = std::meta::identifier_of(Member);
        constexpr auto nm = std::define_static_string(nm_sv);
        auto key = std::string(nm);
        auto gh = std::make_shared<std::function<Bare()>>(std::move(getter));
        auto sh = std::make_shared<std::function<void(Bare)>>(std::move(setter));
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), gh.get(),
            get_prop_set_impl<Member>(), sh.get(), gh, sh
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
        auto gh = std::make_shared<std::function<Bare()>>(std::move(getter));
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), gh.get(),
            nullptr, nullptr, gh, {}
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
        auto gh = std::make_shared<std::function<Bare()>>(
            [stored] { return *stored; });
        auto sh = std::make_shared<std::function<void(Bare)>>(
            [stored](Bare v) { *stored = std::move(v); });
        prop_slots_[key] = {
            get_prop_get_impl<Member>(), gh.get(),
            get_prop_set_impl<Member>(), sh.get(), gh, sh
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
