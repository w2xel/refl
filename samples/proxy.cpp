// Sample: structural proxy — call a concrete type through an abstract
// interface it does not inherit from.
//
// IDrawable is an abstract interface (pure virtual, unimplemented).
// Square is a concrete type with compatible methods but no inheritance
// relationship to IDrawable.  Proxy<IDrawable> synthesizes a typed dispatch
// struct from IDrawable's interface, then bind<Square>(...) wires Square's
// invokers into those fields.  Calls through the proxy dispatch to
// Square's methods with IDrawable's typed return types.
#include <refl/dyn.hpp>
#include <cstdio>

// Interface — abstract, cannot be constructed.
struct IDrawable {
    virtual int render(int scale) = 0;
    virtual void set_tint(int t) = 0;
    virtual ~IDrawable() = default;
};

// Implementation — same method names and signatures, but does NOT
// inherit IDrawable.  The proxy matches structurally by name.
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

int main() {
    // Proxy<IDrawable> with a Square behind it.
    refl::Proxy<IDrawable> p;
    p.bind<Square>(4);

    std::printf("=== Proxy<IDrawable> → Square(4) ===\n");
    std::printf("is_bound = %d\n", p.is_bound());

    int r1 = p->render(2);
    std::printf("render(2) = %d  (4*4*2*1 = 32)\n", r1);

    p->set_tint(3);
    int r2 = p->render(2);
    std::printf("render(2) after tint=3 = %d  (4*4*2*3 = 96)\n", r2);

    // Same proxy type, different implementation.
    refl::Proxy<IDrawable> p2;
    p2.bind<Triangle>(6, 8);

    std::printf("\n=== Proxy<IDrawable> → Triangle(6, 8) ===\n");
    int r3 = p2->render(2);
    std::printf("render(2) = %d  (6*8*2*1/2 = 48)\n", r3);

    p2->set_tint(5);
    int r4 = p2->render(1);
    std::printf("render(1) after tint=5 = %d  (6*8*1*5/2 = 120)\n", r4);

    // Unbound proxy is safe to inspect.
    refl::Proxy<IDrawable> empty;
    std::printf("\n=== unbound proxy ===\n");
    std::printf("is_bound = %d\n", empty.is_bound());

    return 0;
}
