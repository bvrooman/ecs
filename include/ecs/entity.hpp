// ecs/entity.hpp
//
// Entity handles and component type identity.

#pragma once

#include "component_id.hpp" // ComponentId
#include "detail/handle.hpp"
#include "reflection/type_names.hpp"
#include <atomic>

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

namespace detail {
    // Atomic: component_id<T> instantiations are magic statics, but each guards
    // only its *own* initialization -- two distinct component types first
    // touched on two threads would otherwise race on the shared counter and
    // could be handed the same id (silently aliasing their columns). The counter
    // is a raw integer (std::atomic has no fetch_add for a class type); each
    // freshly allocated value is minted into a ComponentId before it is handed
    // out, so callers only ever see the strong type.
    inline ComponentId next_component_id() noexcept {
        static std::atomic<ComponentId::type> counter {0};
        return ComponentId {counter.fetch_add(1, std::memory_order_relaxed)};
    }
} // namespace detail

// Instantiating component_id<T> also registers T's name for tooling when
// ECS_REFLECT_NAMES is enabled (register_component_name is a no-op otherwise).
template <class T>
inline ComponentId const component_id =
    detail::register_component_name<T>(detail::next_component_id());

} // namespace ecs
