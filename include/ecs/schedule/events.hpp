// ecs/schedule/events.hpp
//
// The events Schedule::run emits as it executes, as one std::variant -- each
// alternative carries the data for that boundary. Observe them via
// Schedule::events(); an observer is any callable void(ScheduleEvent const&),
// typically one that std::visits (see event/emitter.hpp and
// event/observer.hpp). This is the successor to the old virtual
// ScheduleObserver: the core announces boundaries as values and observers
// decide what to do with them (time, log, count, ...). The profiler in tools/
// is one such observer.

#pragma once

#include <ecs/schedule/wave_id.hpp>
#include <ecs/system_id.hpp>

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
        WaveId ordinal;
        std::size_t n_systems;
    };
    struct WaveEnd {
        WaveId ordinal;
        // The command-flush portion of the wave (the flush runs at the
        // barrier, inside the WaveBegin/WaveEnd interval).
        double flush_us = 0;
        // The wave's single-threaded build.
        double build_us = 0;
        double sort_us  = 0;
    };
    // Measured work for one system in one wave -- emitted for EVERY system,
    // lone or fanned, after the wave's barrier hooks and its flush. This is the
    // per-system boundary observers key on; there is no separate wall-clock
    // begin/end bracket, since a fanned wave interleaves every system's items
    // across the lanes and so has no per-system wall interval.
    // busy_us is the sum of the system's work-item durations across all lanes
    // (CPU time, not wall: a fanned wave's busy times legitimately sum past
    // the wave's wall duration); prepare_us/finish_us bracket the system's
    // single-threaded barrier hooks (Reduce folds, Extract pre-sizing...), so
    // a parallel system whose finish fold rivals its dispatch is visible directly.
    // flush_us is the part of the wave's command flush spent applying THIS
    // system's commands (spawns/despawns/sort...) -- so a system whose real
    // cost is a barrier command, not busy work, is attributed directly.
    struct SystemWork {
        SystemId id;
        std::string_view name;
        double busy_us      = 0;
        double prepare_us   = 0;
        double finish_us    = 0;
        std::uint32_t items = 0; // work items this wave (1 for serial)
        double flush_us     = 0; // this system's share of the wave's cmd flush
    };
    // Emitted instead of the remaining WaveEnd/TickEnd when a system throws and
    // the run unwinds, so observers tracking an open tick/wave can reset instead
    // of carrying stale state into the next tick.
    struct TickAbort {
        WaveId ordinal;
        SystemId system;
    };
} // namespace sched_event

using ScheduleEvent = std::variant<sched_event::TickBegin,
                                   sched_event::TickEnd,
                                   sched_event::WaveBegin,
                                   sched_event::WaveEnd,
                                   sched_event::SystemWork,
                                   sched_event::TickAbort>;

} // namespace ecs
