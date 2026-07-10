// ecs/schedule/schedule.hpp
//
// The Schedule: registration, conflict-driven wave building, and the run loop.
//
// A *system* is registered in one of two forms:
//
//   * imperative (add/add_once/add_dynamic): a callable whose PARAMETER TYPES
//     declare what it touches --
//       void physics(Query<Position, const Velocity> q, ResMut<Gravity> g);
//     Query<Cs...> (const component = read, non-const = write), Res<T>/
//     ResMut<T>, Commands& (deferred edits), WorldView (reads everything),
//     World& (exclusive). The scheduler derives the read/write sets from the
//     parameters and constructs them when the system runs, so the declared
//     access cannot drift from actual use. Opaque to the executor: one work
//     item.
//
//   * kernel (add_kernel<Cs...>): a component list plus a chunk kernel --
//     the declarative for_each_chunk. Same access derivation, but the
//     iteration is VISIBLE, so the executor slices it into work items.
//
// Conflicting systems (one writes what another reads or writes) are assigned
// wavefront *levels*; each wave is conflict-free and executed by the work-item
// executor (executor.hpp) with a single pool dispatch. Recorded edits flush at
// each wave barrier. A `phase<N>` tag gives coarse ordering across barriers
// independent of conflicts.

#pragma once

#include "../event/emitter.hpp"
#include "../query.hpp"
#include "../world.hpp"
#include "access.hpp"
#include "events.hpp"
#include "executor.hpp"
#include "system.hpp"
#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ecs {

class Schedule {
public:
    // The stored record type, exposed for tooling (systems() hands out a
    // vector of these; the visualizer reads name/access/phase/level).
    using System = detail::SystemRecord;

