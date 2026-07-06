// Commands mutation API: deferral, setup, one-shot/phased/removable systems.
#include "check.hpp"
#include "setup.hpp"
#include <atomic>
#include <vector>

using namespace ecs;

struct Position {
    float x, y;
};
struct Velocity {
    float dx, dy;
};
struct Doomed {}; // tag marking entities to delete

// A run context (here a setup system run inline) records mutations and flushes
// when it ends -- so entities are live afterwards, but not during the callback.
// (Taking World& makes the system exclusive; fine for a single setup system.)
static void setup_applies_after_it_ends() {
    World w;
    Entity e;
    setup(w, [&](WorldView view, Commands& cmd) {
        e = cmd.spawn(Position {1, 1});
        CHECK(!view.alive(e)); // recorded, not yet materialized
    });
    CHECK(w.alive(e));
    CHECK(w.size() == 1);
}

// Because mutation is deferred, destroying mid-iteration is safe.
static void destroy_during_iteration_is_safe() {
    World w;
    std::vector<Entity> all;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 10; ++i)
            all.push_back(cmd.spawn(Position {float(i), 0}));
    });
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 10; i += 2)
            cmd.add<Doomed>(all[i], {}); // mark evens
    });

    setup(w, [&](WorldView view, Commands& cmd) {
        view.query<Position, Doomed>().for_each_serial([&](Entity e, auto&, auto&) {
            cmd.destroy(e);
        });
        CHECK(view.size() == 10); // nothing applied yet (still recording)
    });

    CHECK(w.size() == 5);
    for (int i = 0; i < 10; ++i)
        CHECK(w.alive(all[i]) == (i % 2 == 1)); // odds survive
}

static void add_and_remove() {
    World w;
    Entity e;
    setup(w, [&](Commands& cmd) { e = cmd.spawn(Position {1, 2}); });

    setup(w, [&](WorldView view, Commands& cmd) {
        cmd.add<Velocity>(e, Velocity {3, 4});
        CHECK(!view.has<Velocity>(e)); // deferred within the context
    });
    CHECK(w.has<Velocity>(e));
    CHECK(w.get<Velocity>(e).dx == 3.f);
    CHECK(w.get<Position>(e).y == 2.f); // survived archetype move

    setup(w, [&](Commands& cmd) { cmd.remove<Velocity>(e); });
    CHECK(!w.has<Velocity>(e));
}

static void spawn_returns_usable_handle() {
    World w;
    Entity e;
    setup(w, [&](WorldView view, Commands& cmd) {
        e = cmd.spawn(Position {7, 8});        // handle valid immediately
        cmd.add<Velocity>(e, Velocity {1, 2}); // follow-up edit on it
        CHECK(!view.alive(e));                 // materializes at flush
    });
    CHECK(w.alive(e));
    CHECK(w.has<Velocity>(e));
    CHECK(w.get<Position>(e).x == 7.f);
    CHECK(w.get<Velocity>(e).dy == 2.f);
}

// set() on a component the entity does not have is a no-op (it must not throw
// out of the flush). set() on a present component overwrites it.
static void set_on_missing_component_is_noop() {
    World w;
    Entity e;
    setup(w, [&](Commands& cmd) { e = cmd.spawn(Position {1, 2}); });

    bool threw = false;
    try {
        setup(w, [&](Commands& cmd) {
            cmd.set(e, Velocity {9, 9}); // e has no Velocity
            cmd.set(e, Position {3, 4}); // e has Position -> overwritten
        });
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(!w.has<Velocity>(e));         // missing-component set dropped
    CHECK(w.get<Position>(e).x == 3.f); // present-component set applied
}

static void reserved_handles_are_distinct() {
    World w;
    Entity a, b, c;
    setup(w, [&](Commands& cmd) {
        a = cmd.spawn(Position {0, 0});
        b = cmd.spawn(Position {0, 0});
        c = cmd.spawn(Position {0, 0});
    });
    CHECK(!(a == b));
    CHECK(!(b == c));
    CHECK(w.size() == 3);
}

static void reserved_slot_reuse_bumps_generation() {
    World w;
    Entity first;
    setup(w, [&](Commands& cmd) { first = cmd.spawn(Position {1, 1}); });
    setup(w, [&](Commands& cmd) { cmd.destroy(first); });

    Entity reused;
    setup(w, [&](Commands& cmd) { reused = cmd.spawn(Position {2, 2}); });
    CHECK(reused.index == first.index);           // slot reused
    CHECK(reused.generation != first.generation); // fresh generation
    CHECK(!w.alive(first));                       // stale handle stays dead
    CHECK(w.alive(reused));
}

static void destroy_then_add_is_safe() {
    World w;
    Entity e;
    setup(w, [&](Commands& cmd) { e = cmd.spawn(Position {0, 0}); });
    setup(w, [&](Commands& cmd) {
        cmd.destroy(e);
        cmd.add<Velocity>(e, Velocity {9, 9}); // targets an entity destroyed earlier
    });
    CHECK(!w.alive(e)); // add no-op'd; no crash
    CHECK(w.size() == 0);
}

// Bulk creation is just a loop of spawn(); handles collected in the loop are
// usable after the flush.
static void bulk_spawn_loop() {
    World w;
    std::vector<Entity> es;
    setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < 1000; ++i)
            es.push_back(cmd.spawn(Position {float(i), 0}, Velocity {1, 1}));
        for (std::size_t i = 0; i < 50; ++i)
            cmd.spawn(Position {float(i), -1});
    });
    CHECK(w.size() == 1050);
    CHECK((query<Position, Velocity>(w).count() == 1000));
    CHECK((query<Position>(w).count() == 1050));

    CHECK(es.size() == 1000);
    CHECK(w.alive(es.front()) && w.alive(es.back()));
    CHECK(w.get<Position>(es[500]).x == 500.f);

    double sum_x = 0;
    query<Position, Velocity>(w).for_each_serial([&](auto& p, auto&) { sum_x += p.x; });
    CHECK(sum_x == double(999) * 1000 / 2); // 0+1+...+999
}

