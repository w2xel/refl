// Toolchain self-check: proves the dev environment can compile C++26
// reflection (P2996) and expansion statements (P1306) — the two features the
// framework is built on. Returns non-zero (and fails `meson test`) if the
// reflection machinery is broken.
//
// The query functions return std::vector<info>, which allocates and so cannot
// itself be a constant expression. std::define_static_array (P3491) materialises
// the result into static storage, making it usable as a template-for range.
#include <meta>
#include <cstdio>

struct Point {
    int x;
    int y;
};

int main() {
    constexpr auto refl = ^^Point;
    static constexpr auto members = std::define_static_array(
        std::meta::nonstatic_data_members_of(refl, std::meta::access_context::unchecked()));

    int member_count = 0;
    template for (constexpr auto m : members) {
        ++member_count;
        std::printf("  member: %s\n", std::meta::identifier_of(m).data());
    }

    if (member_count != 2) {
        std::fprintf(stderr, "expected 2 members, got %d\n", member_count);
        return 1;
    }

    std::printf("reflection self-check ok\n");
    return 0;
}
