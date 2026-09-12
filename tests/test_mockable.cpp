// Mockable<T> prototype test: prove a synthetic ClassInfo + Object flows
// through Proxy<T> unchanged.  Abstract interface, no real object.
#include <refl/mockable.hpp>

#include <cstdio>
#include <stdexcept>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } \
} while (0)

// Abstract interface — cannot be constructed, only mocked.
struct IShape {
    virtual int  area(int scale) = 0;
    virtual void set_color(int c) = 0;
    virtual int  color() const = 0;
    virtual ~IShape() = default;
};

// Concrete type with a 2-arg method, to exercise that path.
struct ICalculator {
    virtual int add(int a, int b) = 0;
    virtual ~ICalculator() = default;
};

int main() {
    // --- Abstract interface mocking ---
    auto m = refl::Mockable<IShape>::create();
    m->implement<^^IShape::area>([](int scale) { return scale * 100; });

    int last_color = -1;
    m->implement<^^IShape::set_color>([&last_color](int c) {
        last_color = c;
    });
    m->implement<^^IShape::color>([]() { return 42; });

    auto p = m->proxy();
    int a = p->area(5);
    CHECK(a == 500, "area(5) should be 500");

    p->set_color(99);
    CHECK(last_color == 99, "set_color should store 99");

    int c = p->color();
    CHECK(c == 42, "color() should be 42");

    // --- Re-implement (replace a method at runtime) ---
    m->implement<^^IShape::area>([](int scale) { return scale * 200; });
    int a2 = p->area(5);
    CHECK(a2 == 1000, "re-implemented area(5) should be 1000");

    // --- 2-arg method ---
    auto mc = refl::Mockable<ICalculator>::create();
    mc->implement<^^ICalculator::add>([](int a, int b) { return a + b; });
    auto pc = mc->proxy();
    int sum = pc->add(3, 4);
    CHECK(sum == 7, "add(3,4) should be 7");

    // --- Unimplemented method throws at call time ---
    // IShape has area, set_color, color — all implemented.
    // But if we create a fresh mock and call without implement:
    auto m2 = refl::Mockable<IShape>::create();
    auto p2 = m2->proxy();
    bool threw = false;
    try { (void)p2->area(1); }
    catch (const std::bad_any_cast&) { threw = true; }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw, "calling unimplemented method should throw");

    // --- Mock Object is not cast_safe to T (no base) ---
    auto obj = m->as_object();
    auto cast_result = obj.cast_safe<IShape>();
    CHECK(!cast_result.has_value(), "cast_safe<IShape> on mock should fail");

    printf("All mockable tests passed.\n");
    return 0;
}
