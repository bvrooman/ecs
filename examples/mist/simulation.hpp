// examples/mist/simulation.hpp
//
// The mist simulation, expressed as an ECS schedule. Kept separate from main so
// the renderer file stays focused on windowing and drawing.
#pragma once

#include "mist.hpp" // MistInput

namespace ecs {
class Schedule;
}

// Register the mist systems on `schedule`: a one-shot scatter of `count`
// particles, then per tick input -> clock -> flow -> integrate -> extract. They
// operate on the components/resources in mist.hpp and publish draw-ready frames
// to a TripleBuffer<RenderSnapshot> resource the caller must have installed.
// `in` points at the atomics the main thread updates with the live cursor.
void build_mist_schedule(ecs::Schedule& schedule, MistInput in, int count);
