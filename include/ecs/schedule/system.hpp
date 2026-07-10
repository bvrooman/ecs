// ecs/schedule/system.hpp
//
// What a registered system IS: the SystemRecord the Schedule stores, plus the
// two protocols that populate it --
//
//   * the system-parameter protocol (detail::system_param<P>): how an
//     imperative system's parameter types declare access and are bound when it
//     runs, and
//   * the kernel plumbing (detail::make_chunk): how a declarative kernel
//     system's chunk arguments are built for a row range.
//
// The Schedule (schedule.hpp) consumes these; the executor (executor.hpp)
// consumes SystemRecord.

#pragma once

#include "../detail/move_only_function.hpp"
#include "../query.hpp"
#include "../world.hpp"
#include "access.hpp"
#include <cstddef>
#include <cstdint>
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
struct fn_traits<R (C::*)(A...)&> {
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
// Full escape hatch: a raw World& can also mutate component values through
// a non-const query, so its access cannot be analyzed -- the system is
// marked exclusive and runs alone. Prefer Query/Res, or WorldView for
// read-only access.
template <>
struct system_param<World&> {
    static void declare(SystemAccess& a) { a.exclusive = true; }
    static World& bind(World& w, Commands&, WorkerPool&) { return w; }
};

// A supported system parameter: anything the protocol above can declare and
// bind. The primary system_param template is intentionally undefined, so an
// unsupported type (Query<..> const&, T*, int, ...) fails this concept instead
// of erroring inside an instantiation.
template <class P>
concept SystemParam = requires(SystemAccess& a, World& w, Commands& c, WorkerPool& p) {
    system_param<P>::declare(a);
    system_param<P>::bind(w, c, p);
};

template <class Args>
inline constexpr bool all_system_params_v = false;
template <class... A>
inline constexpr bool all_system_params_v<std::tuple<A...>> = (SystemParam<A> && ...);

template <class P>
inline constexpr bool is_query_param_v = false;
template <class... Cs>
inline constexpr bool is_query_param_v<Query<Cs...>> = true;

// A parameter allowed AFTER the chunks of a kernel system. Everything a
// kernel item binds must be safe to hand to concurrently-running items of the
// SAME system: Res<T> (shared read), Commands& (sharded recording), WorldView
// (read-only), and ResMut<T> (allowed, but the resource is shared across the
// system's concurrent items -- treat it like any shared state in a
// for_each_chunk kernel). A Query would recreate the nested-dispatch hazard
// and a World& would hand unanalyzable mutable access to parallel items, so
// both are excluded.
template <class P>
concept KernelExtraParam = SystemParam<P> && !is_query_param_v<P> &&
                           !std::is_same_v<P, World&>;

template <class Args>
inline constexpr bool all_kernel_extras_v = false;
template <class... E>
inline constexpr bool all_kernel_extras_v<std::tuple<E...>> =
    (KernelExtraParam<E> && ...);

// The tuple of parameters after the first Off (used to slice a kernel's
// trailing extras off its (span, chunks..., extras...) signature).
template <class Tuple,
          std::size_t Off,
          class = std::make_index_sequence<std::tuple_size_v<Tuple> - Off>>
struct tuple_tail_impl;
template <class Tuple, std::size_t Off, std::size_t... I>
struct tuple_tail_impl<Tuple, Off, std::index_sequence<I...>> {
    using type = std::tuple<std::tuple_element_t<Off + I, Tuple>...>;
};
template <class Tuple, std::size_t Off>
using tuple_tail = typename tuple_tail_impl<Tuple, Off>::type;

// Fold a parameter pack's declared access into `a` / bind and invoke -- the
// two halves of the imperative-system protocol, driven by Schedule::add.
template <class Args, std::size_t... I>
void declare_params(SystemAccess& a, std::index_sequence<I...>) {
    (system_param<std::tuple_element_t<I, Args>>::declare(a), ...);
}
template <class Args, class Fn, std::size_t... I>
void invoke_with_params(
    Fn& fn, World& w, Commands& c, WorkerPool& pool, std::index_sequence<I...>) {
    fn(system_param<std::tuple_element_t<I, Args>>::bind(w, c, pool)...);
}

// Build the chunk argument for component C over rows [b, e) of an archetype --
// the kernel-system counterpart of Query's private chunk_arg.
template <class C>
chunk<C> make_chunk(Archetype& arch, std::size_t b, std::size_t e) {
    return chunk<C>(arch.column<std::remove_const_t<C>>().store, b, e);
}

// A memoized query match list, keyed by (world instance, archetype
// generation) -- the per-record counterpart of Query's per-type thread_local
// memo, for kernel systems whose component set is a runtime value.
struct MatchCache {
    std::vector<std::uint32_t> const* list = nullptr;
    std::uint64_t world                    = 0;
    std::uint64_t gen                      = ~std::uint64_t {0};

    std::vector<std::uint32_t> const& resolve(World const& w, Signature const& sig) {
        if (world != w.instance_id() || gen != w.archetype_generation()) {
            list  = &w.matching_archetypes(sig);
            world = w.instance_id();
            gen   = w.archetype_generation();
        }
        return *list;
    }
};

// One registered system, in either of its two forms.
struct SystemRecord {
    SystemId id = 0;
    std::string name;
    SystemAccess access;
    // Imperative body (add/add_once/add_dynamic): opaque to the scheduler,
    // so the whole system is ONE work item. Null for kernel systems.
    // move_only_function (not function) so a system may capture a move-only
    // value (e.g. a unique_ptr or a move_only_function of its own).
    move_only_function<void(World&, Commands&, WorkerPool&)> run;
    // Kernel body (add_kernel): the executor slices the matched rows into
    // work items and invokes this once per (archetype, row-range) item; the
    // Commands& lets the kernel's trailing extra parameters bind. Null for
    // imperative systems.
    move_only_function<void(World&, Commands&, std::uint32_t, std::size_t, std::size_t)>
        run_range;
    Signature query_sig; // kernel systems: sorted required-component ids
    MatchCache match;    // kernel systems: memoized query_sig match list
    int phase         = 0;
    std::size_t level = 0;
    bool once         = false;

    [[nodiscard]]
    bool is_kernel() const noexcept {
        return static_cast<bool>(run_range);
    }
};

} // namespace ecs::detail
