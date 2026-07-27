// ecs/schedule/wave_id.hpp
//
// A wave's identity within a tick: its 0-based position in the schedule's
// canonical (phase, level)-sorted plan order, which is also the order the
// waves run in. Assigned by build_wave_plans once the plans are sorted (see
// graph.hpp) -- NOT the wavefront level, which repeats across phases.
//
// Its own header because the schedule events carry it (events.hpp) and so do
// the observers in tools/, neither of which should have to pull in the wave
// executor to name it.

#pragma once

#include <ecs/strong_id.hpp> // StrongId

#include <cstdint>

namespace ecs {

using WaveId = StrongId<struct WaveIdTag, std::uint32_t>;

} // namespace ecs
