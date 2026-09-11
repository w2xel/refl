// Core reflection API test: register a class, find it by name, find a
// constructor, construct an instance (type-erased Object), find a function,
// invoke it, find a field, get/set it, cast back, resolve overloads, walk
// inheritance, and reflect enums.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/refl.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

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

// Operator support: operator+, operator==, operator[], operator+=
struct Vec {
    int x, y;
    Vec(int x, int y) : x(x), y(y) {}
    Vec operator+(const Vec& o) const { return Vec(x + o.x, y + o.y); }
    bool operator==(const Vec& o) const { return x == o.x && y == o.y; }
    int operator[](int i) const { return i == 0 ? x : y; }
    Vec& operator+=(const Vec& o) { x += o.x; y += o.y; return *this; }
};

// Public-only base walking: public, protected, private inheritance
struct PubBase { int pv; PubBase() : pv(0) {} PubBase(int v) : pv(v) {} int pmethod() const { return pv; } };
struct ProtBase { int rv; ProtBase() : rv(0) {} ProtBase(int v) : rv(v) {} int rmethod() const { return rv; } };
struct PrivBase { int iv; PrivBase() : iv(0) {} PrivBase(int v) : iv(v) {} int imethod() const { return iv; } };

struct AccessMixed : public PubBase, protected ProtBase, private PrivBase {
    int m;
    AccessMixed(int p, int r, int i, int m) : PubBase(p), ProtBase(r), PrivBase(i), m(m) {}
};

// A function taking a base-class argument — used to test derived-to-base
// argument conversion: passing a Point (derived) where Base is expected.
struct Receiver {
    int acc;
    Receiver() : acc(0) {}
    int add_base(Base b) { acc += b.base_val; return acc; }
};

// Class with a deleted default constructor — registration must not fail.
struct NoDefault {
    NoDefault() = delete;
    NoDefault(int v) : v(v) {}
    int v;
    int get() const { return v; }
};

// Class with a class-typed field — used to test field-set upcast.
struct Holder {
    Base b;
    Holder() : b() {}
    Holder(int v) : b(v) {}
};

// Move-only member: unique_ptr has no copy getter/setter — get_ref is the
// only access path.  Exercises the getter=nullptr / setter=nullptr branch.
struct MoveOnly {
    std::unique_ptr<int> ptr;
    int plain;
    MoveOnly(int v) : ptr(std::make_unique<int>(v)), plain(v) {}
    int get_val() const { return *ptr; }
};

// Conversion operator: must NOT be registered as a function.
struct WithConv {
    int v;
    WithConv(int v) : v(v) {}
    operator int() const { return v; }
    int get() const { return v; }
};

// Private/protected members must NOT be reflected — only public members
// are registered, matching the public-base filter on bases_of.
class Priv {
public:
    Priv() : pub(0) {}
    int pub;
    int pub_method() const { return pub * 2; }
    static int pub_static;
    static int pub_static_fn() { return 99; }
protected:
    int prot_field;
    int prot_method() const { return prot_field; }
    static int prot_static;
    static int prot_static_fn() { return 0; }
private:
    int priv_field;
    int priv_method() const { return priv_field; }
    static int priv_static;
    static int priv_static_fn() { return 0; }
};

int Priv::pub_static = 0;
int Priv::prot_static = 0;
int Priv::priv_static = 0;

// Base with a static member function — for find_static_functions base-walk.
struct StaticBase {
    static int sbval() { return 7; }
    int sb() const { return 1; }
};
struct StaticChild : StaticBase {
    int c;
    StaticChild() : c(0) {}
};

// Private override of a public virtual: the override is excluded from
// Derived's own function list, but find_function resolves it via the
// base walk to the public declaration.  Virtual dispatch still hits the
// private override at runtime — access is static, dispatch is dynamic.
struct VBase {
    int v;
    VBase() : v(0) {}
    VBase(int v) : v(v) {}
    virtual int method() const { return v * 10; }
    virtual ~VBase() = default;
};
struct VDerived : VBase {
    VDerived(int v) : VBase(v) {}
private:
    int method() const override { return v * 100; }
};

// Non-virtual private hide: base's public fn is found via base walk and
// called directly (no virtual dispatch, so the derived's hiding version
// is never reached through the base declaration).
struct NVBase {
    int v;
    NVBase() : v(0) {}
    NVBase(int v) : v(v) {}
    int fn() const { return v * 7; }
};
struct NVDerived : NVBase {
    NVDerived(int v) : NVBase(v) {}
private:
    int fn() const { return v * 77; }
};

// Name hiding with overloads: Derived declares set(int), which hides
// BOTH Base::set(int) and Base::set(int,int) in C++.  The framework's
// find_function, find_functions, and all_functions must respect this:
// base set overloads are not reachable through Derived.
struct HideBase {
    int v;
    HideBase() : v(0) {}
    HideBase(int v) : v(v) {}
    void set(int a) { v = a; }
    void set(int a, int b) { v = a + b; }
    int get() const { return v; }
};
struct HideDerived : HideBase {
    HideDerived(int v) : HideBase(v) {}
    void set(int a) { v = a * 100; }  // hides both Base::set overloads
};

// Diamond inheritance: DiamBottom inherits from both DiamLeft and DiamRight,
// which both inherit from DiamBase.  Without dedup, all_functions/all_fields
// would list DiamBase's members twice (once per path).
struct DiamBase {
    int v;
    static int dsval;
    DiamBase() : v(0) {}
    DiamBase(int v) : v(v) {}
    int dfn() const { return v; }
    static int dsfn() { return dsval; }
};
int DiamBase::dsval = 77;

struct DiamLeft : DiamBase {
    DiamLeft() {}
    DiamLeft(int v) : DiamBase(v) {}
};
struct DiamRight : DiamBase {
    DiamRight() {}
    DiamRight(int v) : DiamBase(v) {}
};
struct DiamBottom : DiamLeft, DiamRight {
    DiamBottom() {}
    DiamBottom(int l, int r) : DiamLeft(l), DiamRight(r) {}
};

