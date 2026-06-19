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

int main() {
  RUN_SUITE(create_and_query);
  RUN_SUITE(add_remove_moves_archetype_preserving_data);
  RUN_SUITE(destroy_and_generation_reuse);
  RUN_SUITE(soa_fast_path);
  return REPORT();
}
