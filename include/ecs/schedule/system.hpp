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
    // work items and invokes this once per (archetype, row-range) item.
    // Null for imperative systems.
    move_only_function<void(World&, std::uint32_t, std::size_t, std::size_t)> run_range;
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
