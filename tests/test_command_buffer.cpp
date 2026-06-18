// Auto-deferring mutation API + command buffer tests.
#include "check.hpp"
#include "ecs/ecs.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <vector>

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Doomed {}; // tag marking entities to delete

// With exclusive access (no schedule, no defer_scope) mutations are immediate.
static void immediate_mutation() {
  World w;
  Entity e = w.spawn(Position{1, 1});
  CHECK(w.alive(e));
  CHECK(w.size() == 1);
  w.add<Velocity>(e, Velocity{2, 2});
  CHECK(w.has<Velocity>(e));
  w.set<Position>(e, Position{9, 9});
  CHECK(w.get<Position>(e).x == 9.f);
  w.remove<Velocity>(e);
  CHECK(!w.has<Velocity>(e));
  w.destroy(e);
  CHECK(!w.alive(e));
  CHECK(w.size() == 0);
}

// A defer_scope makes structural edits safe during a manual query loop: they
// are recorded and applied when the scope exits.
static void defer_scope_destroy_during_iteration() {
  World w;
  std::vector<Entity> all;
  for (int i = 0; i < 10; ++i) all.push_back(w.spawn(Position{float(i), 0}));
  for (int i = 0; i < 10; i += 2) w.add<Doomed>(all[i], {}); // mark evens

  {
    auto guard = w.defer_scope();
    query<Position, Doomed>(w).each(
        [&](Entity e, Position&, Doomed&) { w.destroy(e); });
    CHECK(w.size() == 10);            // nothing applied yet
    CHECK(w.commands().size() == 5);  // five destroys recorded
  } // scope exit -> flush

  CHECK(w.size() == 5);
  CHECK(w.commands().empty());
  for (int i = 0; i < 10; ++i)
    CHECK(w.alive(all[i]) == (i % 2 == 1)); // odds survive
}

static void defer_scope_add_and_remove() {
  World w;
  Entity e = w.spawn(Position{1, 2});
  {
    auto guard = w.defer_scope();
    w.add<Velocity>(e, Velocity{3, 4});
    CHECK(!w.has<Velocity>(e)); // deferred
  }
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Velocity>(e).dx == 3.f);
  CHECK(w.get<Position>(e).y == 2.f); // survived archetype move

  {
    auto guard = w.defer_scope();
    w.remove<Velocity>(e);
    CHECK(w.has<Velocity>(e)); // still deferred
  }
  CHECK(!w.has<Velocity>(e));
}

static void spawn_returns_usable_handle() {
  World w;
  Entity e;
  {
    auto guard = w.defer_scope();
    // The handle is valid immediately and can receive follow-up edits, even
    // though storage is created only at flush.
    e = w.spawn(Position{7, 8});
    w.add<Velocity>(e, Velocity{1, 2});
    CHECK(!w.alive(e)); // not materialized yet
  }
  CHECK(w.alive(e));
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Position>(e).x == 7.f);
  CHECK(w.get<Velocity>(e).dy == 2.f);
}

static void reserved_handles_are_distinct() {
  World w;
  Entity a, b, c;
  {
    auto guard = w.defer_scope();
    a = w.spawn(Position{0, 0});
    b = w.spawn(Position{0, 0});
    c = w.spawn(Position{0, 0});
    CHECK(!(a == b));
    CHECK(!(b == c));
  }
  CHECK(w.size() == 3);
  CHECK(w.alive(a) && w.alive(b) && w.alive(c));
}

static void reserved_slot_reuse_bumps_generation() {
  World w;
  Entity first = w.spawn(Position{1, 1});
  w.destroy(first); // frees the slot, bumps its generation

  Entity reused;
  {
    auto guard = w.defer_scope();
    reused = w.spawn(Position{2, 2});
  }
  CHECK(reused.index == first.index);            // slot reused
  CHECK(reused.generation != first.generation);  // fresh generation
  CHECK(!w.alive(first));                          // stale handle stays dead
  CHECK(w.alive(reused));
}

static void destroy_then_add_is_safe() {
  World w;
  Entity e = w.spawn(Position{0, 0});
  {
    auto guard = w.defer_scope();
    w.destroy(e);
    w.add<Velocity>(e, Velocity{9, 9}); // targets an entity destroyed earlier
  }
  CHECK(!w.alive(e)); // add no-op'd; no assert/crash
  CHECK(w.size() == 0);
}

// Inside a schedule, mutations auto-defer (no defer_scope needed) and flush at
// each level barrier.
static void schedule_flushes_between_levels() {
  World w;
  for (int i = 0; i < 4; ++i) w.spawn(Position{0, 0}, Velocity{1, 1});

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

static void spawn_with_followup_edits_in_schedule() {
  World w;
  Schedule sched;
  sched.add("spawner", [](World& wr) {
    for (int i = 0; i < 5; ++i) {
      Entity e = wr.spawn(Position{float(i), 0});
      if (i % 2 == 0) wr.add<Velocity>(e, Velocity{1, 1});
    }
  });
  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());
  CHECK(w.size() == 5);
  CHECK((query<Position>(w).count() == 5));
  CHECK((query<Position, Velocity>(w).count() == 3)); // i = 0,2,4
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

int main() {
  RUN_SUITE(immediate_mutation);
  RUN_SUITE(defer_scope_destroy_during_iteration);
  RUN_SUITE(defer_scope_add_and_remove);
  RUN_SUITE(spawn_returns_usable_handle);
  RUN_SUITE(reserved_handles_are_distinct);
  RUN_SUITE(reserved_slot_reuse_bumps_generation);
  RUN_SUITE(destroy_then_add_is_safe);
  RUN_SUITE(schedule_flushes_between_levels);
  RUN_SUITE(spawn_with_followup_edits_in_schedule);
  RUN_SUITE(concurrent_reserve_and_followup_edits);
  return REPORT();
}
