#pragma once

#include <ecs/schedule/system.hpp>
#include <ecs/schedule/params/state.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ecs::detail {

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

// The primary template covers every ordinary system parameter (stateless; binds
// through system_param). The kernel's single Query parameter is NOT routed
// through here -- system_param<Query<Cs...>> binds it sliced to the work item
// (see Schedule::add_kernel and query.hpp). Each primitive specializes this
// template in its own header.
template <class P>
struct kernel_param {
    static constexpr bool allowed = KernelParam<P>;
    using state                   = no_state;
    static void declare(SystemAccess& a) { system_param<P>::declare(a); }
    static void prepare(state&,
                        World&,
                        std::span<std::uint32_t const>,
                        KernelWaveContext const&) {}
    static void finish(state&, World&) {}
    static P bind(state&, World& w, Commands& c, WorkItem const& item) {
        return system_param<P>::bind(w, c, item);
    }
};
}