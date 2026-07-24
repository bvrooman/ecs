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

#include "../../query.hpp" // Query, query_param_traits (slicing)
#include "../../world.hpp" // World, Commands, Res/ResMut, resource_id
#include "../system.hpp"   // system_param, SystemParam, is_res_mut_v, KernelWaveContext
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ecs {
namespace detail {

    // Reset a slot or target for the next run, keeping heap capacity where the
    // type offers it (vectors, maps, strings); assign a fresh value otherwise.
    template <class T>
    void reset_value(T& v) {
        if constexpr (requires { v.clear(); })
            v.clear();
        else
            v = T {};
    }

    // The per-item slots every appending/folding face shares: one T per work
    // item, indexed by ordinal, reset (capacity retained) at each prepare.
    // Slots are cache-line padded because items write into their slot
    // throughout iteration, and adjacent small values would otherwise
    // false-share across lanes -- ~kTargetItemsPerKernel slots per system, so
    // the padding costs a few KB.
    template <class T>
    struct slot_array {
        struct alignas(128) Slot {
            T value {};
        };
        std::vector<Slot> slots;
        std::size_t active = 0;

        void prepare(std::size_t const n) {
            active = n;
            if (slots.size() < n)
                slots.resize(n);
            for (std::size_t i = 0; i < n; ++i)
                reset_value(slots[i].value);
        }
        [[nodiscard]]
        T& operator[](std::size_t const i) noexcept {
            return slots[i].value;
        }
    };

    struct no_state {};

    // The primary template covers every ordinary system parameter (stateless;
    // binds through system_param against the shared 1-lane pool). The kernel's
    // single Query parameter is NOT routed through here -- the executor binds
    // it sliced via query_param_traits (see Schedule::add_kernel). Each
    // primitive specializes this template in its own header.
    template <class P>
    struct kernel_param {
        static constexpr bool allowed = SystemParam<P> && !is_res_mut_v<P>;
        using state                   = no_state;
        static void declare(SystemAccess& a) { system_param<P>::declare(a); }
        static void prepare(state&,
                            World&,
                            std::span<std::uint32_t const>,
                            KernelWaveContext const&) {}
        static void finish(state&, World&) {}
        static P bind(
            state&, World& w, Commands& c, WorkItem const& item, std::uint32_t) {
            return system_param<P>::bind(w, c, item);
        }
    };

    // Pack helpers over a kernel's full parameter tuple.
    template <class Args>
    struct kernel_params_info;
    template <class... A>
    struct kernel_params_info<std::tuple<A...>> {
        static constexpr bool all_allowed =
            ((is_query_param_v<A> || kernel_param<A>::allowed) && ...);
        static constexpr bool any_stateful =
            (!std::is_same_v<typename kernel_param<A>::state, no_state> || ...);
        using states = std::tuple<typename kernel_param<A>::state...>;
    };

    // Whole-parameter-list drivers, folded over the kernel's Args tuple.
    // Plain function templates (not nested generic lambdas): they are called
    // from closures in Schedule::add_kernel, and keeping them ordinary
    // functions keeps the instantiation shape simple.
    template <class A>
    void kernel_declare_one(SystemAccess& a) {
        if constexpr (is_query_param_v<A>)
            system_param<A>::declare(a);
        else
            kernel_param<A>::declare(a);
    }
    template <class Args, std::size_t... I>
    void kernel_declare(SystemAccess& a, std::index_sequence<I...>) {
        (kernel_declare_one<std::tuple_element_t<I, Args>>(a), ...);
    }

    template <class Args, class States, std::size_t... I>
    void kernel_prepare_all(States& st,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const& ctx,
                            std::index_sequence<I...>) {
        (kernel_param<std::tuple_element_t<I, Args>>::prepare(std::get<I>(st),
                                                              w,
                                                              rows,
                                                              ctx),
         ...);
    }
    template <class Args, class States, std::size_t... I>
    void kernel_finish_all(States& st, World& w, std::index_sequence<I...>) {
        (kernel_param<std::tuple_element_t<I, Args>>::finish(std::get<I>(st), w), ...);
    }

    template <class P>
    struct KernelBind {
        template <class State>
        static decltype(auto) bind(State& s,
                                   World& w,
                                   Commands& c,
                                   WorkItem const& item,
                                   std::uint32_t ordinal) {

            return kernel_param<P>::bind(s, w, c, item, ordinal);
        }
    };
    template <class... Cs>
    struct KernelBind<Query<Cs...>> {
        template <class State>
        static decltype(auto) bind(
            State&, World& w, Commands& c, WorkItem const& item, std::uint32_t) {
            return system_param<Query<Cs...>>::bind(w, c, item);
        }
    };

    template <class Args, class Fn, class States, std::size_t... I>
    void kernel_invoke(Fn& fn,
                       States& st,
                       World& w,
                       Commands& c,
                       WorkItem const& item,
                       std::uint32_t ordinal,
                       std::index_sequence<I...>) {
        fn(KernelBind<std::tuple_element_t<I, Args>>::bind(std::get<I>(st),
                                                           w,
                                                           c,
                                                           item,
                                                           ordinal)...);
    }

} // namespace detail
} // namespace ecs
