// Full API test: register a class, find it by name, find a constructor,
// construct an instance, find a function, and invoke it.
//
// Returns non-zero (fails meson test) on any assertion failure.
#include <refl/refl.hpp>
#include <cstdio>
#include <cstdlib>

struct Point {
    int x;
    int y;
    Point(int x, int y) : x(x), y(y) {}
    int sum() const { return x + y; }
    void set(int a, int b) { x = a; y = b; }
};

// Force registration of Point into the global pool.
// Instantiating Refl<Point> triggers the static registrar.
[[maybe_unused]] static refl::Refl<Point> reg_point;

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

    // --- data members ---
    const auto& members = cls.data_members();
    CHECK(members.size() == 2, "Point should have 2 data members");
    CHECK(members[0] == "x", "first member should be \"x\"");
    CHECK(members[1] == "y", "second member should be \"y\"");

    const auto& member_types = cls.data_member_types();
    CHECK(member_types[0] == "int", "x should be int");
    CHECK(member_types[1] == "int", "y should be int");

    // --- find_constructor ---
    auto ctor_result = cls.find_constructor({"int", "int"});
    CHECK(ctor_result.has_value(), "find_constructor({\"int\",\"int\"}) should succeed");
    auto ctor = *ctor_result;
    CHECK(ctor.param_types().size() == 2, "constructor should have 2 params");
    CHECK(ctor.param_types()[0] == "int", "first param should be int");
    CHECK(ctor.param_types()[1] == "int", "second param should be int");

    // --- construct via call ---
    auto obj_result = ctor.call<Point>(1, 3);
    CHECK(obj_result.has_value(), "ctor.call<Point>(1, 3) should succeed");
    auto obj = *obj_result;
    CHECK(obj.get().x == 1, "constructed x should be 1");
    CHECK(obj.get().y == 3, "constructed y should be 3");

    // --- find_function ---
    auto fn_result = cls.find_function("sum");
    CHECK(fn_result.has_value(), "find_function(\"sum\") should succeed");
    auto fn = *fn_result;
    CHECK(fn.name() == "sum", "function name should be \"sum\"");
    CHECK(fn.return_type() == "int", "sum should return int");
    CHECK(fn.param_types().size() == 0, "sum should have 0 params");

    // --- invoke function ---
    std::any ret = fn.invoke<Point>(obj.get());
    CHECK(ret.has_value(), "invoke sum should return non-empty any");
    int result = std::any_cast<int>(ret);
    CHECK(result == 4, "sum(1,3) should be 4");

    // --- find and invoke void function ---
    auto set_result = cls.find_function("set");
    CHECK(set_result.has_value(), "find_function(\"set\") should succeed");
    auto set_fn = *set_result;
    CHECK(set_fn.param_types().size() == 2, "set should have 2 params");

    set_fn.invoke<Point>(obj.get(), 10, 20);
    CHECK(obj.get().x == 10, "after set, x should be 10");
    CHECK(obj.get().y == 20, "after set, y should be 20");

    // re-invoke sum to verify
    std::any ret2 = fn.invoke<Point>(obj.get());
    int result2 = std::any_cast<int>(ret2);
    CHECK(result2 == 30, "sum(10,20) should be 30");

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

    std::printf("refl API test ok\n");
    return 0;
}
