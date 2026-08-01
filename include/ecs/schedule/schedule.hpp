// ecs/schedule/schedule.hpp
//
// The Schedule: registration, conflict-driven wave building, and the run loop.
//
// A *system* is registered in one of two forms:
//
//   * serial (add_serial/add_dynamic_serial): a callable whose PARAMETER TYPES
//     declare what it touches --
//       void physics(Query<Position, const Velocity> q, ResMut<Gravity> g);
//     Query<Cs...> (const component = read, non-const = write), Res<T>/
//     ResMut<T>, Commands& (deferred edits), WorldView (reads everything).
//     The scheduler derives the read/write sets from the parameters and
//     constructs them when the system runs, so the declared access cannot
//     drift from actual use. Opaque to the executor: one work item. (Raw
//     World& is deliberately not a system parameter -- see system.hpp.)
//
//   * parallel (add_parallel): the SAME signature rules, with exactly one Query
//     parameter -- whose iteration is VISIBLE, so the executor slices it into
//     work items, binding the Query restricted to each item's rows.
//
// Conflicting systems (one writes what another reads or writes) are assigned
// wavefront *levels*; each wave is conflict-free and executed by the work-item
// executor (wave.hpp) with a single pool dispatch. Recorded edits flush at
// each wave barrier. A `phase{n}` option gives coarse ordering across barriers
// independent of conflicts.

#pragma once

#include <ecs/detail/container/id_vector.hpp>
#include <ecs/event/emitter.hpp>
#include <ecs/parallel/worker_pool.hpp>
#include <ecs/query.hpp>
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/events.hpp>
#include <ecs/schedule/graph.hpp>
#include <ecs/schedule/params.hpp>
#include <ecs/schedule/system.hpp>
#include <ecs/schedule/validate.hpp>
#include <ecs/schedule/wave.hpp>
#include <ecs/world.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ecs {

class Schedule {
public:
    // The stored record type, exposed for tooling (systems() hands out a
    // vector of these; the visualizer reads name/access/phase/level).
    using System = detail::SystemRecord;
    using Wave   = std::vector<SystemId>;

    // Register a serial system. Its access is derived from its parameter
    // types. Registration options follow the body as an optional pack -- each may
    // be given at most once, in any order (see schedule/access.hpp):
    //
    //   phase{n}  coarse ordering across barriers (default 0)
    //   every{n}  run only on ticks where tick % n == 0 (default 1)
    //   times{n}  retire after being due n times (default 0 = never)
    //
    //   sched.add_serial("integrate", [](Query<Position, Velocity const> q){ ... });
    //   sched.add_serial("startup",   setup_fn, phase{-1}, times{1});
    //   sched.add_serial("resort",    [](Commands& c){ c.sort<Position>(cell); },
    //                    phase{-1}, every{64});
    //
    // Returns a handle remove() can unschedule.
    template <class Fn, class... Opts>
    SystemId add_serial(std::string name, Fn&& fn, Opts... opts) {
        auto const o = detail::resolve_options(opts...);
        static_assert(!detail::has_partition<Opts...>(),
                      "add_serial: partition_by<K> has no meaning on a serial system "
                      "-- it is ONE opaque work item by construction, so there is "
                      "nothing for a partition to split it into. Use add_parallel");
        return emplace(std::move(name),
                       std::forward<Fn>(fn),
                       o.times,
                       o.phase,
                       o.every,
                       o.grain);
    }

