// Full API test: register a class, find it by name, find a constructor,
// construct an instance (type-erased Object), find a function, invoke it,
// find a field, get/set it, and cast back to the concrete type.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/refl.hpp>
#include <any>
#include <cstdio>
#include <cstdlib>

struct Point {
    int x;
    int y;
    const int id = 42;
    Point(int x, int y) : x(x), y(y), id(0) {}
    int sum() const { return x + y; }
    void set(int a, int b) { x = a; y = b; }
};

// Force registration of Point into the global pool.
[[maybe_unused]] static refl::Refl<Point> reg_point;

struct Wrong {};

#define CHECK(cond, msg) \
    do { if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1; \
    } } while (0)

int main() {
    // --- find_class ---
    auto cls_result = refl::find_class("Point");
    CHECK(cls_result.has_value(), "find_class(\"Point\") should succeed");
    auto cls = *cls_result;
    CHECK(cls.name() == "Point", "class name should be \"Point\"");

    // --- fields enumeration ---
    const auto& fields = cls.fields();
    CHECK(fields.size() == 3, "Point should have 3 fields (x, y, id)");
    CHECK(fields[0].name == "x", "first field should be \"x\"");
    CHECK(fields[1].name == "y", "second field should be \"y\"");
    CHECK(fields[2].name == "id", "third field should be \"id\"");
    CHECK(fields[0].type == "int", "x type should be int");

    // --- find_constructor ---
    auto ctor_result = cls.find_constructor({"int", "int"});
    CHECK(ctor_result.has_value(), "find_constructor({\"int\",\"int\"}) should succeed");
    auto ctor = *ctor_result;
    CHECK(ctor.param_types().size() == 2, "constructor should have 2 params");

    // --- construct via call (type-erased) ---
    auto obj_result = ctor.call(1, 3);
    CHECK(obj_result.has_value(), "ctor.call(1, 3) should succeed");
    auto obj = std::move(*obj_result);
    CHECK(obj.valid(), "object should be valid");
    CHECK(obj.class_name() == "Point", "object class name should be \"Point\"");

    // --- fast cast (unchecked) ---
    CHECK(obj.cast<Point>()->x == 1, "constructed x should be 1");
    CHECK(obj.cast<Point>()->y == 3, "constructed y should be 3");

    // --- safe cast (checked) ---
    auto safe = obj.cast_safe<Point>();
    CHECK(safe.has_value(), "cast_safe<Point> should succeed");
    CHECK((*safe)->sum() == 4, "safe-cast sum should be 4");

    auto bad_cast = obj.cast_safe<Wrong>();
    CHECK(!bad_cast.has_value(), "cast_safe<Wrong> should fail");
    CHECK(bad_cast.error() == refl::Error::TypeError, "should be TypeError");

    // --- find_function ---
    auto fn_result = cls.find_function("sum");
    CHECK(fn_result.has_value(), "find_function(\"sum\") should succeed");
    auto fn = *fn_result;
    CHECK(fn.name() == "sum", "function name should be \"sum\"");
    CHECK(fn.return_type() == "int", "sum should return int");

    // --- invoke function on Object ---
    std::any ret = fn.invoke(obj);
    CHECK(ret.has_value(), "invoke sum should return non-empty any");
    CHECK(std::any_cast<int>(ret) == 4, "sum(1,3) should be 4");

    // --- invoke void function on Object ---
    auto set_result = cls.find_function("set");
    CHECK(set_result.has_value(), "find_function(\"set\") should succeed");
    auto set_fn = *set_result;
    std::any set_ret = set_fn.invoke(obj, 10, 20);
    CHECK(!set_ret.has_value(), "set returns void, any should be empty");
    CHECK(obj.cast<Point>()->x == 10, "after set, x should be 10");
    CHECK(obj.cast<Point>()->y == 20, "after set, y should be 20");

    // --- field get/set via reflected Field ---
    auto x_field = cls.find_field("x");
    CHECK(x_field.has_value(), "find_field(\"x\") should succeed");
    auto xf = *x_field;
    CHECK(xf.name() == "x", "field name should be x");
    CHECK(xf.type() == "int", "field type should be int");
    CHECK(!xf.is_readonly(), "x should not be readonly");

    std::any xval = xf.get(obj);
    CHECK(std::any_cast<int>(xval) == 10, "field get x should be 10");

    auto set_result2 = xf.set(obj, std::any(77));
    CHECK(set_result2.has_value(), "set x should succeed");
    CHECK(obj.cast<Point>()->x == 77, "after field set, x should be 77");

    // --- readonly field (const) ---
    auto id_field = cls.find_field("id");
    CHECK(id_field.has_value(), "find_field(\"id\") should succeed");
    auto idf = *id_field;
    CHECK(idf.is_readonly(), "id should be readonly (const)");

    auto set_id = idf.set(obj, std::any(99));
    CHECK(!set_id.has_value(), "set on readonly should fail");
    CHECK(set_id.error() == refl::Error::BadSignature, "should be BadSignature");

    // --- error cases ---
    auto bad_class = refl::find_class("NoSuchClass");
    CHECK(!bad_class.has_value(), "find_class for non-existent should fail");
    CHECK(bad_class.error() == refl::Error::NotFound, "should be NotFound");

    auto bad_ctor = cls.find_constructor({"double"});
    CHECK(!bad_ctor.has_value(), "find_constructor with wrong types should fail");
    CHECK(bad_ctor.error() == refl::Error::BadSignature, "should be BadSignature");

    auto bad_fn = cls.find_function("no_such_function");
    CHECK(!bad_fn.has_value(), "find_function for non-existent should fail");
    CHECK(bad_fn.error() == refl::Error::NotFound, "should be NotFound");

    auto bad_field = cls.find_field("no_such_field");
    CHECK(!bad_field.has_value(), "find_field for non-existent should fail");
    CHECK(bad_field.error() == refl::Error::NotFound, "should be NotFound");

    std::printf("refl API test ok\n");
    return 0;
}
