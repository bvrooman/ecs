// tools/schedule_report.hpp
//
// A schedule observer that reports rolling timing distributions: for the whole
// tick, each wave, and each system it collects durations and periodically prints
// mean/sd/p50/p99/max to a Sink -- with each wave's systems grouped beneath it,
// in execution order. It is a plain callable observer (operator()(ScheduleEvent
// const&) that std::visits), registered on the schedule's event emitter:
//
//   ecs::diag::ScheduleReport report(ecs::diag::to_stream(std::cout));
//   sched.events().add(std::ref(report));   // or: subscribe(std::ref(report))
//   for (;;) sched.run(world, pool);         // prints ~every 2s
//   report.flush();                          // final report at shutdown
//
// It reuses ecs::diag::TickStats for the mean/sd/p50/p99/max reduction, so every
// granularity prints the same line already used for "sim tick". It is one of the
// two timing observers the profiler was split into (the other is ScheduleTrace);
// register either, both, or neither.
//
// Not gated by any macro -- always available. Whether to attach it is the
// application's choice, typically at runtime; the mist example, for one, attaches
// it only when the ECS_STATS env var is set.

#pragma once

#include "ecs/schedule.hpp" // ecs::ScheduleEvent, ecs::sched_event, ecs::SystemId
#include "sink.hpp"         // ecs::diag::Sink
#include "stats.hpp"        // ecs::diag::TickStats

#include <chrono>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace ecs::diag {

// Not thread-safe: attach to one Schedule driven from one thread (the sim
// thread). Self-timing -- it reads the clock at each boundary independently, so
// it composes with other observers without coordinating with them. It handles
// every ScheduleEvent alternative (an exhaustive visitor), so adding a new event
// kind is a compile error here until it is accounted for.
class ScheduleReport {
public:
    explicit ScheduleReport(Sink sink, double report_every_s = 2.0)
        : sink_(std::move(sink))
        , tick_(report_every_s) {}

    void operator()(ScheduleEvent const& e) {
        using namespace sched_event;
        std::visit(
            event::overloaded {
                [this](TickBegin const& ev) {
                    if (waves_.size() < ev.n_waves) {
                        waves_.resize(ev.n_waves);
                        wave_systems_.resize(ev.n_waves);
                    }
                    // Rebuild the per-wave ordering each tick so it always reflects
                    // the current execution order (cheap: small vectors, capacity
                    // retained).
                    for (auto& ids : wave_systems_)
                        ids.clear();
                    t_tick_ = clock::now();
                },
                [this](WaveBegin const& ev) {
                    cur_wave_ = ev.level;
                    t_wave_   = clock::now();
                },
                [this](SystemBegin const& ev) {
                    cur_ = &slot(ev.id, ev.name);
                    wave_systems_[cur_wave_].push_back(ev.id); // execution order
                    t_sys_ = clock::now();
                },
                [this](SystemEnd const&) { cur_->stats.sample(elapsed_us(t_sys_)); },
                [this](WaveEnd const& ev) {
                    waves_[ev.level].sample(elapsed_us(t_wave_));
                },
                [this](TickEnd const&) {
                    tick_.sample(elapsed_us(t_tick_));
                    if (auto const s = tick_.due())
                        report(*s);
                },
            },
            e);
    }

    // Emit a report now regardless of cadence (e.g. at shutdown). No-op if no
    // samples are pending.
    void flush() {
        if (auto const s = tick_.flush())
            report(*s);
    }

private:
    using clock = TickStats::clock;

    struct Slot {
        std::string name;
        TickStats   stats;
    };

    static double elapsed_us(clock::time_point t0) {
        return std::chrono::duration<double, std::micro>(clock::now() - t0).count();
    }

    Slot& slot(SystemId id, std::string_view name) {
        Slot& s = systems_[id]; // default-constructs on first sight
        if (s.name.empty())
            s.name.assign(name);
        return s;
    }

    void report(TickStats::Summary const& tick) {
        sink_(TickStats::format(tick, "sim tick"));
        // Each wave in execution order, then its systems grouped beneath it, also
        // in execution order (the order they ran within the wave).
        for (std::size_t w = 0; w < waves_.size(); ++w) {
            auto const wave = waves_[w].flush();
            if (!wave)
                continue; // this wave did not run in the window
            sink_("  " + TickStats::format(*wave, "wave " + std::to_string(w)));
            for (SystemId id : wave_systems_[w]) {
                auto const it = systems_.find(id);
                if (it == systems_.end())
                    continue;
                if (auto const s = it->second.stats.flush())
                    sink_("    " + TickStats::format(*s, it->second.name));
            }
        }
    }

    Sink sink_;
    TickStats tick_;               // whole-tick distribution + report cadence
    std::vector<TickStats> waves_; // per wave index
    std::unordered_map<SystemId, Slot> systems_;
    // Per-wave system ids in execution order, rebuilt each tick (index == wave).
    std::vector<std::vector<SystemId>> wave_systems_;

    clock::time_point t_tick_ {}, t_wave_ {}, t_sys_ {};
    std::size_t cur_wave_ = 0;
    Slot* cur_            = nullptr;
};

} // namespace ecs::diag
