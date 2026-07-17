// tests/test_signature.cpp -- the Signature value type (ecs/archetype.hpp):
// a sorted, de-duplicated ComponentId set that owns the id<->column-slot lookup
// and the add/remove archetype transitions.

#include "check.hpp"
#include "ecs/ecs.hpp"
#include <unordered_map>
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

// Constructs from any range of ComponentId (not just an init-list); the range
// ctor is constrained off Signature, so copy/move still copy rather than
// re-iterating.
static void constructs_from_a_range() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    std::vector<ComponentId> const v {b, a, b}; // unsorted, dup
    Signature const s(v);                       // range ctor
    CHECK(s == Signature({a, b}));
    CHECK(s.size() == 2);

    Signature const copy = s; // copy ctor, not the range ctor
    CHECK(copy == s);
    CHECK(copy.hash() == s.hash());
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

// Equal signatures (however built) compare equal and hash equal; the cached
// hash tracks with()/without() edits. Usable as an unordered_map key with no
// bespoke hasher.
static void hash_and_equality() {
    auto const a = ecs::component_id<A>;
    auto const b = ecs::component_id<B>;
    Signature const s1 {a, b};
    Signature const s2 {b, a, a}; // same set, different input order + dup
    CHECK(s1 == s2);
    CHECK(s1.hash() == s2.hash());
    CHECK(std::hash<Signature> {}(s1) == s1.hash());

    Signature const just_a {a};
    CHECK(just_a != s1);
    // building up to the same set reproduces the same cached hash
    CHECK(just_a.with(b) == s1);
    CHECK(just_a.with(b).hash() == s1.hash());
    CHECK(s1.without(b) == just_a);
    CHECK(s1.without(b).hash() == just_a.hash());

    std::unordered_map<Signature, int> m; // no explicit hasher
    m[s1] = 7;
    CHECK(m.at(s2) == 7); // s2 finds s1's entry
}

int main() {
    RUN_SUITE(normalizes_on_construction);
    RUN_SUITE(constructs_from_a_range);
    RUN_SUITE(find_gives_column_slot);
    RUN_SUITE(with_and_without);
    RUN_SUITE(includes_is_superset);
    RUN_SUITE(hash_and_equality);
    return REPORT();
}