// Inside a schedule, mutations record automatically and flush at each barrier.
// The consumer is ordered after the producer with a phase (real ordering rather
// than a fabricated access conflict).
static void schedule_flushes_between_levels() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 4; ++i)
            cmd.spawn(Position {0, 0}, Velocity {1, 1});
    });

    std::atomic<int> seen_by_consumer {0};
    Schedule sched;
    sched.add("producer", [](Commands& cmd) {
        for (int i = 0; i < 3; ++i)
            cmd.spawn(Position {9, 9});
    });
    sched.add(
        "consumer",
        [&](Query<Position const> q) { seen_by_consumer.store(int(q.count())); },
        phase<1> {});

    CHECK(sched.level_count() == 2); // phase 0 producer, phase 1 consumer
    WorkerPool pool {4};
    sched.run(w, pool);

    CHECK(w.size() == 7);                // 4 original + 3 spawned
    CHECK(seen_by_consumer.load() == 7); // consumer saw the producer's flush
}

static void concurrent_reserve_and_followup_edits() {
    World w;
    Schedule sched;
    constexpr int kSystems   = 8;
    constexpr int kPerSystem = 250;
    for (int s = 0; s < kSystems; ++s)
        sched.add("spawner", [](Commands& cmd) {
            for (int i = 0; i < kPerSystem; ++i) {
                Entity e = cmd.spawn(Position {1, 1});
                cmd.add<Velocity>(e, Velocity {2, 2});
            }
        });
    CHECK(sched.level_count() == 1); // no tracked access -> all parallel

    WorkerPool pool {8};
    sched.run(w, pool);
    std::size_t const total = std::size_t(kSystems * kPerSystem);
    CHECK(w.size() == total);
    CHECK((query<Position, Velocity>(w).count() == total)); // no lost edits
}

static void add_once_runs_once_then_removed() {
    World w;
    Schedule sched;
    std::atomic<int> runs {0};
    sched.add_once("setup", [&](Commands& cmd) {
        runs.fetch_add(1, std::memory_order_relaxed);
        cmd.spawn(Position {1, 1});
        cmd.spawn(Position {2, 2});
    });
    CHECK(sched.size() == 1);

    WorkerPool pool {2};
    sched.run(w, pool);
    CHECK(runs.load() == 1);
    CHECK(w.size() == 2);
    CHECK(sched.size() == 0); // unscheduled after running

    sched.run(w, pool);      // nothing to run now
    CHECK(runs.load() == 1); // did not run again
    CHECK(w.size() == 2);
}

static void phase_orders_startup_before_update() {
    World w;
    Schedule sched;
    std::atomic<int> seen {-1};
    sched.add_once(
        "startup",
        [](Commands& cmd) { cmd.spawn(Position {0, 0}); },
        phase<-1> {});
    sched.add(
        "update",
        [&](Query<Position const> q) {
            seen.store(int(q.count()), std::memory_order_relaxed);
        },
        phase<0> {});

    CHECK(sched.level_count() == 2); // wave 0 startup, wave 1 update
    WorkerPool pool {4};
    sched.run(w, pool);

    CHECK(seen.load() == 1); // update saw the entity startup spawned
    CHECK(w.size() == 1);
    CHECK(sched.size() == 1); // startup (one-shot) removed; update remains
}

static void remove_unschedules_system() {
    World w;
    Schedule sched;
    std::atomic<int> runs {0};
    SystemId id = sched.add("counter", [&] { runs.fetch_add(1); });
    sched.add("keep", [] {});
    CHECK(sched.size() == 2);

    CHECK(sched.remove(id));
    CHECK(sched.size() == 1);
    CHECK(!sched.remove(id)); // already gone

    WorkerPool pool {2};
    sched.run(w, pool);
    CHECK(runs.load() == 0); // removed system never ran
}

static void inline_run_executes_systems() {
    World w;
    Schedule sched;
    std::atomic<int> ran {0};
    sched.add("spawn3", [&](Commands& cmd) {
        ran.fetch_add(1);
        for (int i = 0; i < 3; ++i)
            cmd.spawn(Position {0, 0});
    });
    sched.run(w); // inline, no thread pool
    CHECK(ran.load() == 1);
    CHECK(w.size() == 3);
}

int main() {
    RUN_SUITE(setup_applies_after_it_ends);
    RUN_SUITE(destroy_during_iteration_is_safe);
    RUN_SUITE(add_and_remove);
    RUN_SUITE(spawn_returns_usable_handle);
    RUN_SUITE(set_on_missing_component_is_noop);
    RUN_SUITE(reserved_handles_are_distinct);
    RUN_SUITE(reserved_slot_reuse_bumps_generation);
    RUN_SUITE(destroy_then_add_is_safe);
    RUN_SUITE(bulk_spawn_loop);
    RUN_SUITE(schedule_flushes_between_levels);
    RUN_SUITE(concurrent_reserve_and_followup_edits);
    RUN_SUITE(add_once_runs_once_then_removed);
    RUN_SUITE(phase_orders_startup_before_update);
    RUN_SUITE(remove_unschedules_system);
    RUN_SUITE(inline_run_executes_systems);
    return REPORT();
}
