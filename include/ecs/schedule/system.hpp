// ecs/schedule/system.hpp
//
// What a registered system IS: the SystemRecord the Schedule stores, plus the
// two protocols that populate it --
//
//   * the system-parameter protocol (detail::system_param<P>): how a system's
//     parameter types declare access and are bound when it runs, and
//   * the kernel plumbing (query_param_traits / bind_kernel_param): how a
//     kernel system's single Query parameter binds restricted to one work
//     item's row range.
//
// The Schedule (schedule.hpp) consumes these; the executor (executor.hpp)
// consumes SystemRecord.

#pragma once

#include "../detail/move_only_function.hpp"
#include "../query.hpp"
#include "../world.hpp"
#include "access.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ecs::detail {

// --- extract a callable's parameter types ----------------------------------
// fn_traits covers the member-call-operator forms (cv/ref/noexcept variants);
// callable_args below is the SFINAE-friendly entry point that dispatches a
// functor through &F::operator() and handles plain functions directly (a
// function reference decays to the pointer form).
template <class F>
struct fn_traits; // primary undefined: probed via callable_args
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
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) &> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const&> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) & noexcept> {
    using args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const & noexcept> {
    using args = std::tuple<A...>;
};

// SFINAE-friendly: `type` exists only for callables whose parameter list can
// be recovered -- a functor with exactly one non-template operator() (a
// generic lambda or an overload set fails the void_t probe rather than
// hard-erroring), or a function pointer/reference.
template <class F, class = void>
struct callable_args {};
template <class F>
struct callable_args<F, std::void_t<decltype(&F::operator())>> {
    using type = typename fn_traits<decltype(&F::operator())>::args;
};
template <class R, class... A>
struct callable_args<R (*)(A...), void> {
    using type = std::tuple<A...>;
};
template <class R, class... A>
struct callable_args<R (*)(A...) noexcept, void> {
    using type = std::tuple<A...>;
};

template <class F>
using system_args_t = typename callable_args<std::decay_t<F>>::type;

// A system whose parameter types can be recovered at all (the prerequisite
// for deriving access from them).
template <class F>
concept IntrospectableSystem =
    requires { typename callable_args<std::decay_t<F>>::type; };

// --- system parameter protocol: declare(access) + bind(world, commands) ----
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
// shared 1-lane pool, so it is never null here; the Query carries it so
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
// Note there is deliberately NO system_param<World&>: raw world access cannot
// be analyzed and is dangerous under any concurrent executor, so it is not a
// system parameter. Setup happens on the World directly (outside a run),
// mutation goes through Commands, ad-hoc reads through WorldView; a system
// registered via add_dynamic may still declare itself `exclusive` when its
// access is genuinely unanalyzable.

// A supported system parameter: anything the protocol above can declare and
// bind. The primary system_param template is intentionally undefined, so an
// unsupported type (Query<..> const&, T*, int, ...) fails this concept instead
// of erroring inside an instantiation.
template <class P>
concept SystemParam = requires(SystemAccess& a, World& w, Commands& c, WorkerPool& p) {
    system_param<P>::declare(a);
    system_param<P>::bind(w, c, p);
};

template <class P>
inline constexpr bool is_query_param_v = false;
template <class... Cs>
inline constexpr bool is_query_param_v<Query<Cs...>> = true;

template <class P>
inline constexpr bool is_res_mut_v = false;
template <class T>
inline constexpr bool is_res_mut_v<ResMut<T>> = true;

template <class Args>
inline constexpr bool any_res_mut_v = false;
template <class... A>
inline constexpr bool any_res_mut_v<std::tuple<A...>> = (is_res_mut_v<A> || ...);

// How many parameters of Args are Query<Cs...>, and where the first one sits.
// A kernel system must have exactly one: it is the iteration the executor
// slices into work items.
template <class Args>
struct query_info;
template <class... A>
struct query_info<std::tuple<A...>> {
    static constexpr std::size_t count =
        (std::size_t {0} + ... + (is_query_param_v<A> ? 1u : 0u));
    static constexpr std::size_t index = [] {
        constexpr bool is_q[] = {is_query_param_v<A>..., false};
        for (std::size_t i = 0; i < sizeof...(A); ++i)
            if (is_q[i])
                return i;
        return ~std::size_t {0};
    }();
};

// The slicing half of a kernel system's Query parameter: its required
// signature (for match building) and the restricted bind the executor hands
// each work item. Befriended by Query for the private pieces.
template <class P>
struct query_param_traits;
template <class... Cs>
struct query_param_traits<Query<Cs...>> {
    static Signature const& signature() { return Query<Cs...>::required(); }
    static Query<Cs...> bind_slice(World& w,
                                   ArchetypeId archetype,
                                   std::size_t b,
                                   std::size_t e) {
        return Query<Cs...>(w, parallel::serial_pool(), archetype, b, e);
    }
};

// The whole-parameter-list declare/invoke drivers live with the parameter
// protocols: schedule/params/local.hpp for imperative systems (which adds the
// stateful Local<T> face), schedule/params/protocol.hpp for kernel systems.

// Timing shorthand shared by the schedule/executor instrumentation.
using sched_clock = std::chrono::steady_clock;
inline double elapsed_us(sched_clock::time_point const t0) {
    return std::chrono::duration<double, std::micro>(sched_clock::now() - t0).count();
}

// Identity a wave hands to stateful kernel parameters' prepare hooks: which
// system is preparing and which schedule tick this is. Random derives its
// per-item stream seeds from these (deterministic, lane-count-independent);
// most parameters ignore it.
struct KernelWaveContext {
    SystemId system    = {};
    std::uint64_t tick = 0;
};

// A memoized query match list, keyed by (world instance, archetype
// generation) -- the per-record counterpart of Query's per-type thread_local
// memo, for kernel systems whose component set is a runtime value.
struct MatchCache {
    std::vector<ArchetypeId> const* list = nullptr;
    WorldId world                        = WorldId::none();
    std::uint64_t gen                    = ~std::uint64_t {0};

    std::vector<ArchetypeId> const& resolve(World const& w, Signature const& sig) {
        if (world != w.instance_id() || gen != w.archetype_generation()) {
            list  = &w.matching_archetypes(sig);
            world = w.instance_id();
            gen   = w.archetype_generation();
        }
        return *list;
    }
};

using RunFn      = move_only_function<void(World&, Commands&, WorkerPool&)>;
using RunRangeFn = move_only_function<
    void(World&, Commands&, ArchetypeId, std::size_t, std::size_t, std::uint32_t)>;
using PrepareItemsFn = move_only_function<
    void(World&, std::span<std::uint32_t const>, KernelWaveContext const&)>;
using FinishItemsFn = move_only_function<void(World&)>;

// One registered system, in either of its two forms.
struct SystemRecord {
    using Phase   = int;
    using Level   = std::uint32_t;
    using WaveKey = std::pair<Phase, Level>;

    SystemId id = {};
    std::string name;
    SystemAccess access;
    // Imperative body (add/add_once/add_dynamic): opaque to the scheduler,
    // so the whole system is ONE work item. Null for kernel systems.
    // move_only_function (not function) so a system may capture a move-only
    // value (e.g. a unique_ptr or a move_only_function of its own).
    RunFn run;
    // Kernel body (add_kernel): the executor slices the matched rows into
    // work items and invokes this once per (archetype, row-range) item. The
    // Commands& lets the kernel's other parameters bind; the trailing ordinal
    // is the item's index in generation (serial-walk) order, which indexes the
    // per-item slots of stateful parameters (Reduce/Extract -- see
    // kernel_params.hpp). Null for imperative systems.
    RunRangeFn run_range;
    // Barrier hooks for stateful kernel parameters (null when the system has
    // none): prepare runs single-threaded before the wave's dispatch with the
    // system's per-item row counts in ordinal order plus the wave context;
    // finish runs single-threaded after the join, before the command flush
    // (skipped on abort).
    PrepareItemsFn prepare_items;
    FinishItemsFn finish_items;
    Signature query_sig; // kernel systems: sorted required-component ids
    MatchCache match;    // kernel systems: memoized query_sig match list
    Phase phase = 0;
    Level level = 0;
    bool once   = false;
    // Tombstone: a removed (or spent one-shot) system. Its slot is retained so
    // every other system keeps its position -- and a SystemId, being that
    // position, stays valid. Skipped by leveling, wave building, and size().
    bool dead = false;
    // Cadence: the system runs only on ticks where tick % every == 0 (every==1
    // is every tick). A skipped system contributes no work items that tick;
    // a wave left empty by skips runs no barrier. Always >= 1.
    std::uint64_t every = 1;

    [[nodiscard]]
    bool is_kernel() const noexcept {
        return static_cast<bool>(run_range);
    }

    [[nodiscard]]
    WaveKey wave_key() const noexcept {
        return {phase, level};
    }
};

} // namespace ecs::detail
