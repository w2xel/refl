// Core reflection API test: register a class, find it by name, find a
// constructor, construct an instance (type-erased Object), find a function,
// invoke it, find a field, get/set it, cast back, resolve overloads, walk
// inheritance, and reflect enums.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/refl.hpp>
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

enum Color { Red = 10, Green = 20, Blue = 30 };

struct Mixed {
    int v;
    Mixed(int v) : v(v) {}
    int compute(int f) const { return v * f; }
    double compute(double f) const { return v * f; }
};

// Two classes with the same unqualified name in different namespaces —
// must not collide in the pool or in cast_safe.
namespace alpha { struct Widget { int id; Widget(int i) : id(i) {} int val() const { return id; } }; }
namespace beta  { struct Widget { int id; Widget(int i) : id(i) {} int val() const { return id * 10; } }; }

// Multi-inheritance: two bases at different offsets.
struct Left  { int lv; Left() : lv(0) {} Left(int v) : lv(v) {} int lmethod() const { return lv * 3; } };
struct Right { int rv; Right() : rv(0) {} Right(int v) : rv(v) {} int rmethod() const { return rv * 5; } };
struct Diamond : Left, Right {
    int d;
    Diamond(int l, int r, int d) : Left(l), Right(r), d(d) {}
    int dmethod() const { return d; }
};

// 3-level transitive: Deep -> Mid -> {Left, Right}
struct Mid : Left, Right {
    int m;
    Mid(int l, int r, int m) : Left(l), Right(r), m(m) {}
    int mmethod() const { return m; }
};
struct Deep : Mid {
    int dp;
    Deep(int l, int r, int m, int dp) : Mid(l, r, m), dp(dp) {}
    int dmethod() const { return dp; }
};

[[maybe_unused]] static refl::Reg<Base> reg_base;
[[maybe_unused]] static refl::Reg<Point> reg_point;
[[maybe_unused]] static refl::Reg<Color> reg_color;
[[maybe_unused]] static refl::Reg<Mixed> reg_mixed;
[[maybe_unused]] static refl::Reg<alpha::Widget> reg_alpha_widget;
[[maybe_unused]] static refl::Reg<beta::Widget> reg_beta_widget;
[[maybe_unused]] static refl::Reg<Left> reg_left;
[[maybe_unused]] static refl::Reg<Right> reg_right;
[[maybe_unused]] static refl::Reg<Diamond> reg_diamond;
[[maybe_unused]] static refl::Reg<Mid> reg_mid;
[[maybe_unused]] static refl::Reg<Deep> reg_deep;

struct Wrong {};

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } } while (0)

