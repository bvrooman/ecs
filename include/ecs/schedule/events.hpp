// ecs/schedule/events.hpp
//
// The events Schedule::run emits as it executes, as one std::variant -- each
// alternative carries the data for that boundary (`level` is the 0-based wave
// index in execution order). Observe them via Schedule::events(); an observer is
// any callable void(ScheduleEvent const&), typically one that std::visits (see
// event/emitter.hpp and event/observer.hpp). This is the successor to the old
// virtual ScheduleObserver: the core announces boundaries as values and
// observers decide what to do with them (time, log, count, ...). The profiler
// in tools/ is one such observer.

#pragma once

#include "access.hpp" // SystemId
#include <cstddef>
#include <string_view>
#include <variant>

namespace ecs {

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

} // namespace ecs
