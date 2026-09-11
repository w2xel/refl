// Sample: runtime reflection with the refl core (type-erased API).
//
// Demonstrates: class registration, type-erased construction, field get/set,
// function invocation, safe cast, overloaded function resolution, inherited
// member access, and enum reflection.
#include <refl/refl.hpp>
#include <cstdio>

struct Shape {
    int id;
    Shape() : id(0) {}
    Shape(int id) : id(id) {}
    int area() const { return 0; }
};

struct Rect : Shape {
    int w;
    int h;
    static int total_created;
    Rect(int w, int h) : Shape(w), w(w), h(h) { ++total_created; }
    int area() const { return w * h; }
    void resize(int nw, int nh) { w = nw; h = nh; }
    void resize(int sq) { w = sq; h = sq; }
    static int get_total() { return total_created; }
};

int Rect::total_created = 0;

enum ShapeType { Circle = 1, Square = 2, Triangle = 3 };

[[maybe_unused]] static refl::Reg<Shape> reg_shape;
[[maybe_unused]] static refl::Reg<Rect> reg_rect;
[[maybe_unused]] static refl::Reg<ShapeType> reg_type;

int main() {
    // List all registered classes.
    std::printf("registered classes:");
    for (const auto& n : refl::list_all_classes()) std::printf(" %s", n.c_str());
    std::printf("\n");

    auto cls = *refl::find_class("Rect");
    std::printf("class: %s\n", cls.name().c_str());
    std::printf("  bases:");
    for (const auto& b : cls.bases()) std::printf(" %s", b.name().c_str());
    std::printf("\n");

    // Construct.
    auto obj = *cls.find_constructor({"int", "int"})->call(3, 4);
    std::printf("  object: %.*s\n", static_cast<int>(obj.class_name().size()), obj.class_name().data());

    // Field get/set.
    auto wf = *cls.find_field("w");
    std::printf("  field w = %d\n", *wf.get(obj)->cast_safe<int>().value());
    (void)wf.set(obj, 10);

    // Overloaded function resolution.
    auto resize2 = *cls.find_function("resize", {"int", "int"});
    std::printf("  resize(%zu params)\n", resize2.param_types().size());
    (void)resize2.invoke(obj, 5, 6).value();
    auto rect = obj.cast_safe<Rect>().value();
    std::printf("  after resize: w=%d h=%d\n", rect->w, rect->h);

    auto overloads = cls.find_functions("resize");
    std::printf("  resize overloads: %zu\n", overloads.size());

    // Inherited method from Shape.
    auto inherited = cls.find_function("area");
    std::printf("  area() = %d\n", *inherited->invoke(obj)->cast_safe<int>().value());

    // Safe cast.
    std::printf("  safe cast: w=%d h=%d\n", rect->w, rect->h);

    // Static data member.
    auto sf = *cls.find_static_field("total_created");
    std::printf("  static field total_created = %d\n", *sf.get()->cast_safe<int>().value());

    // Static member function.
    auto sfn = *cls.find_static_function("get_total");
    std::printf("  static fn get_total() = %d\n", *sfn.invoke()->cast_safe<int>().value());

    // Enum reflection.
    auto e = *refl::find_enum("ShapeType");
    std::printf("enum: %s\n", e.name().c_str());
    for (const auto& en : e.enumerators())
        std::printf("  %s = %lld\n", en.name.c_str(), en.value);
    auto sq = e.find_enumerator("Square");
    std::printf("  Square = %lld\n", sq->value());

    // Value-to-enum: look up an enumerator by its integer value, then read
    // the name back (round-trip a runtime value to an enumerator name).
    auto by_val = e.find_enumerator(3);
    std::printf("  value 3 = %s\n", by_val->name().c_str());

    // Clone (deep copy through type-erased handle).
    auto cloned = *obj.clone();
    std::printf("  clone: %s\n", cloned.to_string().c_str());
    std::printf("  clone w=%d (independent of original)\n",
                cloned.cast_safe<Rect>().value()->w);

    return 0;
}
