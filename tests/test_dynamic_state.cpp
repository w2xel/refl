#include <refl/dyn.hpp>
#include <refl/extensions/observed.hpp>
#include <cassert>

struct Shape {
    int render(int) const;
    void update(int&);
    int& reference();
    int value;
};
struct Square {
    int value;
    explicit Square(int n) : value(n) {}
    int render(int scale) const { return value * value * scale; }
    void update(int& out) { out = value; }
    int& reference() { return value; }
};
struct Triangle {
    int value;
    explicit Triangle(int n) : value(n) {}
    int render(int scale) const { return value * scale; }
    void update(int& out) { out = value + 1; }
    int& reference() { return value; }
};
struct BadShape { int render(int) const { return 0; } };
struct ThrowingSquare : Square { ThrowingSquare() : Square(0) { throw std::runtime_error("constructor"); } };
struct Overloads { int call(int); double call(double); };
consteval std::meta::info overload(std::meta::info parameter) {
    for (auto member : std::meta::members_of(^^Overloads, std::meta::access_context::unchecked())) {
        if (std::meta::has_identifier(member) && std::meta::identifier_of(member) == "call" &&
            std::meta::type_of(std::meta::parameters_of(member)[0]) == parameter) return member;
    }
    throw "missing overload";
}
struct OnDrop { std::function<void()> callback; ~OnDrop() { callback(); } };
int main() {
    constexpr auto integer = overload(^^int);
    constexpr auto floating = overload(^^double);
    refl::Dyn<Overloads> overloaded;
    overloaded.implement<integer>([](int n) { return n + 1; });
    overloaded.implement<floating>([](double n) { return n * 2; });
    overloaded.implement<integer>([](int n) { return n + 2; });
    assert(overloaded->call(1) == 3 && overloaded->call(1.5) == 3.0);
    overloaded.implement<integer>([](int) -> short { return 7; });
    assert(overloaded->call(0) == 7); // Storage follows the declared int result.
    {
        auto callback = std::make_shared<OnDrop>();
        callback->callback = [weak = overloaded.weak()] { weak().detach_native(); };
        overloaded.implement<integer>([callback](int n) { return n; });
        callback.reset();
        overloaded.implement<integer>([](int n) { return n; });
        try { (void)overloaded->call(1); assert(false); } catch (const refl::ReflectionError&) {}
    }

    constexpr auto render = refl::member_id<^^Shape::render>();
    constexpr auto reference = refl::member_id<^^Shape::reference>();
    refl::Dyn<Shape> dynamic;
    dynamic.reset_native<Square>(4);
    auto live = dynamic.dispatch();
    auto captured = dynamic.capture_binding();
    auto saved = dynamic.target<^^Shape::render>();
    dynamic.implement<^^Shape::render>([](int) { return 33; });
    assert(captured->render(1) == 33);
    dynamic.reset_native<Triangle>(6);
    assert(dynamic->render(2) == 12 && captured->render(1) == 33);
    assert(refl::try_call<int>(live, render, 2).value() == 12);
    dynamic.replace<^^Shape::render>(saved);
    assert(dynamic->render(2) == 32);
    dynamic.restore<^^Shape::render>();
    assert(dynamic->render(2) == 12);
    dynamic->value = 7;
    assert(dynamic->render(2) == 14);
    assert(refl::try_call<int>(dynamic.dispatch(refl::OperationKind::read), refl::member_id<^^Shape::value>()).value() == 7);
    {
        auto view = refl::try_call_retained<const int>(dynamic.dispatch(refl::OperationKind::view), refl::member_id<^^Shape::value>()).value();
        assert(view.get() == 7);
        dynamic->value = 8;
        assert(view.get() == 8);
        dynamic->value = 7;
    }
    try { dynamic.reset_native<BadShape>(); assert(false); } catch (const refl::ReflectionError&) {}
    try { dynamic.reset_native<ThrowingSquare>(); assert(false); } catch (const std::runtime_error&) {}
    assert(dynamic->render(2) == 14);
    auto wrong = refl::make_target<double(int)>([](int) { return 0.0; }).value();
    try { dynamic.replace<^^Shape::render>(wrong); assert(false); } catch (const refl::ReflectionError&) {}
    assert(dynamic->render(2) == 14);
    dynamic.wrap<^^Shape::render>([](auto& previous, int n) { return previous(n) + 1; });
    dynamic.wrap<^^Shape::render>([](auto& previous, int n) { return previous(n) * 2; });
    assert(dynamic->render(2) == 30);
    auto moved = std::move(dynamic);
    assert(moved->render(2) == 30);
    moved.reset_native<Square>(3);
    assert(refl::try_call<int>(live, render, 2).value() == 18);

    // Detachment retains only explicitly independent replacements.
    moved.implement<^^Shape::update>([](int& out) { out = 90; },
        {.native_dependency = refl::NativeDependency::independent});
    moved.implement<^^Shape::render>([](int) { return 100; }); // Unknown dependency.
    moved.detach_native();
    assert(moved.is_dynamic() && !moved.get_class());
    int out = 0;
    moved->update(out);
    assert(out == 90);
    try { (void)moved->render(1); assert(false); } catch (const refl::ReflectionError&) {}
    try { moved.restore<^^Shape::render>(); assert(false); } catch (const refl::ReflectionError&) {}
    moved.replace<^^Shape::render>(saved);
    assert(moved->render(2) == 32);
    moved.detach_native(); // A saved native target is still native-dependent.
    try { (void)moved->render(1); assert(false); } catch (const refl::ReflectionError&) {}

    moved.implement<^^Shape::render>([](int n) { return n + 2; },
        {.native_dependency = refl::NativeDependency::independent});
    moved.wrap<^^Shape::render>([](auto& previous, int n) { return previous(n) * 3; }, refl::NativeDependency::independent);
    moved.detach_native();
    assert(moved->render(2) == 12);
    {
        auto dependency = std::make_shared<int>(5);
        std::weak_ptr<int> dependency_lifetime = dependency;
        moved.implement<^^Shape::render>([dependency](int) { return *dependency; },
            {.native_dependency = refl::NativeDependency::native});
        dependency.reset();
        auto retained_target = moved.target<^^Shape::render>();
        moved.detach_native();
        assert(!dependency_lifetime.expired());
        retained_target = {};
        assert(dependency_lifetime.expired());
    }

    // A typed raw reference is rejected before entry; retained export keeps the old closure.
    auto owner = std::make_shared<int>(42);
    std::weak_ptr<int> lifetime = owner;
    int effects = 0;
    moved.implement<^^Shape::reference>([owner, &effects](refl::Dyn<Shape>::Self& self) -> int& {
        ++effects;
        self.implement<^^Shape::reference>([]() -> int& { static int next = 8; return next; });
        return *owner;
    }, {.result_lifetime = refl::ResultLifetime::callable_context,
        .native_dependency = refl::NativeDependency::independent});
    owner.reset();
    try { (void)moved->reference(); assert(false); } catch (const refl::ReflectionError&) {}
    assert(effects == 0);
    {
        auto retained = refl::try_call_retained<int>(moved.dispatch(), reference).value();
        assert(effects == 1 && retained.get() == 42 && !lifetime.expired());
    }
    assert(lifetime.expired());

    moved.implement<^^Shape::reference>([value = 77]() mutable -> int& { return value; },
        {.result_lifetime = refl::ResultLifetime::callable_context});
    auto observed = refl::observe(moved.dispatch(), [](std::exception_ptr) noexcept {});
    auto subscription = observed.after(reference, [&](const refl::CallCompletedEvent&) {
        moved.implement<^^Shape::reference>([]() -> int& { static int next = 2; return next; });
    });
    assert(refl::try_call_retained<int>(observed, reference).value().get() == 77);

    moved.set_property<^^Shape::value>(123);
    auto property = refl::try_call_retained<const int>(moved.dispatch(refl::OperationKind::view), refl::member_id<^^Shape::value>()).value();
    moved.set_property<^^Shape::value>(456);
    assert(property.get() == 123 && moved->value == 456);

    // Saved callbacks never dereference a destroyed facade.
    refl::CallTarget orphan;
    {
        refl::Dyn<Shape> temporary;
        temporary.implement<^^Shape::render>([](refl::Dyn<Shape>::Self&, int) { return 1; });
        orphan = temporary.target<^^Shape::render>();
    }
    try { (void)refl::detail::call_bound<int>(orphan, false, 1); assert(false); }
    catch (const refl::ReflectionError& error) { assert(error.diagnostic.code == refl::DiagnosticCode::null_handle); }
}
