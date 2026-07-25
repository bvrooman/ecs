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

#include "../detail/id_vector.hpp"
#include "../event/emitter.hpp"
#include "../parallel/worker_pool.hpp"
#include "../query.hpp"
#include "../world.hpp"
#include "access.hpp"
#include "events.hpp"
#include "kernel_params.hpp"
#include "params/imperative.hpp"
#include "system.hpp"
#include "wave.hpp"
#include <algorithm>
#include <cstddef>
#include <memory>
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
    using Wave   = std::vector<SystemId>;

    // Register an imperative system. Its access is derived from its parameter
    // types; an optional trailing phase<N> tag gives coarse ordering (default
    // phase 0), and an optional trailing `every` runs the system only on ticks
    // where tick % every == 0 (default 1 = every tick -- e.g. re-sort the flock
    // every 64 ticks). Returns a handle remove() can unschedule. Example:
    //   sched.add("integrate", [](Query<Position, const Velocity> q){ ... });
    //   sched.add("startup",   setup_fn, phase<-1>{});
    //   sched.add("resort", [](Commands& c){ c.sort<Position>(cell); },
    //             phase<-1>{}, /*every=*/64);
    template <class Fn, int P = 0>
    SystemId add(std::string name, Fn&& fn, phase<P> = {}, std::uint64_t every = 1) {
        return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/false, P, every);
    }

    // One-shot system: runs on the next run() and is then removed (e.g. setup).
    template <class Fn, int P = 0>
    SystemId add_once(std::string name, Fn&& fn, phase<P> = {}) {
        return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/true, P, 1);
    }

    // Register a KERNEL system. Same signature rules as add() -- the system's
    // parameter types declare its access -- with one structural requirement:
    // exactly ONE Query<Cs...> parameter. That query is the iteration the
    // executor slices: the system body is invoked once per work item with the
    // Query restricted to that item's rows, so `q.for_each_chunk(...)` /
    // `q.for_each(...)` inside iterate just the slice, inline on the
    // claiming lane. An imperative add() system with independent per-row work
    // becomes a kernel system by changing one word:
    //
    //   sched.add_kernel("steer",
    //       [](Query<const Position, Velocity> q, Res<Clock> clk) {
    //           q.for_each([&](auto& p, auto& v) { ... });
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
    SystemId add_kernel(std::string name,
                        Fn&& fn,
                        phase<P>            = {},
                        std::uint64_t every = 1) {
        return emplace_kernel(std::move(name), std::forward<Fn>(fn), P, every);
    }

    // Register a system whose access is *declared* (a runtime SystemAccess)
    // rather than derived from C++ parameter types -- the entry point for a
    // JS-defined system. `run` is the type-erased body (typically a runtime query
    // that dispatches per chunk) with the usual (World&, Commands&, WorkItem const&)
    // signature. It conflicts and levels against every other system by `access`
    // exactly like a native one, so JS and C++ systems share one wave plan.
    //
    // Serial: the scheduler emits ONE opaque work item, so `run` is invoked once
    // per tick and must iterate every matching archetype itself. The WorkItem it
    // is handed carries no row slice (a serial system has no query_sig for the
    // wave to build one from), so `run` ignores it.
    template <class Run>
    SystemId add_dynamic(std::string name,
                         SystemAccess access,
                         Run&& run,
                         int phase = 0,
                         bool once = false) {
        return emplace_dynamic(std::move(name),
                               std::move(access),
                               Signature::null(),
                               std::forward<Run>(run),
                               phase,
                               once,
                               /*is_parallel=*/false);
    }

    // Parallel counterpart of add_dynamic -- the dynamic mirror of add_kernel.
    // The scheduler resolves `query_sig`'s matched archetypes, slices them into
    // row-range work items, and fans those across the pool, invoking `run` once
    // per slice. `run` reads its WorkItem for the archetype and [begin, end) rows
    // to process (see Wave::build_parallel); it must NOT self-iterate or
    // self-dispatch -- the pool is already fanned, and a nested parallel_for on
    // it would throw. `query_sig` is REQUIRED (a null signature matches nothing,
    // so the scheduler would build zero items and never call `run`).
    template <class Run>
    SystemId add_dynamic_parallel(std::string name,
                                  SystemAccess access,
                                  Signature query_sig,
                                  Run&& run,
                                  int phase = 0,
                                  bool once = false) {
        return emplace_dynamic(std::move(name),
                               std::move(access),
                               std::move(query_sig),
                               std::forward<Run>(run),
                               phase,
                               once,
                               /*is_parallel=*/true);
    }

    // Unschedule a system by handle. Returns true if it was present (and live).
    // Tombstoned, not erased: the slot stays put so every other system keeps its
    // position, and a SystemId (which IS that position) is never invalidated by
    // an unrelated removal.
    bool remove(SystemId id) {
        for (auto& s : systems_)
            if (s.id == id && !s.dead) {
                s.dead = true;
                dirty_ = true;
                return true;
            }
        return false;
    }

    // Live (non-tombstoned) system count. Removed and spent one-shot systems
    // retain their slot but do not count here.
    [[nodiscard]]
    std::size_t size() const noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(systems_, [](System const& s) { return !s.dead; }));
    }
    // Number of sequential waves (barriers) a run() executes: ascending
    // (phase, conflict-level) groups.
    [[nodiscard]]
    auto level_count() {
        rebuild();
        return wave_plans_.size();
    }
    [[nodiscard]]
    auto const& systems() const {
        return systems_;
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
    // caller with the full pool (a fanned wave's items interleave across the
    // lanes, so there is no per-system wall interval to observe). Either way,
    // every system reports MEASURED work via a SystemWork event: busy time
    // summed over its items (recorded by the claiming lanes) plus its barrier
    // prepare/finish hook durations.
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
        using namespace sched_event;
        events_.emit(TickBegin {wave_plans_.size()});
        auto cmds = Commands {world};
        auto lvl  = 0uz;
        for (auto& plan : wave_plans_) {
            auto sz = plan.prepare(systems_, tick_);
            if (sz > 0) {
                events_.emit(WaveBegin {lvl, sz});
                double const flush_us = run_wave(plan, world, cmds, pool, lvl);
                events_.emit(WaveEnd {lvl, flush_us});
            }
            ++lvl;
        }
        events_.emit(TickEnd {});
        prune_once();
    }

    [[nodiscard]]
    auto& events(this auto& self) noexcept {
        return self.events_;
    }

    // Pre-pay the first tick's one-time kernel-param initialization. For every
    // kernel system, this sizes the per-item slot state and runs the prepare
    // hooks (reduce targets reset, extract/collect/bin targets pre-size)
    // against the CURRENT, already-populated world -- so the first real run()
    // allocates nothing and its per-system busy times are steady from tick 0
    // instead of paying a cold outlier. It runs NO system bodies and NO barrier
    // folds, so it does not advance or otherwise mutate simulation state (the
    // targets it touches are reset every tick regardless) and emits no events.
    // Call once, after the world is populated (entities spawned) and before the
    // timed/rendered loop; idempotent.
    //
    // A kernel param whose partial holds fixed-size scratch (e.g. a dense
    // touched-cell index) should allocate that scratch in the partial's
    // constructor, not lazily on first use -- then sizing the slot array here
    // pre-pays it. Otherwise prewarm still covers the slot array and the
    // barrier targets, but the partial's own lazy heap growth lands on tick 0.
    void prewarm(World& world) {
        rebuild();
        for (auto& plan : wave_plans_) {
            plan.prepare(systems_, tick_);
            auto& wave = plan.build(systems_, world, tick_);
            wave.prepare(world);
        }
    }

