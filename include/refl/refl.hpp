// refl — C++26 compile-time-reflection-based runtime reflection framework (core).
//
// Register a type T with refl::Reg<T> (or refl::ensure_registered<T>()) so it
// enters a global pool queryable at runtime via find_class("T").  From a
// Class handle you can find constructors, member functions, and data fields
// by name or parameter-type name, then construct objects, invoke functions,
// and get/set fields through the returned handles.
//
// The framework is type-erased at the call boundary: Constructor::call
// returns an Object (an owning, type-erased handle).  You only need to know
// the concrete type when you explicitly cast an Object to get at its
// members directly.
//
// Lookups return std::expected<T, Error> — dereference with operator* or
// check has_value()/error(), as shown in the design docs.
//
// The typed dispatch / dynamic-implementation layer (Dyn<T>) lives in
// refl/dyn.hpp and includes this header.
#pragma once

#include <meta>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

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

// Forward declarations — needed for function pointer type aliases below.
class Object;

// ---------------------------------------------------------------------------
// Type-erased function pointer signatures for factories, invokers, getters,
// and setters.
//
// Args arrive as const Object* — each carries a void* to the caller's
// stack storage plus a string_view type name (from display_string_of, no
// RTTI).  The invoker checks the type name against the compile-time param
// type and moves from the caller's storage.
//
// The invoker also receives the shared_ptr<void> owner of the target
// object (empty for non-owning / stack objects).  This allows
// reference-returning functions (operator=, operator+=, etc.) to return
// an aliasing Object that shares ownership with the original.
//
// Returns are Object (owning or non-owning).  Void functions return
// a default (invalid) Object.  The caller checks .valid() to distinguish
// void from a real return value.
// ---------------------------------------------------------------------------
using FactoryFn       = Object (*)(const Object* args);
using InvokerFn       = Object (*)(const std::shared_ptr<void>& owner,
                                  void* obj, const Object* args);
using GetterFn        = Object (*)(void* obj);
using SetterFn        = void  (*)(void* obj, const Object* val);
using StaticGetterFn  = Object (*)();
using StaticSetterFn  = void  (*)(const Object* val);
using StaticInvokerFn = Object (*)(const Object* args);
using CloneFn         = std::shared_ptr<void> (*)(void* obj);

struct ConstructorInfo {
    std::vector<std::string> param_types;
    FactoryFn factory;
};

struct FunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    InvokerFn invoker;
};

struct StaticFunctionInfo {
    std::string name;
    std::vector<std::string> param_types;
    std::string return_type;
    StaticInvokerFn invoker;
};

struct FieldInfo {
    std::string name;
    std::string type;
    std::ptrdiff_t offset;  // byte offset of member within T (for get_ref)
    GetterFn getter;        // nullptr if move-only (not copy-constructible)
    SetterFn setter;        // nullptr for const / not move-assignable
    bool is_const;          // true for const-qualified members
};

struct StaticFieldInfo {
    std::string name;
    std::string type;
    void* address;             // address of the static storage (for get_ref)
    StaticGetterFn getter;
    StaticSetterFn setter;  // nullptr for const / not move-assignable
    bool is_const;          // true for const-qualified members
};

struct BaseInfo {
    std::string name;
    std::ptrdiff_t offset;  // byte offset of this base within the derived class
};

struct ClassInfo {
    std::string name;
    std::vector<BaseInfo> bases;
    std::vector<FieldInfo> fields;
    std::vector<StaticFieldInfo> static_fields;
    std::vector<ConstructorInfo> constructors;
    std::vector<FunctionInfo> functions;
    std::vector<StaticFunctionInfo> static_functions;
    CloneFn clone = nullptr;  // nullptr if T is not copy-constructible
};

struct EnumeratorInfo {
    std::string name;
    long long value;
};

struct EnumInfo {
    std::string name;
    std::vector<EnumeratorInfo> enumerators;
};

// ---------------------------------------------------------------------------
// Global class and enum pools — Meyers singletons to avoid SIOF.
//
// Values are held in unique_ptr so that Class/Field/Function/... handles
// (which store const ClassInfo* / const EnumInfo* into the pool) remain
// valid across rehashes: a runtime ensure_registered<T>() after lookups
// have started can grow the unordered_map, but the heap-allocated info
// objects never move.
// ---------------------------------------------------------------------------
inline std::unordered_map<std::string, std::unique_ptr<ClassInfo>>& class_pool() {
    static std::unordered_map<std::string, std::unique_ptr<ClassInfo>> pool;
    return pool;
}

inline std::unordered_map<std::string, std::unique_ptr<EnumInfo>>& enum_pool() {
    static std::unordered_map<std::string, std::unique_ptr<EnumInfo>> pool;
    return pool;
}

inline std::mutex& pool_mutex() {
    static std::mutex m;
    return m;
}

struct Registrar {
    explicit Registrar(const ClassInfo& info) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        class_pool()[info.name] = std::make_unique<ClassInfo>(std::move(info));
    }
};

struct EnumRegistrar {
    explicit EnumRegistrar(const EnumInfo& info) {
        std::lock_guard<std::mutex> lk(pool_mutex());
        enum_pool()[info.name] = std::make_unique<EnumInfo>(std::move(info));
    }
};

// Forward declaration — defined later, used by the base-hierarchy helpers.
class Class;
std::expected<Class, Error> find_class(std::string_view name);

// ---------------------------------------------------------------------------
// Type-erased factory / invoker / getter / setter templates.
//
// Each is parameterised on the target type T and (where relevant) the
// compile-time meta::info of the specific constructor, member function, or
// data member.  template-for in make_info instantiates one of these per
// reflected member, and stores its address in ClassInfo.
// ---------------------------------------------------------------------------

namespace detail {

// Normalize a type string for the string-based overload-resolution API
// (find_constructor / find_function with type-name lists): lowercase,
// strip whitespace, ref qualifiers, and const (west- and east-const).
// Applied at both storage (param_types) and lookup so both sides compare
// the same canonical form.
//
// The invocation path (call/invoke/set) does NOT use this — arg type
// checking there is exact, via compile-time type_name<T>() (display_string_of).
//
// East-const (" const" suffix) stripping is live for hand-written query
// strings (a user may write "int const"); display_string_of emits west-const,
// so stored param_types never carry the suffix, but the query side can.
// const is stripped while whitespace still separates it as a token, so
// "const_iterator" is left untouched.
//
// Not stripped: volatile.  "const int* const" normalizes to "int*",
// indistinguishable from "const int*" — not a practical problem for
// reflected signatures (value types, pointers, references).
inline std::string normalize_type(std::string_view sv) {
    std::string s;
    for (char c : sv)
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto strip_suffix = [&](std::string_view suffix) {
        if (s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(),
                       suffix.size(), suffix) == 0) {
            s.erase(s.size() - suffix.size());
        }
    };
    auto strip_prefix = [&](std::string_view prefix) {
        if (s.size() >= prefix.size() &&
            s.compare(0, prefix.size(), prefix) == 0) {
            s.erase(0, prefix.size());
        }
    };

    // Strip ref qualifiers, then const (trailing east, leading west),
    // while whitespace separates "const" from the type name.
    strip_suffix("&&");
    strip_suffix("&");
    while (s.size() >= 6 && s.compare(s.size() - 6, 6, " const") == 0)
        s.erase(s.size() - 6);
    strip_prefix("const ");

    std::string result;
    for (char c : s)
        if (c != ' ' && c != '\t') result += c;
    return result;
}

// Compile-time fully-qualified type name for pool keys and safe-cast checks.
// display_string_of yields the qualified name (e.g. "ns::Point"), so two
// classes with the same unqualified name in different namespaces no longer
// collide in the pool or in cast_safe.  Global-scope types have no prefix.
template <typename T>
consteval std::string_view type_name() {
    return std::meta::display_string_of(^^T);
}

// Type trait: is T a shared_ptr specialization?
// Used to disambiguate the Object lvalue-borrow constructor from the
// shared_ptr owning constructor.
template <typename T> struct is_shared_ptr : std::false_type {};
template <typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template <typename T> inline constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

// Detect a consteval member function by its display string.  GCC 16.2's
// <meta> has no is_consteval query (added in a later P2996 revision), so we
// inspect display_string_of, which prefixes the specifier: "[static ]consteval
// <ret> ...".  Taking the address of an immediate function is ill-formed, so
// such members would break detail::invoker / static_invoker if registered —
// make_info skips them.  constexpr (non-consteval) functions are unaffected.
//
// Takes std::meta::info by value (not as a template parameter) so it can be
// called from regular for-loops in consteval functions, not just template-for.
consteval bool is_consteval_fn(std::meta::info Fn) {
    std::string_view ds = std::meta::display_string_of(Fn);
    if (ds.starts_with("static ")) ds.remove_prefix(7);
    return ds.starts_with("consteval ");
}

