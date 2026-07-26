#pragma once

#include <ecs/archetype_id.hpp>
#include <ecs/detail/container/small_vector.hpp>
#include <ecs/system_id.hpp>

#include <cstdint>

namespace ecs::detail {

// One row-range of a matched archetype: rows [begin, end) of `archetype`.
struct Unit {
    ArchetypeId archetype;
    std::uint32_t begin;
    std::uint32_t end;
};

// One claimable piece of a wave's work -- what a single lane runs at a time. A
// kernel (add_kernel) system is sliced into several items, each a ~grain-sized
// row range of one matched archetype (one Unit); a serial (add/add_once) system
// is one item whose `units` span every matched archetype. `units` has inline
// capacity 1 -- the kernel-slice case, the overwhelming majority of items, never
// allocates; only a serial system spanning >1 archetype spills to the heap.
// `ordinal` is the item's index within ITS SYSTEM in generation order
// (archetypes ascending, rows ascending -- the serial-walk order); it survives
// the LPT sort and indexes
// the per-item slots of stateful kernel parameters, and barrier-time folds walk
// ordinals so results are canonical-ordered no matter which lane ran what.
// begin/end bound the item's rows within its system -- their difference is its
// row count (what Query::count() returns).
struct WorkItem {
    SystemId system; // index into the schedule's system list
    SmallVector<Unit, 1> units;
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t ordinal;
    double busy_us = 0;
};

} // namespace ecs::detail
