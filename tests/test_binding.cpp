#include <refl/dyn.hpp>
#include <cassert>
struct BindingBase { int value = 5; void update(int& out) { out = value; } };
struct BindingNative : BindingBase {
    int take(std::unique_ptr<int> n) { return *n; }
    std::unique_ptr<int> produce() const { return std::make_unique<int>(value); }
    int read() const { return value; }
};
struct BindingInterface {
    void update(int&);
    int take(std::unique_ptr<int>);
    std::unique_ptr<int> produce() const;
    int read() const;
    int value;
};
struct WrongReference { void update(int) {} };
struct RefInterface { void update(int&); };
struct UnsupportedBinding { int&& reference() { static int value = 0; return std::move(value); } };
struct BadBinding { int read() { return 0; } };
struct BindingMock { void update(int&); int take(std::unique_ptr<int>); int sum(int,int,int); };
int main() {
    try {
        refl::Proxy<UnsupportedBinding> unsupported(refl::Class(refl::describe_class<UnsupportedBinding>()).find_constructor({})->call().value());
        assert(false);
    } catch (const refl::ReflectionError& error) {
        assert(error.diagnostic.code == refl::DiagnosticCode::unsupported);
    }
    auto type = refl::Class(refl::describe_class<BindingNative>());
    auto a = type.find_constructor({})->call().value();
    auto b = type.find_constructor({})->call().value();
    refl::Proxy<BindingInterface> first(a), second(b);
    assert(first.binding_plan() == second.binding_plan());
    first->value = 9;
    assert(first->read() == 9 && second->read() == 5);
    int out = 0;
    first->update(out);
    assert(out == 9);
    const int constant = 0;
    try { first->update(constant); assert(false); } catch (const refl::ReflectionError&) {}
    try { std::as_const(first)->update(out); assert(false); } catch (const refl::ReflectionError&) {}
    auto owned = std::make_unique<int>(8);
    try { (void)first->take(owned); assert(false); } catch (const refl::ReflectionError&) {}
    assert(owned && first->take(std::move(owned)) == 8 && !owned);
    assert(*first->produce() == 9);
    auto plan = first.binding_plan();
    try {
        auto bad = refl::Class(refl::describe_class<BadBinding>()).find_constructor({})->call().value();
        first.bind(bad);
        assert(false);
    } catch (const refl::ReflectionError&) {}
    assert(first.binding_plan() == plan && first->read() == 9);
    try {
        auto wrong = refl::Class(refl::describe_class<WrongReference>()).find_constructor({})->call().value();
        refl::Proxy<RefInterface> rejected(wrong);
        assert(false);
    } catch (const refl::ReflectionError&) {}
    auto moved = std::move(first);
    assert(!first.is_bound() && moved->read() == 9);
    try { (void)first->read(); assert(false); } catch (const refl::ReflectionError&) {}
    auto mock = std::make_shared<refl::Dyn<BindingMock>>();
    mock->implement<^^BindingMock::update>([](int& value) { value = 11; });
    mock->implement<^^BindingMock::take>([](std::unique_ptr<int> value) { return *value; });
    mock->implement<^^BindingMock::sum>([](int x, int y, int z) { return x+y+z; });
    auto view = mock->capture_binding();
    view->update(out);
    assert(out == 11 && view->sum(1,2,3) == 6);
    assert(view->take(std::make_unique<int>(12)) == 12);
    mock->implement<^^BindingMock::update>([](int& value) { value = 14; });
    view->update(out);
    assert(out == 14);
}