// Shared member filters — used by both make_info (refl core) and Dyn<T>'s
// consteval dispatch synthesis (dyn layer).  Dyn adds has_identifier on top
// (it needs named struct fields); make_info also accepts operators
// (has_identifier || is_operator_function).
consteval bool is_public_method(std::meta::info m) {
    return std::meta::is_function(m)
        && std::meta::is_public(m)
        && !std::meta::is_deleted(m)
        && !is_consteval_fn(m);
}

consteval bool is_public_data_member(std::meta::info m) {
    return !std::meta::is_bit_field(m)
        && std::meta::is_public(m);
}

// Match a query (already-normalized type names) against a candidate's
// param_types (also pre-normalized at storage time).  Returns true if
// the param counts and types match.
inline bool match_signature(
    const std::vector<std::string>& candidate_types,
    const std::vector<std::string>& query) {
    if (candidate_types.size() != query.size()) return false;
    for (std::size_t j = 0; j < query.size(); ++j) {
        if (candidate_types[j] != query[j]) return false;
    }
    return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Forward declarations.
// ---------------------------------------------------------------------------
class Class;
class Base;
class Constructor;
class Function;
class Field;
class StaticField;
class StaticFunction;
class Object;
class Enum;
class Enumerator;

namespace detail {

// Unlocked helper — caller must hold pool_mutex.
// Returns the byte offset of `base_name` within `derived_name` (accumulated
// through the base hierarchy), or nullopt if not found.  Offset 0 means
// either exact match or first base.
inline std::optional<std::ptrdiff_t>
base_offset_unlocked(std::string_view derived_name, std::string_view base_name) {
    if (derived_name == base_name) return 0;
    auto it = class_pool().find(std::string(derived_name));
    if (it == class_pool().end()) return std::nullopt;
    for (const auto& b : it->second->bases) {
        if (b.name == base_name) return b.offset;
        auto deeper = base_offset_unlocked(b.name, base_name);
        if (deeper) return b.offset + *deeper;
    }
    return std::nullopt;
}

// Collect every distinct byte offset from `derived_name` to `base_name`,
// one per path through the base hierarchy.  Used for ambiguity detection:
// more than one distinct offset means the base subobject is reached via
// two paths (a diamond), so an unqualified lookup is ambiguous (C++ would
// reject it).  Offsets are deduped so empty-base-optimisation merges that
// land on the same address count as one subobject.
//
// ponytail: walks all paths, O(2^n) in pathological diamond lattices; fine
// for realistic hierarchies.  Virtual inheritance is unsupported (offset_of
// is not constant for virtual bases), matching the rest of the framework.
inline std::set<std::ptrdiff_t>
base_path_offsets_unlocked(std::string_view derived_name, std::string_view base_name) {
    std::set<std::ptrdiff_t> offsets;
    if (derived_name == base_name) { offsets.insert(0); return offsets; }
    auto it = class_pool().find(std::string(derived_name));
    if (it == class_pool().end()) return offsets;
    for (const auto& b : it->second->bases) {
        if (b.name == base_name) offsets.insert(b.offset);
        for (auto o : base_path_offsets_unlocked(b.name, base_name))
            offsets.insert(b.offset + o);
    }
    return offsets;
}

// Check whether `base_name` is a base class of `derived_name` by walking
// the registered bases hierarchy.  Used by Object::is_class and cast_safe
// for runtime upcast checks.
inline bool is_base_of_unlocked(std::string_view derived_name, std::string_view base_name) {
    return base_offset_unlocked(derived_name, base_name).has_value();
}

// Check whether `base_name` is a base class of `derived_name`.
// Also returns the accumulated byte offset if it is.
inline std::optional<std::ptrdiff_t>
is_base_of_with_offset(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return base_offset_unlocked(derived_name, base_name);
}

inline bool is_base_of(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return is_base_of_unlocked(derived_name, base_name);
}

// Upcast offset with diamond-ambiguity check.  Returns the byte offset
// if the base is uniquely reachable; Error::Ambiguous if reachable via
// 2+ distinct offsets (a diamond — C++ rejects an unqualified upcast
// to the shared base); Error::TypeError if not a base at all.
// Used by Object::cast_safe / cast_ref — the cast must not silently pick
// one of two base subobjects.
inline std::expected<std::ptrdiff_t, Error>
upcast_offset_unlocked(std::string_view derived_name, std::string_view base_name) {
    auto offsets = base_path_offsets_unlocked(derived_name, base_name);
    if (offsets.empty()) return std::unexpected(Error::TypeError);
    if (offsets.size() > 1) return std::unexpected(Error::Ambiguous);
    return *offsets.begin();
}

inline std::expected<std::ptrdiff_t, Error>
upcast_offset(std::string_view derived_name, std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    return upcast_offset_unlocked(derived_name, base_name);
}

// Adjust a pointer from a most-derived object to a base subobject.
// Returns nullptr if the base is not found (guards against pool races
// after is_class passed).  Caller need not hold pool_mutex — this helper
// takes it for the offset lookup.
//
// ponytail: the call sites do is_class() then adjust_to_base() in two
// separate lock acquisitions.  This is a TOCTOU window in theory, but the
// pool is append-only at static-init time — no unregister exists — so a
// class cannot disappear between the check and the adjustment.
inline void* adjust_to_base(void* obj, std::string_view derived_name,
                            std::string_view base_name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto off = base_offset_unlocked(derived_name, base_name);
    if (!off) return nullptr;
    return static_cast<char*>(obj) + *off;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Registration — the single responsibility of the core.
//
// Call ensure_registered<T>() (or instantiate refl::Reg<T> as a static
// variable) to trigger static-initialisation of RegistrarHolder<T>::registrar,
// which populates the global pool with T's metadata.  Once registered, T is
// found via find_class("T") and constructed/called through type-erased
// handles — no wrapper object needed at call sites.
// ---------------------------------------------------------------------------

template <typename T>
class RegistrarHolder {
    static ClassInfo make_info();
public:
    static inline const Registrar registrar{make_info()};
};

// Enum registration — triggered when T is an enum type.
template <typename T>
class EnumRegistrarHolder {
    static EnumInfo make_enum_info();
public:
    static inline const EnumRegistrar registrar{make_enum_info()};
};

template <typename T>
inline void ensure_registered() {
    if constexpr (std::is_enum_v<T>) {
        (void)EnumRegistrarHolder<T>::registrar;
    } else {
        (void)RegistrarHolder<T>::registrar;
    }
}

// Registration-only tag.  Instantiate as a static variable to register T
// in the global pool at static-initialisation time — the core's sole
// user-facing side effect.
//
//   [[maybe_unused]] static refl::Reg<MyClass> reg_my_class;
//   // ... find_class("MyClass") now works.
template <typename T>
struct Reg {
    Reg() { ensure_registered<T>(); }
};
// ---------------------------------------------------------------------------
// Object — a type-erased handle to an instance, either owning (backed by
// shared_ptr) or non-owning (raw pointer into a stack or heap object).
//
// Returned by Constructor::call (owning) and Function::invoke (owning for
// value returns, aliasing for reference returns, invalid for void).  Also
// used as the argument type for invoke/get/set — constructed from any
// concrete object via the template constructor, which infers the class
// name at compile time.
//
// is_owned() distinguishes the two modes.  cast_safe<T>() returns a
// shared_ptr<T> (owning, keeps alive) for owned Objects.  cast_ref<T>()
// returns a raw T* for both modes — the caller manages lifetime.
// ---------------------------------------------------------------------------

class Object {
public:
    Object() = default;

    // Owning construction — from a shared_ptr (factory, getter, invoker).
    Object(std::shared_ptr<void> owner, std::string_view class_name)
        : owner_(std::move(owner))
        , ptr_(owner_.get())
        , class_name_(class_name) {}

    // Owning construction with separate pointer (aliasing — for reference
    // returns where the shared_ptr keeps the original alive but the
    // pointer points at a subobject or the object itself).
    Object(std::shared_ptr<void> owner, void* ptr,
           std::string_view class_name)
        : owner_(std::move(owner))
        , ptr_(ptr)
        , class_name_(class_name) {}

    // Non-owning construction — raw pointer + class name.
    // Call-scoped only: the caller must keep the source alive for the
    // duration of the call.  Storing a non-owning Object past its
    // source's lifetime is UB.  Used for argument passing and
    // introspection of stack objects; not suitable for long-lived
    // containers like Proxy.
    Object(void* ptr, std::string_view class_name)
        : ptr_(ptr), class_name_(class_name) {}

    // From shared_ptr<T> — implicit owning construction.  This is the
    // idiomatic way to create an Object from a heap-managed instance;
    // the shared_ptr keeps the object alive for the Object's lifetime.
    //
    //   auto sp = std::make_shared<Point>(1, 2);
    //   refl::Object obj = sp;           // implicit, owning
    //   refl::Proxy<IDrawable> p(sp);    // implicit, owning
    template <typename T>
        requires (not std::same_as<std::remove_cvref_t<T>, void>) &&
                 (not std::same_as<std::remove_cvref_t<T>, Object>)
    Object(const std::shared_ptr<T>& sp) noexcept
        : owner_(std::static_pointer_cast<void>(sp))
        , ptr_(sp.get())
        , class_name_(detail::type_name<std::remove_cvref_t<T>>()) {}

    // From a concrete lvalue — non-owning borrow.
    // Call-scoped only: safe for passing to invoke/get/set where the
    // Object is consumed within the call.  Storing the resulting Object
    // past the source's lifetime (e.g. in a Proxy) is UB.  Rejected for
    // const T: a mutable borrow of a const object would let cast_ref<T>()
    // write through it (UB).  A const lvalue instead falls through to the
    // rvalue ctor, which makes an owning copy.
    template <typename T>
        requires (not std::same_as<std::remove_cvref_t<T>, Object>) &&
                 (not std::is_const_v<T>) &&
                 (not detail::is_shared_ptr_v<std::remove_cvref_t<T>>)
    Object(T& obj) noexcept
        : ptr_(static_cast<void*>(std::addressof(obj)))
        , class_name_(detail::type_name<std::remove_cvref_t<T>>()) {}

    // From a concrete rvalue — owning (moves into shared_ptr).
    template <typename T>
        requires (not std::same_as<std::remove_cvref_t<T>, Object>) &&
                 (not detail::is_shared_ptr_v<std::remove_cvref_t<T>>)
    Object(T&& obj)
        : owner_(std::make_shared<std::remove_cvref_t<T>>(std::move(obj)))
        , ptr_(owner_.get())
        , class_name_(detail::type_name<std::remove_cvref_t<T>>()) {}

    // Block temporaries from the template constructor (below) — but allow
    // move construction (needed for expected<Object> returns).
    Object(Object&&) noexcept = default;
    Object(const Object&) = default;
    Object& operator=(const Object&) = default;
    Object& operator=(Object&&) noexcept = default;

    std::string_view class_name() const { return class_name_; }

    bool is_owned() const { return static_cast<bool>(owner_); }

    bool is_class(std::string_view name) const {
        if (!valid()) return false;
        if (class_name_ == name) return true;
        return detail::is_base_of(class_name_, name);
    }

    // Owning cast — returns a shared_ptr<T> that keeps the object alive.
    // Only works for owned Objects.  Returns Error::NotOwned for
    // non-owning Objects (use cast_ref instead).  Returns Error::Ambiguous
    // if T is a base reachable via 2+ distinct offsets (a diamond) — C++
    // rejects an unqualified upcast to the shared base, and so does this.
    template <typename T>
    std::expected<std::shared_ptr<T>, Error> cast_safe() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!is_owned()) return std::unexpected(Error::NotOwned);
        const auto tname = detail::type_name<T>();
        auto off = detail::upcast_offset(class_name_, tname);
        if (!off) return std::unexpected(off.error());
        auto* adjusted = static_cast<char*>(ptr_) + *off;
        return std::shared_ptr<T>(owner_,
                                  static_cast<T*>(static_cast<void*>(adjusted)));
    }

    // Non-owning cast — returns a raw T* for both owned and non-owning
    // Objects.  The caller must ensure the object stays alive.
    // Returns Error::Ambiguous for diamond-ambiguous upcasts (same as
    // cast_safe).
    template <typename T>
    std::expected<T*, Error> cast_ref() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto tname = detail::type_name<T>();
        auto off = detail::upcast_offset(class_name_, tname);
        if (!off) return std::unexpected(off.error());
        auto* adjusted = static_cast<char*>(ptr_) + *off;
        return static_cast<T*>(static_cast<void*>(adjusted));
    }

    std::expected<Object, Error> clone() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!is_owned()) return std::unexpected(Error::NotOwned);
        std::lock_guard<std::mutex> lk(pool_mutex());
        auto it = class_pool().find(std::string(class_name_));
        if (it == class_pool().end() || !it->second->clone)
            return std::unexpected(Error::NotCopyable);
        auto copied = it->second->clone(ptr_);
        return Object(std::move(copied), class_name_);
    }

    std::string to_string() const {
        if (!valid()) return "Object(invalid)";
        return "Object(" + std::string(class_name_) + " @ " +
               std::to_string(reinterpret_cast<std::uintptr_t>(ptr_)) + ")";
    }

    bool valid() const { return ptr_ != nullptr; }
    explicit operator bool() const { return valid(); }

    // Raw pointer to the object — for passing to invokers/getters/setters.
    void* raw() const { return ptr_; }
    const std::shared_ptr<void>& owner() const { return owner_; }

