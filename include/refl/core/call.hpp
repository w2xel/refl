#pragma once
#include <refl/core/value.hpp>
#include <array>
#include <functional>
#include <span>
#include <string>

namespace refl {
enum class ResultLifetime { unknown, receiver, argument, callable_context, external };
enum class ReferenceExport { restricted, caller_borrow };
enum class NativeDependency { unknown, independent, native };
enum class ExportKind { erased, raw_reference, retained_reference };
struct TargetOptions {
    ResultLifetime result_lifetime = ResultLifetime::unknown;
    ReferenceExport reference_export = ReferenceExport::restricted;
    NativeDependency native_dependency = NativeDependency::unknown;
    std::size_t argument_index = 0;
    ObjectView receiver = {};
    LifetimeAnchor external_anchor = {};
    OperationKind operation = OperationKind::method;
};
struct CallFrame {
    ObjectView receiver = {};
    std::span<const ArgumentView> arguments;
};
constexpr bool supported_signature(const Signature& signature) {
    if (is_volatile(signature.receiver.qualifiers) || signature.receiver.reference != ReferenceKind::none)
        return false;
    if (is_volatile(signature.result.qualifiers) || signature.result.reference == ReferenceKind::rvalue)
        return false;
    for (auto p : signature.parameters) if (is_volatile(p.qualifiers)) return false;
    return true;
}
namespace detail {
template<class T> decltype(auto) call_argument(const ArgumentView& arg) {
    using U = std::remove_reference_t<T>;
    using Bare = std::remove_cv_t<U>;
    if constexpr (std::is_reference_v<T>) {
        return static_cast<T>(**arg.object.template get<U>());
    } else {
        if constexpr (std::is_copy_constructible_v<Bare>) {
            if (arg.category == ValueCategory::lvalue || arg.object.read_only())
                return Bare(**arg.object.template get<const Bare>());
        }
        return Bare(std::move(**arg.object.template get<Bare>()));
    }
}
inline LifetimeAnchor result_anchor(const TargetOptions& options, const CallFrame& frame,
                                    LifetimeAnchor context) {
    switch (options.result_lifetime) {
        case ResultLifetime::receiver: return frame.receiver.anchor();
        case ResultLifetime::argument:
            return options.argument_index < frame.arguments.size()
                ? frame.arguments[options.argument_index].object.anchor() : LifetimeAnchor{};
        case ResultLifetime::callable_context: return context;
        case ResultLifetime::external: return options.external_anchor;
        case ResultLifetime::unknown: return {};
    }
    return {};
}
}
class CallTarget {
    struct Record {
        Signature signature;
        TargetOptions options;
        std::function<CallResult(CallFrame&)> invoke;
    };
    std::shared_ptr<const Record> record_;
public:
    CallTarget() = default;
    bool valid() const { return static_cast<bool>(record_); }
    const Signature& signature() const { return record_->signature; }
    const TargetOptions& options() const { return record_->options; }
    // The runtime validator is the only public invocation entry point.
    friend Result<CallResult> invoke_target(const CallTarget&, CallFrame&, MemberId, ExportKind);
    template<class T, class... A> static Result<CallTarget> constructor() {
        CallTarget target;
        TargetOptions options;
        options.operation = OperationKind::construct;
        target.record_ = std::make_shared<const Record>(signature_of<T(A...)>(), options,
            [](CallFrame& frame) -> CallResult {
                return [&]<std::size_t... I>(std::index_sequence<I...>) -> CallResult {
                    return OwnedValue::construct<T>(detail::call_argument<A>(frame.arguments[I])...);
                }(std::index_sequence_for<A...>{});
            });
        return target;
    }
    template<class S, class F> static Result<CallTarget> make(F&& callable, TargetOptions options = {}) {
        using Traits = detail::signature_traits<S>;
        using R = typename Traits::result;
        using Args = typename Traits::arguments;
        auto signature = signature_of<S>();
        if (!supported_signature(signature)) return std::unexpected(Diagnostic{DiagnosticCode::unsupported});
        constexpr bool supported_types = []<std::size_t... I>(std::index_sequence<I...>) {
            return !std::is_volatile_v<std::remove_reference_t<R>> && !std::is_rvalue_reference_v<R> &&
                (!std::is_volatile_v<std::remove_reference_t<std::tuple_element_t<I, Args>>> && ...) &&
                std::is_invocable_r_v<R, std::decay_t<F>&, std::tuple_element_t<I, Args>...>;
        }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        if constexpr (!supported_types) {
            return std::unexpected(Diagnostic{DiagnosticCode::unsupported});
        } else {
        auto context = std::make_shared<std::decay_t<F>>(std::forward<F>(callable));
        CallTarget target;
        auto invoke = [context, options](CallFrame& frame) -> CallResult {
            return [&]<std::size_t... I>(std::index_sequence<I...>) -> CallResult {
                if constexpr (std::is_void_v<R>) {
                    std::invoke(*context, detail::call_argument<std::tuple_element_t<I, Args>>(frame.arguments[I])...);
                    return VoidResult{};
                } else if constexpr (std::is_lvalue_reference_v<R>) {
                    R result = std::invoke(*context, detail::call_argument<std::tuple_element_t<I, Args>>(frame.arguments[I])...);
                    return ObjectView::from(result, detail::result_anchor(options, frame, context));
                } else if constexpr (!std::is_rvalue_reference_v<R>) {
                    return OwnedValue::from(R(std::invoke(*context,
                        detail::call_argument<std::tuple_element_t<I, Args>>(frame.arguments[I])...)));
                } else {
                    throw ReflectionError({DiagnosticCode::unsupported});
                }
            }(std::make_index_sequence<std::tuple_size_v<Args>>{});
        };
        target.record_ = std::make_shared<const Record>(std::move(signature), std::move(options), std::move(invoke));
        return target;
        }
    }
};
template<class S, class F> Result<CallTarget> make_target(F&& callable, TargetOptions options = {}) {
    return CallTarget::make<S>(std::forward<F>(callable), std::move(options));
}
using OperationKey = std::pair<MemberId, OperationKind>;
struct OperationDescriptor {
    MemberId member;
    std::string name;
    Signature signature;
    OperationKind operation = OperationKind::method;
};
using InterfaceSchema = std::vector<OperationDescriptor>;
struct ResolvedCall {
    CallTarget target;
    ObjectView receiver = {};
    LifetimeAnchor state_owner;
};
class DispatchHandle {
public:
    using Completion = std::function<void(const CallFrame&, const CallResult&)>;
    using PrepareCompletion = std::function<Completion(MemberId, OperationKind)>;
private:
    std::shared_ptr<const InterfaceSchema> schema_;
    std::function<Result<ResolvedCall>(MemberId)> resolve_;
    PrepareCompletion prepare_completion_;
public:
    DispatchHandle() = default;
    DispatchHandle(std::shared_ptr<const InterfaceSchema> schema,
                   std::function<Result<ResolvedCall>(MemberId)> resolve,
                   PrepareCompletion prepare_completion = {})
        : schema_(std::move(schema)), resolve_(std::move(resolve)),
          prepare_completion_(std::move(prepare_completion)) {}
    bool valid() const { return schema_ && static_cast<bool>(resolve_); }
    const std::shared_ptr<const InterfaceSchema>& schema() const { return schema_; }
    Result<ResolvedCall> resolve(MemberId member) const {
        if (!valid()) return std::unexpected(Diagnostic{DiagnosticCode::null_handle, member});
        const OperationDescriptor* operation = nullptr;
        for (const auto& candidate : *schema_)
            if (candidate.member == member) { operation = &candidate; break; }
        if (!operation) return std::unexpected(Diagnostic{DiagnosticCode::not_found, member});
        auto result = resolve_(member);
        if (!result) return result;
        if (!result->target.valid()) return std::unexpected(Diagnostic{DiagnosticCode::null_handle, member});
        if (!compatible_signature(operation->signature, result->target.signature()) ||
            operation->operation != result->target.options().operation)
            return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, member});
        return result;
    }
    Completion prepare_completion(MemberId member, OperationKind operation) const {
        return prepare_completion_ ? prepare_completion_(member, operation) : Completion{};
    }
    DispatchHandle with_completion(PrepareCompletion prepare) const {
        auto result = *this;
        auto previous = prepare_completion_;
        result.prepare_completion_ = [previous = std::move(previous), prepare = std::move(prepare)]
            (MemberId member, OperationKind operation) {
                auto first = previous ? previous(member, operation) : Completion{};
                auto second = prepare ? prepare(member, operation) : Completion{};
                if (!first) return second;
                if (!second) return first;
                return Completion([first = std::move(first), second = std::move(second)]
                    (const CallFrame& frame, const CallResult& result) {
                        first(frame, result);
                        second(frame, result);
                    });
            };
        return result;
    }
};
}
