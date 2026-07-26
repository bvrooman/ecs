// World::sort_rows / Archetype::permute_rows: the data-layout maintenance
// operation behind key-coherent work items (docs/IMPROVEMENTS.md, parallel-
// primitives section). Rows are reordered; entity handles must not notice,
// iteration must come out in key order, the sort must be stable, and a
// schedule run over the sorted layout must stay lane-count-invariant.
#include <ecs/dynamic/dynamic_column.hpp>
#include <ecs/ecs.hpp>

#include "check.hpp"
#include "setup.hpp"
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace ecs;

struct Position {
    float x = 0, y = 0;
};
struct Health {
    int hp = 0;
};
struct Frozen {}; // tag: exercises the zero-field column through permutation

// Deterministic value scramble (an LCG step) so spawn order is uncorrelated
// with key order.
static float scrambled(int const i) {
    return float((std::uint32_t(i) * 1664525u + 1013904223u) % 1000u);
}

// Rows come out in key order per archetype, every entity handle still reads
// its own component values, and archetypes without the key component are
// untouched.
static void sort_orders_rows_and_preserves_handles() {
    World w;
    std::vector<Entity> es;
    std::vector<Entity> plain; // Health-only archetype: no Position, untouched
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 1'000; ++i) {
            float const x = scrambled(i);
            if (i % 3 == 0)
                es.push_back(cmd.spawn(Position {x, -x}, Health {i}, Frozen {}));
            else
                es.push_back(cmd.spawn(Position {x, -x}, Health {i}));
            if (i % 5 == 0)
                plain.push_back(cmd.spawn(Health {10'000 + i}));
        }
    });

    w.sort_rows<Position>([](Position const& p) { return p.x; });

    // Handles survive: each entity still reads its own values.
    bool handles_ok = true;
    for (std::size_t i = 0; i < es.size(); ++i) {
        auto const p = w.get<Position>(es[i]);
        if (p.x != scrambled(int(i)) || p.y != -p.x || w.get<Health>(es[i]).hp != int(i))
            handles_ok = false;
    }
    for (std::size_t i = 0; i < plain.size(); ++i)
        if (w.get<Health>(plain[i]).hp != 10'000 + int(i) * 5)
            handles_ok = false;
    CHECK(handles_ok);

    // Ascending within each archetype: walk the two Position archetypes via
    // their distinguishing signatures.
    auto ascending = [](World& w) {
        float prev = -1.0f;
        bool ok    = true;
        // Three fixed params (not `auto const&...`): with a variadic tail the
        // row/Entity arity check in for_each_row would bind p to the Entity.
        w.for_each<Position const, Health const, Frozen const>([&](auto const& p,
                                                                   auto const&,
                                                                   auto const&) {
            if (p.x < prev)
                ok = false;
            prev = p.x;
        });
        return ok;
    };
    CHECK(ascending(w));
    CHECK((w.count<Position const, Health const>() == 1'000));
}

// Stability: equal keys keep their original (spawn) relative order, so the
// permutation -- and everything downstream of it -- is canonical.
static void sort_is_stable() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 400; ++i)
            cmd.spawn(Position {float(i % 4), 0}, Health {i});
    });

    w.sort_rows<Position>([](Position const& p) { return p.x; });

    float prev_key = -1.0f;
    int prev_hp    = -1;
    bool ok        = true;
    w.for_each<Position const, Health const>([&](auto const& p, auto const& h) {
        if (p.x < prev_key)
            ok = false; // keys ascend
        if (p.x == prev_key && h.hp <= prev_hp)
            ok = false; // within a key: spawn order preserved
        if (p.x != prev_key)
            prev_hp = -1;
        prev_key = p.x;
        if (h.hp <= prev_hp)
            ok = false;
        prev_hp = h.hp;
    });
    CHECK(ok);
}

// Sorting after swap-and-pop churn (the state a periodic re-sort repairs):
// holes were filled from the tail, then the sort restores key order.
static void sort_after_churn() {
    World w;
    std::vector<Entity> es;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 600; ++i)
            es.push_back(cmd.spawn(Position {scrambled(i), 0}, Health {i}));
    });
    setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < es.size(); i += 3)
            cmd.destroy(es[i]);
    });
    CHECK(w.size() == 400);

    w.sort_rows<Position>([](Position const& p) { return p.x; });

    float prev = -1.0f;
    bool ok    = true;
    int seen   = 0;
    w.for_each<Position const, Health const>([&](auto const& p, auto const& h) {
        if (p.x < prev)
            ok = false;
        prev = p.x;
        if (h.hp % 3 == 0)
            ok = false; // destroyed rows must be gone
        ++seen;
    });
    CHECK(ok);
    CHECK(seen == 400);
    // Survivors still read their own values through their handles.
    for (std::size_t i = 0; i < es.size(); ++i)
        if (i % 3 != 0)
            CHECK(w.get<Health>(es[i]).hp == int(i));
}

