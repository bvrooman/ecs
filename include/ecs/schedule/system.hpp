// ecs/schedule/system.hpp
//
// What a registered system IS: the SystemRecord the Schedule stores, plus the
// two protocols that populate it --
//
//   * the system-parameter protocol (detail::system_param<P>): how a system's
//     parameter types declare access and are bound when it runs, and
//   * the parallel plumbing (system_param<Query<Cs...>>): how a parallel system's
//     single Query parameter binds restricted to one work item's row range.
//
// The Schedule (schedule.hpp) consumes these; the Wave executor (wave.hpp)
// consumes SystemRecord.

#pragma once

#include <ecs/detail/compat/move_only_function.hpp>
#include <ecs/query.hpp> // Query -- system_param<Query<Cs...>>
#include <ecs/schedule/access.hpp>
#include <ecs/schedule/work_item.hpp>
#include <ecs/world.hpp> // World, Commands, WorldView

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
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

template <class... Cs>
struct system_param<Query<Cs...>> {
    static void declare(SystemAccess& a) { (declare_component<Cs>(a), ...); }
    static Query<Cs...> bind(World& w, Commands&, WorkItem const& item) {
        return Query<Cs...>(w, item);
    }
    static Signature const& signature() { return Query<Cs...>::required(); }
};
template <class T>
struct system_param<Res<T>> {
    static void declare(SystemAccess& a) { a.res_reads.push_back(resource_id<T>); }
    static Res<T> bind(World& w, Commands&, WorkItem const&) { return Res<T>(w); }
};
template <class T>
struct system_param<ResMut<T>> {
    static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }
    static ResMut<T> bind(World& w, Commands&, WorkItem const&) { return ResMut<T>(w); }
};
template <>
struct system_param<Commands&> {
    // A side channel: no tracked component/resource access. We still flag it
    // so tooling can surface that the system records commands.
    static void declare(SystemAccess& a) { a.commands = true; }
    static Commands& bind(World&, Commands& c, WorkItem const&) { return c; }
};
// Read-only ad-hoc access: WorldView reads everything but writes nothing,
// so it runs in parallel with other readers and is serialized only against
// writers.
template <>
struct system_param<WorldView> {
    static void declare(SystemAccess& a) { a.reads_all = true; }
    static WorldView bind(World& w, Commands&, WorkItem const&) { return WorldView(w); }
};
// Note there is deliberately NO system_param<World&>: raw world access cannot
// be analyzed and is dangerous under any concurrent executor, so it is not a
// system parameter. Setup happens on the World directly (outside a run),
// mutation goes through Commands, ad-hoc reads through WorldView; a system
// registered via add_dynamic_serial may still declare itself `exclusive` when its
// access is genuinely unanalyzable.

// A supported system parameter: anything the protocol above can declare and
// bind. The primary system_param template is intentionally undefined, so an
// unsupported type (Query<..> const&, T*, int, ...) fails this concept instead
// of erroring inside an instantiation.
template <class P>
concept SystemParam =
    requires(SystemAccess& a, World& w, Commands& c, WorkItem const& item) {
        system_param<P>::declare(a);
        system_param<P>::bind(w, c, item);
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

template <class P>
concept ParallelParam = SystemParam<P> && !is_res_mut_v<P>;

// How many parameters of Args are Query<Cs...>, and where the first one sits.
// A parallel system must have exactly one: it is the iteration the executor
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
    static constexpr bool has_query = index != ~std::size_t {0};
};

// The whole-parameter-list declare/invoke drivers live in
// schedule/params/protocol.hpp. They are generic over the parameter policy, so
// one set serves both kinds -- instantiated with serial_param for add() and
// with parallel_param for add_parallel().

// Timing shorthand shared by the schedule/executor instrumentation.
using sched_clock = std::chrono::steady_clock;
inline double elapsed_us(sched_clock::time_point const t0) {
    return std::chrono::duration<double, std::micro>(sched_clock::now() - t0).count();
}

// Identity a wave hands to stateful parameters' prepare hooks: which
// system is preparing and which schedule tick this is. Random derives its
// per-item stream seeds from these (deterministic, lane-count-independent);
// most parameters ignore it.
struct WaveContext {
    SystemId system    = {};
    std::uint64_t tick = 0;
};

// A memoized query match list, keyed by (world instance, archetype
// generation) -- the per-record counterpart of Query's per-type thread_local
// memo, for parallel systems whose component set is a runtime value.
struct MatchCache {
    std::vector<ArchetypeId> const* list = nullptr;
    WorldId world                        = WorldId::none();
    std::uint64_t gen                    = ~std::uint64_t {0};

    auto const& resolve(World const& w, Signature const& sig) {
        if (world != w.instance_id() || gen != w.archetype_generation()) {
            list  = &w.matching_archetypes(sig);
            world = w.instance_id();
            gen   = w.archetype_generation();
        }
        return *list;
    }
};

using RunFn = move_only_function<void(World&, Commands&, WorkItem const&)>;
using PrepareItemsFn =
    move_only_function<void(World&, std::span<std::uint32_t const>, WaveContext const&)>;
using FinishItemsFn = move_only_function<void(World&)>;

// One registered system, in either of its two forms.
struct SystemRecord {
    using Phase   = int;
    using Level   = std::uint32_t;
    using WaveKey = std::pair<Phase, Level>;

    SystemId id = {};
    std::string name;
    SystemAccess access;
    RunFn run;
    PrepareItemsFn prepare_items;
    FinishItemsFn finish_items;
    Signature query_sig = Signature::null();
    MatchCache match; // parallel systems: memoized query_sig match list
    Phase phase = 0;
    Level level = 0;
    // Run budget: the system is retired after it has been due `times` times
    // (0 = unlimited). times == 1 is the one-shot/setup case. Counted in
    // Schedule::run only -- prewarm() also walks the wave plans, and must not
    // spend a system's budget before it has run.
    std::uint64_t times = 0;
    std::uint64_t runs  = 0;
    // Tombstone: a removed (or spent) system. Its slot is retained so
    // every other system keeps its position -- and a SystemId, being that
    // position, stays valid. Skipped by leveling, wave building, and size().
    bool dead = false;
    // Cadence: the system runs only on ticks where tick % every == 0 (every==1
    // is every tick). A skipped system contributes no work items that tick;
    // a wave left empty by skips runs no barrier. Always >= 1.
    std::uint64_t every = 1;
    bool is_parallel    = false;

    [[nodiscard]]
    WaveKey wave_key() const noexcept {
        return {phase, level};
    }
};

} // namespace ecs::detail
