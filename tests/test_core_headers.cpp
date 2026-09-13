// This consumer deliberately uses neither <meta> nor the umbrella header.
#include <refl/core/descriptor.hpp>
#include <refl/core/error.hpp>
#include <refl/runtime/registry.hpp>
#include <expected>
#include <utility>

int main() {
    refl::ClassInfo descriptor;
    descriptor.name = "Synthetic";
    descriptor.fields.push_back({"value", "int", 0, nullptr, nullptr, false});
    const auto retained = std::make_shared<const refl::ClassInfo>(std::move(descriptor));
    std::expected<std::shared_ptr<const refl::ClassInfo>, refl::Error> result(retained);
    refl::Registry registry;
    if (!registry.publish(retained) || registry.find_class("Synthetic") != retained) return 1;
    return result && (*result)->fields.front().name == "value" ? 0 : 1;
}
