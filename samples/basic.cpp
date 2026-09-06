// Sample: runtime reflection with the refl framework.
//
// Demonstrates: class registration, type-erased construction, field get/set,
// function invocation, safe cast, overloaded function resolution, inherited
// member access, and enum reflection.
#include <refl/refl.hpp>
#include <any>
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
    Rect(int w, int h) : Shape(w), w(w), h(h) {}
    int area() const { return w * h; }
    void resize(int nw, int nh) { w = nw; h = nh; }
    void resize(int sq) { w = sq; h = sq; }
};

enum ShapeType { Circle = 1, Square = 2, Triangle = 3 };

[[maybe_unused]] static refl::Refl<Shape> reg_shape;
[[maybe_unused]] static refl::Refl<Rect> reg_rect;
[[maybe_unused]] static refl::Refl<ShapeType> reg_type;

int main() {
    // List all registered classes.
    std::printf("registered classes:");
    for (const auto& n : refl::list_all_classes()) std::printf(" %s", n.c_str());
    std::printf("\n");

    auto cls = *refl::find_class("Rect");
    std::printf("class: %s\n", cls.name().c_str());
    std::printf("  bases:");
    for (const auto& b : cls.base_names()) std::printf(" %s", b.c_str());
    std::printf("\n");

    // Construct.
    auto obj = *cls.find_constructor({"int", "int"})->call(3, 4);
    std::printf("  object: %s\n", obj.class_name().c_str());

    // Field get/set.
    auto wf = *cls.find_field("w");
    std::printf("  field w = %d\n", std::any_cast<int>(wf.get(obj)));
    (void)wf.set(obj, std::any(10));

    // Overloaded function resolution.
    auto resize2 = *cls.find_function("resize", {"int", "int"});
    std::printf("  resize(%zu params)\n", resize2.param_types().size());
    resize2.invoke(obj, 5, 6);
    std::printf("  after resize: w=%d h=%d\n", obj.cast<Rect>()->w, obj.cast<Rect>()->h);

    auto overloads = cls.find_functions("resize");
    std::printf("  resize overloads: %zu\n", overloads.size());

    // Inherited method from Shape.
    auto inherited = cls.find_function("area");
    std::printf("  area() = %d\n", std::any_cast<int>(inherited->invoke(obj)));

    // Safe cast.
    auto safe = obj.cast_safe<Rect>();
    std::printf("  safe cast: w=%d h=%d\n", safe.value()->w, safe.value()->h);

    // Enum reflection.
    auto e = *refl::find_enum("ShapeType");
    std::printf("enum: %s\n", e.name().c_str());
    for (const auto& en : e.enumerators())
        std::printf("  %s = %lld\n", en.name.c_str(), en.value);
    auto sq = e.find_enumerator("Square");
    std::printf("  Square = %lld\n", sq->value());

    return 0;
}
