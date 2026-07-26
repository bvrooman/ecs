// ecs/diag/schedule_trace.hpp
//
// A schedule observer that writes a per-tick timing trace: one CSV row per
// system per tick, buffered and flushed to a Sink. Feed it to a file for
// offline analysis -- the tools/schedule_report package renders it into a
// self-contained HTML report (see tools/schedule_report/README.md). It is a
// plain callable observer (operator()(ScheduleEvent const&) that
// std::visits), registered on the schedule's event emitter:
//
//   std::ofstream csv("trace.csv");
//   ecs::diag::ScheduleTrace trace(ecs::diag::to_stream(csv));
//   sched.events().add(std::ref(trace));   // or: subscribe(std::ref(trace))
//   for (;;) sched.run(world, pool);
//   trace.flush();                          // drain the tail at shutdown
//
// Columns (every measurement numeric; `system` is the row's key):
//
//   tick        run() ordinal since the trace attached (0-based)
//   wave        the system's wave index within the tick
//   system      system name
//   busy_us     sum of the system's work-item durations across all lanes
//               (CPU time: a fanned wave's busy columns sum past wall time)
//   prepare_us  the system's single-threaded barrier prepare hook
//   finish_us   the system's single-threaded barrier finish hook (folds)
//   items       work items the system contributed (1 for imperative)
//   wave_us     wall duration of the whole wave (repeated on each of the
//               wave's rows; includes the flush)
//   flush_us    command-flush portion of wave_us (the WHOLE wave's, repeated
//               on each of the wave's rows)
//   tick_us     wall duration of the whole tick (repeated on the tick's rows)
//   cmd_us      the part of the wave's command flush spent applying THIS
//               system's commands (spawns/despawns/sort...) -- so a system
//               whose real cost is a barrier command, not busy work, carries
//               it here. Sums to flush_us across the wave's systems.
//   build_us    wall duration of the wave build's flatten phase. Repeated per row.
//   sort_us     wall duration of the LPT sort. Repeated per row.
//
// A per-N-tick system (Schedule `every`) simply has rows only on the ticks it
// ran -- like a one-shot phase, its wave exists for a subset of ticks.
//
// It is one of the two timing observers the profiler was split into (the other
// is ScheduleReport, which aggregates instead of tracing); register either,
// both, or neither. Unlike the report it keeps no distribution -- just raw rows
// -- so it has no TickStats dependency.
//
// Not gated by any macro -- always available; whether to attach it is the
// application's choice.

#pragma once

#include <ecs/event/observer.hpp> // ecs::event::overloaded
#include <ecs/schedule.hpp>        // ecs::ScheduleEvent, ecs::sched_event, ecs::SystemId
#include <ecs/diag/sink.hpp>                // ecs::diag::Sink
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ecs::diag {

// Not thread-safe: attach to one Schedule driven from one thread. Self-timing
// for the wave/tick wall columns, so it composes with other observers (e.g.
// ScheduleReport) independently; the per-system columns carry the executor's
// own measurements (SystemWork). It reacts to only the events it needs (a
// partial visitor: the trailing catch-all ignores the rest and any event kind
// added later).
class ScheduleTrace {
public:
    explicit ScheduleTrace(Sink sink, double flush_every_s = 2.0)
        : sink_(std::move(sink))
        , period_(std::chrono::duration_cast<clock::duration>(
              std::chrono::duration<double>(flush_every_s)))
        , last_flush_(clock::now()) {
        sink_("tick,wave,system,busy_us,prepare_us,finish_us,items,"
              "wave_us,flush_us,tick_us,cmd_us,build_us,sort_us");
    }

    void operator()(ScheduleEvent const& e) {
        using namespace sched_event;
        std::visit(
            event::overloaded {
                [this](TickBegin const&) {
                    tick_first_row_ = rows_.size();
                    t_tick_         = clock::now();
                },
                [this](WaveBegin const& ev) {
                    cur_wave_       = ev.level;
                    wave_first_row_ = rows_.size();
                    t_wave_         = clock::now();
                },
                [this](SystemWork const& ev) {
                    auto& name_slot = names_[ev.id];
                    if (name_slot.empty())
                        name_slot.assign(ev.name);
                    rows_.push_back(Row {tick_no_,
                                         cur_wave_,
                                         ev.id,
                                         ev.busy_us,
                                         ev.prepare_us,
                                         ev.finish_us,
                                         ev.items,
                                         0.0,
                                         0.0,
                                         0.0,
                                         ev.flush_us,
                                         0.0,
                                         0.0});
                },
                [this](WaveEnd const& ev) {
                    double const us = elapsed_us(t_wave_);
                    for (auto i = wave_first_row_; i < rows_.size(); ++i) {
                        rows_[i].wave_us  = us;
                        rows_[i].flush_us = ev.flush_us;
                        rows_[i].build_us = ev.build_us;
                        rows_[i].sort_us  = ev.sort_us;
                    }
                },
                [this](TickEnd const&) {
                    double const us = elapsed_us(t_tick_);
                    for (auto i = tick_first_row_; i < rows_.size(); ++i)
                        rows_[i].tick_us = us;
                    ++tick_no_;
                    if (clock::now() - last_flush_ >= period_) {
                        flush();
                        last_flush_ = clock::now();
                    }
                },
                [this](TickAbort const&) {
                    // Drop the aborted tick's partial rows: a truncated tick
                    // would skew every downstream distribution.
                    rows_.resize(tick_first_row_);
                },
            },
            e);
    }

    // Format and emit every buffered row now (e.g. at shutdown). No-op if empty.
    void flush() {
        char buf[320];
        for (Row const& r : rows_) {
            auto const it  = names_.find(r.id);
            char const* nm = it != names_.end() ? it->second.c_str() : "?";
            int const k = std::snprintf(
                buf,
                sizeof(buf),
                "%llu,%zu,%s,%.1f,%.2f,%.2f,%u,%.1f,%.2f,%.1f,%.2f,%.2f,%.2f",
                static_cast<unsigned long long>(r.tick),
                r.wave,
                nm,
                r.busy_us,
                r.prepare_us,
                r.finish_us,
                r.items,
                r.wave_us,
                r.flush_us,
                r.tick_us,
                r.cmd_us,
                r.build_us,
                r.sort_us);
            sink_(std::string_view(buf, k > 0 ? static_cast<std::size_t>(k) : 0));
        }
        rows_.clear();
        tick_first_row_ = 0;
        wave_first_row_ = 0;
    }

private:
    using clock = std::chrono::steady_clock;

    static double elapsed_us(clock::time_point t0) {
        return std::chrono::duration<double, std::micro>(clock::now() - t0).count();
    }

    struct Row {
        std::uint64_t tick;
        std::size_t wave;
        SystemId id;
        double busy_us;
        double prepare_us;
        double finish_us;
        std::uint32_t items;
        double wave_us;
        double flush_us; // the whole wave's command flush (repeated per row)
        double tick_us;
        double cmd_us;   // THIS system's share of the wave's command flush
        double build_us; // the wave's flatten, per row
        double sort_us;  // the LPT sort, per row
    };

    Sink sink_;
    clock::duration period_;
    clock::time_point last_flush_;
    std::unordered_map<SystemId, std::string> names_; // id -> system name
    std::vector<Row> rows_;                           // rows since last flush

    clock::time_point t_tick_ {}, t_wave_ {};
    std::size_t cur_wave_       = 0;
    std::size_t tick_first_row_ = 0; // first row of the current tick
    std::size_t wave_first_row_ = 0; // first row of the current wave
    std::uint64_t tick_no_      = 0;
};

} // namespace ecs::diag
