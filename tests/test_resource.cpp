// Resource registry + resource-aware scheduling tests.
#include "check.hpp"
#include "setup.hpp"

#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <memory>

using namespace ecs;

struct Clock { double t = 0.0; };
struct Counter { int n = 0; };

// A move-only resource (e.g. a device/engine handle) must be storable.
struct MoveOnly {
  std::unique_ptr<int> p;
  explicit MoveOnly(int v) : p(std::make_unique<int>(v)) {}
};

struct Position { float x; };

static void emplace_get_has_remove() {
  World w;
  CHECK(!w.has_resource<Clock>());
  CHECK(w.try_resource<Clock>() == nullptr);

  Clock& c = w.emplace_resource<Clock>(Clock{1.5});
  CHECK(w.has_resource<Clock>());
  CHECK(w.resource<Clock>().t == 1.5);

  c.t = 2.0;                       // returned reference aliases storage
  CHECK(w.resource<Clock>().t == 2.0);
  CHECK(w.try_resource<Clock>() != nullptr);

  w.remove_resource<Clock>();
  CHECK(!w.has_resource<Clock>());
}

static void move_only_resource() {
  World w;
  w.emplace_resource<MoveOnly>(42);
  CHECK(*w.resource<MoveOnly>().p == 42);
}

static void const_access() {
  World w;
  w.emplace_resource<Clock>(Clock{3.0});
  const World& cw = w;
  CHECK(cw.resource<Clock>().t == 3.0); // const overload compiles & reads
}

static void resource_conflict_serializes() {
  // Two systems that both write the same resource (ResMut) must land on
  // different levels; two that only read it (Res) may share a level.
  Schedule sched;
  sched.add("writer_a", [](ResMut<Counter>) {});
  sched.add("writer_b", [](ResMut<Counter>) {});
  CHECK(sched.level_count() == 2);

  Schedule readers;
  readers.add("reader_a", [](Res<Counter>) {});
  readers.add("reader_b", [](Res<Counter>) {});
  CHECK(readers.level_count() == 1);
}

static void resource_and_component_conflicts_are_independent() {
  // A resource conflict alone is enough to serialize, even when component
  // access is disjoint.
  Schedule sched;
  sched.add("a", [](Query<Position>, ResMut<Counter>) {});
  sched.add("b", [](Res<Counter>) {}); // no component overlap
  CHECK(sched.level_count() == 2);     // serialized purely by the resource
}

static void serialized_writers_run_without_races() {
  World w;
  w.emplace_resource<Counter>();
  setup(w, [&](Commands& cmd) {
    for (int i = 0; i < 10'000; ++i) cmd.spawn(Position{1.0f});
  });

  // Both systems mutate the shared Counter; ResMut<Counter> on each forces them
  // onto separate levels, so the unsynchronized += is safe.
  Schedule sched;
  sched.add("inc_by_count", [](Query<const Position> q, ResMut<Counter> c) {
    c->n += int(q.count());
  });
  sched.add("inc_by_one", [](ResMut<Counter> c) { c->n += 1; });

  CHECK(sched.level_count() == 2);

  exec::static_thread_pool pool{4};
  sched.run(w, pool.get_scheduler());
  // 10000 (from count) + 1
  CHECK(w.resource<Counter>().n == 10'001);
}

int main() {
  RUN_SUITE(emplace_get_has_remove);
  RUN_SUITE(move_only_resource);
  RUN_SUITE(const_access);
  RUN_SUITE(resource_conflict_serializes);
  RUN_SUITE(resource_and_component_conflicts_are_independent);
  RUN_SUITE(serialized_writers_run_without_races);
  return REPORT();
}
