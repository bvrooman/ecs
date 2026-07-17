// ecs/entity.hpp
//
// Entity handles and component type identity.

#pragma once

#include "component_id.hpp" // ComponentId
#include "reflection/type_names.hpp"
#include <atomic>
#include <compare>
#include <cstdint>
#include <functional>

namespace ecs {

// A generational handle. `index` selects a slot in the world's entity table;
// `generation` is bumped on destruction so stale handles compare unequal to the
// live entity that later reuses the slot.
struct Entity {
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    // A sentinel that can never be handed out by the world (indices are
    // allocated from 0 upward and 2^32-1 is unreachable in practice), for "no
    // entity" fields. Note Entity{} is NOT null -- it is a valid handle for the
    // first entity ever spawned.
    static constexpr Entity null() noexcept {
        return Entity {0xFFFF'FFFFu, 0xFFFF'FFFFu};
    }
    // True when this handle is not the null() sentinel.
    [[nodiscard]]
    explicit constexpr operator bool() const noexcept {
        return !(*this == null());
    }

    friend bool operator==(Entity, Entity)  = default;
    friend auto operator<=>(Entity, Entity) = default;
};

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
        static std::atomic<std::uint32_t> counter {0};
        return ComponentId {counter.fetch_add(1, std::memory_order_relaxed)};
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
