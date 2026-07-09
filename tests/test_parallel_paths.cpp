// The concurrent paths the rest of the suite exercises only serially: a real
// multi-lane query split (row counts above the pool's serial threshold),
// command recording from parallel kernels (the sharded buffer's actual use
// case), the nested-dispatch error (a query inside a chunk kernel), and the
// opt-in concurrent-systems run policy.
#include "check.hpp"
#include "ecs/ecs.hpp"
#include "setup.hpp"
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>

using namespace ecs;

struct Position {
    float x = 0, y = 0;
};
struct Velocity {
    float x = 0, y = 0;
};
struct Health {
    int hp = 0;
};

// for_each_chunk across a 4-lane pool with a row count well above the serial
// threshold: every row is written exactly once, i.e. the lanes' chunk slices
// are an exact disjoint cover of every matching archetype's rows.
static void multi_lane_chunk_split_covers_every_row() {
    World w;
    std::size_t const n = 20'000;
    setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < n; ++i) {
            cmd.spawn(Position {}, Velocity {1, 2});
            if (i % 3 == 0) // second matching archetype: {Position, Velocity, Health}
                cmd.spawn(Position {}, Velocity {1, 2}, Health {1});
        }
    });

    WorkerPool pool {4};
    Schedule s;
    s.add("bump", [](Query<Position, const Velocity> q) {
        q.for_each_chunk([](std::span<Entity>, chunk<Position> p,
                            chunk<const Velocity> v) {
            auto px = p.column<0>();
            auto vx = v.column<0>();
            for (std::size_t i = 0; i < px.size(); ++i)
                px[i] += vx[i];
        });
    });
    int const ticks = 5;
    for (int t = 0; t < ticks; ++t)
        s.run(w, pool);

    bool all_exact = true;
    query<const Position>(w).for_each_serial([&](auto& p) {
        if (p.x != float(ticks))
            all_exact = false;
    });
    CHECK(all_exact);
}

// Same property through for_each_parallel (the per-row ergonomic path).
static void multi_lane_for_each_parallel_covers_every_row() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 10'000; ++i)
            cmd.spawn(Health {0});
    });
    WorkerPool pool {4};
    Schedule s;
    s.add("inc", [](Query<Health> q) {
        q.for_each_parallel([](auto& h) { h.hp += 1; });
    });
    s.run(w, pool);
    CHECK(query<const Health>(w).count() == 10'000);
    bool all_one = true;
    query<const Health>(w).for_each_serial([&](auto& h) {
        if (h.hp != 1)
            all_one = false;
    });
    CHECK(all_one);
}

// Commands recorded from INSIDE a parallel chunk kernel: the sharded command
// buffer's raison d'etre. Every lane records spawns and destroys concurrently;
// after the flush the world must reflect exactly the recorded edits.
static void commands_recorded_from_parallel_kernel() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 8'192; ++i)
            cmd.spawn(Health {i});
    });

    WorkerPool pool {4};
    Schedule s;
    s.add("reap_and_seed", [](Query<const Health> q, Commands& cmd) {
        q.for_each_chunk([&](std::span<Entity> ents, chunk<const Health> h) {
            auto hp = h.column<0>();
            for (std::size_t i = 0; i < hp.size(); ++i) {
                if (hp[i] % 2 == 0)
                    cmd.destroy(ents[i]); // recorded on this lane's shard
                else
                    cmd.spawn(Position {float(hp[i]), 0});
            }
        });
    });
    s.run(w, pool);

    // 4096 even-hp entities destroyed; 4096 Positions spawned.
    CHECK(query<const Health>(w).count() == 4'096);
    CHECK(query<const Position>(w).count() == 4'096);
    CHECK(w.size() == 8'192);
}

// A Query iterated inside another query's chunk kernel would dispatch on the
// pool that is already mid-dispatch. That is disallowed: the nested dispatch
// throws (surfacing at the outer join like any kernel exception) instead of
// corrupting the in-flight job slot or deadlocking.
static void nested_query_inside_kernel_throws() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 4'096; ++i)
            cmd.spawn(Position {1, 0});
        for (int i = 0; i < 4'096; ++i)
            cmd.spawn(Health {2});
    });

    WorkerPool pool {4};
    Schedule s;
    // Both queries bind the SAME schedule pool; q2's for_each_chunk inside q's
    // kernel is a nested dispatch on a pool that is already mid-dispatch.
    s.add("nested", [](Query<Position> q, Query<const Health> q2) {
        q.for_each_chunk([&](std::span<Entity>, chunk<Position>) {
            q2.for_each_chunk([](std::span<Entity>, chunk<const Health>) {});
        });
    });
    bool threw = false;
    try {
        s.run(w, pool);
    } catch (std::logic_error const&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(w.size() == 8'192); // the aborted run left the world intact

    // The pool remains fully usable after the rejected nested dispatch.
    std::atomic<int> hits {0};
    pool.parallel_for(10'000, [&](std::size_t b, std::size_t e) {
        hits.fetch_add(int(e - b), std::memory_order_relaxed);
    });
    CHECK(hits.load() == 10'000);
}

// RunPolicy::concurrent_systems: a wave of conflict-free systems fans out
// across lanes. Results must be exactly what sequential execution produces.
static void concurrent_systems_policy_matches_sequential() {
    struct A {
        float v = 0;
    };
    struct B {
        float v = 0;
    };
    struct C {
        float v = 0;
    };

    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 5'000; ++i)
            cmd.spawn(A {}, B {}, C {});
    });

    Schedule s;
    s.add("a", [](Query<A> q) { q.for_each_serial([](auto& a) { a.v += 1; }); });
    s.add("b", [](Query<B> q) { q.for_each_serial([](auto& b) { b.v += 2; }); });
    s.add("c", [](Query<C> q) {
        q.for_each_chunk([](std::span<Entity>, chunk<C> c) {
            for (auto& v : c.column<0>())
                v += 3;
        });
    });
    CHECK(s.level_count() == 1); // all three are conflict-free: one wave

    WorkerPool pool {4};
    int const ticks = 50;
    for (int t = 0; t < ticks; ++t)
        s.run(w, pool, RunPolicy::concurrent_systems);

    bool exact = true;
    query<const A, const B, const C>(w).for_each_serial(
        [&](auto& a, auto& b, auto& c) {
            if (a.v != float(ticks) || b.v != 2.f * ticks || c.v != 3.f * ticks)
                exact = false;
        });
    CHECK(exact);
}

int main() {
    RUN_SUITE(multi_lane_chunk_split_covers_every_row);
    RUN_SUITE(multi_lane_for_each_parallel_covers_every_row);
    RUN_SUITE(commands_recorded_from_parallel_kernel);
    RUN_SUITE(nested_query_inside_kernel_throws);
    RUN_SUITE(concurrent_systems_policy_matches_sequential);
    return REPORT();
}
