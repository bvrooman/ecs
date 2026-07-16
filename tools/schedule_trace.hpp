// tools/schedule_trace.hpp
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
// Columns (every measurement numeric except `wave`; `system` is the row's key):
//
//   tick        run() ordinal since the trace attached (0-based)
//   wave        the system's wave index within the tick, OR the literal
//               `maint` for a maintenance-hook row (see below)
//   system      system name (or the maintenance hook's name on a maint row)
//   busy_us     sum of the system's work-item durations across all lanes
//               (CPU time: a fanned wave's busy columns sum past wall time).
//               On a maint row: that hook's single-threaded cost this tick.
//   prepare_us  the system's single-threaded barrier prepare hook (0 on maint)
//   finish_us   the system's single-threaded barrier finish hook (0 on maint)
//   items       work items the system contributed (1 for imperative; 0 on maint)
//   wave_us     wall duration of the whole wave (repeated on each of the
//               wave's rows; includes the flush). On a maint row: the whole
//               maintenance phase's wall this tick (sum of its hooks).
//   flush_us    command-flush portion of wave_us (repeated likewise; 0 on maint)
//   tick_us     wall duration of the tick's WAVES (repeated on every row of the
//               tick, maint rows included); systems only -- excludes
//               maintenance, so the true frame wall is tick_us + the tick's
//               maintenance-phase wall.
//
// Maintenance rows: a Schedule::add_maintenance hook runs world-quiescent
// BEFORE the tick's waves (see Schedule::add_maintenance). Each hook that fired
// emits one row with wave=`maint`, keyed by the hook name -- the frame's opening
// serial phase, modeled as pseudo-systems so a hook ranks and charts alongside
// real systems. A tick with no maintenance simply has no maint rows.
//
// It is one of the two timing observers the profiler was split into (the other
// is ScheduleReport, which aggregates instead of tracing); register either,
// both, or neither. Unlike the report it keeps no distribution -- just raw rows
// -- so it has no TickStats dependency.
//
// Not gated by any macro -- always available; whether to attach it is the
// application's choice.

#pragma once

#include "ecs/event/observer.hpp" // ecs::event::overloaded
#include "ecs/schedule.hpp"        // ecs::ScheduleEvent, ecs::sched_event, ecs::SystemId
#include "sink.hpp"                // ecs::diag::Sink
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
              "wave_us,flush_us,tick_us");
    }

    void operator()(ScheduleEvent const& e) {
        using namespace sched_event;
        std::visit(
            event::overloaded {
                [this](Maintenance const& ev) {
                    // Fires before TickBegin, world-quiescent. Buffer the hook
                    // (name + cost); TickBegin materializes the rows once the
                    // phase total is known.
                    pending_maint_.push_back({std::string(ev.name), ev.us});
                },
                [this](TickBegin const&) {
                    tick_first_row_ = rows_.size();
                    // Materialize the maintenance phase as the tick's opening
                    // rows: one per hook, wave=maint, wave_us = the phase wall
                    // (sum of hooks, run serially). tick_us is stamped at
                    // TickEnd alongside the system rows.
                    double phase_us = 0;
                    for (auto const& m : pending_maint_)
                        phase_us += m.second;
                    for (auto const& m : pending_maint_)
                        rows_.push_back(Row {tick_no_, 0, SystemId {0}, m.second,
                                             0.0, 0.0, 0, phase_us, 0.0, 0.0,
                                             true, m.first});
                    pending_maint_.clear();
                    t_tick_ = clock::now();
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
                                         false,
                                         {}});
                },
                [this](WaveEnd const& ev) {
                    double const us = elapsed_us(t_wave_);
                    for (auto i = wave_first_row_; i < rows_.size(); ++i) {
                        rows_[i].wave_us  = us;
                        rows_[i].flush_us = ev.flush_us;
                    }
                },
                [this](TickEnd const&) {
                    // Systems-only tick wall (maintenance ran before t_tick_),
                    // stamped on every row of the tick including the maint rows.
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
                    // would skew every downstream distribution. The tick's maint
                    // rows are among them (pushed at TickBegin) and go too.
                    rows_.resize(tick_first_row_);
                    pending_maint_.clear();
                },
                [](auto const&) {}, // SystemBegin/SystemEnd: unused by the trace
            },
            e);
    }

    // Format and emit every buffered row now (e.g. at shutdown). No-op if empty.
    void flush() {
        char buf[320];
        char wave[16];
        for (Row const& r : rows_) {
            char const* nm;
            if (r.is_maint) {
                std::snprintf(wave, sizeof(wave), "maint");
                nm = r.maint_name.c_str();
            } else {
                std::snprintf(wave, sizeof(wave), "%zu", r.wave);
                auto const it = names_.find(r.id);
                nm            = it != names_.end() ? it->second.c_str() : "?";
            }
            int const k = std::snprintf(buf,
                                        sizeof(buf),
                                        "%llu,%s,%s,%.1f,%.2f,%.2f,%u,%.1f,%.2f,%.1f",
                                        static_cast<unsigned long long>(r.tick),
                                        wave,
                                        nm,
                                        r.busy_us,
                                        r.prepare_us,
                                        r.finish_us,
                                        r.items,
                                        r.wave_us,
                                        r.flush_us,
                                        r.tick_us);
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
        std::size_t wave; // wave index; ignored (written as `maint`) when is_maint
        SystemId id;      // valid unless is_maint
        double busy_us;
        double prepare_us;
        double finish_us;
        std::uint32_t items;
        double wave_us;
        double flush_us;
        double tick_us;
        bool is_maint;          // a maintenance-hook row (serial pre-wave phase)
        std::string maint_name; // hook name when is_maint (empty otherwise)
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
    // Maintenance hooks that fired since the last TickBegin (name, us), drained
    // into rows at TickBegin once the phase total is known.
    std::vector<std::pair<std::string, double>> pending_maint_;
};

} // namespace ecs::diag
