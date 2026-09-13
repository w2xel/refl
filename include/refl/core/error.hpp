// Runtime diagnostics; independent of compiler reflection.
#pragma once
#include <refl/core/type.hpp>
#include <expected>
#include <stdexcept>

namespace refl {

// ---------------------------------------------------------------------------
// Error — returned inside std::expected when a lookup fails.
// ---------------------------------------------------------------------------
enum class Error {
    NotFound,
    NullHandle,
    TypeError,
    ArityMismatch,   // argument count doesn't match the member's param count
    ReadOnly,        // write to a const / bit-field member
    NotOwned,        // cast_safe / clone on a non-owning (borrowed) Object
    NotCopyable,     // clone of a non-copy-constructible class / get on move-only
    Ambiguous,       // name resolves to 2+ base subobjects (diamond / multi-base)
};

enum class DiagnosticCode {
    not_found, null_handle, type_mismatch, arity_mismatch, read_only,
    requires_consumption, unsupported, reference_export, missing_anchor, conflict, ambiguous
};
struct Diagnostic {
    DiagnosticCode code;
    MemberId member = {};
    TypeUse expected = {};
    TypeUse actual = {};
    std::size_t argument_index = static_cast<std::size_t>(-1);
};
template<class T> using Result = std::expected<T, Diagnostic>;
class ReflectionError : public std::runtime_error {
public:
    Diagnostic diagnostic;
    explicit ReflectionError(Diagnostic d) : std::runtime_error("reflection contract failure"), diagnostic(d) {}
};
}  // namespace refl
