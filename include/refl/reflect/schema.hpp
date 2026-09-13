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