    // Register a PARALLEL system. Same signature rules as add_serial() -- the system's
    // parameter types declare its access -- with one structural requirement:
    // exactly ONE Query<Cs...> parameter. That query is the iteration the
    // executor slices: the system body is invoked once per work item with the
    // Query restricted to that item's rows, so `q.for_each_chunk(...)` /
    // `q.for_each(...)` inside iterate just the slice, inline on the
    // claiming lane. A serial add_serial() system with independent per-row work
    // becomes a parallel system by changing one word:
    //
    //   sched.add_parallel("steer",
    //       [](Query<const Position, Velocity> q, Res<Clock> clk) {
    //           q.for_each([&](auto& p, auto& v) { ... });
    //       });
    //
    // Unlike a serial system (an opaque callable the executor must run
    // whole), a parallel system's iteration is VISIBLE to the scheduler: its
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
    // Ordered iteration and effects still belong in add_serial() systems. The sliced
    // Query is bound to the shared 1-lane pool, so nothing a parallel body does
    // can reach a nested dispatch.
    template <class Fn, class... Opts>
    SystemId add_parallel(std::string name, Fn&& fn, Opts... opts) {
        auto const o = detail::resolve_options(opts...);
        // partition_by<K> travels as a type, not a value -- see access.hpp.
        return emplace_parallel<detail::partition_key_t<Opts...>>(std::move(name),
                                                                  std::forward<Fn>(fn),
                                                                  o.phase,
                                                                  o.every,
                                                                  o.times,
                                                                  o.grain);
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
    template <class Run, class... Opts>
    SystemId add_dynamic_serial(std::string name,
                                SystemAccess access,
                                Run&& run,
                                Opts... opts) {
        auto const o = detail::resolve_options(opts...);
        return emplace_dynamic(std::move(name),
                               std::move(access),
                               Signature::null(),
                               std::forward<Run>(run),
                               o.phase,
                               o.every,
                               o.times,
                               o.grain,
                               /*is_parallel=*/false);
    }

    // Parallel counterpart of add_dynamic_serial -- the dynamic mirror of add_parallel.
    // The scheduler resolves `query_sig`'s matched archetypes, slices them into
    // row-range work items, and fans those across the pool, invoking `run` once
    // per slice. `run` reads its WorkItem for the archetype and [begin, end) rows
    // to process (see Wave::build_parallel); it must NOT self-iterate or
    // self-dispatch -- the pool is already fanned, and a nested dispatch on
    // it would throw. `query_sig` is REQUIRED (a null signature matches nothing,
    // so the scheduler would build zero items and never call `run`).
    template <class Run, class... Opts>
    SystemId add_dynamic_parallel(std::string name,
                                  SystemAccess access,
                                  Signature query_sig,
                                  Run&& run,
                                  Opts... opts) {
        auto const o = detail::resolve_options(opts...);
        return emplace_dynamic(std::move(name),
                               std::move(access),
                               std::move(query_sig),
                               std::forward<Run>(run),
                               o.phase,
                               o.every,
                               o.times,
                               o.grain,
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
    // setup: register a times=1 setup system, then run(world).
    void run(World& world) { run(world, parallel::serial_pool()); }

    // The WORK-ITEM executor (see wave.hpp for the mechanism). Each wave
    // is flattened into one item list -- parallel systems sliced into row
    // ranges, serial systems one opaque item each -- and executed with a
    // single pool dispatch, lanes claiming items dynamically. A heavy parallel
    // system's slices overlap both with each other AND with the wave's other
    // systems: data parallelism within and across systems from one fork-join.
    //
    // The single-item wave short-circuits to run that item inline on the
    // caller (a fanned wave's items interleave across the lanes, so there is no
    // per-system wall interval to observe). Either way,
    // every system reports MEASURED work via a SystemWork event: busy time
    // summed over its items (recorded by the claiming lanes) plus its barrier
    // prepare/finish hook durations.
    //
    // Determinism: item contents and order are fixed; item-to-lane assignment
    // is not. Commands recorded by PARALLEL systems replay in canonical order
    // regardless (per-item stores enqueued at the barrier in ordinal order --
    // see schedule/params/commands.hpp; spawn() handles are still reserved at
    // record time, so the IDs themselves remain timing-dependent). Commands
    // recorded by serial systems keep the thread-sharded path, so their
    // cross-shard replay order -- already unspecified -- varies run to run.
    // A 1-lane pool claims items in list order and is fully deterministic.
    //
    // Recorded edits flush at each wave barrier; one-shot systems are removed
    // afterwards. A system that throws (in its body or a parallel item)
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
        for (auto& plan : wave_plans_) {
            plan.prepare(systems_, tick_);
            if (plan.size() > 0) {
                try {
                    run_wave(plan, world, cmds, pool);
                } catch (...) {
                    events_.emit(TickAbort {plan.id, SystemId::none()});
                    throw;
                }
            }
        }
        events_.emit(TickEnd {});
        detail::prune_expired(systems_, tick_, dirty_);
    }

    [[nodiscard]]
    auto& events(this auto& self) noexcept {
        return self.events_;
    }

    // Pre-pay the first tick's one-time parameter initialization. For every
    // stateful system, this sizes the per-item slot state and runs the prepare
    // hooks (reduce targets reset, extract/collect/bin targets pre-size)
    // against the CURRENT, already-populated world -- so the first real run()
    // allocates nothing and its per-system busy times are steady from tick 0
    // instead of paying a cold outlier. It runs NO system bodies and NO barrier
    // folds, so it does not advance or otherwise mutate simulation state (the
    // targets it touches are reset every tick regardless) and emits no events.
    // Call once, after the world is populated (entities spawned) and before the
    // timed/rendered loop; idempotent.
    //
    // A parallel param whose partial holds fixed-size scratch (e.g. a dense
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

    // Shared body of add_dynamic_serial / add_dynamic_parallel: assemble a
    // declared-access System and hand it to register_system. is_parallel + query_sig
    // decide how the wave builds this system's work items (one opaque item vs. sliced row
    // ranges).
    template <class Run>
    SystemId emplace_dynamic(std::string name,
                             SystemAccess access,
                             Signature query_sig,
                             Run&& run,
                             int phase,
                             std::uint64_t every,
                             std::uint64_t times,
                             std::size_t grain,
                             bool is_parallel) {
        System sys;
        sys.name        = std::move(name);
        sys.access      = std::move(access); // caller-supplied ids: normalized below
        sys.query_sig   = std::move(query_sig);
        sys.phase       = phase;
        sys.every       = every;
        sys.times       = times;
        sys.grain       = grain;
        sys.is_parallel = is_parallel;
        sys.run         = std::forward<Run>(run);
        return register_system(std::move(sys));
    }

    // The bodies behind add_serial and add_parallel. Each states its parameter
    // requirements as a flat list of static_asserts, then one build guard hands
    // the shared core to build_system<Param> below; serial and parallel differ
    // only in the parameter policy (ParamS vs ParamP) and the is_parallel flag.
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
    using ParamS = detail::serial_param<A>;

    template <class A>
    using ParamP = detail::parallel_param<A>;

    // Build and register a system under parameter policy `Param` (ParamS or
    // ParamP). Passing the policy as a template-template parameter is what lets
    // ONE core serve both add_serial() and add_parallel(): a local `using Param =
    // ParamS;` cannot work -- ParamS is a TEMPLATE, not a type, and a template crosses a
    // function boundary only as a template argument. Only ever called from a
    // validated `if constexpr` branch, so it never instantiates on a rejected
    // parameter list (keeping each static_assert the lone diagnostic; see above).
    template <template <class> class Param, class PKey = void, class Fn>
    SystemId build_system(std::string name,
                          Fn&& fn,
                          std::uint64_t const times,
                          int const phase,
                          std::uint64_t const every,
                          std::size_t const grain,
                          bool const is_parallel) {
        using Args       = detail::system_args_t<Fn>;
        constexpr auto N = std::tuple_size_v<Args>;
        using Info       = detail::params_info<Param, Args>;
        using Seq        = std::make_index_sequence<N>;
        System sys;
        sys.name        = std::move(name);
        sys.phase       = phase;
        sys.times       = times;
        sys.every       = std::max<std::uint64_t>(1, every);
        sys.grain       = grain;
        sys.is_parallel = is_parallel;
        detail::declare<Param, Args>(sys.access, Seq {});
        if constexpr (detail::query_info<Args>::has_query) {
            constexpr auto QI = detail::query_info<Args>::index;
            using Q           = std::tuple_element_t<QI, Args>;
            sys.query_sig     = detail::system_param<Q>::signature();
        }
        if constexpr (!std::is_void_v<PKey>) {
            sys.partition = detail::make_partition<PKey>();
            // The key is read (conflict analysis must see it: a system that
            // WRITES the key must not run alongside one partitioned by it), and
            // it narrows the match -- a row with no K has no group, so an
            // archetype without K is not this system's business.
            sys.access.reads.push_back(component_id<PKey>);
            sys.query_sig = sys.query_sig.with(component_id<PKey>);
        }

        auto states = std::make_shared<typename Info::states>();
        sys.run = [fn = std::forward<Fn>(fn),
                   states](World& w, Commands& c, detail::WorkItem const& item) mutable {
            detail::invoke<Param, Args>(fn, *states, w, c, item, Seq {});
        };
        if constexpr (Info::any_stateful) {
            sys.prepare_items = [states](World& w,
                                         std::span<std::uint32_t const> rows,
                                         detail::WaveContext const& ctx) {
                detail::prepare_all<Param, Args>(*states, w, rows, ctx, Seq {});
            };
            sys.finish_items = [states](World& w, WorkerPool& pool) {
                detail::finish_all<Param, Args>(*states, w, pool, Seq {});
            };
        }
        return register_system(std::move(sys));
    }

    template <class Fn>
    SystemId emplace(std::string name,
                     Fn&& fn,
                     std::uint64_t const times,
                     int const phase,
                     std::uint64_t const every,
                     std::size_t const grain) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        static_assert(detail::at_most_one_query<Fn>(),
                      "a system may take AT MOST ONE Query<Cs...> parameter -- the "
                      "work-item model binds a system's one query to its one work "
                      "item, and parallel systems already require exactly one. For "
                      "pairwise/neighbor work, build a spatial structure with "
                      "Reduce<T, Op> and read it via Res<T>; for ad-hoc reads across "
                      "several component sets, use WorldView");
        static_assert(detail::params_allowed<ParamS, Fn>(),
                      "unsupported system parameter type: a system may take "
                      "Query<Cs...>, Res<T>, ResMut<T>, Commands&, WorldView, "
                      "ColumnView<Cs...>, Local<T>, and Exec (spelled exactly so -- "
                      "e.g. Query by value, Commands by reference); raw World& is "
                      "deliberately not a system parameter, and the per-item "
                      "primitives (Reduce/Extract/Collect/Bin/EventWriter/"
                      "EventReader/Scratch/Random) are parallel-only (register "
                      "with add_parallel)");
        if constexpr (detail::IntrospectableSystem<Fn> &&
                      detail::at_most_one_query<Fn>() &&
                      detail::params_allowed<ParamS, Fn>())
            return build_system<ParamS>(std::move(name),
                                        std::forward<Fn>(fn),
                                        times,
                                        phase,
                                        every,
                                        grain,
                                        /*is_parallel=*/false);
        return SystemId::none(); // reached only when a static_assert above fired
    }

    // PKey is the partition_by<K> key type, or void when the system has none.
    template <class PKey, class Fn>
    SystemId emplace_parallel(std::string name,
                              Fn&& fn,
                              int const phase,
                              std::uint64_t const every,
                              std::uint64_t const times,
                              std::size_t const grain) {
        static_assert(detail::IntrospectableSystem<Fn>,
                      "a system must be a plain function or a functor with exactly "
                      "one non-template call operator -- a generic (auto-parameter) "
                      "lambda cannot work here, because the parameter TYPES are what "
                      "declare the system's access");
        static_assert(detail::at_most_one_query<Fn>(),
                      "a system may take AT MOST ONE Query<Cs...> parameter -- the "
                      "work-item model binds a system's one query to its one work "
                      "item, and parallel systems already require exactly one. For "
                      "pairwise/neighbor work, build a spatial structure with "
                      "Reduce<T, Op> and read it via Res<T>; for ad-hoc reads across "
                      "several component sets, use WorldView");
        static_assert(
            detail::has_query<Fn>(),
            "add_parallel: a parallel system must take exactly one Query<Cs...> "
            "-- it is the row iteration the scheduler slices into parallel "
            "work items, so a query-less system has no rows to parallelize "
            "and would never run. The per-item primitives (Reduce/Extract/"
            "Collect/EventWriter/...) are per-row too, so they need the query "
            "as well. For resource-only or effect-only work use add_serial(), whose "
            "one work item still runs concurrently with the wave's others");
        static_assert(!detail::has_res_mut<Fn>(),
                      "add_parallel: ResMut<T> is not allowed in a parallel system -- "
                      "its work items run concurrently, so writes through ResMut "
                      "would race. Read resources via Res<T>; fold shared state "
                      "with Reduce<T, Op>, write gather-shaped output with "
                      "Extract<T> (see schedule/params/)");
        // `|| has_res_mut` so a ResMut (which also fails params_allowed) fires
        // only its own, more specific assert above.
        static_assert(
            detail::params_allowed<ParamP, Fn>() || detail::has_res_mut<Fn>(),
            "add_parallel: unsupported parallel parameter type -- a parallel "
            "system may take one Query<Cs...>, plus Res<T>, Commands&, "
            "WorldView, ColumnView<Cs...>, Reduce<T, Op>, Extract<T>, Collect<T>, "
            "EventWriter<T>, EventReader<T>, Bin<V>, Scratch<T>, and "
            "Random (Local<T> is serial-only: per-system state has "
            "no race-free meaning across a parallel system's concurrent items)");
        if constexpr (detail::IntrospectableSystem<Fn> &&
                      detail::at_most_one_query<Fn>() && detail::has_query<Fn>() &&
                      !detail::has_res_mut<Fn>() && detail::params_allowed<ParamP, Fn>())
            return build_system<ParamP, PKey>(std::move(name),
                                              std::forward<Fn>(fn),
                                              times,
                                              phase,
                                              every,
                                              grain,
                                              /*is_parallel=*/true);
        return SystemId::none(); // reached only when a static_assert above fired
    }

    // Build and execute one wave's item list; on a throw, discard the aborted
    // run's recorded edits and emit TickAbort (see run()).
    void run_wave(WavePlan& plan, World& world, Commands& cmds, WorkerPool& pool) {
        using namespace sched_event;
        events_.emit(WaveBegin {plan.id, plan.size()});
        flush_attrib_.clear();
        auto& wave    = plan.build(systems_, world, tick_);
        auto flush_us = 0.0;
        try {
            wave.prepare(world);
            wave.run(systems_, world, cmds, pool);
            wave.finish(world, pool);
            auto const tf = detail::sched_clock::now();
            world.apply_commands(&flush_attrib_);
            flush_us = detail::elapsed_us(tf);
        } catch (...) {
            world.discard_commands();
            throw;
        }
        auto const& plan_result = plan.result();
        auto const& wave_result = wave.result();
        for (auto id : plan) {
            auto const fit  = flush_attrib_.find(id);
            auto const flsh = fit != flush_attrib_.end() ? fit->second : 0.0;
            events_.emit(SystemWork {id,
                                     systems_[id].name,
                                     wave_result.busy_us[id],
                                     wave_result.prepare_us[id],
                                     wave_result.finish_us[id],
                                     wave_result.item_counts[id],
                                     flsh});
        }
        events_.emit(
            WaveEnd {plan.id, flush_us, plan_result.build_us, plan_result.sort_us});
    }

    void rebuild() {
        if (!dirty_)
            return;
        detail::assign_levels(systems_);
        detail::build_wave_plans(systems_, wave_plans_);
        dirty_ = false;
    }

    event::Emitter<ScheduleEvent> events_;
    using SystemVector = detail::SystemVector;
    SystemVector systems_;
    std::vector<WavePlan> wave_plans_;
    FlushAttrib flush_attrib_; // per-system flush time, per wave
    std::uint64_t tick_ = 0;   // run() count; part of Random's stream identity
    bool dirty_         = true;
};

} // namespace ecs
