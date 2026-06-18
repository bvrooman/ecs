// Deferred command buffer tests.
#include "check.hpp"
#include "ecs/ecs.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <vector>

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Doomed {}; // tag marking entities to delete

static void deferred_destroy_during_iteration() {
  World w;
  std::vector<Entity> all;
  for (int i = 0; i < 10; ++i)
    all.push_back(w.create_with(Position{float(i), 0}));
  // Mark the even ones.
  for (int i = 0; i < 10; i += 2) w.add<Doomed>(all[i], {});

  // Record destroys while iterating -- mutating now would invalidate the query.
  query<Position, Doomed>(w).each(
      [&](Entity e, Position&, Doomed&) { w.commands().destroy(e); });
  CHECK(w.size() == 10); // nothing happened yet
  CHECK(w.commands().size() == 5);

  w.apply_commands();
  CHECK(w.size() == 5);
  CHECK(w.commands().empty());
  for (int i = 0; i < 10; ++i)
    CHECK(w.alive(all[i]) == (i % 2 == 1)); // odd ones survive
}

static void deferred_add_and_remove() {
  World w;
  Entity e = w.create_with(Position{1, 2});
  w.commands().add<Velocity>(e, Velocity{3, 4});
  CHECK(!w.has<Velocity>(e)); // deferred
  w.apply_commands();
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Velocity>(e).dx == 3.f);
  CHECK(w.get<Position>(e).y == 2.f); // survived archetype move

  w.commands().remove<Velocity>(e);
  w.apply_commands();
  CHECK(!w.has<Velocity>(e));
}

static void deferred_spawn() {
  World w;
  w.commands().spawn(Position{5, 5}, Velocity{1, 1});
  w.commands().spawn(Position{6, 6});
  CHECK(w.size() == 0);
  w.apply_commands();
  CHECK(w.size() == 2);
  CHECK((query<Position, Velocity>(w).count() == 1));
  CHECK((query<Position>(w).count() == 2));
}

static void spawn_returns_usable_handle() {
  World w;
  // The handle is valid immediately and can receive further deferred edits in
  // the same frame, even though storage is created only at apply.
  Entity e = w.commands().spawn(Position{7, 8});
  w.commands().add<Velocity>(e, Velocity{1, 2}); // edit the reserved entity
  CHECK(!w.alive(e));                            // not materialized yet

  w.apply_commands();
  CHECK(w.alive(e));
  CHECK(w.has<Velocity>(e));
  CHECK(w.get<Position>(e).x == 7.f);
  CHECK(w.get<Velocity>(e).dy == 2.f);
}

static void reserved_handles_are_distinct() {
  World w;
  Entity a = w.commands().spawn(Position{0, 0});
  Entity b = w.commands().spawn(Position{0, 0});
  Entity c = w.commands().spawn(Position{0, 0});
  CHECK(!(a == b));
  CHECK(!(b == c));
  w.apply_commands();
  CHECK(w.size() == 3);
  CHECK(w.alive(a) && w.alive(b) && w.alive(c));
}

static void reserved_slot_reuse_bumps_generation() {
  World w;
  Entity first = w.create_with(Position{1, 1});
  w.destroy(first); // frees the slot, bumps its generation

  Entity reused = w.commands().spawn(Position{2, 2});
  w.apply_commands();
  CHECK(reused.index == first.index);          // slot reused
  CHECK(reused.generation != first.generation); // fresh generation
  CHECK(!w.alive(first));                        // stale handle stays dead
  CHECK(w.alive(reused));
}

static void destroy_then_add_is_safe() {
  World w;
  Entity e = w.create_with(Position{0, 0});
  w.commands().destroy(e);
  w.commands().add<Velocity>(e, Velocity{9, 9}); // targets a now-dead entity
  w.apply_commands();                            // must not assert/crash
  CHECK(!w.alive(e));
  CHECK(w.size() == 0);
}

