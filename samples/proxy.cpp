// Sample: structural proxy — call a concrete type through an interface
// it does not inherit from, using a type-erased Object.
//
// IDrawable is a declaration-only interface.  Square and Triangle are
// concrete types with compatible methods but no inheritance relationship
// to IDrawable.  Proxy<IDrawable> synthesizes a typed dispatch struct from
// IDrawable's interface, then an owning Object is bound — all type/signature
// checking happens at runtime via the reflection pool.
//
// Two construction paths are shown:
// 1. shared_ptr<T> → Object (implicit, idiomatic for known types)
// 2. Constructor::call → Object (fully type-erased, for runtime dispatch)
#include <refl/dyn.hpp>
#include <cstdio>
#include <memory>

// Interface — declaration only, no bodies.  Proxy never calls these;
// they exist solely for compile-time signature extraction via reflection.
// No link error: the methods are never odr-used (Proxy reflects their
// signatures, not their addresses).
struct IDrawable {
    int render(int scale) const;
    void set_tint(int t);
};

// Implementation — same method names and signatures, no inheritance.
struct Square {
    int side;
    int tint = 1;
    Square(int s) : side(s) {}
    int render(int scale) const { return side * side * scale * tint; }
    void set_tint(int t) { tint = t; }
};

// A second implementation with a different shape.
struct Triangle {
    int base, height;
    int tint = 1;
    Triangle(int b, int h) : base(b), height(h) {}
    int render(int scale) const { return base * height * scale * tint / 2; }
    void set_tint(int t) { tint = t; }
};

// Register both types at static-init time.
[[maybe_unused]] static refl::Reg<Square> reg_square;
[[maybe_unused]] static refl::Reg<Triangle> reg_triangle;

int main() {
    // Path 1: shared_ptr<T> → Object (implicit conversion).
    // The idiomatic way when you know the type at compile time.
    std::printf("=== Proxy<IDrawable> → Square(4) via shared_ptr ===\n");
    auto sp = std::make_shared<Square>(4);
    refl::Proxy<IDrawable> p(sp);
    std::printf("is_bound = %d\n", p.is_bound());

    int r1 = p->render(2);
    std::printf("render(2) = %d  (4*4*2*1 = 32)\n", r1);

    p->set_tint(3);
    int r2 = p->render(2);
    std::printf("render(2) after tint=3 = %d  (4*4*2*3 = 96)\n", r2);

    // Path 2: Constructor::call → Object (fully type-erased).
    // Look up by name, construct by name.  Proxy<IDrawable> never sees
    // "Triangle" in its own code — it receives an Object and resolves
    // everything at runtime.
    auto tri_cls = *refl::find_class("Triangle");
    auto tri_obj = *tri_cls.constructors()[0].call(6, 8);

    std::printf("\n=== Proxy<IDrawable> → Triangle(6, 8) via Constructor::call ===\n");
    refl::Proxy<IDrawable> p2(tri_obj);
    int r3 = p2->render(2);
    std::printf("render(2) = %d  (6*8*2*1/2 = 48)\n", r3);

    p2->set_tint(5);
    int r4 = p2->render(1);
    std::printf("render(1) after tint=5 = %d  (6*8*1*5/2 = 120)\n", r4);

    // Rebind: swap the object behind a proxy at runtime.
    // tri_obj was mutated by p2 above (set_tint(5)), so render reflects
    // the updated state — the Object is shared, not copied.
    std::printf("\n=== Rebind: Square → Triangle ===\n");
    p.bind(tri_obj);
    int r5 = p->render(1);
    std::printf("render(1) after rebind = %d  (6*8*1*5/2 = 120, tint=5 from p2)\n", r5);

    // Unbound proxy is safe to inspect.
    refl::Proxy<IDrawable> empty;
    std::printf("\n=== unbound proxy ===\n");
    std::printf("is_bound = %d\n", empty.is_bound());

    return 0;
}

