// ecs/entity.hpp
//
// Entity handles and component type identity.

#pragma once

#include <ecs/component_id.hpp> // ComponentId
#include <ecs/detail/handle.hpp>
#include <ecs/reflection/type_names.hpp>

namespace ecs {

// An entity is a generational handle into the world's entity table: a tagged
// detail::Handle whose `index` selects a slot and whose `generation` is bumped
// on destruction, so stale handles compare unequal to the live entity that
// later reuses the slot. The world (via its HandleVector) is the only minter;
// Entity::from_raw rebuilds one from an external index+generation pair (e.g. a
// JS host passing an id back in). Handle provides null()/operator bool, the
// comparisons, and std::hash.
using Entity = detail::Handle<struct EntityTag>;

// ComponentId is defined in component_id.hpp (a leaf header the storage core
// and dynamic registry can include without the reflection machinery here).

// Instantiating component_id<T> also registers T's name for tooling when
// ECS_REFLECT_NAMES is enabled (register_component_name is a no-op otherwise).
template <class T>
inline ComponentId const component_id =
    detail::register_component_name<T>(ComponentId::next());

} // namespace ecs
