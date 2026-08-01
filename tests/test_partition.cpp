// partition_by<K>: a parallel system whose work item is a GROUP of rows rather
// than an arbitrary slice of them. The executor scans the key component for runs
// of equal value, gathers every run of one key into one item, and emits items in
// ascending key order. What has to hold: a group is never split across items, a
// group that spans archetypes is still one item, ordinals stay canonical, and
// the key is visible to conflict analysis.

#include "check.hpp"
#include "setup.hpp"
#include <atomic>
#include <cstddef>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace ecs;

namespace {

struct Key {
    std::uint32_t k;
};
struct Val {
    int v;
};
struct Other {
    int v;
};
struct Tag {};

// Spawn one entity per (key, value) pair, carrying Cs... alongside.
template <class... Cs>
void spawn_keyed(World& world, std::vector<std::pair<std::uint32_t, int>> const& rows) {
    Schedule init;
    init.add_serial(
        "spawn",
        [&rows](Commands& cmd) {
            for (auto const& [k, v] : rows)
                cmd.spawn(Key {k}, Val {v}, Cs {}...);
        },
        times {1});
    init.run(world);
}

// The item shape a run produced: for each item, the set of keys its rows carried
// and the rows it saw. Guarded because items run concurrently.
struct Observed {
    std::mutex mu;
    std::vector<std::set<std::uint32_t>> keys_per_item;
    std::vector<std::vector<int>> vals_per_item;

