#pragma once
#include <refl/runtime/invoke.hpp>
#include <map>

namespace refl {
class DispatchTable {
public:
    using Targets = std::map<OperationKey, CallTarget>;
private:
    struct Generation { Targets baseline, current; ObjectView native; };
    struct Live { std::shared_ptr<Generation> current = std::make_shared<Generation>(); };
    std::shared_ptr<const InterfaceSchema> schema_;
    std::shared_ptr<Live> live_ = std::make_shared<Live>();
    DispatchTable(std::shared_ptr<const InterfaceSchema> schema, std::shared_ptr<Live> live)
        : schema_(std::move(schema)), live_(std::move(live)) {}
    Result<void> validate(OperationKey key, const CallTarget& target) const {
        for (const auto& operation : *schema_) {
            if (OperationKey{operation.member, operation.operation} != key) continue;
            if (target.valid() && compatible_signature(operation.signature, target.signature()) &&
                target.options().operation == operation.operation) return {};
            return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, key.first});
        }
        return std::unexpected(Diagnostic{DiagnosticCode::not_found, key.first});
    }
    static Result<ResolvedCall> resolve(std::shared_ptr<Generation> generation, OperationKey key) {
        auto found = generation->current.find(key);
        if (found == generation->current.end())
            return std::unexpected(Diagnostic{DiagnosticCode::not_found, key.first});
        return ResolvedCall{found->second, found->second.options().receiver, generation};
    }
    std::shared_ptr<const InterfaceSchema> schema_for(OperationKind kind) const {
        auto result = std::make_shared<InterfaceSchema>();
        for (const auto& operation : *schema_)
            if (operation.operation == kind) result->push_back(operation);
        return result;
    }
public:
    explicit DispatchTable(std::shared_ptr<const InterfaceSchema> schema) : schema_(std::move(schema)) {
        if (!schema_) throw ReflectionError({DiagnosticCode::null_handle});
        std::map<OperationKey, bool> keys;
        for (const auto& operation : *schema_) {
            if (!supported_signature(operation.signature) || operation.signature.is_noexcept)
                throw ReflectionError({DiagnosticCode::unsupported, operation.member});
            if (!keys.emplace(OperationKey{operation.member, operation.operation}, true).second)
                throw ReflectionError({DiagnosticCode::ambiguous, operation.member});
        }
    }
    DispatchHandle dispatch(OperationKind kind = OperationKind::method) const {
        return {schema_for(kind), [live = live_, kind](MemberId member) {
            return resolve(live->current, {member, kind});
        }};
    }
    DispatchHandle capture(OperationKind kind = OperationKind::method) const {
        return {schema_for(kind), [generation = live_->current, kind](MemberId member) {
            return resolve(generation, {member, kind});
        }};
    }
    // The callback holds no state owner. Lock it before modifying state from a slot.
    auto weak() const {
        return [weak = std::weak_ptr<Live>(live_), schema = schema_]() {
            auto live = weak.lock();
            if (!live) throw ReflectionError({DiagnosticCode::null_handle});
            return DispatchTable(schema, std::move(live));
        };
    }
    ObjectView native() const { return live_->current->native; }
    Result<CallTarget> target(MemberId member, OperationKind kind = OperationKind::method) const {
        auto call = resolve(live_->current, {member, kind});
        if (!call) return std::unexpected(call.error());
        return call->target;
    }
    Result<void> replace(MemberId member, CallTarget target, OperationKind kind = OperationKind::method) {
        auto checked = validate({member, kind}, target);
        if (!checked) return checked;
        auto state = live_->current; // A retired callable can reenter state publication.
        state->current.insert_or_assign({member, kind}, std::move(target));
        return {};
    }
    Result<void> reset(Targets targets, ObjectView native = {}) {
        if (native.valid() && !native.anchor()) return std::unexpected(Diagnostic{DiagnosticCode::missing_anchor});
        if (targets.size() != schema_->size()) return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch});
        for (const auto& [key, target] : targets) {
            auto checked = validate(key, target);
            if (!checked) return checked;
        }
        auto next = std::make_shared<Generation>(targets, targets, std::move(native));
        live_->current = std::move(next);
        return {};
    }
    Result<void> restore(MemberId member, OperationKind kind = OperationKind::method) {
        auto state = live_->current;
        auto found = state->baseline.find({member, kind});
        if (found == state->baseline.end()) return std::unexpected(Diagnostic{DiagnosticCode::not_found, member});
        state->current.insert_or_assign(found->first, found->second);
        return {};
    }
    void detach_native() {
        auto next = std::make_shared<Generation>();
        for (const auto& [key, target] : live_->current->current)
            if (target.options().native_dependency == NativeDependency::independent)
                next->current.emplace(key, target);
        live_->current = std::move(next);
    }
};
}
