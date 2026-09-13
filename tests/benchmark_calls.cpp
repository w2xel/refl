#include <refl/extensions/observed.hpp>
#include "prototype/source.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace { std::size_t allocations = 0; }
void* operator new(std::size_t size) {
    ++allocations;
    if (auto p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
struct BenchmarkInterface {};
constexpr refl::MemberId render{refl::type_id<BenchmarkInterface>(), 0};
[[gnu::noinline]] int native_render(int n) { return n * 3; }
template<class F> void measure(const char* name, F call) {
    constexpr int iterations = 20000;
    const auto before = allocations;
    auto start = std::chrono::steady_clock::now();
    long long sum = 0;
    for (int i = 0; i < iterations; ++i) sum += call(i);
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    std::printf("%s: %.1f ns/call, %.2f allocations/call, checksum=%lld\n", name,
        static_cast<double>(nanos) / iterations,
        static_cast<double>(allocations - before) / iterations, sum);
}
int main() {
    auto schema = std::make_shared<const refl::InterfaceSchema>(refl::InterfaceSchema{
        {render, "render", refl::signature_of<int(int)>()}});
    auto target = refl::make_target<int(int)>(native_render).value();
    refl::DispatchHandle native(schema, [target](auto) -> refl::Result<refl::ResolvedCall> {
        return refl::ResolvedCall{target, {}, {}};
    });
    prototype::Source table(schema);
    table.reset({{render, target}}).value();
    auto slots = table.dispatch();
    auto observed = refl::observe(slots, [](std::exception_ptr) noexcept {});
    int deliveries = 0;
    auto token = observed.after(render, [&](const auto&) { ++deliveries; });
    measure("direct", native_render);
    measure("runtime", [&](int n) {
        std::array args{refl::argument(n)};
        auto result = refl::invoke(native, render, args).value();
        return std::get<refl::OwnedValue>(result).take<int>().value();
    });
    measure("typed facade", [&](int n) { return refl::try_call<int>(native, render, n).value(); });
    measure("prototype slots", [&](int n) { return refl::try_call<int>(slots, render, n).value(); });
    measure("observed", [&](int n) { return refl::try_call<int>(observed, render, n).value(); });
    return deliveries == 20000 ? 0 : 1;
}
