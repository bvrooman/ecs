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

// A kernel system's rows are sliced into work items and claimed across lanes;
// every row of every matching archetype must be visited exactly once per tick.
static void kernel_system_covers_every_row() {
    World w;
    std::size_t const n = 20'000;
    setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < n; ++i) {
            cmd.spawn(Position {}, Velocity {1, 2});
            if (i % 3 == 0) // second matching archetype
                cmd.spawn(Position {}, Velocity {1, 2}, Health {1});
        }
    });

    Schedule s;
    // Same signature rules as add(): the one Query parameter is what the
    // executor slices; the body iterates just its item's rows.
    s.add_kernel("integrate", [](Query<Position, const Velocity> q) {
        q.for_each_chunk([](std::span<Entity>, chunk<Position> p,
                            chunk<const Velocity> v) {
            auto px = p.column<0>();
            auto vx = v.column<0>();
            for (std::size_t i = 0; i < px.size(); ++i)
                px[i] += vx[i];
        });
    });

    WorkerPool pool {4};
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

// A mixed wave -- kernel systems, imperative systems, and a command recorder,
// all conflict-free -- flattens into one item list; results must be exactly
// what sequential execution would produce, on both a 1-lane and a 4-lane pool.
static void mixed_wave_flattened_dispatch_is_exact() {
    struct A {
        float v = 0;
    };
    struct B {
        float v = 0;
    };
    struct C {
        float v = 0;
    };

    for (unsigned lanes : {1u, 4u}) {
        World w;
        setup(w, [&](Commands& cmd) {
            for (int i = 0; i < 5'000; ++i)
                cmd.spawn(A {}, B {}, C {});
        });

        Schedule s;
        // Kernel bodies may use the per-row ergonomic path too -- the sliced
        // Query's for_each_serial covers just the item's rows.
        s.add_kernel("a", [](Query<A> q) {
            q.for_each_serial([](auto& a) { a.v += 1; });
        });
        s.add("b", [](Query<B> q) { q.for_each_serial([](auto& b) { b.v += 2; }); });
        s.add_kernel("c", [](Query<C> q) {
            q.for_each_chunk([](std::span<Entity>, chunk<C> c) {
                for (auto& v : c.column<0>())
                    v += 3;
            });
        });
        s.add("spawner", [](Commands& cmd) { cmd.spawn(B {100}); });
        CHECK(s.level_count() == 1); // all conflict-free: one wave, one dispatch

        WorkerPool pool {lanes};
        int const ticks = 20;
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);

        bool exact = true;
        query<const A, const B, const C>(w).for_each_serial(
            [&](auto& a, auto& b, auto& c) {
                if (a.v != float(ticks) || b.v != 2.f * ticks || c.v != 3.f * ticks)
                    exact = false;
            });
        CHECK(exact);
        CHECK(w.size() == 5'000u + std::size_t(ticks)); // spawner ran every tick
    }
}

// Kernel systems may take trailing system parameters (Res/ResMut/Commands&/
// WorldView), bound per item -- the mist "steer" shape: components + resources.
static void kernel_with_resource_and_commands_extras() {
    struct Clock {
        float dt = 0;
    };
    struct Goal {
        float x = 0;
    };

    World w;
    w.emplace_resource<Clock>(0.5f);
    w.emplace_resource<Goal>(10.f);
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 12'000; ++i)
            cmd.spawn(Position {}, Velocity {2, 0});
    });

    Schedule s;
    s.add_kernel("steer",
                 [](Query<Position, const Velocity> q, Res<Clock> clk,
                    Res<Goal> goal, Commands& cmd) {
                     q.for_each_chunk([&](std::span<Entity> ents, chunk<Position> p,
                                          chunk<const Velocity> v) {
                         auto px = p.column<0>();
                         auto vx = v.column<0>();
                         for (std::size_t i = 0; i < px.size(); ++i) {
                             px[i] += vx[i] * clk->dt;
                             if (px[i] > goal->x)
                                 cmd.destroy(ents[i]); // sharded: safe per item
                         }
                     });
                 });
    // The Res reads must show in the derived access: a Clock WRITER conflicts.
    s.add("tick", [](ResMut<Clock>) {});
    CHECK(s.level_count() == 2);

    WorkerPool pool {4};
    for (int t = 0; t < 12; ++t) // 12 ticks x 2*0.5 = +12 > 10: all destroyed
        s.run(w, pool);
    CHECK(w.size() == 0);
}

// --- the parallel-primitives slot substrate: Reduce / Extract ---------------

struct FSum {
    float v = 0;
};
struct FAdd {
    void operator()(FSum& into, FSum& partial) const { into.v += partial.v; }
};

// Populate two archetypes with a value pattern whose float sum is order-
// sensitive, so any change in partial boundaries or fold order would change
// the bits of the result.
static void populate_mixed(World& w, int n) {
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < n; ++i) {
            float const x = 1.0f + float(i % 1013) * 0.03125f;
            cmd.spawn(Position {x, 0});
            if (i % 3 == 0)
                cmd.spawn(Position {x * 0.5f, 0}, Health {i});
        }
    });
}

