// Sample: runtime interface implementation with Refl<T>.
//
// Demonstrates: building an object that implements an abstract interface
// at runtime, without ever writing a concrete class.  Methods are
// provided as lambdas via implement<^^T::method>().
//
// Also demonstrates: swapping between a real object and a dynamic one
// using reset() — the same Refl<T> can hold either a concrete instance
// or a runtime-implemented interface.
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
    refl::Refl<Circle> circle(5);
    std::printf("=== Real Circle (radius=5) ===\n");
    int ca = circle->area(2);
    std::printf("  area(2) = %d\n", ca);
    circle->set_color(42);
    int circle_color = circle->color;
    std::printf("  color = %d\n", circle_color);

    // --- Dynamic interface: Refl<IRenderer> implemented at runtime.
    std::printf("\n=== Dynamic IRenderer ===\n");
    refl::Refl<IRenderer> renderer;

    int stored_color = 0;
    int fake_radius = 10;

    renderer.implement<^^IRenderer::area>([fake_radius](int scale) {
        return scale * fake_radius * fake_radius;
    });
    renderer.implement<^^IRenderer::set_color>([&stored_color](int c) {
        stored_color = c;
        std::printf("  set_color(%d) — stored\n", c);
    });

    int ra = renderer->area(3);
    std::printf("  area(3) = %d\n", ra);
    renderer->set_color(99);
    std::printf("  stored_color = %d\n", stored_color);

    // --- Re-implement at runtime (swap the implementation).
    std::printf("\n=== Re-implemented IRenderer ===\n");
    renderer.implement<^^IRenderer::area>([](int scale) {
        return scale * 7;
    });
    int ra2 = renderer->area(3);
    std::printf("  area(3) = %d\n", ra2);

    // --- Use both interchangeably via operator->.
    std::printf("\n=== Both via operator-> ===\n");
    int circle_area = circle->area(1);
    int renderer_area = renderer->area(1);
    std::printf("  circle.area(1) = %d\n", circle_area);
    std::printf("  renderer.area(1) = %d\n", renderer_area);

    return 0;
}
