// ecs/event/emitter.hpp
//
// A minimal, standalone broadcast emitter over a std::variant of event types, in
// namespace ecs::event (its own folder/namespace so the short name Emitter does
// not collide with domain types like a particle-fountain Emitter component under
// `using namespace ecs;`).
//
// An Emitter<Event> keeps a list of observers and broadcasts each emitted event
// to all of them, in registration order. `Event` is a std::variant whose
// alternatives are small structs, each carrying the data for one kind of event.
// An observer is any callable `void(Event const&)` -- typically one that
// std::visits the variant. The visitor helpers to write one (`observer(...)`,
// `overloaded`) live in event/observer.hpp; the Emitter itself is agnostic to
// how an observer is built.
//
//   struct Opened { int fd; };
//   struct Closed {};
//   using Event = std::variant<Opened, Closed>;
//
//   ecs::event::Emitter<Event> bus;
//   auto tok = bus.add(ecs::event::observer(          // observer(): event/observer.hpp
//       [](Opened const& o) { open(o.fd); },
//       [](Closed const&)   { close();    }));        // other alternatives ignored
//   bus.emit(Opened{3});                              // visits each observer
//   bus.remove(tok);                                  // ...or unregister by token
//
//   // RAII alternative: the observer is removed when `sub` leaves scope.
//   auto sub = bus.subscribe(/* observer */);
//
// Observers may be stateful: a class with operator()(Event const&) registered by
// std::ref, a reference-capturing handler, or a value-captured `mutable` handler.
//
// Not thread-safe: add/remove/emit from one thread, and an observer must not add
// to or remove from the same Emitter while it is handling an event (that would
// mutate the list being iterated).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ecs::event {

// A broadcast emitter over the variant type Event.
template <class Event>
class Emitter {
public:
    using Observer = std::function<void(Event const&)>;
    // Opaque handle from add(), accepted by remove(). Never reused, so a stale
    // token just removes nothing. 0 is never a live token.
    using Token = std::uint64_t;

    // A move-only RAII handle from subscribe(): it removes its observer when
    // destroyed (or reset / reassigned). It holds a raw Emitter*, so it must NOT
    // outlive the Emitter it came from -- destroying it after the Emitter is gone
    // would call remove() through a dangling pointer. When you need a handle
    // decoupled from Emitter lifetime, use the token-based add()/remove().
    class [[nodiscard(
        "dropping the subscription handle immediately unsubscribes; it must be stored")]]
    Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& o) noexcept
            : emitter_(std::exchange(o.emitter_, nullptr))
            , token_(std::exchange(o.token_, 0)) {}
        Subscription& operator=(Subscription&& o) noexcept {
            if (this != &o) {
                reset();
                emitter_ = std::exchange(o.emitter_, nullptr);
                token_   = std::exchange(o.token_, 0);
            }
            return *this;
        }
        Subscription(Subscription const&)            = delete;
        Subscription& operator=(Subscription const&) = delete;
        ~Subscription() { reset(); }

        // Remove the observer now (idempotent; also run by the destructor).
        void reset() {
            if (emitter_) {
                emitter_->remove(token_);
                emitter_ = nullptr;
                token_   = 0;
            }
        }
        // Relinquish ownership *without* removing; returns the token so the caller
        // can remove() it by hand later. The Subscription becomes empty.
        Token release() noexcept {
            emitter_ = nullptr;
            return std::exchange(token_, 0);
        }
        [[nodiscard]]
        Token token() const noexcept {
            return token_;
        }
        // True while this handle owns a live registration.
        [[nodiscard]]
        explicit operator bool() const noexcept {
            return emitter_ != nullptr;
        }

    private:
        friend Emitter;
        Subscription(Emitter* e, Token t) noexcept
            : emitter_(e)
            , token_(t) {}
        Emitter* emitter_ = nullptr;
        Token token_      = 0;
    };

    // Register an observer; returns a Token to remove() it later. Observers are
    // notified in registration order. An empty observer is ignored (returns 0).
    Token add(Observer obs) {
        if (!obs)
            return 0;
        Token const tok = ++last_;
        observers_.emplace_back(tok, std::move(obs));
        return tok;
    }

    // Register an observer and get an RAII Subscription.
    Subscription subscribe(Observer obs) {
        Token const tok = add(std::move(obs));
        return tok ? Subscription {this, tok} : Subscription {};
    }

    // Remove the observer with this token. Returns true if one was removed.
    bool remove(Token tok) {
        return std::erase_if(observers_,
                             [tok](auto const& e) { return e.first == tok; }) != 0;
    }

    [[nodiscard]]
    std::size_t observer_count() const noexcept {
        return observers_.size();
    }

    // Broadcast one event to every observer, in registration order. `e` may be
    // any alternative of Event (or an Event). With no observers it constructs
    // nothing beyond its argument and returns immediately, so the idle path stays
    // cheap. emit() is const: it does not change the Emitter's own observer list
    // (observers may still mutate their own state -- see observer()).
    template <class E>
    void emit(E&& e) const {
        if (observers_.empty())
            return;
        Event const ev(std::forward<E>(e));
        for (auto const& [tok, obs] : observers_)
            obs(ev);
    }

private:
    std::vector<std::pair<Token, Observer>> observers_;
    Token last_ = 0;
};

} // namespace ecs::event
