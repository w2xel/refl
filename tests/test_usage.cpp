// Executable documentation. Named regions are included by MkDocs.
// Keep setup, actions, and expected results together in each section.
// --8<-- [start:headers]
#include <refl/dyn.hpp>
#include <refl/extensions/observed.hpp>
#include <cassert>
#include <memory>
// --8<-- [end:headers]

// --8<-- [start:runtime-types]
struct Shape {
    int id = 1;
};
struct Rectangle : Shape {
    int width;
    int height;
    inline static int created = 0;

    Rectangle(int w, int h) : width(w), height(h) { ++created; }
    int area() const { return width * height; }
    void resize(int side) { width = height = side; }
    void resize(int w, int h) { width = w; height = h; }
    static int count() { return created; }
};
enum class ShapeKind { circle = 1, rectangle = 2 };
// --8<-- [end:runtime-types]

void runtime_usage() {
    // --8<-- [start:registry]
    refl::Registry registry;
    auto descriptor = refl::describe_class<Rectangle>();
    registry.publish(descriptor).value();
    refl::Class type(registry.find_class("Rectangle"));

    auto object = type.find_constructor({"int", "int"}).value().call(3, 4).value();
    auto rectangle = object.cast_safe<Rectangle>().value();
    assert(rectangle->width == 3 && rectangle->height == 4);
    assert(!registry.find_class("Shape")); // Describing a base does not publish it.
    // --8<-- [end:registry]

    // --8<-- [start:runtime-members]
    auto width = type.find_field("width").value();
    width.set(object, 5).value();
    assert(*width.get(object).value().cast_safe<int>().value() == 5);

    auto resize = type.find_function("resize", {"int", "int"}).value();
    resize.invoke(object, 6, 7).value();
    auto area = type.find_function("area").value().invoke(object).value();
    assert(*area.cast_safe<int>().value() == 42);
    assert(type.find_functions("resize").size() == 2);

    // Inherited fields and checked base casts use retained metadata.
    assert(*type.find_field("id").value().get(object).value().cast_safe<int>().value() == 1);
    assert(object.cast_safe<Shape>().value()->id == 1);

    auto clone = object.clone().value();
    width.set(clone, 10).value();
    assert(rectangle->width == 6);
    assert(clone.cast_safe<Rectangle>().value()->width == 10);
    // --8<-- [end:runtime-members]

    // --8<-- [start:static-enum]
    auto count = type.find_static_function("count").value().invoke().value();
    assert(*count.cast_safe<int>().value() == 1);
    auto created = type.find_static_field("created").value().get().value();
    assert(*created.cast_safe<int>().value() == 1);

    registry.publish(refl::describe_enum<ShapeKind>()).value();
    refl::Enum kind(registry.find_enum("ShapeKind"));
    assert(kind.find_enumerator("rectangle").value().value() == 2);
    assert(kind.find_enumerator(1).value().name() == "circle");
    // --8<-- [end:static-enum]
}

// --8<-- [start:binding-types]
struct Drawable {
    int render(int scale) const; // Declarations only; no inheritance required.
    int size;
};
struct Square {
    int size;
    explicit Square(int n) : size(n) {}
    int render(int scale) const { return size * size * scale; }
};
struct Line {
    int size;
    explicit Line(int n) : size(n) {}
    int render(int scale) const { return size * scale; }
};
// --8<-- [end:binding-types]

void binding_usage() {
    // --8<-- [start:binding]
    auto square = std::make_shared<Square>(4);
    refl::Object object(square, refl::describe_class<Square>());
    refl::Proxy<Drawable> view(object);
    assert(view->render(2) == 32);

    view->size = 5;
    assert(square->size == 5 && view->render(2) == 50);

    refl::Class line(refl::describe_class<Line>());
    auto replacement = line.find_constructor({"int"}).value().call(6).value();
    view.bind(replacement);
    assert(view->render(2) == 12);
    assert(replacement.cast_safe<Line>().value()->size == 6);
    // --8<-- [end:binding]
}

