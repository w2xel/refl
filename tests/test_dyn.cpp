// Dyn<T> dispatch test: typed method calls, properties, hooks (connect /
// on_change / emit), dynamic properties, runtime method implementation
// (mocking), real-to-dynamic-to-real switching, and cross-object connect.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/dyn.hpp>
#include <any>
#include <array>
#include <cstdio>
#include <cstdlib>

struct Base {
    int base_val;
    Base() : base_val(0) {}
    Base(int v) : base_val(v) {}
    int base_method() const { return base_val * 2; }
};

struct Point : Base {
    int x;
    int y;
    std::array<int, 3> coords = {0, 0, 0};
    const int id = 42;
    static int instance_count;
    static const int max_instances = 100;
    Point(int x, int y) : Base(x), x(x), y(y), id(0) { ++instance_count; }
    int sum() const { return x + y; }
    void set(int a, int b) { x = a; y = b; }
    void set(int a) { x = a; }
    static int get_instance_count() { return instance_count; }
    static void reset_count() { instance_count = 0; }
};

int Point::instance_count = 0;

struct Mixed {
    int v;
    Mixed(int v) : v(v) {}
    int compute(int f) const { return v * f; }
    double compute(double f) const { return v * f; }
};

struct IShape {
    virtual int area(int scale) = 0;
    virtual void set_color(int c) = 0;
    virtual ~IShape() = default;
};

// Interface with overloaded method, declared in one order.
struct IOverload {
    int compute(int);
    int compute(int, int);
};

// Impl with the same overloads declared in REVERSED order.
// Without signature matching, the invokers would be paired by position
// and p->compute(3) would call the 2-arg invoker — wrong.
struct OverloadImpl {
    int compute(int a, int b) { return a * 10 + b; }
    int compute(int a) { return a * 100; }
};

// For bind-time validation tests: interface with a method the impl lacks.
struct IMissing { int exists(int); int missing(int); };
struct HasPartial { int exists(int x) { return x; } };

// For bind-time validation tests: return type mismatch.
struct IBadReturn { int compute(int); };
struct DoubleReturn { double compute(int x) { return x * 1.5; } };

[[maybe_unused]] static refl::Dyn<Point> reg_point;
[[maybe_unused]] static refl::Dyn<Mixed> reg_mixed;
[[maybe_unused]] static refl::Reg<OverloadImpl> reg_overload_impl;
[[maybe_unused]] static refl::Reg<HasPartial> reg_has_partial;
[[maybe_unused]] static refl::Reg<DoubleReturn> reg_double_return;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } } while (0)

