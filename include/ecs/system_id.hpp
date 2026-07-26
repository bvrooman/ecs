// ecs/system_id.hpp
//
// SystemId: the strong, monotonic id the scheduler assigns to each registered
// system (0 is the "no system" sentinel it never hands out).
//
// It lives in its own tiny leaf header -- rather than inside the scheduler --
// so the command buffer can tag each deferred command with the system that
// recorded it (flush attribution) using the real type, without taking a
// dependency on the scheduler. A SystemId is core id vocabulary, like Entity.

#pragma once

#include <ecs/strong_id.hpp> // StrongId
#include <cstdint>

namespace ecs {

using SystemId = StrongId<struct SystemIdTag, std::uint32_t>;

} // namespace ecs
