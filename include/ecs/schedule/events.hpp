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
#include <cstdint>
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
        // The command-flush portion of the wave (the flush runs at the
        // barrier, inside the WaveBegin/WaveEnd interval).
        double flush_us = 0;
    };
    // Wall-clock bracketing of ONE system running alone in its wave (the only
    // case where a per-system wall interval exists -- a multi-system wave runs
    // every system's work items interleaved across the pool's lanes in one
    // dispatch, so fanned waves emit no SystemBegin/SystemEnd at all).
    struct SystemBegin {
        SystemId id;
        std::string_view name;
    };
    struct SystemEnd {
        SystemId id;
    };
    // Measured work for one system in one wave -- emitted for EVERY system,
    // lone or fanned, after the wave's barrier hooks and its flush.
    // busy_us is the sum of the system's work-item durations across all lanes
    // (CPU time, not wall: a fanned wave's busy times legitimately sum past
    // the wave's wall duration); prepare_us/finish_us bracket the system's
    // single-threaded barrier hooks (Reduce folds, Extract pre-sizing...), so
    // a kernel whose finish fold rivals its dispatch is visible directly.
    // flush_us is the part of the wave's command flush spent applying THIS
    // system's commands (spawns/despawns/sort...) -- so a system whose real
    // cost is a barrier command, not busy work, is attributed directly.
    struct SystemWork {
        SystemId id;
        std::string_view name;
        double busy_us      = 0;
        double prepare_us   = 0;
        double finish_us    = 0;
        std::uint32_t items = 0; // work items this wave (1 for imperative)
        double flush_us     = 0; // this system's share of the wave's cmd flush
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
                                   sched_event::SystemWork,
                                   sched_event::TickAbort>;

} // namespace ecs
