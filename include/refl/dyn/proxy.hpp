// Typed views share structural plans and invoke through common call frames.
#pragma once

#include <refl/refl.hpp>
#include <refl/reflect/schema.hpp>

#include <map>
#include <stdexcept>

namespace refl {
namespace detail {
inline CallTarget require_target(Result<CallTarget> result) {
    if (!result) throw ReflectionError(result.error());
    return std::move(*result);
}
struct BindingPlan {
    std::shared_ptr<const ClassInfo> descriptor;
    std::map<std::string, std::vector<MemberLocation>> methods;
    std::map<std::string, MemberLocation> properties;
};
struct BindingState {
    Object object;
    std::shared_ptr<const BindingPlan> plan;
};

template<class R, class... A> R call_bound(const CallTarget& target, bool readonly, A&&... args) {
    if (!target.valid()) throw ReflectionError({DiagnosticCode::null_handle});
    std::array<ArgumentView, sizeof...(A)> arguments{native_argument(std::forward<A>(args))...};
    project_legacy_arguments(arguments, target.signature());
    auto receiver = target.options().receiver;
    CallFrame frame{readonly ? receiver.as_const() : receiver, arguments};
    auto result = invoke_target(target, frame, {},
        std::is_reference_v<R> ? ExportKind::raw_reference : ExportKind::erased);
    if (!result) throw ReflectionError(result.error());
    if constexpr (!std::is_void_v<R>) {
        auto pointer = result_view(*result).template get<std::remove_reference_t<R>>();
        if (!pointer) throw ReflectionError(pointer.error());
        if constexpr (std::is_reference_v<R>) return **pointer;
        else return R(std::move(**pointer));
    }
}
consteval std::meta::info make_fn_sig(std::meta::info member) {
    return std::meta::type_of(member);
}
}

template<class Sig> struct sig_traits : detail::signature_traits<Sig> {
    using return_type = typename detail::signature_traits<Sig>::result;
};
template<class Sig, class... A> concept matches_sig = []<std::size_t... I>(std::index_sequence<I...>) {
    using Args = typename sig_traits<Sig>::arguments;
    return std::is_same_v<std::tuple<std::remove_cvref_t<std::tuple_element_t<I, Args>>...>,
                          std::tuple<std::remove_cvref_t<A>...>>;
}(std::make_index_sequence<std::tuple_size_v<typename sig_traits<Sig>::arguments>>{});

template<class... Sigs> struct TypedMethod {
    std::vector<CallTarget> targets;
    DispatchHandle source;
    std::vector<MemberId> members;
    static std::vector<Signature> signatures() { return {signature_of<Sigs>()...}; }
    void bind(const detail::BindingState& state, const std::vector<detail::MemberLocation>& entries) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (bind_one<std::tuple_element_t<I, std::tuple<Sigs...>>>(state.object, entries[I]), ...);
        }(std::index_sequence_for<Sigs...>{});
    }
    template<class S> void bind_one(const Object& object, const detail::MemberLocation& entry) {
        const auto& function = entry.first->functions[entry.second];
        auto native = detail::operation(*entry.first, function.member, OperationKind::method);
        if (native) {
            auto receiver = detail::project_legacy_view(object.view(), *entry.first);
            if (!receiver) throw ReflectionError(receiver.error());
            TargetOptions options;
            options.reference_export = ReferenceExport::caller_borrow;
            options.result_lifetime = ResultLifetime::receiver;
            auto target = native->bind(*receiver, options);
            if (!target) throw ReflectionError(target.error());
            targets.push_back(std::move(*target));
        } else throw ReflectionError({DiagnosticCode::unsupported});
    }
    template<class Self, class... A> decltype(auto) operator()(this Self&& self, A&&... args) {
        return self.template call_dispatch<std::is_const_v<std::remove_reference_t<Self>>, 0>(std::forward<A>(args)...);
    }
    template<bool Const, std::size_t I, class... A> decltype(auto) call_dispatch(A&&... args) const {
        using S = std::tuple_element_t<I, std::tuple<Sigs...>>;
        if constexpr (matches_sig<S, A...>) {
            if (source.valid()) {
                auto call = source.resolve(members.at(I));
                if (!call) throw ReflectionError(call.error());
                return detail::call_bound<typename sig_traits<S>::return_type>(call->target, Const, std::forward<A>(args)...);
            }
            if (I >= targets.size()) throw ReflectionError({DiagnosticCode::null_handle});
            return detail::call_bound<typename sig_traits<S>::return_type>(targets[I], Const, std::forward<A>(args)...);
        } else if constexpr (I + 1 < sizeof...(Sigs)) {
            return call_dispatch<Const, I + 1>(std::forward<A>(args)...);
        } else static_assert(false, "no matching overload");
    }
};

