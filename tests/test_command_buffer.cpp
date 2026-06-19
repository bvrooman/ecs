// Commands mutation API: deferral, setup, spawn_n, one-shot/phased/removable
// systems.
#include "check.hpp"
#include "setup.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <vector>

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Doomed {}; // tag marking entities to delete

// A run context (here a setup system run inline) records mutations and flushes
// when it ends -- so entities are live afterwards, but not during the callback.
static void setup_applies_after_it_ends() {
  World w;
  Entity e;
  setup(w, [&](World& wr, Commands& cmd) {
    e = cmd.spawn(Position{1, 1});
    CHECK(!wr.alive(e)); // recorded, not yet materialized
  });
  CHECK(w.alive(e));
  CHECK(w.size() == 1);
}

// Because mutation is deferred, destroying mid-iteration is safe.
static void destroy_during_iteration_is_safe() {
  World w;
  std::vector<Entity> all;
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 10; ++i) all.push_back(cmd.spawn(Position{float(i), 0}));
  });
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 10; i += 2) cmd.add<Doomed>(all[i], {}); // mark evens
  });

  setup(w, [&](World& wr, Commands& cmd) {
    query<Position, Doomed>(wr).each(
        [&](Entity e, Position&, Doomed&) { cmd.destroy(e); });
    CHECK(wr.size() == 10); // nothing applied yet (still recording)
  });

  CHECK(w.size() == 5);
  for (int i = 0; i < 10; ++i)
    CHECK(w.alive(all[i]) == (i % 2 == 1)); // odds survive
}

static void add_and_remove() {
  World w;
  Entity e;
  setup(w, [&](World&, Commands& cmd) { e = cmd.spawn(Position{1, 2}); });

  setup(w, [&](World& wr, Commands& cmd) {
    cmd.add<Velocity>(e, Velocity{3, 4});
    CHECK(!wr.has<Velocity>(e)); // deferred within the context
  });
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Velocity>(e).dx == 3.f);
  CHECK(w.get<Position>(e).y == 2.f); // survived archetype move

  setup(w, [&](World&, Commands& cmd) { cmd.remove<Velocity>(e); });
  CHECK(!w.has<Velocity>(e));
}

static void spawn_returns_usable_handle() {
  World w;
  Entity e;
  setup(w, [&](World& wr, Commands& cmd) {
    e = cmd.spawn(Position{7, 8});       // handle valid immediately
    cmd.add<Velocity>(e, Velocity{1, 2}); // follow-up edit on it
    CHECK(!wr.alive(e));                  // materializes at flush
  });
  CHECK(w.alive(e));
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Position>(e).x == 7.f);
  CHECK(w.get<Velocity>(e).dy == 2.f);
}

static void reserved_handles_are_distinct() {
  World w;
  Entity a, b, c;
  setup(w, [&](World&, Commands& cmd) {
    a = cmd.spawn(Position{0, 0});
    b = cmd.spawn(Position{0, 0});
    c = cmd.spawn(Position{0, 0});
  });
  CHECK(!(a == b));
  CHECK(!(b == c));
  CHECK(w.size() == 3);
}

static void reserved_slot_reuse_bumps_generation() {
  World w;
  Entity first;
  setup(w, [&](World&, Commands& cmd) { first = cmd.spawn(Position{1, 1}); });
  setup(w, [&](World&, Commands& cmd) { cmd.destroy(first); });

  Entity reused;
  setup(w, [&](World&, Commands& cmd) { reused = cmd.spawn(Position{2, 2}); });
  CHECK(reused.index == first.index);            // slot reused
  CHECK(reused.generation != first.generation);  // fresh generation
  CHECK(!w.alive(first));                          // stale handle stays dead
  CHECK(w.alive(reused));
}

static void destroy_then_add_is_safe() {
  World w;
  Entity e;
  setup(w, [&](World&, Commands& cmd) { e = cmd.spawn(Position{0, 0}); });
  setup(w, [&](World&, Commands& cmd) {
    cmd.destroy(e);
    cmd.add<Velocity>(e, Velocity{9, 9}); // targets an entity destroyed earlier
  });
  CHECK(!w.alive(e)); // add no-op'd; no crash
  CHECK(w.size() == 0);
}

// spawn_n: bulk creation as a single recorded command, returning handles.
static void spawn_n_bulk() {
  World w;
  std::vector<Entity> es;
  setup(w, [&](World&, Commands& cmd) {
    es = cmd.spawn_n(1000, [](std::size_t i) {
      return std::tuple{Position{float(i), 0}, Velocity{1, 1}};
    });
    cmd.spawn_n(50, [](std::size_t i) { return Position{float(i), -1}; });
  });
  CHECK(w.size() == 1050);
  CHECK((query<Position, Velocity>(w).count() == 1000));
  CHECK((query<Position>(w).count() == 1050));

  CHECK(es.size() == 1000);
  CHECK(w.alive(es.front()) && w.alive(es.back()));
  CHECK(w.get<Position>(es[500]).x == 500.f);

  double sum_x = 0;
  query<Position, Velocity>(w).each(
      [&](Entity, Position& p, Velocity&) { sum_x += p.x; });
  CHECK(sum_x == double(999) * 1000 / 2); // 0+1+...+999
}

