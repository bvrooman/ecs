// std::execution scheduler tests.
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
  sched.add("physics", [](World&, Commands&) {}, reads<Velocity>{},
            writes<Position>{});
  sched.add("damage", [](World&, Commands&) {}, writes<Health>{});
  sched.add("render", [](World&, Commands&) {}, reads<Position>{});
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
  sched.add("a", [](World&, Commands&) {}, writes<Position>{});
  sched.add("b", [](World&, Commands&) {}, writes<Velocity>{});
  sched.add("c", [](World&, Commands&) {}, writes<Health>{});
  CHECK(sched.level_count() == 1);
}

static void run_on_thread_pool_executes_all_systems() {
  World w;
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 1000; ++i)
      cmd.spawn(Position{0, 0}, Velocity{1, 2}, Health{100});
  });

  std::atomic<int> render_calls{0};
  Schedule sched;
  sched.add("physics",
            [](World& wr, Commands&) {
              query<Position, Velocity>(wr).each([](Entity, Position& p,
                                                    Velocity& v) {
                p.x += v.dx;
                p.y += v.dy;
              });
            },
            reads<Velocity>{}, writes<Position>{});
  sched.add("damage",
            [](World& wr, Commands&) {
              query<Health>(wr).each([](Entity, Health& h) { h.hp -= 1; });
            },
            writes<Health>{});
  sched.add("render",
            [&](World&, Commands&) {
              render_calls.fetch_add(1, std::memory_order_relaxed);
            },
            reads<Position>{});

  exec::static_thread_pool pool{4};
  auto scheduler = pool.get_scheduler();
  sched.run(w, scheduler);
  sched.run(w, scheduler);

  Entity first{0, 0};
  CHECK(w.get<Position>(first).x == 2.f); // physics ran twice
  CHECK(w.get<Health>(first).hp == 98);   // damage ran twice
  CHECK(render_calls.load() == 2);        // render ran each tick
}

int main() {
  RUN_SUITE(leveling_respects_conflicts);
  RUN_SUITE(parallel_systems_are_independent_levels);
  RUN_SUITE(run_on_thread_pool_executes_all_systems);
  return REPORT();
}