// Field name hiding: a derived class redeclaring a field of the same name
// shadows the base field.  find_field and all_fields must return the
// derived's field (not the base's), and the base's other fields stay visible.
struct FieldHideBase { int x = 1; int y = 2; };
struct FieldHideDer : FieldHideBase { int x = 99; };

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
[[maybe_unused]] static refl::Reg<Vec> reg_vec;
[[maybe_unused]] static refl::Reg<PubBase> reg_pub;
[[maybe_unused]] static refl::Reg<ProtBase> reg_prot;
[[maybe_unused]] static refl::Reg<PrivBase> reg_priv;
[[maybe_unused]] static refl::Reg<AccessMixed> reg_access_mixed;
[[maybe_unused]] static refl::Reg<Receiver> reg_receiver;
[[maybe_unused]] static refl::Reg<NoDefault> reg_nodefault;
[[maybe_unused]] static refl::Reg<Holder> reg_holder;
[[maybe_unused]] static refl::Reg<MoveOnly> reg_move_only;
[[maybe_unused]] static refl::Reg<WithConv> reg_with_conv;
[[maybe_unused]] static refl::Reg<StaticBase> reg_static_base;
[[maybe_unused]] static refl::Reg<StaticChild> reg_static_child;
[[maybe_unused]] static refl::Reg<Priv> reg_priv_members;
[[maybe_unused]] static refl::Reg<VBase> reg_vbase;
[[maybe_unused]] static refl::Reg<VDerived> reg_vderived;
[[maybe_unused]] static refl::Reg<NVBase> reg_nvbase;
[[maybe_unused]] static refl::Reg<NVDerived> reg_nvderived;
[[maybe_unused]] static refl::Reg<HideBase> reg_hide_base;
[[maybe_unused]] static refl::Reg<HideDerived> reg_hide_derived;
[[maybe_unused]] static refl::Reg<DiamBase> reg_diam_base;
[[maybe_unused]] static refl::Reg<DiamLeft> reg_diam_left;
[[maybe_unused]] static refl::Reg<DiamRight> reg_diam_right;
[[maybe_unused]] static refl::Reg<DiamBottom> reg_diam_bottom;
[[maybe_unused]] static refl::Reg<FieldHideBase> reg_field_hide_base;
[[maybe_unused]] static refl::Reg<FieldHideDer> reg_field_hide_der;

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
    auto wrong_set = cls.find_field("x")->set(base_obj, 1);
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
    auto r1 = *set1->invoke(obj, 50);
    CHECK(p->x == 50, "after set(50), x should be 50");
    auto r2 = *set2->invoke(obj, 10, 20);
    CHECK(p->x == 10, "after set(10,20), x should be 10");
    CHECK(p->y == 20, "after set(10,20), y should be 20");

    // --- function on Object ---
    auto fn = *cls.find_function("sum");
    CHECK(*fn.invoke(obj)->cast_safe<int>().value() == 30, "sum(10,20) should be 30");

    // --- field get/set ---
    auto xf = *cls.find_field("x");
    CHECK(*xf.get(obj)->cast_safe<int>().value() == 10, "field get x should be 10");
    (void)xf.set(obj, 77);
    CHECK(p->x == 77, "after field set, x should be 77");

    // --- readonly field ---
    auto idf = *cls.find_field("id");
    CHECK(idf.is_const(), "id should be const");
    auto set_id = idf.set(obj, 99);
    CHECK(!set_id.has_value(), "set on readonly should fail");
    CHECK(set_id.error() == refl::Error::ReadOnly, "should be ReadOnly");

    // --- inheritance ---
    CHECK(cls.bases().size() == 1, "Point should have 1 base");
    CHECK(cls.bases()[0].name() == "Base", "base should be Base");

    // inherited field from Base
    auto base_field = cls.find_field("base_val");
    CHECK(base_field.has_value(), "find_field should find inherited base_val");
    CHECK(base_field->name() == "base_val", "inherited field name");

    // inherited method from Base
    auto base_fn = cls.find_function("base_method");
    CHECK(base_fn.has_value(), "find_function should find inherited base_method");
    auto base_ret = *base_fn->invoke(obj);
    CHECK(*base_ret.cast_safe<int>().value() == 2, "base_method on Point(77) should be 154... wait");

    // base_method returns base_val * 2, and Base was constructed with x=1 in Point(1,3)
    // Wait — Point(1,3) calls Base(x) so Base::base_val = 1, base_method = 2
    CHECK(*base_ret.cast_safe<int>().value() == 2, "base_method should be 2 (base_val=1)");

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
    CHECK(!sf->is_const(), "instance_count should not be const");
    // Point(1,3) was constructed once, so instance_count should be 1
    CHECK(*sf->get()->cast_safe<int>().value() == 1, "instance_count should be 1");

    (void)sf->set(42);
    CHECK(*sf->get()->cast_safe<int>().value() == 42, "after set, instance_count should be 42");

    // readonly static field (const)
    auto maxf = cls.find_static_field("max_instances");
    CHECK(maxf.has_value(), "find_static_field(\"max_instances\") should succeed");
    CHECK(maxf->is_const(), "max_instances should be const");
    CHECK(*maxf->get()->cast_safe<int>().value() == 100, "max_instances should be 100");
    auto set_max = maxf->set(200);
    CHECK(!set_max.has_value(), "set on readonly static should fail");
    CHECK(set_max.error() == refl::Error::ReadOnly, "should be ReadOnly");

    // --- static field get_ref (typed + untyped) ---
    // instance_count was set to 42 above; get_ref returns a pointer to the
    // live static storage.
    auto sf_ref = sf->get_ref();
    CHECK(sf_ref.has_value(), "untyped get_ref on static field should succeed");
    CHECK(*static_cast<int*>(sf_ref.value()) == 42, "get_ref should see 42");
    *static_cast<int*>(sf_ref.value()) = 77;
    CHECK(*sf->get()->cast_safe<int>().value() == 77, "after get_ref write, get should see 77");
    auto sf_tref = sf->get_ref<int>();
    CHECK(sf_tref.has_value(), "typed get_ref<int> on static field should succeed");
    CHECK(*sf_tref.value() == 77, "typed get_ref should see 77");
    auto sf_tref_wrong = sf->get_ref<double>();
    CHECK(!sf_tref_wrong.has_value(), "typed get_ref<double> on int static field should fail");
    CHECK(sf_tref_wrong.error() == refl::Error::TypeError, "should be TypeError");
    // get_ref on a const static member returns ReadOnly (no addressable storage).
    auto max_ref = maxf->get_ref();
    CHECK(!max_ref.has_value(), "get_ref on const static should fail");
    CHECK(max_ref.error() == refl::Error::ReadOnly, "const static get_ref should be ReadOnly");
    // Restore for the get_instance_count check below.
    (void)sf->set(42);

    // --- static member functions ---
    auto sf_count = cls.find_static_function("get_instance_count");
    CHECK(sf_count.has_value(), "find_static_function(\"get_instance_count\") should succeed");
    CHECK(sf_count->return_type() == "int", "get_instance_count returns int");
    auto sc_ret = *sf_count->invoke();
    CHECK(*sc_ret.cast_safe<int>().value() == 42, "get_instance_count should be 42");

    auto sf_reset = cls.find_static_function("reset_count");
    CHECK(sf_reset.has_value(), "find_static_function(\"reset_count\") should succeed");
    auto sr_ret = sf_reset->invoke();
    CHECK(sr_ret.has_value(), "reset_count should succeed");
    CHECK(!sr_ret->valid(), "reset_count returns void, Object should be invalid");
    CHECK(Point::instance_count == 0, "after reset_count, instance_count should be 0");

    // --- constructors enumeration ---
    const auto& ctors = cls.constructors();
    CHECK(ctors.size() == 1, "Point should have 1 registered constructor (the 2-param one)");

    // --- functions enumeration (raw accessor) ---
    const auto& fns = cls.functions();
    CHECK(fns.size() >= 3, "Point should have at least 3 member functions (sum, set, set)");
    bool has_sum = false;
    for (const auto& f : fns) if (f.name() == "sum") has_sum = true;
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
    (void)xf.set(cloned, 999);
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
    auto bad_type = fn2.invoke(obj, 1.5, 2);
    CHECK(!bad_type.has_value(), "invoke with wrong arg type should fail");
    CHECK(bad_type.error() == refl::Error::TypeError, "wrong arg type should be TypeError");

    // Static function arity guard.
    auto bad_sfn = sf_count->invoke(1);
    CHECK(!bad_sfn.has_value(), "static invoke with wrong arg count should fail");
    CHECK(bad_sfn.error() == refl::Error::ArityMismatch, "static wrong arity should be ArityMismatch");

    // Field set with wrong any type → TypeError.
    auto bad_field = xf.set(obj, 3.14);
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
    CHECK(*lfn.invoke(dia_obj)->cast_safe<int>().value() == 30, "lmethod via invoke should be 30");
    auto rfn = *dia_cls.find_function("rmethod");
    CHECK(*rfn.invoke(dia_obj)->cast_safe<int>().value() == 100, "rmethod via invoke should be 100");

    // Get/set inherited fields through the type-erased path.
    auto lfield = *dia_cls.find_field("lv");
    CHECK(*lfield.get(dia_obj)->cast_safe<int>().value() == 10, "field get lv should be 10");
    auto rfield = *dia_cls.find_field("rv");
    CHECK(*rfield.get(dia_obj)->cast_safe<int>().value() == 20, "field get rv should be 20");
    (void)lfield.set(dia_obj, 99);
    CHECK(lp.value()->lv == 99, "after field set lv=99, Left::lv should be 99");
    (void)rfield.set(dia_obj, 88);
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
    CHECK(*deep_lfn.invoke(deep_obj)->cast_safe<int>().value() == 3, "transitive lmethod via invoke should be 3");

    // === ObjectRef: field get/set and invoke on stack objects ===
    // No Object, no shared_ptr — just a concrete instance on the stack.
    Point stack_point(100, 200);

    // Field get on stack object — previously impossible (Field::get needed Object).
    auto stack_x = cls.find_field("x")->get(stack_point);
    CHECK(stack_x.has_value(), "field get on stack object should succeed");
    CHECK(*stack_x->cast_safe<int>().value() == 100, "stack field get x should be 100");

    // Field set on stack object — previously impossible.
    (void)cls.find_field("x")->set(stack_point, 555);
    CHECK(stack_point.x == 555, "stack field set x should be 555");

    // Invoke on stack object — previously needed invoke<T> overload.
    auto stack_sum = cls.find_function("sum")->invoke(stack_point);
    CHECK(stack_sum.has_value(), "invoke on stack object should succeed");
    CHECK(*stack_sum->cast_safe<int>().value() == 755, "stack invoke sum should be 755 (555+200)");

    // === NotOwned vs NotCopyable ===
    // cast_safe on a non-owning (borrowed) Object returns NotOwned, not
    // NotCopyable — the object isn't uncopyable, it just isn't owned.
    refl::Object borrowed(stack_point);
    auto borrowed_cast = borrowed.cast_safe<Point>();
    CHECK(!borrowed_cast.has_value(), "cast_safe on non-owning should fail");
    CHECK(borrowed_cast.error() == refl::Error::NotOwned, "non-owning cast should be NotOwned");
    // cast_ref still works on a non-owning Object.
    auto borrowed_ref = borrowed.cast_ref<Point>();
    CHECK(borrowed_ref.has_value(), "cast_ref on non-owning should succeed");
    CHECK(borrowed_ref.value()->x == 555, "borrowed ref x should be 555");

    // === Derived-to-base argument conversion ===
    // A function taking Base can be invoked with a Point (derived) —
    // the argument is upcast to the Base subobject automatically.
    auto recv_cls = *refl::find_class("Receiver");
    auto recv = *recv_cls.find_constructor({})->call();
    auto add_fn = *recv_cls.find_function("add_base");
    // Point(1,3) calls Base(x) so base_val=1; add_base copies Base by value.
    auto ab_ret = add_fn.invoke(recv, obj);
    CHECK(ab_ret.has_value(), "invoke add_base with Point (derived) should succeed");
    CHECK(*ab_ret->cast_safe<int>().value() == 1, "add_base should see base_val=1");
    // Passing a genuine Base should still work (exact match path).
    auto base_arg = *base_cls.find_constructor({"int"})->call(5);
    auto ab2_ret = add_fn.invoke(recv, base_arg);
    CHECK(ab2_ret.has_value(), "invoke add_base with Base (exact) should succeed");
    CHECK(*ab2_ret->cast_safe<int>().value() == 6, "add_base should accumulate to 6");
    // Passing an unrelated type should still fail.
    auto bad_arg_ret = add_fn.invoke(recv, 3.14);
    CHECK(!bad_arg_ret.has_value(), "invoke add_base with wrong type should fail");
    CHECK(bad_arg_ret.error() == refl::Error::TypeError, "wrong arg type should be TypeError");

    // === Typed get_ref ===
    auto xf2 = *cls.find_field("x");
    auto typed_ref = xf2.get_ref<int>(obj);
    CHECK(typed_ref.has_value(), "typed get_ref<int> should succeed");
    CHECK(*typed_ref.value() == 77, "typed get_ref x should be 77");
    *typed_ref.value() = 888;
    CHECK(p->x == 888, "after typed get_ref write, x should be 888");
    auto typed_wrong = xf2.get_ref<double>(obj);
    CHECK(!typed_wrong.has_value(), "typed get_ref<double> on int field should fail");
    CHECK(typed_wrong.error() == refl::Error::TypeError, "wrong type get_ref should be TypeError");

    // === Operators ===
    auto vec_cls = *refl::find_class("Vec");
    auto v1 = *vec_cls.find_constructor({"int", "int"})->call(3, 4);
    auto v2 = *vec_cls.find_constructor({"int", "int"})->call(1, 2);

    // operator+ — returns Vec by value
    auto add = vec_cls.find_function("operator+");
    CHECK(add.has_value(), "find operator+ should succeed");
    auto add_ret = add->invoke(v1, v2);
    CHECK(add_ret.has_value(), "invoke operator+ should succeed");
    CHECK(add_ret->valid(), "operator+ returns a value");
    auto add_obj = add_ret->cast_safe<Vec>().value();
    CHECK(add_obj->x == 4, "operator+ x should be 4");
    CHECK(add_obj->y == 6, "operator+ y should be 6");

    // operator== — returns bool
    auto eq = vec_cls.find_function("operator==");
    CHECK(eq.has_value(), "find operator== should succeed");
    auto eq_ret = eq->invoke(v1, v2);
    CHECK(eq_ret.has_value() && eq_ret->valid(), "operator== returns a value");
    CHECK(*eq_ret->cast_safe<bool>().value() == false, "3,4 != 1,2");

    // operator[] — returns int
    auto idx = vec_cls.find_function("operator[]");
    CHECK(idx.has_value(), "find operator[] should succeed");
    auto idx_ret = idx->invoke(v1, 0);
    CHECK(*idx_ret->cast_safe<int>().value() == 3, "operator[](0) should be 3");

    // operator+= — returns Vec& (reference), valid non-owning Object + mutation
    auto pe = vec_cls.find_function("operator+=");
    CHECK(pe.has_value(), "find operator+= should succeed");
    auto pe_ret = pe->invoke(v1, v2);
    CHECK(pe_ret.has_value(), "invoke operator+= should succeed");
    CHECK(pe_ret->valid(), "operator+= returns reference, Object should be valid");
    CHECK(pe_ret->is_owned(), "operator+= on owned Object returns aliasing (owned) Object");
    // The returned reference points at v1 (the object += was called on).
    auto pe_ref = pe_ret->cast_ref<Vec>().value();
    CHECK(pe_ref->x == 4, "operator+= return x should be 4");
    CHECK(pe_ref->y == 6, "operator+= return y should be 6");
    // Mutation should have happened on v1.
    auto vp = v1.cast_safe<Vec>().value();
    CHECK(vp->x == 4, "after += x should be 4");
    CHECK(vp->y == 6, "after += y should be 6");

    // operator= (defaulted) — should be findable
    auto assign = vec_cls.find_function("operator=");
    CHECK(assign.has_value(), "find operator= should succeed");

    // === Public-only base walking ===
    auto am_cls = *refl::find_class("AccessMixed");
    auto am_obj = *am_cls.find_constructor({"int", "int", "int", "int"})->call(10, 20, 30, 40);

    CHECK(am_obj.is_class("PubBase"), "is_class PubBase (public) should be true");
    CHECK(!am_obj.is_class("ProtBase"), "is_class ProtBase (protected) should be false");
    CHECK(!am_obj.is_class("PrivBase"), "is_class PrivBase (private) should be false");

    auto pub_cast = am_obj.cast_safe<PubBase>();
    CHECK(pub_cast.has_value(), "cast_safe<PubBase> should succeed");
    CHECK(pub_cast.value()->pv == 10, "PubBase::pv should be 10");

    CHECK(!am_obj.cast_safe<ProtBase>().has_value(), "cast_safe<ProtBase> should fail");
    CHECK(!am_obj.cast_safe<PrivBase>().has_value(), "cast_safe<PrivBase> should fail");

    auto pfn = am_cls.find_function("pmethod");
    CHECK(pfn.has_value(), "find pmethod from public base should succeed");
    CHECK(*pfn->invoke(am_obj)->cast_safe<int>().value() == 10, "pmethod should be 10");

    CHECK(!am_cls.find_function("rmethod").has_value(), "find rmethod from protected base should fail");
    CHECK(!am_cls.find_function("imethod").has_value(), "find imethod from private base should fail");

    // === Deleted constructor: registration must not fail ===
    auto nd_cls = *refl::find_class("NoDefault");
    auto nd_obj = nd_cls.find_constructor({"int"})->call(42);
    CHECK(nd_obj.has_value(), "class with deleted default ctor should register and construct");
    auto nd_ret = *nd_cls.find_function("get")->invoke(*nd_obj);
    CHECK(*nd_ret.cast_safe<int>().value() == 42, "NoDefault get() should return 42");
    CHECK(nd_cls.constructors().size() == 1, "NoDefault should have 1 registered ctor");

    // === Field set with derived-to-base value (upcast, matching invoke) ===
    auto holder_cls = *refl::find_class("Holder");
    auto holder = *holder_cls.find_constructor({"int"})->call(5);
    auto bfield = holder_cls.find_field("b");
    CHECK(bfield.has_value(), "find field b on Holder should succeed");
    // stack_point is a Point (derives from Base) — set should upcast + slice.
    auto set_derived = bfield->set(holder, stack_point);
    CHECK(set_derived.has_value(), "field set with derived value should upcast and succeed");
    auto holder_b = holder.cast_safe<Holder>().value();
    CHECK(holder_b->b.base_val == 100, "field set from Point should see base_val=100 (upcast + slice)");
    // Exact-type set still works.
    auto base_for_set = *base_cls.find_constructor({"int"})->call(7);
    auto base_sp = base_for_set.cast_safe<Base>().value();
    auto set_exact = bfield->set(holder, *base_sp);
    CHECK(set_exact.has_value(), "field set with exact Base value should succeed");
    CHECK(holder_b->b.base_val == 7, "after exact set, base_val should be 7");

    // === Move-only member: move-assignable set works, get is not available ===
    auto mo_cls = *refl::find_class("MoveOnly");
    auto mo_obj = *mo_cls.find_constructor({"int"})->call(42);
    auto mo_ptr_field = *mo_cls.find_field("ptr");
    CHECK(!mo_ptr_field.has_getter(), "unique_ptr field has no copy getter");
    CHECK(!mo_ptr_field.is_const(), "unique_ptr field is not const (has move setter)");
    CHECK(mo_ptr_field.has_setter(), "unique_ptr field has a move setter");
    // get is unavailable (move-only member, no copy getter).
    auto mo_get = mo_ptr_field.get(mo_obj);
    CHECK(!mo_get.has_value(), "get on move-only field should fail");
    CHECK(mo_get.error() == refl::Error::NotCopyable, "should be NotCopyable");
    // set SUCCEEDS — unique_ptr is move-assignable.
    auto mo_set = mo_ptr_field.set(mo_obj, std::make_unique<int>(9));
    CHECK(mo_set.has_value(), "set on move-only move-assignable field should succeed");
    // Verify the value changed via get_ref.
    auto mo_tref = mo_ptr_field.get_ref<std::unique_ptr<int>>(mo_obj);
    CHECK(mo_tref.has_value(), "typed get_ref<unique_ptr<int>> should succeed");
    CHECK(**mo_tref.value() == 9, "after set, move-only field should be 9");
    // Untyped get_ref (void*) — still works for direct access.
    auto mo_ref = mo_ptr_field.get_ref(mo_obj);
    CHECK(mo_ref.has_value(), "untyped get_ref on move-only field should succeed");
    auto* uptr = static_cast<std::unique_ptr<int>*>(mo_ref.value());
    CHECK(**uptr == 9, "move-only field value via get_ref should be 9 after set");
    auto mo_tref_wrong = mo_ptr_field.get_ref<int>(mo_obj);
    CHECK(!mo_tref_wrong.has_value(), "typed get_ref<int> on unique_ptr field should fail");
    CHECK(mo_tref_wrong.error() == refl::Error::TypeError, "should be TypeError");
    // Plain field on the same class still has a copy getter.
    auto mo_plain = *mo_cls.find_field("plain");
    CHECK(mo_plain.has_getter(), "plain field has getter");
    CHECK(*mo_plain.get(mo_obj)->cast_safe<int>().value() == 42, "plain field get should be 42");

    // === Untyped get_ref (void*) on a plain field ===
    auto plain_ref = xf2.get_ref(obj);
    CHECK(plain_ref.has_value(), "untyped get_ref on plain field should succeed");
    *static_cast<int*>(plain_ref.value()) = 1234;
    CHECK(p->x == 1234, "after untyped get_ref write, x should be 1234");

    // === Conversion operator must NOT be registered ===
    auto wc_cls = *refl::find_class("WithConv");
    CHECK(wc_cls.find_function("get").has_value(), "WithConv::get should be registered");
    CHECK(!wc_cls.find_function("operator int").has_value(),
          "conversion operator should NOT be registered");
    // No registered function may be the conversion.
    for (const auto& f : wc_cls.functions())
        CHECK(f.name() != "operator int", "no registered function may be the conversion");

    // === find_functions / find_static_functions walk bases ===
    auto p_overloads = cls.find_functions("base_method");
    CHECK(p_overloads.size() == 1, "find_functions should walk bases (base_method in Base)");
    auto sc_cls = *refl::find_class("StaticChild");
    auto sb_overloads = sc_cls.find_static_functions("sbval");
    CHECK(sb_overloads.size() == 1, "find_static_functions should walk bases (sbval in StaticBase)");
    auto sb_member = sc_cls.find_functions("sb");
    CHECK(sb_member.size() == 1, "find_functions should walk bases (sb in StaticBase)");

    // === const lvalue becomes owning (no mutable borrow of const) ===
    const Point const_pt(11, 22);
    refl::Object const_obj(const_pt);
    CHECK(const_obj.is_owned(), "const lvalue must not be a mutable borrow (owning copy)");
    auto cr = const_obj.cast_ref<Point>();
    CHECK(cr.has_value(), "cast_ref on const-derived (now owned) object should work");
    CHECK(cr.value()->x == 11, "const-borrow copy x should be 11");
    CHECK(cr.value()->y == 22, "const-borrow copy y should be 22");
    CHECK(const_pt.x == 11, "original const object must be unchanged");

    // === Private/protected members are excluded ===
    auto priv_cls = *refl::find_class("Priv");
    CHECK(priv_cls.fields().size() == 1, "Priv should have 1 public field (pub)");
    CHECK(priv_cls.find_field("pub").has_value(), "public field pub should be findable");
    CHECK(!priv_cls.find_field("prot_field").has_value(), "protected field must not be reflected");
    CHECK(!priv_cls.find_field("priv_field").has_value(), "private field must not be reflected");

    CHECK(priv_cls.static_fields().size() == 1, "Priv should have 1 public static field");
    CHECK(priv_cls.find_static_field("pub_static").has_value(), "public static field should be findable");
    CHECK(!priv_cls.find_static_field("prot_static").has_value(), "protected static field must not be reflected");
    CHECK(!priv_cls.find_static_field("priv_static").has_value(), "private static field must not be reflected");

    CHECK(priv_cls.find_function("pub_method").has_value(), "public method should be findable");
    CHECK(!priv_cls.find_function("prot_method").has_value(), "protected method must not be reflected");
    CHECK(!priv_cls.find_function("priv_method").has_value(), "private method must not be reflected");

    CHECK(priv_cls.find_static_function("pub_static_fn").has_value(), "public static fn should be findable");
    CHECK(!priv_cls.find_static_function("prot_static_fn").has_value(), "protected static fn must not be reflected");
    CHECK(!priv_cls.find_static_function("priv_static_fn").has_value(), "private static fn must not be reflected");

    // Public default constructor is still registered.
    CHECK(priv_cls.find_constructor({}).has_value(), "public default ctor should be registered");

    // === Private override of public virtual ===
    // The private override is not in VDerived's own function list, but
    // find_function resolves it via the base walk to VBase's public
    // declaration.  Virtual dispatch then hits the private override.
    auto vd_cls = *refl::find_class("VDerived");
    bool vd_has_method = false;
    for (const auto& f : vd_cls.functions())
        if (f.name() == "method") vd_has_method = true;
    CHECK(!vd_has_method, "private override should not appear in VDerived::functions()");
    auto vd_method = vd_cls.find_function("method");
    CHECK(vd_method.has_value(), "find_function should find 'method' via base walk");
    auto vd_obj = *vd_cls.find_constructor({"int"})->call(3);
    auto vd_ret = vd_method->invoke(vd_obj);
    CHECK(vd_ret.has_value(), "invoke method on VDerived should succeed");
    CHECK(*vd_ret->cast_safe<int>().value() == 300, "virtual dispatch should hit private override (3*100)");

    // On the base itself, the base version runs.
    auto vb_cls = *refl::find_class("VBase");
    auto vb_obj = *vb_cls.find_constructor({"int"})->call(3);
    auto vb_ret = vb_cls.find_function("method")->invoke(vb_obj);
    CHECK(*vb_ret->cast_safe<int>().value() == 30, "base method should be 30 (3*10)");

    // === Non-virtual private hide ===
    // The private hide is excluded; find_function finds NVBase's public fn
    // via base walk and calls it directly (no virtual dispatch).
    auto nvd_cls = *refl::find_class("NVDerived");
    bool nvd_has_fn = false;
    for (const auto& f : nvd_cls.functions())
        if (f.name() == "fn") nvd_has_fn = true;
    CHECK(!nvd_has_fn, "private hide should not appear in NVDerived::functions()");
    auto nvd_fn = nvd_cls.find_function("fn");
    CHECK(nvd_fn.has_value(), "find_function should find 'fn' via base walk");
    auto nvd_obj = *nvd_cls.find_constructor({"int"})->call(5);
    auto nvd_ret = nvd_fn->invoke(nvd_obj);
    CHECK(nvd_ret.has_value(), "invoke fn on NVDerived should succeed");
    CHECK(*nvd_ret->cast_safe<int>().value() == 35, "non-virtual hide should call base fn (5*7)");

    // === all_functions(): merged view across hierarchy ===
    // Point has sum, set, set (3 own — operator= is deleted due to const
    // id member); Base has base_method, operator=, operator= (3 inherited).
    // functions() returns only Point's own; all_functions() includes Base's.
    CHECK(cls.functions().size() == 3, "functions() should have 3 (sum, set, set)");
    auto all_fns = cls.all_functions();
    CHECK(all_fns.size() == 6, "all_functions() should have 6 (3 own + 3 inherited from Base)");

    // Verify base_method is present in the merged view.
    bool all_has_base_method = false;
    for (const auto& f : all_fns)
        if (f.name() == "base_method") all_has_base_method = true;
    CHECK(all_has_base_method, "all_functions() should include inherited base_method");

    // Find base_method in the merged vector and invoke it directly.
    for (const auto& f : all_fns) {
        if (f.name() == "base_method") {
            CHECK(f.valid(), "Function from all_functions() should be valid");
            auto ret = f.invoke(obj);
            CHECK(ret.has_value(), "invoke via all_functions() Function should succeed");
            CHECK(*ret->cast_safe<int>().value() == 2, "base_method via all_functions() should be 2");
            break;
        }
    }

    // === all_fields(): merged view across hierarchy ===
    // Point has x, y, coords, id (4 own); Base has base_val (1 inherited).
    CHECK(cls.fields().size() == 4, "fields() should have 4 (x, y, coords, id)");
    auto all_flds = cls.all_fields();
    CHECK(all_flds.size() == 5, "all_fields() should have 5 (4 own + 1 inherited from Base)");
    bool all_has_base_val = false;
    for (const auto& f : all_flds)
        if (f.name() == "base_val") all_has_base_val = true;
    CHECK(all_has_base_val, "all_fields() should include inherited base_val");
    // Invoke get on the inherited field via the merged view.
    for (const auto& f : all_flds) {
        if (f.name() == "base_val") {
            CHECK(f.valid(), "Field from all_fields() should be valid");
            auto v = f.get(obj);
            CHECK(v.has_value(), "get via all_fields() Field should succeed");
            CHECK(*v->cast_safe<int>().value() == 1, "base_val via all_fields() should be 1");
            break;
        }
    }

    // === all_static_fields(): merged view across hierarchy ===
    // Point has instance_count, max_instances (2 own); Base has none.
    CHECK(cls.static_fields().size() == 2, "static_fields() should have 2");
    auto all_sflds = cls.all_static_fields();
    CHECK(all_sflds.size() == 2, "all_static_fields() should have 2 (none inherited)");
    bool all_has_max = false;
    for (const auto& f : all_sflds)
        if (f.name() == "max_instances") all_has_max = true;
    CHECK(all_has_max, "all_static_fields() should include max_instances");

    // === all_static_functions(): merged view across hierarchy ===
    // StaticChild has none; StaticBase has sbval (1 inherited).
    auto sc_cls2 = *refl::find_class("StaticChild");
    CHECK(sc_cls2.static_functions().empty(), "StaticChild has 0 own static functions");
    auto all_sfns = sc_cls2.all_static_functions();
    CHECK(all_sfns.size() == 1, "all_static_functions() should have 1 (sbval from StaticBase)");

    for (const auto& sf : all_sfns) {
        if (sf.name() == "sbval") {
            CHECK(sf.valid(), "StaticFunction from all_static_functions() should be valid");
            auto ret = sf.invoke();
            CHECK(ret.has_value(), "invoke via all_static_functions() StaticFunction should succeed");
            CHECK(*ret->cast_safe<int>().value() == 7, "sbval via all_static_functions() should be 7");
            break;
        }
    }

    // === Name hiding with overloads ===
    // HideDerived::set(int) hides both HideBase::set(int) and set(int,int).
    auto hd_cls = *refl::find_class("HideDerived");
    auto hd_obj = *hd_cls.find_constructor({"int"})->call(0);

    // find_function("set", {"int"}) — finds Derived's set(int).
    auto hd_set1 = hd_cls.find_function("set", {"int"});
    CHECK(hd_set1.has_value(), "find_function('set',{'int'}) on HideDerived should find derived set(int)");

    // find_function("set", {"int","int"}) — base's set(int,int) is HIDDEN.
    auto hd_set2 = hd_cls.find_function("set", {"int", "int"});
    CHECK(!hd_set2.has_value(), "find_function('set',{'int','int'}) on HideDerived should fail (hidden)");

    // find_functions("set") — only Derived's set(int), not Base's two.
    auto hd_overloads = hd_cls.find_functions("set");
    CHECK(hd_overloads.size() == 1, "find_functions('set') on HideDerived should be 1 (only derived)");

    // all_functions() — base's set overloads hidden, but get() still visible.
    auto hd_all = hd_cls.all_functions();
    int hd_set_count = 0;
    bool hd_has_get = false;
    for (const auto& f : hd_all) {
        if (f.name() == "set") ++hd_set_count;
        if (f.name() == "get") hd_has_get = true;
    }
    CHECK(hd_set_count == 1, "all_functions() should have 1 set (derived only, base hidden)");
    CHECK(hd_has_get, "all_functions() should include inherited get() (not hidden)");

    // Invoke the derived set(int) to confirm it works.
    (void)hd_set1->invoke(hd_obj, 3);
    auto hd_p = hd_obj.cast_safe<HideDerived>().value();
    CHECK(hd_p->v == 300, "derived set(3) should give 300 (3*100)");

    // === Diamond inheritance: dedup in all_functions / all_fields ===
    // DiamBottom -> {DiamLeft, DiamRight} -> DiamBase.
    // DiamBase has field v, function dfn, static dsval, static dsfn.
    // Without dedup, each appears twice (once per path to DiamBase).
    auto db_cls = *refl::find_class("DiamBottom");

    auto db_all_fns = db_cls.all_functions();
    int db_dfn_count = 0;
    for (const auto& f : db_all_fns)
        if (f.name() == "dfn") ++db_dfn_count;
    CHECK(db_dfn_count == 1, "all_functions() should have 1 dfn (diamond dedup)");

    auto db_all_flds = db_cls.all_fields();
    int db_v_count = 0;
    for (const auto& f : db_all_flds)
        if (f.name() == "v") ++db_v_count;
    CHECK(db_v_count == 1, "all_fields() should have 1 v (diamond dedup)");

    auto db_all_sflds = db_cls.all_static_fields();
    int db_dsval_count = 0;
    for (const auto& f : db_all_sflds)
        if (f.name() == "dsval") ++db_dsval_count;
    CHECK(db_dsval_count == 1, "all_static_fields() should have 1 dsval (diamond dedup)");

    auto db_all_sfns = db_cls.all_static_functions();
    int db_dsfn_count = 0;
    for (const auto& f : db_all_sfns)
        if (f.name() == "dsfn") ++db_dsfn_count;
    CHECK(db_dsfn_count == 1, "all_static_functions() should have 1 dsfn (diamond dedup)");

    // find_functions / find_static_functions also dedup.
    CHECK(db_cls.find_functions("dfn").size() == 1, "find_functions('dfn') should be 1 (diamond dedup)");
    CHECK(db_cls.find_static_functions("dsfn").size() == 1, "find_static_functions('dsfn') should be 1 (diamond dedup)");

    // The deduped dfn is callable (adjusts to first DiamBase subobject).
    auto db_obj = *db_cls.find_constructor({})->call();
    auto db_fn = db_cls.find_function("dfn");
    CHECK(db_fn.has_value(), "find_function('dfn') should succeed via base walk");
    // DiamLeft and DiamRight were default-constructed, so v=0 on both paths.
    auto db_ret = db_fn->invoke(db_obj);
    CHECK(db_ret.has_value(), "invoke dfn on DiamBottom should succeed");
    CHECK(*db_ret->cast_safe<int>().value() == 0, "dfn should be 0 (default-constructed)");

    // === Base handle: bases() returns Base handles, not raw structs ===
    auto db_bases = db_cls.bases();
    CHECK(db_bases.size() == 2, "DiamBottom should have 2 bases");
    CHECK(db_bases[0].name() == "DiamLeft", "first base should be DiamLeft");
    CHECK(db_bases[1].name() == "DiamRight", "second base should be DiamRight");
    CHECK(db_bases[0].valid(), "Base handle should be valid");
    CHECK(db_bases[0].offset() >= 0, "Base offset should be non-negative");
    // Point has 1 base.
    CHECK(cls.bases()[0].name() == "Base", "Point base name via handle");

    // === Null-safety: default-constructed Class/Enum ===
    refl::Class null_cls;
    CHECK(!null_cls.valid(), "default Class should be invalid");
    CHECK(null_cls.name().empty(), "invalid Class name() should return empty string");
    CHECK(null_cls.bases().empty(), "invalid Class bases() should return empty vector");
    CHECK(null_cls.fields().empty(), "invalid Class fields() should return empty vector");
    CHECK(null_cls.functions().empty(), "invalid Class functions() should return empty vector");
    CHECK(null_cls.static_fields().empty(), "invalid Class static_fields() should return empty");
    CHECK(null_cls.static_functions().empty(), "invalid Class static_functions() should return empty");
    CHECK(null_cls.constructors().empty(), "invalid Class constructors() should return empty");
    CHECK(!null_cls.find_function("x").has_value(), "invalid Class find_function should fail");
    CHECK(null_cls.find_function("x").error() == refl::Error::NullHandle, "should be NullHandle");
    CHECK(null_cls.all_functions().empty(), "invalid Class all_functions() should return empty");

    refl::Enum null_enum;
    CHECK(!null_enum.valid(), "default Enum should be invalid");
    CHECK(null_enum.name().empty(), "invalid Enum name() should return empty string");
    CHECK(null_enum.enumerators().empty(), "invalid Enum enumerators() should return empty");
    CHECK(!null_enum.find_enumerator("x").has_value(), "invalid Enum find_enumerator should fail");
    CHECK(null_enum.find_enumerator("x").error() == refl::Error::NullHandle, "should be NullHandle");

    // === is_const semantics: const vs non-const vs move-only ===
    // is_const reports the const qualifier, not writability — a non-const
    // but non-move-assignable member is not const yet has no setter.
    // const member: is_const=true, has_setter=false, has_getter=true (copyable)
    CHECK(idf.is_const(), "const int member should be const");
    CHECK(!idf.has_setter(), "const int member should have no setter");
    CHECK(idf.has_getter(), "const int member should have a getter");
    // non-const plain member: is_const=false, has_setter=true
    CHECK(!xf.is_const(), "non-const int member should not be const");
    CHECK(xf.has_setter(), "non-const int member should have a setter");
    // move-only move-assignable member: is_const=false (not const), has_setter=true
    CHECK(!mo_ptr_field.is_const(), "unique_ptr member should not be const");
    CHECK(mo_ptr_field.has_setter(), "unique_ptr member should have a move setter");
    CHECK(!mo_ptr_field.has_getter(), "unique_ptr member should have no copy getter");
    // const static member: is_const=true
    CHECK(maxf->is_const(), "const static member should be const");
    CHECK(!maxf->has_setter(), "const static member should have no setter");
    // non-const static member: is_const=false
    CHECK(!sf->is_const(), "non-const static member should not be const");
    CHECK(sf->has_setter(), "non-const static member should have a setter");

    // === Field name hiding: derived field shadows base field of same name ===
    auto fh_cls = *refl::find_class("FieldHideDer");
    // all_fields: x appears once (the derived's), y is still visible from base.
    auto fh_all = fh_cls.all_fields();
    int fh_x_count = 0;
    bool fh_has_y = false;
    for (const auto& f : fh_all) {
        if (f.name() == "x") ++fh_x_count;
        if (f.name() == "y") fh_has_y = true;
    }
    CHECK(fh_x_count == 1, "all_fields() should have 1 x (derived shadows base)");
    CHECK(fh_has_y, "all_fields() should still include base's y");

    // find_field("x") returns the derived's field (value 99), not the base's (1).
    auto fh_xf = fh_cls.find_field("x");
    CHECK(fh_xf.has_value(), "find_field x should succeed");
    auto fh_obj = *fh_cls.find_constructor({})->call();
    auto fh_g = fh_xf->get(fh_obj);
    CHECK(fh_g.has_value(), "get x should succeed");
    CHECK(*fh_g->cast_safe<int>().value() == 99, "shadowed x should be derived's (99)");

    // y is inherited and unaffected.
    auto fh_yf = fh_cls.find_field("y");
    CHECK(fh_yf.has_value(), "find_field y should succeed (inherited, not hidden)");
    CHECK(*fh_yf->get(fh_obj)->cast_safe<int>().value() == 2, "y should be base's (2)");

    std::printf("refl core API test ok\n");
    return 0;
}
