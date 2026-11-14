// examples/hello.cpp
//
// The README's opening example, character for character. It lives here so the
// normal build compiles the snippet on the project's front page: when the
// library changes underneath it, this file stops building instead of the
// README quietly going stale. Change the two together -- everything below the
// marker must match the README's first code block exactly.
//
// The layout is the README's, not clang-format's, so the two can be compared
// line for line -- hence the opt-out, which must stay a bare marker.

// clang-format off
#include <ecs/ecs.hpp>
using namespace ecs;

struct Position { float x, y, z; };   // components are just structs
struct Velocity { float x, y, z; };

int main() {
  World world;
  Schedule init;                                  // setup is a one-shot system
  init.add_serial("populate", [](Commands& cmd) {
    for (int i = 0; i < 100'000; ++i)             // an entity is whatever
      cmd.spawn(Position{}, Velocity{1, 0, 0});   // components you give it
  }, times{1});                                   // runs once, then drops out
  init.run(world);                                // serial (a 1-lane pool)

  Schedule schedule;
  // A parallel system: its access is derived from its parameter types (here, write
  // Position, read const Velocity). `add_parallel` lets the executor slice the
  // matched rows into work items across the pool's lanes -- that IS the
  // parallelism. Each work item binds `q` to its own slice, so the body below
  // runs once per lane over disjoint rows (race-free by construction).
  schedule.add_parallel("integrate", [](Query<Position, const Velocity> q) {
    // for_each_chunk is the SoA access shape (not the source of parallelism): a
    // `chunk` per component whose column<I>() is that field's contiguous span
    // over this work item's slice -- no per-row gather/scatter.
    q.for_each_chunk([](std::span<Entity const>,
                        chunk<Position> pos, chunk<const Velocity> vel) {
      auto px = pos.column<0>(); // this slice's x column (SoA)
      auto vx = vel.column<0>();
      for (std::size_t i = 0; i < px.size(); ++i)
        px[i] += vx[i];          // tight, vectorizable loop
    });
  });

  WorkerPool pool{8};                            // 8 lanes (1 = plain serial)
  schedule.run(world, pool);                     // parallel rows fan out across lanes
}
