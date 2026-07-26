#pragma once

#include <ecs/schedule/params/state.hpp>
#include <ecs/schedule/system.hpp>

#include <cstdint>
#include <span>

namespace ecs::detail {
// The serial-parameter protocol: how each parameter type of an
// add_serial() system declares access, owns per-system state, and
// binds for a run. The primary template covers every ordinary system
// parameter (stateless, binds through system_param); Local<T> is the one
// stateful face.
template <class P>
struct serial_param {
    static constexpr bool allowed = SystemParam<P>;
    using state                   = no_state;
    static void declare(SystemAccess& a) { system_param<P>::declare(a); }
    static void prepare(state&,
                        World&,
                        std::span<std::uint32_t const>,
                        WaveContext const&) {}
    static void finish(state&, World&) {}
    static P bind(state&, World& w, Commands& c, WorkItem const& item) {
        return system_param<P>::bind(w, c, item);
    }
};
}
