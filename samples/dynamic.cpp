// Sample: runtime interface implementation and dynamic switching with Refl<T>.
//
// Demonstrates:
// 1. Building an object that implements an abstract interface at runtime.
// 2. Starting with a real object, then switching to a mock at runtime.
// 3. Switching back to a real object after mocking.
//
// This enables mocking patterns: test with a real object, swap to a mock
// for specific test cases, then swap back — all through the same Refl<T>.
#include <refl/refl.hpp>
#include <cstdio>

// A concrete shape — can be constructed normally.
struct Circle {
    int radius;
    Circle(int r) : radius(r) {}
    int area(int scale) const { return 314 * scale * radius / 100; }
    void set_color(int c) { color = c; }
    int color = 0;
};

// An abstract interface — cannot be constructed, only implemented.
struct IRenderer {
    virtual int area(int scale) = 0;
    virtual void set_color(int c) = 0;
    virtual ~IRenderer() = default;
};

[[maybe_unused]] static refl::Refl<Circle> reg_circle;
[[maybe_unused]] static refl::Refl<IRenderer> reg_renderer;

int main() {
    // --- Real object: Refl<Circle> with a concrete instance.
    std::printf("=== Real Circle (radius=5) ===\n");
    refl::Refl<Circle> circle(5);
    int ca = circle->area(2);
    std::printf("  area(2) = %d\n", ca);
    circle->set_color(42);
    int circle_color = circle->color;
    std::printf("  color = %d\n", circle_color);
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());

    // --- Switch to dynamic (mock) mode.
    std::printf("\n=== Mock Circle ===\n");
    int mock_color = 0;
    circle.implement<^^Circle::area>([](int scale) {
        return scale * 1000;  // mock: always 1000*scale
    });
    circle.implement<^^Circle::set_color>([&mock_color](int c) {
        mock_color = c;
    });
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());
    int ma = circle->area(2);
    std::printf("  mock area(2) = %d\n", ma);
    circle->set_color(77);
    std::printf("  mock_color = %d\n", mock_color);

    // --- Switch back to real mode.
    std::printf("\n=== Back to Real Circle (radius=10) ===\n");
    circle.reset(10);
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());
    int ra = circle->area(1);
    std::printf("  real area(1) = %d\n", ra);

    // --- Dynamic interface: Refl<IRenderer> implemented at runtime.
    std::printf("\n=== Dynamic IRenderer (abstract) ===\n");
    refl::Refl<IRenderer> renderer;
    int stored_color = 0;
    int fake_radius = 7;

    renderer.implement<^^IRenderer::area>([fake_radius](int scale) {
        return scale * fake_radius * fake_radius;
    });
    renderer.implement<^^IRenderer::set_color>([&stored_color](int c) {
        stored_color = c;
        std::printf("  set_color(%d) — stored\n", c);
    });

    int r_area = renderer->area(3);
    std::printf("  area(3) = %d\n", r_area);
    renderer->set_color(99);
    std::printf("  stored_color = %d\n", stored_color);

    // Re-implement at runtime.
    std::printf("\n=== Re-implemented IRenderer ===\n");
    renderer.implement<^^IRenderer::area>([](int scale) {
        return scale * 7;
    });
    int r_area2 = renderer->area(3);
    std::printf("  area(3) = %d\n", r_area2);

    return 0;
}

