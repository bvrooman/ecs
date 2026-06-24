// ecs/entity.hpp
//
// Entity handles and component type identity.

#pragma once

#include "reflection/type_names.hpp"
#include <functional>

namespace ecs {

// A generational handle. `index` selects a slot in the world's entity table;
// `generation` is bumped on destruction so stale handles compare unequal to the
// live entity that later reuses the slot.
struct Entity {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    friend bool operator==(Entity, Entity) = default;
};

// Stable, process-local id assigned to each component type on first use.
using ComponentId = std::uint32_t;

namespace detail {
    inline ComponentId next_component_id() noexcept {
        static ComponentId counter = 0;
        return counter++;
    }
} // namespace detail

// Instantiating component_id<T> also registers T's name for tooling when
// ECS_REFLECT_NAMES is enabled (register_component_name is a no-op otherwise).
template <class T>
inline ComponentId const component_id =
    detail::register_component_name<T>(detail::next_component_id());

} // namespace ecs

template <>
struct std::hash<ecs::Entity> {
    auto operator()(ecs::Entity const e) const noexcept {
        // Pack into a 64-bit key first: std::size_t is 32-bit on ILP32 targets
        // (e.g. wasm32), where `generation << 32` would be undefined and drop the
        // generation. Narrow to size_t only for the bucket index.
        std::uint64_t const key =
            (static_cast<std::uint64_t>(e.generation) << 32) ^ e.index;
        return static_cast<std::size_t>(key);
    }
};