static void schedule_flushes_between_levels() {
  // A producer (level 0) spawns; a consumer that reads what the producer wrote
  // lands on level 1 and must see the spawned entities applied at the barrier.
  World w;
  for (int i = 0; i < 4; ++i) w.create_with(Position{0, 0}, Velocity{1, 1});

  std::atomic<int> seen_by_consumer{0};
  Schedule sched;
  // producer writes Velocity; spawns 3 new Position-only entities
  sched.add("producer",
            [](World& wr) {
              for (int i = 0; i < 3; ++i) wr.commands().spawn(Position{9, 9});
            },
            writes<Velocity>{});
  // consumer reads Velocity (=> level after producer) and counts Position
  sched.add("consumer",
            [&](World& wr) {
              seen_by_consumer.store(int(query<Position>(wr).count()));
            },
            reads<Velocity>{});

  CHECK(sched.level_count() == 2);
  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());

  CHECK(w.size() == 7);                  // 4 original + 3 spawned
  CHECK(seen_by_consumer.load() == 7);   // consumer saw the flush from level 0
}

static void spawn_with_followup_edits_in_schedule() {
  // A single system spawns entities and immediately records a follow-up add on
  // each returned handle; both are applied at the level barrier.
  World w;
  Schedule sched;
  sched.add("spawner", [](World& wr) {
    for (int i = 0; i < 5; ++i) {
      Entity e = wr.commands().spawn(Position{float(i), 0});
      if (i % 2 == 0) wr.commands().add<Velocity>(e, Velocity{1, 1});
    }
  });

  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());
  CHECK(w.size() == 5);
  CHECK((query<Position>(w).count() == 5));
  CHECK((query<Position, Velocity>(w).count() == 3)); // i = 0,2,4
}

static void concurrent_reserve_and_followup_edits() {
  // Parallel systems each spawn-and-add via the returned handle; reservation
  // must hand out unique slots across threads and every entity must end up
  // with its Velocity.
  World w;
  Schedule sched;
  constexpr int kSystems = 8;
  constexpr int kPerSystem = 250;
  for (int s = 0; s < kSystems; ++s)
    sched.add("spawner", [](World& wr) {
      for (int i = 0; i < kPerSystem; ++i) {
        Entity e = wr.commands().spawn(Position{1, 1});
        wr.commands().add<Velocity>(e, Velocity{2, 2});
      }
    });
  CHECK(sched.level_count() == 1);

  exec::static_thread_pool pool{8};
  sched.run(w, pool.get_scheduler());
  const std::size_t total = std::size_t(kSystems * kPerSystem);
  CHECK(w.size() == total);
  CHECK((query<Position, Velocity>(w).count() == total)); // no lost edits
}

static void concurrent_recording_is_thread_safe() {
  // Many systems on the same level record into the shared buffer concurrently;
  // they have disjoint component writes so they run in parallel.
  World w;
  Schedule sched;
  constexpr int kSystems = 8;
  constexpr int kPerSystem = 500;
  // Each system writes a distinct tag-less marker via a unique component so the
  // scheduler keeps them on one level; here we just give them no declared
  // access => no conflicts => all level 0, fully parallel.
  for (int s = 0; s < kSystems; ++s)
    sched.add("spawner", [](World& wr) {
      for (int i = 0; i < kPerSystem; ++i) wr.commands().spawn(Position{1, 1});
    });
  CHECK(sched.level_count() == 1);

  exec::static_thread_pool pool{8};
  sched.run(w, pool.get_scheduler());
  CHECK(w.size() == std::size_t(kSystems * kPerSystem));
}

int main() {
  RUN_SUITE(deferred_destroy_during_iteration);
  RUN_SUITE(deferred_add_and_remove);
  RUN_SUITE(deferred_spawn);
  RUN_SUITE(spawn_returns_usable_handle);
  RUN_SUITE(reserved_handles_are_distinct);
  RUN_SUITE(reserved_slot_reuse_bumps_generation);
  RUN_SUITE(destroy_then_add_is_safe);
  RUN_SUITE(schedule_flushes_between_levels);
  RUN_SUITE(spawn_with_followup_edits_in_schedule);
  RUN_SUITE(concurrent_reserve_and_followup_edits);
  RUN_SUITE(concurrent_recording_is_thread_safe);
  return REPORT();
}