// Reduce: per-item partials folded at the barrier in canonical order. The
// result must be BITWISE identical at 1 lane and 4 lanes (lane-count-
// independent slicing + ordinal-order fold), the target must be rebuilt each
// run (no accumulation across ticks), and the reduce's declared resource
// write must level readers into a later wave.
static void reduce_is_bitwise_deterministic_across_lane_counts() {
    auto run_sum = [](unsigned lanes, int ticks) {
        World w;
        w.emplace_resource<FSum>();
        populate_mixed(w, 30'000);
        Schedule s;
        s.add_kernel("sum", [](Query<const Position> q, Reduce<FSum, FAdd> sum) {
            q.for_each_serial([&](auto& p) { sum->v += p.x; });
        });
        s.add("read", [](Res<FSum>) {}); // reader of the reduce target
        if (s.level_count() != 2)
            return -1.0f; // leveling broken; fail loudly below
        WorkerPool pool {lanes};
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);
        return w.resource<FSum>().v;
    };

    float const serial1  = run_sum(1, 1);
    float const parallel = run_sum(4, 1);
    float const repeated = run_sum(4, 5); // rebuilt per run: same value after 5

    // Reference: a double-precision serial walk. NOT bitwise-comparable to the
    // sliced float fold -- per-item partials change the float summation TREE
    // (association), which is inherent to any partitioned reduction (and both
    // float results drift ~sqrt(n)*eps from the true sum). The guarantee under
    // test is that the tree itself is fixed: same slicing and same fold order
    // at every lane count and every run.
    double expect = 0;
    {
        World w;
        populate_mixed(w, 30'000);
        query<const Position>(w).for_each_serial([&](auto& p) { expect += p.x; });
    }
    double const tol = expect * 1e-3;
    CHECK(double(serial1) > expect - tol && double(serial1) < expect + tol);
    CHECK(parallel == serial1); // 4 lanes == 1 lane, BITWISE
    CHECK(repeated == serial1); // target rebuilt each run, not accumulated
}

// Reduce with vector partials (the clear()-keeps-capacity reset path and a
// moving/merging Op): a filtered collect must match the serial filtered walk
// exactly, including order.
struct HpConcat {
    void operator()(std::vector<int>& into, std::vector<int>& partial) const {
        into.insert(into.end(), partial.begin(), partial.end());
        partial.clear();
    }
};
static void reduce_vector_partials_match_serial_collect() {
    World w;
    w.emplace_resource<std::vector<int>>();
    populate_mixed(w, 12'000);

    Schedule s;
    s.add_kernel("collect",
                 [](Query<const Health> q, Reduce<std::vector<int>, HpConcat> out) {
                     q.for_each_serial([&](auto& h) {
                         if (h.hp % 7 == 0)
                             out->push_back(h.hp);
                     });
                 });
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t) // repeated runs: rebuilt, capacity retained
        s.run(w, pool);

    std::vector<int> expect;
    query<const Health>(w).for_each_serial([&](auto& h) {
        if (h.hp % 7 == 0)
            expect.push_back(h.hp);
    });
    CHECK(!expect.empty());
    CHECK(w.resource<std::vector<int>>() == expect);
}

// Extract: disjoint per-item spans over a pre-sized buffer resource. The
// gathered buffer must equal the serial walk's output exactly (same values,
// same order), at 4 lanes, across repeated runs.
static void extract_matches_serial_gather_order() {
    World w;
    w.emplace_resource<std::vector<float>>();
    populate_mixed(w, 20'000);

    Schedule s;
    s.add_kernel("extract",
                 [](Query<const Position> q, Extract<std::vector<float>> out) {
                     q.for_each_chunk([&](std::span<Entity>, chunk<const Position> p) {
                         auto x = p.column<0>();
                         for (std::size_t i = 0; i < x.size(); ++i)
                             out[i] = x[i];
                     });
                 });
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t)
        s.run(w, pool);

    std::vector<float> expect;
    query<const Position>(w).for_each_serial([&](auto& p) { expect.push_back(p.x); });
    CHECK(w.resource<std::vector<float>>().size() == expect.size());
    CHECK(w.resource<std::vector<float>>() == expect);
}

int main() {
    RUN_SUITE(multi_lane_chunk_split_covers_every_row);
    RUN_SUITE(multi_lane_for_each_parallel_covers_every_row);
    RUN_SUITE(commands_recorded_from_parallel_kernel);
    RUN_SUITE(nested_query_inside_kernel_throws);
    RUN_SUITE(kernel_system_covers_every_row);
    RUN_SUITE(mixed_wave_flattened_dispatch_is_exact);
    RUN_SUITE(kernel_with_resource_and_commands_extras);
    RUN_SUITE(reduce_is_bitwise_deterministic_across_lane_counts);
    RUN_SUITE(reduce_vector_partials_match_serial_collect);
    RUN_SUITE(extract_matches_serial_gather_order);
    return REPORT();
}
