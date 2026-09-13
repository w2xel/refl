// Sample: typed dispatch and dynamic implementation with Dyn<T>.
//
// Demonstrates:
// 1. Dyn<T> dispatch struct — typed method calls, member-like property
//    access.
// 2. Building an object that implements an abstract interface at runtime.
// 3. Starting with a real object, then switching to a mock at runtime.
// 4. Switching back to a real object after mocking.
#include <refl/dyn.hpp>
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

// A rect with overloaded resize, for the dispatch-struct demo.
struct Rect {
    int w;
    int h;
    Rect(int w, int h) : w(w), h(h) {}
    void resize(int nw, int nh) { w = nw; h = nh; }
    void resize(int sq) { w = sq; h = sq; }
};

[[maybe_unused]] static refl::Dyn<Circle> reg_circle;
[[maybe_unused]] static refl::Dyn<IRenderer> reg_renderer;
[[maybe_unused]] static refl::Dyn<Rect> reg_rect;

int main() {
    // --- Dyn<T> dispatch struct: typed calls.
    // Hooks are user-side: wrap your lambda in implement.
    std::printf("=== Dyn<Rect> dispatch (3, 4) ===\n");
    refl::Dyn<Rect> r(3, 4);
    r->resize(5, 6);
    std::printf("  after resize(5,6): w=%d h=%d\n", r.get().w, r.get().h);
    r->resize(7);
    std::printf("  after resize(7): w=%d h=%d\n", r.get().w, r.get().h);

    // --- Real object: Dyn<Circle> with a concrete instance.
    std::printf("\n=== Real Circle (radius=5) ===\n");
    refl::Dyn<Circle> circle(5);
    int ca = circle->area(2);
    std::printf("  area(2) = %d\n", ca);
    circle->set_color(42);
    int circle_color = circle->color;
    std::printf("  color = %d\n", circle_color);
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());

    // --- Switch to dynamic (mock) mode.
    std::printf("\n=== Mock Circle ===\n");
    int mock_color = 0;
    circle.implement<^^Circle::area>([](refl::Dyn<Circle>::Self&, int scale) {
        return scale * 1000;  // mock: always 1000*scale
    });
    circle.implement<^^Circle::set_color>([&mock_color](refl::Dyn<Circle>::Self&, int c) {
        mock_color = c;
    });
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());
    int ma = circle->area(2);
    std::printf("  mock area(2) = %d\n", ma);
    circle->set_color(77);
    std::printf("  mock_color = %d\n", mock_color);

    // --- Switch back to real mode.
    std::printf("\n=== Back to Real Circle (radius=10) ===\n");
    circle.reset_native<Circle>(10);
    std::printf("  is_dynamic = %d\n", circle.is_dynamic());
    int ra = circle->area(1);
    std::printf("  real area(1) = %d\n", ra);

    // --- Dynamic interface: Dyn<IRenderer> implemented at runtime.
    std::printf("\n=== Dynamic IRenderer (abstract) ===\n");
    refl::Dyn<IRenderer> renderer;
    int stored_color = 0;
    int fake_radius = 7;

    renderer.implement<^^IRenderer::area>([fake_radius](refl::Dyn<IRenderer>::Self&, int scale) {
        return scale * fake_radius * fake_radius;
    });
    renderer.implement<^^IRenderer::set_color>([&stored_color](refl::Dyn<IRenderer>::Self&, int c) {
        stored_color = c;
        std::printf("  set_color(%d) — stored\n", c);
    });

    int r_area = renderer->area(3);
    std::printf("  area(3) = %d\n", r_area);
    renderer->set_color(99);
    std::printf("  stored_color = %d\n", stored_color);

    // Re-implement at runtime.
    std::printf("\n=== Re-implemented IRenderer ===\n");
    renderer.implement<^^IRenderer::area>([](refl::Dyn<IRenderer>::Self&, int scale) {
        return scale * 7;
    });
    int r_area2 = renderer->area(3);
    std::printf("  area(3) = %d\n", r_area2);

    return 0;
}
