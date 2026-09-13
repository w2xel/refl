#include <refl/runtime/invoke.hpp>
#include "contract_types.hpp"
#include <cstdio>
#include <cstdlib>
#include <source_location>

void check(bool condition, std::source_location at = std::source_location::current()) {
    if (!condition) { std::fprintf(stderr, "contract check failed: %u\n", at.line()); std::abort(); }
}
int main() {
    using namespace refl;
    static_assert(type_id<contract_test::Widget>() != type_id<contract_test::widget>());
    static_assert(type_use<int&>() != type_use<const int&>());
    static_assert(type_use<const int*>() != type_use<int* const>());
    static_assert(type_use<int[2]>() != type_use<int[3]>());
    check(type_id<contract_test::Widget>() == contract_test::peer_identity());
    auto pointer = describe_type<const int*>();
    check(pointer->kind == TypeKind::pointer && pointer->element.qualifiers == Cv::constant);
    auto array = describe_type<int[3][2]>();
    check(array->extent == 3 && array->element_descriptor->extent == 2);
    auto signature = signature_of<int(const int&, int&&) const volatile && noexcept>();
    check(signature.receiver.qualifiers == Cv::constant_volatile);
    check(signature.receiver.reference == ReferenceKind::rvalue && signature.is_noexcept);
    check(!supported_signature(signature));
    check(!make_target<int(volatile int&)>([](volatile int& n) { return n; }));
    check(!make_target<volatile int&()>([]() -> volatile int& { static volatile int n; return n; }));
    check(!make_target<int()>([] {}));
    int mutable_value = 3;
    const int immutable = 4;
    check(!borrow_const(mutable_value).get<int>());
    check(!borrow(immutable).get<int>());
    check(*borrow(immutable).get<const int>().value() == 4);
    check(!borrow(mutable_value).get<double>());
    int effects = 0;
    auto target = make_target<int(int&)>([&](int& n) { ++effects; return ++n; }).value();
    constexpr MemberId member{type_id<contract_test::Widget>(), 0};
    auto schema = std::make_shared<const InterfaceSchema>(InterfaceSchema{{member, "next", target.signature()}});
    DispatchHandle source(schema, [target](auto) -> Result<ResolvedCall> { return ResolvedCall{target, {}, {}}; });
    auto result = try_call<double>(source, member, mutable_value);
    check(!result && result.error().code == DiagnosticCode::type_mismatch && effects == 0);
    auto arity = try_call<int>(source, member);
    check(!arity && arity.error().code == DiagnosticCode::arity_mismatch && effects == 0);
    auto readonly = try_call<int>(source, member, immutable);
    check(!readonly && readonly.error().argument_index == 0 && effects == 0);
    auto rvalue = try_call<int>(source, member, 5);
    check(!rvalue && effects == 0);
    check(try_call<int>(source, member, mutable_value).value() == 4 && effects == 1);
    auto wrong = make_target<double(int&)>([](int&) { return 1.0; }).value();
    DispatchHandle corrupt(schema, [wrong](auto) -> Result<ResolvedCall> { return ResolvedCall{wrong, {}, {}}; });
    check(!try_call<int>(corrupt, member, mutable_value));
    // A provider cannot hide the bound receiver's read-only access.
    auto readonly_receiver = make_target<int(int&)>([&](int&) { ++effects; return 1; },
        {.receiver = borrow(immutable)}).value();
    DispatchHandle bound(schema, [readonly_receiver](auto) -> Result<ResolvedCall> {
        return ResolvedCall{readonly_receiver, {}, {}};
    });
    check(!try_call<int>(bound, member, mutable_value) && effects == 1);
    auto reference = make_target<const int&(const int&)>([](const int& n) -> const int& { return n; },
        {.result_lifetime = ResultLifetime::argument, .reference_export = ReferenceExport::caller_borrow}).value();
    auto ref_schema = std::make_shared<const InterfaceSchema>(InterfaceSchema{{member, "ref", reference.signature()}});
    DispatchHandle refs(ref_schema, [reference](auto) -> Result<ResolvedCall> { return ResolvedCall{reference, {}, {}}; });
    check(!try_call<const int&>(refs, member, 5));
    check(&try_call<const int&>(refs, member, immutable).value().get() == &immutable);
    check(!try_call_retained<const int>(refs, member, immutable));
    auto owner = std::make_shared<const int>(8);
    ArgumentView anchored{ObjectView::from(*owner, owner), ValueCategory::lvalue};
    auto retained = try_call_retained<const int>(refs, member, anchored).value();
    owner.reset();
    check(retained.get() == 8);
    std::puts("core contracts passed");
}
