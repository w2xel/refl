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

    printf("All hooks tests passed.\n");
    return 0;
}
