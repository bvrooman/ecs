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
#include "work_item.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace ecs::detail {

inline constexpr ArchetypeId kImperative {0xFFFF'FFFFu};

using SystemVector = IdVector<SystemId, SystemRecord>;

// Run one item. An imperative system receives `pool` -- the caller decides
// whether that is the real pool (lone-item shortcut) or the shared 1-lane one
// (inside a multi-item dispatch, where a nested dispatch would be an error).
inline void run_work_item(WorkItem const& it,
                          SystemVector& systems,
                          World& world,
                          Commands& cmds) {
    auto& system       = systems[it.system];
    recording_source() = system.id;
    system.run(world, cmds, it, it.ordinal);
}

} // namespace ecs::detail
