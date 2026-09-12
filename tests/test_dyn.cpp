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

// For const-ness validation tests: interface declares const, impl doesn't.
struct IConstMethod { int compute(int) const; };
struct NonConstImpl { int compute(int x) { return x; } };

// For const-impl-satisfies-nonconst-interface test: a const impl method is a
// valid (stronger) match for a non-const interface declaration — calling a
// const method on a non-const object is fine.
struct INonConstSet { int set(int); };
struct ConstSetImpl { int v; ConstSetImpl() : v(0) {} int set(int x) const { return x; } };

// For non-owning/shared_ptr Proxy tests.
struct ISimple { int get_value(); };
struct SimpleImpl { int v; SimpleImpl(int v) : v(v) {} int get_value() { return v; } };
// Second impl type with the same ISimple interface — lets a failed-rebind test
// distinguish which object the proxy reports/dispatches to by class name.
struct SimpleImplB { int v; SimpleImplB(int v) : v(v) {} int get_value() { return v; } };

// For cross-base ambiguous name-lookup rejection: DiamondLeft and DiamondRight
// each declare a method of the same name with distinct signatures.  A class
// inheriting both has two base subobjects with that name — an ambiguous lookup
// that C++ rejects.  The proxy must reject the bind rather than silently pick
// one base's invoker.
struct IAmbigBase { int method(int); };
struct DiamondLeft  { int method(int x) const { return x + 1; } };
struct DiamondRight { int method(int x) const { return x + 2; } };
struct DiamondAmbig : DiamondLeft, DiamondRight {};

// For cross-base unambiguous inherited method: DiamondMid declares the only
// own method of a given name, hiding the same-named method in its base, so the
// lookup is unambiguous and must bind to DiamondMid's implementation.
struct IDiamondOverride { int method(int); };
struct DiamondBase { int method(int x) const { return x + 100; } };
struct DiamondMid : DiamondBase { int method(int x) const { return x + 10; } };

// For Proxy operator tests.
struct IVec2 {
    int x, y;
    IVec2(int x, int y) : x(x), y(y) {}
    IVec2 operator+(const IVec2& o) const { return IVec2(x + o.x, y + o.y); }
    bool operator==(const IVec2& o) const { return x == o.x && y == o.y; }
    bool operator<(const IVec2& o) const { return x + y < o.x + o.y; }
};
struct Vec2Impl {
    int x, y;
    Vec2Impl(int x, int y) : x(x), y(y) {}
    Vec2Impl operator+(const Vec2Impl& o) const { return Vec2Impl(x + o.x, y + o.y); }
    bool operator==(const Vec2Impl& o) const { return x == o.x && y == o.y; }
    bool operator<(const Vec2Impl& o) const { return x + y < o.x + o.y; }
};
// A different impl type — same operator signatures, different type name.
struct Vec2Other {
    int x, y;
    Vec2Other(int x, int y) : x(x), y(y) {}
    Vec2Other operator+(const Vec2Other& o) const { return Vec2Other(x + o.x, y + o.y); }
    bool operator==(const Vec2Other& o) const { return x == o.x && y == o.y; }
    bool operator<(const Vec2Other& o) const { return x + y < o.x + o.y; }
};

// For Proxy operator+(concrete value) tests — interface with operator+(int).
struct IScalable {
    int v;
    IScalable(int v) : v(v) {}
    IScalable operator*(int s) const { return IScalable(v * s); }
};
struct ScalableImpl {
    int v;
    ScalableImpl(int v) : v(v) {}
    ScalableImpl operator*(int s) const { return ScalableImpl(v * s); }
};

// For Proxy overloaded-operator dispatch tests — operator+(int) and
// operator+(double) with distinct return values so we can tell which
// overload ran.  Returns non-class types (int) to verify the raw-value
// return path.
struct IOverOp {
    int v;
    IOverOp(int v) : v(v) {}
    int operator+(int x) const;
    int operator+(double x) const;
};
struct OverOpImpl {
    int v;
    OverOpImpl(int v) : v(v) {}
    // Declared in reverse order vs the interface — dispatch must match
    // by argument type, not by declaration order.
    int operator+(double) const { return 7070; }
    int operator+(int) const { return 1010; }
};

