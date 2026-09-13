#pragma once
#include <refl/runtime/invoke.hpp>
#include <map>

// Prototype state provider. Production calls use refl/runtime/invoke.hpp.
namespace prototype {
class Source {
    struct Generation {
        std::map<refl::MemberId, refl::CallTarget> baseline;
        std::map<refl::MemberId, refl::CallTarget> slots;
    };
    struct Live {
        std::shared_ptr<Generation> current = std::make_shared<Generation>();
    };
    std::shared_ptr<const refl::InterfaceSchema> schema_;
    std::shared_ptr<Live> live_ = std::make_shared<Live>();
    static refl::Result<refl::ResolvedCall> resolve(const std::shared_ptr<Generation>& state,
                                                  refl::MemberId member) {
        auto it = state->slots.find(member);
        if (it == state->slots.end() || !it->second.valid())
            return std::unexpected(refl::Diagnostic{refl::DiagnosticCode::not_found, member});
        return refl::ResolvedCall{it->second, it->second.options().receiver, state};
    }
    bool compatible(refl::MemberId member, const refl::CallTarget& target) const {
        for (const auto& requirement : *schema_) {
            if (requirement.member != member) continue;
            auto actual = target.signature();
            actual.is_noexcept = requirement.signature.is_noexcept;
            return actual == requirement.signature && target.options().operation == requirement.operation;
        }
        return false;
    }
public:
    explicit Source(std::shared_ptr<const refl::InterfaceSchema> schema) : schema_(std::move(schema)) {
        for (const auto& operation : *schema_)
            if (!refl::supported_signature(operation.signature) || operation.signature.is_noexcept)
                throw refl::ReflectionError({refl::DiagnosticCode::unsupported, operation.member});
    }
    refl::DispatchHandle dispatch() const {
        return {schema_, [live = live_](auto member) { return resolve(live->current, member); }};
    }
    refl::DispatchHandle capture() const {
        return {schema_, [state = live_->current](auto member) { return resolve(state, member); }};
    }
    refl::Result<void> replace(refl::MemberId member, refl::CallTarget target) {
        if (!target.valid() || !compatible(member, target))
            return std::unexpected(refl::Diagnostic{refl::DiagnosticCode::type_mismatch, member});
        live_->current->slots[member] = std::move(target);
        return {};
    }
    refl::Result<void> reset(std::map<refl::MemberId, refl::CallTarget> native) {
        if (native.size() != schema_->size())
            return std::unexpected(refl::Diagnostic{refl::DiagnosticCode::type_mismatch});
        for (const auto& [member, target] : native)
            if (!target.valid() || !compatible(member, target))
                return std::unexpected(refl::Diagnostic{refl::DiagnosticCode::type_mismatch, member});
        auto next = std::make_shared<Generation>(native, native);
        live_->current = std::move(next);
        return {};
    }
    refl::CallTarget target(refl::MemberId member) const { return live_->current->slots.at(member); }
    void restore(refl::MemberId member) { live_->current->slots[member] = live_->current->baseline.at(member); }
    void detach() {
        auto next = std::make_shared<Generation>();
        for (const auto& [member, target] : live_->current->slots)
            if (target.options().native_dependency == refl::NativeDependency::independent)
                next->slots.emplace(member, target);
        live_->current = std::move(next);
    }
};
}