int main() {
    // === Dyn<T> dispatch-struct tests ===

    // --- construct via Dyn<T> args constructor ---
    refl::Dyn<Point> rp(1, 2);
    CHECK(rp.get().x == 1, "Dyn<Point> get().x should be 1");
    CHECK(rp.get().y == 2, "Dyn<Point> get().y should be 2");

    // --- typed method call via -> (real return type, no any_cast!) ---
    int sum_result = rp->sum();
    CHECK(sum_result == 3, "rp->sum() should be 3");

    // --- overloaded methods via -> ---
    rp->set(50);
    CHECK(rp.get().x == 50, "after rp->set(50), x should be 50");
    rp->set(10, 20);
    CHECK(rp.get().x == 10, "after rp->set(10,20), x should be 10");
    CHECK(rp.get().y == 20, "after rp->set(10,20), y should be 20");

    // --- property get/set via -> (member-like syntax) ---
    int xval = rp->x;
    CHECK(xval == 10, "rp->x should be 10 (implicit conversion)");
    rp->x = 99;
    CHECK(rp.get().x == 99, "after rp->x = 99, x should be 99");

    // --- readonly property (compile-time rejected assignment) ---
    CHECK(rp->id.is_readonly(), "id property should be readonly");
    int id_val = rp->id;
    CHECK(id_val == 0, "rp->id should be 0 (Point ctor sets id=0)");
    // rp->id = 100;  // COMPILE ERROR: operator= deleted for Readonly=true
    // Verify the readonly constraint at compile time:
    static_assert(decltype(rp->id)::is_readonly(), "id must be Readonly=true");
    static_assert(!decltype(rp->x)::is_readonly(), "x must be Readonly=false");

    // --- connect (function call hook) ---
    int hook_result = 0;
    rp.connect("sum", [&hook_result](refl::Object& r) {
        hook_result = *r.template cast_ref<int>().value();
    });
    (void)rp->sum();
    CHECK(hook_result == 119, "connect hook should fire after sum() with result 119 (99+20)");

    // --- on_change (property change hook) ---
    int change_result = 0;
    rp.on_change("x", [&change_result](refl::Object& v) {
        change_result = *v.template cast_ref<int>().value();
    });
    rp->x = 42;
    CHECK(change_result == 42, "on_change hook should fire with new value 42");
    CHECK(rp.get().x == 42, "after on_change set, x should be 42");

    // --- registration still works via default constructor ---
    // (Dyn<Point> reg_point above + rp(1,2) registered Point)
    CHECK(refl::find_class("Point").has_value(), "Point should still be registered");

    // === operator[] on array members ===
    rp->coords[0] = 10;
    rp->coords[1] = 20;
    rp->coords[2] = 30;
    CHECK(rp.get().coords[0] == 10, "coords[0] should be 10");
    CHECK(rp.get().coords[1] == 20, "coords[1] should be 20");
    CHECK(rp.get().coords[2] == 30, "coords[2] should be 30");
    int c1 = rp->coords[1];
    CHECK(c1 == 20, "coords[1] read via operator[] should be 20");

    // === static members in the dispatch struct ===
    int ic = rp->instance_count;
    CHECK(ic >= 1, "instance_count via dispatch should be >= 1");
    rp->instance_count = 50;
    CHECK(Point::instance_count == 50, "after instance_count=50, static should be 50");

    int max = rp->max_instances;
    CHECK(max == 100, "max_instances should be 100");
    // rp->max_instances = 200;  // COMPILE ERROR — readonly
    static_assert(decltype(rp->max_instances)::is_readonly(), "max must be readonly");
    static_assert(!decltype(rp->instance_count)::is_readonly(), "instance_count must be writable");

    int gi = rp->get_instance_count();
    CHECK(gi == 50, "get_instance_count() via dispatch should be 50");

    rp->reset_count();
    CHECK(Point::instance_count == 0, "after reset_count(), instance_count should be 0");

    // === swap via reset() ===
    rp.reset(100, 200);
    CHECK(rp.get().x == 100, "after reset(100,200), x should be 100");
    CHECK(rp.get().y == 200, "after reset(100,200), y should be 200");
    rp->set(5, 6);
    CHECK(rp.get().x == 5, "after set(5,6) on swapped object, x should be 5");
    rp->coords[0] = 999;
    CHECK(rp.get().coords[0] == 999, "coords[0] on swapped object should be 999");

    // === typed overload dispatch: Mixed::compute(int) returns int, compute(double) returns double ===
    // No variant — the return type is picked by argument type at compile time.
    refl::Dyn<Mixed> rm(5);
    int ci = rm->compute(3);
    CHECK(ci == 15, "compute(3) should be 15 (5*3)");
    double cd = rm->compute(3.0);
    CHECK(cd == 15.0, "compute(3.0) should be 15.0 (5*3.0)");

    // === dynamic interface implementation ===
    // Dyn<IShape> with an abstract T — no object constructed, methods
    // implemented via runtime callables.
    refl::Dyn<IShape> ishape;
    ishape.implement<^^IShape::area>([](refl::Dyn<IShape>&, int scale) {
        return scale * 100;
    });
    ishape.implement<^^IShape::set_color>([](refl::Dyn<IShape>&, int) {
        // no-op for test
    });

    int ar = ishape->area(5);
    CHECK(ar == 500, "dynamic area(5) should be 500 (5*100)");
    ishape->set_color(42);  // should not crash
    CHECK(true, "set_color called successfully");

    // Re-implement at runtime
    ishape.implement<^^IShape::area>([](refl::Dyn<IShape>&, int scale) {
        return scale * 200;
    });
    int ar2 = ishape->area(5);
    CHECK(ar2 == 1000, "dynamic area(5) after re-implement should be 1000");

    // === real-to-dynamic-to-real switching ===
    // Start with a real Point, switch to dynamic (mock), then back.
    refl::Dyn<Point> rp2(3, 4);
    CHECK(!rp2.is_dynamic(), "rp2 should start in real mode");
    CHECK(rp2->sum() == 7, "real sum() should be 7 (3+4)");

    // Per-method override: keep real object, override just sum().
    rp2.implement<^^Point::sum>([](refl::Dyn<Point>&) { return 999; });
    CHECK(!rp2.is_dynamic(), "rp2 should NOT be in full dynamic mode (partial override)");
    CHECK(rp2->sum() == 999, "overridden sum() should be 999");
    rp2->set(10, 20);
    CHECK(rp2.get().x == 10, "set() should still call real object (x=10)");
    CHECK(rp2.get().y == 20, "set() should still call real object (y=20)");

    // Restore the real sum() — removes the override.
    rp2.restore<^^Point::sum>();
    CHECK(rp2->sum() == 30, "restored sum() should be 30 (10+20)");

    // Full dynamic mode: make_dynamic() then implement everything.
    rp2.make_dynamic();
    CHECK(rp2.is_dynamic(), "rp2 should be in dynamic mode after make_dynamic");
    rp2.implement<^^Point::sum>([](refl::Dyn<Point>&) { return 42; });
    CHECK(rp2->sum() == 42, "mocked sum() should be 42");
    // Note: ^^Point::set can't be used — it's an overload set.
    rp2.implement<^^Point::sum>([](refl::Dyn<Point>&) { return 84; });
    CHECK(rp2->sum() == 84, "re-implemented sum() should be 84");

    // Switch back to real mode.
    rp2.reset(1, 2);
    CHECK(!rp2.is_dynamic(), "rp2 should be in real mode after reset");
    CHECK(rp2->sum() == 3, "real sum() should be 3 (1+2)");

    // String-based implement (no ^^ syntax, compile-time checked).
    rp2.implement<"sum">([](refl::Dyn<Point>&) { return 777; });
    CHECK(rp2->sum() == 777, "string-based implement sum() should be 777");
    rp2.restore<^^Point::sum>();
    CHECK(rp2->sum() == 3, "restored sum() should be 3 again");

    // Verify the self reference can access the real object.
    rp2.implement<"sum">([](refl::Dyn<Point>& self) {
        return self.get().x + self.get().y + 100;
    });
    CHECK(rp2->sum() == 103, "self-ref sum() should be 103 (1+2+100)");
    rp2.restore<^^Point::sum>();
    CHECK(rp2->sum() == 3, "restored sum() should be 3 after self-ref test");

    // === multi-listener hooks ===
    int hook_a = 0, hook_b = 0;
    rp2.connect("sum", [&hook_a](refl::Object& r) {
        hook_a = *r.template cast_ref<int>().value();
    });
    rp2.connect("sum", [&hook_b](refl::Object& r) {
        hook_b = *r.template cast_ref<int>().value();
    });
    (void)rp2->sum();
    CHECK(hook_a == 3, "multi-listener hook A should fire with 3");
    CHECK(hook_b == 3, "multi-listener hook B should fire with 3");

    // === explicit emit ===
    int emit_result = 0;
    rp2.connect("custom_signal", [&emit_result](refl::Object& v) {
        emit_result = *v.template cast_ref<int>().value();
    });
    rp2.emit("custom_signal", 42);
    CHECK(emit_result == 42, "emit should fire connected callbacks");

    // === dynamic properties ===
    rp2.set_property("dynamic_val", 123);
    int dp_val = *rp2.get_property("dynamic_val").template cast_ref<int>().value();
    CHECK(dp_val == 123, "dynamic property should be 123");

    // dynamic property with on_change
    int dyn_change = 0;
    rp2.on_change("dynamic_val", [&dyn_change](refl::Object& v) {
        dyn_change = *v.template cast_ref<int>().value();
    });
    rp2.set_property("dynamic_val", 456);
    CHECK(dyn_change == 456, "on_change should fire on dynamic property set");

    // === cross-object connect ===
    refl::Dyn<Point> rp3(10, 20);
    int cross_result = 0;
    rp3.connect("sum", [&cross_result](refl::Object& r) {
        cross_result = *r.template cast_ref<int>().value();
    });
    // Connect rp2's "sum" to rp3's "sum" — when rp2->sum() fires,
    // rp3's hooks receive rp2's result (forwarded via emit).
    rp2.connect("sum", &rp3, "sum");
    (void)rp2->sum();
    // rp2->sum() returns 3 (1+2), forwarded to rp3's hook.
    CHECK(cross_result == 3, "cross-object connect: rp3 hook should receive rp2's result (3)");

    // === Proxy: signature-matched overload binding ===
    // IOverload declares compute(int) then compute(int,int).
    // OverloadImpl declares them REVERSED: compute(int,int) then compute(int).
    // Signature matching must pair them correctly despite the reordering.
    auto ov_cls = *refl::find_class("OverloadImpl");
    auto ov_obj = *ov_cls.constructors()[0].call();
    refl::Proxy<IOverload> po(ov_obj);
    int oc1 = po->compute(3);
    CHECK(oc1 == 300, "compute(3) should be 300 (3*100), not 30 (wrong invoker)");
    int oc2 = po->compute(3, 2);
    CHECK(oc2 == 32, "compute(3,2) should be 32 (3*10+2), not wrong invoker");

    // === Proxy: bind-time validation throws on mismatch ===
    // Interface has a method the impl doesn't — must throw at bind time.
    auto hp_cls = *refl::find_class("HasPartial");
    auto hp_obj = *hp_cls.constructors()[0].call();
    bool threw_missing = false;
    try {
        refl::Proxy<IMissing> bad(hp_obj);
    } catch (const std::runtime_error&) {
        threw_missing = true;
    }
    CHECK(threw_missing, "Proxy<IMissing> should throw — HasPartial lacks 'missing'");

    // Return type mismatch — must throw at bind time.
    auto dr_cls = *refl::find_class("DoubleReturn");
    auto dr_obj = *dr_cls.constructors()[0].call();
    bool threw_return = false;
    try {
        refl::Proxy<IBadReturn> bad2(dr_obj);
    } catch (const std::runtime_error&) {
        threw_return = true;
    }
    CHECK(threw_return, "Proxy<IBadReturn> should throw — return type mismatch (int vs double)");

    std::printf("dyn dispatch test ok\n");
    return 0;
}
