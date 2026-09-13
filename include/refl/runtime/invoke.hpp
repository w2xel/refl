#pragma once
#include <refl/core/call.hpp>

namespace refl {
inline Result<CallResult> invoke_target(const CallTarget& target, CallFrame& frame,
                                        MemberId member, ExportKind mode) {
    auto error = [&](DiagnosticCode code, TypeUse expected = {}, TypeUse actual = {},
                     std::size_t index = static_cast<std::size_t>(-1)) -> Result<CallResult> {
        return std::unexpected(Diagnostic{code, member, expected, actual, index});
    };
    if (!target.valid()) return error(DiagnosticCode::null_handle);
    if (target.options().receiver.valid()) {
        const bool read_only = frame.receiver.read_only();
        frame.receiver = target.options().receiver;
        if (read_only) frame.receiver = frame.receiver.as_const();
    }
    const auto& signature = target.signature();
    if (!supported_signature(signature)) return error(DiagnosticCode::unsupported);
    if (signature.parameters.size() != frame.arguments.size()) return error(DiagnosticCode::arity_mismatch);
    if (frame.receiver.valid() && frame.receiver.read_only() && !is_const(signature.receiver.qualifiers))
        return error(DiagnosticCode::read_only);
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        auto expected = signature.parameters[i];
        const auto& arg = frame.arguments[i];
        auto actual = arg.object.type();
        if (!arg.object.valid()) return error(DiagnosticCode::null_handle, expected, actual, i);
        if (expected.type != actual.type) return error(DiagnosticCode::type_mismatch, expected, actual, i);
        if (expected.reference != ReferenceKind::none && !is_const(expected.qualifiers) && arg.object.read_only())
            return error(DiagnosticCode::read_only, expected, actual, i);
        if (expected.reference == ReferenceKind::lvalue && !is_const(expected.qualifiers)
            && arg.category != ValueCategory::lvalue)
            return error(DiagnosticCode::type_mismatch, expected, actual, i);
        if ((expected.reference == ReferenceKind::rvalue ||
             (expected.reference == ReferenceKind::none && !expected.copyable)) &&
            (arg.category != ValueCategory::consumable || arg.object.read_only()))
            return error(DiagnosticCode::requires_consumption, expected, actual, i);
    }
    const auto& options = target.options();
    if (signature.result.reference != ReferenceKind::none) {
        if (mode == ExportKind::raw_reference && options.reference_export != ReferenceExport::caller_borrow)
            return error(DiagnosticCode::reference_export);
        if (mode == ExportKind::raw_reference && options.result_lifetime == ResultLifetime::argument &&
            (options.argument_index >= frame.arguments.size() ||
             frame.arguments[options.argument_index].category == ValueCategory::consumable))
            return error(DiagnosticCode::reference_export);
        if (options.result_lifetime == ResultLifetime::unknown) {
            for (const auto& arg : frame.arguments)
                if (arg.category == ValueCategory::consumable) return error(DiagnosticCode::reference_export);
        }
        if (mode == ExportKind::retained_reference &&
            (options.result_lifetime == ResultLifetime::unknown ||
             (options.result_lifetime != ResultLifetime::callable_context &&
              !detail::result_anchor(options, frame, {}))))
            return error(DiagnosticCode::missing_anchor);
    } else if (mode != ExportKind::erased) {
        return error(DiagnosticCode::type_mismatch);
    }
    auto result = target.record_->invoke(frame);
    if (mode == ExportKind::retained_reference && !result_view(result).anchor())
        return error(DiagnosticCode::missing_anchor);
    return result;
}
inline Result<CallResult> invoke(const DispatchHandle& source, MemberId member,
                                 std::span<const ArgumentView> arguments, ExportKind mode = ExportKind::erased,
                                 bool read_only_receiver = false) {
    auto call = source.resolve(member);
    if (!call) return std::unexpected(call.error());
    CallFrame frame{call->receiver, arguments};
    if (read_only_receiver) frame.receiver = frame.receiver.as_const();
    auto completion = source.prepare_completion(member, call->target.options().operation);
    auto result = invoke_target(call->target, frame, member, mode);
    if (result && completion) completion(frame, *result);
    return result;
}
template<class Source>
Result<void> check_result_type(const Source& source, MemberId member, TypeUse expected) {
    if (!source.schema()) return std::unexpected(Diagnostic{DiagnosticCode::null_handle, member});
    for (const auto& operation : *source.schema()) {
        if (operation.member != member) continue;
        if (operation.signature.result != expected)
            return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, member, expected, operation.signature.result});
        return {};
    }
    return std::unexpected(Diagnostic{DiagnosticCode::not_found, member});
}
template<class R> using TypedResult = std::conditional_t<std::is_lvalue_reference_v<R>,
    std::reference_wrapper<std::remove_reference_t<R>>, R>;
template<class R, class Source, class... A>
Result<TypedResult<R>> try_call(const Source& source, MemberId member, A&&... values) {
    auto compatible = check_result_type(source, member, type_use<R>());
    if (!compatible) return std::unexpected(compatible.error());
    std::array<ArgumentView, sizeof...(A)> arguments{argument(std::forward<A>(values))...};
    auto result = invoke(source, member, arguments,
                         std::is_reference_v<R> ? ExportKind::raw_reference : ExportKind::erased);
    if (!result) return std::unexpected(result.error());
    if constexpr (std::is_void_v<R>) {
        if (!std::holds_alternative<VoidResult>(*result))
            return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, member});
        return {};
    } else if constexpr (std::is_lvalue_reference_v<R>) {
        auto ptr = result_view(*result).template get<std::remove_reference_t<R>>();
        if (!ptr) return std::unexpected(ptr.error());
        return std::ref(**ptr);
    } else {
        auto owned = std::get_if<OwnedValue>(&*result);
        if (!owned) return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, member});
        return owned->template take<R>();
    }
}
template<class T, class Source, class... A>
Result<RetainedRef<T>> try_call_retained(const Source& source, MemberId member, A&&... values) {
    auto compatible = check_result_type(source, member, type_use<T&>());
    if (!compatible) return std::unexpected(compatible.error());
    std::array<ArgumentView, sizeof...(A)> arguments{argument(std::forward<A>(values))...};
    auto result = invoke(source, member, arguments, ExportKind::retained_reference);
    if (!result) return std::unexpected(result.error());
    auto view = result_view(*result);
    auto ptr = view.template get<T>();
    if (!ptr) return std::unexpected(ptr.error());
    if (!view.anchor()) return std::unexpected(Diagnostic{DiagnosticCode::missing_anchor, member});
    return RetainedRef<T>(std::move(view));
}
}
