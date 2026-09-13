#include <refl/extensions/observed.hpp>
#include "prototype/source.hpp"
#include <cstdio>
#include <cstdlib>
#include <source_location>

void check(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) { std::fprintf(stderr, "check failed: %u\n", location.line()); std::abort(); }
}
struct Interface {};
constexpr refl::MemberId update{refl::type_id<Interface>(), 0};
constexpr refl::MemberId value{refl::type_id<Interface>(), 1};
constexpr refl::MemberId reference{refl::type_id<Interface>(), 2};
struct Square { int size; };
struct Triangle { int size; };
struct ThrowingCopy {
    ThrowingCopy() = default;
    ThrowingCopy(const ThrowingCopy&) { throw 8; }
    ThrowingCopy(ThrowingCopy&&) = default;
};
struct ThrowingMove {
    static inline int moves = 0;
    static inline int fail_on = 0;
    ThrowingMove() = default;
    ThrowingMove(const ThrowingMove&) = delete;
    ThrowingMove(ThrowingMove&&) { if (++moves == fail_on) throw 7; }
};
int main() {
    auto schema = std::make_shared<const refl::InterfaceSchema>(refl::InterfaceSchema{
        {update, "update", refl::signature_of<void(int&)>()},
        {value, "value", refl::signature_of<std::unique_ptr<int>()>()},
        {reference, "reference", refl::signature_of<int&()>()}
    });
    prototype::Source source(schema);
    int calls = 0;
    auto update_target = refl::make_target<void(int&)>([&](int& n) { ++calls; n = 42; }).value();
    auto value_target = refl::make_target<std::unique_ptr<int>()>([] { return std::make_unique<int>(9); }).value();
    std::weak_ptr<int> closure_lifetime;
    auto ref_target = [&] {
        auto owned = std::make_shared<int>(73);
        closure_lifetime = owned;
        return refl::make_target<int&()>([owned, &calls]() -> int& { ++calls; return *owned; },
            {.result_lifetime = refl::ResultLifetime::callable_context}).value();
    }();
    check(source.reset({{update, update_target}, {value, value_target}, {reference, ref_target}}).has_value());
    ref_target = {};
    int n = 0;
    auto live = source.dispatch();
    check(refl::try_call<void>(live, update, n).has_value());
    check(n == 42);
    const int constant = 0;
    auto rejected = refl::try_call<void>(live, update, constant);
    check(!rejected && rejected.error().code == refl::DiagnosticCode::read_only && calls == 1);
    check(!source.replace(update, refl::make_target<int()>([] { return 1; }).value()));
    check(refl::try_call<void>(live, update, n).has_value());
    int events = 0, errors = 0;
    auto observed = refl::observe(live, [&](std::exception_ptr) noexcept { ++errors; });
    auto subscription = observed.after(value, [&](const refl::CallCompletedEvent& event) {
        ++events;
        auto result = event.result().get<const std::unique_ptr<int>>().value();
        check(**result == 9);
        check(!event.result().get<std::unique_ptr<int>>());
    });
    check(*refl::try_call<std::unique_ptr<int>>(observed, value).value() == 9);
    check(events == 1);
    check(*refl::try_call<std::unique_ptr<int>>(live, value).value() == 9 && events == 1);
    auto fail_listener = observed.after(value, [](const auto&) { throw 1; });
    check(refl::try_call<std::unique_ptr<int>>(observed, value).has_value() && errors == 1);
    // Observer replacement cannot destroy the selected closure before retained export.
    auto replace_listener = observed.after(reference, [&](const auto&) {
        source.replace(reference, refl::make_target<int&()>([]() -> int& { static int next = 8; return next; }).value()).value();
    });
    int before = calls;
    check(!refl::try_call<int&>(observed, reference));
    check(calls == before);
    auto retained = refl::try_call_retained<int>(observed, reference);
    // The native baseline also retains the initial target until this reset.
    source.reset({{update, update_target}, {value, value_target}, {reference, source.target(reference)}}).value();
    check(!closure_lifetime.expired() && retained->get() == 73);
    retained = std::unexpected(refl::Diagnostic{refl::DiagnosticCode::not_found});
    check(closure_lifetime.expired());
    source.replace(reference, refl::make_target<int&()>([&]() -> int& { return n; },
        {.result_lifetime = refl::ResultLifetime::external,
         .reference_export = refl::ReferenceExport::caller_borrow}).value()).value();
    check(&refl::try_call<int&>(live, reference).value().get() == &n);
    // Captures keep their generation; live dispatch and observation follow reset.
    auto captured = source.capture();
    source.reset({{update, refl::make_target<void(int&)>([](int& x) { x = 100; }).value()},
                  {value, value_target}, {reference, source.target(reference)}}).value();
    refl::try_call<void>(captured, update, n).value(); check(n == 42);
    refl::try_call<void>(live, update, n).value(); check(n == 100);
    check(!source.reset({}));
    refl::try_call<void>(observed, update, n).value(); check(n == 100);
    // One schema is reused by independent instance states.
    prototype::Source second(schema);
    second.reset({{update, update_target}, {value, value_target}, {reference, source.target(reference)}}).value();
    refl::try_call<void>(second.dispatch(), update, n).value(); check(n == 42);
    refl::try_call<void>(live, update, n).value(); check(n == 100);
    source.replace(value, refl::make_target<std::unique_ptr<int>()>([] { return std::make_unique<int>(9); },
        {.native_dependency = refl::NativeDependency::independent}).value()).value();
    source.detach();
    check(!refl::try_call<void>(live, update, n));
    check(refl::try_call<std::unique_ptr<int>>(observed, value).has_value());
    // Native targets retain their original receiver across reset and restore.
    auto make_native = [](auto owner) {
        return refl::make_target<void(int&)>([owner](int& out) { out = owner->size; },
            {.native_dependency = refl::NativeDependency::native,
             .receiver = refl::ObjectView::from(*owner, owner)}).value();
    };
    auto square = make_native(std::make_shared<Square>(4));
    auto triangle = make_native(std::make_shared<Triangle>(6));
    source.reset({{update, square}, {value, value_target}, {reference, second.target(reference)}}).value();
    auto saved = source.target(update);
    source.reset({{update, triangle}, {value, value_target}, {reference, second.target(reference)}}).value();
    source.replace(update, saved).value();
    refl::try_call<void>(live, update, n).value(); check(n == 4);
    source.restore(update);
    refl::try_call<void>(live, update, n).value(); check(n == 6);
    // Move-only input requires an explicit rvalue.
    auto consume = refl::make_target<int(std::unique_ptr<int>)>([](auto p) { return *p; }).value();
    refl::DispatchHandle consume_source(schema, [consume](auto) -> refl::Result<refl::ResolvedCall> { return refl::ResolvedCall{consume, {}, {}}; });
    auto input = std::make_unique<int>(55);
    check(!refl::try_call<int>(consume_source, value, input) && input);
    check(refl::try_call<int>(consume_source, value, std::move(input)).value() == 55 && !input);
    // Capture failure follows target effects; extraction failure follows delivery.
    auto throwing = refl::make_target<ThrowingMove()>([&] { ++calls; return ThrowingMove{}; }).value();
    refl::DispatchHandle throwing_source(schema, [throwing](auto) -> refl::Result<refl::ResolvedCall> { return refl::ResolvedCall{throwing, {}, {}}; });
    auto observed_throw = refl::observe(throwing_source, [](std::exception_ptr) noexcept {});
    auto on_throw = observed_throw.after(value, [&](const auto&) { ++events; });
    auto old_events = events;
    ThrowingMove::moves = 0; ThrowingMove::fail_on = 1;
    before = calls;
    try { (void)refl::try_call<ThrowingMove>(observed_throw, value); check(false); } catch (int) {}
    check(calls == before + 1 && events == old_events);
    ThrowingMove::moves = 0; ThrowingMove::fail_on = 2;
    try { (void)refl::try_call<ThrowingMove>(observed_throw, value); check(false); } catch (int) {}
    check(events == old_events + 1);
    // Argument materialization can fail before user code or event delivery.
    auto prepare = refl::make_target<void(ThrowingCopy)>([&](auto) { ++calls; }).value();
    refl::DispatchHandle prepare_source(schema, [prepare](auto) -> refl::Result<refl::ResolvedCall> {
        return refl::ResolvedCall{prepare, {}, {}};
    });
    auto observed_prepare = refl::observe(prepare_source, [](std::exception_ptr) noexcept {});
    auto on_prepare = observed_prepare.after(update, [&](const auto&) { ++events; });
    ThrowingCopy copy;
    before = calls; old_events = events;
    try { (void)refl::try_call<void>(observed_prepare, update, copy); check(false); } catch (int) {}
    check(calls == before && events == old_events);
    // Delivery uses an entry snapshot, checks active tokens, and supports reentry.
    auto ordered = refl::observe(live, [](std::exception_ptr) noexcept {});
    refl::Subscription late, skipped;
    std::vector<int> order;
    bool nested = false;
    auto first = ordered.after(update, [&](const auto&) {
        order.push_back(1);
        skipped.unsubscribe();
        if (!nested) {
            nested = true;
            late = ordered.after(update, [&](const auto&) { order.push_back(3); });
            refl::try_call<void>(ordered, update, n).value();
        }
    });
    skipped = ordered.after(update, [&](const auto&) { order.push_back(2); });
    refl::try_call<void>(ordered, update, n).value();
    check(order == std::vector<int>({1, 1, 3}));
    try {
        prototype::Source unsupported(std::make_shared<const refl::InterfaceSchema>(refl::InterfaceSchema{
            {update, "noexcept", refl::signature_of<int() noexcept>()}}));
        check(false);
    } catch (const refl::ReflectionError& error) { check(error.diagnostic.code == refl::DiagnosticCode::unsupported); }
    check(!refl::make_target<int&&()>([]() -> int&& { static int x; return std::move(x); }));
    check(!refl::make_target<int() &>([] { return 0; }));
    check(!refl::make_target<int() volatile>([] { return 0; }));
    std::puts("call contract prototype passed");
}
