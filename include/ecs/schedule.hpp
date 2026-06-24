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
// Execution (run(world, WorkerPool&)): systems run sequentially in level order,
// but each system's Query::for_each_chunk splits its rows across the pool's
// lanes -- data parallelism *within* a system. Recorded edits flush at each
// level barrier. `run(world)` is sugar for a 1-lane (serial) pool. See
// worker_pool.hpp.

#pragma once

#include "detail/move_only_function.hpp"
#include "query.hpp"
#include "world.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
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

    // bind() also receives the active WorkerPool* (null for an ad-hoc serial
    // run); the Query carries it so for_each_chunk splits rows across the lanes.
    template <class... Cs>
    struct system_param<Query<Cs...>> {
        static void declare(SystemAccess& a) { (declare_component<Cs>(a), ...); }
        static Query<Cs...> bind(World& w, Commands&, WorkerPool* pool) {
            return Query<Cs...>(w, pool);
        }
    };
    template <class T>
    struct system_param<Res<T>> {
        static void declare(SystemAccess& a) { a.res_reads.push_back(resource_id<T>); }
        static Res<T> bind(World& w, Commands&, WorkerPool*) { return Res<T>(w); }
    };
    template <class T>
    struct system_param<ResMut<T>> {
        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }
        static ResMut<T> bind(World& w, Commands&, WorkerPool*) { return ResMut<T>(w); }
    };
    template <>
    struct system_param<Commands&> {
        // A side channel: no tracked component/resource access. We still flag it
        // so tooling can surface that the system records commands.
        static void declare(SystemAccess& a) { a.commands = true; }
        static Commands& bind(World&, Commands& c, WorkerPool*) { return c; }
    };
    // Read-only ad-hoc access: WorldView reads everything but writes nothing,
    // so it runs in parallel with other readers and is serialized only against
    // writers.
    template <>
    struct system_param<WorldView> {
        static void declare(SystemAccess& a) { a.reads_all = true; }
        static WorldView bind(World& w, Commands&, WorkerPool*) { return WorldView(w); }
    };
    // Full escape hatch: a raw World& can also mutate component values through
    // a non-const query, so its access cannot be analyzed -- the system is
    // marked exclusive and runs alone. Prefer Query/Res, or WorldView for
    // read-only access.
    template <>
    struct system_param<World&> {
        static void declare(SystemAccess& a) { a.exclusive = true; }
        static World& bind(World& w, Commands&, WorkerPool*) { return w; }
    };

} // namespace detail

class Schedule {
public:
    struct System {
        SystemId id = 0;
        std::string name;
        SystemAccess access;
        // move_only_function (not function) so a system may capture a move-only
        // value (e.g. a unique_ptr or a move_only_function of its own). The
        // WorkerPool* is null except under run(World&, WorkerPool&).
        ecs::detail::move_only_function<void(World&, Commands&, WorkerPool*)> run;
        int phase         = 0;
        std::size_t level = 0;
        bool once         = false;
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

    // Register a system whose access is *declared* (a runtime SystemAccess)
    // rather than derived from C++ parameter types -- the entry point for a
    // JS-defined system. `run` is the type-erased body (typically a runtime query
    // that dispatches per chunk) with the usual (World&, Commands&, WorkerPool*)
    // signature. It conflicts and levels against every other system by `access`
    // exactly like a native one, so JS and C++ systems share one wave plan.
    template <class Run>
    SystemId add_dynamic(std::string name,
                         SystemAccess access,
                         Run&& run,
                         int phase = 0,
                         bool once  = false) {
        System sys;
        auto const id = sys.id = ++next_id_;
        sys.name      = std::move(name);
        sys.access    = std::move(access);
        sys.phase     = phase;
        sys.once      = once;
        sys.run       = std::forward<Run>(run);
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

    // The executor. Systems run sequentially in dependency (wave) order; each
    // system's Query::for_each_chunk splits its rows across the pool's lanes --
    // data parallelism *within* a system, so it scales with cores beyond the
    // handful of independent systems a wave offers. A 1-lane pool is plain serial
    // execution. Recorded edits flush at each wave barrier; one-shot systems are
    // removed afterwards. A system that throws (in its body or a parallel kernel)
    // propagates the exception out of run(); the current wave's edits are not
    // flushed, as on any failed run.
    void run(World& world, WorkerPool& pool) {
        rebuild();
        auto cmds = Commands {world};
        for (auto const& wave : waves_) {
            for (auto const idx : wave)
                systems_[idx].run(world, cmds, &pool);
            world.apply_commands();
        }
        prune_once();
    }

private:
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
        Fn& fn, World& w, Commands& c, WorkerPool* pool, std::index_sequence<I...>) {
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
        sys.run =
            [fn = std::forward<Fn>(fn)](World& w, Commands& c, WorkerPool* pool) mutable {
                invoke<Args>(fn, w, c, pool, std::make_index_sequence<N> {});
            };
        auto const id = sys.id;
        systems_.push_back(std::move(sys));
        dirty_ = true;
        return id;
    }

    using IdList = std::vector<std::uint32_t>;

    static bool intersects(IdList const& a, IdList const& b) {
        return std::ranges::any_of(a, [b](auto& x) {
            return std::ranges::find(b, x) != b.end();
        });
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
        // each.
        auto groups = std::map<std::pair<int, std::size_t>, std::vector<std::size_t>> {};
        for (std::size_t i = 0; i < systems_.size(); ++i)
            groups[{systems_[i].phase, systems_[i].level}].push_back(i);
        waves_.clear();
        waves_.reserve(groups.size());
        for (auto& idxs : groups | std::views::values)
            waves_.push_back(std::move(idxs));
        dirty_ = false;
    }

    std::vector<System> systems_;
    std::vector<std::vector<std::size_t>> waves_;
    SystemId next_id_ = 0;
    bool dirty_       = true;
};

} // namespace ecs
