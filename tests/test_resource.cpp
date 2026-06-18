// Resource registry + resource-aware scheduling tests.
#include "check.hpp"
#include "ecs/ecs.hpp"

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
  // Two systems that both write the same resource must land on different
  // levels; two that only read it may share a level.
  Schedule sched;
  sched.add("writer_a", [](World&) {}, writes_res<Counter>{});
  sched.add("writer_b", [](World&) {}, writes_res<Counter>{});
  CHECK(sched.level_count() == 2);

  Schedule readers;
  readers.add("reader_a", [](World&) {}, reads_res<Counter>{});
  readers.add("reader_b", [](World&) {}, reads_res<Counter>{});
  CHECK(readers.level_count() == 1);
}

static void resource_and_component_conflicts_are_independent() {
  // A resource conflict alone is enough to serialize, even when component
  // access is disjoint.
  Schedule sched;
  sched.add("a", [](World&) {}, writes<Position>{}, writes_res<Counter>{});
  sched.add("b", [](World&) {}, reads_res<Counter>{}); // no component overlap
  CHECK(sched.level_count() == 2); // serialized purely by the resource
}

static void serialized_writers_run_without_races() {
  World w;
  w.emplace_resource<Counter>();
  for (int i = 0; i < 10'000; ++i) w.spawn(Position{1.0f});

  // Both systems mutate the shared Counter; declaring writes_res<Counter>
  // forces them onto separate levels, so the unsynchronized ++ is safe.
  Schedule sched;
  sched.add("inc_by_count",
            [](World& wr) {
              wr.resource<Counter>().n += int(query<Position>(wr).count());
            },
            reads<Position>{}, writes_res<Counter>{});
  sched.add("inc_by_one", [](World& wr) { wr.resource<Counter>().n += 1; },
            writes_res<Counter>{});

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
