// ecs/schedule/executor.hpp
//
// The work-item executor: how one wave of conflict-free systems becomes a flat
// list of claimable items and how that list is driven across a WorkerPool's
// lanes. Pure mechanism -- no events, no command flushing, no abort handling;
// the Schedule (schedule.hpp) owns that policy and calls these two functions
// per wave.
//
//   * a kernel system contributes one item per row-range slice of each
//     archetype its component set matches (~lanes x 8 slices, floored at
//     kMinItemRows, so dynamic claiming absorbs differing per-row costs), and
//   * an imperative system contributes itself as a single opaque item.
//
// Items are sorted longest-first (greedy LPT -- the biggest work cannot become
// the join's straggler) with a deterministic tie-break, then claimed from an
// atomic cursor by every lane of ONE pool dispatch. Item contents and order
// are deterministic; item-to-lane assignment is not (a 1-lane pool claims in
// list order and replays identically).

#pragma once

#include "../parallel/worker_pool.hpp" // WorkerPool, parallel::serial_pool
#include "../world.hpp"
#include "system.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace ecs::detail {

// One unit of claimable work: a row-range of one kernel system's matched
// archetype, or (archetype == kImperative) an entire opaque system. `ordinal`
// is the item's index within ITS SYSTEM in generation order (archetypes
// ascending, rows ascending -- the serial-walk order); it survives the LPT
// sort and indexes the per-item slots of stateful kernel parameters, and
// barrier-time folds walk ordinals so results are canonical-ordered no matter
// which lane ran what.
struct WorkItem {
    SystemId system;       // index into the schedule's system list
    ArchetypeId archetype; // world archetype, or kImperative
    std::uint32_t begin, end;
    std::uint32_t ordinal;
    // Measured execution time of this item, written by the lane that ran it
    // (disjoint per item: no cross-lane contention); the Schedule rolls
    // items up per system into a SystemWork event.
    double busy_us = 0;
};
inline constexpr ArchetypeId kImperative {0xFFFF'FFFFu};

using SystemVector = IdVector<SystemId, SystemRecord>;

// Run one item. An imperative system receives `pool` -- the caller decides
// whether that is the real pool (lone-item shortcut) or the shared 1-lane one
// (inside a multi-item dispatch, where a nested dispatch would be an error).
inline void run_work_item(WorkItem const& it,
                          SystemVector& systems,
                          World& world,
                          Commands& cmds,
                          WorkerPool& pool) {
    auto& system       = systems[it.system];
    recording_source() = system.id;
    if (it.archetype == kImperative)
        system.run(world, cmds, pool);
    else
        system.run_range(world, cmds, it.archetype, it.begin, it.end, it.ordinal);
}

} // namespace ecs::detail
