#pragma once
#include <refl/dyn.hpp>
#include <refl/extensions/observed.hpp>

namespace refl {
template<class T> class Observed {
    std::shared_ptr<detail::ObservationState> state_;
    ObservedHandle methods_;
    ObservedHandle reads_;
    ObservedHandle writes_;
    ObservedHandle views_;
    Proxy<T> proxy_;

    template<std::meta::info M>
    Subscription subscribe(ObservedHandle& source, OperationKind operation,
                           std::function<void(const CallCompletedEvent&)> callback) {
        return source.after(member_id<M>(), operation, std::move(callback));
    }
public:
    template<class Sink> requires std::is_nothrow_invocable_v<Sink&, std::exception_ptr>
    Observed(const Dyn<T>& source, Sink sink)
        : state_(std::make_shared<detail::ObservationState>(std::move(sink))),
          methods_(source.dispatch(), state_),
          reads_(source.dispatch(OperationKind::read), state_),
          writes_(source.dispatch(OperationKind::write), state_),
          views_(source.dispatch(OperationKind::view), state_),
          proxy_(methods_.dispatch(), reads_.dispatch(), writes_.dispatch()) {}

    auto* operator->() { return proxy_.operator->(); }
    const auto* operator->() const { return proxy_.operator->(); }
    DispatchHandle dispatch(OperationKind operation = OperationKind::method) const {
        switch (operation) {
            case OperationKind::method: return methods_.dispatch();
            case OperationKind::read: return reads_.dispatch();
            case OperationKind::write: return writes_.dispatch();
            case OperationKind::view: return views_.dispatch();
            case OperationKind::construct: return {};
        }
        return {};
    }
    template<std::meta::info M>
    Subscription after(std::function<void(const CallCompletedEvent&)> callback) {
        return subscribe<M>(methods_, OperationKind::method, std::move(callback));
    }
    template<std::meta::info M>
    Subscription after_read(std::function<void(const CallCompletedEvent&)> callback) {
        return subscribe<M>(reads_, OperationKind::read, std::move(callback));
    }
    template<std::meta::info M>
    Subscription after_write(std::function<void(const CallCompletedEvent&)> callback) {
        return subscribe<M>(writes_, OperationKind::write, std::move(callback));
    }
    template<std::meta::info M>
    Subscription after_view(std::function<void(const CallCompletedEvent&)> callback) {
        return subscribe<M>(views_, OperationKind::view, std::move(callback));
    }
};

template<class T, class Sink> auto observe(const Dyn<T>& source, Sink sink) {
    return Observed<T>(source, std::move(sink));
}
}
