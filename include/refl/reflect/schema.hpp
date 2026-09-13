#pragma once
#include <refl/reflect/native.hpp>

namespace refl {
// Describes declarations. No member address or native implementation is required.
template<class T> std::shared_ptr<const InterfaceSchema> describe_interface() {
    static const auto schema = [] {
        InterfaceSchema result;
        static constexpr auto members = std::define_static_array(
            std::meta::members_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr auto m : members) {
            if constexpr (reflect_detail::public_method(m) && std::meta::has_identifier(m) &&
                          !std::meta::is_static_member(m)) {
                using S = [:std::meta::type_of(m):];
                result.push_back({member_id<m>(), std::string(reflect_detail::name<m>()), signature_of<S>()});
            } else if constexpr (std::meta::is_nonstatic_data_member(m) && std::meta::is_public(m) && !std::meta::is_bit_field(m)) {
                using V = [:std::meta::type_of(m):];
                using U = std::remove_cv_t<V>;
                if constexpr (!std::is_volatile_v<V> && !std::is_array_v<V>) {
                    result.push_back({member_id<m>(), std::string(reflect_detail::name<m>()), signature_of<const U&() const>(), OperationKind::view});
                    if constexpr (std::is_copy_constructible_v<U>)
                        result.push_back({member_id<m>(), std::string(reflect_detail::name<m>()), signature_of<U() const>(), OperationKind::read});
                    if constexpr (!std::is_const_v<V> && std::is_move_assignable_v<U>)
                        result.push_back({member_id<m>(), std::string(reflect_detail::name<m>()), signature_of<void(U)>(), OperationKind::write});
                }
            }
        }
        static constexpr auto bases = std::define_static_array(
            std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr auto b : bases) {
            if constexpr (std::meta::is_public(b)) {
                using B = [:std::meta::type_of(b):];
                for (const auto& operation : *describe_interface<B>()) {
                    bool hidden = false;
                    template for (constexpr auto m : members) {
                        if constexpr (std::meta::has_identifier(m))
                            if (operation.name == std::meta::identifier_of(m)) hidden = true;
                    }
                    if (!hidden) result.push_back(operation);
                }
            }
        }
        return std::make_shared<const InterfaceSchema>(std::move(result));
    }();
    return schema;
}
}
