// tests/test_strong_id.cpp -- the strong-id wrapper (ecs/strong_id.hpp)
// and its use for SystemId: zero-overhead layout, value semantics, hashability,
// and that distinct tags are distinct, non-interconvertible types.

#include <ecs/detail/container/id_vector.hpp>
#include <ecs/ecs.hpp>
#include <ecs/strong_id.hpp>

#include "check.hpp"
#include <cstdint>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

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
    static_assert(std::is_same_v<Ticket::type, std::uint32_t>); // underlying rep
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

// Pre-increment advances the id in place -- the monotonic allocator's `++next`.
static void pre_increment() {
    Ticket t {};
    CHECK((++t) == Ticket {1}); // returns the new value
    CHECK(t == Ticket {1});     // and advanced in place
    CHECK((++t) == Ticket {2});
}

// Post-increment yields the value from BEFORE the advance, so a loop can number
// a sequence in order without the counter ever widening to a bare integer.
static void post_increment() {
    Ticket t {};
    CHECK((t++) == Ticket {0}); // returns the old value
    CHECK(t == Ticket {1});     // and advanced in place
    CHECK((t++) == Ticket {1});
    CHECK(t == Ticket {2});

    // The numbering loop it exists for. Note `Ticket i {}`, not `Ticket i = 0`:
    // construction stays explicit.
    std::vector<Ticket> numbered(3);
    for (auto i = Ticket {}; auto& slot : numbered)
        slot = i++;
    CHECK(numbered[0] == Ticket {0});
    CHECK(numbered[1] == Ticket {1});
    CHECK(numbered[2] == Ticket {2});

    // Post-increment is constexpr, like the rest of the wrapper.
    static_assert([] {
        Ticket c {7};
        return (c++).value == 7 && c.value == 8;
    }());
}

// Distinct tags are distinct types that never interconvert -- the whole point.
static void tags_are_distinct_types() {
    static_assert(!std::is_same_v<Ticket, Seat>);
    static_assert(!std::is_convertible_v<Ticket, Seat>);
    static_assert(!std::is_convertible_v<Ticket, std::uint32_t>);  // no implicit decay
    static_assert(!std::is_convertible_v<std::uint32_t, Ticket>);  // no implicit build
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

// SystemId in practice: the scheduler hands out monotonic ids.
static void system_ids_are_monotonic_and_nonzero() {
    Schedule s;
    SystemId const a = s.add_serial("a", [] {});
    SystemId const b = s.add_serial("b", [] {});
    SystemId const c = s.add_serial("c", [] {});
    CHECK(a != SystemId::none()); // never the 0 sentinel
    CHECK(b != SystemId::none());
    CHECK(a != b && b != c && a != c);
    CHECK(a < b); // monotonic in registration order
    CHECK(b < c);

    std::unordered_map<SystemId, int> seen;
    for (SystemId id : {a, b, c})
        ++seen[id];
    CHECK(seen.size() == 3);
}

// Every core id space is a distinct, non-interconvertible type -- SystemId,
// ComponentId, ResourceId, and ArchetypeId can never be crossed even though
// several share std::uint32_t as their underlying rep.
static void id_spaces_are_distinct() {
    static_assert(!std::is_same_v<ecs::ComponentId, ecs::ResourceId>);
    static_assert(!std::is_same_v<ecs::ComponentId, ecs::ArchetypeId>);
    static_assert(!std::is_same_v<ecs::ResourceId, ecs::ArchetypeId>);
    static_assert(!std::is_same_v<ecs::SystemId, ecs::ArchetypeId>);
    static_assert(!std::is_same_v<ecs::ComponentId, ecs::ColumnId>);
    static_assert(!std::is_same_v<ecs::ArchetypeId, ecs::ColumnId>);
    static_assert(!std::is_convertible_v<ecs::ArchetypeId, ecs::ComponentId>);
    static_assert(!std::is_convertible_v<ecs::ArchetypeId, std::uint32_t>);
    static_assert(!std::is_convertible_v<ecs::ColumnId, ecs::ComponentId>);
    CHECK(true);
}

// ComponentId and ResourceId are separate id spaces: distinct types that a
// component and a resource may share a numeric value across without crossing.
static void component_and_resource_ids() {
    static_assert(!std::is_same_v<ecs::ComponentId, ecs::ResourceId>);
    static_assert(!std::is_convertible_v<ecs::ComponentId, ecs::ResourceId>);

    struct A {};
    struct B {};
    // Stable per type, distinct across types.
    CHECK(ecs::component_id<A> == ecs::component_id<A>);
    CHECK(ecs::component_id<A> != ecs::component_id<B>);
    CHECK(ecs::resource_id<A> == ecs::resource_id<A>);
    CHECK(ecs::resource_id<A> != ecs::resource_id<B>);

    // Ids are handed out from 0 up in each space, so a component and a resource
    // can share a numeric value -- but the types keep them apart.
    std::unordered_map<ecs::ComponentId, int> comp;
    std::unordered_map<ecs::ResourceId, int> res;
    comp[ecs::component_id<A>] = 1;
    res[ecs::resource_id<A>]   = 2;
    CHECK(comp.at(ecs::component_id<A>) == 1);
    CHECK(res.at(ecs::resource_id<A>) == 2);
}

// IdVector: indexed only by its id type, and owns id assignment (push returns
// the appended element's id == its position).
static void id_vector_basics() {
    ecs::detail::IdVector<Ticket, std::string> v;
    CHECK(v.empty());
    Ticket const a = v.push_back("a");
    Ticket const b = v.push_back("b");
    CHECK(a == Ticket {0}); // ids are positions, assigned on append
    CHECK(b == Ticket {1});
    CHECK(v.size() == 2);
    CHECK(v[a] == "a"); // operator[] takes the id, not a raw int
    CHECK(v[b] == "b");
    v[a] = "A";
    CHECK(v[a] == "A");
    // range-for yields the elements
    std::string joined;
    for (auto const& s : v)
        joined += s;
    CHECK(joined == "Ab");
    v.pop_back();
    CHECK(v.size() == 1);
}

static void id_vector_enumerate() {
    using ecs::detail::IdVector;
    using TicketVector = IdVector<Ticket, std::string>;
    auto v             = TicketVector {};
    auto const a       = v.push_back("a");
    auto const b       = v.push_back("b");
    auto const c       = v.push_back("c");
    auto const tickets = std::vector {a, b, c};
    auto enumerated    = std::vector<Ticket> {};
    for (auto&& [id, s] : v.enumerate()) {
        static_assert(std::is_same_v<Ticket, decltype(id)>);
        enumerated.push_back(id);
    }
    CHECK(enumerated == tickets);
}

int main() {
    RUN_SUITE(zero_overhead_layout);
    RUN_SUITE(id_vector_basics);
    RUN_SUITE(id_vector_enumerate);
    RUN_SUITE(id_spaces_are_distinct);
    RUN_SUITE(component_and_resource_ids);
    RUN_SUITE(construct_and_read);
    RUN_SUITE(comparable);
    RUN_SUITE(pre_increment);
    RUN_SUITE(post_increment);
    RUN_SUITE(tags_are_distinct_types);
    RUN_SUITE(hashable_key);
    RUN_SUITE(system_ids_are_monotonic_and_nonzero);
    return REPORT();
}
