// examples/nbody/simulation.hpp
//
// The naive O(n^2) n-body simulation, expressed as an ECS schedule. Kept
// separate from main so the harness file stays focused on measurement.
#pragma once

namespace ecs {
class Schedule;
class World;
}

// Install the resources the schedule reads and writes (BodyArray, Potential,
// Motion, Diag) and spawn `count` bodies into `world`. Both live here so a
// caller cannot install half the set -- a missing resource is a bind-time
// failure, not a compile error.
//
// The load-then-prewarm-then-loop pattern is:
//
//   seed_bodies(world, count);
//   build_nbody_schedule(schedule);
//   schedule.prewarm(world);              // pre-pay the kernels' first-tick state
//   for (;;) schedule.run(world, pool);   // every tick is a steady sim tick
//
// Seeding at load (rather than as a spawn-on-tick-0 system) is what lets
// prewarm size the per-item state before the loop, so tick 0 is not a
// cold-start outlier.
void seed_bodies(ecs::World& world, int count);

// Register the per-tick systems: gather -> forces -> integrate -> diagnostics.
// Nothing declares an order; the access derived from each system's parameter
// types levels them into four waves.
void build_nbody_schedule(ecs::Schedule& schedule);
