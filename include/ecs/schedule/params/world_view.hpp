// ecs/schedule/params/world_view.hpp
//
// WorldView as a system parameter: read-only ad-hoc access. It reads
// everything but writes nothing, so it runs in parallel with other readers
// and is serialized only against writers.
//
// There is deliberately NO system_param<World&>: raw world access cannot be
// analyzed and is dangerous under any concurrent executor, so it is not a
// system parameter. Setup happens on the World directly (outside a run),
// mutation goes through Commands, ad-hoc reads through WorldView; a system
// registered via add_dynamic_serial may still declare itself `exclusive` when
// its access is genuinely unanalyzable.

#pragma once

#include <ecs/detail/work_item.hpp>
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/system.hpp> // system_param
#include <ecs/world.hpp>           // World, Commands, WorldView

namespace ecs::detail {

template <>
struct system_param<WorldView> {
    static void declare(SystemAccess& a) { a.reads_all = true; }
    static WorldView bind(World& w, Commands&, WorkItem const&) { return WorldView(w); }
};

} // namespace ecs::detail
