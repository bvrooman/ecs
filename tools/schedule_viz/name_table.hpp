// tools/schedule_viz/name_table.hpp
//
// viz::NameTable -- optional component/resource id -> display-name map. Besides
// the to_dot/to_svg entry points, this is the only public-facing helper.
// Component and resource ids live in separate id spaces, so they get separate
// tables; unknown ids fall back to "c<id>" / "r<id>".

#pragma once

#include "ecs/entity.hpp"   // component_id
#include "ecs/resource.hpp" // resource_id
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
        auto it = comp.find(id);
        return it != comp.end() ? it->second : "c" + std::to_string(id);
    }
    std::string res_name(std::uint32_t id) const {
        auto it = res.find(id);
        return it != res.end() ? it->second : "r" + std::to_string(id);
    }
};

} // namespace viz
