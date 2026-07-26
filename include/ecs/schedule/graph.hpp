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
#include <ecs/system_id.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace ecs::detail {

// The schedule's registered systems, keyed by the SystemId it handed out.
using SystemVector = IdVector<SystemId, SystemRecord>;

// Charge one run against every system that was due this tick and retire the
// ones whose budget is spent (see remove()): they keep their slot so no other
// system's position -- and so no SystemId -- shifts.
//
// Dueness is recomputed here rather than counted in WavePlan::prepare, because
// prewarm() calls prepare() too and must not spend anyone's budget.
inline void prune_expired(SystemVector& systems, std::uint64_t const tick, bool& dirty) {
    for (auto& s : systems) {
        if (s.dead || s.times == 0 || tick % s.every != 0)
            continue;
        if (++s.runs >= s.times) {
            s.dead = true;
            dirty  = true;
        }
    }
}

// Assign wavefront levels intra-phase level = 1 + max(level) over earlier
// conflicting systems in the same phase (cross-phase ordering is handled by
// the phase barrier). Tombstoned systems are skipped -- they take no slot in
// any wave -- but their positions still count, so a live system keeps the
// same level whether or not an earlier system was removed.
inline void assign_levels(SystemVector& systems) {
    for (auto& system : systems)
        system.level = 0;
    for (auto&& [id, s1] : systems.enumerate()) {
        if (s1.dead)
            continue;
        auto level = SystemRecord::Level {0};
        for (auto const& s2 : systems | std::views::take(id.value)) {
            if (!s2.dead && s1.phase == s2.phase && conflicts(s1.access, s2.access))
                level = std::max(level, s2.level + 1);
        }
        s1.level = level;
    }
}

inline void build_wave_plans(SystemVector const& systems,
                             std::vector<WavePlan>& wave_plans) {
    using WaveGroup  = std::pair<SystemRecord::WaveKey, WavePlan>;
    auto search      = [](auto const& g) { return g.first; };
    auto wave_groups = std::vector<WaveGroup> {};
    for (auto&& [id, system] : systems.enumerate()) {
        if (system.dead)
            continue;
        auto key = system.wave_key();
        auto it  = std::ranges::find(wave_groups, key, search);
        if (it == wave_groups.end()) {
            wave_groups.emplace_back(key, WavePlan {});
            it = std::prev(wave_groups.end());
        }
        auto& wave = it->second;
        wave.push_back(id);
    }
    wave_plans.clear();
    wave_plans.reserve(wave_groups.size());
    std::ranges::sort(wave_groups, {}, search);
    std::ranges::copy(wave_groups | std::views::values | std::views::as_rvalue,
                      std::back_inserter(wave_plans));
}

} // namespace ecs::detail
