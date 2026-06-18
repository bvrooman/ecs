// ecs/entity.hpp
//
// Entity handles and component type identity.

#pragma once

#include <cstdint>
#include <functional>

namespace ecs {

// A generational handle. `index` selects a slot in the world's entity table;
// `generation` is bumped on destruction so stale handles compare unequal to the
// live entity that later reuses the slot.
struct Entity {
  std::uint32_t index = 0;
  std::uint32_t generation = 0;

  friend bool operator==(Entity, Entity) = default;
};

inline constexpr Entity null_entity{0xFFFFFFFFu, 0xFFFFFFFFu};

// Stable, process-local id assigned to each component type on first use.
using ComponentId = std::uint32_t;

namespace detail {
inline ComponentId next_component_id() noexcept {
  static ComponentId counter = 0;
  return counter++;
}
} // namespace detail

template <class T>
inline const ComponentId component_id = detail::next_component_id();

} // namespace ecs

template <>
struct std::hash<ecs::Entity> {
  std::size_t operator()(ecs::Entity e) const noexcept {
    return (static_cast<std::size_t>(e.generation) << 32) ^ e.index;
  }
};
