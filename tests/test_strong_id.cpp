// tests/test_strong_id.cpp -- the strong-id wrapper (ecs/detail/strong_id.hpp)
// and its use for SystemId: zero-overhead layout, value semantics, hashability,
// and that distinct tags are distinct, non-interconvertible types.

#include "check.hpp"
#include "ecs/detail/strong_id.hpp"
#include "ecs/ecs.hpp"
#include <cstdint>
#include <type_traits>
#include <unordered_map>

using ecs::Schedule;
using ecs::StrongId;
using ecs::SystemId;

using Ticket = StrongId<struct TicketTag, std::uint32_t>;
using Seat   = StrongId<struct SeatTag, std::uint32_t>;

// Same size/layout as the underlying integer and trivially copyable, so passing
// one costs nothing over passing the raw int.
static void zero_overhead_layout() {
    static_assert(sizeof(Ticket) == sizeof(std::uint32_t));
    static_assert(alignof(Ticket) == alignof(std::uint32_t));
    static_assert(std::is_trivially_copyable_v<Ticket>);
    static_assert(std::is_standard_layout_v<Ticket>);
    CHECK(true);
}

// Default is zero; explicit construction round-trips through .value.
static void construct_and_read() {
    CHECK(Ticket {}.value == 0u);
    Ticket const t {42};
    CHECK(t.value == 42u);
    CHECK(static_cast<std::uint32_t>(t) == 42u); // explicit operator
}

// Equality and ordering compare the underlying value.
static void comparable() {
    CHECK(Ticket {7} == Ticket {7});
    CHECK(Ticket {7} != Ticket {8});
    CHECK(Ticket {7} < Ticket {8});
    CHECK((Ticket {8} <=> Ticket {7}) > 0);
}

// Distinct tags are distinct types that never interconvert -- the whole point.
static void tags_are_distinct_types() {
    static_assert(!std::is_same_v<Ticket, Seat>);
    static_assert(!std::is_convertible_v<Ticket, Seat>);
    static_assert(!std::is_convertible_v<Ticket, std::uint32_t>); // no implicit decay
    static_assert(!std::is_convertible_v<std::uint32_t, Ticket>); // no implicit build
    static_assert(std::is_constructible_v<Ticket, std::uint32_t>); // ... explicit only
    CHECK(true);
}

// Works as an unordered_map key with no bespoke hasher (std::hash is provided).
static void hashable_key() {
    std::unordered_map<Ticket, int> m;
    m[Ticket {1}] = 10;
    m[Ticket {2}] = 20;
    m[Ticket {1}] += 5;
    CHECK(m.size() == 2);
    CHECK(m.at(Ticket {1}) == 15);
    CHECK(m.at(Ticket {2}) == 20);
    CHECK(std::hash<Ticket> {}(Ticket {3}) == std::hash<std::uint32_t> {}(3u));
}

// SystemId in practice: the scheduler hands out monotonic, non-zero ids (0 is
// the reserved "no system" sentinel), and they stay usable as map keys.
static void system_ids_are_monotonic_and_nonzero() {
    Schedule s;
    SystemId const a = s.add("a", [] {});
    SystemId const b = s.add("b", [] {});
    SystemId const c = s.add("c", [] {});
    CHECK(a != SystemId {});         // never the 0 sentinel
    CHECK(b != SystemId {});
    CHECK(a != b && b != c && a != c);
    CHECK(a < b);                    // monotonic in registration order
    CHECK(b < c);

    std::unordered_map<SystemId, int> seen;
    for (SystemId id : {a, b, c})
        ++seen[id];
    CHECK(seen.size() == 3);
}

int main() {
    RUN_SUITE(zero_overhead_layout);
    RUN_SUITE(construct_and_read);
    RUN_SUITE(comparable);
    RUN_SUITE(tags_are_distinct_types);
    RUN_SUITE(hashable_key);
    RUN_SUITE(system_ids_are_monotonic_and_nonzero);
    return REPORT();
}
