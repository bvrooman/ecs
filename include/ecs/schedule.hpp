// ecs/schedule.hpp
//
// Access-analyzed, data-parallel system scheduler.
//
// A *system* is a callable whose parameters declare what it touches:
//
//   void physics(Query<Position, const Velocity> q, ResMut<Gravity> g);
//
//     Query<Cs...>  -- iterate matching entities; a `const` component is a
//     read,
//                      a non-const component is a write.
//     Res<T>        -- read resource T.        ResMut<T> -- write resource T.
//     Commands&     -- record deferred structural edits (no tracked access).
//
// The scheduler derives each system's read/write sets *from these parameter
// types* and constructs the parameters when the system runs -- so the declared
// access cannot drift from what the system actually does (the load-bearing
// assumption behind safe parallelism). It then finds systems that conflict (one
// writes what another reads or writes) and assigns each to a wavefront *level*:
// every system runs after all earlier conflicting systems, and same-level
// systems are conflict-free. A `phase<N>` tag gives coarse ordering across
// barriers independent of conflicts.
//
// Execution (run(world, WorkerPool&)): each wave is flattened into WORK ITEMS
// -- kernel systems (add_kernel) contribute one item per row-range slice of
// their matched archetypes, imperative systems one opaque item each -- sorted
// longest-first and claimed dynamically by the pool's lanes from a single
// dispatch. Data parallelism within and across a wave's systems falls out of
// the one mechanism; see run() for the full story. Recorded edits flush at
// each level barrier. `run(world)` is sugar for a 1-lane (serial) pool, which
// claims items in list order and is fully deterministic. See worker_pool.hpp.

#pragma once

#include "detail/move_only_function.hpp"
#include "event/emitter.hpp"
#include "query.hpp"
#include "world.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ecs {

// Coarse ordering tag: systems run in ascending phase order with a barrier
// between phases; within a phase they are leveled by conflicts. Default 0, so
// phase<-1> is a startup phase, phase<1> a teardown/late phase.
template <int>
struct phase {};

using SystemId = std::uint64_t;

// A system's derived component/resource access (id sets). `exclusive` means the
// system has unanalyzable access (it took a raw World&) and conflicts with
// every other system. `reads_all` means it reads everything (a WorldView): it
// conflicts only with writers, so read-only systems still run in parallel.
struct SystemAccess {
    std::vector<std::uint32_t> reads, writes, res_reads, res_writes;
    bool exclusive = false;
    bool reads_all = false;
    // Purely informational (does not affect conflict analysis): the system took
    // a Commands& and may record deferred structural edits.
    bool commands = false;
};

namespace detail {

    // --- extract a callable's parameter types --------------------------------
    template <class F>
    struct fn_traits : fn_traits<decltype(&F::operator())> {};
    template <class C, class R, class... A>
    struct fn_traits<R (C::*)(A...)> {
        using args = std::tuple<A...>;
    };
    template <class C, class R, class... A>
    struct fn_traits<R (C::*)(A...) const> {
        using args = std::tuple<A...>;
    };
    template <class C, class R, class... A>
    struct fn_traits<R (C::*)(A...) noexcept> {
        using args = std::tuple<A...>;
    };
    template <class C, class R, class... A>
    struct fn_traits<R (C::*)(A...) const noexcept> {
        using args = std::tuple<A...>;
    };
    template <class R, class... A>
    struct fn_traits<R (*)(A...)> {
        using args = std::tuple<A...>;
    };

    // --- system parameter protocol: declare(access) + bind(world, commands) --
    template <class P>
    struct system_param; // unspecialized => using an unsupported parameter type

    template <class C>
    void declare_component(SystemAccess& a) {
        if constexpr (std::is_const_v<C>)
            a.reads.push_back(component_id<std::remove_const_t<C>>);
        else
            a.writes.push_back(component_id<C>);
    }

