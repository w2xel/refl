#pragma once
#include <refl/runtime/invoke.hpp>
#include <algorithm>
#include <exception>

namespace refl {
class CallCompletedEvent {
    MemberId member_;
    OperationKind operation_;
    ObjectView receiver_;
    std::span<const ArgumentView> arguments_;
    const CallResult& result_;
public:
    CallCompletedEvent(MemberId member, OperationKind operation, ObjectView receiver,
                       std::span<const ArgumentView> arguments, const CallResult& result)
        : member_(member), operation_(operation), receiver_(receiver.as_const()),
          arguments_(arguments), result_(result) {}
    MemberId member() const { return member_; }
    OperationKind operation() const { return operation_; }
    ObjectView receiver() const { return receiver_; }
    std::size_t argument_count() const { return arguments_.size(); }
    ArgumentView argument(std::size_t index) const {
        const auto& arg = arguments_[index];
        return {arg.object.as_const(), arg.category};
    }
    ObjectView result() const { return result_view(result_).as_const(); }
};
namespace detail {
struct Listener {
    OperationKey key;
    bool active = true;
    std::function<void(const CallCompletedEvent&)> callback;
};
struct ObservationState {
    std::function<void(std::exception_ptr)> error_sink;
    std::vector<std::shared_ptr<Listener>> listeners;
};
}
class Subscription {
    std::weak_ptr<detail::Listener> listener_;
public:
    Subscription() = default;
    explicit Subscription(const std::shared_ptr<detail::Listener>& listener) : listener_(listener) {}
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    Subscription(Subscription&& other) noexcept : listener_(std::move(other.listener_)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) { unsubscribe(); listener_ = std::move(other.listener_); }
        return *this;
    }
    ~Subscription() { unsubscribe(); }
    void unsubscribe() { if (auto listener = listener_.lock()) listener->active = false; listener_.reset(); }
};
class ObservedHandle {
    std::shared_ptr<detail::ObservationState> state_;
    DispatchHandle source_;
    static DispatchHandle instrument(DispatchHandle source,
                                     const std::shared_ptr<detail::ObservationState>& state) {
        return source.with_completion([state](MemberId member, OperationKind operation) {
            auto listeners = state->listeners;
            return DispatchHandle::Completion(
                [state, listeners = std::move(listeners), member, operation]
                (const CallFrame& frame, const CallResult& result) {
                    CallCompletedEvent event(member, operation, frame.receiver, frame.arguments, result);
                    for (const auto& listener : listeners) {
                        if (!listener->active || listener->key != OperationKey{member, operation}) continue;
                        try { listener->callback(event); }
                        catch (...) { state->error_sink(std::current_exception()); }
                    }
                });
        });
    }
public:
    template<class Sink> requires std::is_nothrow_invocable_v<Sink&, std::exception_ptr>
    ObservedHandle(DispatchHandle source, Sink sink)
        : state_(std::make_shared<detail::ObservationState>(std::move(sink))),
          source_(instrument(std::move(source), state_)) {}
    ObservedHandle(DispatchHandle source, std::shared_ptr<detail::ObservationState> state)
        : state_(std::move(state)), source_(instrument(std::move(source), state_)) {}
    const std::shared_ptr<const InterfaceSchema>& schema() const { return source_.schema(); }
    const DispatchHandle& dispatch() const { return source_; }
    Subscription after(MemberId member, OperationKind operation,
                       std::function<void(const CallCompletedEvent&)> callback) {
        if (!source_.schema()) throw ReflectionError({DiagnosticCode::null_handle, member});
        auto found = std::find_if(source_.schema()->begin(), source_.schema()->end(),
            [=](const auto& candidate) {
                return candidate.member == member && candidate.operation == operation;
            });
        if (found == source_.schema()->end()) throw ReflectionError({DiagnosticCode::not_found, member});
        std::erase_if(state_->listeners, [](const auto& listener) { return !listener->active; });
        auto listener = std::make_shared<detail::Listener>(OperationKey{member, operation}, true,
                                                          std::move(callback));
        state_->listeners.push_back(listener);
        return Subscription(listener);
    }
    Subscription after(MemberId member, std::function<void(const CallCompletedEvent&)> callback) {
        const OperationDescriptor* selected = nullptr;
        if (source_.schema()) {
            for (const auto& candidate : *source_.schema()) {
                if (candidate.member != member) continue;
                if (selected) throw ReflectionError({DiagnosticCode::ambiguous, member});
                selected = &candidate;
            }
        }
        if (!selected) throw ReflectionError({DiagnosticCode::not_found, member});
        return after(member, selected->operation, std::move(callback));
    }
};
template<class Sink> auto observe(DispatchHandle source, Sink sink) {
    return ObservedHandle(std::move(source), std::move(sink));
}
inline Result<CallResult> invoke(const ObservedHandle& observed, MemberId member,
                                 std::span<const ArgumentView> args, ExportKind mode) {
    return invoke(observed.dispatch(), member, args, mode);
}
}