    // Register an imperative system. Its access is derived from its parameter
    // types; an optional trailing phase<N> tag gives coarse ordering (default
    // phase 0). Returns a handle remove() can unschedule. Example:
    //   sched.add("integrate", [](Query<Position, const Velocity> q){ ... });
    //   sched.add("startup",   setup_fn, phase<-1>{});
    template <class Fn, int P = 0>
    SystemId add(std::string name, Fn&& fn, phase<P> = {}) {
        return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/false, P);
    }

    // One-shot system: runs on the next run() and is then removed (e.g. setup).
    template <class Fn, int P = 0>
    SystemId add_once(std::string name, Fn&& fn, phase<P> = {}) {
        return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/true, P);
    }

    // Register a KERNEL system: the declarative form of the for_each_chunk
    // pattern -- a component list plus a chunk kernel, invoked as
    //   fn(std::span<Entity>, chunk<Cs>...)
    // over row ranges the executor chooses. Access is derived from Cs exactly
    // like Query<Cs...> (const = read, non-const = write).
    //
    // Unlike an imperative add() system (an opaque callable the executor must
    // run whole), a kernel system's iteration is VISIBLE to the scheduler: the
    // executor slices its matched rows into work items and overlaps them with
    // the rest of the wave's items across the pool's lanes (see run()). Kernel
    // ranges of one system are disjoint and cross-system conflicts are leveled,
    // so the same race-freedom argument as for_each_chunk applies -- but the
    // kernel must still be independent per-row work (no shared mutable state).
    // A kernel system holds no Query and no pool, so the nested-dispatch error
    // is unreachable in this form. Prefer add_kernel for data-parallel hot
    // systems; use add() when a system needs Commands, resources, WorldView,
    // reductions, or ordered iteration.
    template <class... Cs, class Fn, int P = 0>
    SystemId add_kernel(std::string name, Fn&& fn, phase<P> = {}) {
        static_assert(sizeof...(Cs) > 0,
                      "add_kernel<Cs...>: at least one component type required");
        static_assert(detail::are_distinct_v<std::remove_const_t<Cs>...>,
                      "add_kernel<Cs...>: duplicate component type (ignoring const)");
        static_assert(
            std::is_invocable_v<std::decay_t<Fn>&, std::span<Entity>, chunk<Cs>...>,
            "add_kernel<Cs...> kernel must be callable as "
            "fn(std::span<Entity>, chunk<Cs>...)");
        System sys;
        sys.name = std::move(name);
        (detail::declare_component<Cs>(sys.access), ...);
        sys.phase     = P;
        sys.query_sig = [] {
            auto v = Signature {component_id<std::remove_const_t<Cs>>...};
            std::ranges::sort(v);
            return v;
        }();
        sys.run_range = [fn = std::forward<Fn>(fn)](World& w,
                                                    std::uint32_t ai,
                                                    std::size_t b,
                                                    std::size_t e) mutable {
            auto& arch = *w.archetypes()[ai];
            fn(std::span(arch.entities).subspan(b, e - b),
               detail::make_chunk<Cs>(arch, b, e)...);
        };
        return register_system(std::move(sys));
    }

    // Register a system whose access is *declared* (a runtime SystemAccess)
    // rather than derived from C++ parameter types -- the entry point for a
    // JS-defined system. `run` is the type-erased body (typically a runtime query
    // that dispatches per chunk) with the usual (World&, Commands&, WorkerPool&)
    // signature. It conflicts and levels against every other system by `access`
    // exactly like a native one, so JS and C++ systems share one wave plan.
    template <class Run>
    SystemId add_dynamic(std::string name,
                         SystemAccess access,
                         Run&& run,
                         int phase = 0,
                         bool once = false) {
        System sys;
        sys.name   = std::move(name);
        sys.access = std::move(access); // caller-supplied ids: normalized below
        sys.phase  = phase;
        sys.once   = once;
        sys.run    = std::forward<Run>(run);
        return register_system(std::move(sys));
    }

    // Unschedule a system by handle. Returns true if it was present.
    bool remove(SystemId id) {
        auto const n =
            std::erase_if(systems_, [id](System const& s) { return s.id == id; });
        if (n)
            dirty_ = true;
        return n != 0;
    }

    [[nodiscard]]
    auto size() const noexcept {
        return systems_.size();
    }
    // Number of sequential waves (barriers) a run() executes: ascending
    // (phase, conflict-level) groups.
    [[nodiscard]]
    auto level_count() {
        rebuild();
        return waves_.size();
    }
    [[nodiscard]]
    auto const& systems() const {
        return systems_;
    }

    // The conflict predicate the wavefront leveling uses; forwards to
    // ecs::conflicts (schedule/access.hpp). Kept on Schedule for tooling that
    // spells it Schedule::conflicts.
    static bool conflicts(SystemAccess const& a, SystemAccess const& b) {
        return ecs::conflicts(a, b);
    }

    // Run serially on the calling thread -- the shared 1-lane pool (which has
    // no worker threads and no dispatch state, so this is free). The way to do
    // setup: add_once a setup system, then run(world).
    void run(World& world) { run(world, detail::serial_pool()); }

    // The WORK-ITEM executor (see executor.hpp for the mechanism). Each wave
    // is flattened into one item list -- kernel systems sliced into row
    // ranges, imperative systems one opaque item each -- and executed with a
    // single pool dispatch, lanes claiming items dynamically. A heavy kernel
    // system's slices overlap both with each other AND with the wave's other
    // systems: data parallelism within and across systems from one fork-join.
    //
    // The single-system wave short-circuits to run that system inline on the
    // caller with the full pool, which preserves exact per-system observer
    // timing there; multi-system waves emit balanced Begin/End pairs around
    // the wave instead (observers are not thread-safe).
    //
    // Determinism: item contents and order are fixed; item-to-lane assignment
    // is not, so the cross-shard replay order of Commands recorded by
    // different systems in one wave -- already unspecified -- varies run to
    // run. A 1-lane pool claims items in list order and is fully
    // deterministic.
    //
    // Recorded edits flush at each wave barrier; one-shot systems are removed
    // afterwards. A system that throws (in its body or a kernel item)
    // propagates out of run(); the aborted run's recorded edits are DISCARDED
    // (they must not leak into a later run's first flush, possibly of a
    // different schedule sharing this world) and a TickAbort event is emitted
    // in place of the remaining End events.
    void run(World& world, WorkerPool& pool) {
        rebuild();
        using namespace sched_event;
        events_.emit(TickBegin {waves_.size()});
        auto cmds       = Commands {world};
        std::size_t lvl = 0;
        for (auto const& wave : waves_) {
            events_.emit(WaveBegin {lvl, wave.size()});
            run_wave(wave, world, cmds, pool, lvl);
            try {
                world.apply_commands(); // cleans up its own pending commands on throw
            } catch (...) {
                events_.emit(TickAbort {lvl, SystemId {0}});
                throw;
            }
            events_.emit(WaveEnd {lvl});
            ++lvl;
        }
        events_.emit(TickEnd {});
        prune_once();
    }

    [[nodiscard]]
    auto& events(this auto& self) noexcept {
        return self.events_;
    }

