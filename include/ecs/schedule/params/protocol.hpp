// ecs/schedule/params/protocol.hpp
//
// The kernel-parameter protocol: how each parameter type of a kernel system
// declares access, owns per-item state, participates in the prepare/finish
// barrier hooks, and binds for one work item.
//
// A kernel system's work items run concurrently, so any cross-item state must
// be either read-only or privately owned per item. The substrate: for each
// stateful parameter the system owns an array of per-item SLOTS (indexed by
// the item's ordinal -- its position in generation order: archetypes
// ascending, rows ascending, i.e. the same order a serial query walks), plus
// two barrier-time hooks the executor drives around each wave:
//
//   prepare(state, world, item_rows, ctx)   before the dispatch,
//                                      single-threaded: size/reset the slots,
//                                      pre-size targets
//   finish(state, world)               after the join, before the command
//                                      flush, single-threaded: fold the slots
//                                      into the target in ORDINAL order
//
// Because slot contents are per-item and the fold order is canonical, every
// primitive built on this is deterministic at ANY lane count -- bitwise, even
// for float folds -- which per-row atomics can never give.
//
// The primitives themselves live in the sibling headers (reduce.hpp,
// extract.hpp, collect.hpp, events.hpp, scratch.hpp, random.hpp); this header
// holds the protocol, the primary template covering ordinary stateless
// parameters, and the whole-parameter-list drivers Schedule::add_kernel
// compiles against.

#pragma once

#include <ecs/schedule/params/state.hpp>
#include <ecs/schedule/system.hpp> // system_param, SystemParam, is_res_mut_v, KernelWaveContext
#include <ecs/world.hpp>           // World, Commands, Res/ResMut, resource_id

#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ecs::detail {

// Pack-level facts about a system's whole parameter list, computed under a
// given parameter policy `Param` (imperative_param or kernel_param).
template <template <class> class Param, class Args>
struct params_info;
template <template <class> class Param, class... A>
struct params_info<Param, std::tuple<A...>> {
    static constexpr bool all_allowed = (Param<A>::allowed && ...);
    static constexpr bool any_stateful =
        (!std::is_same_v<typename Param<A>::state, no_state> || ...);
    using states = std::tuple<typename Param<A>::state...>;
};

template <template <class> class Param, class Args, std::size_t... I>
void declare(SystemAccess& a, std::index_sequence<I...>) {
    (Param<std::tuple_element_t<I, Args>>::declare(a), ...);
}

template <template <class> class Param, class Args, class States, std::size_t... I>
void prepare_all(States& st,
                 World& w,
                 std::span<std::uint32_t const> rows,
                 KernelWaveContext const& ctx,
                 std::index_sequence<I...>) {
    (Param<std::tuple_element_t<I, Args>>::prepare(std::get<I>(st), w, rows, ctx), ...);
}
template <template <class> class Param, class Args, class States, std::size_t... I>
void finish_all(States& st, World& w, std::index_sequence<I...>) {
    (Param<std::tuple_element_t<I, Args>>::finish(std::get<I>(st), w), ...);
}

template <template <class> class Param,
          class Args,
          class Fn,
          class States,
          std::size_t... I>
void invoke(Fn& fn,
            States& st,
            World& w,
            Commands& c,
            WorkItem const& item,
            std::index_sequence<I...>) {
    fn(Param<std::tuple_element_t<I, Args>>::bind(std::get<I>(st), w, c, item)...);
}
}
