# Using the proposed architecture

These are target API sketches, not examples of the current implementation. They
show the application-facing operations without requiring knowledge of binding
plans or call frames. The [capability table](generation.md#initial-callable-capabilities)
defines the first implementation target.

## Bind owned or borrowed storage

```cpp
// Target sketch; refl names are unqualified for readability.
struct Drawable { int render(int scale) const; };
struct Square {
    int side;
    int render(int scale) const { return side * side * scale; }
};
struct Triangle {
    int base, height;
    int render(int scale) const { return base * height * scale / 2; }
};

auto object = own(Square{4});
auto view = try_bind<Drawable>(object).value();
int pixels = view->render(2);  // 32; view retains the owned storage

Square local{3};
auto borrowed = try_bind<Drawable>(borrow(local)).value();
int local_pixels = borrowed->render(2);  // 18
auto read_only = try_bind<Drawable>(borrow_const(local)).value();
```

Keep `local` alive while either borrowed proxy is used. Binding a borrow does not
copy it or extend its lifetime. A read-only borrow cannot bind an interface that
requires mutating operations. No registry is needed to bind these known objects;
`own` and the borrow helpers obtain metadata through generation without publishing
it. Register types when the application needs discovery by name.

## Handle a failed bind

```cpp
struct NotDrawable { int side; };
auto candidate = try_bind<Drawable>(own(NotDrawable{4}));
if (!candidate) {
    report(candidate.error());  // application diagnostic reporting
} else {
    view = std::move(candidate).value();
}
int still_usable = view->render(2);  // failed candidate leaves the old view intact
```

The diagnostic identifies the missing requirement and expected signature, with
candidates where available. `.value()` is convenient in sketches; handle `Result`
at application boundaries where failure is expected. Target exceptions and
allocation/copy/move failures can still propagate separately.

## Replace one overload

```cpp
struct Renderer {
    int render(int scale) const;
    int render(double scale) const;
};
constexpr auto render_int = member<Renderer, "render", int(int) const>;
constexpr auto render_double = member<Renderer, "render", int(double) const>;

Dynamic<Renderer> dynamic;
dynamic.implement(render_int, [](int scale) { return 100 * scale; }).value();
dynamic.implement(render_double, [](double scale) {
    return static_cast<int>(50 * scale);
}).value();
dynamic.implement(render_int, [](int scale) { return 200 * scale; }).value();

int changed = dynamic->render(2);       // 400
int unchanged = dynamic->render(2.0);   // 100
```

The member token identifies a declaration, so replacement cannot accidentally
change both overloads. A name-only convenience must report ambiguity. A newly
created interface-only dynamic object has unimplemented slots; calling one fails
validation until an implementation is supplied.

## Reset with live and captured views

```cpp
constexpr auto render_member = member<Drawable, "render", int(int) const>;
auto dynamic = Dynamic<Drawable>::from(own(Square{4})).value();
auto live = try_bind<Drawable>(dynamic.dispatch()).value();
auto captured = dynamic.capture_binding();
auto observed = observe(dynamic, observer_error_sink);
auto subscription = observed.after(render_member, record_result);

dynamic.reset(own(Triangle{6, 8})).value();
int fresh = live->render(2);          // 48, from Triangle
int old = captured->render(2);       // 32, from the retained Square generation
int recorded = observed->render(2);  // 48; subscription survives reset
dynamic.reset_native<Square>(5).value();
```

`observer_error_sink` and `record_result` are application callbacks. Only calls
through `observed` deliver its events. A captured generation still has mutable
slots: capture preserves generation identity, not a frozen copy of every target.
An incompatible reset leaves live dispatch and the current generation intact.
Reset discards replacements in the new generation and explicitly selects new
storage; it does not guess a concrete constructor from `Drawable`.

## Retain a reference across replacement

```cpp
struct TextSource { const std::string& text() const; };
constexpr auto text_member = member<TextSource, "text", const std::string&() const>;
Dynamic<TextSource> text;
text.implement(text_member,
    [value = std::string("original")]() -> const std::string& { return value; },
    {.result_lifetime = callable_context, .native_dependency = independent}).value();

auto observed_text = observe(text, observer_error_sink);
auto subscription = observed_text.after(text_member, [&](const CallCompletedEvent&) {
    text.implement(text_member,
        [value = std::string("replacement")]() -> const std::string& { return value; },
        {.result_lifetime = callable_context, .native_dependency = independent}).value();
});

auto retained = try_call_retained<const std::string>(observed_text, text_member).value();
use(retained.get());  // "original"; retained owns the invoked closure's anchor
```

The registration options assert that the referent lives in the selected closure
and that the closure has no native dependency. The retained helper delivers the
event before returning and preserves the original result owner despite replacement.
Do not store the reference from `get()` beyond the retained handle's lifetime.

An ordinary `observed_text->text()` call is rejected before entering this target:
its closure anchor does not permit exporting a raw C++ reference. Ordinary typed
reference export requires a separate explicit `caller_borrow` policy and independent
caller ownership through observation and later use. Use retained extraction when
dispatch owns the referent. Neither policy protects against mutations that invalidate
an element's address. See the [reference contract](contracts.md#exporting-a-reference-to-the-caller).