private:
    std::shared_ptr<void> owner_;
    void* ptr_ = nullptr;
    // Borrows static storage: always type_name<T>() (consteval).
    std::string_view class_name_;

    friend class Function;
    friend class Field;
};

// ---------------------------------------------------------------------------
// detail templates that need Object to be complete.
// Defined here (after Object) but still in namespace detail.
// ---------------------------------------------------------------------------

namespace detail {

// Build an Object from argument I.  Non-const lvalue args borrow the
// caller's variable directly (so functions with T& out-params write through
// to the caller); rvalue and const-lvalue args borrow the tuple copy (rvalues
// need stable storage; const lvalues can't be borrowed mutably by design).
//
// lvalue-ness is determined from the Args pack (not from std::get on the
// forward_as_tuple result): std::get on tuple<T&&> returns T& due to reference
// collapsing, which would misclassify rvalues as lvalues.
template <std::size_t I, typename Tuple, typename ArgType, typename ArgRef>
Object make_arg_ref(Tuple& storage, ArgRef&& arg) {
    if constexpr (std::is_lvalue_reference_v<ArgType> &&
                 !std::is_const_v<std::remove_reference_t<ArgType>>) {
        return Object(arg);                  // non-owning borrow of caller's lvalue
    } else {
        return Object(std::get<I>(storage)); // borrow the tuple element
    }
}

template <typename... Args>
std::expected<const Object*, Error>
prepare_args(std::optional<std::tuple<std::decay_t<Args>...>>& storage,
             std::array<Object, sizeof...(Args)>& refs,
             std::size_t param_count, Args&&... args) {
    if (sizeof...(Args) != param_count)
        return std::unexpected(Error::ArityMismatch);
    if constexpr (sizeof...(Args) == 0) {
        return nullptr;
    } else {
        storage.emplace(std::forward<Args>(args)...);
        auto fwd = std::forward_as_tuple(args...);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            using Tuple = std::tuple<std::decay_t<Args>...>;
            using ArgTuple = std::tuple<Args...>;
            ((refs[I] = make_arg_ref<I, Tuple, std::tuple_element_t<I, ArgTuple>>(
                           *storage, std::get<I>(fwd))), ...);
        }(std::make_index_sequence<sizeof...(Args)>{});
        return refs.data();
    }
}

// Unbox one Object argument against its compile-time parameter type P.
// Lvalue-reference params return a reference (writes propagate for T& out-params
// when the arg was borrowed directly from the caller).  Rvalue-ref params move.
// By-value params copy if the type is copy-constructible (matching C++: an lvalue
// passed to a by-value parameter is copied, not moved); move-only by-value params
// still move.  Throws bad_cast on a type-name mismatch — caught by checked_call
// and mapped to TypeError.  P is the spliced parameter type (e.g.
// [:std::meta::type_of(params[J]):]);  J is the argument index, passed at runtime.
//
// If the argument's class doesn't exactly match P, checks whether it's a
// derived class of P and adjusts the pointer to the base subobject — the
// same upcast that invoke does for the target object.
template <typename P>
decltype(auto) extract_arg(const Object* args, std::size_t J) {
    using PBare = std::remove_cvref_t<P>;
    constexpr auto param_type = type_name<PBare>();
    void* src = args[J].raw();
    if (args[J].class_name() != param_type) {
        auto off = is_base_of_with_offset(args[J].class_name(), param_type);
        if (!off) throw std::bad_cast{};
        src = static_cast<char*>(src) + *off;
    }
    if constexpr (std::is_lvalue_reference_v<P>)
        return static_cast<std::remove_reference_t<P>&>(*static_cast<PBare*>(src));
    else if constexpr (std::is_rvalue_reference_v<P>)
        return std::move(*static_cast<PBare*>(src));
    else if constexpr (std::is_copy_constructible_v<PBare>)
        return PBare(*static_cast<PBare*>(src));
    else
        return std::move(*static_cast<PBare*>(src));
}

