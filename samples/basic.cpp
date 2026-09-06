// Sample: runtime reflection with the refl framework.
//
// Demonstrates the type-erased API: register a class, find it by name at
// runtime, construct an Object without knowing its C++ type, invoke methods
// on it, get/set fields via reflected handles, and safely cast back.
#include <refl/refl.hpp>
#include <any>
#include <cstdio>

struct Vec2 {
    int x;
    int y;
    Vec2(int x, int y) : x(x), y(y) {}
    int dot(const Vec2& other) const { return x * other.x + y * other.y; }
};

[[maybe_unused]] static refl::Refl<Vec2> reg_vec2;

int main() {
    auto cls = *refl::find_class("Vec2");
    std::printf("class: %s\n", cls.name().c_str());

    // Enumerate fields with types.
    const auto& fields = cls.fields();
    std::printf("  fields:");
    for (const auto& f : fields)
        std::printf(" %s:%s%s", f.name.c_str(), f.type.c_str(),
                    f.setter ? "" : " (ro)");
    std::printf("\n");

    // Construct a type-erased Object.
    auto ctor = *cls.find_constructor({"int", "int"});
    auto obj = std::move(*ctor.call(3, 4));
    std::printf("  object class: %s\n", obj.class_name().c_str());

    // Field get via reflected Field handle.
    auto x_field = *cls.find_field("x");
    int xv = std::any_cast<int>(x_field.get(obj));
    std::printf("  field x = %d\n", xv);

    // Field set via reflected Field handle.
    (void)x_field.set(obj, std::any(10));
    std::printf("  after set, field x = %d\n", std::any_cast<int>(x_field.get(obj)));

    // Safe cast to read the concrete type.
    auto safe = obj.cast_safe<Vec2>();
    auto v = safe.value();
    std::printf("  concrete: (%d, %d)\n", v->x, v->y);

    // Invoke a member function.
    auto fn = *cls.find_function("dot");
    Vec2 other{2, 5};
    std::any result = fn.invoke(obj, other);
    std::printf("  dot((10,4), (2,5)) = %d\n", std::any_cast<int>(result));

    return 0;
}
