// ecs/schedule/params/query.hpp
//
// Query<Cs...> as a system parameter: component access derived from the type
// list (const => read, non-const => write), bound restricted to one work
// item's row range.

#pragma once

#include <ecs/component_id.hpp> // component_id
#include <ecs/detail/work_item.hpp>
#include <ecs/query.hpp> // Query, Signature
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/system.hpp> // system_param
#include <ecs/world.hpp>           // World, Commands

#include <type_traits>

namespace ecs::detail {

template <class C>
void declare_component(SystemAccess& a) {
    if constexpr (std::is_const_v<C>)
        a.reads.push_back(component_id<std::remove_const_t<C>>);
    else
        a.writes.push_back(component_id<C>);
}

template <class... Cs>
struct system_param<Query<Cs...>> {
    static void declare(SystemAccess& a) { (declare_component<Cs>(a), ...); }
    static Query<Cs...> bind(World& w, Commands&, WorkItem const& item) {
        return Query<Cs...>(w, item);
    }
    static Signature const& signature() { return Query<Cs...>::required(); }
};

} // namespace ecs::detail