template<class T, bool Readonly = false> struct TypedProperty {
    using value_type = T;
    CallTarget read, write, view;
    DispatchHandle read_source, write_source;
    MemberId member = {};
    static constexpr bool is_readonly() { return Readonly; }
    operator T() const {
        if (!read_source.valid()) return detail::call_bound<T>(read, true);
        auto call = read_source.resolve(member);
        if (!call) throw ReflectionError(call.error());
        return detail::call_bound<T>(call->target, true);
    }
    void operator=(T value) requires (!Readonly) {
        if (!write_source.valid()) return detail::call_bound<void>(write, false, std::move(value));
        auto call = write_source.resolve(member);
        if (!call) throw ReflectionError(call.error());
        detail::call_bound<void>(call->target, false, std::move(value));
    }
    void bind(const Object& object, const detail::MemberLocation& entry) {
        const auto& field = entry.first->fields[entry.second];
        auto receiver = object.view();
        auto native_view = detail::operation(*entry.first, field.member, OperationKind::view);
        auto native_read = detail::operation(*entry.first, field.member, OperationKind::read);
        auto native_write = detail::operation(*entry.first, field.member, OperationKind::write);
        if (native_read || native_write || native_view) {
            auto adjusted = detail::project_legacy_view(receiver, *entry.first).value();
            if (native_view) view = detail::require_target(native_view->bind(adjusted, {}));
            if (native_read) read = detail::require_target(native_read->bind(adjusted, {}));
            if constexpr (!Readonly) if (native_write) write = detail::require_target(native_write->bind(adjusted, {}));
        }

    }
};

// ---------------------------------------------------------------------------
// Field-type builders — must be after TypedMethod/TypedProperty definitions
// because they use ^^TypedMethod / ^^TypedProperty in consteval substitute().
//
// Statics are not proxied: they are class-level, not instance-level, and
// belong on refl::Class.  Proxy<T>::get_class() and Dyn<T>::get_class()
// return the bound object's Class for static access.
// ---------------------------------------------------------------------------
namespace detail {

// Check if a dispatch field type is a TypedMethod specialization.
template <typename T> struct is_typed_method : std::false_type {};
template <typename... Sigs> struct is_typed_method<TypedMethod<Sigs...>>
    : std::true_type {};
template <typename T> inline constexpr bool is_typed_method_v =
    is_typed_method<T>::value;

// consteval: collect inherited non-static data members from public bases.
// ponytail: recursive base walk, O(n) per base level; fine for realistic
// hierarchies.  Virtual inheritance unsupported (offset_of is not constant
// for virtual bases), matching the rest of the framework.
consteval void collect_inherited_dms(std::meta::info type,
        std::vector<std::meta::info>& out) {
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto bt = std::meta::type_of(b);
            for (auto m : std::meta::nonstatic_data_members_of(bt,
                    std::meta::access_context::unchecked()))
                out.push_back(m);
            collect_inherited_dms(bt, out);
        }
    }
}

