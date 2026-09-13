#include <refl/hooks.hpp>
#include <cstdio>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } \
} while (0)

struct Point {
    int x, y;
    Point(int x, int y) : x(x), y(y) {}
    int sum() { return x + y; }
};

[[maybe_unused]] static refl::Hooks<Point> reg;

int main() {
    refl::Hooks<Point> p(10, 20);

    int hook_a = 0, hook_b = 0;
    p.connect<^^Point::sum>([&hook_a](refl::Object& r) {
        hook_a = *r.template cast_ref<int>().value();
    });
    p.connect<^^Point::sum>([&hook_b](refl::Object& r) {
        hook_b = *r.template cast_ref<int>().value();
    });
    (void)p->sum();
    CHECK(hook_a == 30, "connect hook A should fire with 30");
    CHECK(hook_b == 30, "connect hook B should fire with 30");

    // on_change
    int change_result = 0;
    p.on_change<^^Point::x>([&change_result](refl::Object& v) {
        change_result = *v.template cast_ref<int>().value();
    });
    p->x = 42;
    CHECK(change_result == 42, "on_change should fire with 42");

    // emit
    int emit_result = 0;
    p.connect_signal("custom", [&emit_result](refl::Object& v) {
        emit_result = *v.template cast_ref<int>().value();
    });
    p.emit("custom", refl::Object(std::make_shared<int>(42),
        refl::detail::ensure_class_info<int>()));
    CHECK(emit_result == 42, "emit should fire with 42");

    // cross-object
    refl::Hooks<Point> p2(5, 5);
    int cross_result = 0;
    p2.connect_signal("sum", [&cross_result](refl::Object& r) {
        cross_result = *r.template cast_ref<int>().value();
    });
    p.connect_to<^^Point::sum>(&p2, "sum");
    hook_a = 0; hook_b = 0;
    (void)p->sum();
    printf("hook_a=%d hook_b=%d cross=%d\n", hook_a, hook_b, cross_result);
    CHECK(hook_a == 62, "p hook A should fire with 62 (42+20)");
    CHECK(hook_b == 62, "p hook B should fire with 62 (42+20)");
    CHECK(cross_result == 62, "cross-object should receive 62");

    // Stored event payloads keep their values after delivery and facade destruction.
    refl::Object saved_result, saved_property;
    {
        refl::Hooks<Point> source(2, 3);
        source.connect<^^Point::sum>([&](refl::Object& result) { saved_result = result; });
        source.on_change<^^Point::x>([&](refl::Object& result) { saved_property = result; });
        (void)source->sum();
        source->x = 8;
    }
    CHECK(saved_result.is_owned() && *saved_result.cast_safe<int>().value() == 5, "method event retains its payload");
    CHECK(saved_property.is_owned() && *saved_property.cast_safe<int>().value() == 8, "property event retains its payload");

    // Wrappers retain replaced implementations and compose in installation order.
    {
        refl::Dyn<Point> wrapped(1, 2);
        auto context = std::make_shared<int>(10);
        std::weak_ptr<int> lifetime = context;
        wrapped.implement<^^Point::sum>([context](refl::Dyn<Point>::Self&) {
            return *context;
        });
        context.reset();
        wrapped.wrap<^^Point::sum>([](auto& original, refl::Dyn<Point>::Self&) {
            return original() + 1;
        });
        wrapped.wrap<^^Point::sum>([](auto& original, refl::Dyn<Point>::Self&) {
            return original() * 2;
        });
        CHECK(!lifetime.expired(), "wrapper chain should retain its implementation");
        CHECK(wrapped->sum() == 22, "nested wrappers should compose as B(A(target))");
        refl::Dyn<Point> moved(std::move(wrapped));
        CHECK(moved->sum() == 22, "moving Dyn should preserve wrapper contexts");
        moved.restore<^^Point::sum>();
        CHECK(moved->sum() == 3, "restore should select the native implementation");
        CHECK(lifetime.expired(), "restore should release inactive wrapper contexts");
    }

    printf("All hooks tests passed.\n");
    return 0;
}