// For Proxy operator return-type mismatch tests — interface declares a
// primitive return, impl returns a different primitive type.  The proxy
// must detect the mismatch at call time, not reinterpret the storage.
struct IIntAdd { int v; IIntAdd(int v) : v(v) {} int operator+(int) const; };
struct LongAddImpl {
    int v;
    LongAddImpl(int v) : v(v) {}
    long operator+(int x) const { return static_cast<long>(v) + x; }
};

// For Proxy readonly-array operator[] tests — a const array member must
// not expose a mutable operator[] that bypasses the Readonly contract.
struct IConstArr {
    const std::array<int, 2> data;
    IConstArr() : data{10, 20} {}
};
struct ConstArrImpl {
    const std::array<int, 2> data;
    ConstArrImpl() : data{10, 20} {}
};

// For const-Proxy operator[] tests — a non-readonly array member accessed
// through a const Proxy must yield a const reference (read-only element
// access), matching the const-correctness of operator->() const.
struct IArr {
    std::array<int, 2> data;
    IArr() : data{10, 20} {}
};
struct ArrImpl {
    std::array<int, 2> data;
    ArrImpl() : data{10, 20} {}
};

// For reference-return tests — a method returning T& must preserve the
// reference through the proxy, not silently copy.  Writing through the
// returned reference must mutate the underlying object.
struct RefGet {
    int x;
    RefGet() : x(42) {}
    int& get_ref() { return x; }
    int val() const { return x; }
};

// For move-only after_set tests — on_change on a move-only member must
// not crash when the getter is null.  The hook fires with the value
// that was just set instead.
struct MoveOnlyProp {
    std::unique_ptr<int> ptr;
    MoveOnlyProp() : ptr(std::make_unique<int>(0)) {}
};

[[maybe_unused]] static refl::Dyn<Point> reg_point;
[[maybe_unused]] static refl::Dyn<Mixed> reg_mixed;
[[maybe_unused]] static refl::Reg<OverloadImpl> reg_overload_impl;
[[maybe_unused]] static refl::Reg<HasPartial> reg_has_partial;
[[maybe_unused]] static refl::Reg<DoubleReturn> reg_double_return;
[[maybe_unused]] static refl::Reg<NonConstImpl> reg_nonconst_impl;
[[maybe_unused]] static refl::Reg<ConstSetImpl> reg_const_set;
[[maybe_unused]] static refl::Reg<SimpleImpl> reg_simple_impl;
[[maybe_unused]] static refl::Reg<SimpleImplB> reg_simple_impl_b;
[[maybe_unused]] static refl::Reg<DiamondAmbig> reg_diamond_ambig;
[[maybe_unused]] static refl::Reg<DiamondMid> reg_diamond_mid;
[[maybe_unused]] static refl::Reg<Vec2Impl> reg_vec2_impl;
[[maybe_unused]] static refl::Reg<Vec2Other> reg_vec2_other;
[[maybe_unused]] static refl::Reg<ScalableImpl> reg_scalable_impl;
[[maybe_unused]] static refl::Reg<OverOpImpl> reg_overop_impl;
[[maybe_unused]] static refl::Reg<LongAddImpl> reg_long_add;
[[maybe_unused]] static refl::Reg<ConstArrImpl> reg_const_arr;
[[maybe_unused]] static refl::Reg<ArrImpl> reg_arr;
[[maybe_unused]] static refl::Dyn<RefGet> reg_ref_get;
[[maybe_unused]] static refl::Dyn<MoveOnlyProp> reg_move_only;

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } } while (0)

// Trait: does a TypedProperty expose operator[]?  Uses void_t (SFINAE)
// rather than a requires-expression, because GCC 16.2 reports a hard
// error (not a soft false) when a deducing-this candidate's constraints
// fail inside a requires-expression.
template <typename, typename = void>
struct has_subscript : std::false_type {};
template <typename P>
struct has_subscript<P, std::void_t<decltype(std::declval<P&>()[std::size_t{}])>>
    : std::true_type {};