// consteval: collect all non-static method member reflections from a
// type and its public bases (for inherited methods).  Respects C++
// name-hiding: if the type declares any own method of a given name,
// all base methods of that name are hidden — matching
// find_function_in_hierarchy on the impl side.
consteval void collect_all_methods(std::meta::info type,
        std::vector<std::meta::info>& out) {
    std::vector<std::string> own_names;
    for (auto m : std::meta::members_of(type,
            std::meta::access_context::unchecked())) {
        if (is_public_method(m) && std::meta::has_identifier(m)
            && !std::meta::is_static_member(m)) {
            out.push_back(m);
            own_names.push_back(std::string(std::meta::identifier_of(m)));
        }
    }
    for (auto b : std::meta::bases_of(type,
            std::meta::access_context::unchecked())) {
        if (std::meta::is_public(b)) {
            auto bt = std::meta::type_of(b);
            std::vector<std::meta::info> inherited;
            collect_all_methods(bt, inherited);
            for (auto m : inherited) {
                auto nm = std::string(std::meta::identifier_of(m));
                bool hidden = false;
                for (const auto& on : own_names)
                    if (on == nm) { hidden = true; break; }
                if (!hidden) out.push_back(m);
            }
        }
    }
}

// Instance surface shared by synthetic metadata and native slot wiring. Match
// the dispatch struct's method hiding and first-visible-field policy.
consteval void collect_instance_members(std::meta::info type,
        std::vector<std::meta::info>& out) {
    collect_all_methods(type, out);
    std::vector<std::meta::info> fields;
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked()))
        fields.push_back(m);
    collect_inherited_dms(type, fields);
    std::vector<std::string> names;
    for (auto m : fields) {
        if (!is_public_data_member(m) || !std::meta::has_identifier(m)) continue;
        auto name = std::string(std::meta::identifier_of(m));
        bool hidden = false;
        for (const auto& previous : names)
            if (previous == name) { hidden = true; break; }
        if (!hidden) {
            names.push_back(name);
            out.push_back(m);
        }
    }
}

// consteval: build the dispatch struct member specs for T's public
// interface — non-static methods (TypedMethod) and data members
// (TypedProperty), including inherited members via base hierarchy walk.
// Shared by Proxy<T> and Dyn<T>.  When add_obj is true, an obj field
// (std::shared_ptr<T>) is appended (Dyn<T> concrete mode only).
// Fills the output vector in-place (consteval functions returning
// std::vector by value are not yet reliably supported by GCC 16.2).
consteval void make_dispatch_specs(std::meta::info type, bool add_obj,
        std::vector<std::meta::info>& specs) {

    // Non-static member functions → TypedMethod (one field per name,
    // carrying all overloads).  collect_all_methods walks the hierarchy
    // once; we group by name and build the function-type sigs inline,
    // avoiding a per-name re-walk.
    std::vector<std::string> method_names;
    std::vector<std::vector<std::meta::info>> method_sigs;
    std::vector<std::meta::info> all_methods;
    collect_all_methods(type, all_methods);
    for (auto m : all_methods) {
        auto nm = std::string(std::meta::identifier_of(m));
        bool found = false;
        for (std::size_t i = 0; i < method_names.size(); ++i) {
            if (method_names[i] == nm) {
                method_sigs[i].push_back(make_fn_sig(m));
                found = true;
                break;
            }
        }
        if (!found) {
            method_names.push_back(nm);
            method_sigs.push_back({make_fn_sig(m)});
        }
    }
    for (std::size_t i = 0; i < method_names.size(); ++i) {
        auto field_type = std::meta::substitute(^^TypedMethod,
            method_sigs[i]);
        specs.push_back(std::meta::data_member_spec(
            field_type, {.name=method_names[i]}));
    }

    // Non-static data members → TypedProperty.  All DMs (own + inherited)
    // are collected in one walk; the property type is built inline from
    // the member reflection, avoiding a per-name re-walk.  First
    // occurrence of each name wins (own members shadow bases).
    std::vector<std::string> field_names;
    std::vector<std::meta::info> all_dms;
    for (auto m : std::meta::nonstatic_data_members_of(type,
            std::meta::access_context::unchecked()))
        all_dms.push_back(m);
    collect_inherited_dms(type, all_dms);

    for (auto m : all_dms) {
        if (is_public_data_member(m) && std::meta::has_identifier(m)) {
            auto nm = std::string(std::meta::identifier_of(m));
            bool dup = false;
            for (const auto& s : field_names)
                if (s == nm) { dup = true; break; }
            if (!dup) {
                field_names.push_back(nm);
                auto mt = std::meta::type_of(m);
                bool is_const = std::meta::is_const_type(mt);
                auto clean_mt = std::meta::substitute(^^std::remove_cvref_t,
                    std::initializer_list<std::meta::info>{mt});
                auto field_type = std::meta::substitute(^^TypedProperty,
                    {clean_mt, std::meta::reflect_constant(is_const)});
                specs.push_back(std::meta::data_member_spec(
                    field_type, {.name=nm}));
            }
        }
    }

    // Dyn<T> concrete mode: append the obj field.
    if (add_obj) {
        auto sp_type = std::meta::substitute(^^std::shared_ptr,
            std::initializer_list<std::meta::info>{type});
        specs.push_back(std::meta::data_member_spec(
            sp_type, {.name="obj"}));
    }
}

}  // namespace detail

