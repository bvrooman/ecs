// ecs/archetype_id.hpp
//
// ArchetypeId: a stable handle to an archetype table within a World. It is the
// archetype's position in the World's archetype vector -- assigned once, on
// creation, and never reused or reordered (archetypes are append-only for the
// World's lifetime), so the handle stays valid for as long as the World does.
//
// A strong type over that index: it still indexes the backing vector (read
// `.value` at those sites), but as a distinct type it can't be crossed with a
// row index, a component id, a lane, or any other integer, and it documents
// intent at every boundary that passes an archetype around.

#pragma once

#include <ecs/strong_id.hpp> // StrongId

#include <cstdint>

namespace ecs {

using ArchetypeId = StrongId<struct ArchetypeIdTag, std::uint32_t>;

} // namespace ecs
