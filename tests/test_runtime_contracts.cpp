#include <refl/refl.hpp>
#include <cstdio>
#include <cstdlib>
#include <source_location>

void check(bool value, std::source_location at = std::source_location::current()) {
    if (!value) { std::fprintf(stderr, "runtime contract failed: %u\n", at.line()); std::abort(); }
}
struct RuntimeBase { int base = 7; };
struct RuntimeObject : RuntimeBase {
    int effects = 0;
    int field = 4;
    void update(int& out) { ++effects; out = field; }
    int consume(std::unique_ptr<int> n) { ++effects; return *n; }
    std::unique_ptr<int> produce() const { return std::make_unique<int>(field); }
    const int& reference() const { return field; }
    int from_base(const RuntimeBase& base) const { return base.base; }
    void throws() { ++effects; throw std::bad_cast{}; }
    static int twice(int n) { return n * 2; }
};
struct UnsupportedReceiver { int call() && { return 1; } };
int main() {
    refl::ensure_registered<RuntimeBase>();
    refl::ensure_registered<RuntimeObject>();
    auto type = refl::find_class("RuntimeObject").value();
    auto object = type.find_constructor({})->call().value();
    int out = 0;
    auto update = type.find_function("update").value();
    check(update.invoke(object, out).has_value() && out == 4);
    const int constant = 0;
    check(!update.invoke(object, constant));
    check(!update.invoke(object, 3));
    auto concrete = object.cast_safe<RuntimeObject>().value();
    check(concrete->effects == 1);
    auto readonly = update.try_invoke(refl::borrow_const(*concrete), out);
    check(!readonly && readonly.error().code == refl::DiagnosticCode::read_only && concrete->effects == 1);
    auto consume = type.find_function("consume").value();
    auto pointer = std::make_unique<int>(8);
    check(!consume.invoke(object, pointer) && pointer && concrete->effects == 1);
    check(*consume.invoke(object, std::move(pointer))->cast_safe<int>().value() == 8 && !pointer);
    auto produced = type.find_function("produce")->invoke(object).value();
    check(**produced.cast_safe<std::unique_ptr<int>>().value() == 4);
    check(*type.find_function("from_base")->invoke(object, *concrete)->cast_safe<int>().value() == 7);
    auto field = type.find_field("field").value();
    check(!field.set(object.as_const(), 12));
    check(field.set(object, 12).has_value());
    check(*field.get(object.as_const())->cast_safe<int>().value() == 12);
    auto reference = type.find_function("reference")->try_invoke(object.view()).value();
    check(refl::result_view(reference).read_only());
    check(!refl::result_view(reference).get<int>());
    check(!refl::result_view(reference).anchor()); // Strict default does not infer provenance.
    auto legacy_ref = type.find_function("reference")->invoke(object).value();
    check(!legacy_ref.cast_ref<int>());
    check(*legacy_ref.cast_safe<const int>().value() == 12);
    auto before = concrete->effects;
    try { (void)type.find_function("throws")->invoke(object); check(false); }
    catch (const std::bad_cast&) {}
    check(concrete->effects == before + 1);
    check(*type.find_static_function("twice")->invoke(6)->cast_safe<int>().value() == 12);
    refl::ensure_registered<UnsupportedReceiver>();
    auto unsupported = refl::find_class("UnsupportedReceiver").value();
    auto receiver = unsupported.find_constructor({})->call().value();
    auto failed = unsupported.find_function("call")->try_invoke(receiver.view());
    check(!failed && failed.error().code == refl::DiagnosticCode::unsupported);
    std::puts("runtime contracts passed");
}
