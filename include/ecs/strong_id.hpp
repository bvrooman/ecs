// ecs/strong_id.hpp
//
// A strongly-typed integer id: StrongId<Tag, Rep> wraps an integer `Rep`
// tagged with a distinct `Tag` type, so ids of different kinds never
// implicitly convert to one another or to a bare integer. This turns a whole
// class of "passed the wrong id" mistakes into compile errors while staying a
// zero-overhead wrapper (trivially copyable, same size/layout as `Rep`).
//
//   using SystemId = StrongId<struct SystemIdTag, std::uint64_t>;
//   SystemId a {1};                  // explicit construction
//   SystemId b {};                   // zero
//   if (a == b) { ... }              // ordered + equality-comparable
//   std::uint64_t raw = a.value;     // explicit read-out of the underlying int
//
// The `Tag` is only ever named in the alias -- an incomplete tag struct is
// enough to make the instantiation distinct, so no tag type needs a definition.
// Construction is explicit and there is no implicit conversion back to `Rep`;
// read `.value` (or cast via the explicit operator) when a raw integer is
// genuinely needed (a map key crossing a layer that stays untyped, a hash mix).

#pragma once

#include <atomic>
#include <compare>
#include <cstddef>
#include <functional>
#include <limits>

namespace ecs {

template <class Tag, class Rep>
struct StrongId {
    // The underlying integer type, exposed so generic code (e.g. an atomic
    // counter that mints ids) can spell it without naming Rep -- `Id::type`.
    using type = Rep;

    Rep value {};

    StrongId() = default;
    explicit constexpr StrongId(Rep v) noexcept
        : value(v) {}

    // Explicit only: a StrongId never silently decays to its underlying integer.
    [[nodiscard]]
    explicit constexpr operator Rep() const noexcept {
        return value;
    }

    // Advance to the next id in sequence. For a monotonic allocator that hands
    // out ids by incrementing a running maximum (`id = ++next`), so the counter
    // can itself be a StrongId rather than a raw integer that has to be wrapped.
    constexpr StrongId& operator++() noexcept {
        ++value;
        return *this;
    }

    constexpr StrongId operator++(int) noexcept {
        auto const before = *this;
        ++value;
        return before;
    }

    // A canonical "none" id, distinct from every id next() hands out (which
    // start at 0 and climb). For fields that mean "no id yet" -- e.g. the query
    // match caches, which must not match any real World on first use.
    static constexpr StrongId none() noexcept {
        return StrongId {std::numeric_limits<Rep>::max()};
    }

    // Mint a fresh id from a monotonic atomic counter (one static counter per
    // StrongId type), starting at 0. Thread-safe with a relaxed fetch_add: id
    // types are first touched on arbitrary threads (the magic-static init of
    // component_id<T>, resource_id<T>, ... each guards only its OWN
    // initialization), so two distinct types minted concurrently must never be
    // handed the same value. Centralizes the counter boilerplate the per-type
    // id minters otherwise repeat.
    //
    // Unique per LINKAGE UNIT, not per process. The counter is a static local
    // in an inline function: one copy after normal linking, but a second copy
    // if the header is compiled into a dlopen'd plugin, or into a shared
    // library built -fvisibility=hidden. Two counters hand out the same id for
    // different component types, which aliases their columns silently. See
    // "Known limitations" in DESIGN.md for the linkage rules that keep it to
    // one copy.
    [[nodiscard]]
    static StrongId next() noexcept {
        static std::atomic<Rep> counter {0};
        return StrongId {counter.fetch_add(1, std::memory_order_relaxed)};
    }

    friend bool operator==(StrongId, StrongId)  = default;
    friend auto operator<=>(StrongId, StrongId) = default;
};

} // namespace ecs

// Usable as an unordered_map key out of the box -- forwards to the underlying
// integer's hash, so tooling that keys observers/tables on an id needs no
// bespoke hasher.
template <class Tag, class Rep>
struct std::hash<ecs::StrongId<Tag, Rep>> {
    std::size_t operator()(ecs::StrongId<Tag, Rep> const id) const noexcept {
        return std::hash<Rep> {}(id.value);
    }
};
