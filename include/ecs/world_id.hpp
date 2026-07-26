// ecs/world_id.hpp
//
// WorldId: the process-local instance id assigned to each World. It guards the
// per-thread query match caches against a destroyed World's address being
// reused by a later World (the cache keys on it alongside the archetype
// generation). A strong type so it can't be confused with an archetype
// generation, an entity index, or any other bare integer it sits next to.
//
// A leaf header so the schedule/query caches can name the type without pulling
// in all of world.hpp.

#pragma once

#include <ecs/detail/strong_id.hpp> // StrongId
#include <cstdint>

namespace ecs {

using WorldId = StrongId<struct WorldIdTag, std::uint64_t>;

} // namespace ecs
