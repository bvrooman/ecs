// tests/test_signature.cpp -- the Signature value type (ecs/archetype.hpp):
// a sorted, de-duplicated ComponentId set that owns the id<->column-slot lookup
// and the add/remove archetype transitions.

#include "check.hpp"
#include "ecs/ecs.hpp"
#include <vector>

using ecs::ColumnId;
using ecs::ComponentId;
using ecs::Signature;

namespace {
struct A {};
struct B {};
struct C {};
} // namespace

// Construction normalizes: sorted and de-duplicated regardless of input order.
static void normalizes_on_construction() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    Signature const s {b, a, a, b}; // out of order, with duplicates
    std::vector<ComponentId> const got(s.begin(), s.end());
    std::vector<ComponentId> want {a, b};
    std::ranges::sort(want);
    CHECK(got == want);
    CHECK(s.size() == 2);
}

// find yields the column slot (position in the sorted set); contains mirrors it.
static void find_gives_column_slot() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    auto const c = ecs::component_id<C>;
    Signature const s {a, b}; // sorted -> positions are 0 and 1 by id order
    auto const fa = s.find(a);
    auto const fb = s.find(b);
    CHECK(fa.has_value());
    CHECK(fb.has_value());
    // The slot indexes back to the same id.
    CHECK(s[*fa] == a);
    CHECK(s[*fb] == b);
    CHECK(s.contains(a));
    CHECK(!s.contains(c));
    CHECK(!s.find(c).has_value());
}

// with/without are the archetype-transition builders; both are idempotent.
static void with_and_without() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    Signature const s {a};
    Signature const sb = s.with(b);
    CHECK(sb.contains(a));
    CHECK(sb.contains(b));
    CHECK(sb.size() == 2);
    CHECK(s.with(a) == s);            // adding a present id is a no-op
    CHECK(sb.without(b) == s);        // remove undoes with
    CHECK(s.without(b) == s);         // removing an absent id is a no-op
}

// includes: superset test over sorted sets (drives archetype/query matching).
static void includes_is_superset() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    auto const c = ecs::component_id<C>;
    Signature const abc {a, b, c};
    Signature const ac {a, c};
    Signature const empty {};
    CHECK(abc.includes(ac));
    CHECK(abc.includes(empty));
    CHECK(!ac.includes(abc));
    CHECK(abc.includes(abc));
}

int main() {
    RUN_SUITE(normalizes_on_construction);
    RUN_SUITE(find_gives_column_slot);
    RUN_SUITE(with_and_without);
    RUN_SUITE(includes_is_superset);
    return REPORT();
}
