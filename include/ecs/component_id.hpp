// ecs/component_id.hpp
//
// ComponentId: the stable, process-local id assigned to each component type on
// first use (see component_id<T> in entity.hpp). A strong type so a component
// id can't be confused with a resource id, a row index, an archetype position,
// or any other integer -- they are all bare integers otherwise.
//
// It lives in its own leaf header so the archetype/storage core and the dynamic
// registry can name the type without pulling in the reflection machinery that
// mints it. Core id vocabulary, like Entity.

#pragma once

#include <ecs/strong_id.hpp> // StrongId

#include <cstdint>

namespace ecs {

using ComponentId = StrongId<struct ComponentIdTag, std::uint32_t>;

} // namespace ecs