// Inside a schedule, mutations record automatically and flush at each barrier.
static void schedule_flushes_between_levels() {
  World w;
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 4; ++i) cmd.spawn(Position{0, 0}, Velocity{1, 1});
  });

  std::atomic<int> seen_by_consumer{0};
  Schedule sched;
  sched.add("producer",
            [](World&, Commands& cmd) {
              for (int i = 0; i < 3; ++i) cmd.spawn(Position{9, 9});
            },
            writes<Velocity>{});
  sched.add("consumer",
            [&](World& wr, Commands&) {
              seen_by_consumer.store(int(query<Position>(wr).count()));
            },
            reads<Velocity>{});

  CHECK(sched.level_count() == 2);
  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());

  CHECK(w.size() == 7);                // 4 original + 3 spawned
  CHECK(seen_by_consumer.load() == 7); // consumer saw wave 0's flush
}

static void concurrent_reserve_and_followup_edits() {
  World w;
  Schedule sched;
  constexpr int kSystems = 8;
  constexpr int kPerSystem = 250;
  for (int s = 0; s < kSystems; ++s)
    sched.add("spawner", [](World&, Commands& cmd) {
      for (int i = 0; i < kPerSystem; ++i) {
        Entity e = cmd.spawn(Position{1, 1});
        cmd.add<Velocity>(e, Velocity{2, 2});
      }
    });
  CHECK(sched.level_count() == 1);

  exec::static_thread_pool pool{8};
  sched.run(w, pool.get_scheduler());
  const std::size_t total = std::size_t(kSystems * kPerSystem);
  CHECK(w.size() == total);
  CHECK((query<Position, Velocity>(w).count() == total)); // no lost edits
}

// add_once: a setup system that runs on the next run() and is then removed.
static void add_once_runs_once_then_removed() {
  World w;
  Schedule sched;
  std::atomic<int> runs{0};
  sched.add_once("setup", [&](World&, Commands& cmd) {
    runs.fetch_add(1, std::memory_order_relaxed);
    cmd.spawn(Position{1, 1});
    cmd.spawn(Position{2, 2});
  });
  CHECK(sched.size() == 1);

  exec::static_thread_pool pool{2};
  sched.run(w, pool.get_scheduler());
  CHECK(runs.load() == 1);
  CHECK(w.size() == 2);
  CHECK(sched.size() == 0); // unscheduled after running

  sched.run(w, pool.get_scheduler()); // nothing to run now
  CHECK(runs.load() == 1);             // did not run again
  CHECK(w.size() == 2);
}

// Phases order systems across barriers regardless of conflicts.
static void phase_orders_startup_before_update() {
  World w;
  Schedule sched;
  std::atomic<int> seen{-1};
  sched.add_once("startup",
                 [](World&, Commands& cmd) { cmd.spawn(Position{0, 0}); },
                 phase<-1>{});
  sched.add("update",
            [&](World& wr, Commands&) {
              seen.store(int(query<Position>(wr).count()),
                         std::memory_order_relaxed);
            },
            phase<0>{});

  CHECK(sched.level_count() == 2); // wave 0 startup, wave 1 update
  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());

  CHECK(seen.load() == 1);  // update saw the entity startup spawned
  CHECK(w.size() == 1);
  CHECK(sched.size() == 1); // startup (one-shot) removed; update remains
}

// remove(): unschedule a system by its handle.
static void remove_unschedules_system() {
  World w;
  Schedule sched;
  std::atomic<int> runs{0};
  SystemId id = sched.add("counter",
                          [&](World&, Commands&) { runs.fetch_add(1); });
  sched.add("keep", [](World&, Commands&) {});
  CHECK(sched.size() == 2);

  CHECK(sched.remove(id));
  CHECK(sched.size() == 1);
  CHECK(!sched.remove(id)); // already gone

  exec::static_thread_pool pool{2};
  sched.run(w, pool.get_scheduler());
  CHECK(runs.load() == 0); // removed system never ran
}

// Inline run: schedule.run(world) with no scheduler executes on this thread.
static void inline_run_executes_systems() {
  World w;
  Schedule sched;
  std::atomic<int> ran{0};
  sched.add("spawn3", [&](World&, Commands& cmd) {
    ran.fetch_add(1);
    for (int i = 0; i < 3; ++i) cmd.spawn(Position{0, 0});
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
  RUN_SUITE(reserved_handles_are_distinct);
  RUN_SUITE(reserved_slot_reuse_bumps_generation);
  RUN_SUITE(destroy_then_add_is_safe);
  RUN_SUITE(spawn_n_bulk);
  RUN_SUITE(schedule_flushes_between_levels);
  RUN_SUITE(concurrent_reserve_and_followup_edits);
  RUN_SUITE(add_once_runs_once_then_removed);
  RUN_SUITE(phase_orders_startup_before_update);
  RUN_SUITE(remove_unschedules_system);
  RUN_SUITE(inline_run_executes_systems);
  return REPORT();
}
