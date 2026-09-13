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

    // Raw-pointer compatibility takes a snapshot of stack metadata.
    refl::Class snapshot;
    refl::Field field_snapshot;
    refl::Enum enum_snapshot;
    refl::Enumerator enumerator_snapshot;
    {
        auto source = describe();
        snapshot = refl::Class(&source);
        field_snapshot = refl::Field(&source, 0);
        source.fields.clear();
        source.name = "Changed";
        refl::EnumInfo values{"LocalEnum", {{"First", 7}}};
        enum_snapshot = refl::Enum(&values);
        enumerator_snapshot = refl::Enumerator(&values, 0);
        values.enumerators.clear();
    }
    check(snapshot.name() == "SyntheticLifetime");
    check(snapshot.fields().at(0).name() == "value");
    check(field_snapshot.name() == "value");
    check(enum_snapshot.find_enumerator(7)->name() == "First");
    check(enumerator_snapshot.value() == 7);
    check(!refl::Class(nullptr));
    check(!refl::Enum(nullptr));

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

    // Enum and enumerator handles retain replaced catalog metadata.
    refl::EnumRegistrar(refl::EnumInfo{"LifetimeEnum", {{"Old", 1}}});
    auto enumeration = refl::find_enum("LifetimeEnum").value();
    auto value = enumeration.find_enumerator("Old").value();
    std::weak_ptr<const refl::EnumInfo> enum_lifetime = refl::enum_pool().at("LifetimeEnum");
    refl::EnumRegistrar(refl::EnumInfo{"LifetimeEnum", {{"New", 2}}});
    check(enumeration.enumerators().at(0).value() == 1);
    enumeration = {};
    check(!enum_lifetime.expired());
    check(value.name() == "Old");
    value = {};
    check(enum_lifetime.expired());
    check(refl::find_enum("LifetimeEnum")->find_enumerator(2)->name() == "New");
    auto enum_source = std::make_shared<const refl::EnumInfo>();
    check(!refl::Enumerator(enum_source, 0));

    // Native member invocation remains usable after catalog replacement.
    refl::ensure_registered<NativeLifetime>();
    auto native = refl::find_class(refl::detail::type_name<NativeLifetime>()).value();
    auto read = native.find_function("read").value();
    auto object = native.find_constructor({})->call().value();
    auto native_source = object.class_info();
    std::weak_ptr<const refl::ClassInfo> native_lifetime = native_source;
    refl::ClassInfo replacement;
    replacement.name = native.name();
    refl::Registrar publish(replacement);
    native = {};
    native_source.reset();
    check(*read.invoke(object)->cast_safe<int>().value() == 17);
    object = {};
    check(!native_lifetime.expired());
    check(read.name() == "read");
    read = {};
    check(native_lifetime.expired());

    // Inherited singular/plural lookups retain the declaring base allocation.
    refl::ClassInfo base;
    base.name = "LifetimeBase";
    base.functions.push_back({"inherited", {}, "int", nullptr, true});
    refl::Registrar publish_base(base);
    std::weak_ptr<const refl::ClassInfo> base_lifetime = refl::class_pool().at(base.name);
    refl::Registrar publish_derived(describe());
    auto derived = refl::find_class("SyntheticLifetime").value();
    auto inherited = derived.find_function("inherited").value();
    auto overloads = derived.find_functions("inherited");
    auto members = derived.all_functions();
    base.functions.clear();
    refl::Registrar replace_base(base);
    derived = {};
    check(inherited.name() == "inherited");
    check(overloads.at(0).name() == "inherited");
    check(!base_lifetime.expired());
    inherited = {};
    overloads.clear();
    check(!base_lifetime.expired());
    members.clear();
    check(base_lifetime.expired());
}