// Trait: can the element returned by operator[] be assigned to?  False
// for readonly properties, whose operator[] yields a const reference.
template <typename, typename = void>
struct is_assignable_subscript : std::false_type {};
template <typename P>
struct is_assignable_subscript<P,
        std::void_t<decltype(std::declval<P&>()[std::size_t{}] = int{})>>
    : std::true_type {};

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

    // === static members accessed via get_class() ===
    // Statics are class-level, not instance-level — they live on refl::Class,
    // not the dispatch struct.
    auto rp_cls = rp.get_class();
    CHECK(rp_cls.valid(), "get_class should return a valid Class for Point");

    auto ic_field = *rp_cls.find_static_field("instance_count");
    int ic = *ic_field.get()->cast_ref<int>().value();
    CHECK(ic >= 1, "instance_count via get_class should be >= 1");
    (void)ic_field.set(50);
    CHECK(Point::instance_count == 50, "after instance_count=50, static should be 50");

    auto max_field = *rp_cls.find_static_field("max_instances");
    int max = *max_field.get()->cast_ref<int>().value();
    CHECK(max == 100, "max_instances should be 100");
    CHECK(max_field.is_const(), "max_instances should be const (readonly)");

    auto gi_fn = *rp_cls.find_static_function("get_instance_count");
    int gi = *gi_fn.invoke()->cast_ref<int>().value();
    CHECK(gi == 50, "get_instance_count() via get_class should be 50");

    (void)(*rp_cls.find_static_function("reset_count")).invoke();
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

    // Const-ness mismatch — must throw at bind time.
    // Interface declares compute(int) const, impl declares compute(int) non-const.
    auto nc_cls = *refl::find_class("NonConstImpl");
    auto nc_obj = *nc_cls.constructors()[0].call();
    bool threw_const = false;
    try {
        refl::Proxy<IConstMethod> bad3(nc_obj);
    } catch (const std::runtime_error&) {
        threw_const = true;
    }
    CHECK(threw_const, "Proxy<IConstMethod> should throw — const-ness mismatch (interface const, impl non-const)");

    // === Proxy: const impl satisfies non-const interface declaration ===
    // A const impl method is a valid (stronger) match for a non-const
    // interface: calling a const method on a non-const object is fine, so
    // the bind should succeed and the method should be callable.
    {
        auto sp = std::make_shared<ConstSetImpl>();
        refl::Proxy<INonConstSet> p(sp);
        CHECK(p->set(7) == 7, "const impl for non-const interface should bind and call");
    }

    // === Proxy: non-owning Object rejected ===
    // A non-owning Object (borrow) must be rejected — the proxy outlives it.
    SimpleImpl stack_impl(42);
    refl::Object borrowed(stack_impl);  // non-owning borrow
    CHECK(!borrowed.is_owned(), "borrowed Object should not be owned");
    bool threw_borrow = false;
    try {
        refl::Proxy<ISimple> bad3(borrowed);
    } catch (const std::runtime_error&) {
        threw_borrow = true;
    }
    CHECK(threw_borrow, "Proxy should reject non-owning Object");

    // shared_ptr path works — implicit conversion to owning Object.
    auto sp = std::make_shared<SimpleImpl>(42);
    refl::Proxy<ISimple> psp(sp);
    CHECK(psp.is_bound(), "Proxy from shared_ptr should be bound");
    CHECK(psp->get_value() == 42, "shared_ptr-backed Proxy get_value() should be 42");

    // === Proxy: failed rebind leaves proxy bound to original object ===
    // bind() validates ownership before replacing obj_, so a rejected
    // (non-owning) rebind must not disturb the existing binding: get_class
    // and calls should still report/dispatch to the original object.
    {
        auto sp1 = std::make_shared<SimpleImpl>(11);
        refl::Proxy<ISimple> p(sp1);
        CHECK(p->get_value() == 11, "original binding value 11");

        SimpleImplB stack_obj(22);
        refl::Object borrowed(stack_obj);  // non-owning, different type
        bool threw = false;
        try {
            p.bind(borrowed);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "rebind to non-owning Object should throw");
        CHECK(p.is_bound(), "proxy should still be bound after failed rebind");
        CHECK(std::string(p.get_class().name()) == "SimpleImpl",
            "get_class should still report original type SimpleImpl after failed rebind");
        CHECK(p->get_value() == 11,
            "calls should still dispatch to original object (value 11) after failed rebind");
    }

    // === Proxy: cross-base ambiguous name-lookup rejected at bind ===
    // DiamondAmbig inherits two bases each declaring method(int).  The lookup
    // hits two distinct base subobjects — an ambiguity C++ rejects — so the
    // proxy must refuse to bind rather than silently pick one base's invoker.
    {
        auto sp = std::make_shared<DiamondAmbig>();
        bool threw = false;
        try {
            refl::Proxy<IAmbigBase> p(sp);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "Proxy<IAmbigBase> on DiamondAmbig should throw — ambiguous base lookup");
    }

    // === Proxy: name-hiding makes an inherited method unambiguous ===
    // DiamondMid declares its own method(int), hiding DiamondBase::method(int).
    // The lookup is unambiguous (one subobject owns the name) and must bind to
    // DiamondMid's implementation, not the hidden base.
    {
        auto sp = std::make_shared<DiamondMid>();
        refl::Proxy<IDiamondOverride> p(sp);
        CHECK(p->method(1) == 11,
            "name-hiding: DiamondMid::method(1) should be 11, not 101 from base");
    }

    // === Proxy operators ===
    // Happy path: same impl type on both proxies.
    {
        auto sp1 = std::make_shared<Vec2Impl>(1, 2);
        auto sp2 = std::make_shared<Vec2Impl>(3, 4);
        refl::Proxy<IVec2> pv1(sp1);
        refl::Proxy<IVec2> pv2(sp2);

        // operator+ (Proxy + Proxy) — returns Proxy<IVec2>
        auto sum = pv1 + pv2;
        CHECK(sum->x == 4, "pv1 + pv2: x should be 4 (1+3)");
        CHECK(sum->y == 6, "pv1 + pv2: y should be 6 (2+4)");

        // operator== (Proxy == Proxy)
        bool eq = pv1 == pv2;
        CHECK(!eq, "pv1 == pv2 should be false");
        refl::Proxy<IVec2> pv1b(sp1);
        bool eq2 = pv1 == pv1b;
        CHECK(eq2, "pv1 == pv1b should be true (same values)");

        // operator< (Proxy < Proxy)
        bool lt = pv1 < pv2;
        CHECK(lt, "pv1 < pv2 should be true (3 < 7)");
    }

    // Exception path: mismatched impl types — the invoker's type check
    // should catch it at call time and throw std::runtime_error.
    {
        auto sp1 = std::make_shared<Vec2Impl>(1, 2);
        auto sp2 = std::make_shared<Vec2Other>(3, 4);
        refl::Proxy<IVec2> pv1(sp1);
        refl::Proxy<IVec2> pv2(sp2);  // different impl type, same interface

        bool threw = false;
        try {
            auto sum = pv1 + pv2;
            (void)sum;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "pv1 + pv2 with mismatched impls should throw runtime_error");
    }

    // === Proxy operator with concrete value ===
    // operator*(int) — the generic template<U> path, not Proxy+Proxy.
    {
        auto sp = std::make_shared<ScalableImpl>(5);
        refl::Proxy<IScalable> p(sp);
        auto scaled = p * 3;
        CHECK(scaled->v == 15, "p * 3 should be 15 (5*3)");
    }

    // === Proxy overloaded-operator dispatch ===
    // operator+(int) and operator+(double) must dispatch to the correct
    // overload based on the argument type, not the declaration order.
    // Returns int (non-class) — verifies the raw-value return path.
    {
        auto sp = std::make_shared<OverOpImpl>(1);
        refl::Proxy<IOverOp> p(sp);

        // int argument → operator+(int), returns 1010
        int ri = p + 3;
        CHECK(ri == 1010, "p + 3 (int) should dispatch to operator+(int) -> 1010");

        // double argument → operator+(double), returns 7070
        int rd = p + 3.0;
        CHECK(rd == 7070, "p + 3.0 (double) should dispatch to operator+(double) -> 7070");
    }

    // === Proxy: operator primitive return-type mismatch throws at call time ===
    // Interface declares operator+(int) returning int; impl returns long.
    // The proxy must not reinterpret the long storage as int — it must throw.
    {
        auto sp = std::make_shared<LongAddImpl>(1);
        refl::Proxy<IIntAdd> p(sp);
        bool threw = false;
        try {
            int r = p + 41;
            (void)r;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "operator+ with mismatched primitive return (int vs long) should throw");
    }

    // === Proxy: readonly array operator[] — read-only element access ===
    // A const array member has Readonly=true. operator[] is still exposed
    // but yields a const reference: element reads work, element writes are a
    // compile error — the same contract as whole-object operator= (deleted
    // for Readonly).  This avoids a copy when reading individual elements of
    // a const container.
    {
        auto sp = std::make_shared<ConstArrImpl>();
        refl::Proxy<IConstArr> p(sp);
        using DataProp = decltype(p->data);
        CHECK(DataProp::is_readonly(), "const array data should be Readonly");
        // operator[] exists for both readonly and non-readonly subscriptable T.
        static_assert(has_subscript<DataProp>::value,
            "readonly array must expose operator[] (read)");
        static_assert(has_subscript<refl::TypedProperty<std::array<int,2>, false>>::value,
            "non-readonly array must expose operator[]");
        // Element reads through the const overload:
        CHECK(p->data[0] == 10, "readonly array element read: data[0] == 10");
        CHECK(p->data[1] == 20, "readonly array element read: data[1] == 20");
        // Element writes through a readonly array are a compile error:
        static_assert(!is_assignable_subscript<DataProp>::value,
            "readonly array element must not be assignable (const ref)");
        static_assert(is_assignable_subscript<refl::TypedProperty<std::array<int,2>, false>>::value,
            "non-readonly array element must be assignable");
        // Read the whole array via the copy conversion still works too.
        std::array<int, 2> copy = p->data;
        CHECK(copy[1] == 20, "readonly array read via copy: data[1] == 20");
    }

    // === Proxy: const-proxy operator[] — const-correctness on non-readonly
    // arrays ===
    // A non-readonly array member accessed through a const Proxy must yield
    // a const reference: element reads work, element writes are a compile
    // error.  This matches operator->() const as a read-only view — before
    // the fix, const Proxy<T> still allowed mutation through operator[].
    {
        auto sp = std::make_shared<ArrImpl>();
        const refl::Proxy<IArr> cp(sp);
        // Element reads work through the const proxy:
        CHECK(cp->data[0] == 10, "const proxy: non-readonly array element read data[0] == 10");
        CHECK(cp->data[1] == 20, "const proxy: non-readonly array element read data[1] == 20");
        // operator[] on a const proxy yields a const reference (compile-time
        // check on the return type — the [0] call is an operator invocation,
        // so decltype preserves cv-qualifiers through the return type).
        static_assert(std::is_const_v<std::remove_reference_t<
            decltype(cp->data[std::size_t{}])>>,
            "const proxy operator[] must yield a const reference");
        static_assert(!std::is_assignable_v<
            decltype(cp->data[std::size_t{}]), int>,
            "const proxy element must not be assignable");
        // The same member on a non-const proxy yields a mutable reference:
        refl::Proxy<IArr> mp(sp);
        static_assert(!std::is_const_v<std::remove_reference_t<
            decltype(mp->data[std::size_t{}])>>,
            "non-const proxy operator[] must yield a mutable reference");
        static_assert(std::is_assignable_v<
            decltype(mp->data[std::size_t{}]), int>,
            "non-const proxy element must be assignable");
        mp->data[0] = 99;
        CHECK(mp->data[0] == 99, "non-const proxy element write: data[0] == 99");
        // The const proxy sees the mutation (same underlying object):
        CHECK(cp->data[0] == 99, "const proxy reads mutation from non-const proxy: data[0] == 99");
    }

    // === Proxy: reference-returning methods preserve references ===
    // A method declared int& get_ref() must return a real int& through
    // the proxy, not a silent copy.  Writing through the returned reference
    // must mutate the underlying object.
    {
        refl::Dyn<RefGet> d;
        d.reset();
        d.get().x = 7;
        auto& ref = d->get_ref();
        static_assert(std::is_same_v<decltype(ref), int&>,
            "reference-returning method must preserve the reference type");
        ref = 99;
        CHECK(d.get().x == 99,
            "writing through reference return should mutate object (x == 99)");
    }

    // === Proxy: const-proxy can call const methods ===
    // Before the fix, operator() was not const-qualified, so calling even
    // a const method through a const Proxy failed to compile.
    {
        refl::Dyn<RefGet> d;
        d.reset();
        d.get().x = 42;
        const refl::Dyn<RefGet>& cd = d;
        int v = cd->val();  // const method through const proxy
        CHECK(v == 42, "const proxy should call const method val() -> 42");
    }

    // === Dyn: on_change on move-only member does not crash ===
    // The getter is null for move-only members (not copy-constructible).
    // Before the fix, after_set called getter(obj) unconditionally —
    // null function pointer dereference, segfault.  After the fix, the
    // hook fires with the value that was just set.
    {
        refl::Dyn<MoveOnlyProp> m;
        m.reset();
        int hook_val = 0;
        m.on_change("ptr", [&hook_val](refl::Object& v) {
            // The value is a unique_ptr<int> — read via cast_ref.
            auto cr = v.template cast_ref<std::unique_ptr<int>>();
            if (cr) hook_val = **cr.value();
        });
        m->ptr = std::make_unique<int>(55);
        CHECK(hook_val == 55,
            "on_change on move-only member should fire with set value (55)");
        CHECK(*m.get().ptr == 55,
            "move-only member should be set to 55");
    }

    // === Proxy: const-correctness — const proxy blocks non-const methods ===
    // A const Proxy<T> yields a const view: const methods work, non-const
    // methods throw at call time.  Before the fix, operator() was
    // unconditionally const and there was no per-overload const gate, so a
    // const proxy could call non-const methods and mutate the object.
    {
        auto sp = std::make_shared<RefGet>();
        sp->x = 42;
        refl::Proxy<RefGet> p(sp);
        // Non-const proxy: both methods work.
        CHECK(p->val() == 42, "non-const proxy: val() should return 42");
        p->get_ref() = 99;
        CHECK(p->val() == 99, "non-const proxy: get_ref() write should mutate");

        // Const proxy: const method works, non-const method throws.
        sp->x = 42;
        const refl::Proxy<RefGet> cp(sp);
        CHECK(cp->val() == 42, "const proxy: val() (const) should return 42");
        bool threw_nonconst = false;
        try {
            cp->get_ref();
        } catch (const std::runtime_error&) {
            threw_nonconst = true;
        }
        CHECK(threw_nonconst,
            "const proxy: get_ref() (non-const) should throw");
    }

    // === Proxy: moved-from proxy is safe — throws, not UB ===
    // After move, the moved-from Proxy is unbound: is_bound() returns
    // false and calls through operator->() throw, rather than calling
    // through stale pointers (use-after-free).  Before the fix, the
    // dispatch fields were not cleared on move, so a call on the
    // moved-from proxy used stale obj/owner pointers.
    {
        auto sp = std::make_shared<SimpleImpl>(42);
        refl::Proxy<ISimple> p(sp);
        CHECK(p.is_bound(), "proxy should be bound before move");
        refl::Proxy<ISimple> moved(std::move(p));
        CHECK(!p.is_bound(), "moved-from proxy should not be bound");
        CHECK(moved.is_bound(), "moved-to proxy should be bound");
        CHECK(moved->get_value() == 42, "moved-to proxy should work");

        bool threw = false;
        try {
            (void)p->get_value();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw, "moved-from proxy call should throw, not UB");
    }

    std::printf("dyn dispatch test ok\n");
    return 0;
}
