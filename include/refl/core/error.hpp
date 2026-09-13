// Runtime diagnostics; independent of compiler reflection.
#pragma once

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

}  // namespace refl
