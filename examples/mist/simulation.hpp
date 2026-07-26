// examples/mist/simulation.hpp
//
// The mist simulation, expressed as an ECS schedule. Kept separate from main so
// the renderer file stays focused on windowing and drawing.
#pragma once

#include <ecs/sync/triple_buffer.hpp>
#include "mist.hpp" // MistInput, RenderSnapshot
#include <cstddef>

namespace ecs {
class Schedule;
class World;
}

// The `extract` kernel's target: the draw-ready snapshot it writes IS the
// triple buffer's back buffer -- resize/data/size forward straight to it, so
// the kernel's disjoint per-item spans write the GPU vertices in place (no
// intermediate copy). back() is stable for the whole wave; the `publish`
// system flips it to the renderer at the next barrier. Install as a resource
// pointing at the TripleBuffer resource (see main.cpp).
struct SnapshotTarget {
    ecs::TripleBuffer<RenderSnapshot>* channel = nullptr;
    void resize(std::size_t const n) { channel->back().resize(n); }
    GpuParticle* data() { return channel->back().data(); }
    [[nodiscard]]
    std::size_t size() const {
        return channel->back().size();
    }
};

// Register the mist per-tick systems on `schedule`: input -> clock -> goal ->
// grid -> steer -> integrate -> extract -> publish. They operate on the
// components/resources in mist.hpp and publish draw-ready frames to a
// TripleBuffer<RenderSnapshot> resource the caller must have installed (plus a
// SnapshotTarget pointing at it, and a FlockStats resource for the metrics
// reduction). `in` points at the atomics the main thread updates with the live
// cursor.
//
// The flock is NOT seeded here. The load-then-prewarm-then-loop pattern is:
//
//   seed_flock(world, count);            // load: spawn the birds
//   build_mist_schedule(schedule, in);
//   schedule.prewarm(world);             // pre-pay the kernels' first-tick state
//   for (;;) schedule.run(world, pool);  // every tick is a steady sim tick
//
// Seeding separately (rather than a scatter-on-tick-0 system) is what lets
// prewarm size the per-bird kernel state before the loop, so tick 0 is not a
// cold-start outlier -- and it mirrors how a real game separates level load
// from the frame loop.
void build_mist_schedule(ecs::Schedule& schedule, MistInput in);

// Spawn `count` birds into `world` now, as a one-shot schedule. Requires an Rng
// resource. Call before build_mist_schedule + prewarm (see the pattern above).
void seed_flock(ecs::World& world, int count);