// A typed view binds an immutable structural plan to one owning object.
template <typename T>
class Proxy {
    struct Dispatch;
    consteval {
        std::vector<std::meta::info> specs;
        detail::make_dispatch_specs(^^T, false, specs);
        std::meta::define_aggregate(^^Dispatch, specs);
    }

    Dispatch dispatch_{};
    detail::BindingState state_;
    DispatchHandle source_;

    static std::shared_ptr<const detail::BindingPlan> plan_for(std::shared_ptr<const ClassInfo> descriptor) {
        static std::mutex mutex;
        static std::map<const ClassInfo*, std::weak_ptr<const detail::BindingPlan>> cache;
        std::lock_guard lock(mutex);
        std::erase_if(cache, [](const auto& entry) { return entry.second.expired(); });
        if (auto found = cache.find(descriptor.get()); found != cache.end())
            if (auto plan = found->second.lock()) return plan;
        auto plan = std::make_shared<detail::BindingPlan>();
        plan->descriptor = descriptor;
        static constexpr auto fields = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch, std::meta::access_context::unchecked()));
        template for (constexpr auto field : fields) {
            using F = [:std::meta::type_of(field):];
            auto name = std::string(std::meta::identifier_of(field));
            if constexpr (detail::is_typed_method_v<F>) {
                std::vector<detail::MemberLocation> candidates;
                auto own = detail::own_indices(descriptor->functions, name);
                for (auto index : own) candidates.emplace_back(descriptor, index);
                if (own.empty()) detail::collect_base_named(descriptor.get(), name, &ClassInfo::functions, candidates);
                detail::dedup_decl(candidates);
                if (detail::subobject_count(descriptor.get(), candidates) > 1)
                    throw ReflectionError({DiagnosticCode::ambiguous});
                auto& selected = plan->methods[name];
                for (const auto& expected : F::signatures()) {
                    if (!supported_signature(expected)) throw ReflectionError({DiagnosticCode::unsupported});
                    auto match = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                        return compatible_signature(expected, candidate.first->functions[candidate.second].signature);
                    });
                    if (match == candidates.end()) throw ReflectionError({DiagnosticCode::type_mismatch});
                    selected.push_back(*match);
                }
            } else {
                std::vector<detail::MemberLocation> candidates;
                auto own = detail::own_indices(descriptor->fields, name);
                for (auto index : own) candidates.emplace_back(descriptor, index);
                if (own.empty()) detail::collect_base_named(descriptor.get(), name, &ClassInfo::fields, candidates);
                detail::dedup_decl(candidates);
                if (detail::subobject_count(descriptor.get(), candidates) > 1)
                    throw ReflectionError({DiagnosticCode::ambiguous});
                if (candidates.empty()) throw ReflectionError({DiagnosticCode::not_found});
                const auto& selected = candidates.front();
                const auto& property = selected.first->fields[selected.second];
                if (property.signature.result.type != type_id<typename F::value_type>() ||
                    (!F::is_readonly() && property.is_const))
                    throw ReflectionError({DiagnosticCode::type_mismatch});
                plan->properties.emplace(name, selected);
            }
        }
        cache[descriptor.get()] = plan;
        return plan;
    }
    void populate() {
        static constexpr auto fields = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch, std::meta::access_context::unchecked()));
        template for (constexpr auto field : fields) {
            using F = [:std::meta::type_of(field):];
            auto name = std::string(std::meta::identifier_of(field));
            if constexpr (detail::is_typed_method_v<F>)
                dispatch_.[:field:].bind(state_, state_.plan->methods.at(name));
            else dispatch_.[:field:].bind(state_.object, state_.plan->properties.at(name));
        }
    }
