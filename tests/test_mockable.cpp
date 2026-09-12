// Mockable<T> test: mocking + post-bind hook injection through Proxy<T>.
#include <refl/mockable.hpp>

#include <cstdio>
#include <stdexcept>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } \
} while (0)

struct IShape {
    virtual int  area(int scale) = 0;
    virtual void set_color(int c) = 0;
    virtual int  color() const = 0;
    virtual ~IShape() = default;
};

struct ICalculator {
    virtual int add(int a, int b) = 0;
    virtual ~ICalculator() = default;
};

int main() {
    // === Basic mocking ===
    auto m = refl::Mockable<IShape>::create();
    m->implement<^^IShape::area>([](int scale) { return scale * 100; });

    int last_color = -1;
    m->implement<^^IShape::set_color>([&last_color](int c) { last_color = c; });
    m->implement<^^IShape::color>([]() { return 42; });

    auto p = m->proxy();
    CHECK(p->area(5) == 500, "area(5) should be 500");
    p->set_color(99);
    CHECK(last_color == 99, "set_color should store 99");
    CHECK(p->color() == 42, "color() should be 42");

    // === Re-implement (replace at runtime, post-bind) ===
    m->implement<^^IShape::area>([](int scale) { return scale * 200; });
    CHECK(p->area(5) == 1000, "re-implemented area(5) should be 1000");

    // === Hook injection AFTER proxy bind ===
    int hook_result = 0;
    m->inject_hook<^^IShape::area>([&hook_result](refl::Object& r) {
        hook_result = *r.template cast_ref<int>().value();
    });
    // Call through the proxy — hook should fire with the result.
    int a = p->area(5);
    CHECK(a == 1000, "area(5) with hook should still return 1000");
    CHECK(hook_result == 1000, "hook should have received result 1000");

    // === Multiple hooks accumulate ===
    int hook2 = 0;
    m->inject_hook<^^IShape::area>([&hook2](refl::Object& r) {
        hook2 = *r.template cast_ref<int>().value() + 1;
    });
    hook_result = 0;
    (void)p->area(3);
    CHECK(hook_result == 600, "first hook should fire (3*200=600)");
    CHECK(hook2 == 601, "second hook should fire (600+1=601)");

    // === Hook on void method ===
    bool void_hook_fired = false;
    m->inject_hook<^^IShape::set_color>([&void_hook_fired](refl::Object&) {
        void_hook_fired = true;
    });
    p->set_color(77);
    CHECK(void_hook_fired, "hook on void method should fire");
    CHECK(last_color == 77, "set_color should still store 77");

    // === 2-arg method ===
    auto mc = refl::Mockable<ICalculator>::create();
    mc->implement<^^ICalculator::add>([](int a, int b) { return a + b; });
    auto pc = mc->proxy();
    CHECK(pc->add(3, 4) == 7, "add(3,4) should be 7");

    // === Lifetime: Mockable destroyed, Proxy keeps it alive ===
    refl::Proxy<IShape> p2;
    {
        auto m2 = refl::Mockable<IShape>::create();
        m2->implement<^^IShape::area>([](int s) { return s * 1000; });
        p2 = m2->proxy();
    }
    CHECK(p2->area(5) == 5000, "area(5) after Mockable destroyed should be 5000");

    // === cast_safe<T> on mock fails (no base) ===
    auto obj = m->as_object();
    CHECK(!obj.cast_safe<IShape>().has_value(),
          "cast_safe<IShape> on mock should fail");

    printf("All mockable tests passed.\n");
    return 0;
}
