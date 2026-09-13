// Dynamic replacement tests: methods, properties, and owned target lifetimes.
// Cross-cutting call behavior stays outside the dispatch table.
#include <refl/dyn.hpp>

#include <cstdio>
#include <stdexcept>

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } \
} while (0)

struct IShape {
    virtual int  area(int scale) = 0;
    virtual void set_color(int c) = 0;
    virtual int  color() const = 0;
    virtual ~IShape() = default;
};

struct ICalculator {
    virtual int add(int a, int b) = 0;
    virtual ~ICalculator() = default;
};

struct IWidget {
    int width;
    int height;
    virtual void draw() = 0;
    virtual ~IWidget() = default;
};

int main() {
    // === Basic mocking ===
    auto m = std::make_shared<refl::Dyn<IShape>>();
    m->implement<^^IShape::area>([](int scale) { return scale * 100; });
    int last_color = -1;
    m->implement<^^IShape::set_color>([&last_color](int c) { last_color = c; });
    m->implement<^^IShape::color>([]() { return 42; });

    auto p = m->capture_binding();
    CHECK(p->area(5) == 500, "area(5) should be 500");
    p->set_color(99);
    CHECK(last_color == 99, "set_color should store 99");
    CHECK(p->color() == 42, "color() should be 42");

    // === Re-implement post-bind ===
    m->implement<^^IShape::area>([](int scale) { return scale * 200; });
    CHECK(p->area(5) == 1000, "re-implemented area(5) should be 1000");

    // === Replacement with captured state ===
    int callback_result = 0;
    m->implement<^^IShape::area>([&callback_result](int scale) {
        int r = scale * 200;
        callback_result = r;
        return r;
    });
    (void)p->area(5);
    CHECK(callback_result == 1000, "replacement should capture result 1000");

    // === Target save / restore ===
    auto saved = m->target<^^IShape::area>();
    m->implement<^^IShape::area>([](int scale) { return scale * 999; });
    CHECK(p->area(1) == 999, "overridden area(1) should be 999");
    m->replace<^^IShape::area>(saved);
    CHECK(p->area(5) == 1000, "restored area(5) should be 1000 again");

    // === 2-arg method ===
    auto mc = std::make_shared<refl::Dyn<ICalculator>>();
    mc->implement<^^ICalculator::add>([](int a, int b) { return a + b; });
    auto pc = mc->capture_binding();
    CHECK(pc->add(3, 4) == 7, "add(3,4) should be 7");

    // === Stored-value property (sugar) ===
    auto mw = std::make_shared<refl::Dyn<IWidget>>();
    mw->set_property<^^IWidget::width>(42);
    mw->set_property<^^IWidget::height>(24);
    mw->implement<^^IWidget::draw>([]() {});
    auto pw = mw->capture_binding();
    CHECK(pw->width == 42, "width should be 42");
    CHECK(pw->height == 24, "height should be 24");

    // === Property write through proxy ===
    pw->width = 99;
    CHECK(pw->width == 99, "width after write should be 99");

    // === Virtual property with setter state ===
    int width_changed_to = 0;
    int stored_w = 42;
    mw->implement_property<^^IWidget::width>(
        [&stored_w]() { return stored_w; },
        [&width_changed_to, &stored_w](int v) {
            stored_w = v;
            width_changed_to = v;
        }
    );
    pw->width = 55;
    CHECK(pw->width == 55, "width after virtual set should be 55");
    CHECK(width_changed_to == 55, "setter should capture 55");

    // === Read-only virtual property ===
    mw->implement_property<^^IWidget::height>(
        []() { return 88; }
    );
    CHECK(pw->height == 88, "read-only property should return 88");

    // === Property target save / restore ===
    auto prop_saved = mw->target<^^IWidget::height>(refl::OperationKind::read);
    mw->implement_property<^^IWidget::height>(
        []() { return 77; }
    );
    CHECK(pw->height == 77, "overridden height should be 77");
    mw->replace<^^IWidget::height>(prop_saved, refl::OperationKind::read);
    CHECK(pw->height == 88, "restored height should be 88");

    // === Lifetime: facade destroyed, Proxy keeps it alive ===
    refl::Proxy<IShape> p2;
    {
        auto m2 = std::make_shared<refl::Dyn<IShape>>();
        m2->implement<^^IShape::area>([](int s) { return s * 1000; });
        p2 = m2->capture_binding();
    }
    CHECK(p2->area(5) == 5000, "area(5) after facade destroyed should be 5000");

    CHECK(!m->get_class(), "interface-only state has no native storage identity");

    // Saved slots retain exactly the implementation they captured.
    {
        auto retained = std::make_shared<refl::Dyn<IShape>>();
        auto context = std::make_shared<int>(123);
        std::weak_ptr<int> lifetime = context;
        retained->implement<^^IShape::area>([context](int scale) {
            return *context * scale;
        });
        context.reset();
        auto original = retained->target<^^IShape::area>();
        retained->implement<^^IShape::area>([](int) { return 9; });
        CHECK(!lifetime.expired(), "saved method slot should retain its context");
        retained->replace<^^IShape::area>(original);
        CHECK(retained->capture_binding()->area(2) == 246, "saved method should restore its context");
        original = {};
        retained->implement<^^IShape::area>([](int) { return 8; });
        CHECK(lifetime.expired(), "replaced method context should be released");
    }

    // A getter saved before replacement retains its original backing value.
    {
        auto retained = std::make_shared<refl::Dyn<IWidget>>();
        auto context = std::make_shared<int>(321);
        std::weak_ptr<int> lifetime = context;
        retained->implement_property<^^IWidget::height>([context] { return *context; });
        context.reset();
        auto original = retained->target<^^IWidget::height>(refl::OperationKind::read);
        retained->implement_property<^^IWidget::height>([] { return 8; });
        CHECK(!lifetime.expired(), "saved property slot should retain its context");
        retained->replace<^^IWidget::height>(original, refl::OperationKind::read);
        CHECK(retained->capture_binding()->height == 321, "saved getter should restore its context");
        original = {};
        retained->implement_property<^^IWidget::height>([] { return 9; });
        CHECK(lifetime.expired(), "replaced getter context should be released");
    }

    // Replacing the executing slot must not destroy its callable mid-call.
    {
        auto changing = std::make_shared<refl::Dyn<IShape>>();
        auto context = std::make_shared<int>(1);
        std::weak_ptr<int> lifetime = context;
        changing->implement<^^IShape::area>([raw = changing.get(), context](int) {
            std::weak_ptr<int> during_call = context;
            raw->implement<^^IShape::area>([](int) { return 22; });
            return during_call.expired() ? -1 : 11;
        });
        context.reset();
        auto view = changing->capture_binding();
        CHECK(view->area(0) == 11, "active call should retain the replaced context");
        CHECK(lifetime.expired(), "active context should be released after return");
        CHECK(view->area(0) == 22, "next call should see the replacement");
    }

    // Schema ownership does not require a synthetic ClassInfo or backend facade.
    std::shared_ptr<const refl::InterfaceSchema> schema;
    std::weak_ptr<refl::Dyn<IShape>> backend_lifetime;
    {
        auto backend = std::make_shared<refl::Dyn<IShape>>();
        backend_lifetime = backend;
        schema = backend->dispatch().schema();
    }
    CHECK(backend_lifetime.expired(), "schema does not retain the facade");
    CHECK(schema->size() == 3, "schema survives the facade");

    printf("All dispatch table tests passed.\n");
    return 0;
}
