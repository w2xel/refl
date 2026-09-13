// Compatibility event facade. Delivery policy moves to Observed in the next step.
#pragma once
#include <refl/dyn.hpp>

namespace refl {
template<class T> class Hooks : public Dyn<T> {
    using Listeners = std::vector<std::function<void(Object&)>>;
    std::map<std::string, std::shared_ptr<Listeners>> hooks_;
    auto listeners(std::string_view name) {
        auto& result = hooks_[std::string(name)];
        if (!result) result = std::make_shared<Listeners>();
        return result;
    }
    static void deliver(const std::shared_ptr<Listeners>& listeners, Object value) {
        auto snapshot = *listeners;
        for (auto& callback : snapshot) callback(value);
    }
public:
    using Dyn<T>::Dyn;
    template<std::meta::info M> void connect(std::function<void(Object&)> callback) {
        using Signature = [:std::meta::type_of(M):];
        using R = typename detail::signature_traits<Signature>::result;
        using Args = typename detail::signature_traits<Signature>::arguments;
        auto callbacks = listeners(std::meta::identifier_of(M));
        bool first = callbacks->empty();
        callbacks->push_back(std::move(callback));
        if (!first) return;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            this->template wrap<M>([callbacks](auto& original, typename Dyn<T>::Self&, std::tuple_element_t<I, Args>... args) -> R {
                if constexpr (std::is_void_v<R>) {
                    original(std::forward<std::tuple_element_t<I, Args>>(args)...);
                    deliver(callbacks, {});
                } else {
                    R result = original(std::forward<std::tuple_element_t<I, Args>>(args)...);
                    using Value = std::remove_cvref_t<R>;
                    static_assert(std::is_copy_constructible_v<Value>, "Use Observed for move-only event results");
                    deliver(callbacks, Object(std::make_shared<Value>(result)));
                    return result;
                }
            });
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
    }
    template<std::meta::info M> void on_change(std::function<void(Object&)> callback) {
        using V = [:std::meta::type_of(M):];
        using U = std::remove_cv_t<V>;
        auto callbacks = listeners(std::meta::identifier_of(M));
        bool first = callbacks->empty();
        callbacks->push_back(std::move(callback));
        if (!first) return;
        auto read = this->template target<M>(OperationKind::read);
        auto write = this->template target<M>(OperationKind::write);
        auto options = write.options();
        auto target = make_target<void(U)>([callbacks, read, write](U value) {
            detail::call_bound<void>(write, false, std::move(value));
            auto current = detail::call_bound<U>(read, true);
            deliver(callbacks, Object(std::make_shared<U>(std::move(current))));
        }, options);
        this->template replace<M>(detail::require_target(std::move(target)), OperationKind::write);
    }
    void emit(std::string_view name, Object value = {}) {
        auto found = hooks_.find(std::string(name));
        if (found != hooks_.end()) deliver(found->second, std::move(value));
    }
    void connect_signal(std::string_view name, std::function<void(Object&)> callback) {
        listeners(name)->push_back(std::move(callback));
    }
    template<std::meta::info M> void connect_to(Hooks* other, std::string_view slot) {
        connect<M>([other, name = std::string(slot)](Object& value) { other->emit(name, value); });
    }
};
}
