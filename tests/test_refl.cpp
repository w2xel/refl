// Full API test: register a class, find it by name, find a constructor,
// construct an instance (type-erased Object), find a function, invoke it,
// find a field, get/set it, cast back, resolve overloads, walk inheritance,
// and reflect enums.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/refl.hpp>
#include <any>
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

[[maybe_unused]] static refl::Refl<Base> reg_base;
[[maybe_unused]] static refl::Refl<Point> reg_point;
[[maybe_unused]] static refl::Refl<Color> reg_color;

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
    CHECK(fields.size() == 3, "Point should have 3 fields (x, y, id)");

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

    // --- fast cast ---
    CHECK(obj.cast<Point>()->x == 1, "constructed x should be 1");
    CHECK(obj.cast<Point>()->y == 3, "constructed y should be 3");

    // --- safe cast ---
    auto safe = obj.cast_safe<Point>();
    CHECK(safe.has_value(), "cast_safe<Point> should succeed");
    CHECK((*safe)->sum() == 4, "safe-cast sum should be 4");

    auto bad_cast = obj.cast_safe<Wrong>();
    CHECK(!bad_cast.has_value(), "cast_safe<Wrong> should fail");
    CHECK(bad_cast.error() == refl::Error::TypeError, "should be TypeError");

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
    std::any r1 = set1->invoke(obj, 50);
    CHECK(obj.cast<Point>()->x == 50, "after set(50), x should be 50");
    std::any r2 = set2->invoke(obj, 10, 20);
    CHECK(obj.cast<Point>()->x == 10, "after set(10,20), x should be 10");
    CHECK(obj.cast<Point>()->y == 20, "after set(10,20), y should be 20");

    // --- function on Object ---
    auto fn = *cls.find_function("sum");
    CHECK(std::any_cast<int>(fn.invoke(obj)) == 30, "sum(10,20) should be 30");

    // --- field get/set ---
    auto xf = *cls.find_field("x");
    CHECK(std::any_cast<int>(xf.get(obj)) == 10, "field get x should be 10");
    (void)xf.set(obj, std::any(77));
    CHECK(obj.cast<Point>()->x == 77, "after field set, x should be 77");

    // --- readonly field ---
    auto idf = *cls.find_field("id");
    CHECK(idf.is_readonly(), "id should be readonly");
    auto set_id = idf.set(obj, std::any(99));
    CHECK(!set_id.has_value(), "set on readonly should fail");
    CHECK(set_id.error() == refl::Error::BadSignature, "should be BadSignature");

    // --- inheritance ---
    CHECK(cls.base_names().size() == 1, "Point should have 1 base");
    CHECK(cls.base_names()[0] == "Base", "base should be Base");

    // inherited field from Base
    auto base_field = cls.find_field("base_val");
    CHECK(base_field.has_value(), "find_field should find inherited base_val");
    CHECK(base_field->name() == "base_val", "inherited field name");

    // inherited method from Base
    auto base_fn = cls.find_function("base_method");
    CHECK(base_fn.has_value(), "find_function should find inherited base_method");
    std::any base_ret = base_fn->invoke(obj);
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
    CHECK(std::any_cast<int>(sf->get()) == 1, "instance_count should be 1");

    (void)sf->set(std::any(42));
    CHECK(std::any_cast<int>(sf->get()) == 42, "after set, instance_count should be 42");

    // readonly static field (const)
    auto maxf = cls.find_static_field("max_instances");
    CHECK(maxf.has_value(), "find_static_field(\"max_instances\") should succeed");
    CHECK(maxf->is_readonly(), "max_instances should be readonly (const)");
    CHECK(std::any_cast<int>(maxf->get()) == 100, "max_instances should be 100");
    auto set_max = maxf->set(std::any(200));
    CHECK(!set_max.has_value(), "set on readonly static should fail");
    CHECK(set_max.error() == refl::Error::BadSignature, "should be BadSignature");

    // --- static member functions ---
    auto sf_count = cls.find_static_function("get_instance_count");
    CHECK(sf_count.has_value(), "find_static_function(\"get_instance_count\") should succeed");
    CHECK(sf_count->return_type() == "int", "get_instance_count returns int");
    std::any sc_ret = sf_count->invoke();
    CHECK(std::any_cast<int>(sc_ret) == 42, "get_instance_count should be 42");

    auto sf_reset = cls.find_static_function("reset_count");
    CHECK(sf_reset.has_value(), "find_static_function(\"reset_count\") should succeed");
    std::any sr_ret = sf_reset->invoke();
    CHECK(!sr_ret.has_value(), "reset_count returns void, any should be empty");
    CHECK(Point::instance_count == 0, "after reset_count, instance_count should be 0");

    // --- error cases ---
    CHECK(!refl::find_class("NoSuchClass").has_value(), "non-existent class should fail");
    CHECK(!cls.find_constructor({"double"}).has_value(), "wrong ctor types should fail");
    CHECK(!cls.find_function("no_such_function").has_value(), "non-existent function should fail");
    CHECK(!cls.find_field("no_such_field").has_value(), "non-existent field should fail");

    std::printf("refl API test ok\n");
    return 0;
}