    // bind() receives the active WorkerPool the run installed -- run(World&) uses a
    // transient 1-lane pool, so it is never null here; the Query carries it so
    // for_each_chunk/for_each_parallel split their rows across the lanes.
    template <class... Cs>
    struct system_param<Query<Cs...>> {
        static void declare(SystemAccess& a) { (declare_component<Cs>(a), ...); }
        static Query<Cs...> bind(World& w, Commands&, WorkerPool& pool) {
            return Query<Cs...>(w, pool);
        }
    };
    template <class T>
    struct system_param<Res<T>> {
        static void declare(SystemAccess& a) { a.res_reads.push_back(resource_id<T>); }
        static Res<T> bind(World& w, Commands&, WorkerPool&) { return Res<T>(w); }
    };
    template <class T>
    struct system_param<ResMut<T>> {
        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }
        static ResMut<T> bind(World& w, Commands&, WorkerPool&) { return ResMut<T>(w); }
    };
    template <>
    struct system_param<Commands&> {
        // A side channel: no tracked component/resource access. We still flag it
        // so tooling can surface that the system records commands.
        static void declare(SystemAccess& a) { a.commands = true; }
        static Commands& bind(World&, Commands& c, WorkerPool&) { return c; }
    };
    // Read-only ad-hoc access: WorldView reads everything but writes nothing,
    // so it runs in parallel with other readers and is serialized only against
    // writers.
    template <>
    struct system_param<WorldView> {
        static void declare(SystemAccess& a) { a.reads_all = true; }
        static WorldView bind(World& w, Commands&, WorkerPool&) { return WorldView(w); }
    };
    // Full escape hatch: a raw World& can also mutate component values through
    // a non-const query, so its access cannot be analyzed -- the system is
    // marked exclusive and runs alone. Prefer Query/Res, or WorldView for
    // read-only access.
    template <>
    struct system_param<World&> {
        static void declare(SystemAccess& a) { a.exclusive = true; }
        static World& bind(World& w, Commands&, WorkerPool&) { return w; }
    };

    // Build the chunk argument for component C over rows [b, e) of an
    // archetype -- the kernel-system counterpart of Query's private chunk_arg.
    template <class C>
    chunk<C> make_chunk(Archetype& arch, std::size_t b, std::size_t e) {
        return chunk<C>(arch.column<std::remove_const_t<C>>().store, b, e);
    }

} // namespace detail

// The events Schedule::run emits as it executes, as one std::variant -- each
// alternative carries the data for that boundary (`level` is the 0-based wave
// index in execution order). Observe them via Schedule::events(); an observer is
// any callable void(ScheduleEvent const&), typically one that std::visits (see
// emitter.hpp). This is the successor to the old virtual ScheduleObserver: the
// core announces boundaries as values and observers decide what to do with them
// (time, log, count, ...). The profiler in tools/ is one such observer.
namespace sched_event {
    struct TickBegin {
        std::size_t n_waves;
    };
    struct TickEnd {};
    struct WaveBegin {
        std::size_t level;
        std::size_t n_systems;
    };
    struct WaveEnd {
        std::size_t level;
    };
    struct SystemBegin {
        SystemId id;
        std::string_view name;
    };
    struct SystemEnd {
        SystemId id;
    };
    // Emitted instead of the remaining SystemEnd/WaveEnd/TickEnd when a system
    // throws and the run unwinds, so observers with open Begin/End pairs can
    // reset instead of carrying stale timers into the next tick.
    struct TickAbort {
        std::size_t level;
        SystemId system;
    };
} // namespace sched_event

using ScheduleEvent = std::variant<sched_event::TickBegin,
                                   sched_event::TickEnd,
                                   sched_event::WaveBegin,
                                   sched_event::WaveEnd,
                                   sched_event::SystemBegin,
                                   sched_event::SystemEnd,
                                   sched_event::TickAbort>;

class Schedule {
public:
    struct System {
        SystemId id = 0;
        std::string name;
        SystemAccess access;
        // Imperative body (add/add_once/add_dynamic): opaque to the scheduler,
        // so the whole system is ONE work item. Null for kernel systems.
        // move_only_function (not function) so a system may capture a move-only
        // value (e.g. a unique_ptr or a move_only_function of its own).
        ecs::detail::move_only_function<void(World&, Commands&, WorkerPool&)> run;
        // Kernel body (add_kernel): the executor slices the matched rows into
        // work items and invokes this once per (archetype, row-range) item.
        // Null for imperative systems.
        ecs::detail::move_only_function<
            void(World&, std::uint32_t, std::size_t, std::size_t)>
            run_range;
        Signature query_sig; // kernel systems: sorted required-component ids
        int phase         = 0;
        std::size_t level = 0;
        bool once         = false;

        [[nodiscard]]
        bool is_kernel() const noexcept {
            return static_cast<bool>(run_range);
        }

        // Kernel systems: match list memoized against the world's archetype
        // generation (same idea as Query's per-type memo, but keyed per system
        // record since the component set is a runtime value here).
        std::vector<std::uint32_t> const* matches = nullptr;
        std::uint64_t match_world                 = 0;
        std::uint64_t match_gen                   = ~std::uint64_t {0};
    };

