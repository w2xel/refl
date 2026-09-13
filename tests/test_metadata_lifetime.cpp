// Descriptor ownership is independent of catalog entries and concrete objects.
#include <refl/refl.hpp>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <limits>

namespace {
void check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(stderr, "Check failed at %s:%u\n", location.file_name(), location.line());
        std::abort();
    }
}

refl::ClassInfo describe() {
    refl::ClassInfo info;
    info.name = "SyntheticLifetime";
    info.bases.push_back({"LifetimeBase", 0});
    info.constructors.push_back({{}, nullptr});
    info.functions.push_back({"read", {}, "int", nullptr, true});
    info.fields.push_back({"value", "int", 0, nullptr, nullptr, false});
    info.static_functions.push_back({"count", {}, "int", nullptr});
    info.static_fields.push_back({"total", "int", nullptr, nullptr, nullptr, false});
    return info;
}

// Each handle alone retains the original allocation, then releases it.
template <typename Select, typename Inspect>
void retains_class_metadata(Select select, Inspect inspect) {
    auto source = std::make_shared<const refl::ClassInfo>(describe());
    std::weak_ptr<const refl::ClassInfo> lifetime = source;
    auto handle = select(refl::Class(source));
    source.reset();
    check(!lifetime.expired());
    inspect(handle);
    auto copy = handle;
    handle = {};
    check(!lifetime.expired());
    inspect(copy);
    copy = {};
    check(lifetime.expired());
}

struct NativeLifetime {
    int value = 17;
    int read() const { return value; }
};
}

int main() {
    retains_class_metadata([](auto c) { return c; },
                          [](auto c) { check(c.name() == "SyntheticLifetime"); });
    retains_class_metadata([](auto c) { return c.find_constructor({}).value(); },
                          [](auto h) { check(h.param_types().empty()); });
    retains_class_metadata([](auto c) { return c.find_function("read").value(); },
                          [](auto h) { check(h.return_type() == "int"); });
    retains_class_metadata([](auto c) { return c.find_function("read", {}).value(); },
                          [](auto h) { check(h.name() == "read"); });
    retains_class_metadata([](auto c) { return c.find_field("value").value(); },
                          [](auto h) { check(h.type() == "int"); });
    retains_class_metadata([](auto c) { return c.find_static_function("count").value(); },
                          [](auto h) { check(h.name() == "count"); });
    retains_class_metadata([](auto c) { return c.find_static_field("total").value(); },
                          [](auto h) { check(h.name() == "total"); });
    retains_class_metadata([](auto c) { return c.bases().at(0); },
                          [](auto h) { check(h.name() == "LifetimeBase"); });
    retains_class_metadata([](auto c) { return c.all_functions().at(0); },
                          [](auto h) { check(h.name() == "read"); });
    retains_class_metadata([](auto c) { return c.find_functions("read").at(0); },
                          [](auto h) { check(h.name() == "read"); });

    // A non-null owner alone does not make an out-of-range member valid.
    auto source = std::make_shared<const refl::ClassInfo>(describe());
    constexpr auto bad = std::numeric_limits<std::size_t>::max();
    check(!refl::Constructor(source, bad));
    check(!refl::Function(source, bad));
    check(!refl::Field(source, bad));
    check(!refl::StaticFunction(source, bad));
    check(!refl::StaticField(source, bad));
    check(!refl::Base(source, bad));
    check(refl::Function(source, bad).invoke(refl::Object{}).error() == refl::Error::NullHandle);

    // Catalog ownership is independent of retained handles.
    refl::Enum enumeration;
    refl::Enumerator value;
    std::weak_ptr<const refl::EnumInfo> enum_lifetime;
    {
        refl::Registry registry;
        auto descriptor = std::make_shared<const refl::EnumInfo>(refl::EnumInfo{"LifetimeEnum", {{"Old", 1}}});
        enum_lifetime = descriptor;
        check(registry.publish(descriptor).has_value());
        check(registry.publish(descriptor).has_value());
        check(!registry.publish(std::make_shared<const refl::EnumInfo>(refl::EnumInfo{"LifetimeEnum", {{"New", 2}}})));
        enumeration = refl::Enum(registry.find_enum("LifetimeEnum"));
        value = enumeration.find_enumerator("Old").value();
    }
    enumeration = {};
    check(!enum_lifetime.expired() && value.name() == "Old");
    value = {};
    check(enum_lifetime.expired());

    refl::Function inherited;
    std::weak_ptr<const refl::ClassInfo> base_lifetime;
    {
        refl::ClassInfo base;
        base.name = "LifetimeBase";
        base.functions.push_back({"inherited", {}, "int", nullptr, true});
        auto base_descriptor = std::make_shared<const refl::ClassInfo>(std::move(base));
        base_lifetime = base_descriptor;
        auto derived = describe();
        derived.bases[0].descriptor = base_descriptor;
        refl::Class handle(std::make_shared<const refl::ClassInfo>(std::move(derived)));
        inherited = handle.find_function("inherited").value();
        check(handle.all_functions().size() == 2);
        check(handle.bases()[0].as_class().find_function("inherited").has_value());
    }
    check(!base_lifetime.expired() && inherited.name() == "inherited");
    inherited = {};
    check(base_lifetime.expired());
}
