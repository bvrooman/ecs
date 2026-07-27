// ecs/schedule/wave_id.hpp

#pragma once

#include <ecs/strong_id.hpp> // StrongId

#include <cstdint>

namespace ecs::detail {

using WaveId = StrongId<struct WaveIdTag, std::uint32_t>;

} // namespace ecs