private:
    // Stamp an id, normalize the access sets, and store -- the single funnel
    // every add_* form goes through.
    SystemId register_system(System sys) {
        sys.id = ++next_id_;
        detail::normalize_access(sys.access);
        auto const id = sys.id;
        systems_.push_back(std::move(sys));
        dirty_ = true;
        return id;
    }

    template <class Fn>
    SystemId emplace(std::string name, Fn&& fn, bool const once, int const phase) {
        using Args       = detail::fn_traits<std::decay_t<Fn>>::args;
        constexpr auto N = std::tuple_size_v<Args>;
        System sys;
        sys.name  = std::move(name);
        sys.phase = phase;
        sys.once  = once;
        detail::declare_params<Args>(sys.access, std::make_index_sequence<N> {});
        sys.run =
            [fn = std::forward<Fn>(fn)](World& w, Commands& c, WorkerPool& pool) mutable {
                detail::invoke_with_params<Args>(
                    fn, w, c, pool, std::make_index_sequence<N> {});
            };
        return register_system(std::move(sys));
    }

    // Build and execute one wave's item list; on a throw, discard the aborted
    // run's recorded edits and emit TickAbort (see run()).
    void run_wave(std::vector<std::size_t> const& wave,
                  World& world,
                  Commands& cmds,
                  WorkerPool& pool,
                  std::size_t lvl) {
        using namespace sched_event;
        detail::build_wave_items(items_, wave, systems_, world, pool.lanes());
        auto const lone = wave.size() == 1;
        if (lone)
            events_.emit(SystemBegin {systems_[wave[0]].id, systems_[wave[0]].name});
        try {
            detail::run_wave_items(items_, systems_, world, cmds, pool);
        } catch (...) {
            world.discard_commands();
            events_.emit(TickAbort {lvl, lone ? systems_[wave[0]].id : SystemId {0}});
            throw;
        }
        if (lone) {
            events_.emit(SystemEnd {systems_[wave[0]].id});
        } else {
            // Balanced pairs for observers; per-system durations are not
            // individually meaningful in a fanned-out wave (wave timing is).
            for (auto const idx : wave) {
                events_.emit(SystemBegin {systems_[idx].id, systems_[idx].name});
                events_.emit(SystemEnd {systems_[idx].id});
            }
        }
    }

    void prune_once() {
        if (std::erase_if(systems_, [](System const& s) { return s.once; }))
            dirty_ = true;
    }

    // Assign wavefront levels and group systems into waves: intra-phase level
    // = 1 + max(level) over earlier conflicting systems in the same phase
    // (cross-phase ordering is handled by the phase barrier); waves run in
    // ascending (phase, level) order with a barrier between each.
    void rebuild() {
        if (!dirty_)
            return;
        for (std::size_t i = 0; i < systems_.size(); ++i) {
            std::size_t lvl = 0;
            for (std::size_t j = 0; j < i; ++j)
                if (systems_[j].phase == systems_[i].phase &&
                    conflicts(systems_[i].access, systems_[j].access))
                    lvl = std::max(lvl, systems_[j].level + 1);
            systems_[i].level = lvl;
        }
        // Flat vector of (key, members) instead of a std::map: no per-rebuild
        // node allocations, and the group count is small.
        using Key   = std::pair<int, std::size_t>;
        auto groups = std::vector<std::pair<Key, std::vector<std::size_t>>> {};
        for (std::size_t i = 0; i < systems_.size(); ++i) {
            Key const key {systems_[i].phase, systems_[i].level};
            auto it = std::ranges::find(groups, key, [](auto const& g) {
                return g.first;
            });
            if (it == groups.end()) {
                groups.emplace_back(key, std::vector<std::size_t> {});
                it = std::prev(groups.end());
            }
            it->second.push_back(i);
        }
        std::ranges::sort(groups, {}, [](auto const& g) { return g.first; });
        waves_.clear();
        waves_.reserve(groups.size());
        for (auto& [key, idxs] : groups)
            waves_.push_back(std::move(idxs));
        dirty_ = false;
    }

    event::Emitter<ScheduleEvent> events_;
    std::vector<System> systems_;
    std::vector<std::vector<std::size_t>> waves_;
    std::vector<detail::WorkItem> items_; // per-wave scratch, capacity retained
    SystemId next_id_ = 0;
    bool dirty_       = true;
};

} // namespace ecs