template <typename T, std::meta::info Ctor>
Object factory(const Object* args) {
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Ctor));
    constexpr std::size_t n = params.size();

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) -> decltype(auto) {
        using P = [:std::meta::type_of(params[J]):];
        return extract_arg<P>(args, J);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> Object {
        return Object(std::make_shared<T>(extract(std::integral_constant<std::size_t, I>{})...),
                      type_name<T>());
    }(std::make_index_sequence<n>{});
}

template <typename T, std::meta::info Fn>
Object invoker(const std::shared_ptr<void>& owner, void* obj,
               const Object* args) {
    auto* target = static_cast<T*>(obj);
    auto mfn = &[:Fn:];
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Fn));
    constexpr std::size_t n = params.size();
    using R = [:std::meta::return_type_of(Fn):];
    using RStore = std::remove_cvref_t<R>;

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) -> decltype(auto) {
        using P = [:std::meta::type_of(params[J]):];
        return extract_arg<P>(args, J);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> Object {
        if constexpr (std::is_void_v<R>) {
            (target->*mfn)(extract(std::integral_constant<std::size_t, I>{})...);
            return Object{};
        } else if constexpr (std::is_reference_v<R>) {
            auto& ref = (target->*mfn)(extract(std::integral_constant<std::size_t, I>{})...);
            return Object(owner, std::addressof(ref), type_name<RStore>());
        } else {
            return Object(std::make_shared<RStore>(
                (target->*mfn)(extract(std::integral_constant<std::size_t, I>{})...)),
                type_name<RStore>());
        }
    }(std::make_index_sequence<n>{});
}

template <typename T, std::meta::info Member>
Object getter(void* obj) {
    auto* target = static_cast<T*>(obj);
    auto ptr = &[:Member:];
    using MemberType = [:std::meta::type_of(Member):];
    using StorageType = std::remove_const_t<std::remove_reference_t<MemberType>>;
    return Object(std::make_shared<StorageType>(target->*ptr),
                 type_name<StorageType>());
}

template <typename T, std::meta::info Member>
void setter(void* obj, const Object* val) {
    auto* target = static_cast<T*>(obj);
    auto ptr = &[:Member:];
    using MemberType = [:std::meta::type_of(Member):];
    using MemberBare = std::remove_cvref_t<MemberType>;
    constexpr auto member_type = type_name<MemberBare>();
    void* src = val->raw();
    if (val->class_name() != member_type) {
        auto off = is_base_of_with_offset(val->class_name(), member_type);
        if (!off) throw std::bad_cast{};
        src = static_cast<char*>(src) + *off;
    }
    target->*ptr = std::move(*static_cast<MemberBare*>(src));
}

template <std::meta::info Member>
Object static_getter() {
    using MemberType = [:std::meta::type_of(Member):];
    using StorageType = std::remove_const_t<std::remove_reference_t<MemberType>>;
    if constexpr (std::is_const_v<MemberType>) {
        // const static member: read the compile-time constant value rather
        // than odr-using the storage.  A const static with only an in-class
        // initializer (e.g. `static const int x = 100;`) is a declaration,
        // not a definition — taking its address (as `&[:Member:]` does)
        // is an odr-use that can fail to link without an out-of-line
        // definition.  constant_of yields a reflection of the constant
        // initializer; splicing it produces the value with no address taken.
        // get_ref still returns ReadOnly for these (no addressable storage);
        // get() copies the value out.
        return Object(std::make_shared<StorageType>(
            [:std::meta::constant_of(Member):]), type_name<StorageType>());
    } else {
        auto* ptr = &[:Member:];
        return Object(std::make_shared<StorageType>(*ptr),
                     type_name<StorageType>());
    }
}

template <std::meta::info Member>
void static_setter(const Object* val) {
    auto* ptr = &[:Member:];
    using MemberType = [:std::meta::type_of(Member):];
    using MemberBare = std::remove_cvref_t<MemberType>;
    constexpr auto member_type = type_name<MemberBare>();
    void* src = val->raw();
    if (val->class_name() != member_type) {
        auto off = is_base_of_with_offset(val->class_name(), member_type);
        if (!off) throw std::bad_cast{};
        src = static_cast<char*>(src) + *off;
    }
    *ptr = std::move(*static_cast<MemberBare*>(src));
}

template <typename T>
std::shared_ptr<void> clone(void* obj) {
    auto* src = static_cast<T*>(obj);
    return std::make_shared<T>(*src);
}

template <std::meta::info Fn>
Object static_invoker(const Object* args) {
    auto fn = &[:Fn:];
    static constexpr auto params = std::define_static_array(
        std::meta::parameters_of(Fn));
    constexpr std::size_t n = params.size();
    using R = [:std::meta::return_type_of(Fn):];
    using RStore = std::remove_cvref_t<R>;

    auto extract = [&]<std::size_t J>(std::integral_constant<std::size_t, J>) -> decltype(auto) {
        using P = [:std::meta::type_of(params[J]):];
        return extract_arg<P>(args, J);
    };

    return [&]<std::size_t... I>(std::index_sequence<I...>) -> Object {
        if constexpr (std::is_void_v<R>) {
            fn(extract(std::integral_constant<std::size_t, I>{})...);
            return Object{};
        } else if constexpr (std::is_reference_v<R>) {
            auto& ref = fn(extract(std::integral_constant<std::size_t, I>{})...);
            return Object(std::addressof(ref), type_name<RStore>());
        } else {
            return Object(std::make_shared<RStore>(
                fn(extract(std::integral_constant<std::size_t, I>{})...)),
                type_name<RStore>());
        }
    }(std::make_index_sequence<n>{});
}