void dynamic_usage() {
    // --8<-- [start:implementation]
    refl::Dyn<Drawable> dynamic;
    dynamic.implement<^^Drawable::render>([](int scale) { return scale * 10; });
    dynamic.set_property<^^Drawable::size>(3);
    assert(dynamic->render(2) == 20 && dynamic->size == 3);

    dynamic.wrap<^^Drawable::render>([](auto& previous, int scale) {
        return previous(scale) + 1;
    });
    assert(dynamic->render(2) == 21);
    // --8<-- [end:implementation]

    // --8<-- [start:generations]
    dynamic.reset_native<Square>(4);
    auto live = dynamic.dispatch();
    auto captured = dynamic.capture_binding();
    auto saved = dynamic.target<^^Drawable::render>();
    constexpr auto render = refl::member_id<^^Drawable::render>();

    dynamic.implement<^^Drawable::render>([](int) { return 99; });
    assert(captured->render(2) == 99); // Same generation, replaced target.

    dynamic.reset_native<Line>(6);
    assert(refl::try_call<int>(live, render, 2).value() == 12);
    assert(captured->render(2) == 99); // Reset publishes a new generation.

    dynamic.replace<^^Drawable::render>(saved);
    assert(dynamic->render(2) == 32); // Saved Square receiver.
    dynamic.restore<^^Drawable::render>();
    assert(dynamic->render(2) == 12); // Current Line baseline.
    // --8<-- [end:generations]

    // --8<-- [start:detach]
    dynamic.implement<^^Drawable::render>([](int scale) { return scale * 7; },
        {.native_dependency = refl::NativeDependency::independent});
    dynamic.detach_native();
    assert(dynamic.is_dynamic());
    assert(dynamic->render(2) == 14);
    // --8<-- [end:detach]
}

// --8<-- [start:reference-type]
struct Counter {
    int& value();
};
// --8<-- [end:reference-type]

void reference_usage() {
    // --8<-- [start:retained-reference]
    refl::Dyn<Counter> counter;
    counter.implement<^^Counter::value>([value = 42]() mutable -> int& { return value; },
        {.result_lifetime = refl::ResultLifetime::callable_context});

    auto retained = refl::try_call_retained<int>(
        counter.dispatch(), refl::member_id<^^Counter::value>()).value();
    counter.implement<^^Counter::value>([value = 7]() mutable -> int& { return value; },
        {.result_lifetime = refl::ResultLifetime::callable_context});
    assert(retained.get() == 42); // Retains the original closure.
    // --8<-- [end:retained-reference]
}

void observation_usage() {
    // --8<-- [start:observation]
    refl::Dyn<Drawable> dynamic;
    dynamic.reset_native<Square>(4);
    constexpr auto render = refl::member_id<^^Drawable::render>();
    int events = 0;
    int listener_errors = 0;
    auto observed = refl::observe(dynamic.dispatch(), [&](std::exception_ptr) noexcept {
        ++listener_errors;
    });
    auto subscription = observed.after(render, [&](const refl::CallCompletedEvent& event) {
        assert(event.member() == render && event.argument_count() == 1);
        ++events;
    });

    assert(refl::try_call<int>(observed, render, 2).value() == 32);
    assert(events == 1);
    assert(dynamic->render(2) == 32); // Direct calls bypass this adapter.
    assert(events == 1);

    dynamic.reset_native<Line>(6);
    assert(refl::try_call<int>(observed, render, 2).value() == 12);
    assert(events == 2); // Subscriptions survive reset.

    subscription.unsubscribe();
    assert(refl::try_call<int>(observed, render, 2).value() == 12);
    assert(events == 2 && listener_errors == 0);
    // --8<-- [end:observation]
}

int main() {
    runtime_usage();
    binding_usage();
    dynamic_usage();
    reference_usage();
    observation_usage();
}