public:
    Proxy() = default;
    explicit Proxy(Object object) {
        if (!object.valid()) return;
        if (!object.is_owned()) throw ReflectionError({DiagnosticCode::missing_anchor});
        if (!object.class_info()) throw ReflectionError({DiagnosticCode::null_handle});
        state_ = {std::move(object), {}};
        state_.plan = plan_for(state_.object.class_info());
        populate();
    }
    Proxy(DispatchHandle methods, DispatchHandle reads = {}, DispatchHandle writes = {}) : source_(methods) {
        if (!methods.valid()) throw ReflectionError({DiagnosticCode::null_handle});
        auto schema = describe_interface<T>();
        for (const auto& required : *schema) {
            if (required.operation == OperationKind::view) continue;
            const auto& source = required.operation == OperationKind::method ? methods :
                                 required.operation == OperationKind::read ? reads : writes;
            if (!source.valid()) throw ReflectionError({DiagnosticCode::not_found, required.member});
            auto found = std::find_if(source.schema()->begin(), source.schema()->end(), [&](const auto& operation) {
                return operation.member == required.member && operation.operation == required.operation &&
                       compatible_signature(required.signature, operation.signature);
            });
            if (found == source.schema()->end()) throw ReflectionError({DiagnosticCode::type_mismatch, required.member});
        }
        static constexpr auto fields = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch, std::meta::access_context::unchecked()));
        template for (constexpr auto field : fields) {
            using F = [:std::meta::type_of(field):];
            auto& target = dispatch_.[:field:];
            if constexpr (detail::is_typed_method_v<F>) target.source = methods;
            else { target.read_source = reads; target.write_source = writes; }
            for (const auto& operation : *schema) {
                if (operation.name != std::meta::identifier_of(field)) continue;
                if constexpr (detail::is_typed_method_v<F>) {
                    if (operation.operation == OperationKind::method) target.members.push_back(operation.member);
                } else target.member = operation.member;
            }
        }
    }
    std::map<OperationKey, CallTarget> native_targets() const {
        std::map<OperationKey, CallTarget> result;
        auto schema = describe_interface<T>();
        static constexpr auto fields = std::define_static_array(
            std::meta::nonstatic_data_members_of(^^Dispatch, std::meta::access_context::unchecked()));
        template for (constexpr auto field : fields) {
            using F = [:std::meta::type_of(field):];
            const auto& target = dispatch_.[:field:];
            [[maybe_unused]] std::size_t index = 0;
            for (const auto& operation : *schema) {
                if (operation.name != std::meta::identifier_of(field)) continue;
                if constexpr (detail::is_typed_method_v<F>) {
                    result.emplace(OperationKey{operation.member, operation.operation}, target.targets.at(index++));
                } else {
                    const auto& selected = operation.operation == OperationKind::read ? target.read :
                                           operation.operation == OperationKind::write ? target.write : target.view;
                    if (selected.valid()) result.emplace(OperationKey{operation.member, operation.operation}, selected);
                }
            }
        }
        return result;
    }
    void bind(Object object) { *this = Proxy(std::move(object)); }
    auto* operator->() { return &dispatch_; }
    const auto* operator->() const { return &dispatch_; }
    bool is_bound() const { return state_.object.valid() || source_.valid(); }
    Class get_class() const { return Class(state_.object.class_info()); }
    const std::shared_ptr<const detail::BindingPlan>& binding_plan() const { return state_.plan; }

    // -----------------------------------------------------------------------
    // Operators — looked up at call time via Object::invoke_op, which
    // resolves overloads by the argument's runtime type.
    //
    // The return type depends on the interface's operator return type:
    //   - void:           returns nothing
    //   - bool:           returns bool (comparison operators)
    //   - class type:     returns Proxy<R> wrapping the result (chainable)
    //   - non-class type: returns the raw value (int, double, etc.)
    //
    // Each operator generates two methods:
    //   1. Same-type: operator+(const Proxy& other)
    //   2. Generic: template<U> operator+(U&& val)
    //
    // Neither exists if T has no such operator.  The constraint uses
    // decltype — no consteval helpers needed for the return type.
    // -----------------------------------------------------------------------
