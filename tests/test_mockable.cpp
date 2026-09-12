// Mockable<T> test: mocking + method hooks + property hooks through Proxy<T>.
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

// Interface with data members (for property tests).
struct IWidget {
    int width;
    int height;
    virtual void draw() = 0;
    virtual ~IWidget() = default;
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

    // === Re-implement post-bind ===
    m->implement<^^IShape::area>([](int scale) { return scale * 200; });
    CHECK(p->area(5) == 1000, "re-implemented area(5) should be 1000");

    // === Method hook injection after bind ===
    int hook_result = 0;
    m->inject_hook<^^IShape::area>([&hook_result](refl::Object& r) {
        hook_result = *r.template cast_ref<int>().value();
    });
    (void)p->area(5);
    CHECK(hook_result == 1000, "method hook should fire with result 1000");

    // === Multiple method hooks accumulate ===
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
    CHECK(last_color == 77, "set_color should store 77");

    // === 2-arg method ===
    auto mc = refl::Mockable<ICalculator>::create();
    mc->implement<^^ICalculator::add>([](int a, int b) { return a + b; });
    auto pc = mc->proxy();
    CHECK(pc->add(3, 4) == 7, "add(3,4) should be 7");

    // === Property mock: set + read through proxy ===
    auto mw = refl::Mockable<IWidget>::create();
    mw->set_property<^^IWidget::width>(42);
    mw->set_property<^^IWidget::height>(24);
    mw->implement<^^IWidget::draw>([]() {});
    auto pw = mw->proxy();
    CHECK(pw->width == 42, "width should be 42");
    CHECK(pw->height == 24, "height should be 24");

    // === Property write through proxy ===
    pw->width = 99;
    CHECK(pw->width == 99, "width after write should be 99");

    // === Property on_change hook after bind ===
    int width_changed_to = 0;
    mw->on_change<^^IWidget::width>([&width_changed_to](refl::Object& v) {
        width_changed_to = *v.template cast_ref<int>().value();
    });
    pw->width = 55;
    CHECK(pw->width == 55, "width after on_change write should be 55");
    CHECK(width_changed_to == 55, "on_change should fire with value 55");

    // === Multiple property hooks accumulate ===
    int width_hook2 = 0;
    mw->on_change<^^IWidget::width>([&width_hook2](refl::Object& v) {
        width_hook2 = *v.template cast_ref<int>().value() * 2;
    });
    width_changed_to = 0;
    pw->width = 10;
    CHECK(width_changed_to == 10, "first on_change should fire with 10");
    CHECK(width_hook2 == 20, "second on_change should fire with 20");

    // === on_change on height (separate property) ===
    int height_changed = 0;
    mw->on_change<^^IWidget::height>([&height_changed](refl::Object& v) {
        height_changed = *v.template cast_ref<int>().value();
    });
    pw->height = 88;
    CHECK(height_changed == 88, "height on_change should fire with 88");

    // === Lifetime: Mockable destroyed, Proxy keeps it alive ===
    refl::Proxy<IShape> p2;
    {
        auto m2 = refl::Mockable<IShape>::create();
        m2->implement<^^IShape::area>([](int s) { return s * 1000; });
        p2 = m2->proxy();
    }
    CHECK(p2->area(5) == 5000, "area(5) after Mockable destroyed should be 5000");

    // === cast_safe<T> on mock fails ===
    auto obj = m->as_object();
    CHECK(!obj.cast_safe<IShape>().has_value(),
          "cast_safe<IShape> on mock should fail");

    printf("All mockable tests passed.\n");
    return 0;
}
