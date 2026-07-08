// tools/schedule_trace.hpp
//
// A schedule observer that writes a per-tick timing trace: one CSV row per system
// per tick ("tick,wave,system,us"), buffered and flushed to a Sink. Feed it to a
// file for offline flame/timeline analysis. It is a plain callable observer
// (operator()(ScheduleEvent const&) that std::visits), registered on the
// schedule's event emitter:
//
//   std::ofstream csv("trace.csv");
//   ecs::diag::ScheduleTrace trace(ecs::diag::to_stream(csv));
//   sched.events().add(std::ref(trace));   // or: subscribe(std::ref(trace))
//   for (;;) sched.run(world, pool);
//   trace.flush();                          // drain the tail at shutdown
//
// It is one of the two timing observers the profiler was split into (the other
// is ScheduleReport, which aggregates instead of tracing); register either,
// both, or neither. Unlike the report it keeps no distribution -- just raw rows
// -- so it has no TickStats dependency.
//
// Not gated by any macro -- always available; whether to attach it is the
// application's choice.

#pragma once

#include "ecs/schedule.hpp" // ecs::ScheduleEvent, ecs::sched_event, ecs::SystemId
#include "sink.hpp"         // ecs::diag::Sink
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

// Not thread-safe: attach to one Schedule driven from one thread. Self-timing,
// so it composes with other observers (e.g. ScheduleReport) independently. The
// CSV header row is emitted to the sink on construction. It reacts to only the
// events it needs (a partial visitor: the trailing catch-all ignores TickBegin
// and WaveEnd, and any event kind added later).
class ScheduleTrace {
public:
    explicit ScheduleTrace(Sink sink, double flush_every_s = 2.0)
        : sink_(std::move(sink))
        , period_(std::chrono::duration_cast<clock::duration>(
              std::chrono::duration<double>(flush_every_s)))
        , last_flush_(clock::now()) {
        sink_("tick,wave,system,us");
    }

    void operator()(ScheduleEvent const& e) {
        using namespace sched_event;
        std::visit(event::overloaded {
                       [this](WaveBegin const& ev) { cur_wave_ = ev.level; },
                       [this](SystemBegin const& ev) {
                           cur_id_         = ev.id;
                           auto& name_slot = names_[ev.id];
                           if (name_slot.empty())
                               name_slot.assign(ev.name);
                           t_sys_ = clock::now();
                       },
                       [this](SystemEnd const&) {
                           double const us =
                               std::chrono::duration<double, std::micro>(clock::now() -
                                                                         t_sys_)
                                   .count();
                           pending_.push_back(Row {tick_no_, cur_wave_, cur_id_, us});
                       },
                       [this](TickEnd const&) {
                           ++tick_no_;
                           if (clock::now() - last_flush_ >= period_) {
                               flush();
                               last_flush_ = clock::now();
                           }
                       },
                       [](auto const&) {}, // TickBegin / WaveEnd: unused by the trace
                   },
                   e);
    }

    // Format and emit every buffered row now (e.g. at shutdown). No-op if empty.
    void flush() {
        char buf[320];
        for (Row const& r : pending_) {
            auto const it  = names_.find(r.id);
            char const* nm = it != names_.end() ? it->second.c_str() : "?";
            int const k    = std::snprintf(buf,
                                        sizeof(buf),
                                        "%llu,%zu,%s,%.1f",
                                        static_cast<unsigned long long>(r.tick),
                                        r.wave,
                                        nm,
                                        r.us);
            sink_(std::string_view(buf, k > 0 ? static_cast<std::size_t>(k) : 0));
        }
        pending_.clear();
    }

private:
    using clock = std::chrono::steady_clock;

    struct Row {
        std::uint64_t tick;
        std::size_t wave;
        SystemId id;
        double us;
    };

    Sink sink_;
    clock::duration period_;
    clock::time_point last_flush_;
    std::unordered_map<SystemId, std::string> names_; // id -> system name
    std::vector<Row> pending_;                        // rows since last flush

    clock::time_point t_sys_ {};
    std::size_t cur_wave_  = 0;
    SystemId cur_id_       = 0;
    std::uint64_t tick_no_ = 0;
};

} // namespace ecs::diag
