// ecs/event/observer.hpp
//
// Visitor helpers for building observers over a std::variant of events, in
// namespace ecs::event. Pair with event/emitter.hpp (the Emitter that broadcasts
// events to observers): the two are independent -- an Emitter stores any callable
// void(Event const&); these help you write one that std::visits.
//
//   overloaded{f, g, ...}  -- one callable whose overload set is f | g | ...: the
//                             visitor half of std::visit. With no catch-all it is
//                             *exhaustive* -- std::visit fails to compile on an
//                             unhandled alternative, a compile-time safety net for
//                             when a new event kind is added to the variant.
//
//   observer(h1, h2, ...)  -- a callable Event -> void built from per-alternative
//                             handlers, with a catch-all appended so it ignores
//                             any alternative you did not name: a *partial* visitor.
//
//   bus.add(ecs::event::observer(
//       [](Opened const& o) { open(o.fd); },
//       [](Closed const&)   { close();    }));   // other alternatives ignored
//
// The observer() lambda is `mutable`, so a value-captured `mutable` handler
// (`[n = 0](Ev const&) mutable { ++n; }`) is a self-contained stateful observer.

#pragma once

#include <utility>
#include <variant>

namespace ecs::event {

// overloaded{f, g, ...} is one callable whose overload set is the union of the
// f, g, ... call operators -- the visitor half of std::visit. With no catch-all
// it is exhaustive: std::visit fails to compile if an alternative is unhandled.
template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

// Build an observer (a callable Event -> void) from per-alternative handlers. A
// trailing catch-all swallows any alternative none of the handlers match, so an
// observer may react to just the events it wants. For an exhaustive visitor (a
// compile error on a missing alternative) use overloaded + std::visit directly.
//
// The returned observer is `mutable`, so a handler may hold its own mutable state
// (e.g. `[n = 0](Ev const&) mutable { ++n; }`) -- a self-contained stateful
// observer, no external object needed. It persists across emits because the
// Emitter stores the observer once and reuses it.
template <class... Handlers>
auto observer(Handlers... handlers) {
    auto visitor = overloaded {std::move(handlers)..., [](auto const&) {}};
    return [visitor = std::move(visitor)](auto const& event) mutable {
        std::visit(visitor, event);
    };
}

} // namespace ecs::event
