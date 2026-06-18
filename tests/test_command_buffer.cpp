// Command-buffer-only mutation: run_once, in-schedule deferral, one-shot
// systems and system removal.
#include "check.hpp"
#include "ecs/ecs.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <vector>

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Doomed {}; // tag marking entities to delete

// run_once opens a context, runs the callback (recording mutations), then
// flushes -- so entities are live once it returns, but not during the callback.
static void run_once_applies_after_return() {
  World w;
  Entity e;
  w.run_once([&](World& wr) {
    e = wr.spawn(Position{1, 1});
    CHECK(!wr.alive(e)); // recorded, not yet materialized
  });
  CHECK(w.alive(e));
  CHECK(w.size() == 1);
}

// Because every structural edit is deferred, destroying mid-iteration is safe.
static void destroy_during_iteration_is_safe() {
  World w;
  std::vector<Entity> all;
  w.run_once([&](World& wr) {
    for (int i = 0; i < 10; ++i) all.push_back(wr.spawn(Position{float(i), 0}));
  });
  w.run_once([&](World& wr) {
    for (int i = 0; i < 10; i += 2) wr.add<Doomed>(all[i], {}); // mark evens
  });

  w.run_once([&](World& wr) {
    query<Position, Doomed>(wr).each(
        [&](Entity e, Position&, Doomed&) { wr.destroy(e); });
    CHECK(wr.size() == 10);            // nothing applied yet (still recording)
    CHECK(wr.commands().size() == 5);
  });

  CHECK(w.size() == 5);
  for (int i = 0; i < 10; ++i)
    CHECK(w.alive(all[i]) == (i % 2 == 1)); // odds survive
}

static void add_and_remove() {
  World w;
  Entity e;
  w.run_once([&](World& wr) { e = wr.spawn(Position{1, 2}); });

  w.run_once([&](World& wr) {
    wr.add<Velocity>(e, Velocity{3, 4});
    CHECK(!wr.has<Velocity>(e)); // deferred within the context
  });
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Velocity>(e).dx == 3.f);
  CHECK(w.get<Position>(e).y == 2.f); // survived archetype move

  w.run_once([&](World& wr) { wr.remove<Velocity>(e); });
  CHECK(!w.has<Velocity>(e));
}

static void spawn_returns_usable_handle() {
  World w;
  Entity e;
  w.run_once([&](World& wr) {
    e = wr.spawn(Position{7, 8});  // handle valid immediately
    wr.add<Velocity>(e, Velocity{1, 2}); // follow-up edit on it
    CHECK(!wr.alive(e));           // materializes at flush
  });
  CHECK(w.alive(e));
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Position>(e).x == 7.f);
  CHECK(w.get<Velocity>(e).dy == 2.f);
}

static void reserved_handles_are_distinct() {
  World w;
  Entity a, b, c;
  w.run_once([&](World& wr) {
    a = wr.spawn(Position{0, 0});
    b = wr.spawn(Position{0, 0});
    c = wr.spawn(Position{0, 0});
  });
  CHECK(!(a == b));
  CHECK(!(b == c));
  CHECK(w.size() == 3);
}

static void reserved_slot_reuse_bumps_generation() {
  World w;
  Entity first;
  w.run_once([&](World& wr) { first = wr.spawn(Position{1, 1}); });
  w.run_once([&](World& wr) { wr.destroy(first); });

  Entity reused;
  w.run_once([&](World& wr) { reused = wr.spawn(Position{2, 2}); });
  CHECK(reused.index == first.index);            // slot reused
  CHECK(reused.generation != first.generation);  // fresh generation
  CHECK(!w.alive(first));                          // stale handle stays dead
  CHECK(w.alive(reused));
}

static void destroy_then_add_is_safe() {
  World w;
  Entity e;
  w.run_once([&](World& wr) { e = wr.spawn(Position{0, 0}); });
  w.run_once([&](World& wr) {
    wr.destroy(e);
    wr.add<Velocity>(e, Velocity{9, 9}); // targets an entity destroyed earlier
  });
  CHECK(!w.alive(e)); // add no-op'd; no crash
  CHECK(w.size() == 0);
}

// Inside a schedule, mutations record automatically and flush at each barrier.
static void schedule_flushes_between_levels() {
  World w;
  w.run_once([&](World& wr) {
    for (int i = 0; i < 4; ++i) wr.spawn(Position{0, 0}, Velocity{1, 1});
  });

  std::atomic<int> seen_by_consumer{0};
  Schedule sched;
  sched.add("producer",
            [](World& wr) {
              for (int i = 0; i < 3; ++i) wr.spawn(Position{9, 9});
            },
            writes<Velocity>{});
  sched.add("consumer",
            [&](World& wr) {
              seen_by_consumer.store(int(query<Position>(wr).count()));
            },
            reads<Velocity>{});

  CHECK(sched.level_count() == 2);
  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());

  CHECK(w.size() == 7);                // 4 original + 3 spawned
  CHECK(seen_by_consumer.load() == 7); // consumer saw level 0's flush
}

static void concurrent_reserve_and_followup_edits() {
  World w;
  Schedule sched;
  constexpr int kSystems = 8;
  constexpr int kPerSystem = 250;
  for (int s = 0; s < kSystems; ++s)
    sched.add("spawner", [](World& wr) {
      for (int i = 0; i < kPerSystem; ++i) {
        Entity e = wr.spawn(Position{1, 1});
        wr.add<Velocity>(e, Velocity{2, 2});
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
  sched.add_once("setup", [&](World& wr) {
    runs.fetch_add(1, std::memory_order_relaxed);
    wr.spawn(Position{1, 1});
    wr.spawn(Position{2, 2});
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

// remove(): unschedule a system by its handle.
static void remove_unschedules_system() {
  World w;
  Schedule sched;
  std::atomic<int> runs{0};
  SystemId id = sched.add("counter",
                          [&](World&) { runs.fetch_add(1); });
  sched.add("keep", [](World&) {});
  CHECK(sched.size() == 2);

  CHECK(sched.remove(id));
  CHECK(sched.size() == 1);
  CHECK(!sched.remove(id)); // already gone

  exec::static_thread_pool pool{2};
  sched.run(w, pool.get_scheduler());
  CHECK(runs.load() == 0); // removed system never ran
}

int main() {
  RUN_SUITE(run_once_applies_after_return);
  RUN_SUITE(destroy_during_iteration_is_safe);
  RUN_SUITE(add_and_remove);
  RUN_SUITE(spawn_returns_usable_handle);
  RUN_SUITE(reserved_handles_are_distinct);
  RUN_SUITE(reserved_slot_reuse_bumps_generation);
  RUN_SUITE(destroy_then_add_is_safe);
  RUN_SUITE(schedule_flushes_between_levels);
  RUN_SUITE(concurrent_reserve_and_followup_edits);
  RUN_SUITE(add_once_runs_once_then_removed);
  RUN_SUITE(remove_unschedules_system);
  return REPORT();
}
