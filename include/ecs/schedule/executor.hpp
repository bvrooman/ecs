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
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace ecs::detail {

// One unit of claimable work: a row-range of one kernel system's matched
// archetype, or (archetype == kImperative) an entire opaque system.
struct WorkItem {
    std::uint32_t system;    // index into the schedule's system list
    std::uint32_t archetype; // world archetype index, or kImperative
    std::uint32_t begin, end;
};
inline constexpr std::uint32_t kImperative = 0xFFFF'FFFFu;
// A slice below this many rows is not worth a separate claim.
inline constexpr std::size_t kMinItemRows = 1024;

// Flatten one wave into `items` (capacity retained by the caller across waves
// and ticks: no steady-state allocation) and sort it into claim order.
inline void build_wave_items(std::vector<WorkItem>& items,
                             std::span<std::size_t const> wave,
                             std::span<SystemRecord> systems,
                             World const& world,
                             unsigned lanes) {
    items.clear();
    for (auto const idx : wave) {
        auto& s = systems[idx];
        if (!s.is_kernel()) {
            items.push_back({std::uint32_t(idx), kImperative, 0, 0});
            continue;
        }
        auto const& matches = s.match.resolve(world, s.query_sig);
        std::size_t total   = 0;
        for (auto const ai : matches)
            total += world.archetypes()[ai]->size();
        if (total == 0)
            continue;
        // ~lanes x 8 items per kernel system: enough oversubscription for
        // dynamic claiming to balance differing per-row kernel costs, with a
        // floor so an item always amortizes its claim. (An archetype smaller
        // than the grain still yields its own item -- a claim is one
        // fetch_add, far cheaper than the alternative bookkeeping.)
        std::size_t const grain =
            lanes <= 1
                ? total
                : std::max<std::size_t>(kMinItemRows, total / (std::size_t {lanes} * 8));
        for (auto const ai : matches) {
            auto const n = world.archetypes()[ai]->size();
            for (std::size_t b = 0; b < n; b += grain) {
                auto const e = std::min(n, b + grain);
                items.push_back(
                    {std::uint32_t(idx), ai, std::uint32_t(b), std::uint32_t(e)});
            }
        }
    }
    // Longest first (imperative items -- unknown cost -- ahead of everything);
    // deterministic tie-break so a 1-lane run replays identically.
    std::ranges::sort(items, [](WorkItem const& a, WorkItem const& b) {
        bool const ia = a.archetype == kImperative;
        bool const ib = b.archetype == kImperative;
        if (ia != ib)
            return ia;
        auto const ra = a.end - a.begin;
        auto const rb = b.end - b.begin;
        if (ra != rb)
            return ra > rb;
        return std::tie(a.system, a.archetype, a.begin) <
               std::tie(b.system, b.archetype, b.begin);
    });
}

// Run one item. An imperative system receives `pool` -- the caller decides
// whether that is the real pool (lone-item shortcut) or the shared 1-lane one
// (inside a multi-item dispatch, where a nested dispatch would be an error).
inline void run_work_item(WorkItem const& it,
                          std::span<SystemRecord> systems,
                          World& world,
                          Commands& cmds,
                          WorkerPool& pool) {
    auto& s = systems[it.system];
    if (it.archetype == kImperative)
        s.run(world, cmds, pool);
    else
        s.run_range(world, cmds, it.archetype, it.begin, it.end);
}

// Execute a built item list. A lone item runs inline on the caller (an
// imperative system then gets the FULL pool -- the common single-system wave
// keeps its within-system data parallelism); otherwise one dispatch, every
// lane claiming items from an atomic cursor, imperative items bound to the
// shared 1-lane pool so their queries iterate inline on their lane. Exceptions
// (from a system body or a kernel item) propagate out of the dispatch join;
// the caller owns abort policy.
inline void run_wave_items(std::vector<WorkItem> const& items,
                           std::span<SystemRecord> systems,
                           World& world,
                           Commands& cmds,
                           WorkerPool& pool) {
    if (items.empty())
        return;
    if (items.size() == 1) {
        run_work_item(items[0], systems, world, cmds, pool);
        return;
    }
    std::atomic<std::size_t> next {0};
    auto& inner = parallel::serial_pool(); // 1-lane: dispatch-free, shareable
    pool.parallel_for(items.size(), /*min_parallel=*/2, [&](std::size_t, std::size_t) {
        // Every lane claims items until none remain; the handed [b, e) slice
        // is ignored in favor of dynamic claims.
        for (auto i = next.fetch_add(1, std::memory_order_relaxed); i < items.size();
             i = next.fetch_add(1, std::memory_order_relaxed))
            run_work_item(items[i], systems, world, cmds, inner);
    });
}

} // namespace ecs::detail