// Integral keys take the counting-sort path (compact range) or fall back to
// the comparison sort (spread range); both must be stable and produce the
// same canonical order. Negative keys exercise the offset-by-min bucket math.
static void integral_keys_sort_correctly_on_both_paths() {
    auto check_sorted_stable = [](auto key) {
        World w;
        setup(w, [&](Commands& cmd) {
            for (int i = 0; i < 500; ++i)
                cmd.spawn(Position {scrambled(i), 0}, Health {i});
        });
        w.sort_rows<Position>(key);
        auto prev_key = key(Position {});
        int prev_hp   = -1;
        bool first    = true;
        bool ok       = true;
        w.for_each<Position const, Health const>([&](auto const& p, auto const& h) {
            auto const k = key(p);
            if (!first) {
                if (k < prev_key)
                    ok = false; // keys ascend
                if (k == prev_key && h.hp <= prev_hp)
                    ok = false; // stability within a key
            }
            first    = false;
            prev_key = k;
            prev_hp  = h.hp;
        });
        CHECK(ok);
    };
    // Compact range incl. negatives (range 41 << 4n): counting path.
    check_sorted_stable([](auto const& p) { return int(p.x) % 21 - 10; });
    // Spread range (steps of 1e6 >> 4n): stable_sort fallback.
    check_sorted_stable([](auto const& p) { return int(p.x) * 1'000'000; });
}

// min_disorder: small drift is left alone (a re-sort would not pay for
// itself), real disorder still sorts.
static void min_disorder_skips_small_drift() {
    World w;
    std::vector<Entity> es;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 1'000; ++i)
            es.push_back(cmd.spawn(Position {scrambled(i), 0}, Health {i}));
    });
    auto const key = [](Position const& p) { return int(p.x); };
    w.sort_rows<Position>(key);

    // Perturb slightly: destroying a few rows swap-and-pops tail rows into
    // the holes -- a handful of descents out of ~1000 pairs.
    setup(w, [&](Commands& cmd) {
        for (std::size_t i = 100; i < 106; ++i)
            cmd.destroy(es[i]);
    });
    auto descents = [&] {
        int prev = -1, d = 0;
        w.for_each<Position const>([&](auto const& p) {
            if (int(p.x) < prev)
                ++d;
            prev = int(p.x);
        });
        return d;
    };
    int const drift = descents();
    CHECK(drift > 0); // the perturbation actually disordered something

    w.sort_rows<Position>(key, 0.05); // ~6 descents / 993 pairs < 5%: skipped
    CHECK(descents() == drift);       // untouched

    w.sort_rows<Position>(key); // default threshold: sorts
    CHECK(descents() == 0);
}