template <typename Fn, typename... CallArgs>
auto checked_call(Fn&& fn, CallArgs&&... args)
    -> std::expected<decltype(fn(std::forward<CallArgs>(args)...)), Error> {
    try {
        if constexpr (std::is_void_v<decltype(fn(std::forward<CallArgs>(args)...))>) {
            fn(std::forward<CallArgs>(args)...);
            return {};
        } else {
            return fn(std::forward<CallArgs>(args)...);
        }
    } catch (const std::bad_cast&) {
        return std::unexpected(Error::TypeError);
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// make_info — gather all reflection metadata for T at compile time.
// ---------------------------------------------------------------------------

template <typename T>
ClassInfo RegistrarHolder<T>::make_info() {
    ClassInfo info;
    info.name = std::string(std::meta::display_string_of(^^T));

    // Base classes — store name + byte offset within T.
    // Only public bases are stored, so the base-hierarchy helpers (used by
    // find_function, find_field, is_class, cast_safe, etc.) naturally skip
    // protected and private inheritance.
    static constexpr auto bases = std::define_static_array(
        std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto b : bases) {
        if constexpr (std::meta::is_public(b)) {
            info.bases.push_back({
                std::string(std::meta::display_string_of(std::meta::type_of(b))),
                std::meta::offset_of(b).bytes,
            });
        }
    }

    // Data members — generate getter/setter for each.  Store the byte
    // offset for get_ref (works for all members, including move-only).
    // Only public members are reflected, matching the public-base filter
    // on bases_of — the framework reflects the public interface.
    static constexpr auto data_members = std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : data_members) {
        // Skip bit-fields and non-public members.
        if constexpr (detail::is_public_data_member(m)) {
            FieldInfo fi;
            fi.name = std::string(std::meta::identifier_of(m));
            fi.type = std::string(
                std::meta::display_string_of(std::meta::type_of(m)));
            fi.offset = std::meta::offset_of(m).bytes;

            using MemberType = [:std::meta::type_of(m):];
            using MemberBare = std::remove_cvref_t<MemberType>;

            fi.is_const = std::is_const_v<MemberType>;

            // Getter: only for copy-constructible members (getter copies
            // into a shared_ptr).  Move-only members are get_ref-only.
            if constexpr (std::is_copy_constructible_v<MemberBare>) {
                fi.getter = &detail::getter<T, m>;
            } else {
                fi.getter = nullptr;
            }

            // Setter: skip const members and non-move-assignable members.
            // The setter move-assigns, so copy-constructibility is not
            // required — unique_ptr and other move-only types are settable.
            if constexpr (std::is_const_v<MemberType> ||
                          !std::is_move_assignable_v<MemberBare>) {
                fi.setter = nullptr;
            } else {
                fi.setter = &detail::setter<T, m>;
            }

            info.fields.push_back(std::move(fi));
        }
    }

    // Static data members — generate static getter/setter for each.
    // Only public members are reflected (same policy as non-static above).
    static constexpr auto static_data = std::define_static_array(
        std::meta::static_data_members_of(^^T, std::meta::access_context::unchecked()));
    template for (constexpr auto m : static_data) {
        if constexpr (std::meta::is_public(m)) {
            StaticFieldInfo fi;
            fi.name = std::string(std::meta::identifier_of(m));
            fi.type = std::string(
                std::meta::display_string_of(std::meta::type_of(m)));

            using MemberType = [:std::meta::type_of(m):];
            using MemberBare = std::remove_cvref_t<MemberType>;

            fi.is_const = std::is_const_v<MemberType>;

            // Capture the address for get_ref.  const static members may
            // lack an out-of-line definition (static const int with an
            // in-class initializer is a declaration, not a definition),
            // so taking their address is an odr-use that can fail to link.
            // Skip them — get_ref returns Error::ReadOnly; get() (copy)
            // still works for reading the value.
            if constexpr (!std::is_const_v<MemberType>) {
                fi.address = const_cast<void*>(
                    static_cast<const void*>(&[:m:]));
            } else {
                fi.address = nullptr;
            }

            if constexpr (std::is_copy_constructible_v<MemberBare>) {
                fi.getter = &detail::static_getter<m>;
            } else {
                fi.getter = nullptr;
            }

            if constexpr (std::is_const_v<MemberType> ||
                          !std::is_move_assignable_v<MemberBare>) {
                fi.setter = nullptr;
            } else {
                fi.setter = &detail::static_setter<m>;
            }

            info.static_fields.push_back(std::move(fi));
        }
    }

    // All members — filter for public constructors and named functions.
    // Non-public members are excluded (same policy as data members above).
    static constexpr auto all_members = std::define_static_array(
        std::meta::members_of(^^T, std::meta::access_context::unchecked()));

    template for (constexpr auto m : all_members) {
        if constexpr (std::meta::is_constructor(m) &&
                      !std::meta::is_deleted(m) &&
                      std::meta::is_public(m) &&
                      !std::is_abstract_v<T>) {
            static constexpr auto params = std::define_static_array(
                std::meta::parameters_of(m));
            constexpr std::size_t n = params.size();

            // Skip copy/move constructors (first param is T or T&/T&&).
            if constexpr (n == 0) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                info.constructors.push_back(std::move(ci));
            } else if constexpr (n == 1) {
                using P0 = [:std::meta::type_of(params[0]):];
                if constexpr (!std::is_same_v<std::remove_cvref_t<P0>, T>) {
                    ConstructorInfo ci;
                    ci.factory = &detail::factory<T, m>;
                    template for (constexpr auto p : params) {
                        ci.param_types.emplace_back(
                            detail::normalize_type(
                            std::meta::display_string_of(std::meta::type_of(p))));
                    }
                    info.constructors.push_back(std::move(ci));
                }
            } else if constexpr (n >= 2) {
                ConstructorInfo ci;
                ci.factory = &detail::factory<T, m>;
                template for (constexpr auto p : params) {
                    ci.param_types.emplace_back(
                            detail::normalize_type(
                            std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.constructors.push_back(std::move(ci));
            }
        } else if constexpr (detail::is_public_method(m) &&
                            (std::meta::has_identifier(m) ||
                             std::meta::is_operator_function(m))) {
            static constexpr auto fparams = std::define_static_array(
                std::meta::parameters_of(m));

            // Name: identifier for normal functions, "operator" + symbol
            // for overloaded operators (including defaulted operator=).
            std::string fname;
            if constexpr (std::meta::has_identifier(m)) {
                fname = std::string(std::meta::identifier_of(m));
            } else {
                constexpr auto op = std::meta::operator_of(m);
                constexpr auto sym = std::meta::symbol_of(op);
                fname = "operator" + std::string(sym);
            }

            if constexpr (std::meta::is_static_member(m)) {
                // Static member function — no obj pointer.
                StaticFunctionInfo fi;
                fi.name = fname;
                fi.return_type = std::string(
                    std::meta::display_string_of(std::meta::return_type_of(m)));
                fi.invoker = &detail::static_invoker<m>;
                template for (constexpr auto p : fparams) {
                    fi.param_types.emplace_back(
                        detail::normalize_type(
                        std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.static_functions.push_back(std::move(fi));
            } else {
                FunctionInfo fi;
                fi.name = fname;
                fi.return_type = std::string(
                    std::meta::display_string_of(std::meta::return_type_of(m)));
                fi.invoker = &detail::invoker<T, m>;
                template for (constexpr auto p : fparams) {
                    fi.param_types.emplace_back(
                        detail::normalize_type(
                        std::meta::display_string_of(std::meta::type_of(p))));
                }
                info.functions.push_back(std::move(fi));
            }
        }
    }

    // Clone function — only if T is copy-constructible.
    if constexpr (std::is_copy_constructible_v<T>) {
        info.clone = &detail::clone<T>;
    }

    return info;
}
// ---------------------------------------------------------------------------

template <typename T>
EnumInfo EnumRegistrarHolder<T>::make_enum_info() {
    EnumInfo info;
    info.name = std::string(std::meta::display_string_of(^^T));

    static constexpr auto enumerators = std::define_static_array(
        std::meta::enumerators_of(^^T));

    template for (constexpr auto e : enumerators) {
        EnumeratorInfo ei;
        ei.name = std::string(std::meta::identifier_of(e));
        constexpr auto val = std::meta::constant_of(e);
        ei.value = static_cast<long long>([:val:]);
        info.enumerators.push_back(std::move(ei));
    }

    return info;
}

// ---------------------------------------------------------------------------
// Runtime handles — Class, Constructor, Function, Field, Enum, Enumerator.
// ---------------------------------------------------------------------------

class Constructor {
public:
    Constructor() = default;
    Constructor(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::vector<std::string>& param_types() const {
        return owner_->constructors[idx_].param_types;
    }

    // Construct via the type-erased factory.  Returns Error::ArityMismatch
    // if the argument count doesn't match the constructor's parameter count,
    // Error::TypeError if an argument's type doesn't match the parameter
    // type, Error::NullHandle if the handle is invalid.
    template <typename... Args>
    std::expected<Object, Error> call(Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto& ci = owner_->constructors[idx_];
        std::optional<std::tuple<std::decay_t<Args>...>> storage;
        std::array<Object, sizeof...(Args)> refs;
        auto args_ptr = detail::prepare_args(storage, refs,
                                             ci.param_types.size(),
                                             std::forward<Args>(args)...);
        if (!args_ptr) return std::unexpected(args_ptr.error());
        return detail::checked_call(ci.factory, *args_ptr);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Function {
public:
    Function() = default;
    Function(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->functions[idx_].name; }
    const std::vector<std::string>& param_types() const {
        return owner_->functions[idx_].param_types;
    }
    const std::string& return_type() const {
        return owner_->functions[idx_].return_type;
    }

    // Invoke on any object — owned Object or stack/concrete instance.
    // Returns Error::TypeError if obj is not the function's class (or a
    // derived class) or if an argument's type doesn't match the parameter
    // type, Error::ArityMismatch if the argument count is wrong,
    // Error::NullHandle if the handle is invalid.
    // The return value is an Object; .valid() is false for void functions,
    // .is_owned() is false for reference returns on non-owning inputs.
    template <typename... Args>
    std::expected<Object, Error> invoke(Object obj, Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        const auto& fi = owner_->functions[idx_];
        std::optional<std::tuple<std::decay_t<Args>...>> storage;
        std::array<Object, sizeof...(Args)> refs;
        auto args_ptr = detail::prepare_args(storage, refs,
                                             fi.param_types.size(),
                                             std::forward<Args>(args)...);
        if (!args_ptr) return std::unexpected(args_ptr.error());
        void* adj = detail::adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_call(fi.invoker, obj.owner(), adj, *args_ptr);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    friend class Class;
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Field {
public:
    Field() = default;
    Field(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->fields[idx_].name; }
    const std::string& type() const { return owner_->fields[idx_].type; }
    bool is_const() const { return owner_->fields[idx_].is_const; }
    bool has_getter() const { return owner_->fields[idx_].getter != nullptr; }
    bool has_setter() const { return owner_->fields[idx_].setter != nullptr; }

    // Get the field value (copy).  Returns Error::TypeError if obj is not
    // the field's class, Error::NullHandle if the handle is invalid,
    // Error::NotCopyable if the field has no getter (move-only member).
    std::expected<Object, Error> get(Object obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        if (!has_getter()) return std::unexpected(Error::NotCopyable);
        void* adj = detail::adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return detail::checked_call(owner_->fields[idx_].getter, adj);
    }

    // Get a non-owning pointer to the field inside the object — works
    // for all non-const members including move-only.  The pointer is valid
    // as long as the object is alive.  The caller casts void* to the
    // member type.  Returns Error::ReadOnly for const members (a writable
    // void* into a const member would let the caller cast away const),
    // Error::TypeError if obj is not the field's class, Error::NullHandle
    // if the handle is invalid.  Use the typed get_ref<T>() for const
    // members — it returns a const T* and so is safe to expose.
    std::expected<void*, Error> get_ref(Object obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        if (is_const()) return std::unexpected(Error::ReadOnly);
        void* adj = detail::adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return static_cast<char*>(adj) + owner_->fields[idx_].offset;
    }

    // Typed get_ref — returns a typed pointer into the field inside the
    // object.  Checks T against the field's stored type name at runtime.
    // For all members including move-only.  The pointer is valid as long
    // as the object is alive.
    // Returns Error::TypeError if T doesn't match the field type or obj
    // is not the field's class, Error::NullHandle if the handle is invalid.
    template <typename T>
    std::expected<T*, Error> get_ref(Object obj) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        if (detail::type_name<T>() != owner_->fields[idx_].type)
            return std::unexpected(Error::TypeError);
        void* adj = detail::adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        return static_cast<T*>(static_cast<void*>(
            static_cast<char*>(adj) + owner_->fields[idx_].offset));
    }

    // Set the field value (move-assigns).  Returns Error::ReadOnly if the
    // field has no setter (const or not move-assignable), Error::TypeError
    // if obj is not the field's class or the value's type doesn't match or
    // derive from the field type, Error::NullHandle if the Field handle is
    // invalid.
    template <typename V>
    std::expected<void, Error> set(Object obj, V&& val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!obj.valid()) return std::unexpected(Error::NullHandle);
        if (!has_setter()) return std::unexpected(Error::ReadOnly);
        void* adj = detail::adjust_to_base(obj.raw(), obj.class_name(), owner_->name);
        if (!adj) return std::unexpected(Error::TypeError);
        std::decay_t<V> storage(std::forward<V>(val));
        Object val_ref(&storage, detail::type_name<std::decay_t<V>>());
        return detail::checked_call(owner_->fields[idx_].setter,
                                    adj, &val_ref);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    friend class Class;
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class StaticField {
public:
    StaticField() = default;
    StaticField(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->static_fields[idx_].name; }
    const std::string& type() const { return owner_->static_fields[idx_].type; }
    bool is_const() const { return owner_->static_fields[idx_].is_const; }
    bool has_getter() const { return owner_->static_fields[idx_].getter != nullptr; }
    bool has_setter() const { return owner_->static_fields[idx_].setter != nullptr; }

    // Get the static field value (copy).  Returns Error::NullHandle if
    // the handle is invalid, Error::NotCopyable if the field has no
    // getter (move-only static member).
    std::expected<Object, Error> get() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!has_getter()) return std::unexpected(Error::NotCopyable);
        return detail::checked_call(owner_->static_fields[idx_].getter);
    }

    // Get a non-owning pointer to the static storage — works for all
    // non-const members including move-only.  The pointer is valid for the
    // program's lifetime (static storage).  The caller casts void* to the
    // member type.  Returns Error::ReadOnly for const members (which may
    // lack addressable storage), Error::NullHandle if the handle is invalid.
    std::expected<void*, Error> get_ref() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        auto* addr = owner_->static_fields[idx_].address;
        if (!addr) return std::unexpected(Error::ReadOnly);
        return addr;
    }

    // Typed get_ref — returns a typed pointer to the static storage.
    // Checks T against the field's stored type name at runtime.  For all
    // non-const members including move-only.  Returns Error::TypeError if
    // T doesn't match the field type, Error::ReadOnly for const members,
    // Error::NullHandle if the handle is invalid.
    template <typename T>
    std::expected<T*, Error> get_ref() const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (detail::type_name<T>() != owner_->static_fields[idx_].type)
            return std::unexpected(Error::TypeError);
        auto* addr = owner_->static_fields[idx_].address;
        if (!addr) return std::unexpected(Error::ReadOnly);
        return static_cast<T*>(addr);
    }

    // Set the static field value (move-assigns).  Returns Error::ReadOnly if
    // the field has no setter (const or not move-assignable), Error::TypeError
    // if the value's type doesn't match or derive from the field type,
    // Error::NullHandle if the handle is invalid.
    template <typename V>
    std::expected<void, Error> set(V&& val) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        if (!has_setter()) return std::unexpected(Error::ReadOnly);
        std::decay_t<V> storage(std::forward<V>(val));
        Object val_ref(&storage, detail::type_name<std::decay_t<V>>());
        return detail::checked_call(owner_->static_fields[idx_].setter,
                                    &val_ref);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    friend class Class;
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class StaticFunction {
public:
    StaticFunction() = default;
    StaticFunction(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->static_functions[idx_].name; }
    const std::vector<std::string>& param_types() const {
        return owner_->static_functions[idx_].param_types;
    }
    const std::string& return_type() const {
        return owner_->static_functions[idx_].return_type;
    }

    // Invoke the static function.  Returns Error::ArityMismatch if the
    // argument count is wrong, Error::TypeError if an argument's type
    // doesn't match the parameter type, Error::NullHandle if the handle
    // is invalid.  The return value is an Object; .valid() is false for
    // void functions.
    template <typename... Args>
    std::expected<Object, Error> invoke(Args&&... args) const {
        if (!valid()) return std::unexpected(Error::NullHandle);
        const auto& sf = owner_->static_functions[idx_];
        std::optional<std::tuple<std::decay_t<Args>...>> storage;
        std::array<Object, sizeof...(Args)> refs;
        auto args_ptr = detail::prepare_args(storage, refs,
                                             sf.param_types.size(),
                                             std::forward<Args>(args)...);
        if (!args_ptr) return std::unexpected(args_ptr.error());
        return detail::checked_call(sf.invoker, *args_ptr);
    }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    friend class Class;
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Enumerator {
public:
    Enumerator() = default;
    Enumerator(const EnumInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->enumerators[idx_].name; }
    long long value() const { return owner_->enumerators[idx_].value; }

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const EnumInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Enum {
public:
    Enum() = default;
    explicit Enum(const EnumInfo* info) : info_(info) {}

    const std::string& name() const {
        static const std::string empty;
        return info_ ? info_->name : empty;
    }

    std::vector<Enumerator> enumerators() const {
        std::vector<Enumerator> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->enumerators.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::expected<Enumerator, Error> find_enumerator(std::string_view name) const;
    std::expected<Enumerator, Error> find_enumerator(long long value) const;

    bool valid() const { return info_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    const EnumInfo* info_ = nullptr;
};

class Base {
public:
    Base() = default;
    Base(const ClassInfo* owner, std::size_t idx)
        : owner_(owner), idx_(idx) {}

    const std::string& name() const { return owner_->bases[idx_].name; }
    std::ptrdiff_t offset() const { return owner_->bases[idx_].offset; }

    // The Class handle for this base (invalid if the base class is not
    // registered — e.g. it was never Reg<T>'d).
    Class as_class() const;

    bool valid() const { return owner_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    friend class Class;
    const ClassInfo* owner_ = nullptr;
    std::size_t idx_ = 0;
};

class Class {
public:
    Class() = default;
    explicit Class(const ClassInfo* info) : info_(info) {}

    const std::string& name() const {
        static const std::string empty;
        return info_ ? info_->name : empty;
    }

    std::vector<Base> bases() const {
        std::vector<Base> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->bases.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    // All base classes across the full hierarchy (transitive), deduplicated
    // by name so a diamond-shared base appears once.  Unlike bases(), which
    // returns only the direct bases, this walks the whole inheritance graph.
    std::vector<Base> all_bases() const;

    std::vector<Field> fields() const {
        std::vector<Field> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->fields.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::vector<StaticField> static_fields() const {
        std::vector<StaticField> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->static_fields.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::vector<StaticFunction> static_functions() const {
        std::vector<StaticFunction> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->static_functions.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::vector<Constructor> constructors() const {
        std::vector<Constructor> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->constructors.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::vector<Function> functions() const {
        std::vector<Function> result;
        if (!valid()) return result;
        for (std::size_t i = 0; i < info_->functions.size(); ++i)
            result.emplace_back(info_, i);
        return result;
    }

    std::expected<Constructor, Error> find_constructor(
        std::initializer_list<std::string_view> types) const;

    // Find a function by name.  If multiple overloads exist, returns the
    // first match.  Walks base classes if not found in this class.
    // Returns Error::Ambiguous if the name resolves to 2+ base subobjects.
    std::expected<Function, Error> find_function(std::string_view name) const;

    // Find a function by name and parameter type names.  This disambiguates
    // overloads.  Matching uses the same normalization as find_constructor
    // (ignores const, ref, whitespace).  Walks base classes.
    // Returns Error::Ambiguous if the name is ambiguous before overload
    // resolution (C++ resolves name-lookup ambiguity first).
    std::expected<Function, Error> find_function(
        std::string_view name,
        std::initializer_list<std::string_view> types) const;

    // Find all overloads of a function by name.  Walks base classes.
    std::vector<Function> find_functions(std::string_view name) const;

    // All functions across the full hierarchy (this class + bases).
    // Returns Function handles by value (merged view), unlike functions()
    // which returns only this class's own members.
    std::vector<Function> all_functions() const;

    std::expected<Field, Error> find_field(std::string_view name) const;

    // All fields across the full hierarchy (this class + bases).
    // Returns Field handles by value (merged view), unlike fields()
    // which returns only this class's own members.
    std::vector<Field> all_fields() const;

    // Find a static data member by name.  Walks base classes.
    std::expected<StaticField, Error> find_static_field(std::string_view name) const;

    // All static fields across the full hierarchy (this class + bases).
    // Returns StaticField handles by value (merged view), unlike
    // static_fields() which returns only this class's own members.
    std::vector<StaticField> all_static_fields() const;

    // Find a static member function by name.  Walks base classes.
    std::expected<StaticFunction, Error> find_static_function(std::string_view name) const;

    // Find a static member function by name and parameter type names.
    // Disambiguates overloads.  Walks base classes.
    std::expected<StaticFunction, Error> find_static_function(
        std::string_view name,
        std::initializer_list<std::string_view> types) const;

    // Find all overloads of a static function by name.  Walks base classes.
    std::vector<StaticFunction> find_static_functions(std::string_view name) const;

    // All static functions across the full hierarchy (this class + bases).
    // Returns StaticFunction handles by value (merged view).
    std::vector<StaticFunction> all_static_functions() const;

    bool valid() const { return info_ != nullptr; }
    explicit operator bool() const { return valid(); }

private:
    // Remove duplicate handles produced by diamond inheritance — the same
    // (ClassInfo*, idx) pair can appear via two base paths.  Order-preserving.
    template <typename Handle>
    static void dedup_handles(std::vector<Handle>& v) {
        std::set<std::pair<const ClassInfo*, std::size_t>> seen;
        v.erase(std::remove_if(v.begin(), v.end(),
            [&seen](const Handle& h) {
                return !seen.insert({h.owner_, h.idx_}).second;
            }), v.end());
    }

    // Drop handles whose declaring class is reachable from `derived` via
    // more than one base-subobject path (a diamond).  An unqualified C++
    // lookup of such a member is ambiguous, so the framework does not
    // return it.  Caller must hold pool_mutex.
    template <typename Handle>
    static void exclude_ambiguous(std::vector<Handle>& v,
                                 std::string_view derived) {
        v.erase(std::remove_if(v.begin(), v.end(),
            [derived](const Handle& h) {
                return detail::base_path_offsets_unlocked(
                    derived, h.owner_->name).size() > 1;
            }), v.end());
    }

    const ClassInfo* info_ = nullptr;
};

// ---------------------------------------------------------------------------
// Free functions — main entry points.
// ---------------------------------------------------------------------------

inline std::expected<Class, Error> find_class(std::string_view name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto it = class_pool().find(std::string(name));
    if (it == class_pool().end())
        return std::unexpected(Error::NotFound);
    return Class(it->second.get());
}

// Enumerate all registered class names.
inline std::vector<std::string> list_all_classes() {
    std::lock_guard<std::mutex> lk(pool_mutex());
    std::vector<std::string> names;
    names.reserve(class_pool().size());
    for (const auto& [name, info] : class_pool())
        names.push_back(name);
    return names;
}

// Find an enum by runtime string name.
inline std::expected<Enum, Error> find_enum(std::string_view name) {
    std::lock_guard<std::mutex> lk(pool_mutex());
    auto it = enum_pool().find(std::string(name));
    if (it == enum_pool().end())
        return std::unexpected(Error::NotFound);
    return Enum(it->second.get());
}

// Enumerate all registered enum names.
inline std::vector<std::string> list_all_enums() {
    std::lock_guard<std::mutex> lk(pool_mutex());
    std::vector<std::string> names;
    names.reserve(enum_pool().size());
    for (const auto& [name, info] : enum_pool())
        names.push_back(name);
    return names;
}

// ---------------------------------------------------------------------------
// Base-hierarchy lookup helpers.
//
// find_named / find_named_sig / find_all_named / collect_all implement the
// shared lookup policy used by every Class::find_<member> and all_<member>:
//
//   * C++ name hiding — if a class declares a name, its own bases' same-name
//     members are not searched (the name hides deeper overloads).
//   * Ambiguity — if a name resolves to two or more base subobjects (a
//     diamond, or the same name in two sibling bases), an unqualified
//     lookup is ambiguous: the singular find_* return Error::Ambiguous and
//     the plural all_*/find_<member>s omit the member entirely, matching
//     C++ (which rejects the unqualified access as ambiguous).
//
// Ambiguity is decided by the number of distinct byte-offset paths from the
// derived class to the declaring class (base_path_offsets_unlocked); >1 is
// a diamond.  Two different sibling bases each declaring the name also yield
// 2+ subobjects and are reported as ambiguous for singular lookups, but
// each declaration (uniquely reachable) is still listed by the plural views.
// ---------------------------------------------------------------------------
namespace detail {

// Indices in `v` whose `.name == name`.
template <typename Info>
std::vector<std::size_t> own_indices(const std::vector<Info>& v,
                                     std::string_view name) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i].name == name) out.push_back(i);
    return out;
}

// Set of names defined directly on this class — used for name hiding.
template <typename Member>
std::set<std::string> own_names(const std::vector<Member>& members) {
    std::set<std::string> names;
    for (const auto& m : members)
        names.insert(m.name);
    return names;
}

// Recursively collect (declaring ClassInfo*, idx) for members named `name`
// in C's base hierarchy, respecting name hiding: a base that declares `name`
// hides the same name in its own bases (we don't descend into them);
// sibling bases are all searched (their declarations may be ambiguous).
// Caller must hold pool_mutex.
template <typename Info>
void collect_base_named(const ClassInfo* C, std::string_view name,
    const std::vector<Info> ClassInfo::* vec,
    std::vector<std::pair<const ClassInfo*, std::size_t>>& out) {
    for (const auto& b : C->bases) {
        auto it = class_pool().find(b.name);
        if (it == class_pool().end()) continue;
        const ClassInfo* bc = it->second.get();
        auto idxs = own_indices(bc->*vec, name);
        if (!idxs.empty()) {
            for (auto i : idxs) out.emplace_back(bc, i);
        } else {
            collect_base_named(bc, name, vec, out);
        }
    }
}

// Dedup (ClassInfo*, idx) pairs — a diamond reaches the same declaration
// via two paths.  Order-preserving.
inline void dedup_decl(
    std::vector<std::pair<const ClassInfo*, std::size_t>>& v) {
    std::set<std::pair<const ClassInfo*, std::size_t>> seen;
    v.erase(std::remove_if(v.begin(), v.end(),
        [&seen](const auto& p) { return !seen.insert(p).second; }), v.end());
}

// Number of distinct base subobjects that offer `name` in C's hierarchy.
// Multiple overloads in the same base share one subobject, so they count
// once.  >1 means an unqualified lookup is ambiguous (diamond or siblings).
// Caller must hold pool_mutex.
inline std::size_t subobject_count(
    const ClassInfo* C,
    const std::vector<std::pair<const ClassInfo*, std::size_t>>& decls) {
    std::set<std::string> classes;
    for (const auto& [ci, idx] : decls)
        classes.insert(ci->name);
    std::size_t total = 0;
    for (const auto& cn : classes)
        total += base_path_offsets_unlocked(C->name, cn).size();
    return total;
}

// Singular lookup by name.  Own members hide bases.  Among base hits, the
// number of distinct base subobjects offering the name is checked: >=2 →
// Ambiguous; otherwise return the first declaration (first overload).
template <typename Handle, typename Info>
std::expected<Handle, Error> find_named(const ClassInfo* C,
    std::string_view name,
    const std::vector<Info> ClassInfo::* vec) {
    // Own members are read without the lock — ClassInfo is immutable after
    // registration.  The base walk dereferences the pool, so it locks.
    for (std::size_t i = 0; i < (C->*vec).size(); ++i)
        if ((C->*vec)[i].name == name)
            return Handle(C, i);  // own hides bases

    std::vector<std::pair<const ClassInfo*, std::size_t>> decls;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        collect_base_named(C, name, vec, decls);
        dedup_decl(decls);
        if (subobject_count(C, decls) >= 2) return std::unexpected(Error::Ambiguous);
        if (!decls.empty()) return Handle(decls[0].first, decls[0].second);
    }
    return std::unexpected(Error::NotFound);
}

// Singular lookup by name + parameter-type signature.  Own members hide
// bases (if any own member has the name, base overloads are hidden even
// when none matches the signature).  Ambiguity is decided over the whole
// name-lookup set (before overload resolution), matching C++.  Among base
// hits, the first signature match is returned.
template <typename Handle, typename Info>
std::expected<Handle, Error> find_named_sig(const ClassInfo* C,
    std::string_view name,
    std::initializer_list<std::string_view> types,
    const std::vector<Info> ClassInfo::* vec) {
    std::vector<std::string> query;
    for (auto t : types) query.push_back(normalize_type(t));

    bool name_exists = false;
    for (std::size_t i = 0; i < (C->*vec).size(); ++i) {
        if ((C->*vec)[i].name == name) {
            name_exists = true;
            if (match_signature((C->*vec)[i].param_types, query))
                return Handle(C, i);
        }
    }
    if (name_exists) return std::unexpected(Error::NotFound);

    std::vector<std::pair<const ClassInfo*, std::size_t>> decls;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        collect_base_named(C, name, vec, decls);
        dedup_decl(decls);
        if (subobject_count(C, decls) >= 2) return std::unexpected(Error::Ambiguous);
        for (const auto& [ci, idx] : decls)
            if (match_signature((ci->*vec)[idx].param_types, query))
                return Handle(ci, idx);
    }
    return std::unexpected(Error::NotFound);
}

// Plural lookup by name: all overloads of `name` across the hierarchy.
// Own members hide bases.  Diamond-ambiguous declarations (path count >1)
// are omitted.  Returns handles by value.
template <typename Handle, typename Info>
std::vector<Handle> find_all_named(const ClassInfo* C,
    std::string_view name,
    const std::vector<Info> ClassInfo::* vec) {
    std::vector<Handle> results;
    for (std::size_t i = 0; i < (C->*vec).size(); ++i)
        if ((C->*vec)[i].name == name)
            results.emplace_back(C, i);
    if (!results.empty()) return results;  // own hides bases

    std::vector<std::pair<const ClassInfo*, std::size_t>> decls;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        collect_base_named(C, name, vec, decls);
        dedup_decl(decls);
        std::erase_if(decls, [&](const auto& p) {
            return base_path_offsets_unlocked(C->name, p.first->name).size() > 1;
        });
    }
    for (const auto& [ci, idx] : decls)
        results.emplace_back(ci, idx);
    return results;
}

// Collect every member across the hierarchy (all names), respecting name
// hiding (a class's own names hide same-name members in its bases).
// Caller must hold pool_mutex.
template <typename Handle, typename Info>
void collect_all_unlocked(const ClassInfo* C,
    const std::vector<Info> ClassInfo::* vec,
    std::vector<Handle>& results) {
    auto hidden = own_names(C->*vec);
    for (std::size_t i = 0; i < (C->*vec).size(); ++i)
        results.emplace_back(C, i);
    for (const auto& b : C->bases) {
        auto it = class_pool().find(b.name);
        if (it == class_pool().end()) continue;
        std::vector<Handle> more;
        collect_all_unlocked<Handle, Info>(it->second.get(), vec, more);
        for (auto& h : more)
            if (!hidden.count(h.name()))
                results.push_back(std::move(h));
    }
}
}  // namespace detail

inline std::expected<Constructor, Error> Class::find_constructor(
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    // Constructors are not inherited — only this class's own constructors
    // are searched (unlike find_function/find_field, which walk bases).
    std::vector<std::string> query;
    for (auto t : types)
        query.push_back(detail::normalize_type(t));

    for (std::size_t i = 0; i < info_->constructors.size(); ++i) {
        if (detail::match_signature(info_->constructors[i].param_types, query))
            return Constructor(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named<Function, FunctionInfo>(
        info_, name, &ClassInfo::functions);
}

inline std::expected<Field, Error> Class::find_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named<Field, FieldInfo>(
        info_, name, &ClassInfo::fields);
}

inline std::vector<Field> Class::all_fields() const {
    std::vector<Field> results;
    if (!valid()) return results;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        detail::collect_all_unlocked<Field, FieldInfo>(
            info_, &ClassInfo::fields, results);
        dedup_handles(results);
        exclude_ambiguous(results, info_->name);
    }
    return results;
}

inline std::expected<Function, Error> Class::find_function(
    std::string_view name,
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named_sig<Function, FunctionInfo>(
        info_, name, types, &ClassInfo::functions);
}

inline std::vector<Function> Class::find_functions(
    std::string_view name) const {
    if (!valid()) return {};
    return detail::find_all_named<Function, FunctionInfo>(
        info_, name, &ClassInfo::functions);
}

inline std::vector<Function> Class::all_functions() const {
    std::vector<Function> results;
    if (!valid()) return results;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        detail::collect_all_unlocked<Function, FunctionInfo>(
            info_, &ClassInfo::functions, results);
        dedup_handles(results);
        exclude_ambiguous(results, info_->name);
    }
    return results;
}

inline std::expected<Enumerator, Error> Enum::find_enumerator(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->enumerators.size(); ++i) {
        if (info_->enumerators[i].name == name)
            return Enumerator(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<Enumerator, Error> Enum::find_enumerator(
    long long value) const {
    if (!valid()) return std::unexpected(Error::NullHandle);

    for (std::size_t i = 0; i < info_->enumerators.size(); ++i) {
        if (info_->enumerators[i].value == value)
            return Enumerator(info_, i);
    }
    return std::unexpected(Error::NotFound);
}

inline std::expected<StaticField, Error> Class::find_static_field(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named<StaticField, StaticFieldInfo>(
        info_, name, &ClassInfo::static_fields);
}

inline std::vector<StaticField> Class::all_static_fields() const {
    std::vector<StaticField> results;
    if (!valid()) return results;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        detail::collect_all_unlocked<StaticField, StaticFieldInfo>(
            info_, &ClassInfo::static_fields, results);
        dedup_handles(results);
        exclude_ambiguous(results, info_->name);
    }
    return results;
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named<StaticFunction, StaticFunctionInfo>(
        info_, name, &ClassInfo::static_functions);
}

inline std::expected<StaticFunction, Error> Class::find_static_function(
    std::string_view name,
    std::initializer_list<std::string_view> types) const {
    if (!valid()) return std::unexpected(Error::NullHandle);
    return detail::find_named_sig<StaticFunction, StaticFunctionInfo>(
        info_, name, types, &ClassInfo::static_functions);
}

inline std::vector<StaticFunction> Class::find_static_functions(
    std::string_view name) const {
    if (!valid()) return {};
    return detail::find_all_named<StaticFunction, StaticFunctionInfo>(
        info_, name, &ClassInfo::static_functions);
}

inline std::vector<StaticFunction> Class::all_static_functions() const {
    std::vector<StaticFunction> results;
    if (!valid()) return results;
    {
        std::lock_guard<std::mutex> lk(pool_mutex());
        detail::collect_all_unlocked<StaticFunction, StaticFunctionInfo>(
            info_, &ClassInfo::static_functions, results);
        dedup_handles(results);
        exclude_ambiguous(results, info_->name);
    }
    return results;
}

inline Class Base::as_class() const {
    if (!valid()) return Class{};
    auto c = find_class(owner_->bases[idx_].name);
    return c.value_or(Class{});
}

// All base classes across the full inheritance graph (transitive),
// deduplicated by name so a diamond-shared base appears once.
// Order: direct bases first, then their bases (depth-first).
inline std::vector<Base> Class::all_bases() const {
    std::vector<Base> results;
    if (!valid()) return results;
    std::set<std::string> seen;
    // Recursively append bases, deduping by name.  Uses (owner_, idx) where
    // owner_ is the *derived* class that lists this base — so each Base
    // handle reports the correct per-derivation offset.
    auto walk = [&](auto& self, const ClassInfo* C) -> void {
        for (std::size_t i = 0; i < C->bases.size(); ++i) {
            const auto& b = C->bases[i];
            if (seen.insert(b.name).second) {
                results.emplace_back(C, i);
                auto it = class_pool().find(b.name);
                if (it != class_pool().end())
                    self(self, it->second.get());
            }
        }
    };
    std::lock_guard<std::mutex> lk(pool_mutex());
    walk(walk, info_);
    return results;
}

}  // namespace refl
