// ecs/reflection/type_names.hpp
//
// Optional component/resource type-name registry for tooling and diagnostics.
//
// Gated on ECS_REFLECT_NAMES (default 0). When 0 this header is nearly empty:
// the register hooks are no-ops and component_name()/resource_name() return ""
// -- the engine pays nothing. When 1, instantiating component_id<T> /
// resource_id<T> records reflect::type_name<T>() into a per-id-space registry,
// so tools can resolve a numeric id back to a readable name with no manual
// table. Enable it per-consumer (e.g. the schedule-viz target) or library-wide
// via the ECS_REFLECT_NAMES CMake option.
//
// Names are stored as string_view into persistent storage (string literals /
// reflected identifiers), so the registry never allocates for them.

#pragma once

#ifndef ECS_REFLECT_NAMES
#define ECS_REFLECT_NAMES 0
#endif

#include "../component_id.hpp" // ComponentId
#include "../resource_id.hpp"  // ResourceId
#include <cstdint>
#include <string_view>

#if ECS_REFLECT_NAMES
#include "reflect.hpp"
#include <unordered_map>
#endif

namespace ecs {

#if ECS_REFLECT_NAMES
namespace detail {
    inline std::unordered_map<std::uint32_t, std::string_view>& component_name_map() {
        static std::unordered_map<std::uint32_t, std::string_view> m;
        return m;
    }
    inline std::unordered_map<std::uint32_t, std::string_view>& resource_name_map() {
        static std::unordered_map<std::uint32_t, std::string_view> m;
        return m;
    }
} // namespace detail
#endif

namespace detail {
    // Called from component_id<T> / resource_id<T> initialisation. Records the
    // reflected name when ECS_REFLECT_NAMES is on; a transparent no-op otherwise.
    template <class T>
    inline ComponentId register_component_name(ComponentId id) {
#if ECS_REFLECT_NAMES
        component_name_map().emplace(id.value, reflect::type_name<T>());
#endif
        return id;
    }
    template <class T>
    inline ResourceId register_resource_name(ResourceId id) {
#if ECS_REFLECT_NAMES
        resource_name_map().emplace(id.value, reflect::type_name<T>());
#endif
        return id;
    }
} // namespace detail

// Readable name for a component / resource id, or "" if unknown (always "" when
// ECS_REFLECT_NAMES is off). Component and resource ids are separate spaces.
inline std::string_view component_name(std::uint32_t id) {
#if ECS_REFLECT_NAMES
    auto const& m = detail::component_name_map();
    auto it       = m.find(id);
    return it != m.end() ? it->second : std::string_view {};
#else
    (void)id;
    return {};
#endif
}
inline std::string_view resource_name(std::uint32_t id) {
#if ECS_REFLECT_NAMES
    auto const& m = detail::resource_name_map();
    auto it       = m.find(id);
    return it != m.end() ? it->second : std::string_view {};
#else
    (void)id;
    return {};
#endif
}

} // namespace ecs