public:
#define PROXY_BINARY_OP(symbol, op_name) \
    auto operator symbol(const Proxy& other) const \
        requires requires(T a, T b) { a symbol b; } \
    { \
        using R = decltype(std::declval<T>() symbol std::declval<T>()); \
        Object arg = other.state_.object; \
        auto result = state_.object.invoke_op(op_name, arg); \
        if (!result) \
            throw std::runtime_error( \
                "Proxy: " op_name " — no matching overload for '" + \
                std::string(arg.class_name()) + "' in '" + \
                std::string(state_.object.class_name()) + "'"); \
        if constexpr (std::is_void_v<R>) return; \
        else if constexpr (std::is_class_v<std::remove_cvref_t<R>>) \
            return Proxy(std::move(*result)); \
        else { \
            auto cr = result->template cast_ref<std::remove_cvref_t<R>>(); \
            if (!cr) throw std::runtime_error( \
                "Proxy: " op_name " — return type mismatch: interface expects '" \
                + std::string(detail::type_name<std::remove_cvref_t<R>>()) \
                + "', impl returns '" + std::string(result->class_name()) + "'"); \
            return std::move(*cr.value()); \
        } \
    } \
    template <typename U> \
        requires (!std::is_same_v<std::remove_cvref_t<U>, Proxy>) \
              && requires(T a, U b) { a symbol b; } \
    auto operator symbol(U&& val) const \
    { \
        using R = decltype(std::declval<T>() symbol std::declval<U>()); \
        Object arg(std::forward<U>(val)); \
        auto result = state_.object.invoke_op(op_name, arg); \
        if (!result) \
            throw std::runtime_error( \
                "Proxy: " op_name " — no matching overload for '" + \
                std::string(arg.class_name()) + "' in '" + \
                std::string(state_.object.class_name()) + "'"); \
        if constexpr (std::is_void_v<R>) return; \
        else if constexpr (std::is_class_v<std::remove_cvref_t<R>>) \
            return Proxy(std::move(*result)); \
        else { \
            auto cr = result->template cast_ref<std::remove_cvref_t<R>>(); \
            if (!cr) throw std::runtime_error( \
                "Proxy: " op_name " — return type mismatch: interface expects '" \
                + std::string(detail::type_name<std::remove_cvref_t<R>>()) \
                + "', impl returns '" + std::string(result->class_name()) + "'"); \
            return std::move(*cr.value()); \
        } \
    }

    PROXY_BINARY_OP(+, "operator+")
    PROXY_BINARY_OP(-, "operator-")
    PROXY_BINARY_OP(*, "operator*")
    PROXY_BINARY_OP(/ , "operator/")
    PROXY_BINARY_OP(%, "operator%")
    PROXY_BINARY_OP(==, "operator==")
    PROXY_BINARY_OP(!=, "operator!=")
    PROXY_BINARY_OP(< , "operator<")
    PROXY_BINARY_OP(> , "operator>")
    PROXY_BINARY_OP(<=, "operator<=")
    PROXY_BINARY_OP(>=, "operator>=")

#undef PROXY_BINARY_OP

    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;
    Proxy(Proxy&& other) noexcept
        : dispatch_(std::move(other.dispatch_)), state_(std::move(other.state_)), source_(std::move(other.source_)) {
        other.dispatch_ = {};
        other.state_ = {};
        other.source_ = {};
    }
    Proxy& operator=(Proxy&& other) noexcept {
        if (this != &other) {
            dispatch_ = std::move(other.dispatch_);
            state_ = std::move(other.state_);
            source_ = std::move(other.source_);
            other.dispatch_ = {};
            other.state_ = {};
        other.source_ = {};
        }
        return *this;
    }

};

}  // namespace refl
