// This consumer deliberately uses neither <meta> nor the umbrella header.
#include <refl/core/descriptor.hpp>
#include <refl/core/error.hpp>
#include <expected>
#include <utility>

int main() {
    refl::ClassInfo descriptor;
    descriptor.name = "Synthetic";
    descriptor.fields.push_back({"value", "int", 0, nullptr, nullptr, false});
    const auto retained = std::make_shared<const refl::ClassInfo>(std::move(descriptor));
    std::expected<std::shared_ptr<const refl::ClassInfo>, refl::Error> result(retained);
    return result && (*result)->fields.front().name == "value" ? 0 : 1;
}
