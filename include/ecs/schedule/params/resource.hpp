// ecs/schedule/params/resource.hpp
//
// Res<T> / ResMut<T> as system parameters: a plain resource read and a plain
// resource write. The accumulating resource writes are Reduce and Collect,
// which declare a fold instead (reduce.hpp, collect.hpp).

#pragma once

#include <ecs/detail/work_item.hpp>
#include <ecs/resource_id.hpp> // resource_id
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/system.hpp> // system_param
#include <ecs/world.hpp>           // World, Commands, Res, ResMut

namespace ecs::detail {

template <class T>
struct system_param<Res<T>> {
    static void declare(SystemAccess& a) { a.res_reads.push_back(resource_id<T>); }
    static Res<T> bind(World& w, Commands&, WorkItem const&) { return Res<T>(w); }
};

template <class T>
struct system_param<ResMut<T>> {
    static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }
    static ResMut<T> bind(World& w, Commands&, WorkItem const&) { return ResMut<T>(w); }
};

} // namespace ecs::detail
