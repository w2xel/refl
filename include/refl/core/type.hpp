#pragma once
#include <cstddef>
#include <functional>
#include <tuple>
#include <type_traits>
#include <vector>

namespace refl {
namespace detail { template<class T> inline char identity_token; }
struct TypeId {
    const void* token = nullptr;
    friend bool operator==(TypeId, TypeId) = default;
    friend bool operator<(TypeId a, TypeId b) { return std::less<const void*>{}(a.token, b.token); }
};
template<class T> constexpr TypeId type_id() { return {&detail::identity_token<T>}; }
enum class Cv { none, constant, volatile_, constant_volatile };
enum class ReferenceKind { none, lvalue, rvalue };
struct TypeUse {
    TypeId type;
    Cv qualifiers = Cv::none;
    ReferenceKind reference = ReferenceKind::none;
    bool copyable = false;
    friend bool operator==(const TypeUse&, const TypeUse&) = default;
};
template<class T> constexpr TypeUse type_use() {
    using U = std::remove_reference_t<T>;
    return {type_id<std::remove_cv_t<U>>(),
        std::is_const_v<U> ? (std::is_volatile_v<U> ? Cv::constant_volatile : Cv::constant)
                           : (std::is_volatile_v<U> ? Cv::volatile_ : Cv::none),
        std::is_lvalue_reference_v<T> ? ReferenceKind::lvalue :
        std::is_rvalue_reference_v<T> ? ReferenceKind::rvalue : ReferenceKind::none,
        std::is_copy_constructible_v<std::remove_cv_t<U>>};
}
constexpr bool is_const(Cv cv) { return cv == Cv::constant || cv == Cv::constant_volatile; }
constexpr bool is_volatile(Cv cv) { return cv == Cv::volatile_ || cv == Cv::constant_volatile; }
struct MemberId {
    TypeId declaring_type;
    std::size_t declaration_index = 0;
    friend bool operator==(MemberId, MemberId) = default;
    friend bool operator<(MemberId a, MemberId b) {
        return a.declaring_type == b.declaring_type ? a.declaration_index < b.declaration_index
                                                   : a.declaring_type < b.declaring_type;
    }
};
enum class OperationKind { method, construct, read, write, view };
struct ReceiverQualifiers {
    Cv qualifiers = Cv::none;
    ReferenceKind reference = ReferenceKind::none;
    friend bool operator==(ReceiverQualifiers, ReceiverQualifiers) = default;
};
struct Signature {
    std::vector<TypeUse> parameters;
    TypeUse result;
    ReceiverQualifiers receiver;
    bool is_noexcept = false;
    friend bool operator==(const Signature&, const Signature&) = default;
};
namespace detail {
template<class S> struct signature_traits;
template<class R, class... A> struct signature_traits<R(A...)> {
    using result = R;
    using arguments = std::tuple<A...>;
    static Signature signature() { return {{type_use<A>()...}, type_use<R>(), {}, false}; }
};
#define REFL_SIGNATURE_QUALIFIERS(Q, CV, REF, NOEX) \
    template<class R, class... A> struct signature_traits<R(A...) Q> : signature_traits<R(A...)> { \
        static Signature signature() { return {{type_use<A>()...}, type_use<R>(), {CV, REF}, NOEX}; } \
    };
REFL_SIGNATURE_QUALIFIERS(const, Cv::constant, ReferenceKind::none, false)
REFL_SIGNATURE_QUALIFIERS(volatile, Cv::volatile_, ReferenceKind::none, false)
REFL_SIGNATURE_QUALIFIERS(const volatile, Cv::constant_volatile, ReferenceKind::none, false)
REFL_SIGNATURE_QUALIFIERS(&, Cv::none, ReferenceKind::lvalue, false)
REFL_SIGNATURE_QUALIFIERS(const &, Cv::constant, ReferenceKind::lvalue, false)
REFL_SIGNATURE_QUALIFIERS(&&, Cv::none, ReferenceKind::rvalue, false)
REFL_SIGNATURE_QUALIFIERS(const &&, Cv::constant, ReferenceKind::rvalue, false)
REFL_SIGNATURE_QUALIFIERS(noexcept, Cv::none, ReferenceKind::none, true)
REFL_SIGNATURE_QUALIFIERS(const noexcept, Cv::constant, ReferenceKind::none, true)
#undef REFL_SIGNATURE_QUALIFIERS
}
template<class S> Signature signature_of() { return detail::signature_traits<S>::signature(); }
}
