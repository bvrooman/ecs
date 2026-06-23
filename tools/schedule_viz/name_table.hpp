// tools/schedule_viz/name_table.hpp
//
// viz::NameTable -- optional component/resource id -> display-name override map.
// Besides the to_dot/to_svg entry points, this is the only public-facing helper.
// An id's label is resolved in order: an explicit entry here, then the reflected
// name from ecs::component_name/resource_name (populated when the ECS is built
// with ECS_REFLECT_NAMES), then a "c<id>"/"r<id>" placeholder. Component and
// resource ids are separate spaces, hence separate tables.

#pragma once

#include "ecs/entity.hpp"     // component_id
#include "ecs/resource.hpp"   // resource_id
#include "ecs/reflection/type_names.hpp" // component_name / resource_name
#include <cstdint>
#include <string>
#include <unordered_map>

namespace viz {

struct NameTable {
    std::unordered_map<std::uint32_t, std::string> comp, res;

    template <class C>
    NameTable& component(std::string n) {
        comp[ecs::component_id<C>] = std::move(n);
        return *this;
    }
    template <class R>
    NameTable& resource(std::string n) {
        res[ecs::resource_id<R>] = std::move(n);
        return *this;
    }
    std::string comp_name(std::uint32_t id) const {
        if (auto it = comp.find(id); it != comp.end())
            return it->second; // explicit override
        if (auto n = ecs::component_name(id); !n.empty())
            return std::string(n); // reflected (ECS_REFLECT_NAMES)
        return "c" + std::to_string(id);
    }
    std::string res_name(std::uint32_t id) const {
        if (auto it = res.find(id); it != res.end())
            return it->second; // explicit override
        if (auto n = ecs::resource_name(id); !n.empty())
            return std::string(n); // reflected (ECS_REFLECT_NAMES)
        return "r" + std::to_string(id);
    }
};

} // namespace viz
