// ecs/detail/work_item.hpp
//
// One claimable slice of a wave. Here rather than under schedule/ because
// Query is built from a WorkItem: parking it with the scheduler made the core
// query type depend on schedule/, which pulled the whole scheduler in behind
// it. It is a leaf -- it needs no scheduler machinery, only ids.

#pragma once

#include <ecs/archetype_id.hpp>
#include <ecs/detail/container/small_vector.hpp>
#include <ecs/system_id.hpp>

#include <cstdint>

namespace ecs::parallel {
class WorkerPool; // only ever pointed at here; see WorkItem::pool
}

namespace ecs::detail {

// One row-range of a matched archetype: rows [begin, end) of `archetype`.
struct Unit {
    ArchetypeId archetype;
    std::uint32_t begin;
    std::uint32_t end;
};

// One claimable piece of a wave's work -- what a single lane runs at a time. A
// parallel (add_parallel) system is sliced into several items, each a ~grain-sized
// row range of one matched archetype (one Unit); a serial (add_serial) system
// is one item whose `units` span every matched archetype.
struct WorkItem {
    SystemId system; // index into the schedule's system list
    SmallVector<Unit, 1> units;
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t ordinal;
    double busy_us = 0;
    // The pool executing this item, so a system can fan work out onto the lanes
    // it is already running on (the Exec parameter). Dispatching is only legal
    // when the item is the wave's ONLY one -- see schedule/params/exec.hpp,
    // which is what makes Exec declare its system exclusive.
    parallel::WorkerPool* pool = nullptr;
};

} // namespace ecs::detail
