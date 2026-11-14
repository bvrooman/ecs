// ecs/schedule/graph.hpp
//
// The graph half of the scheduler: turning a flat list of registered systems
// into wavefront levels and the wave plans the executor runs. Pulled out of
// schedule.hpp because these are pure transformations over the system vector
// -- they read and write it and nothing else about a Schedule.

#pragma once

#include <ecs/detail/container/id_vector.hpp>
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/system.hpp>
#include <ecs/schedule/wave.hpp>
#include <ecs/schedule/wave_id.hpp>
#include <ecs/system_id.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ecs::detail {

// The schedule's registered systems, keyed by the SystemId it handed out.
using SystemVector = IdVector<SystemId, SystemRecord>;

// Drop a dead system's callables -- and with them everything they own: the
// body's captures, Local<T> state, per-item partials and command stores. The
// record itself stays (name, access, phase) so its SystemId keeps its slot and
// tooling can still read it. Call only when the system cannot be executing.
inline void release(SystemRecord& s) noexcept {
    s.run           = nullptr;
    s.prepare_items = nullptr;
    s.finish_items  = nullptr;
    s.abort_items   = nullptr;
}

// Charge one run to each system of a wave that just completed -- its edits are
// in the world -- appending to `spent` the ones whose budget is now used up.
// Charged per wave, not per tick: if a later wave aborts the tick, a one-shot
// whose wave already flushed has still run, and must not run again.
//
// Only Schedule::run charges (prewarm() prepares waves too, and must not spend
// anyone's budget). `due` is the wave's due list from WavePlan::prepare.
inline void charge_runs(SystemVector& systems,
                        std::span<SystemId const> const due,
                        std::vector<SystemId>& spent) {
    for (auto const id : due) {
        auto& s = systems[id];
        if (s.dead || s.times == 0)
            continue;
        if (++s.runs >= s.times)
            spent.push_back(id);
    }
}

// Assign wavefront levels intra-phase level = 1 + max(level) over earlier
// conflicting systems in the same phase (cross-phase ordering is handled by
// the phase barrier). `live` lists the live systems in ascending id order --
// "earlier" is registration order -- so removed and retired systems cost
// nothing here, and a live system keeps the same level whether or not an
// earlier system was removed.
inline void assign_levels(SystemVector& systems, std::span<SystemId const> const live) {
    for (std::size_t i = 0; i < live.size(); ++i) {
        auto& s1   = systems[live[i]];
        auto level = SystemRecord::Level {0};
        for (auto const earlier : live.first(i)) {
            auto const& s2 = systems[earlier];
            if (s1.phase == s2.phase && conflicts(s1.access, s2.access))
                level = std::max(level, s2.level + 1);
        }
        s1.level = level;
    }
}

inline void build_wave_plans(SystemVector const& systems,
                             std::span<SystemId const> const live,
                             std::vector<WavePlan>& wave_plans) {
    using WaveGroup  = std::pair<SystemRecord::WaveKey, WavePlan>;
    auto search      = [](auto const& g) { return g.first; };
    auto wave_groups = std::vector<WaveGroup> {};
    for (auto const system_id : live) {
        auto const& system = systems[system_id];
        auto key           = system.wave_key();
        auto it            = std::ranges::find(wave_groups, key, search);
        if (it == wave_groups.end()) {
            wave_groups.emplace_back(key, WavePlan {});
            it = std::prev(wave_groups.end());
        }
        auto& wave = it->second;
        wave.push_back(system_id);
    }
    wave_plans.clear();
    wave_plans.reserve(wave_groups.size());
    std::ranges::sort(wave_groups, {}, search);
    std::ranges::copy(wave_groups | std::views::values | std::views::as_rvalue,
                      std::back_inserter(wave_plans));
    for (auto id = WaveId {}; auto& plan : wave_plans)
        plan.id = id++;
}

} // namespace ecs::detail
