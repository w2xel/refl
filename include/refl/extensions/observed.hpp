#pragma once
#include <refl/runtime/invoke.hpp>
#include <exception>
#include <map>

namespace refl {
class CallCompletedEvent {
    MemberId member_;
    ObjectView receiver_;
    std::span<const ArgumentView> arguments_;
    const CallResult& result_;
public:
    CallCompletedEvent(MemberId member, ObjectView receiver, std::span<const ArgumentView> arguments,
                       const CallResult& result)
        : member_(member), receiver_(receiver.as_const()), arguments_(arguments), result_(result) {}
    MemberId member() const { return member_; }
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
    MemberId member;
    bool active = true;
    std::function<void(const CallCompletedEvent&)> callback;
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
class Observed {
    struct State {
        DispatchHandle source;
        std::function<void(std::exception_ptr)> error_sink;
        std::vector<std::shared_ptr<detail::Listener>> listeners;
    };
    std::shared_ptr<State> state_;
public:
    template<class Sink> requires std::is_nothrow_invocable_v<Sink&, std::exception_ptr>
    Observed(DispatchHandle source, Sink sink)
        : state_(std::make_shared<State>(std::move(source), std::move(sink),
                                        std::vector<std::shared_ptr<detail::Listener>>{})) {}
    Subscription after(MemberId member, std::function<void(const CallCompletedEvent&)> callback) {
        std::erase_if(state_->listeners, [](const auto& listener) { return !listener->active; });
        auto listener = std::make_shared<detail::Listener>(member, true, std::move(callback));
        state_->listeners.push_back(listener);
        return Subscription(listener);
    }
    friend Result<CallResult> invoke(const Observed& observed, MemberId member,
                                     std::span<const ArgumentView> args, ExportKind mode) {
        auto state = observed.state_;
        auto listeners = state->listeners; // Allocate before target entry.
        auto call = state->source.resolve(member);
        if (!call) return std::unexpected(call.error());
        CallFrame frame{call->receiver, args};
        auto result = invoke_target(call->target, frame, member, mode);
        if (!result) return result;
        CallCompletedEvent event(member, frame.receiver, args, *result);
        for (const auto& listener : listeners) {
            if (!listener->active || listener->member != member) continue;
            try { listener->callback(event); }
            catch (...) { state->error_sink(std::current_exception()); }
        }
        return result;
    }
};
template<class Sink> auto observe(DispatchHandle source, Sink sink) {
    return Observed(std::move(source), std::move(sink));
}
}
