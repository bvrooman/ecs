// tests/test_emitter.cpp -- the standalone variant event bus (ecs/event/emitter.hpp):
// overloaded, the observer() partial-visitor factory, and Emitter add/emit/remove.
// No engine dependency beyond the header.

#include "check.hpp"
#include "ecs/event/emitter.hpp"

#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using ecs::event::Emitter;
using ecs::event::observer;
using ecs::event::overloaded;

// A small event set: each alternative carries its own payload.
struct Hello {
    int n;
};
struct Bye {};
struct Ping {
    std::string msg;
};
using Event = std::variant<Hello, Bye, Ping>;

// overloaded + std::visit is an exhaustive visitor (every alternative handled).
static void overloaded_visits_each_alternative() {
    auto tag = [](Event const& e) {
        return std::visit(overloaded {[](Hello const&) { return 'H'; },
                                      [](Bye const&) { return 'B'; },
                                      [](Ping const&) { return 'P'; }},
                          e);
    };
    CHECK(tag(Hello {1}) == 'H');
    CHECK(tag(Bye {}) == 'B');
    CHECK(tag(Ping {"x"}) == 'P');
}

// observer(handlers...) reacts to the alternatives it names and ignores the rest.
static void observer_ignores_unhandled() {
    int hellos = 0, pings = 0;
    auto obs = observer([&](Hello const& h) { hellos += h.n; },
                        [&](Ping const& p) { pings += int(p.msg.size()); });
    obs(Event {Hello {3}}); // handled
    obs(Event {Bye {}});    // no handler -> ignored by the catch-all
    obs(Event {Ping {"ab"}}); // handled
    CHECK(hellos == 3);
    CHECK(pings == 2);
}

// Emitter broadcasts to every observer, in registration order.
static void emitter_broadcasts_in_order() {
    Emitter<Event> bus;
    std::vector<int> log;
    bus.add(observer([&](Hello const& h) { log.push_back(10 + h.n); }));
    bus.add(observer([&](Hello const& h) { log.push_back(20 + h.n); }));
    CHECK(bus.observer_count() == 2);
    bus.emit(Hello {5});
    CHECK((log == std::vector<int> {15, 25}));
}

// emit accepts a bare alternative or a whole Event, lvalue or rvalue.
static void emit_accepts_alternative_or_event() {
    Emitter<Event> bus;
    int sum = 0;
    bus.add(observer([&](Hello const& h) { sum += h.n; }));
    bus.emit(Hello {1});         // alternative (rvalue)
    Event const ev = Hello {2};  //
    bus.emit(ev);                // whole Event (lvalue)
    bus.emit(Event {Hello {3}}); // whole Event (rvalue)
    CHECK(sum == 6);
}

// remove(token) detaches exactly that observer; a stale/unknown token is a no-op.
static void remove_detaches_one() {
    Emitter<Event> bus;
    int a = 0, b = 0;
    auto const ta = bus.add(observer([&](Bye const&) { ++a; }));
    bus.add(observer([&](Bye const&) { ++b; }));
    bus.emit(Bye {});
    CHECK((a == 1 && b == 1));

    CHECK(bus.remove(ta));
    CHECK(bus.observer_count() == 1);
    bus.emit(Bye {});
    CHECK((a == 1 && b == 2)); // a detached, b still notified

    CHECK(!bus.remove(ta));                            // already gone
    CHECK(!bus.remove(Emitter<Event>::Token {9999}));  // never issued
    CHECK(bus.add(Emitter<Event>::Observer {}) == 0);  // empty observer ignored
    CHECK(bus.observer_count() == 1);
}

// Emitting with no observers does nothing (and must not throw).
static void empty_emit_is_noop() {
    Emitter<Event> bus;
    bus.emit(Hello {1});
    CHECK(bus.observer_count() == 0);
}

// A stateful observer: a class with operator()(Event const&) registered by
// std::ref -- the pattern the timing tools use.
struct Counter {
    int hellos = 0, others = 0;
    void operator()(Event const& e) {
        std::visit(overloaded {[&](Hello const&) { ++hellos; },
                               [&](auto const&) { ++others; }},
                   e);
    }
};
static void stateful_observer_via_ref() {
    Emitter<Event> bus;
    Counter c;
    bus.add(std::ref(c));
    bus.emit(Hello {});
    bus.emit(Bye {});
    bus.emit(Hello {});
    CHECK(c.hellos == 2);
    CHECK(c.others == 1);
}

// RAII: subscribe() returns a handle that removes the observer on scope exit.
static void subscription_removes_on_scope_exit() {
    Emitter<Event> bus;
    int a = 0;
    {
        auto sub = bus.subscribe(observer([&](Bye const&) { ++a; }));
        CHECK(bool(sub));
        CHECK(bus.observer_count() == 1);
        bus.emit(Bye {});
        CHECK(a == 1);
    } // sub destroyed here -> observer removed
    CHECK(bus.observer_count() == 0);
    bus.emit(Bye {});
    CHECK(a == 1); // no longer notified
}

// A Subscription is move-only; release() hands back the token without removing.
static void subscription_moves_and_releases() {
    Emitter<Event> bus;
    int a = 0;
    auto sub   = bus.subscribe(observer([&](Bye const&) { ++a; }));
    auto moved = std::move(sub);
    CHECK(!sub); // moved-from is empty (removes nothing on destruction)
    CHECK(bool(moved));
    CHECK(bus.observer_count() == 1);

    auto const tok = moved.release(); // give up ownership, keep the registration
    CHECK(!moved);
    CHECK(bus.observer_count() == 1);
    bus.emit(Bye {});
    CHECK(a == 1);
    CHECK(bus.remove(tok)); // now remove it by hand
    CHECK(bus.observer_count() == 0);
}

// A vector<Subscription> ties several registrations to one owner's lifetime.
static void subscriptions_collected_in_a_bag() {
    Emitter<Event> bus;
    int hits = 0;
    {
        std::vector<Emitter<Event>::Subscription> bag;
        bag.push_back(bus.subscribe(observer([&](Hello const&) { ++hits; })));
        bag.push_back(bus.subscribe(observer([&](Bye const&) { ++hits; })));
        CHECK(bus.observer_count() == 2);
        bus.emit(Hello {});
        bus.emit(Bye {});
        CHECK(hits == 2);
    } // bag destroyed -> both removed
    CHECK(bus.observer_count() == 0);
}

// A handler that owns mutable state (value capture + mutable): the count persists
// across emits inside the stored observer; `last` (by reference) reads it out.
static void observer_owns_mutable_state() {
    Emitter<Event> bus;
    int last = 0;
    bus.add(observer([count = 0, &last](Hello const&) mutable { last = ++count; }));
    bus.emit(Hello {});
    CHECK(last == 1);
    bus.emit(Hello {});
    CHECK(last == 2);
    bus.emit(Bye {}); // ignored; does not touch count
    bus.emit(Hello {});
    CHECK(last == 3);
}

int main() {
    RUN_SUITE(overloaded_visits_each_alternative);
    RUN_SUITE(observer_ignores_unhandled);
    RUN_SUITE(emitter_broadcasts_in_order);
    RUN_SUITE(emit_accepts_alternative_or_event);
    RUN_SUITE(remove_detaches_one);
    RUN_SUITE(empty_emit_is_noop);
    RUN_SUITE(stateful_observer_via_ref);
    RUN_SUITE(subscription_removes_on_scope_exit);
    RUN_SUITE(subscription_moves_and_releases);
    RUN_SUITE(subscriptions_collected_in_a_bag);
    RUN_SUITE(observer_owns_mutable_state);
    return REPORT();
}
