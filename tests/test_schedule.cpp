// std::execution scheduler tests. Access is derived from system parameters.
#include "check.hpp"
#include "setup.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Health { int hp; };

static void leveling_respects_conflicts() {
  Schedule sched;
  sched.add("physics", [](Query<Position>) {});       // writes Position
  sched.add("damage", [](Query<Health>) {});          // writes Health
  sched.add("render", [](Query<const Position>) {});  // reads Position
  // physics & damage are independent -> level 0; render reads Position that
  // physics writes -> level 1.
  CHECK(sched.level_count() == 2);
  CHECK(sched.systems()[0].level == 0); // physics
  CHECK(sched.systems()[1].level == 0); // damage
  CHECK(sched.systems()[2].level == 1); // render
}

static void parallel_systems_are_independent_levels() {
  Schedule sched;
  // Three systems with disjoint writes -> all level 0, fully parallel.
  sched.add("a", [](Query<Position>) {});
  sched.add("b", [](Query<Velocity>) {});
  sched.add("c", [](Query<Health>) {});
  CHECK(sched.level_count() == 1);
}

static void run_on_thread_pool_executes_all_systems() {
  World w;
  setup(w, [&](Commands& cmd) {
    for (int i = 0; i < 1000; ++i)
      cmd.spawn(Position{0, 0}, Velocity{1, 2}, Health{100});
  });

  std::atomic<int> render_calls{0};
  Schedule sched;
  // writes Position, reads Velocity
  sched.add("physics", [](Query<Position, const Velocity> q) {
    q.each([](Entity, Position& p, const Velocity& v) {
      p.x += v.dx;
      p.y += v.dy;
    });
  });
  // writes Health
  sched.add("damage", [](Query<Health> q) {
    q.each([](Entity, Health& h) { h.hp -= 1; });
  });
  // no ECS access -- just a side effect
  sched.add("render",
            [&] { render_calls.fetch_add(1, std::memory_order_relaxed); });

  exec::static_thread_pool pool{4};
  auto scheduler = pool.get_scheduler();
  sched.run(w, scheduler);
  sched.run(w, scheduler);

  Entity first{0, 0};
  CHECK(w.get<Position>(first).x == 2.f); // physics ran twice
  CHECK(w.get<Health>(first).hp == 98);   // damage ran twice
  CHECK(render_calls.load() == 2);        // render ran each tick
}

// A WorldView reads everything but writes nothing: read-only systems sharing a
// WorldView run in the same wave, but a WorldView reader is serialized after any
// writer (it could observe a half-applied write otherwise). A raw World&, by
// contrast, is exclusive and conflicts with everyone.
static void worldview_reads_all_parallel_but_after_writers() {
  {
    Schedule sched;
    sched.add("observe_a", [](WorldView) {});
    sched.add("observe_b", [](WorldView) {});
    CHECK(sched.level_count() == 1); // two readers -> one wave
  }
  {
    Schedule sched;
    sched.add("mutate", [](Query<Position>) {});  // writes Position
    sched.add("observe", [](WorldView) {});        // reads everything
    CHECK(sched.level_count() == 2);               // reader serialized after writer
  }
  {
    Schedule sched;
    sched.add("observe", [](WorldView) {});         // reads everything
    sched.add("exclusive", [](World&) {});          // unanalyzable
    CHECK(sched.level_count() == 2);                // World& conflicts with all
  }
}

static void worldview_sees_writers_flush_in_prior_wave() {
  World w;
  setup(w, [&](Commands& cmd) {
    for (int i = 0; i < 5; ++i) cmd.spawn(Position{1, 1});
  });

  std::atomic<int> observed{-1};
  Schedule sched;
  // Spawns three more, flushed at the wave barrier.
  sched.add("spawn", [](Commands& cmd) {
    for (int i = 0; i < 3; ++i) cmd.spawn(Position{2, 2});
  });
  // Read-only ad-hoc access via WorldView, serialized after spawn by phase.
  sched.add("observe",
            [&](WorldView view) {
              observed.store(int(view.size()), std::memory_order_relaxed);
            },
            phase<1>{});

  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());
  CHECK(w.size() == 8);
  CHECK(observed.load() == 8); // saw the spawn wave's flush
}

int main() {
  RUN_SUITE(leveling_respects_conflicts);
  RUN_SUITE(parallel_systems_are_independent_levels);
  RUN_SUITE(run_on_thread_pool_executes_all_systems);
  RUN_SUITE(worldview_reads_all_parallel_but_after_writers);
  RUN_SUITE(worldview_sees_writers_flush_in_prior_wave);
  return REPORT();
}
