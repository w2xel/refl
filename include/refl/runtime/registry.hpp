#pragma once
#include <refl/core/descriptor.hpp>
#include <mutex>
#include <unordered_map>

namespace refl {
class Registry {
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const ClassInfo>> classes_;
    std::unordered_map<std::string, std::shared_ptr<const EnumInfo>> enums_;
    template<class T> Result<void> publish_into(
        std::unordered_map<std::string, std::shared_ptr<const T>>& entries,
        std::shared_ptr<const T> descriptor) {
        if (!descriptor || descriptor->name.empty())
            return std::unexpected(Diagnostic{DiagnosticCode::null_handle});
        std::lock_guard lock(mutex_);
        auto [it, inserted] = entries.emplace(descriptor->name, descriptor);
        if (!inserted && it->second != descriptor)
            return std::unexpected(Diagnostic{DiagnosticCode::conflict});
        return {};
    }
public:
    Result<void> publish(std::shared_ptr<const ClassInfo> descriptor) {
        return publish_into(classes_, std::move(descriptor));
    }
    Result<void> publish(std::shared_ptr<const EnumInfo> descriptor) {
        return publish_into(enums_, std::move(descriptor));
    }
    std::shared_ptr<const ClassInfo> find_class(std::string_view name) const {
        std::lock_guard lock(mutex_);
        auto it = classes_.find(std::string(name));
        return it == classes_.end() ? nullptr : it->second;
    }
    std::shared_ptr<const EnumInfo> find_enum(std::string_view name) const {
        std::lock_guard lock(mutex_);
        auto it = enums_.find(std::string(name));
        return it == enums_.end() ? nullptr : it->second;
    }
    std::vector<std::string> class_names() const {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        for (const auto& entry : classes_) result.push_back(entry.first);
        return result;
    }
    std::vector<std::string> enum_names() const {
        std::lock_guard lock(mutex_);
        std::vector<std::string> result;
        for (const auto& entry : enums_) result.push_back(entry.first);
        return result;
    }
};
inline Registry& default_registry() { static Registry registry; return registry; }
}
