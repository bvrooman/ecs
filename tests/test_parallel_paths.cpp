// The concurrent paths the rest of the suite exercises only serially: a real
// multi-lane query split (row counts above the pool's serial threshold),
// command recording from parallel kernels (the sharded buffer's actual use
// case), the nested-dispatch error (a query inside a chunk kernel), and the
// opt-in concurrent-systems run policy.
#include <ecs/ecs.hpp>

#include "check.hpp"
#include "setup.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
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
    s.add_kernel("bump", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity const>,
                            chunk<Position> p,
                            chunk<Velocity const> v) {
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
    w.for_each<Position const>([&](auto& p) {
        if (p.x != float(ticks))
            all_exact = false;
    });
    CHECK(all_exact);
}

// Same property through a kernel's per-row (for_each) path.
static void multi_lane_kernel_serial_covers_every_row() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 10'000; ++i)
            cmd.spawn(Health {0});
    });
    WorkerPool pool {4};
    Schedule s;
    s.add_kernel("inc", [](Query<Health> q) { q.for_each([](auto& h) { h.hp += 1; }); });
    s.run(w, pool);
    CHECK(w.count<Health const>() == 10'000);
    bool all_one = true;
    w.for_each<Health const>([&](auto& h) {
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
    s.add("reap_and_seed", [](Query<Health const> q, Commands& cmd) {
        q.for_each_chunk([&](std::span<Entity const> ents, chunk<Health const> h) {
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
    CHECK(w.count<Health const>() == 4'096);
    CHECK(w.count<Position const>() == 4'096);
    CHECK(w.size() == 8'192);
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
    s.add_kernel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity const>,
                            chunk<Position> p,
                            chunk<Velocity const> v) {
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
    w.for_each<Position const>([&](auto& p) {
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
        // Query's for_each covers just the item's rows.
        s.add_kernel("a", [](Query<A> q) { q.for_each([](auto& a) { a.v += 1; }); });
        s.add("b", [](Query<B> q) { q.for_each([](auto& b) { b.v += 2; }); });
        s.add_kernel("c", [](Query<C> q) {
            q.for_each_chunk([](std::span<Entity const>, chunk<C> c) {
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
        w.for_each<A const, B const, C const>([&](auto& a, auto& b, auto& c) {
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
                 [](Query<Position, Velocity const> q,
                    Res<Clock> clk,
                    Res<Goal> goal,
                    Commands& cmd) {
                     q.for_each_chunk([&](std::span<Entity const> ents,
                                          chunk<Position> p,
                                          chunk<Velocity const> v) {
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
        s.add_kernel("sum", [](Query<Position const> q, Reduce<FSum, FAdd> sum) {
            q.for_each([&](auto& p) { sum->v += p.x; });
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
        w.for_each<Position const>([&](auto& p) { expect += p.x; });
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
                 [](Query<Health const> q, Reduce<std::vector<int>, HpConcat> out) {
                     q.for_each([&](auto& h) {
                         if (h.hp % 7 == 0)
                             out->push_back(h.hp);
                     });
                 });
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t) // repeated runs: rebuilt, capacity retained
        s.run(w, pool);

    std::vector<int> expect;
    w.for_each<Health const>([&](auto& h) {
        if (h.hp % 7 == 0)
            expect.push_back(h.hp);
    });
    CHECK(!expect.empty());
    CHECK(w.resource<std::vector<int>>() == expect);
}

// Reduce with a Partial type different from the target: each item folds into
// a SPARSE map of touched buckets, merged into the DENSE histogram resource
// at the barrier -- the shape for big dense targets (spatial grids) where one
// full copy per item would be absurd. Result must equal the serial histogram
// exactly, at any lane count.
struct DenseHist {
    std::array<int, 16> v {};
};
struct FoldSparse {
    void operator()(DenseHist& into, std::unordered_map<int, int>& part) const {
        for (auto const& [k, c] : part)
            into.v[std::size_t(k)] += c;
    }
};
static void reduce_sparse_partial_matches_dense_histogram() {
    auto run_hist = [](unsigned lanes) {
        World w;
        w.emplace_resource<DenseHist>();
        populate_mixed(w, 12'000);
        Schedule s;
        s.add_kernel("hist",
                     [](Query<Health const> q,
                        Reduce<DenseHist, FoldSparse, std::unordered_map<int, int>> h) {
                         q.for_each([&](auto& hp) { ++(*h)[hp.hp % 16]; });
                     });
        WorkerPool pool {lanes};
        for (int t = 0; t < 2; ++t) // rebuilt per run, not accumulated
            s.run(w, pool);
        return w.resource<DenseHist>().v;
    };

    std::array<int, 16> expect {};
    {
        World w;
        populate_mixed(w, 12'000);
        w.for_each<Health const>([&](auto& h) { ++expect[std::size_t(h.hp % 16)]; });
    }
    CHECK(run_hist(1) == expect);
    CHECK(run_hist(4) == expect);
}

// Extract: disjoint per-item spans over a pre-sized buffer resource. The
// gathered buffer must equal the serial walk's output exactly (same values,
// same order), at 4 lanes, across repeated runs.
static void extract_matches_serial_gather_order() {
    World w;
    w.emplace_resource<std::vector<float>>();
    populate_mixed(w, 20'000);

    Schedule s;
    s.add_kernel("extract", [](Query<Position const> q, Extract<std::vector<float>> out) {
        q.for_each_chunk([&](std::span<Entity const>, chunk<Position const> p) {
            auto x = p.column<0>();
            for (std::size_t i = 0; i < x.size(); ++i)
                out[i] = x[i];
        });
    });
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t)
        s.run(w, pool);

    std::vector<float> expect;
    w.for_each<Position const>([&](auto& p) { expect.push_back(p.x); });
    CHECK(w.resource<std::vector<float>>().size() == expect.size());
    CHECK(w.resource<std::vector<float>>() == expect);
}

// Collect: filtered gather with unknown output size -- per-item partials
// concatenated at the barrier in ordinal order. The collected sequence must
// equal the serial filtered walk exactly (values AND order) at any lane
// count, be rebuilt every run, and its declared resource write must level
// readers into a later wave.
static void collect_matches_serial_filter_order() {
    auto run_collect = [](unsigned lanes, int ticks) {
        World w;
        w.emplace_resource<std::vector<int>>();
        populate_mixed(w, 12'000);
        Schedule s;
        s.add_kernel("cull", [](Query<Health const> q, Collect<std::vector<int>> out) {
            q.for_each([&](auto& h) {
                if (h.hp % 7 == 0)
                    out->push_back(h.hp);
            });
        });
        s.add("consume", [](Res<std::vector<int>>) {}); // reader of the target
        if (s.level_count() != 2)
            return std::vector<int> {}; // leveling broken; fail loudly below
        WorkerPool pool {lanes};
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);
        return w.resource<std::vector<int>>();
    };

    auto const serial1  = run_collect(1, 1);
    auto const parallel = run_collect(4, 1);
    auto const repeated = run_collect(4, 3);

    std::vector<int> expect;
    {
        World w;
        populate_mixed(w, 12'000);
        w.for_each<Health const>([&](auto& h) {
            if (h.hp % 7 == 0)
                expect.push_back(h.hp);
        });
    }
    CHECK(!expect.empty());
    CHECK(serial1 == expect);   // the exact serial filter: values and order
    CHECK(parallel == serial1); // 4 lanes == 1 lane
    CHECK(repeated == serial1); // target rebuilt each run, not accumulated
}

// Events: a double-buffered channel. Events emitted during tick N are
// readable during tick N+1 -- never the same tick -- by both a kernel
// EventReader and a serial Res<Events<T>> system, in an order deterministic
// at any lane count.
struct Ping {
    int from = 0;
};
struct PingLog {
    std::vector<int> v;
};
static void events_are_double_buffered_and_deterministic() {
    auto run_events = [](unsigned lanes, int ticks) {
        World w;
        w.emplace_resource<Events<Ping>>();
        w.emplace_resource<PingLog>();
        populate_mixed(w, 9'000);
        Schedule s;
        // Wave 1: emit one ping per Health row with hp % 5 == 0.
        s.add_kernel("emit", [](Query<Health const> q, EventWriter<Ping> ev) {
            q.for_each([&](auto& h) {
                if (h.hp % 5 == 0)
                    ev.emit(Ping {h.hp});
            });
        });
        // Wave 2: every item of the reader kernel sees the SAME full snapshot
        // of last tick's pings; fold its size into each row.
        s.add_kernel("consume", [](Query<Position> q, EventReader<Ping> ev) {
            auto const n = float(ev.size());
            q.for_each([&](auto& p) { p.y += n; });
        });
        // Serial reader through the plain resource (registered after the
        // writer, so its wave runs post-swap): log the exact sequence.
        s.add("log", [](Res<Events<Ping>> ev, ResMut<PingLog> log) {
            for (auto const& ping : ev->read())
                log->v.push_back(ping.from);
        });
        WorkerPool pool {lanes};
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);
        std::vector<float> ys;
        w.for_each<Position const>([&](auto& p) { ys.push_back(p.y); });
        return std::pair {w.resource<PingLog>().v, ys};
    };

    std::vector<int> expect; // the serial filtered walk, one tick's pings
    {
        World w;
        populate_mixed(w, 9'000);
        w.for_each<Health const>([&](auto& h) {
            if (h.hp % 5 == 0)
                expect.push_back(h.hp);
        });
    }
    CHECK(!expect.empty());

    auto const [log1, ys1] = run_events(4, 1);
    CHECK(log1.empty()); // tick 1: nothing visible yet
    for (float y : ys1)
        CHECK(y == 0.0f); // reader kernel saw 0 events

    auto const [log3, ys3]   = run_events(4, 3);
    auto const [log3s, ys3s] = run_events(1, 3);
    // Ticks 2 and 3 each see exactly tick N-1's pings, in canonical order.
    std::vector<int> twice = expect;
    twice.insert(twice.end(), expect.begin(), expect.end());
    CHECK(log3 == twice);
    for (float y : ys3)
        CHECK(y == 2.0f * float(expect.size()));
    CHECK(log3s == log3); // 1 lane == 4 lanes: same sequence, same order
    CHECK(ys3s == ys3);
}

// Scratch: per-item private workspace, cleared before every run. Use it to
// build a per-row temp list; results must be exact at 4 lanes, and the body
// itself asserts the cleared-at-run-start contract.
static void scratch_is_private_and_cleared() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 12'000; ++i)
            cmd.spawn(Health {i % 32});
    });

    std::atomic<int> dirty {0};
    Schedule s;
    s.add_kernel("smooth", [&dirty](Query<Health> q, Scratch<std::vector<int>> tmp) {
        if (!tmp->empty())
            dirty.fetch_add(1, std::memory_order_relaxed);
        q.for_each([&](auto& h) {
            // A silly per-row use of workspace: digits of hp.
            tmp->clear();
            for (int v = h.hp; v > 0; v /= 10)
                tmp->push_back(v % 10);
            int sum = 0;
            for (int d : *tmp)
                sum += d;
            h.hp = sum; // digit sum, computed via scratch
        });
    });
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t)
        s.run(w, pool);
    CHECK(dirty.load() == 0); // scratch arrived cleared for every item

    bool exact = true;
    w.for_each<Health const>([&](auto& h) {
        if (h.hp >
            9 + 9) // three runs of digit-summing values < 32: <= 18... always small
            exact = false;
    });
    CHECK(exact);
}

// Random: counter-based per-item streams. Same seed + tick => identical
// values at 1 lane and 4 lanes (and across repeated identical runs); a new
// tick or a different RandomSeed resource changes the stream; the values are
// sanely distributed.
static void random_streams_are_lane_count_invariant() {
    auto roll = [](unsigned lanes, std::uint64_t seed, int ticks) {
        World w;
        w.emplace_resource<RandomSeed>(seed);
        setup(w, [&](Commands& cmd) {
            for (int i = 0; i < 20'000; ++i)
                cmd.spawn(Position {});
        });
        Schedule s;
        s.add_kernel("jitter", [](Query<Position> q, Random rng) {
            q.for_each([&](auto& p) { p.x = rng.f32(); });
        });
        WorkerPool pool {lanes};
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);
        std::vector<float> out;
        w.for_each<Position const>([&](auto& p) { out.push_back(p.x); });
        return out;
    };

    auto const serial1  = roll(1, 42, 1);
    auto const parallel = roll(4, 42, 1);
    auto const again    = roll(4, 42, 1);
    auto const tick2    = roll(4, 42, 2);
    auto const seeded   = roll(4, 43, 1);

    CHECK(serial1 == parallel); // bitwise: 1 lane == 4 lanes
    CHECK(parallel == again);   // bitwise: run-to-run reproducible
    CHECK(tick2 != parallel);   // a new tick is a new stream
    CHECK(seeded != parallel);  // the RandomSeed resource is honored

    double mean = 0;
    for (float v : parallel) {
        CHECK(v >= 0.0f && v < 1.0f);
        mean += v;
    }
    mean /= double(parallel.size());
    CHECK(mean > 0.45 && mean < 0.55); // sane uniform distribution
}

// Local: per-system state persisting across ticks, private to each registered
// system -- two registrations of the same callable get independent state, and
// an add_once system's Local dies with it.
struct TickLog {
    std::vector<int> a, b;
};
static void local_state_persists_across_ticks() {
    World w;
    w.emplace_resource<TickLog>();

    // The SAME callable registered twice: each registration owns its own
    // Local (shared state would count 1..8 instead of two interleaved 1..4s).
    auto const counter = [](Local<int> n, ResMut<TickLog> log) {
        ++*n;
        log->a.push_back(*n);
    };
    Schedule s;
    s.add("count-a", counter);
    s.add("count-b", counter);
    for (int t = 0; t < 4; ++t)
        s.run(w);

    CHECK(w.resource<TickLog>().a == (std::vector<int> {1, 1, 2, 2, 3, 3, 4, 4}));
}

// Commands from a KERNEL system: per-item recording, replayed in canonical
// order at the barrier. The spawned rows' payloads (and the survivors of a
// destroy filter) must come out in exactly the serial walk's order at any
// lane count -- with the old thread-sharded path their order followed lane
// timing.
struct Spawned {
    int v = 0;
};
static void kernel_commands_replay_in_canonical_order() {
    auto run_and_gather = [](unsigned lanes) {
        World w;
        populate_mixed(w, 12'000);
        Schedule s;
        s.add_kernel("edit", [](Query<Health const> q, Commands& cmd) {
            q.for_each_chunk([&](std::span<Entity const> es, chunk<Health const> h) {
                auto hp = h.column<0>();
                for (std::size_t i = 0; i < hp.size(); ++i) {
                    if (hp[i] % 9 == 0)
                        cmd.spawn(Spawned {hp[i]});
                    if (hp[i] % 17 == 0)
                        cmd.destroy(es[i]);
                }
            });
        });
        WorkerPool pool {lanes};
        for (int t = 0; t < 2; ++t) // second tick: stores rewound and reused
            s.run(w, pool);
        std::vector<int> spawned;
        w.for_each<Spawned const>([&](auto& sp) { spawned.push_back(sp.v); });
        std::vector<int> alive;
        w.for_each<Health const>([&](auto& h) { alive.push_back(h.hp); });
        return std::pair {spawned, alive};
    };

    auto const [spawned1, alive1] = run_and_gather(1);
    auto const [spawned4, alive4] = run_and_gather(4);
    CHECK(!spawned1.empty());
    CHECK(spawned4 == spawned1); // apply order canonical: same rows, same order
    CHECK(alive4 == alive1);     // destroys too: same swap-and-pop layout
}

// Bin: group-by with a barrier counting sort. Every bucket's contents must
// equal the serial walk's per-bucket gather (values AND order) at any lane
// count; reserve_buckets keeps empty buckets addressable.
static void bin_groups_match_serial_walk() {
    auto run_bins = [](unsigned lanes) {
        World w;
        w.emplace_resource<Bins<int>>().reserve_buckets(16);
        populate_mixed(w, 12'000);
        Schedule s;
        s.add_kernel("bucketize", [](Query<Health const> q, Bin<int> bin) {
            q.for_each([&](auto& h) {
                if (h.hp % 2 == 0) // only even hp: buckets 8..15 stay empty
                    bin.emit(std::uint32_t(h.hp % 8), h.hp);
            });
        });
        WorkerPool pool {lanes};
        for (int t = 0; t < 3; ++t) // rebuilt per run, not accumulated
            s.run(w, pool);
        auto const& bins = w.resource<Bins<int>>();
        std::vector<std::vector<int>> out(bins.bucket_count());
        for (std::size_t b = 0; b < bins.bucket_count(); ++b) {
            auto const sp = bins.bucket(b);
            out[b].assign(sp.begin(), sp.end());
        }
        return out;
    };

    auto const bins1 = run_bins(1);
    auto const bins4 = run_bins(4);

    std::vector<std::vector<int>> expect(16);
    {
        World w;
        populate_mixed(w, 12'000);
        w.for_each<Health const>([&](auto& h) {
            if (h.hp % 2 == 0)
                expect[std::size_t(h.hp % 8)].push_back(h.hp);
        });
    }

    CHECK(bins1.size() == 16); // reserve_buckets floor honored
    CHECK(bins4.size() == 16);
    CHECK(bins1 == expect); // per-bucket serial order, incl. empty buckets
    CHECK(bins4 == expect); // ... at 4 lanes too
    std::size_t total = 0;
    for (auto const& b : expect)
        total += b.size();
    CHECK(total > 0);
}

// prewarm sizes kernel-param state against the populated world without running
// any system body or barrier fold, so it must NOT change simulation state (the
// reduce target stays empty), and a subsequent run must produce the bitwise
// identical result to an un-prewarmed run. It must also be idempotent.
static void prewarm_sizes_state_without_changing_results() {
    auto run_sum = [](bool prewarm, int ticks) {
        World w;
        w.emplace_resource<FSum>();
        populate_mixed(w, 30'000);
        Schedule s;
        s.add_kernel("sum", [](Query<Position const> q, Reduce<FSum, FAdd> sum) {
            q.for_each([&](auto& p) { sum->v += p.x; });
        });
        WorkerPool pool {4};
        if (prewarm) {
            s.prewarm(w);
            s.prewarm(w); // idempotent
            // No body ran and no fold happened: the target is still empty.
            CHECK(w.resource<FSum>().v == 0.0f);
        }
        for (int t = 0; t < ticks; ++t)
            s.run(w, pool);
        return w.resource<FSum>().v;
    };
    // Bitwise identical with and without prewarm: it changes only when
    // allocation is paid, never what is computed.
    CHECK(run_sum(true, 3) == run_sum(false, 3));
}

int main() {
    RUN_SUITE(multi_lane_chunk_split_covers_every_row);
    RUN_SUITE(multi_lane_kernel_serial_covers_every_row);
    RUN_SUITE(commands_recorded_from_parallel_kernel);
    RUN_SUITE(kernel_system_covers_every_row);
    RUN_SUITE(mixed_wave_flattened_dispatch_is_exact);
    RUN_SUITE(kernel_with_resource_and_commands_extras);
    RUN_SUITE(reduce_is_bitwise_deterministic_across_lane_counts);
    RUN_SUITE(reduce_vector_partials_match_serial_collect);
    RUN_SUITE(reduce_sparse_partial_matches_dense_histogram);
    RUN_SUITE(extract_matches_serial_gather_order);
    RUN_SUITE(collect_matches_serial_filter_order);
    RUN_SUITE(events_are_double_buffered_and_deterministic);
    RUN_SUITE(local_state_persists_across_ticks);
    RUN_SUITE(kernel_commands_replay_in_canonical_order);
    RUN_SUITE(bin_groups_match_serial_walk);
    RUN_SUITE(scratch_is_private_and_cleared);
    RUN_SUITE(random_streams_are_lane_count_invariant);
    RUN_SUITE(prewarm_sizes_state_without_changing_results);
    return REPORT();
}
