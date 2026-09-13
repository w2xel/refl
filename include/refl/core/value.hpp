#pragma once
#include <refl/core/type.hpp>
#include <refl/core/error.hpp>
#include <expected>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace refl {
struct ClassInfo;
namespace detail { struct ViewAccess; }
using LifetimeAnchor = std::shared_ptr<const void>;
enum class ValueCategory { lvalue, consumable };

class ObjectView {
    const void* address_ = nullptr;
    TypeUse type_ = {};
    LifetimeAnchor anchor_;
    TypeHandle descriptor_;
    std::shared_ptr<const ClassInfo> native_;
    friend struct detail::ViewAccess;
public:
    ObjectView() = default;
    template<class T> static ObjectView from(T& value, LifetimeAnchor anchor = {}) {
        static_assert(!std::is_volatile_v<T>, "volatile storage is unsupported");
        ObjectView view;
        view.address_ = std::addressof(value);
        view.type_ = type_use<T>();
        view.descriptor_ = describe_type<T>();
        view.anchor_ = std::move(anchor);
        return view;
    }
    bool valid() const { return address_ != nullptr; }
    TypeUse type() const { return type_; }
    const TypeHandle& descriptor() const { return descriptor_; }
    const std::shared_ptr<const ClassInfo>& native_descriptor() const { return native_; }
    const void* address() const { return address_; }
    const LifetimeAnchor& anchor() const { return anchor_; }
    bool read_only() const { return is_const(type_.qualifiers); }
    ObjectView as_const() const {
        auto copy = *this;
        copy.type_.qualifiers = Cv::constant;
        return copy;
    }
    template<class T> Result<T*> get() const {
        if (!valid()) return std::unexpected(Diagnostic{DiagnosticCode::null_handle});
        if (type_.type != type_id<std::remove_cv_t<T>>())
            return std::unexpected(Diagnostic{DiagnosticCode::type_mismatch, {}, type_use<T>(), type_});
        if (read_only() && !std::is_const_v<T>)
            return std::unexpected(Diagnostic{DiagnosticCode::read_only});
        return static_cast<T*>(const_cast<void*>(address_));
    }
};
namespace detail {
// Trusted bridge for generated metadata and the legacy erased API.
struct ViewAccess {
    static ObjectView attach(ObjectView view, std::shared_ptr<const ClassInfo> descriptor) {
        view.native_ = std::move(descriptor);
        return view;
    }
};
}
template<class T> ObjectView borrow(T& value) { return ObjectView::from(value); }
template<class T> ObjectView borrow_const(const T& value) { return ObjectView::from(value); }
struct ArgumentView {
    ObjectView object;
    ValueCategory category = ValueCategory::lvalue;
};
template<class T> ArgumentView argument(T&& value) {
    return {borrow(value), std::is_lvalue_reference_v<T> ? ValueCategory::lvalue : ValueCategory::consumable};
}
inline ArgumentView argument(ArgumentView value) { return value; }

// Owning call-result storage. The legacy Object facade is adapted at its boundary.
class OwnedValue {
    ObjectView view_;
public:
    OwnedValue() = default;
    template<class T, class... A> static OwnedValue construct(A&&... args) {
        auto owner = std::make_shared<T>(std::forward<A>(args)...);
        OwnedValue result;
        result.view_ = ObjectView::from(*owner, owner);
        return result;
    }
    template<class T> static OwnedValue from(T&& value) {
        auto owner = std::make_shared<std::remove_cvref_t<T>>(std::forward<T>(value));
        OwnedValue result;
        result.view_ = ObjectView::from(*owner, owner);
        return result;
    }
    ObjectView view() const { return view_; }
    template<class T> Result<T> take() {
        auto ptr = view_.template get<T>();
        if (!ptr) return std::unexpected(ptr.error());
        return T(std::move(**ptr));
    }
};
struct VoidResult {};
using CallResult = std::variant<VoidResult, OwnedValue, ObjectView>;
inline ObjectView result_view(const CallResult& result) {
    if (auto owned = std::get_if<OwnedValue>(&result)) return owned->view();
    if (auto view = std::get_if<ObjectView>(&result)) return *view;
    return {};
}
template<class T> class RetainedRef {
    ObjectView view_;
public:
    explicit RetainedRef(ObjectView view) : view_(std::move(view)) {
        auto ptr = view_.template get<T>();
        if (!ptr) throw ReflectionError(ptr.error());
        if (!view_.anchor()) throw ReflectionError({DiagnosticCode::missing_anchor});
    }
    T& get() const { return **view_.template get<T>(); }
};
}
