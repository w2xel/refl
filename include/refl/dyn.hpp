// Dynamic state uses owned targets and explicit generation publication.
#pragma once
#include <refl/dynamic/dispatch_table.hpp>
#include <refl/dyn/proxy.hpp>

namespace refl {
template<class T> class Dyn {
    DispatchTable table_{describe_interface<T>()};
    Proxy<T> proxy_{table_.dispatch(), table_.dispatch(OperationKind::read), table_.dispatch(OperationKind::write)};
    static void checked(Result<void> result) { if (!result) throw ReflectionError(result.error()); }
public:
    // A call-scoped backend handle; closures retain only a weak state reference.
    class Self {
        DispatchTable table_;
    public:
        explicit Self(DispatchTable table) : table_(std::move(table)) {}
        template<class U = T> U& get() const {
            auto result = table_.native().template get<U>();
            if (!result) throw ReflectionError(result.error());
            return **result;
        }
        template<std::meta::info M, class F> void implement(F fn, TargetOptions options = {}) {
            using S = [:std::meta::type_of(M):];
            checked(table_.replace(member_id<M>(), detail::require_target(make_target<S>(std::move(fn), options))));
        }
    };
    Dyn() = default; // Interface-only; no implicit native construction.
    explicit Dyn(Object object) { reset(std::move(object)); }
    template<class... A> requires (sizeof...(A) > 0 && std::is_constructible_v<T, A...>)
    explicit Dyn(A&&... args) { reset_native<T>(std::forward<A>(args)...); }
    Dyn(const Dyn&) = delete;
    Dyn& operator=(const Dyn&) = delete;
    Dyn(Dyn&&) noexcept = default;
    Dyn& operator=(Dyn&&) noexcept = default;
    auto* operator->() { return proxy_.operator->(); }
    const auto* operator->() const { return proxy_.operator->(); }
    DispatchHandle dispatch(OperationKind kind = OperationKind::method) const { return table_.dispatch(kind); }
    Proxy<T> capture_binding() const {
        return Proxy<T>(table_.capture(), table_.capture(OperationKind::read), table_.capture(OperationKind::write));
    }
    auto weak() const { return table_.weak(); }
    template<class U = T> U& get() { return Self(table_).template get<U>(); }
    template<class U = T> const U& get() const { return Self(table_).template get<U>(); }
    Class get_class() const { return Class(table_.native().native_descriptor()); }
    bool is_dynamic() const { return !table_.native().valid(); }
    void reset(Object object) {
        Proxy<T> bound(object); // Complete validation before state publication.
        checked(table_.reset(bound.native_targets(), object.view()));
    }
    template<class U, class... A> void reset_native(A&&... args) {
        auto owner = std::make_shared<U>(std::forward<A>(args)...);
        reset(Object(owner, describe_class<U>()));
    }
    void detach_native() { table_.detach_native(); }
    template<std::meta::info M> CallTarget target(OperationKind kind = OperationKind::method) const {
        return detail::require_target(table_.target(member_id<M>(), kind));
    }
    template<std::meta::info M> void replace(CallTarget target, OperationKind kind = OperationKind::method) {
        checked(table_.replace(member_id<M>(), std::move(target), kind));
    }
    template<std::meta::info M> void restore(OperationKind kind = OperationKind::method) {
        checked(table_.restore(member_id<M>(), kind));
    }
    template<std::meta::info M, class F> void implement(F fn, TargetOptions options = {}) {
        using S = [:std::meta::type_of(M):];
        using Args = typename detail::signature_traits<S>::arguments;
        auto weak = table_.weak();
        auto target = [&]<std::size_t... I>(std::index_sequence<I...>) {
            return make_target<S>([weak, fn = std::move(fn)](std::tuple_element_t<I, Args>... args) mutable -> decltype(auto) {
                if constexpr (std::is_invocable_v<F&, Self&, std::tuple_element_t<I, Args>...>) {
                    Self self(weak());
                    return std::invoke(fn, self, std::forward<std::tuple_element_t<I, Args>>(args)...);
                } else return std::invoke(fn, std::forward<std::tuple_element_t<I, Args>>(args)...);
            }, options);
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        replace<M>(detail::require_target(std::move(target)));
    }
    template<std::meta::info M, class F> void wrap(F fn, NativeDependency additional = NativeDependency::unknown) {
        using S = [:std::meta::type_of(M):];
        using R = typename detail::signature_traits<S>::result;
        using Args = typename detail::signature_traits<S>::arguments;
        auto previous = target<M>();
        auto options = previous.options();
        if (options.native_dependency == NativeDependency::native || additional == NativeDependency::native)
            options.native_dependency = NativeDependency::native;
        else if (options.native_dependency != NativeDependency::independent || additional != NativeDependency::independent)
            options.native_dependency = NativeDependency::unknown;
        auto weak = table_.weak();
        auto wrapped = [&]<std::size_t... I>(std::index_sequence<I...>) {
            return make_target<S>([previous, weak, fn = std::move(fn)](std::tuple_element_t<I, Args>... args) mutable -> decltype(auto) {
                auto original = [previous](std::tuple_element_t<I, Args>... values) -> R {
                    std::array<ArgumentView, sizeof...(I)> arguments{detail::native_argument(std::forward<std::tuple_element_t<I, Args>>(values))...};
                    CallFrame frame{previous.options().receiver, arguments};
                    auto result = invoke_target(previous, frame, {}, ExportKind::erased);
                    if (!result) throw ReflectionError(result.error());
                    if constexpr (!std::is_void_v<R>) {
                        auto pointer = result_view(*result).template get<std::remove_reference_t<R>>();
                        if (!pointer) throw ReflectionError(pointer.error());
                        if constexpr (std::is_reference_v<R>) return **pointer;
                        else return R(std::move(**pointer));
                    }
                };
                if constexpr (std::is_invocable_v<F&, decltype(original)&, Self&, std::tuple_element_t<I, Args>...>) {
                    Self self(weak());
                    return std::invoke(fn, original, self, std::forward<std::tuple_element_t<I, Args>>(args)...);
                } else return std::invoke(fn, original, std::forward<std::tuple_element_t<I, Args>>(args)...);
            }, options);
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        replace<M>(detail::require_target(std::move(wrapped)));
    }
    template<std::meta::info M, class G, class S> void implement_property(G get, S set, TargetOptions options = {}) {
        using V = [:std::meta::type_of(M):];
        using U = std::remove_cv_t<V>;
        options.operation = OperationKind::read;
        auto read = detail::require_target(make_target<U() const>(std::move(get), options));
        options.operation = OperationKind::write;
        auto write = detail::require_target(make_target<void(U)>(std::move(set), options));
        replace<M>(std::move(read), OperationKind::read);
        replace<M>(std::move(write), OperationKind::write);
    }
    template<std::meta::info M, class G> void implement_property(G get, TargetOptions options = {}) {
        using V = [:std::meta::type_of(M):];
        options.operation = OperationKind::read;
        replace<M>(detail::require_target(make_target<std::remove_cv_t<V>() const>(std::move(get), options)), OperationKind::read);
    }
    template<std::meta::info M, class V> void set_property(V&& value) {
        using Member = [:std::meta::type_of(M):];
        using U = std::remove_cv_t<Member>;
        auto stored = std::make_shared<U>(std::forward<V>(value));
        TargetOptions options{.native_dependency = NativeDependency::independent};
        if constexpr (!std::is_const_v<Member> && std::is_copy_constructible_v<U>)
            implement_property<M>([stored] { return *stored; }, [stored](U next) { *stored = std::move(next); }, options);
        else if constexpr (std::is_copy_constructible_v<U>) implement_property<M>([stored] { return *stored; }, options);
        else if constexpr (!std::is_const_v<Member>) {
            options.operation = OperationKind::write;
            replace<M>(detail::require_target(make_target<void(U)>([stored](U next) { *stored = std::move(next); }, options)), OperationKind::write);
        }
        options.operation = OperationKind::view;
        options.result_lifetime = ResultLifetime::callable_context;
        replace<M>(detail::require_target(make_target<const U&() const>([stored]() -> const U& { return *stored; }, options)), OperationKind::view);
    }

};
}