// A per-N-tick system (Schedule `every`) runs only on ticks where
// tick % every == 0, and Commands::sort defers World::sort_rows to the barrier
// so a plain phase<-1> system can re-sort the layout -- shown by sorting from
// such a system while kernels run every tick, with churn re-disordering rows in
// between; the schedule's results stay bitwise lane-count-invariant.
static void every_cadence_fires_and_sort_command_stays_invariant() {
    int fired = 0;
    {
        World w;
        Schedule s;
        w.emplace_resource<int>(0);
        s.add_serial("count", [&](Res<int>) { ++fired; }, phase<0> {}, /*every=*/3);
        for (int t = 0; t < 9; ++t)
            s.run(w);
    }
    CHECK(fired == 3); // ticks 3, 6, 9

    auto run = [](unsigned lanes) {
        World w;
        w.emplace_resource<std::vector<float>>();
        setup(w, [&](Commands& cmd) {
            for (int i = 0; i < 10'000; ++i)
                cmd.spawn(Position {scrambled(i), 0}, Health {i});
        });
        Schedule s;
        // Re-sort by cell every 4 ticks: a phase<-1> system recording a sort
        // command, applied at that phase's (world-quiescent) barrier.
        s.add_serial(
            "sort",
            [](Commands& c) {
                c.sort<Position>([](Position const& p) { return int(p.x); });
            },
            phase<-1> {},
            /*every=*/4);
        // Churn: cull a band each tick (swap-and-pop disorders the tail)...
        s.add_parallel("cull", [](Query<Health const> q, Commands& cmd) {
            q.for_each_chunk([&](std::span<Entity const> es, chunk<Health const> h) {
                auto const hp = h.column<0>();
                for (std::size_t i = 0; i < hp.size(); ++i)
                    if (hp[i] % 97 == 3)
                        cmd.destroy(es[i]);
            });
        });
        // ...and gather the walk order every tick.
        s.add_parallel("gather",
                       [](Query<Position const> q, Extract<std::vector<float>> out) {
                           q.for_each_chunk([&](std::span<Entity const>,
                                                chunk<Position const> p) {
                               auto const x = p.column<0>();
                               for (std::size_t i = 0; i < x.size(); ++i)
                                   out[i] = x[i];
                           });
                       });
        WorkerPool pool {lanes};
        std::vector<float> log;
        for (int t = 0; t < 12; ++t) {
            s.run(w, pool);
            auto const& snap = w.resource<std::vector<float>>();
            log.insert(log.end(), snap.begin(), snap.end());
        }
        return log;
    };
    auto const serial   = run(1);
    auto const parallel = run(4);
    CHECK(!serial.empty());
    CHECK(serial == parallel); // maintenance + churn + kernels: still bitwise
}

// The point of sorting: schedule results over the sorted layout remain
// bitwise identical at 1 lane and 4 lanes (the permutation is canonical, so
// the determinism guarantee composes with it).
static void sorted_layout_stays_lane_count_invariant() {
    auto run = [](unsigned lanes) {
        World w;
        w.emplace_resource<std::vector<float>>();
        setup(w, [&](Commands& cmd) {
            for (int i = 0; i < 20'000; ++i)
                cmd.spawn(Position {scrambled(i), float(i)});
        });
        w.sort_rows<Position>([](Position const& p) { return p.x; });
        Schedule s;
        s.add_parallel("gather",
                       [](Query<Position const> q, Extract<std::vector<float>> out) {
                           q.for_each_chunk([&](std::span<Entity const>,
                                                chunk<Position const> p) {
                               auto const x = p.column<0>();
                               for (std::size_t i = 0; i < x.size(); ++i)
                                   out[i] = x[i];
                           });
                       });
        WorkerPool pool {lanes};
        s.run(w, pool);
        return w.resource<std::vector<float>>();
    };
    auto const serial   = run(1);
    auto const parallel = run(4);
    CHECK(!serial.empty());
    CHECK(serial == parallel);             // bitwise: 1 lane == 4 lanes
    CHECK(std::ranges::is_sorted(serial)); // and the walk sees key order
}

// DynamicColumn implements the same permutation (a JS-defined component's
// rows move with the archetype like a native column's).
static void dynamic_column_applies_permutation() {
    using namespace ecs::dynamic;
    ComponentDesc desc;
    desc.name   = "blob";
    desc.fields = {Field {"a", FieldType::f32, 4, 0}, Field {"b", FieldType::i32, 4, 4}};
    desc.stride = 8;

    DynamicColumn col(desc);
    for (int i = 0; i < 5; ++i) {
        float const a        = float(i) * 1.5f;
        std::int32_t const b = 100 + i;
        std::byte blob[8];
        std::memcpy(blob, &a, 4);
        std::memcpy(blob + 4, &b, 4);
        col.push(blob);
    }
    std::uint32_t const perm[] = {4, 2, 0, 3, 1}; // new row i takes old perm[i]
    col.apply_permutation(perm);

    bool ok = true;
    for (std::size_t i = 0; i < 5; ++i) {
        std::byte blob[8];
        col.gather(i, blob);
        float a;
        std::int32_t b;
        std::memcpy(&a, blob, 4);
        std::memcpy(&b, blob + 4, 4);
        if (a != float(perm[i]) * 1.5f || b != 100 + std::int32_t(perm[i]))
            ok = false;
    }
    CHECK(ok);
    CHECK(col.size() == 5);
}

// Commands::sort defers World::sort_rows to the barrier, so a system can request
// the permute even though sort_rows itself is structural. The layout is sorted
// after the wave's flush, exactly as a direct sort_rows would leave it.
static void sort_command_applies_at_barrier() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 1'000; ++i)
            cmd.spawn(Position {scrambled(i), 0}, Health {i});
    });
    auto descents = [&] {
        int prev = -1, d = 0;
        w.for_each<Position const>([&](auto const& p) {
            if (int(p.x) < prev)
                ++d;
            prev = int(p.x);
        });
        return d;
    };
    CHECK(descents() > 0); // scrambled to start

    Schedule s;
    s.add_serial("resort", [](Commands& c) {
        c.sort<Position>([](Position const& p) { return int(p.x); });
    });
    s.run(w);               // records the sort; applied at the wave barrier
    CHECK(descents() == 0); // sorted after the flush
}

int main() {
    RUN_SUITE(sort_orders_rows_and_preserves_handles);
    RUN_SUITE(sort_command_applies_at_barrier);
    RUN_SUITE(sort_is_stable);
    RUN_SUITE(sort_after_churn);
    RUN_SUITE(integral_keys_sort_correctly_on_both_paths);
    RUN_SUITE(min_disorder_skips_small_drift);
    RUN_SUITE(every_cadence_fires_and_sort_command_stays_invariant);
    RUN_SUITE(sorted_layout_stays_lane_count_invariant);
    RUN_SUITE(dynamic_column_applies_permutation);
    return REPORT();
}
