// Sample: runtime reflection with the refl framework.
//
// Demonstrates the type-erased API: register a class, find it by name at
// runtime, construct an Object without knowing its C++ type, invoke methods
// on it, and cast back to the concrete type only when you need direct access.
#include <refl/refl.hpp>
#include <any>
#include <cstdio>

struct Vec2 {
    int x;
    int y;
    Vec2(int x, int y) : x(x), y(y) {}
    int dot(const Vec2& other) const { return x * other.x + y * other.y; }
};

// Registering: instantiating Refl<Vec2> adds Vec2 to the global pool.
[[maybe_unused]] static refl::Refl<Vec2> reg_vec2;

int main() {
    // Find the class by runtime string name.
    auto cls = *refl::find_class("Vec2");
    std::printf("class: %s\n", cls.name().c_str());

    // List data members.
    const auto& members = cls.data_members();
    std::printf("  data members:");
    for (std::size_t i = 0; i < members.size(); ++i)
        std::printf(" %s:%s", members[i].c_str(), cls.data_member_types()[i].c_str());
    std::printf("\n");

    // Find a constructor by parameter type names.
    auto ctor = *cls.find_constructor({"int", "int"});
    std::printf("  constructor params:");
    for (const auto& p : ctor.param_types()) std::printf(" %s", p.c_str());
    std::printf("\n");

    // Construct a type-erased Object — no template parameter needed.
    auto obj = std::move(*ctor.call(3, 4));
    std::printf("  object class: %s\n", obj.class_name().c_str());

    // Cast back to the concrete type to read fields.
    auto& v = obj.cast<Vec2>();
    std::printf("  constructed: (%d, %d)\n", v.x, v.y);

    // Find and invoke a member function on the Object.
    auto fn = *cls.find_function("dot");
    std::printf("  function: %s -> %s\n", fn.name().c_str(), fn.return_type().c_str());

    Vec2 other{2, 5};
    std::any result = fn.invoke(obj, other);
    int dot = std::any_cast<int>(result);
    std::printf("  dot((3,4), (2,5)) = %d\n", dot);

    return 0;
}
