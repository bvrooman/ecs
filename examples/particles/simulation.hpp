// examples/particles/simulation.hpp
//
// The particle simulation, expressed as an ECS schedule. Kept separate from
// main so the renderer file stays focused on windowing and drawing.
#pragma once

namespace ecs {
class Schedule;
}

// Register the particle systems (emitter, gravity, age, integrate, reaper,
// extract) on `schedule`. They operate on the components/resources declared in
// particles.hpp and publish draw-ready frames to a
// TripleBuffer<RenderSnapshot> resource the caller must have installed.
void build_particle_schedule(ecs::Schedule& schedule);
