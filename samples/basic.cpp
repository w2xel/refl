// Sample: runtime reflection with the refl framework.
//
// Demonstrates the full API: register a class, find it by name at runtime,
// query its constructors and member functions, construct objects, and
// invoke methods through reflected handles.
#include <refl/refl.hpp>
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

    // Construct an object via the reflected constructor.
    auto obj = *ctor.call<Vec2>(3, 4);
    std::printf("  constructed: (%d, %d)\n", obj.get().x, obj.get().y);

    // Find and invoke a member function.
    auto fn = *cls.find_function("dot");
    std::printf("  function: %s -> %s\n", fn.name().c_str(), fn.return_type().c_str());

    Vec2 other{2, 5};
    std::any result = fn.invoke<Vec2>(obj.get(), other);
    int dot = std::any_cast<int>(result);
    std::printf("  dot((3,4), (2,5)) = %d\n", dot);

    return 0;
}
