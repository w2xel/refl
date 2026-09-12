// Mockable<T> test: mocking + virtual properties through Proxy<T>.
// Hooks are user-side: wrap your lambda to add a hook.
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

struct IWidget {
    int width;
    int height;
    const int id = 7;
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

    // === Hook via wrapped lambda (no inject_hook API) ===
    int hook_result = 0;
    m->implement<^^IShape::area>([&hook_result](int scale) {
        int r = scale * 200;
        hook_result = r;
        return r;
    });
    (void)p->area(5);
    CHECK(hook_result == 1000, "wrapped lambda should observe result 1000");

    // === Hook on void method ===
    bool void_hook_fired = false;
    m->implement<^^IShape::set_color>([&last_color, &void_hook_fired](int c) {
        last_color = c;
        void_hook_fired = true;
    });
    p->set_color(77);
    CHECK(void_hook_fired, "void method hook should fire");
    CHECK(last_color == 77, "set_color should store 77");

    // === 2-arg method ===
    auto mc = refl::Mockable<ICalculator>::create();
    mc->implement<^^ICalculator::add>([](int a, int b) { return a + b; });
    auto pc = mc->proxy();
    CHECK(pc->add(3, 4) == 7, "add(3,4) should be 7");

    // === Stored-value property (sugar) ===
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

    // === Virtual property with hook in the setter ===
    int width_changed_to = 0;
    int stored_w = 42;
    mw->implement_property<^^IWidget::width>(
        [&stored_w]() { return stored_w; },
        [&width_changed_to, &stored_w](int v) {
            stored_w = v;
            width_changed_to = v;
        }
    );
    pw->width = 55;
    CHECK(pw->width == 55, "width after virtual set should be 55");
    CHECK(width_changed_to == 55, "setter hook should observe 55");

    // === Read-only virtual property ===
    mw->implement_property<^^IWidget::height>(
        []() { return 88; }
    );
    CHECK(pw->height == 88, "read-only property should return 88");

    // === Re-implement property to add another hook ===
    int hook2 = 0;
    mw->implement_property<^^IWidget::width>(
        [&stored_w]() { return stored_w; },
        [&width_changed_to, &hook2, &stored_w](int v) {
            stored_w = v;
            width_changed_to = v;
            hook2 = v * 2;
        }
    );
    width_changed_to = 0;
    pw->width = 10;
    CHECK(width_changed_to == 10, "re-implemented setter hook should fire with 10");
    CHECK(hook2 == 20, "second hook should fire with 20");

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