int main() {
    // --- list_all_classes ---
    auto all = refl::list_all_classes();
    CHECK(all.size() >= 2, "should have at least 2 classes registered");
    bool found_point = false;
    for (const auto& n : all) if (n == "Point") found_point = true;
    CHECK(found_point, "Point should be in list_all_classes");

    // --- find_class ---
    auto cls = *refl::find_class("Point");
    CHECK(cls.name() == "Point", "class name should be \"Point\"");

    // --- fields ---
    const auto& fields = cls.fields();
    CHECK(fields.size() == 4, "Point should have 4 fields (x, y, coords, id)");

    // --- find_constructor ---
    auto ctor = *cls.find_constructor({"int", "int"});
    CHECK(ctor.param_types().size() == 2, "constructor should have 2 params");

    // --- default constructor (on Base, which has one) ---
    auto base_cls = *refl::find_class("Base");
    auto def_ctor = base_cls.find_constructor({});
    CHECK(def_ctor.has_value(), "find_constructor({}) (default) should succeed on Base");
    CHECK(def_ctor->param_types().size() == 0, "default ctor has 0 params");

    // --- construct ---
    auto obj = *ctor.call(1, 3);
    CHECK(obj.class_name() == "Point", "object class name should be \"Point\"");

    // --- safe cast ---
    auto safe = obj.cast_safe<Point>();
    CHECK(safe.has_value(), "cast_safe<Point> should succeed");
    auto p = safe.value();
    CHECK(p->x == 1, "constructed x should be 1");
    CHECK(p->y == 3, "constructed y should be 3");
    CHECK(p->sum() == 4, "safe-cast sum should be 4");

    auto bad_cast = obj.cast_safe<Wrong>();
    CHECK(!bad_cast.has_value(), "cast_safe<Wrong> should fail");
    CHECK(bad_cast.error() == refl::Error::TypeError, "should be TypeError");

    // --- is_class ---
    CHECK(obj.is_class("Point"), "is_class(Point) should be true");
    CHECK(obj.is_class("Base"), "is_class(Base) should be true (Point derives from Base)");
    CHECK(!obj.is_class("Wrong"), "is_class(Wrong) should be false");

    // --- base cast to base class ---
    auto base_cast = obj.cast_safe<Base>();
    CHECK(base_cast.has_value(), "cast_safe<Base> on a Point should succeed");
    CHECK(base_cast.value()->base_val == 1, "base cast should see base_val=1");

    // --- type-checked invoke/get/set on a wrong-class Object ---
    auto base_obj = *def_ctor->call();
    auto wrong_invoke = cls.find_function("sum")->invoke(base_obj);
    CHECK(!wrong_invoke.has_value(), "invoke on wrong-class object should fail");
    CHECK(wrong_invoke.error() == refl::Error::TypeError, "should be TypeError");
    auto wrong_get = cls.find_field("x")->get(base_obj);
    CHECK(!wrong_get.has_value(), "get on wrong-class object should fail");
    CHECK(wrong_get.error() == refl::Error::TypeError, "should be TypeError");
    auto wrong_set = cls.find_field("x")->set(base_obj, std::any(1));
    CHECK(!wrong_set.has_value(), "set on wrong-class object should fail");
    CHECK(wrong_set.error() == refl::Error::TypeError, "should be TypeError");

    // --- overloaded function resolution ---
    auto set1 = cls.find_function("set", {"int"});
    CHECK(set1.has_value(), "find_function(\"set\", {\"int\"}) should succeed");
    auto set2 = cls.find_function("set", {"int", "int"});
    CHECK(set2.has_value(), "find_function(\"set\", {\"int\",\"int\"}) should succeed");
    CHECK(set1->param_types().size() == 1, "set(int) has 1 param");
    CHECK(set2->param_types().size() == 2, "set(int,int) has 2 params");

    // --- find_functions (all overloads) ---
    auto overloads = cls.find_functions("set");
    CHECK(overloads.size() == 2, "should find 2 overloads of set");

    // --- invoke overloaded ---
    std::any r1 = *set1->invoke(obj, 50);
    CHECK(p->x == 50, "after set(50), x should be 50");
    std::any r2 = *set2->invoke(obj, 10, 20);
    CHECK(p->x == 10, "after set(10,20), x should be 10");
    CHECK(p->y == 20, "after set(10,20), y should be 20");

    // --- function on Object ---
    auto fn = *cls.find_function("sum");
    CHECK(std::any_cast<int>(*fn.invoke(obj)) == 30, "sum(10,20) should be 30");

    // --- field get/set ---
    auto xf = *cls.find_field("x");
    CHECK(std::any_cast<int>(*xf.get(obj)) == 10, "field get x should be 10");
    (void)xf.set(obj, std::any(77));
    CHECK(p->x == 77, "after field set, x should be 77");

    // --- readonly field ---
    auto idf = *cls.find_field("id");
    CHECK(idf.is_readonly(), "id should be readonly");
    auto set_id = idf.set(obj, std::any(99));
    CHECK(!set_id.has_value(), "set on readonly should fail");
    CHECK(set_id.error() == refl::Error::ReadOnly, "should be ReadOnly");

    // --- inheritance ---
    CHECK(cls.bases().size() == 1, "Point should have 1 base");
    CHECK(cls.bases()[0].name == "Base", "base should be Base");

    // inherited field from Base
    auto base_field = cls.find_field("base_val");
    CHECK(base_field.has_value(), "find_field should find inherited base_val");
    CHECK(base_field->name() == "base_val", "inherited field name");

    // inherited method from Base
    auto base_fn = cls.find_function("base_method");
    CHECK(base_fn.has_value(), "find_function should find inherited base_method");
    std::any base_ret = *base_fn->invoke(obj);
    CHECK(std::any_cast<int>(base_ret) == 2, "base_method on Point(77) should be 154... wait");

    // base_method returns base_val * 2, and Base was constructed with x=1 in Point(1,3)
    // Wait — Point(1,3) calls Base(x) so Base::base_val = 1, base_method = 2
    CHECK(std::any_cast<int>(base_ret) == 2, "base_method should be 2 (base_val=1)");

    // --- enum reflection ---
    auto enums = refl::list_all_enums();
    CHECK(enums.size() >= 1, "should have at least 1 enum registered");
    bool found_color = false;
    for (const auto& n : enums) if (n == "Color") found_color = true;
    CHECK(found_color, "Color should be in list_all_enums");

    auto e = *refl::find_enum("Color");
    CHECK(e.name() == "Color", "enum name should be Color");
    CHECK(e.enumerators().size() == 3, "Color should have 3 enumerators");

    auto red = e.find_enumerator("Red");
    CHECK(red.has_value(), "find_enumerator(\"Red\") should succeed");
    CHECK(red->value() == 10, "Red should be 10");
    CHECK(red->name() == "Red", "enumerator name should be Red");

    auto green = e.find_enumerator(20);
    CHECK(green.has_value(), "find_enumerator(20) should succeed");
    CHECK(green->name() == "Green", "value 20 should be Green");

    auto bad_enum = e.find_enumerator("NoSuchColor");
    CHECK(!bad_enum.has_value(), "find_enumerator for non-existent should fail");
    CHECK(bad_enum.error() == refl::Error::NotFound, "should be NotFound");

    // --- static data members ---
    const auto& sfields = cls.static_fields();
    CHECK(sfields.size() == 2, "Point should have 2 static fields");

    auto sf = cls.find_static_field("instance_count");
    CHECK(sf.has_value(), "find_static_field(\"instance_count\") should succeed");
    CHECK(sf->name() == "instance_count", "static field name");
    CHECK(!sf->is_readonly(), "instance_count should not be readonly");
    // Point(1,3) was constructed once, so instance_count should be 1
    CHECK(std::any_cast<int>(*sf->get()) == 1, "instance_count should be 1");

    (void)sf->set(std::any(42));
    CHECK(std::any_cast<int>(*sf->get()) == 42, "after set, instance_count should be 42");

    // readonly static field (const)
    auto maxf = cls.find_static_field("max_instances");
    CHECK(maxf.has_value(), "find_static_field(\"max_instances\") should succeed");
    CHECK(maxf->is_readonly(), "max_instances should be readonly (const)");
    CHECK(std::any_cast<int>(*maxf->get()) == 100, "max_instances should be 100");
    auto set_max = maxf->set(std::any(200));
    CHECK(!set_max.has_value(), "set on readonly static should fail");
    CHECK(set_max.error() == refl::Error::ReadOnly, "should be ReadOnly");

    // --- static member functions ---
    auto sf_count = cls.find_static_function("get_instance_count");
    CHECK(sf_count.has_value(), "find_static_function(\"get_instance_count\") should succeed");
    CHECK(sf_count->return_type() == "int", "get_instance_count returns int");
    std::any sc_ret = *sf_count->invoke();
    CHECK(std::any_cast<int>(sc_ret) == 42, "get_instance_count should be 42");

    auto sf_reset = cls.find_static_function("reset_count");
    CHECK(sf_reset.has_value(), "find_static_function(\"reset_count\") should succeed");
    auto sr_ret = sf_reset->invoke();
    CHECK(sr_ret.has_value(), "reset_count should succeed");
    CHECK(!sr_ret->has_value(), "reset_count returns void, any should be empty");
    CHECK(Point::instance_count == 0, "after reset_count, instance_count should be 0");

    // --- constructors enumeration ---
    const auto& ctors = cls.constructors();
    CHECK(ctors.size() == 1, "Point should have 1 registered constructor (the 2-param one)");

    // --- functions enumeration (raw accessor) ---
    const auto& fns = cls.functions();
    CHECK(fns.size() >= 3, "Point should have at least 3 member functions (sum, set, set)");
    bool has_sum = false;
    for (const auto& f : fns) if (f.name == "sum") has_sum = true;
    CHECK(has_sum, "functions() should list sum");

    // --- find_static_function with param types ---
    // (Point has no overloaded static functions, so just verify the name-based lookup works)
    auto sf_by_params = cls.find_static_function("get_instance_count", {});
    CHECK(sf_by_params.has_value(), "find_static_function with empty types should succeed");

    // --- find_static_functions (all overloads) ---
    auto sf_overloads = cls.find_static_functions("get_instance_count");
    CHECK(sf_overloads.size() == 1, "should find 1 overload of get_instance_count");

    // --- static field/function base walk ---
    // Base has no static members; Point does.  Verify find_static_field works
    // when called from Base (should find Point's static via... wait, Base doesn't
    // derive from Point.  Let's just verify Base's own static lookup works.)
    // Actually, Base has no static members, so this tests the not-found path.
    auto base_cls2 = *refl::find_class("Base");
    CHECK(base_cls2.static_fields().empty(), "Base should have 0 static fields");
    CHECK(base_cls2.static_functions().empty(), "Base should have 0 static functions");

    // --- clone ---
    auto clone_result = obj.clone();
    CHECK(clone_result.has_value(), "clone should succeed");
    auto cloned = std::move(*clone_result);
    CHECK(cloned.valid(), "cloned object should be valid");
    CHECK(cloned.class_name() == "Point", "cloned class name should be Point");
    // cloned should have same field values as original
    auto cloned_p = cloned.cast_safe<Point>().value();
    CHECK(cloned_p->x == 77, "cloned x should match original (77)");
    CHECK(cloned_p->y == 20, "cloned y should match original (20)");
    // modifying clone should not affect original
    (void)xf.set(cloned, std::any(999));
    CHECK(p->x == 77, "original x should still be 77 after modifying clone");
    CHECK(cloned_p->x == 999, "cloned x should be 999");

    // --- to_string ---
    std::string ts = obj.to_string();
    CHECK(ts.substr(0, 14) == "Object(Point @", "to_string should start with Object(Point @");
    CHECK(ts.find("Point") != std::string::npos, "to_string should contain class name");

    // --- error cases ---
    CHECK(!refl::find_class("NoSuchClass").has_value(), "non-existent class should fail");
    CHECK(!cls.find_constructor({"double"}).has_value(), "wrong ctor types should fail");
    CHECK(!cls.find_function("no_such_function").has_value(), "non-existent function should fail");
    CHECK(!cls.find_field("no_such_field").has_value(), "non-existent field should fail");

    // === qualified names: same unqualified name in different namespaces ===
    auto a_cls = refl::find_class("alpha::Widget");
    CHECK(a_cls.has_value(), "find_class(\"alpha::Widget\") should succeed");
    auto b_cls = refl::find_class("beta::Widget");
    CHECK(b_cls.has_value(), "find_class(\"beta::Widget\") should succeed");
    CHECK(a_cls->name() != b_cls->name(), "alpha::Widget and beta::Widget must be distinct");
    CHECK(a_cls->name() == "alpha::Widget", "alpha name should be qualified");
    CHECK(b_cls->name() == "beta::Widget", "beta name should be qualified");
    // The unqualified "Widget" must NOT resolve to either.
    CHECK(!refl::find_class("Widget").has_value(), "unqualified Widget should not be findable");

    auto a_obj = *a_cls->find_constructor({"int"})->call(7);
    auto b_obj = *b_cls->find_constructor({"int"})->call(7);

    auto a_cast = a_obj.cast_safe<alpha::Widget>();
    CHECK(a_cast.has_value(), "cast_safe<alpha::Widget> on alpha object should succeed");
    CHECK(a_cast.value()->id == 7, "alpha Widget id should be 7");
    CHECK(a_cast.value()->val() == 7, "alpha Widget val() should be 7");

    auto b_cast = b_obj.cast_safe<beta::Widget>();
    CHECK(b_cast.has_value(), "cast_safe<beta::Widget> on beta object should succeed");
    CHECK(b_cast.value()->val() == 70, "beta Widget val() should be 70");

    // Cross-cast: alpha::Widget must NOT cast to beta::Widget.
    auto cross = a_obj.cast_safe<beta::Widget>();
    CHECK(!cross.has_value(), "cast_safe<beta::Widget> on alpha object should fail");
    CHECK(cross.error() == refl::Error::TypeError, "cross-namespace cast should be TypeError");

    // === arity + type guards (#5) ===
    // Too few args → ArityMismatch (not UB).
    auto few = ctor.call(1);
    CHECK(!few.has_value(), "call(1) on 2-param ctor should fail");
    CHECK(few.error() == refl::Error::ArityMismatch, "too few args should be ArityMismatch");
    // Too many args → ArityMismatch.
    auto many = ctor.call(1, 2, 3);
    CHECK(!many.has_value(), "call(1,2,3) on 2-param ctor should fail");
    CHECK(many.error() == refl::Error::ArityMismatch, "too many args should be ArityMismatch");

    auto fn2 = *cls.find_function("set", {"int", "int"});
    auto bad_arity = fn2.invoke(obj, 99);
    CHECK(!bad_arity.has_value(), "invoke with wrong arg count should fail");
    CHECK(bad_arity.error() == refl::Error::ArityMismatch, "wrong arity invoke should be ArityMismatch");

    // Wrong any type → TypeError (not bad_any_cast throw).
    auto bad_type = fn2.invoke(obj, std::any(1.5), std::any(2));
    CHECK(!bad_type.has_value(), "invoke with wrong arg type should fail");
    CHECK(bad_type.error() == refl::Error::TypeError, "wrong arg type should be TypeError");

    // Static function arity guard.
    auto bad_sfn = sf_count->invoke(1);
    CHECK(!bad_sfn.has_value(), "static invoke with wrong arg count should fail");
    CHECK(bad_sfn.error() == refl::Error::ArityMismatch, "static wrong arity should be ArityMismatch");

    // Field set with wrong any type → TypeError.
    auto bad_field = xf.set(obj, std::any(3.14));
    CHECK(!bad_field.has_value(), "field set with wrong type should fail");
    CHECK(bad_field.error() == refl::Error::TypeError, "field wrong type should be TypeError");

    // === multiple inheritance: offset-adjusted base access ===
    auto dia_cls = *refl::find_class("Diamond");
    auto dia_obj = *dia_cls.find_constructor({"int", "int", "int"})->call(10, 20, 30);
    CHECK(dia_obj.is_class("Diamond"), "is_class Diamond");
    CHECK(dia_obj.is_class("Left"), "is_class Left (Diamond derives from Left)");
    CHECK(dia_obj.is_class("Right"), "is_class Right (Diamond derives from Right)");

    // cast_safe to each base — must get the correct subobject, not the
    // first member of the wrong base.
    auto lp = dia_obj.cast_safe<Left>();
    CHECK(lp.has_value(), "cast_safe<Left> on Diamond should succeed");
    CHECK(lp.value()->lv == 10, "Left::lv should be 10");
    CHECK(lp.value()->lmethod() == 30, "Left::lmethod() should be 30 (10*3)");

    auto rp = dia_obj.cast_safe<Right>();
    CHECK(rp.has_value(), "cast_safe<Right> on Diamond should succeed");
    CHECK(rp.value()->rv == 20, "Right::rv should be 20");
    CHECK(rp.value()->rmethod() == 100, "Right::rmethod() should be 100 (20*5)");

    // Invoke inherited methods through the type-erased path.
    auto lfn = *dia_cls.find_function("lmethod");
    CHECK(std::any_cast<int>(*lfn.invoke(dia_obj)) == 30, "lmethod via invoke should be 30");
    auto rfn = *dia_cls.find_function("rmethod");
    CHECK(std::any_cast<int>(*rfn.invoke(dia_obj)) == 100, "rmethod via invoke should be 100");

    // Get/set inherited fields through the type-erased path.
    auto lfield = *dia_cls.find_field("lv");
    CHECK(std::any_cast<int>(*lfield.get(dia_obj)) == 10, "field get lv should be 10");
    auto rfield = *dia_cls.find_field("rv");
    CHECK(std::any_cast<int>(*rfield.get(dia_obj)) == 20, "field get rv should be 20");
    (void)lfield.set(dia_obj, std::any(99));
    CHECK(lp.value()->lv == 99, "after field set lv=99, Left::lv should be 99");
    (void)rfield.set(dia_obj, std::any(88));
    CHECK(rp.value()->rv == 88, "after field set rv=88, Right::rv should be 88");

    // === 3-level transitive: Deep -> Mid -> {Left, Right} ===
    auto deep_obj = *refl::find_class("Deep")
        ->find_constructor({"int", "int", "int", "int"})->call(1, 2, 3, 4);
    auto deep_lp = deep_obj.cast_safe<Left>();
    CHECK(deep_lp.has_value(), "cast_safe<Left> on Deep should succeed");
    CHECK(deep_lp.value()->lv == 1, "transitive Left::lv should be 1");
    CHECK(deep_lp.value()->lmethod() == 3, "transitive lmethod should be 3 (1*3)");
    auto deep_rp = deep_obj.cast_safe<Right>();
    CHECK(deep_rp.has_value(), "cast_safe<Right> on Deep should succeed");
    CHECK(deep_rp.value()->rv == 2, "transitive Right::rv should be 2");
    CHECK(deep_rp.value()->rmethod() == 10, "transitive rmethod should be 10 (2*5)");
    auto deep_mp = deep_obj.cast_safe<Mid>();
    CHECK(deep_mp.has_value(), "cast_safe<Mid> on Deep should succeed");
    CHECK(deep_mp.value()->m == 3, "transitive Mid::m should be 3");

    // Inherited method 2 levels up.
    auto deep_lfn = *refl::find_class("Deep")->find_function("lmethod");
    CHECK(std::any_cast<int>(*deep_lfn.invoke(deep_obj)) == 3, "transitive lmethod via invoke should be 3");

    std::printf("refl core API test ok\n");
    return 0;
}