    void record(std::set<std::uint32_t> k, std::vector<int> v) {
        auto const lock = std::lock_guard {mu};
        keys_per_item.push_back(std::move(k));
        vals_per_item.push_back(std::move(v));
    }
};

// --- one item per key -------------------------------------------------------
// The defining property. Nine rows over three keys is one item per key, whatever
// the lane count -- not the executor's default sizing, which would make all nine
// a single item (they are far below kMinItemRows).
void partition_makes_one_item_per_key() {
    World world;
    spawn_keyed(world,
                {{0, 0}, {0, 1}, {0, 2}, {1, 3}, {1, 4}, {2, 5}, {2, 6}, {2, 7}, {2, 8}});

    Observed obs;
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&obs](Query<Val const, Key const> q) {
            std::set<std::uint32_t> keys;
            std::vector<int> vals;
            q.for_each([&](auto const& v, auto const& k) {
                keys.insert(k.k);
                vals.push_back(v.v);
            });
            obs.record(std::move(keys), std::move(vals));
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(obs.keys_per_item.size() == 3);
    // Every item saw exactly one key -- no group was split, none were merged.
    for (auto const& ks : obs.keys_per_item)
        CHECK(ks.size() == 1);
    // And the group sizes are the ones the data has, not a slicing of it.
    auto sizes = std::vector<std::size_t> {};
    for (auto const& vs : obs.vals_per_item)
        sizes.push_back(vs.size());
    std::ranges::sort(sizes);
    CHECK(sizes == (std::vector<std::size_t> {2, 3, 4}));
}

// A group runs whole on ONE lane -- the reason the option exists. Recorded per
// key rather than per item so the check is about the data, not the item list.
void a_group_never_splits_across_lanes() {
    World world;
    std::vector<std::pair<std::uint32_t, int>> rows;
    for (std::uint32_t k = 0; k < 16; ++k)
        for (int i = 0; i < 40; ++i)
            rows.push_back({k, int(k) * 100 + i});
    spawn_keyed(world, rows);

    std::mutex mu;
    std::map<std::uint32_t, std::set<std::thread::id>> lanes_per_key;
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&](Query<Key const> q) {
            auto const me   = std::this_thread::get_id();
            auto const lock = std::lock_guard {mu};
            q.for_each([&](auto const& k) { lanes_per_key[k.k].insert(me); });
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(lanes_per_key.size() == 16);
    for (auto const& [k, lanes] : lanes_per_key)
        CHECK(lanes.size() == 1);
}

// --- groups that span archetypes --------------------------------------------
// A group is defined by its key value, not by where its rows live: an entity
// that gained a tag moved archetype without leaving its group, so the item must
// carry units from both.
void a_group_spanning_archetypes_is_still_one_item() {
    World world;
    spawn_keyed(world, {{0, 0}, {0, 1}, {1, 2}});         // Key, Val
    spawn_keyed<Tag>(world, {{0, 10}, {1, 11}, {1, 12}}); // Key, Val, Tag

    Observed obs;
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&obs](Query<Val const, Key const> q) {
            std::set<std::uint32_t> keys;
            std::vector<int> vals;
            q.for_each([&](auto const& v, auto const& k) {
                keys.insert(k.k);
                vals.push_back(v.v);
            });
            obs.record(std::move(keys), std::move(vals));
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(obs.keys_per_item.size() == 2); // two keys, not two archetypes x two keys
    for (auto const& ks : obs.keys_per_item)
        CHECK(ks.size() == 1);
    auto sizes = std::vector<std::size_t> {};
    for (auto const& vs : obs.vals_per_item)
        sizes.push_back(vs.size());
    std::ranges::sort(sizes);
    CHECK(sizes == (std::vector<std::size_t> {3, 3})); // {0,1,10} and {2,11,12}
}

// --- unsorted rows ----------------------------------------------------------
// Grouping is by value, so interleaved keys still give one item per key. They
// cost more units inside it, which is a performance property, not a correctness
// one -- this pins the correctness half.
void interleaved_keys_still_group_by_value() {
    World world;
    spawn_keyed(world, {{2, 0}, {0, 1}, {1, 2}, {0, 3}, {2, 4}, {0, 5}});

    Observed obs;
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&obs](Query<Val const, Key const> q) {
            std::set<std::uint32_t> keys;
            std::vector<int> vals;
            q.for_each([&](auto const& v, auto const& k) {
                keys.insert(k.k);
                vals.push_back(v.v);
            });
            obs.record(std::move(keys), std::move(vals));
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(obs.keys_per_item.size() == 3);
    for (auto const& ks : obs.keys_per_item)
        CHECK(ks.size() == 1);
    auto sizes = std::vector<std::size_t> {};
    for (auto const& vs : obs.vals_per_item)
        sizes.push_back(vs.size());
    std::ranges::sort(sizes);
    CHECK(sizes == (std::vector<std::size_t> {1, 2, 3}));
}

// --- canonical order --------------------------------------------------------
// Items are emitted in ascending key order, so a Collect concatenates in that
// order at any lane count -- the same guarantee an ordinary parallel system
// gives for row order.
void items_are_ordered_by_key() {
    World world;
    spawn_keyed(world, {{7, 70}, {3, 30}, {9, 90}, {3, 31}, {1, 10}, {7, 71}});
    world.emplace_resource<std::vector<int>>();

    Schedule sched;
    sched.add_parallel(
        "gather",
        [](Query<Val const> q, Collect<std::vector<int>> out) {
            q.for_each([&](auto const& v) { out->push_back(v.v); });
        },
        partition_by<Key> {});

    for (unsigned lanes : {1u, 2u, 4u}) {
        WorkerPool pool {lanes};
        sched.run(world, pool);
        auto const& got = world.resource<std::vector<int>>();
        CHECK(got == (std::vector<int> {10, 30, 31, 70, 71, 90}));
    }
}

// --- the key is declared -----------------------------------------------------
// A partitioned system reads its key. A system that WRITES the key must
// therefore be ordered against it, even though the body never names Key.
void the_key_is_declared_as_a_read() {
    Schedule sched;
    sched.add_parallel("rekey", [](Query<Key> q) { (void)q; });
    sched.add_parallel(
        "per-group",
        [](Query<Val const> q) { (void)q; },
        partition_by<Key> {});
    CHECK(sched.level_count() == 2);

    // Without the partition the two systems share no component and share a wave.
    Schedule loose;
    loose.add_parallel("rekey", [](Query<Key> q) { (void)q; });
    loose.add_parallel("plain", [](Query<Val const> q) { (void)q; });
    CHECK(loose.level_count() == 1);
}

// --- the key narrows the match ----------------------------------------------
// A row with no key has no group, so an archetype without K is not the
// partitioned system's business -- even when the Query alone would match it.
void rows_without_the_key_are_not_matched() {
    World world;
    spawn_keyed(world, {{0, 1}, {0, 2}});
    setup(world, [](Commands& cmd) {
        cmd.spawn(Val {100}); // no Key
        cmd.spawn(Val {200});
    });

    std::atomic<int> sum {0};
    Schedule sched;
    sched.add_parallel(
        "sum",
        [&sum](Query<Val const> q) { q.for_each([&](auto const& v) { sum += v.v; }); },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(sum.load() == 3); // the keyless rows contributed nothing
}

// --- composition -------------------------------------------------------------
// The partition replaces item SIZING, not the rest of the parallel contract: the
// per-item primitives keep working, indexed by the group's ordinal.
struct IntAdd {
    void operator()(int& into, int& part) const { into += part; }
};

void partition_composes_with_per_item_primitives() {
    World world;
    spawn_keyed(world, {{0, 1}, {0, 2}, {1, 4}, {1, 8}, {2, 16}});
    world.emplace_resource<int>(0);

    Schedule sched;
    sched.add_parallel(
        "fold",
        [](Query<Val const> q, Reduce<int, IntAdd> acc) {
            q.for_each([&](auto const& v) { *acc += v.v; });
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(world.resource<int>() == 31);
}

// An empty world is not a special case: no rows, no runs, no items, no crash.
void an_empty_partition_builds_no_items() {
    World world;
    std::atomic<int> calls {0};
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&calls](Query<Val const> q) {
            (void)q;
            ++calls;
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(calls.load() == 0);
}

// Signed keys order the way the type does, not the way their bit pattern does.
struct SKey {
    std::int32_t k;
};

void signed_keys_order_by_value() {
    World world;
    setup(world, [](Commands& cmd) {
        cmd.spawn(SKey {5}, Val {50});
        cmd.spawn(SKey {-3}, Val {-30});
        cmd.spawn(SKey {0}, Val {0});
        cmd.spawn(SKey {-7}, Val {-70});
    });
    world.emplace_resource<std::vector<int>>();

    Schedule sched;
    sched.add_parallel(
        "gather",
        [](Query<Val const> q, Collect<std::vector<int>> out) {
            q.for_each([&](auto const& v) { out->push_back(v.v); });
        },
        partition_by<SKey> {});
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(world.resource<std::vector<int>>() == (std::vector<int> {-70, -30, 0, 50}));
}

// A group is one item, so it stays whole even when it dwarfs the default grain.
// This is the case the default sizing gets wrong in the other direction: it
// would cut this group into kMinItemRows-sized pieces.
void a_large_group_is_not_split_by_the_default_grain() {
    World world;
    std::vector<std::pair<std::uint32_t, int>> rows;
    for (int i = 0; i < 5000; ++i)
        rows.push_back({0, i}); // one key, well past kMinItemRows (1024)
    spawn_keyed(world, rows);

    std::atomic<int> items {0};
    Schedule sched;
    sched.add_parallel(
        "per-group",
        [&items](Query<Val const> q) {
            (void)q;
            ++items;
        },
        partition_by<Key> {});
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(items.load() == 1);
}

} // namespace

int main() {
    RUN_SUITE(partition_makes_one_item_per_key);
    RUN_SUITE(a_group_never_splits_across_lanes);
    RUN_SUITE(a_group_spanning_archetypes_is_still_one_item);
    RUN_SUITE(interleaved_keys_still_group_by_value);
    RUN_SUITE(items_are_ordered_by_key);
    RUN_SUITE(the_key_is_declared_as_a_read);
    RUN_SUITE(rows_without_the_key_are_not_matched);
    RUN_SUITE(partition_composes_with_per_item_primitives);
    RUN_SUITE(an_empty_partition_builds_no_items);
    RUN_SUITE(signed_keys_order_by_value);
    RUN_SUITE(a_large_group_is_not_split_by_the_default_grain);
    return REPORT();
}
