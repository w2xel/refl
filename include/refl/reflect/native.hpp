#pragma once
#include <refl/core/descriptor.hpp>
#include <meta>

namespace refl {
template<class T> std::shared_ptr<const ClassInfo> describe_native();
namespace reflect_detail {
template<std::meta::info M> struct reflected_type { using type = [:M:]; };
template<class T> consteval std::string_view type_name() { return std::meta::display_string_of(^^T); }
consteval bool immediate(std::meta::info m) {
    auto name = std::meta::display_string_of(m);
    if (name.starts_with("static ")) name.remove_prefix(7);
    return name.starts_with("consteval ");
}
consteval bool public_method(std::meta::info m) {
    return std::meta::is_function(m) && std::meta::is_public(m) &&
        !std::meta::is_deleted(m) && !immediate(m);
}
template<class T> ObjectView native_view(void* pointer, LifetimeAnchor owner, bool readonly) {
    auto view = ObjectView::from(*static_cast<T*>(pointer), std::move(owner));
    return readonly ? view.as_const() : view;
}
template<class T> std::shared_ptr<const ClassInfo> value_descriptor() {
    using U = std::remove_cvref_t<T>;
    static const auto descriptor = [] {
        ClassInfo info;
        info.name = std::string(type_name<U>());
        info.identity = type_id<U>();
        if constexpr (!std::is_void_v<U>) info.make_view = &native_view<U>;
        return std::make_shared<const ClassInfo>(std::move(info));
    }();
    return descriptor;
}
template<std::meta::info M> consteval std::string_view name() {
    if constexpr (std::meta::has_identifier(M)) return std::meta::identifier_of(M);
    else return std::define_static_string(std::string("operator") +
        std::string(std::meta::symbol_of(std::meta::operator_of(M))));
}
}
template<std::meta::info M> consteval MemberId member_id() {
    constexpr auto parent = std::meta::parent_of(M);
    using Owner = [:parent:];
    auto members = std::meta::members_of(parent, std::meta::access_context::unchecked());
    for (std::size_t i = 0; i < members.size(); ++i)
        if (members[i] == M) return {type_id<Owner>(), i};
    throw "member has no declaration index";
}
namespace reflect_detail {
template<class T, std::meta::info M> NativeOperation method() {
    using S = [:std::meta::type_of(M):];
    using Traits = detail::signature_traits<S>;
    using R = typename Traits::result;
    using Args = typename Traits::arguments;
    NativeOperation op;
    op.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<S>()};
    op.result_descriptor = &value_descriptor<R>;
    op.bind = [](ObjectView receiver, TargetOptions options) -> Result<CallTarget> {
        if constexpr (!supported_signature(signature_of<S>())) {
            return std::unexpected(Diagnostic{DiagnosticCode::unsupported, member_id<M>()});
        } else {
            options.native_dependency = NativeDependency::native;
            if constexpr (!std::meta::is_static_member(M)) {
                using Receiver = std::conditional_t<is_const(signature_of<S>().receiver.qualifiers), const T, T>;
                auto pointer = receiver.template get<Receiver>();
                if (!pointer) return std::unexpected(pointer.error());
                options.receiver = receiver;
                return [&]<std::size_t... I>(std::index_sequence<I...>) {
                    return make_target<S>([pointer = *pointer](std::tuple_element_t<I, Args>... args) -> R {
                        return (pointer->* &[:M:])(std::forward<std::tuple_element_t<I, Args>>(args)...);
                    }, std::move(options));
                }(std::make_index_sequence<std::tuple_size_v<Args>>{});
            } else {
                options.receiver = {};
                return make_target<S>(&[:M:], std::move(options));
            }
        }
    };
    return op;
}
template<class T, std::meta::info M> NativeOperation constructor() {
    static constexpr auto params = std::define_static_array(std::meta::parameters_of(M));
    return []<std::size_t... I>(std::index_sequence<I...>) {
        using S = T(typename reflected_type<std::meta::type_of(params[I])>::type...);
        NativeOperation op;
        op.descriptor = {member_id<M>(), "<constructor>", signature_of<S>(), OperationKind::construct};
        op.result_descriptor = &describe_native<T>;
        op.bind = [](ObjectView, TargetOptions) {
            return CallTarget::constructor<T, typename reflected_type<std::meta::type_of(params[I])>::type...>();
        };
        return op;
    }(std::make_index_sequence<params.size()>{});
}
template<class T, std::meta::info M> void fields(std::vector<NativeOperation>& operations) {
    using V = [:std::meta::type_of(M):];
    using U = std::remove_cv_t<V>;
    if constexpr (!std::is_volatile_v<V> && !std::is_array_v<V> && !std::meta::is_static_member(M)) {
        if constexpr (std::is_copy_constructible_v<U>) {
            NativeOperation read;
            read.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<U() const>(), OperationKind::read};
            read.result_descriptor = &value_descriptor<U>;
            read.bind = [](ObjectView receiver, TargetOptions options) -> Result<CallTarget> {
                auto ptr = receiver.template get<const T>();
                if (!ptr) return std::unexpected(ptr.error());
                options.receiver = receiver;
                options.operation = OperationKind::read;
                return make_target<U() const>([ptr = *ptr] { return ptr->[:M:]; }, options);
            };
            operations.push_back(std::move(read));
        }
        NativeOperation view;
        view.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<const U&() const>(), OperationKind::view};
        view.result_descriptor = &value_descriptor<U>;
        view.bind = [](ObjectView receiver, TargetOptions options) -> Result<CallTarget> {
            auto ptr = receiver.template get<const T>();
            if (!ptr) return std::unexpected(ptr.error());
            options.receiver = receiver;
            options.operation = OperationKind::view;
            options.result_lifetime = ResultLifetime::receiver;
            return make_target<const U&() const>([ptr = *ptr]() -> const U& { return ptr->[:M:]; }, options);
        };
        operations.push_back(std::move(view));
        if constexpr (!std::is_const_v<V> && std::is_move_assignable_v<U>) {
            NativeOperation write;
            write.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<void(U)>(), OperationKind::write};
            write.result_descriptor = &value_descriptor<void>;
            write.bind = [](ObjectView receiver, TargetOptions options) -> Result<CallTarget> {
                auto ptr = receiver.template get<T>();
                if (!ptr) return std::unexpected(ptr.error());
                options.receiver = receiver;
                options.operation = OperationKind::write;
                return make_target<void(U)>([ptr = *ptr](U value) { ptr->[:M:] = std::move(value); }, options);
            };
            operations.push_back(std::move(write));
        }
    }
}
template<std::meta::info M> void static_fields(std::vector<NativeOperation>& operations) {
    using V = [:std::meta::type_of(M):];
    using U = std::remove_cv_t<V>;
    if constexpr (!std::is_volatile_v<V> && !std::is_array_v<V>) {
        if constexpr (std::is_copy_constructible_v<U>) {
            NativeOperation read;
            read.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<U()>(), OperationKind::read};
            read.result_descriptor = &value_descriptor<U>;
            read.bind = [](ObjectView, TargetOptions options) {
                options.operation = OperationKind::read;
                return make_target<U()>([] {
                    if constexpr (std::is_const_v<V>) return U([:std::meta::constant_of(M):]);
                    else return U([:M:]);
                }, options);
            };
            operations.push_back(std::move(read));
        }
        if constexpr (!std::is_const_v<V> && std::is_move_assignable_v<U>) {
            NativeOperation write;
            write.descriptor = {member_id<M>(), std::string(name<M>()), signature_of<void(U)>(), OperationKind::write};
            write.result_descriptor = &value_descriptor<void>;
            write.bind = [](ObjectView, TargetOptions options) {
                options.operation = OperationKind::write;
                return make_target<void(U)>([](U value) { [:M:] = std::move(value); }, options);
            };
            operations.push_back(std::move(write));
        }
    }
}
template<class T> void populate_operations(ClassInfo& info) {
    info.identity = type_id<T>();
    info.make_view = &native_view<T>;
    static constexpr auto members = std::define_static_array(
        std::meta::members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : members) {
        if constexpr (public_method(m) && (std::meta::has_identifier(m) || std::meta::is_operator_function(m))) {
            info.operations.push_back(method<T, m>());
        } else if constexpr (std::meta::is_constructor(m) && std::meta::is_public(m) &&
                             !std::meta::is_deleted(m) && !std::is_abstract_v<T>) {
            static constexpr auto params = std::define_static_array(std::meta::parameters_of(m));
            constexpr bool copy_or_move = [] {
                if constexpr (params.size() == 1) {
                    using P = [:std::meta::type_of(params[0]):];
                    return std::is_same_v<std::remove_cvref_t<P>, T>;
                } else return false;
            }();
            if constexpr (!copy_or_move) info.operations.push_back(constructor<T, m>());
        } else if constexpr (std::meta::is_nonstatic_data_member(m) && std::meta::is_public(m) &&
                             !std::meta::is_bit_field(m)) {
            fields<T, m>(info.operations);
        } else if constexpr (std::meta::is_variable(m) && std::meta::is_static_member(m) && std::meta::is_public(m)) {
            static_fields<m>(info.operations);
        }
    }
}
}
template<class T> std::shared_ptr<const ClassInfo> describe_native() {
    static const auto descriptor = [] {
        ClassInfo info;
        info.name = std::string(reflect_detail::type_name<T>());
        reflect_detail::populate_operations<T>(info);
        return std::make_shared<const ClassInfo>(std::move(info));
    }();
    return descriptor;
}

}