    // Register a system. Its access is derived from its parameter types; an
    // optional trailing phase<N> tag gives coarse ordering (default phase 0).
    // Returns a handle remove() can unschedule. Example:
    //   sched.add("integrate", [](Query<Position, const Velocity> q){ ... });
    //   sched.add("startup",   setup_fn, phase<-1>{});
    template <class Fn, int P = 0>
    SystemId add(std::string name, Fn&& fn, phase<P> = {}) {
        return emplace(std::move(name),
                       std::forward<Fn>(fn),
                       /*once=*/false,
                       P);
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
        static_assert(ecs::detail::are_distinct_v<std::remove_const_t<Cs>...>,
                      "add_kernel<Cs...>: duplicate component type (ignoring const)");
        static_assert(
            std::is_invocable_v<std::decay_t<Fn>&, std::span<Entity>, chunk<Cs>...>,
            "add_kernel<Cs...> kernel must be callable as "
            "fn(std::span<Entity>, chunk<Cs>...)");
        System sys;
        sys.id   = ++next_id_;
        sys.name = std::move(name);
        (detail::declare_component<Cs>(sys.access), ...);
        normalize(sys.access);
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
        auto const id = sys.id;
        systems_.push_back(std::move(sys));
        dirty_ = true;
        return id;
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
        auto const id = ++next_id_;
        System sys;
        sys.id     = id;
        sys.name   = std::move(name);
        sys.access = std::move(access);
        normalize(sys.access); // caller-supplied ids: sort + dedupe
        sys.phase = phase;
        sys.once  = once;
        sys.run   = std::forward<Run>(run);
        systems_.push_back(std::move(sys));
        dirty_ = true;
        return id;
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

    // Do two access sets conflict? One writes what the other reads or writes
    // (read/read never conflicts); an `exclusive` set conflicts with everything,
    // a `reads_all` set only with writers. Components and resources are checked
    // independently. Public so out-of-tree tooling (e.g. the schedule visualizer
    // in tools/) can rebuild the same dependency graph without re-deriving it.
    static bool conflicts(SystemAccess const& a, SystemAccess const& b) {
        if (a.exclusive || b.exclusive)
            return true;
        if ((a.reads_all && writes_any(b)) || (b.reads_all && writes_any(a)))
            return true;
        return conflicts_on(a.writes, a.reads, b.writes, b.reads) ||
               conflicts_on(a.res_writes, a.res_reads, b.res_writes, b.res_reads);
    }

    // Run serially on the calling thread -- sugar for a transient 1-lane pool
    // (which spawns no worker threads, so it is free). The way to do setup:
    // add_once a setup system, then run(world).
    void run(World& world) {
        WorkerPool serial {1};
        run(world, serial);
    }

    // The WORK-ITEM executor. Each wave (whose members are conflict-free by
    // construction) is flattened into one list of work items:
    //
    //   * a kernel system (add_kernel) contributes one item per row-range
    //     slice of each archetype its component set matches (~lanes x 8 items,
    //     so dynamic claiming absorbs differing per-row kernel costs), and
    //   * an imperative system (add/add_once/add_dynamic) contributes itself
    //     as a single opaque item.
    //
    // The list is sorted longest-first (greedy LPT -- the biggest work cannot
    // become the straggler at the join) and executed with ONE pool dispatch:
    // every lane claims items from an atomic cursor until none remain. A heavy
    // kernel system's slices therefore overlap both with each other AND with
    // the wave's other systems -- data parallelism within and across systems
    // from a single fork-join, no task queue. Imperative items run bound to
    // the shared 1-lane pool (their queries iterate inline on their lane); the
    // common single-system wave short-circuits to run that system inline on
    // the caller with the full pool, which preserves exact per-system observer
    // timing there (multi-system waves emit balanced Begin/End pairs around
    // the wave instead -- observers are not thread-safe).
    //
    // Item CONTENTS are deterministic (same slices, same sorted order every
    // tick); item-to-lane assignment is not, so the cross-shard replay order
    // of Commands recorded by different systems in one wave -- already
    // unspecified -- varies run to run. A 1-lane pool claims items in list
    // order on the caller and remains fully deterministic.
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
            build_items(wave, world, pool.lanes());
            if (wave.size() == 1) {
                auto const& s = systems_[wave[0]];
                events_.emit(SystemBegin {s.id, s.name});
                run_items(world, cmds, pool, lvl, s.id);
                events_.emit(SystemEnd {s.id});
            } else {
                run_items(world, cmds, pool, lvl, SystemId {0});
                for (auto const idx : wave) {
                    events_.emit(SystemBegin {systems_[idx].id, systems_[idx].name});
                    events_.emit(SystemEnd {systems_[idx].id});
                }
            }
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
    event::Emitter<ScheduleEvent> events_;

    // One unit of claimable work: a row-range of one kernel system's matched
    // archetype, or (archetype == kImperative) an entire opaque system.
    struct WorkItem {
        std::uint32_t system;    // index into systems_
        std::uint32_t archetype; // world archetype index, or kImperative
        std::uint32_t begin, end;
    };
    static constexpr std::uint32_t kImperative = 0xFFFF'FFFFu;
    // A slice below this many rows is not worth a separate claim.
    static constexpr std::size_t kMinItemRows = 1024;

    // Flatten one wave into items_ (capacity retained across waves and ticks:
    // no steady-state allocation) and sort longest-first with a deterministic
    // tie-break. Imperative items -- unknown cost -- sort ahead of everything.
    void build_items(std::vector<std::size_t> const& wave,
                     World& world,
                     unsigned lanes) {
        items_.clear();
        for (auto const idx : wave) {
            auto& s = systems_[idx];
            if (!s.is_kernel()) {
                items_.push_back({std::uint32_t(idx), kImperative, 0, 0});
                continue;
            }
            if (s.match_world != world.instance_id() ||
                s.match_gen != world.archetype_generation()) {
                s.matches     = &world.matching_archetypes(s.query_sig);
                s.match_world = world.instance_id();
                s.match_gen   = world.archetype_generation();
            }
            std::size_t total = 0;
            for (auto const ai : *s.matches)
                total += world.archetypes()[ai]->size();
            if (total == 0)
                continue;
            // ~lanes x 8 items per kernel system: enough oversubscription for
            // dynamic claiming to balance differing per-row kernel costs, with
            // a floor so an item always amortizes its claim. (An archetype
            // smaller than the grain still yields its own item -- a claim is
            // one fetch_add, far cheaper than the alternative bookkeeping.)
            std::size_t const grain =
                lanes <= 1 ? total
                           : std::max<std::size_t>(
                                 kMinItemRows,
                                 total / (std::size_t {lanes} * 8));
            for (auto const ai : *s.matches) {
                auto const n = world.archetypes()[ai]->size();
                for (std::size_t b = 0; b < n; b += grain) {
                    auto const e = std::min(n, b + grain);
                    items_.push_back({std::uint32_t(idx),
                                      ai,
                                      std::uint32_t(b),
                                      std::uint32_t(e)});
                }
            }
        }
        std::ranges::sort(items_, [](WorkItem const& a, WorkItem const& b) {
            bool const ia = a.archetype == kImperative;
            bool const ib = b.archetype == kImperative;
            if (ia != ib)
                return ia; // imperative (unknown cost) first
            auto const ra = a.end - a.begin;
            auto const rb = b.end - b.begin;
            if (ra != rb)
                return ra > rb; // then longest first (LPT)
            return std::tie(a.system, a.archetype, a.begin) <
                   std::tie(b.system, b.archetype, b.begin);
        });
    }

    // Execute items_: a lone item runs inline on the caller (an imperative
    // system then gets the FULL pool -- the common single-system wave keeps
    // today's within-system data parallelism); otherwise one dispatch, every
    // lane claiming items from an atomic cursor. Imperative items are bound to
    // the shared 1-lane pool so their queries iterate inline on their lane (a
    // nested dispatch on `pool` is an error by design; see WorkerPool). Any
    // throw aborts the run: recorded edits are discarded and TickAbort emitted.
    void run_items(World& world,
                   Commands& cmds,
                   WorkerPool& pool,
                   std::size_t lvl,
                   SystemId aborted) {
        if (items_.empty())
            return;
        try {
            if (items_.size() == 1) {
                run_item(items_[0], world, cmds, pool);
            } else {
                std::atomic<std::size_t> next {0};
                auto& inner = ecs::detail::serial_pool(); // 1-lane: dispatch-free
                pool.parallel_for(
                    items_.size(), /*min_parallel=*/2, [&](std::size_t, std::size_t) {
                        // Every lane claims items until none remain; the handed
                        // [b, e) slice is ignored in favor of dynamic claims.
                        for (auto i = next.fetch_add(1, std::memory_order_relaxed);
                             i < items_.size();
                             i = next.fetch_add(1, std::memory_order_relaxed))
                            run_item(items_[i], world, cmds, inner);
                    });
            }
        } catch (...) {
            world.discard_commands();
            events_.emit(sched_event::TickAbort {lvl, aborted});
            throw;
        }
    }

    void run_item(WorkItem const& it, World& world, Commands& cmds, WorkerPool& pool) {
        auto& s = systems_[it.system];
        if (it.archetype == kImperative)
            s.run(world, cmds, pool);
        else
            s.run_range(world, it.archetype, it.begin, it.end);
    }

    void prune_once() {
        if (std::erase_if(systems_, [](System const& s) { return s.once; }))
            dirty_ = true;
    }

    template <class Args, std::size_t... I>
    static void declare_into(SystemAccess& a, std::index_sequence<I...>) {
        (detail::system_param<std::tuple_element_t<I, Args>>::declare(a), ...);
    }
    template <class Args, class Fn, std::size_t... I>
    static void invoke(
        Fn& fn, World& w, Commands& c, WorkerPool& pool, std::index_sequence<I...>) {
        fn(detail::system_param<std::tuple_element_t<I, Args>>::bind(w, c, pool)...);
    }

    template <class Fn>
    SystemId emplace(std::string name, Fn&& fn, bool const once, int const phase) {
        using Args       = detail::fn_traits<std::decay_t<Fn>>::args;
        constexpr auto N = std::tuple_size_v<Args>;
        System sys;
        sys.id    = ++next_id_;
        sys.name  = std::move(name);
        sys.phase = phase;
        sys.once  = once;
        declare_into<Args>(sys.access, std::make_index_sequence<N> {});
        normalize(sys.access);
        sys.run =
            [fn = std::forward<Fn>(fn)](World& w, Commands& c, WorkerPool& pool) mutable {
                invoke<Args>(fn, w, c, pool, std::make_index_sequence<N> {});
            };
        auto const id = sys.id;
        systems_.push_back(std::move(sys));
        dirty_ = true;
        return id;
    }

    using IdList = std::vector<std::uint32_t>;

    // Access id lists are kept sorted + deduplicated (see normalize), so every
    // conflict check is a linear merge rather than a quadratic scan.
    static void normalize(SystemAccess& a) {
        auto norm = [](IdList& v) {
            std::ranges::sort(v);
            v.erase(std::ranges::unique(v).begin(), v.end());
        };
        norm(a.reads);
        norm(a.writes);
        norm(a.res_reads);
        norm(a.res_writes);
    }

    static bool intersects(IdList const& a, IdList const& b) {
        auto ia = a.begin();
        auto ib = b.begin();
        while (ia != a.end() && ib != b.end()) {
            if (*ia < *ib)
                ++ia;
            else if (*ib < *ia)
                ++ib;
            else
                return true;
        }
        return false;
    }
    static bool conflicts_on(IdList const& aw,
                             IdList const& ar,
                             IdList const& bw,
                             IdList const& br) {
        return intersects(aw, br) || intersects(aw, bw) || intersects(bw, ar);
    }
    static bool writes_any(SystemAccess const& a) {
        return !a.writes.empty() || !a.res_writes.empty();
    }
    static bool conflict(System const& a, System const& b) {
        return conflicts(a.access, b.access);
    }

    void rebuild() {
        if (!dirty_)
            return;
        // Intra-phase level: 1 + max level over earlier conflicting systems in
        // the same phase (cross-phase ordering is handled by the phase
        // barrier).
        for (std::size_t i = 0; i < systems_.size(); ++i) {
            std::size_t lvl = 0;
            for (std::size_t j = 0; j < i; ++j)
                if (systems_[j].phase == systems_[i].phase &&
                    conflict(systems_[i], systems_[j]))
                    lvl = std::max(lvl, systems_[j].level + 1);
            systems_[i].level = lvl;
        }
        // Waves run in ascending (phase, level) order with a barrier between
        // each. Flat vector of (key, members) instead of a std::map: no
        // per-rebuild node allocations, and the group count is small.
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

    std::vector<System> systems_;
    std::vector<std::vector<std::size_t>> waves_;
    std::vector<WorkItem> items_; // per-wave scratch, capacity retained
    SystemId next_id_ = 0;
    bool dirty_       = true;
};

} // namespace ecs
