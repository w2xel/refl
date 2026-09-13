#include <refl/extensions/observed_dyn.hpp>
#include <cassert>

struct Point {
    int x;
    int y;
    Point(int x, int y) : x(x), y(y) {}
    int sum() const { return x + y; }
};

int main() {
    refl::Dyn<Point> source(10, 20);
    int listener_errors = 0;
    auto observed = refl::observe(source, [&](std::exception_ptr) noexcept {
        ++listener_errors;
    });

    int method_events = 0;
    int last_result = 0;
    auto on_sum = observed.after<^^Point::sum>([&](const refl::CallCompletedEvent& event) {
        assert(event.member() == refl::member_id<^^Point::sum>());
        assert(event.operation() == refl::OperationKind::method);
        assert(event.receiver().read_only());
        last_result = *event.result().get<const int>().value();
        ++method_events;
    });
    assert(observed->sum() == 30 && method_events == 1 && last_result == 30);
    assert(source->sum() == 30 && method_events == 1);

    int read_events = 0;
    auto on_read = observed.after_read<^^Point::x>([&](const refl::CallCompletedEvent& event) {
        assert(event.operation() == refl::OperationKind::read);
        assert(event.argument_count() == 0);
        assert(*event.result().get<const int>().value() == 10);
        ++read_events;
    });
    assert(static_cast<int>(observed->x) == 10 && read_events == 1);

    int write_events = 0;
    auto on_write = observed.after_write<^^Point::x>([&](const refl::CallCompletedEvent& event) {
        assert(event.operation() == refl::OperationKind::write);
        assert(event.argument_count() == 1);
        assert(*event.argument(0).object.get<const int>().value() == 42);
        assert(!event.result().valid());
        ++write_events;
    });
    observed->x = 42;
    assert(source.get().x == 42 && write_events == 1);
    source->x = 7;
    assert(write_events == 1);

    int view_events = 0;
    auto on_view = observed.after_view<^^Point::x>([&](const refl::CallCompletedEvent& event) {
        assert(event.operation() == refl::OperationKind::view);
        assert(*event.result().get<const int>().value() == 7);
        ++view_events;
    });
    auto retained = refl::try_call_retained<const int>(
        observed.dispatch(refl::OperationKind::view), refl::member_id<^^Point::x>()).value();
    assert(retained.get() == 7 && view_events == 1);

    auto failing = observed.after<^^Point::sum>([](const auto&) { throw 1; });
    assert(observed->sum() == 27);
    assert(method_events == 2 && last_result == 27 && listener_errors == 1);

    source.reset_native<Point>(2, 3);
    assert(observed->sum() == 5);
    assert(method_events == 3 && last_result == 5);

    on_sum.unsubscribe();
    assert(observed->sum() == 5);
    assert(method_events == 3 && listener_errors == 3);
}
