#include <refl/refl.hpp>
#include <refl/reflect/schema.hpp>
#include <cassert>
struct LocalBase { int value = 4; int read() const { return value; } };
struct LocalDerived : LocalBase { int twice() const { return value * 2; } };
struct DeclarationOnly { int read() const; void update(int&); };
int main() {
    auto schema = refl::describe_interface<DeclarationOnly>();
    assert(schema->size() == 2);
    auto descriptor = refl::describe_class<LocalDerived>();
    assert(!refl::find_class("LocalDerived"));
    assert(!refl::find_class("LocalBase"));
    refl::Class retained;
    {
        refl::Registry first, second;
        assert(first.publish(descriptor));
        assert(first.publish(descriptor));
        assert(!second.find_class("LocalDerived"));
        auto other = std::make_shared<const refl::ClassInfo>(*descriptor);
        assert(second.publish(other));
        assert(!first.publish(other));
        retained = refl::Class(first.find_class("LocalDerived"));
    }
    auto object = retained.find_constructor({})->call().value();
    assert(*retained.find_function("read")->invoke(object)->cast_safe<int>().value() == 4);
    assert(object.cast_safe<LocalBase>().value()->value == 4);
    assert(retained.bases()[0].as_class().find_function("read"));
}