private:
    // The single funnel every add_* form goes through: normalize the access
    // sets, append, and stamp the record's id to its slot position (push_back
    // owns id assignment). id == position -- and, because removal tombstones
    // rather than compacts (see remove()), it stays that way for the schedule's
    // life, which is what keeps a returned SystemId a stable handle.
    SystemId register_system(System sys) {
        detail::normalize_access(sys.access);
        auto const id   = systems_.push_back(std::move(sys));
        systems_[id].id = id;
        dirty_          = true;
        return id;
    }

    // Shared body of add_dynamic / add_dynamic_parallel: assemble a declared-access
    // System and hand it to register_system. is_parallel + query_sig decide how the
    // wave builds this system's work items (one opaque item vs. sliced row ranges).
    template <class Run>
    SystemId emplace_dynamic(std::string name,
                             SystemAccess access,
                             Signature query_sig,
                             Run&& run,
                             int phase,
                             bool once,
                             bool is_parallel) {
        System sys;
        sys.name        = std::move(name);
        sys.access      = std::move(access); // caller-supplied ids: normalized below
        sys.query_sig   = std::move(query_sig);
        sys.phase       = phase;
        sys.once        = once;
        sys.is_parallel = is_parallel;
        sys.run         = std::forward<Run>(run);
        return register_system(std::move(sys));
    }

    // The bodies behind add/add_once and add_kernel. Each states its parameter
    // requirements as a flat list of static_asserts, then one build guard hands
    // the shared core to build_system<Param> below; imperative and kernel differ
    // only in the parameter policy (ParamI vs ParamK), the is_parallel flag, and
    // whether a one-shot `once` is possible.
    //
    // The requirements are phrased as self-gating predicates (params_allowed /
    // at_most_one_query / has_res_mut), not inline conditions -- load-bearing, because a
    // check over system_args_t<Fn> cannot even be NAMED for a non-introspectable
    // Fn (that alias is ill-formed), so it must gate that instantiation itself.
    // Each returns its deferring value when Fn is not introspectable, so the
    // IntrospectableSystem assert stays the lone diagnostic there; the build
    // guard ANDs in IntrospectableSystem<Fn> so build_system never instantiates
    // on rejected input, which would otherwise bury the assert under a cascade.
    // The trailing fallback return keeps that ill-formed instantiation from also
    // erroring on a missing return. (This is why a bare `if constexpr (!cond)
    // return;` cannot un-nest these: the code after it still instantiates.)

    template <class A>
    using ParamI = detail::imperative_param<A>;

    template <class A>
    using ParamK = detail::kernel_param<A>;

    // Build and register a system under parameter policy `Param` (ParamI or
    // ParamK). Passing the policy as a template-template parameter is what lets
    // ONE core serve both add() and add_kernel(): a local `using Param = ParamI;`
    // cannot work -- ParamI is a TEMPLATE, not a type, and a template crosses a
    // function boundary only as a template argument. Only ever called from a
    // validated `if constexpr` branch, so it never instantiates on a rejected
    // parameter list (keeping each static_assert the lone diagnostic; see above).
    template <template <class> class Param, class Fn>
    SystemId build_system(std::string name,
                          Fn&& fn,
                          bool const once,
                          int const phase,
                          std::uint64_t const every,
                          bool const is_parallel) {
        using Args       = detail::system_args_t<Fn>;
        constexpr auto N = std::tuple_size_v<Args>;
        using Info       = detail::params_info<Param, Args>;
        using Seq        = std::make_index_sequence<N>;
        System sys;
        sys.name        = std::move(name);
        sys.phase       = phase;
        sys.once        = once;
        sys.every       = std::max<std::uint64_t>(1, every);
        sys.is_parallel = is_parallel;
        detail::declare<Param, Args>(sys.access, Seq {});
        if constexpr (detail::query_info<Args>::has_query) {
            constexpr auto QI = detail::query_info<Args>::index;
            using Q           = std::tuple_element_t<QI, Args>;
            sys.query_sig     = detail::system_param<Q>::signature();
        }

        auto states = std::make_shared<typename Info::states>();
        sys.run     = [fn = std::forward<Fn>(fn),
                   states](World& w, Commands& c, detail::WorkItem const& item) mutable {
            detail::invoke<Param, Args>(fn, *states, w, c, item, Seq {});
        };
        if constexpr (Info::any_stateful) {
            sys.prepare_items = [states](World& w,
                                         std::span<std::uint32_t const> rows,
                                         detail::KernelWaveContext const& ctx) {
                detail::prepare_all<Param, Args>(*states, w, rows, ctx, Seq {});
            };
            sys.finish_items = [states](World& w) {
                detail::finish_all<Param, Args>(*states, w, Seq {});
            };
        }
        return register_system(std::move(sys));
    }

    // Self-gating checks used by the static_asserts and build guards below. Each
    // returns its DEFERRING value when Fn is not introspectable (params /
    // at_most_one_query -> true, has_res_mut -> false), so that case yields the
    // IntrospectableSystem assert alone; otherwise it reports the real check over
    // the argument list.
    template <template <class> class Param, class Fn>
    static consteval bool params_allowed() {
        if constexpr (detail::IntrospectableSystem<Fn>)
            return detail::params_info<Param, detail::system_args_t<Fn>>::all_allowed;
        else
            return true;
    }
    template <class Fn>
    static consteval bool at_most_one_query() {
        if constexpr (detail::IntrospectableSystem<Fn>)
            return detail::query_info<detail::system_args_t<Fn>>::count <= 1;
        else
            return true;
    }
    template <class Fn>
    static consteval bool has_res_mut() {
        if constexpr (detail::IntrospectableSystem<Fn>)
            return detail::any_res_mut_v<detail::system_args_t<Fn>>;
        else
            return false;
    }

    template <class Fn>
    SystemId emplace(std::string name,
                     Fn&& fn,
                     bool const once,
                     int const phase,
                     std::uint64_t const every) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        static_assert(at_most_one_query<Fn>(),
                      "a system may take AT MOST ONE Query<Cs...> parameter -- the "
                      "work-item model binds a system's one query to its one work "
                      "item, and kernels already require exactly one. For "
                      "pairwise/neighbor work, build a spatial structure with "
                      "Reduce<T, Op> and read it via Res<T>; for ad-hoc reads across "
                      "several component sets, use WorldView");
        static_assert(params_allowed<ParamI, Fn>(),
                      "unsupported system parameter type: a system may take "
                      "Query<Cs...>, Res<T>, ResMut<T>, Commands&, WorldView, "
                      "and Local<T> (spelled exactly so -- e.g. Query by "
                      "value, Commands by reference); raw World& is "
                      "deliberately not a system parameter, and the per-item "
                      "primitives (Reduce/Extract/Collect/EventWriter/"
                      "EventReader/Scratch/Random) are kernel-only (register "
                      "with add_kernel)");
        if constexpr (detail::IntrospectableSystem<Fn> && at_most_one_query<Fn>() &&
                      params_allowed<ParamI, Fn>())
            return build_system<ParamI>(std::move(name),
                                        std::forward<Fn>(fn),
                                        once,
                                        phase,
                                        every,
                                        /*is_parallel=*/false);
        return SystemId::none(); // reached only when a static_assert above fired
    }

    template <class Fn>
    SystemId emplace_kernel(std::string name,
                            Fn&& fn,
                            int const phase,
                            std::uint64_t const every) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        static_assert(at_most_one_query<Fn>(),
                      "a system may take AT MOST ONE Query<Cs...> parameter -- the "
                      "work-item model binds a system's one query to its one work "
                      "item, and kernels already require exactly one. For "
                      "pairwise/neighbor work, build a spatial structure with "
                      "Reduce<T, Op> and read it via Res<T>; for ad-hoc reads across "
                      "several component sets, use WorldView");
        static_assert(!has_res_mut<Fn>(),
                      "add_kernel: ResMut<T> is not allowed in a kernel system -- "
                      "its work items run concurrently, so writes through ResMut "
                      "would race. Read resources via Res<T>; fold shared state "
                      "with Reduce<T, Op>, write gather-shaped output with "
                      "Extract<T> (see schedule/params/)");
        // `|| has_res_mut` so a ResMut (which also fails params_allowed) fires
        // only its own, more specific assert above.
        static_assert(params_allowed<ParamK, Fn>() || has_res_mut<Fn>(),
                      "add_kernel: unsupported kernel parameter type -- a kernel "
                      "system may take one Query<Cs...>, plus Res<T>, Commands&, "
                      "WorldView, Reduce<T, Op>, Extract<T>, Collect<T>, "
                      "EventWriter<T>, EventReader<T>, Bin<V>, Scratch<T>, and "
                      "Random (Local<T> is imperative-only: per-system state has "
                      "no race-free meaning across a kernel's concurrent items)");
        if constexpr (detail::IntrospectableSystem<Fn> && at_most_one_query<Fn>() &&
                      !has_res_mut<Fn>() && params_allowed<ParamK, Fn>())
            return build_system<ParamK>(std::move(name),
                                        std::forward<Fn>(fn),
                                        /*once=*/false,
                                        phase,
                                        every,
                                        /*is_parallel=*/true);
        return SystemId::none(); // reached only when a static_assert above fired
    }

    // Build and execute one wave's item list; on a throw, discard the aborted
    // run's recorded edits and emit TickAbort (see run()).
    double run_wave(
        WavePlan& plan, World& world, Commands& cmds, WorkerPool& pool, std::size_t lvl) {
        using namespace sched_event;
        flush_attrib_.clear();
        auto& wave    = plan.build(systems_, world, tick_);
        auto flush_us = 0.0;
        try {
            wave.prepare(world);
            wave.run(systems_, world, cmds, pool);
            wave.finish(world);
            auto const tf = detail::sched_clock::now();
            world.apply_commands(&flush_attrib_);
            flush_us = detail::elapsed_us(tf);
        } catch (...) {
            world.discard_commands();
            events_.emit(TickAbort {lvl, SystemId::none()});
            throw;
        }
        auto const& result = wave.result();
        for (auto id : plan) {
            auto const fit  = flush_attrib_.find(id);
            auto const flsh = fit != flush_attrib_.end() ? fit->second : 0.0;
            events_.emit(SystemWork {id,
                                     systems_[id].name,
                                     result.busy_us[id],
                                     result.prepare_us[id],
                                     result.finish_us[id],
                                     result.item_counts[id],
                                     flsh});
        }
        return flush_us;
    }

    // Tombstone spent one-shot systems (see remove()): they keep their slot so
    // no other system's position -- and so no SystemId -- shifts.
    void prune_once() {
        for (auto& s : systems_)
            if (s.once && !s.dead) {
                s.dead = true;
                dirty_ = true;
            }
    }

    // Assign wavefront levels intra-phase level = 1 + max(level) over earlier
    // conflicting systems in the same phase (cross-phase ordering is handled by
    // the phase barrier). Tombstoned systems are skipped -- they take no slot in
    // any wave -- but their positions still count, so a live system keeps the
    // same level whether or not an earlier system was removed.
    void assign_levels() {
        for (auto& system : systems_)
            system.level = 0;
        for (auto&& [id, s1] : systems_.enumerate()) {
            if (s1.dead)
                continue;
            auto level = System::Level {0};
            for (auto const& s2 : systems_ | std::views::take(id.value)) {
                if (!s2.dead && s1.phase == s2.phase && conflicts(s1.access, s2.access))
                    level = std::max(level, s2.level + 1);
            }
            s1.level = level;
        }
    }

    void build_wave_plans() {
        using WaveGroup  = std::pair<System::WaveKey, WavePlan>;
        auto search      = [](auto const& g) { return g.first; };
        auto wave_groups = std::vector<WaveGroup> {};
        for (auto&& [id, system] : systems_.enumerate()) {
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
        std::ranges::sort(wave_groups, {}, search);
        wave_plans_.clear();
        wave_plans_.reserve(wave_groups.size());
        std::ranges::copy(wave_groups | std::views::values | std::views::as_rvalue,
                          std::back_inserter(wave_plans_));
    }

    void rebuild() {
        if (!dirty_)
            return;
        assign_levels();
        build_wave_plans();
        dirty_ = false;
    }

    event::Emitter<ScheduleEvent> events_;
    using SystemVector = detail::IdVector<SystemId, System>;
    SystemVector systems_;
    std::vector<WavePlan> wave_plans_;
    FlushAttrib flush_attrib_; // per-system flush time, per wave
    std::uint64_t tick_ = 0;   // run() count; part of Random's stream identity
    bool dirty_         = true;
};

} // namespace ecs
