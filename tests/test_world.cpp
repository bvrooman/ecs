// World / archetype / query tests.
#include "check.hpp"
#include "setup.hpp"

using namespace ecs;

struct Position { float x, y; };
struct Velocity { float dx, dy; };
struct Frozen {}; // tag

static void create_and_query() {
  World w;
  Entity a, b, c;
  setup(w, [&](World&, Commands& cmd) {
    a = cmd.spawn(Position{0, 0}, Velocity{1, 2});
    b = cmd.spawn(Position{10, 10});
    c = cmd.spawn(Position{5, 5}, Velocity{-1, -1}, Frozen{});
  });
  CHECK(w.size() == 3);
  CHECK(w.has<Velocity>(a));
  CHECK(!w.has<Velocity>(b));
  CHECK(w.has<Frozen>(c));
  CHECK((query<Position, Velocity>(w).count() == 2));

  // Value mutation through a query is not a structural change -- needs no Commands.
  query<Position, Velocity>(w).each(
      [](Entity, Position& p, Velocity& v) { p.x += v.dx; p.y += v.dy; });
  CHECK(w.get<Position>(a).x == 1.f);
  CHECK(w.get<Position>(c).x == 4.f);
  CHECK(w.get<Position>(b).x == 10.f); // no Velocity -> untouched
}

static void add_remove_moves_archetype_preserving_data() {
  World w;
  Entity b;
  setup(w, [&](World&, Commands& cmd) { b = cmd.spawn(Position{10, 20}); });
  CHECK((query<Position, Velocity>(w).count() == 0));

  setup(w, [&](World&, Commands& cmd) { cmd.add<Velocity>(b, Velocity{3, 3}); });
  CHECK(w.has<Velocity>(b));
  CHECK(w.get<Position>(b).y == 20.f); // Position survived the move
  CHECK((query<Position, Velocity>(w).count() == 1));

  setup(w, [&](World&, Commands& cmd) { cmd.remove<Velocity>(b); });
  CHECK(!w.has<Velocity>(b));
  CHECK(w.get<Position>(b).x == 10.f);
  CHECK((query<Position, Velocity>(w).count() == 0));
}

static void destroy_and_generation_reuse() {
  World w;
  Entity a, a2;
  setup(w, [&](World&, Commands& cmd) {
    a = cmd.spawn(Position{1, 1});
    a2 = cmd.spawn(Position{2, 2});
  });
  (void)a2;
  Entity old = a;
  setup(w, [&](World&, Commands& cmd) { cmd.destroy(a); });
  CHECK(!w.alive(old));
  CHECK(w.size() == 1);

  Entity d;
  setup(w, [&](World&, Commands& cmd) { d = cmd.spawn(); });
  CHECK(d.index == old.index);            // slot reused
  CHECK(d.generation != old.generation);  // stale handle won't match
  CHECK(!w.alive(old));
}

static void soa_fast_path() {
  World w;
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 4; ++i) cmd.spawn(Position{float(i), 0});
  });
  query<Position>(w).for_each_chunk(
      [](std::span<Entity>, soa_storage<Position>& pos) {
        for (auto& x : pos.column<0>()) x += 100.f;
      });
  // every Position.x was bumped by 100 via a contiguous column loop
  std::size_t seen = 0;
  query<Position>(w).each([&](Entity, Position& p) {
    CHECK(p.x >= 100.f);
    ++seen;
  });
  CHECK(seen == 4);
}

static void const_query_marks_read_only() {
  World w;
  Entity e;
  setup(w, [&](World&, Commands& cmd) {
    e = cmd.spawn(Position{1, 1}, Velocity{2, 3});
  });
  // Velocity read-only (const), Position mutable: only Position is written back.
  query<const Velocity, Position>(w).each(
      [](Entity, const Velocity& v, Position& p) { p.x += v.dx; });
  CHECK(w.get<Position>(e).x == 3.f);  // 1 + 2
  CHECK(w.get<Velocity>(e).dx == 2.f); // untouched

  // for_each_chunk hands a const storage for the read-only component. (Note the
  // query also matches the now-empty intermediate {Position} archetype left by
  // the incremental spawn, so accumulate across chunks.)
  float sum_x = 0;
  std::size_t seen = 0;
  query<const Position>(w).for_each_chunk(
      [&](std::span<Entity>, const soa_storage<Position>& pos) {
        for (float x : pos.column<0>()) { // span<const float>
          sum_x += x;
          ++seen;
        }
      });
  CHECK(seen == 1);
  CHECK(sum_x == 3.f);
}

static void spawn_goes_directly_to_final_archetype() {
  World w;
  setup(w, [&](World&, Commands& cmd) {
    for (int i = 0; i < 100; ++i) cmd.spawn(Position{float(i), 0}, Velocity{1, 1});
  });
  CHECK(w.size() == 100);
  CHECK((query<Position, Velocity>(w).count() == 100));
  // Only the empty archetype and {Position, Velocity} exist -- spawning does not
  // pass through (and leave behind) an empty intermediate {Position} archetype.
  CHECK(w.archetypes().size() == 2);
}

static void query_cache_sees_archetypes_created_after_first_query() {
  World w;
  setup(w, [&](World&, Commands& cmd) { cmd.spawn(Position{1, 1}); }); // {Position}
  CHECK((query<Position>(w).count() == 1)); // builds the cache for {Position}

  // A new archetype {Position, Velocity} created later must enter the cache.
  setup(w, [&](World&, Commands& cmd) { cmd.spawn(Position{2, 2}, Velocity{1, 1}); });
  CHECK((query<Position>(w).count() == 2));
  CHECK((query<Position, Velocity>(w).count() == 1));
}

int main() {
  RUN_SUITE(create_and_query);
  RUN_SUITE(add_remove_moves_archetype_preserving_data);
  RUN_SUITE(destroy_and_generation_reuse);
  RUN_SUITE(soa_fast_path);
  RUN_SUITE(const_query_marks_read_only);
  RUN_SUITE(spawn_goes_directly_to_final_archetype);
  RUN_SUITE(query_cache_sees_archetypes_created_after_first_query);
  return REPORT();
}
