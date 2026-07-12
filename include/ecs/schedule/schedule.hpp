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
//     ResMut<T>, Commands& (deferred edits), WorldView (reads everything).
//     The scheduler derives the read/write sets from the parameters and
//     constructs them when the system runs, so the declared access cannot
//     drift from actual use. Opaque to the executor: one work item. (Raw
//     World& is deliberately not a system parameter -- see system.hpp.)
//
//   * kernel (add_kernel): the SAME signature rules, with exactly one Query
//     parameter -- whose iteration is VISIBLE, so the executor slices it into
//     work items, binding the Query restricted to each item's rows.
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
#include "kernel_params.hpp"
#include "system.hpp"
#include <memory>
#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
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

    // Register a KERNEL system. Same signature rules as add() -- the system's
    // parameter types declare its access -- with one structural requirement:
    // exactly ONE Query<Cs...> parameter. That query is the iteration the
    // executor slices: the system body is invoked once per work item with the
    // Query restricted to that item's rows, so `q.for_each_chunk(...)` /
    // `q.for_each_serial(...)` inside iterate just the slice, inline on the
    // claiming lane. An imperative add() system with independent per-row work
    // becomes a kernel system by changing one word:
    //
    //   sched.add_kernel("steer",
    //       [](Query<const Position, Velocity> q, Res<Clock> clk) {
    //           q.for_each_serial([&](auto& p, auto& v) { ... });
    //       });
    //
    // Unlike an imperative system (an opaque callable the executor must run
    // whole), a kernel system's iteration is VISIBLE to the scheduler: its
    // matched rows are sliced into work items and overlapped with the rest of
    // the wave's items across the pool's lanes (see run()). Items of one
    // system cover disjoint rows and cross-system conflicts are leveled, so
    // the same race-freedom argument as for_each_chunk applies -- but the
    // body must be independent per-row work: it runs CONCURRENTLY with the
    // system's own other items. For that reason ResMut<T> is rejected here
    // (writes through it would race between the system's own items -- the
    // conflict analysis only serializes OTHER systems). The allowed parameters
    // besides the Query: Res<T> (shared read), Commands& (per-item recording,
    // replayed in canonical order), WorldView (read-only), and the per-item
    // primitives from
    // schedule/params/ -- Reduce<T, Op> (private partials folded at the
    // barrier), Extract<T> (disjoint spans over a pre-sized buffer),
    // Collect<T> (filtered gather, concatenated at the barrier),
    // EventWriter<T>/EventReader<T> (double-buffered Events<T> channel),
    // Bin<V> (group-by into contiguous per-bucket spans),
    // Scratch<T> (private workspace), Random (deterministic per-item stream).
    // Ordered iteration and effects still belong in add() systems. The sliced
    // Query is bound to the shared 1-lane pool, so nothing a kernel body does
    // can reach a nested dispatch.
    template <class Fn, int P = 0>
    SystemId add_kernel(std::string name, Fn&& fn, phase<P> = {}) {
        return emplace_kernel(std::move(name), std::forward<Fn>(fn), P);
    }

    // Register a MAINTENANCE hook: `fn(World&)` runs single-threaded on the
    // calling thread at the START of every `every_n`-th run(), BEFORE any
    // wave, while the world is quiescent. This is the sanctioned place for
    // work that is structural and therefore cannot be a system -- the
    // canonical use is data-layout upkeep like World::sort_rows every N
    // ticks (see docs/IMPROVEMENTS.md, parallel-primitives section), or
    // capacity trims. Hooks run in registration order, emit no schedule
    // events, and a throwing hook propagates out of run() before the tick
    // does any work.
    template <class Fn>
    void add_maintenance(std::string name, std::uint64_t const every_n, Fn&& fn) {
        maintenance_.push_back(Maintenance {std::move(name),
                                            std::max<std::uint64_t>(1, every_n),
                                            std::forward<Fn>(fn)});
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
    void run(World& world) { run(world, parallel::serial_pool()); }

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
    // is not. Commands recorded by KERNEL systems replay in canonical order
    // regardless (per-item stores enqueued at the barrier in ordinal order --
    // see schedule/params/commands.hpp; spawn() handles are still reserved at
    // record time, so the IDs themselves remain timing-dependent). Commands
    // recorded by imperative systems keep the thread-sharded path, so their
    // cross-shard replay order -- already unspecified -- varies run to run.
    // A 1-lane pool claims items in list order and is fully deterministic.
    //
    // Recorded edits flush at each wave barrier; one-shot systems are removed
    // afterwards. A system that throws (in its body or a kernel item)
    // propagates out of run(); the aborted run's recorded edits are DISCARDED
    // (they must not leak into a later run's first flush, possibly of a
    // different schedule sharing this world) and a TickAbort event is emitted
    // in place of the remaining End events.
    void run(World& world, WorkerPool& pool) {
        rebuild();
        ++tick_; // seeds Random streams; a fresh tick is a fresh stream
        // Maintenance hooks: world quiescent, before any wave (see
        // add_maintenance).
        for (auto& m : maintenance_)
            if (tick_ % m.every == 0)
                m.fn(world);
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

    // The bodies behind add/add_once and add_kernel. Both follow the same
    // diagnostic pattern: each requirement is a static_assert with a
    // deliberate message, and the SAME condition guards the body as an
    // `if constexpr` -- not redundancy, load-bearing: a failed static_assert
    // does not stop the compiler from instantiating the rest of the function,
    // and the body hard-errors on exactly the inputs the asserts reject
    // (system_args_t of a generic lambda, system_param<P> of a non-parameter,
    // tuple_element of a missing Query), which would bury the message under a
    // cascade. The guard makes the assert's message the ONLY diagnostic; the
    // lone fallback return below the guard exists so the already-ill-formed
    // instantiation doesn't add a missing-return error on top.

    template <class Fn>
    SystemId emplace(std::string name, Fn&& fn, bool const once, int const phase) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        if constexpr (detail::IntrospectableSystem<Fn>) {
            using Args = detail::system_args_t<Fn>;
            static_assert(detail::imperative_params_info<Args>::all_allowed,
                          "unsupported system parameter type: a system may take "
                          "Query<Cs...>, Res<T>, ResMut<T>, Commands&, WorldView, "
                          "and Local<T> (spelled exactly so -- e.g. Query by "
                          "value, Commands by reference); raw World& is "
                          "deliberately not a system parameter, and the per-item "
                          "primitives (Reduce/Extract/Collect/EventWriter/"
                          "EventReader/Scratch/Random) are kernel-only (register "
                          "with add_kernel)");
            if constexpr (detail::imperative_params_info<Args>::all_allowed) {
                constexpr auto N = std::tuple_size_v<Args>;
                using Info       = detail::imperative_params_info<Args>;
                using Seq        = std::make_index_sequence<N>;
                System sys;
                sys.name  = std::move(name);
                sys.phase = phase;
                sys.once  = once;
                detail::imperative_declare<Args>(sys.access, Seq {});
                if constexpr (Info::any_stateful) {
                    // Per-system state (Local<T>): lives with the closure, so
                    // it persists exactly as long as the system is registered.
                    auto states = std::make_shared<typename Info::states>();
                    sys.run     = [fn = std::forward<Fn>(fn),
                               states](World& w, Commands& c, WorkerPool& pool) mutable {
                        detail::imperative_invoke<Args>(fn, *states, w, c, pool,
                                                        Seq {});
                    };
                } else {
                    sys.run = [fn = std::forward<Fn>(fn)](World& w,
                                                          Commands& c,
                                                          WorkerPool& pool) mutable {
                        typename Info::states st;
                        detail::imperative_invoke<Args>(fn, st, w, c, pool, Seq {});
                    };
                }
                return register_system(std::move(sys));
            }
        }
        return SystemId {0}; // reached only when a static_assert above fired
    }

    template <class Fn>
    SystemId emplace_kernel(std::string name, Fn&& fn, int const phase) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        if constexpr (detail::IntrospectableSystem<Fn>) {
            using Args = detail::system_args_t<Fn>;
            static_assert(detail::query_info<Args>::count == 1,
                          "add_kernel: the system must take exactly ONE Query<Cs...> "
                          "parameter -- it is the iteration the executor slices into "
                          "work items (use add() for zero or several queries)");
            static_assert(!detail::any_res_mut_v<Args>,
                          "add_kernel: ResMut<T> is not allowed in a kernel system -- "
                          "its work items run concurrently, so writes through ResMut "
                          "would race. Read resources via Res<T>; fold shared state "
                          "with Reduce<T, Op>, write gather-shaped output with "
                          "Extract<T> (see schedule/params/)");
            // `|| any_res_mut` so a ResMut (which also fails all_allowed)
            // fires only its own, more specific assert above.
            static_assert(detail::kernel_params_info<Args>::all_allowed ||
                              detail::any_res_mut_v<Args>,
                          "add_kernel: unsupported kernel parameter type -- a kernel "
                          "system may take one Query<Cs...>, plus Res<T>, Commands&, "
                          "WorldView, Reduce<T, Op>, Extract<T>, Collect<T>, "
                          "EventWriter<T>, EventReader<T>, Bin<V>, Scratch<T>, and "
                          "Random (Local<T> is imperative-only: per-system state has "
                          "no race-free meaning across a kernel's concurrent items)");
            if constexpr (detail::query_info<Args>::count == 1 &&
                          !detail::any_res_mut_v<Args> &&
                          detail::kernel_params_info<Args>::all_allowed) {
                constexpr auto QI = detail::query_info<Args>::index;
                constexpr auto N  = std::tuple_size_v<Args>;
                using Q           = std::tuple_element_t<QI, Args>;
                using Info        = detail::kernel_params_info<Args>;
                using Seq         = std::make_index_sequence<N>;
                System sys;
                sys.name  = std::move(name);
                sys.phase = phase;
                detail::kernel_declare<Args>(sys.access, Seq {});
                sys.query_sig = detail::query_param_traits<Q>::signature();

                // Per-item slot state for stateful parameters (Reduce/Extract),
                // shared between the item bind and the barrier hooks; persists
                // for the system's lifetime so slot capacity is retained.
                auto states   = std::make_shared<typename Info::states>();
                sys.run_range = [fn = std::forward<Fn>(fn),
                                 states](World& w,
                                         Commands& cmds,
                                         std::uint32_t ai,
                                         std::size_t b,
                                         std::size_t e,
                                         std::uint32_t ord) mutable {
                    detail::kernel_invoke<Args, QI>(fn, *states, w, cmds, ai, b, e,
                                                    ord, Seq {});
                };
                if constexpr (Info::any_stateful) {
                    sys.prepare_items = [states](World& w,
                                                 std::span<std::uint32_t const> rows,
                                                 detail::KernelWaveContext const& ctx) {
                        detail::kernel_prepare_all<Args>(*states, w, rows, ctx, Seq {});
                    };
                    sys.finish_items = [states](World& w) {
                        detail::kernel_finish_all<Args>(*states, w, Seq {});
                    };
                }
                return register_system(std::move(sys));
            }
        }
        return SystemId {0}; // reached only when a static_assert above fired
    }

    // Build and execute one wave's item list; on a throw, discard the aborted
    // run's recorded edits and emit TickAbort (see run()).
    void run_wave(std::vector<std::size_t> const& wave,
                  World& world,
                  Commands& cmds,
                  WorkerPool& pool,
                  std::size_t lvl) {
        using namespace sched_event;
        detail::build_wave_items(items_, wave, systems_, world);
        prepare_hooks(wave, world);
        auto const lone = wave.size() == 1;
        if (lone)
            events_.emit(SystemBegin {systems_[wave[0]].id, systems_[wave[0]].name});
        try {
            detail::run_wave_items(items_, systems_, world, cmds, pool);
            // Barrier folds (Reduce et al.) run after the join and before the
            // command flush, single-threaded, in wave (registration) order;
            // skipped when the dispatch aborted.
            for (auto const idx : wave)
                if (systems_[idx].finish_items)
                    systems_[idx].finish_items(world);
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

    // Drive stateful kernel parameters' prepare hooks: for each hooked system,
    // hand it its items' row counts in ordinal order (slot sizing, target
    // pre-sizing, prefix sums). Single-threaded, before the wave's dispatch.
    // A hooked system with zero items still prepares (its reduce target must
    // reset to "empty reduction", its extract target resize to 0).
    void prepare_hooks(std::vector<std::size_t> const& wave, World& world) {
        for (auto const idx : wave) {
            auto& s = systems_[idx];
            if (!s.prepare_items)
                continue;
            rows_scratch_.clear();
            for (auto const& it : items_)
                if (it.system == idx && it.archetype != detail::kImperative) {
                    if (rows_scratch_.size() <= it.ordinal)
                        rows_scratch_.resize(it.ordinal + 1);
                    rows_scratch_[it.ordinal] = it.end - it.begin;
                }
            s.prepare_items(world,
                            rows_scratch_,
                            detail::KernelWaveContext {s.id, tick_});
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

    struct Maintenance {
        std::string name;
        std::uint64_t every = 1;
        detail::move_only_function<void(World&)> fn;
    };

    event::Emitter<ScheduleEvent> events_;
    std::vector<System> systems_;
    std::vector<Maintenance> maintenance_;
    std::vector<std::vector<std::size_t>> waves_;
    std::vector<detail::WorkItem> items_;     // per-wave scratch, capacity retained
    std::vector<std::uint32_t> rows_scratch_; // per-system ordinal rows, reused
    std::uint64_t tick_ = 0; // run() count; part of Random's stream identity
    SystemId next_id_ = 0;
    bool dirty_       = true;
};

} // namespace ecs
